// SPDX-License-Identifier: GPL-3.0-or-later
/* joystick: the pilot's controller, read natively (hangar#3). The topside's
 * pilot.wrench and pilot.buttons at 50 Hz from a Linux joystick device
 * (/dev/input/jsN), with no QGroundControl and no MAVLink between: the same
 * topics, types and rate as topside_mavlink, of which a profile enables one or
 * the other (one writer per topic). Buttons marked arm or disarm call
 * safety.arm or safety.disarm once per press; those marked pass set their bit
 * in pilot.buttons while held.
 *
 * With no controller (unplugged, not yet there), the module keeps publishing
 * at 50 Hz, a neutral wrench flagged as computed from stale input: the mux
 * reads that as no pilot input, as it reads a stale topic, while the manager's
 * staleness watchdog (docs/02 "Staleness is the watchdog") sees a healthy
 * module and does not restart it on every unplug. It looks for the device
 * again every Reopen and carries on without a restart.
 *
 *   hangar-joystick /etc/hangar/joystick.conf
 */
#include "mod.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/joystick.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>

#include "empty.h"
#include "hg_call.h"
#include "hg_ini.h"
#include "joystick_map.h"
#include "pilot_buttons.h"
#include "wrench.h"

#define WHO "hangar-joystick"
#define TICK_NS 20000000ull /* 50 Hz: pilot.wrench's rate in the manifest */
#define MAX_PENDING 4

_Static_assert(sizeof(struct js_ev) == sizeof(struct js_event), "struct js_event is 8 bytes");

static struct js_conf C;
static hg_pub *pub_wrench, *pub_buttons;

static struct {
	int fd;                  /* -1: no controller */
	char path[256];          /* the device open, or the last one tried */
	char name[128];          /* the controller's name, from the driver */
	struct js_state st;
	uint64_t next_open_ns;
	int gone_said;           /* "no controller" logged since it went */
	uint64_t events, opens, calls;
	struct {
		int in_use, is_arm;
		hg_pending *pc;
	} pending[MAX_PENDING];
	char status[160];
} J = { .fd = -1 };

/* ---- The device -------------------------------------------------------------------------- */

static int load_conf(const char *path)
{
	static char text[16384];
	size_t len;
	js_conf_defaults(&C);
	if (hg_ini_read_file(path, text, sizeof text, &len) < 0) {
		mod_log(WHO, "%s: %s", path, strerror(errno));
		return -1;
	}
	char why[256];
	if (js_conf_parse(text, len, &C, why, sizeof why) < 0) {
		mod_log(WHO, "%s: %s", path, why);
		hg_status(why);
		return -1;
	}
	return 0;
}

static void device_name(int fd, char *out, size_t len)
{
	out[0] = 0;
	/* A pipe or a file standing in for the device has no name. */
	if (ioctl(fd, JSIOCGNAME(len > 0 ? len - 1 : 0), out) < 0)
		out[0] = 0;
	out[len - 1] = 0;
}

/* Device=auto: the first /dev/input/jsN whose name holds Name=. */
static int open_auto(void)
{
	DIR *d = opendir("/dev/input");
	if (!d)
		return -1;
	struct dirent *e;
	int fd = -1;
	while (fd < 0 && (e = readdir(d)) != NULL) {
		if (strncmp(e->d_name, "js", 2) != 0)
			continue;
		char path[sizeof J.path];
		snprintf(path, sizeof path, "/dev/input/%.200s", e->d_name);
		int f = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (f < 0)
			continue;
		char name[128];
		device_name(f, name, sizeof name);
		if (strstr(name, C.name)) {
			fd = f;
			snprintf(J.path, sizeof J.path, "%s", path);
		} else {
			close(f);
		}
	}
	closedir(d);
	return fd;
}

static void try_open(int ep, uint64_t now)
{
	J.next_open_ns = now + C.reopen_ns;
	int fd;
	if (strcmp(C.device, "auto") == 0) {
		fd = open_auto();
	} else {
		snprintf(J.path, sizeof J.path, "%s", C.device);
		fd = open(C.device, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	}
	if (fd < 0) {
		if (!J.gone_said) {
			if (strcmp(C.device, "auto") == 0)
				mod_log(WHO, "no controller named like \"%s\" in /dev/input: pilot.wrench neutral and flagged stale until one comes",
					C.name);
			else
				mod_log(WHO, "no controller at %s (%s): pilot.wrench neutral and flagged stale until one comes",
					C.device, strerror(errno));
			J.gone_said = 1;
		}
		return;
	}
	J.fd = fd;
	J.opens++;
	memset(&J.st, 0, sizeof J.st); /* the driver's INIT events say where everything is */
	device_name(fd, J.name, sizeof J.name);
	struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
	epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
	mod_log(WHO, "controller %s%s%s%s", J.path, J.name[0] ? " (" : "", J.name, J.name[0] ? ")" : "");
	J.gone_said = 0;
}

static void close_device(int ep, const char *why)
{
	epoll_ctl(ep, EPOLL_CTL_DEL, J.fd, NULL);
	close(J.fd);
	J.fd = -1;
	memset(&J.st, 0, sizeof J.st);
	if (!J.gone_said) {
		mod_log(WHO, "controller %s gone (%s): pilot.wrench neutral and flagged stale until it comes back", J.path,
			why);
		J.gone_said = 1;
	}
}

/* ---- Calls ------------------------------------------------------------------------------- */

static void start_call(int is_arm)
{
	int k = -1;
	for (int i = 0; i < MAX_PENDING; i++)
		if (!J.pending[i].in_use) {
			k = i;
			break;
		}
	if (k < 0) {
		mod_log(WHO, "%s: too many requests in flight", is_arm ? "arm" : "disarm");
		return;
	}
	empty req = { 0 };
	hg_pending *pc;
	int st = hg_call_start(is_arm ? "safety.arm" : "safety.disarm", &EMPTY_TYPE, &req, &EMPTY_TYPE, &pc, 3000);
	J.calls++;
	if (st != HG_OK) {
		mod_log(WHO, "%s: %s", is_arm ? "arm" : "disarm", hg_status_name(st));
		return;
	}
	J.pending[k].in_use = 1;
	J.pending[k].is_arm = is_arm;
	J.pending[k].pc = pc;
}

static void poll_calls(void)
{
	for (int i = 0; i < MAX_PENDING; i++) {
		if (!J.pending[i].in_use)
			continue;
		hg_call_result r = hg_call_poll(J.pending[i].pc, NULL);
		if (r.status == HG_PENDING)
			continue;
		J.pending[i].in_use = 0;
		const char *what = J.pending[i].is_arm ? "arm" : "disarm";
		if (r.status == HG_OK)
			mod_log(WHO, "%s (button): accepted", what);
		else
			mod_log(WHO, "%s (button): %s %s", what, hg_status_name(r.status), r.reason);
	}
}

/* ---- Events, publishing ---------------------------------------------------------------------- */

static void read_events(int ep)
{
	struct js_ev evs[64];
	for (;;) {
		ssize_t n = read(J.fd, evs, sizeof evs);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		if (n <= 0) {
			close_device(ep, n == 0 ? "end of input" : strerror(errno));
			return;
		}
		size_t k = (size_t)n / sizeof evs[0];
		for (size_t i = 0; i < k; i++)
			js_apply(&J.st, &evs[i]);
		J.events += k;
		if ((size_t)n < sizeof evs)
			break;
	}
	uint32_t pressed = J.st.pressed;
	J.st.pressed = 0;
	for (int b = 0; b < JS_MAX_BUTTONS; b++) {
		if (!(pressed >> b & 1))
			continue;
		if (C.button[b] == JS_FN_ARM)
			start_call(1);
		else if (C.button[b] == JS_FN_DISARM)
			start_call(0);
	}
}

static void publish(uint64_t now)
{
	struct wrench w = { 0 };
	struct pilot_buttons b = { 0 };
	unsigned flags = HG_NO_REFS;
	if (J.fd >= 0) {
		float a[JS_WRENCH_AXES];
		js_wrench(&C, &J.st, a);
		w.surge = a[0];
		w.sway = a[1];
		w.heave = a[2];
		w.roll = a[3];
		w.pitch = a[4];
		w.yaw = a[5];
		b.pressed = js_pass_bits(&C, &J.st);
	} else {
		flags |= HG_STALE; /* no controller: no pilot input, as the mux reads a stale topic */
	}
	hg_publish_with(pub_wrench, &w, sizeof w, now, flags, NULL, 0);
	hg_publish_with(pub_buttons, &b, sizeof b, now, flags, NULL, 0);
}

static void status_line(void)
{
	char line[160];
	if (J.fd >= 0)
		mod_fmt(line, sizeof line, "controller %s%s%s%s; %llu events, %llu calls", J.path, J.name[0] ? " (" : "",
			J.name, J.name[0] ? ")" : "", (unsigned long long)J.events, (unsigned long long)J.calls);
	else
		mod_fmt(line, sizeof line, "no controller (%s): neutral, flagged stale; %llu events, %llu calls",
			strcmp(C.device, "auto") ? C.device : C.name, (unsigned long long)J.events,
			(unsigned long long)J.calls);
	if (strcmp(line, J.status) != 0) {
		mod_fmt(J.status, sizeof J.status, "%s", line);
		hg_status(line);
	}
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s joystick.conf\n", argv[0]);
		return 2;
	}
	mod_init(WHO);
	if (load_conf(argv[1]) < 0)
		return 78;
	pub_wrench = hg_pub_open("pilot.wrench", &WRENCH_TYPE, &(struct hg_pub_opts){ .rate_mhz = 50000 });
	pub_buttons = hg_pub_open("pilot.buttons", &PILOT_BUTTONS_TYPE, &(struct hg_pub_opts){ .rate_mhz = 50000 });
	if (!pub_wrench || !pub_buttons) {
		mod_log(WHO, "hg_pub_open: %s", strerror(errno));
		return 4;
	}
	int ep = epoll_create1(EPOLL_CLOEXEC);
	int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	struct itimerspec its = { .it_interval = { 0, (long)TICK_NS }, .it_value = { 0, (long)TICK_NS } };
	timerfd_settime(tfd, 0, &its, NULL);
	struct epoll_event ev = { .events = EPOLLIN, .data.fd = tfd };
	epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev);
	/* The reply topics of its calls: read by hg_serve and hg_call_poll each
	 * pass, so nothing in the wait set goes unread (hangar#1). */
	hg_sub *subs[HG_MAX_WAIT_FD];
	int n = hg_call_subs(subs, HG_MAX_WAIT_FD);
	int efd = n > 0 ? hg_wait_fd(subs, n) : -1;
	if (efd >= 0) {
		ev.data.fd = efd;
		epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev);
	}
	int calls_wired = 0;
	for (int b = 0; b < JS_MAX_BUTTONS; b++)
		calls_wired |= C.button[b] == JS_FN_ARM || C.button[b] == JS_FN_DISARM;
	if (calls_wired && n <= 0)
		mod_log(WHO, "buttons are marked arm or disarm, but the manifest has no [Call safety.arm]/[Call safety.disarm]");
	uint64_t now = hg_now_ns();
	try_open(ep, now);
	publish(now); /* ready at once, with or without a controller */
	status_line();
	while (!mod_stop) {
		struct epoll_event evs[4];
		int k = epoll_wait(ep, evs, 4, 1000);
		if (k < 0 && errno != EINTR)
			break;
		int tick = 0;
		for (int i = 0; i < k; i++) {
			int fd = evs[i].data.fd;
			if (fd == tfd) {
				uint64_t v;
				tick |= read(tfd, &v, sizeof v) == sizeof v;
			} else if (fd == efd) {
				uint64_t v;
				(void)!read(efd, &v, sizeof v);
			} else if (J.fd >= 0 && fd == J.fd) {
				read_events(ep);
			}
		}
		if (n > 0) {
			hg_serve();
			poll_calls();
		}
		if (tick) {
			now = hg_now_ns();
			if (J.fd < 0 && now >= J.next_open_ns)
				try_open(ep, now);
			publish(now);
			status_line();
		}
	}
	mod_log(WHO, "stopping: %llu events from %llu opens, %llu calls", (unsigned long long)J.events,
		(unsigned long long)J.opens, (unsigned long long)J.calls);
	if (efd >= 0)
		hg_wait_fd_close();
	if (J.fd >= 0)
		close(J.fd);
	return 0;
}
