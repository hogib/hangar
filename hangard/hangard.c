// SPDX-License-Identifier: GPL-3.0-or-later
/* hangard: the event loop, start-up (docs/03 "Start-up" steps 1 to 6),
 * adoption, reload, re-exec, shutdown, the manager's own topics and the
 * systemd notify socket. The rest of the manager is in the files hangard.h
 * lists. */
#include "hangard.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hangar_modules.h"
#include "hangar_system.h"
#include "hg_type.h"
#include "wiring.h"

struct hangard G;
static char **saved_argv;

/* ---- Logging ------------------------------------------------------------- */

void hd_log(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof buf - 2)
		n = (int)sizeof buf - 2;
	buf[n++] = '\n';
	char line[1040];
	int l = mf_fmt(line, sizeof line, "hangard: %.*s", n, buf);
	ssize_t w = write(2, line, (size_t)l);
	(void)w;
}

void hd_die(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	hd_log("fatal: %s", buf);
	exit(1);
}

/* ---- Small helpers --------------------------------------------------------- */

struct module *module_find(const char *name)
{
	for (int i = 0; i < G.n_modules; i++)
		if (strcmp(G.modules[i].name, name) == 0)
			return &G.modules[i];
	return NULL;
}

int epoll_add(int fd, uint64_t tag)
{
	struct epoll_event ev = { .events = EPOLLIN, .data.u64 = tag };
	return epoll_ctl(G.epfd, EPOLL_CTL_ADD, fd, &ev);
}

void epoll_del(int fd)
{
	epoll_ctl(G.epfd, EPOLL_CTL_DEL, fd, NULL);
}

static int mkdir_p(const char *path, mode_t mode)
{
	char buf[HD_PATH];
	mf_fmt(buf, sizeof buf, "%s", path);
	for (char *p = buf + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			if (mkdir(buf, mode) < 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	if (mkdir(buf, mode) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

static void own_dir(const char *path, mode_t mode, gid_t gid)
{
	if (G.is_root)
		(void)chown(path, 0, gid);
	(void)chmod(path, mode);
}

static void read_boot_id(void)
{
	int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
	char buf[64] = "";
	if (fd >= 0) {
		ssize_t n = read(fd, buf, sizeof buf - 1);
		close(fd);
		if (n > 0)
			buf[n] = 0;
	}
	int k = 0;
	for (const char *p = buf; *p && k < 16; p++) {
		int hi = -1, lo = -1;
		if (*p == '-')
			continue;
		const char *q = p + 1;
		hi = (*p >= '0' && *p <= '9') ? *p - '0' : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10 : -1;
		lo = (*q >= '0' && *q <= '9') ? *q - '0' : (*q >= 'a' && *q <= 'f') ? *q - 'a' + 10 : -1;
		if (hi < 0 || lo < 0)
			break;
		G.boot_id[k++] = (unsigned char)(hi << 4 | lo);
		p++;
	}
}

/* ---- systemd notify ------------------------------------------------------- */

static void notify_open(void)
{
	G.notify_fd = -1;
	const char *path = getenv("NOTIFY_SOCKET");
	if (!path || !*path)
		return;
	struct sockaddr_un sa;
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof sa.sun_path)
		return;
	strcpy(sa.sun_path, path);
	if (sa.sun_path[0] == '@')
		sa.sun_path[0] = 0; /* abstract */
	int s = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (s < 0)
		return;
	socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(path));
	if (connect(s, (struct sockaddr *)&sa, len) < 0) {
		close(s);
		return;
	}
	G.notify_fd = s;
}

static void notify(const char *msg)
{
	if (G.notify_fd < 0)
		return;
	ssize_t w = send(G.notify_fd, msg, strlen(msg), MSG_NOSIGNAL);
	(void)w;
}

/* ---- Timer ---------------------------------------------------------------- */

void request_publish(void)
{
	G.publish_now = 1;
}

void arm_timer(void)
{
	uint64_t at = health_next_deadline();
	uint64_t now = hg_now_ns();
	if (health_tick_needed()) {
		if (!G.next_tick_ns || G.next_tick_ns < now)
			G.next_tick_ns = now + G.conf.health_tick_ns;
		if (!at || G.next_tick_ns < at)
			at = G.next_tick_ns;
	} else {
		G.next_tick_ns = 0;
	}
	uint64_t pub = G.publish_now ? now : G.next_publish_ns;
	if (!at || pub < at)
		at = pub;
	if (G.notify_fd >= 0 && (!at || G.next_watchdog_ns < at))
		at = G.next_watchdog_ns;
	struct itimerspec its;
	memset(&its, 0, sizeof its);
	if (at) {
		if (at <= now)
			at = now + 1;
		its.it_value.tv_sec = (time_t)(at / 1000000000u);
		its.it_value.tv_nsec = (long)(at % 1000000000u);
	}
	if (timerfd_settime(G.timfd, TFD_TIMER_ABSTIME, &its, NULL) < 0)
		hd_die("timerfd_settime: %s", strerror(errno));
}

/* ---- The manager's own topics -------------------------------------------- */

static struct hg_schema admin_req_schema;

static void open_own_topics(void)
{
	/* The library in standalone mode creates the files by the same steps
	 * we use for modules (docs/01 "Opening"); an earlier instance's files
	 * are continued. */
	unsetenv("HANGAR_CONTROL");
	unsetenv("HANGAR_WIRING");
	unsetenv("HANGAR_WIRING_FILE");
	setenv("HANGAR_TOPIC_DIR", G.conf.topic_dir, 1);
	setenv("HANGAR_MODULE", "hangar", 1);
	if (hg_init() < 0)
		hd_die("hg_init: %s", hg_init_error());
	char name[64];
	struct hg_pub_opts sys_opts = { .slots = 16, .rate_mhz = 1000, .stale_after_ns = 3000000000ull };
	mf_fmt(name, sizeof name, "hangar.%s.system", G.conf.role);
	G.pub_system = hg_pub_open(name, &HANGAR_SYSTEM_TYPE, &sys_opts);
	if (!G.pub_system)
		hd_die("cannot open %s: %s", name, strerror(errno));
	struct hg_pub_opts mod_opts = { .slots = 256, .rate_mhz = 1000, .stale_after_ns = 3000000000ull };
	mf_fmt(name, sizeof name, "hangar.%s.modules", G.conf.role);
	G.pub_modules = hg_pub_open(name, &HANGAR_MODULES_TYPE, &mod_opts);
	if (!G.pub_modules)
		hd_die("cannot open %s: %s", name, strerror(errno));
	/* hangar.<role>.admin.req: req:hangar, generated with the manifests'
	 * types so that every server's wiring names the same hash. */
	const struct mf_type *adm = mf_type_find(&G.set, "req:hangar");
	if (!adm || hg_schema_parse(adm->schema, adm->schema_bytes, &admin_req_schema, NULL, 0) < 0)
		hd_die("the admin request type is missing");
	admin_req_schema.type.name = "req:hangar";
	struct hg_pub_opts adm_opts = { .slots = 16 };
	mf_fmt(name, sizeof name, "hangar.%s.admin.req", G.conf.role);
	G.pub_admin = hg_pub_open(name, &admin_req_schema.type, &adm_opts);
	if (!G.pub_admin)
		hd_die("cannot open %s: %s", name, strerror(errno));
}

void publish_own_topics(void)
{
	uint64_t now = hg_now_ns();
	struct hangar_system s;
	memset(&s, 0, sizeof s);
	s.state = (uint8_t)G.sys;
	s.since_ns = G.sys_since_ns;
	s.manager_ns = now;
	s.modules = (uint32_t)G.n_modules;
	s.manager_pid = (uint32_t)getpid();
	mf_fmt(s.reason, sizeof s.reason, "%s", G.sys_reason);
	hg_publish(G.pub_system, &s, sizeof s, now);
	for (int i = 0; i < G.n_modules; i++) {
		const struct module *m = &G.modules[i];
		struct hangar_modules d;
		memset(&d, 0, sizeof d);
		mf_fmt(d.name, sizeof d.name, "%s", m->name);
		d.state = (uint8_t)m->st;
		d.index = (uint8_t)i;
		d.count = (uint8_t)G.n_modules;
		d.exit_known = (uint8_t)m->exit_known;
		d.pid = (int32_t)m->pid;
		d.failures = m->failures;
		d.last_exit = m->exit_known ? m->exit_status : 0;
		d.since_ns = m->since_ns;
		d.published_ns = now;
		if (m->st == ST_REFUSED)
			mf_fmt(d.status, sizeof d.status, "%s", m->why);
		else if (m->page)
			memcpy(d.status, m->page + 64, 127);
		hg_publish(G.pub_modules, &d, sizeof d, now);
	}
	G.publish_now = 0;
	G.next_publish_ns = now + 1000000000ull;
}

/* ---- Modules from manifests ----------------------------------------------- */

static struct module *module_add(const char *name)
{
	struct module *m = module_find(name);
	if (m)
		return m;
	if (G.n_modules == HD_MAX_MODULES)
		return NULL;
	m = &G.modules[G.n_modules++];
	memset(m, 0, sizeof *m);
	mf_fmt(m->name, sizeof m->name, "%s", name);
	m->pidfd = -1;
	m->wiring_fd = -1;
	m->page_fd = -1;
	m->st = ST_STOPPED;
	m->since_ns = hg_now_ns();
	return m;
}

static void module_free_wiring(struct module *m)
{
	free(m->wiring);
	m->wiring = NULL;
	m->wiring_len = 0;
	if (m->wiring_fd >= 0)
		close(m->wiring_fd);
	m->wiring_fd = -1;
}

/* Gives m its manifest and wiring from set; 0 if unchanged from what it had. */
static int module_set_manifest(struct module *m, const struct mf_set *set, struct mf_module *mf)
{
	static char text[HD_WIRING_MAX];
	uint64_t whash = 0;
	int len = 0;
	if (!mf->refused && !mf->legacy) {
		len = wiring_text(set, mf, text, sizeof text);
		if (len < 0) {
			mf->refused = 1;
			mf_fmt(mf->reason, sizeof mf->reason, "%s:1: the wiring text exceeds %d bytes", mf->path,
				 HD_WIRING_MAX);
			len = 0;
		} else {
			whash = hg_fnv1a64(text, (size_t)len);
		}
	}
	int changed = m->manifest_hash != mf->text_hash || m->wiring_hash != whash;
	m->mf = mf;
	m->manifest_hash = mf->text_hash;
	m->wiring_hash = whash;
	module_free_wiring(m);
	if (len) {
		m->wiring = malloc((size_t)len + 1);
		if (m->wiring) {
			memcpy(m->wiring, text, (size_t)len + 1);
			m->wiring_len = (size_t)len;
			m->wiring_fd = wiring_memfd(text, (size_t)len);
			if (m->wiring_fd < 0)
				hd_log("%s: cannot create the wiring memfd: %s", m->name, strerror(errno));
		}
	}
	return changed;
}

static void log_set(const struct mf_set *set)
{
	for (int i = 0; i < set->n_modules; i++)
		if (set->modules[i]->refused)
			hd_log("refused: %s", set->modules[i]->reason);
	for (int i = 0; i < set->n_warnings; i++)
		hd_log("warning: %s", set->warnings[i]);
}

static int load_set(struct mf_set *set, char *err, size_t errlen)
{
	mf_set_init(set, G.conf.role, G.conf.privileged_module, G.conf.msg_dir);
	if (mf_set_load_dir(set, G.conf.module_dir) < 0) {
		mf_fmt(err, errlen, "cannot read %s: %s", G.conf.module_dir, strerror(errno));
		return -1;
	}
	int ok = mf_set_check(set);
	/* MaxModules: the ones past it, in name order, are refused. */
	int accepted = 0;
	for (int i = 0; i < set->n_modules; i++) {
		struct mf_module *mf = set->modules[i];
		if (mf->refused)
			continue;
		if (++accepted > G.conf.max_modules) {
			mf->refused = 1;
			mf_fmt(mf->reason, sizeof mf->reason, "%s:1: more than MaxModules=%d modules", mf->path,
				 G.conf.max_modules);
			ok--;
		}
	}
	return ok;
}

/* ---- Start-up --------------------------------------------------------------- */

static void prepare_dirs(void)
{
	if (mkdir_p(G.paths.run, 0755) < 0)
		hd_die("cannot create %s: %s", G.paths.run, strerror(errno));
	own_dir(G.paths.run, 0755, G.gid_hangar);
	if (mkdir_p(G.paths.pages, 0750) < 0 || mkdir_p(G.paths.neutral, 0700) < 0 ||
	    mkdir_p(G.conf.log_dir, 0750) < 0 || mkdir_p(G.paths.var, 0755) < 0)
		hd_die("cannot create the run-time directories under %s: %s", G.paths.run, strerror(errno));
	own_dir(G.paths.pages, 0750, G.gid_hangar);
	own_dir(G.conf.log_dir, 0750, G.gid_hangar);
	if (topic_dir_prepare() < 0)
		hd_die("cannot prepare %s: %s", G.conf.topic_dir, strerror(errno));
	if (mkdir_p(G.conf.recording_dir, 02770) < 0)
		hd_log("cannot create %s: %s", G.conf.recording_dir, strerror(errno));
	else {
		gid_t rec;
		if (G.is_root && lookup_group("hangar-rec", &rec) == 0)
			(void)chown(G.conf.recording_dir, 0, rec);
		(void)chmod(G.conf.recording_dir, 02770);
	}
}

static void find_cgroup(void)
{
	G.cgroup_base[0] = 0;
	if (!G.is_root)
		return;
	int fd = open("/proc/self/cgroup", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	char buf[512] = "";
	ssize_t n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n <= 0)
		return;
	buf[n] = 0;
	char *line = strstr(buf, "0::");
	if (!line)
		return;
	char *nl = strchr(line, '\n');
	if (nl)
		*nl = 0;
	char path[HD_PATH];
	mf_fmt(path, sizeof path, "/sys/fs/cgroup%s", line + 3);
	/* Delegated: we can make sub-groups. */
	char probe[HD_PATH + 16];
	mf_fmt(probe, sizeof probe, "%s/cgroup.procs", path);
	if (access(probe, W_OK) == 0) {
		mf_fmt(G.cgroup_base, sizeof G.cgroup_base, "%s", path);
		hd_log("cgroups: one per module under %s", path);
	}
}

/* Registry entries whose process is still the one recorded. */
static int entry_alive(const struct reg_entry *e, int *pidfd_out)
{
	*pidfd_out = -1;
	if (e->pid <= 0)
		return 0;
	int pidfd = (int)syscall(SYS_pidfd_open, e->pid, 0u);
	if (pidfd < 0)
		return 0;
	unsigned long long st = proc_starttime(e->pid);
	if (!st || st != e->starttime) {
		close(pidfd);
		return 0;
	}
	/* Still alive, or a zombie we are not the parent of? */
	*pidfd_out = pidfd;
	return 1;
}

static int running_state(enum state st)
{
	return st == ST_STARTING || st == ST_READY || st == ST_UNHEALTHY || st == ST_STOPPING;
}

/* Steps 1 and 3: the registry, the Neutral commands of the dead, adoption. */
static void startup_from_registry(int *n_dead, char (*dead)[MF_NAME])
{
	static struct reg_entry entries[HD_MAX_MODULES];
	enum sys_state sys = SYS_NOMINAL;
	uint64_t since = 0;
	char reason[128] = "";
	int n = registry_read(G.paths.registry, entries, HD_MAX_MODULES, &sys, &since, reason, sizeof reason);
	*n_dead = 0;
	if (n < 0) {
		if (errno != ENOENT)
			hd_log("cannot read %s: %s", G.paths.registry, strerror(errno));
		return;
	}
	struct stat st;
	char when[64] = "";
	if (stat(G.paths.registry, &st) == 0)
		mf_fmt(when, sizeof when, "%lld", (long long)st.st_mtime);
	G.sys = sys;
	G.sys_since_ns = since;
	mf_fmt(G.sys_reason, sizeof G.sys_reason, "%s", reason);
	hd_log("registry: %d modules, system %s%s%s", n, sys_state_names[sys], reason[0] ? ": " : "", reason);

	/* Step 1: neutralize first. */
	static struct reg_entry dead_entries[HD_MAX_MODULES];
	static int pidfds[HD_MAX_MODULES];
	int nd = 0;
	for (int i = 0; i < n; i++) {
		pidfds[i] = -1;
		if (running_state(entries[i].st) && !entry_alive(&entries[i], &pidfds[i])) {
			dead_entries[nd++] = entries[i];
			if (*n_dead < HD_MAX_MODULES)
				mf_fmt(dead[(*n_dead)++], MF_NAME, "%s", entries[i].name);
			hd_log("%s: pid %d is gone (registry last written at %s)", entries[i].name, (int)entries[i].pid,
			       when);
		}
	}
	if (nd)
		neutral_run_all(dead_entries, nd);

	/* Step 2 is done by the caller between; step 3: adopt. */
	for (int i = 0; i < n; i++) {
		struct module *m = module_add(entries[i].name);
		if (!m)
			continue;
		m->from_registry = 1;
		m->failures = entries[i].failures;
		m->delay_ns = entries[i].delay_ns;
		m->manifest_hash = entries[i].manifest_hash;
		m->wiring_hash = entries[i].wiring_hash;
		m->st = entries[i].st;
		if (pidfds[i] >= 0) {
			m->pid = entries[i].pid;
			m->pidfd = pidfds[i];
			m->starttime = entries[i].starttime;
			m->adopted = 1;
			m->started_ns = hg_now_ns();
			m->ready_ns = m->started_ns;
			if (m->st == ST_STOPPING)
				m->stop_deadline = hg_now_ns(); /* finish the stop at once */
			if (page_open_existing(m) < 0)
				hd_log("%s: adopted without its page: %s", m->name, strerror(errno));
			if (epoll_add(m->pidfd, TAG_MODULE | (uint64_t)(m - G.modules)) < 0)
				hd_die("epoll_ctl: %s", strerror(errno));
			hd_log("%s: adopted pid %d, %s", m->name, (int)m->pid, state_names[m->st]);
		} else if (running_state(entries[i].st)) {
			/* Dead: its readers learn now; its restart comes with the manifests. */
			topics_mark_writer_died(m->name);
			m->pid = 0;
			m->st = ST_BACKOFF;
			m->restart_at = hg_now_ns();
			m->exit_known = 0;
		} else if (entries[i].st == ST_BACKOFF) {
			m->restart_at = hg_now_ns();
		}
	}
}

static void apply_manifests(struct mf_set *set, char (*dead)[MF_NAME], int n_dead)
{
	/* Every manifest gets a module entry. */
	for (int i = 0; i < set->n_modules; i++) {
		struct mf_module *mf = set->modules[i];
		struct module *m = module_add(mf->name);
		if (!m) {
			hd_log("%s: too many modules", mf->name);
			continue;
		}
		int was_running = m->pid != 0;
		int changed = module_set_manifest(m, set, mf);
		if (mf->refused) {
			if (m->pid)
				module_stop(m, ST_STOPPED, 0, "manifest refused");
			else
				set_state(m, ST_REFUSED, mf->reason);
			continue;
		}
		if (m->st == ST_REFUSED)
			m->st = ST_STOPPED;
		module_outputs_init(m);
		if (was_running && changed) {
			hd_log("%s: manifest or wiring changed since it was started: restarting", m->name);
			module_stop(m, ST_STARTING, 0, "manifest changed");
		}
	}
	/* Modules the registry knew and no manifest names any more. */
	for (int i = 0; i < G.n_modules; i++) {
		struct module *m = &G.modules[i];
		if (m->mf)
			continue;
		if (m->pid) {
			hd_log("%s: no manifest any more: stopping", m->name);
			module_stop(m, ST_STOPPED, 0, "no manifest");
		} else {
			m->st = ST_STOPPED;
		}
	}
	/* Step 6: start what is not running. Dead ones count a failure. */
	for (int i = 0; i < G.n_modules; i++) {
		struct module *m = &G.modules[i];
		if (!m->mf || m->mf->refused || m->pid)
			continue;
		int died = 0;
		for (int k = 0; k < n_dead; k++)
			if (strcmp(dead[k], m->name) == 0)
				died = 1;
		if (died) {
			if (m->mf->policy == MF_RESTART_NEVER)
				set_state(m, ST_STOPPED, "died while no manager was running");
			else
				schedule_restart(m, "died while no manager was running");
		} else if (m->from_registry && (m->st == ST_FAILED || m->st == ST_STOPPED)) {
			/* Kept from the registry: it was down on purpose, or given up,
			 * and stays so until hangar restart or a reload. */
			hd_log("%s: %s in the registry: left down", m->name, state_names[m->st]);
		} else if (m->st == ST_BACKOFF) {
			if (m->restart_at <= hg_now_ns())
				module_start(m);
		} else {
			module_start(m);
		}
	}
}

/* Drops entries that are stopped and have no manifest. */
static void prune_modules(void)
{
	int k = 0;
	for (int i = 0; i < G.n_modules; i++) {
		struct module *m = &G.modules[i];
		if (!m->mf && !m->pid) {
			module_free_wiring(m);
			if (m->page)
				munmap(m->page, HD_PAGE_BYTES);
			if (m->page_fd >= 0)
				close(m->page_fd);
			continue;
		}
		if (k != i) {
			G.modules[k] = *m;
			if (G.modules[k].pidfd >= 0) {
				epoll_del(G.modules[k].pidfd);
				epoll_add(G.modules[k].pidfd, TAG_MODULE | (uint64_t)k);
			}
		}
		k++;
	}
	G.n_modules = k;
}

/* ---- Reload ------------------------------------------------------------------ */

int do_reload(char *err, size_t errlen)
{
	hd_log("reload: reading %s", G.conf.module_dir);
	struct mf_set *set = calloc(1, sizeof *set);
	if (!set) {
		mf_fmt(err, errlen, "out of memory");
		return -1;
	}
	if (load_set(set, err, errlen) < 0) {
		mf_set_free(set);
		free(set);
		return -1;
	}
	/* A refused graph changes nothing. */
	for (int i = 0; i < set->n_modules; i++)
		if (set->modules[i]->refused) {
			mf_fmt(err, errlen, "reload refused, nothing changed: %s", set->modules[i]->reason);
			hd_log("%s", err);
			mf_set_free(set);
			free(set);
			return -1;
		}
	for (int i = 0; i < set->n_warnings; i++)
		hd_log("warning: %s", set->warnings[i]);
	/* Removed modules lose their manifest; the rest are re-pointed. An
	 * entry that lost its manifest earlier and has stopped is dropped
	 * first, so that a manifest coming back starts afresh. */
	prune_modules();
	static int had_mf[HD_MAX_MODULES];
	for (int i = 0; i < G.n_modules; i++) {
		had_mf[i] = G.modules[i].mf != NULL;
		G.modules[i].mf = NULL;
	}
	for (int i = 0; i < set->n_modules; i++) {
		struct mf_module *mf = set->modules[i];
		struct module *m = module_find(mf->name);
		int is_new = !m || !had_mf[m - G.modules];
		m = module_add(mf->name);
		if (!m)
			continue;
		uint64_t mh = m->manifest_hash, wh = m->wiring_hash;
		int changed = module_set_manifest(m, set, mf);
		if (is_new) {
			hd_log("reload: %s added", m->name);
			module_outputs_init(m);
			module_start(m);
		} else if (changed && (mh || wh)) {
			hd_log("reload: %s changed", m->name);
			module_outputs_init(m);
			m->failures = 0;
			m->delay_ns = 0;
			m->n_fail_times = 0;
			m->gave_up = 0;
			if (m->pid)
				module_stop(m, ST_STARTING, 0, "manifest changed");
			else
				module_start(m);
		} else if (m->st == ST_REFUSED) {
			m->st = ST_STOPPED;
			module_outputs_init(m);
			module_start(m);
		}
	}
	for (int i = 0; i < G.n_modules; i++) {
		struct module *m = &G.modules[i];
		if (m->mf)
			continue;
		hd_log("reload: %s removed", m->name);
		if (m->pid)
			module_stop(m, ST_STOPPED, 0, "manifest removed");
		else
			set_state(m, ST_STOPPED, "manifest removed");
	}
	mf_set_free(&G.set);
	G.set = *set;
	free(set);
	prune_modules();
	registry_write();
	request_publish();
	recompute_sys_state();
	return 0;
}

/* ---- Re-exec ----------------------------------------------------------------- */

static int clear_cloexec(int fd)
{
	int fl = fcntl(fd, F_GETFD);
	return fl < 0 ? -1 : fcntl(fd, F_SETFD, fl & ~FD_CLOEXEC);
}

int do_reexec(char *err, size_t errlen)
{
	hd_log("re-executing in place, %d modules kept", G.n_modules);
	int fd = (int)syscall(SYS_memfd_create, "hangard-state", 0u);
	if (fd < 0) {
		mf_fmt(err, errlen, "memfd_create: %s", strerror(errno));
		return -1;
	}
	FILE *f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		mf_fmt(err, errlen, "fdopen: %s", strerror(errno));
		return -1;
	}
	fprintf(f, "hangar-reexec 1\n");
	fprintf(f, "system %s %llu %s\n", sys_state_names[G.sys], (unsigned long long)G.sys_since_ns, G.sys_reason);
	fprintf(f, "control %d\n", G.ctlfd);
	for (int i = 0; i < G.n_modules; i++) {
		const struct module *m = &G.modules[i];
		fprintf(f, "module %s %d %d %llu %s %llu %u %llu %llu %llu %llu %d %d %d %d %016llx %016llx %llu %s %d %d",
			m->name, (int)m->pid, m->pidfd, m->starttime, state_names[m->st],
			(unsigned long long)m->since_ns, m->failures, (unsigned long long)m->delay_ns,
			(unsigned long long)m->restart_at, (unsigned long long)m->started_ns,
			(unsigned long long)m->ready_ns, m->exit_known, m->exit_status, m->gave_up, m->adopted,
			(unsigned long long)m->manifest_hash, (unsigned long long)m->wiring_hash,
			(unsigned long long)m->stop_deadline, state_names[m->after_stop], m->count_failure,
			m->n_fail_times);
		for (int k = 0; k < m->n_fail_times; k++)
			fprintf(f, " %llu", (unsigned long long)m->fail_times[k]);
		fprintf(f, "\n");
		if (m->pidfd >= 0)
			clear_cloexec(m->pidfd);
	}
	fflush(f);
	clear_cloexec(fd);
	clear_cloexec(G.ctlfd);
	char num[16];
	mf_fmt(num, sizeof num, "%d", fd);
	setenv("HANGAR_REEXEC_FD", num, 1);
	registry_write();
	/* The old pub mappings die with the image; the new one continues the
	 * files (docs/03: its topics continue their sequence numbers). */
	execv("/proc/self/exe", saved_argv);
	int e = errno;
	mf_fmt(err, errlen, "execv /proc/self/exe: %s", strerror(e));
	unsetenv("HANGAR_REEXEC_FD");
	for (int i = 0; i < G.n_modules; i++)
		if (G.modules[i].pidfd >= 0)
			fcntl(G.modules[i].pidfd, F_SETFD, FD_CLOEXEC);
	fcntl(G.ctlfd, F_SETFD, FD_CLOEXEC);
	fclose(f);
	return -1;
}

/* Reads the state a predecessor passed; returns 1 if there was one. */
static int startup_from_reexec(void)
{
	const char *s = getenv("HANGAR_REEXEC_FD");
	if (!s || !*s)
		return 0;
	int fd = atoi(s);
	unsetenv("HANGAR_REEXEC_FD");
	FILE *f = fdopen(fd, "r");
	if (!f)
		return 0;
	rewind(f);
	char line[2048];
	int ok = fgets(line, sizeof line, f) && strncmp(line, "hangar-reexec 1", 15) == 0;
	while (ok && fgets(line, sizeof line, f)) {
		size_t len = strlen(line);
		while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = 0;
		if (strncmp(line, "system ", 7) == 0) {
			char st[32] = "";
			unsigned long long since = 0;
			int off = 0;
			if (sscanf(line + 7, "%31s %llu %n", st, &since, &off) >= 2) {
				for (int i = 0; i < 3; i++)
					if (strcmp(sys_state_names[i], st) == 0)
						G.sys = (enum sys_state)i;
				G.sys_since_ns = since;
				mf_fmt(G.sys_reason, sizeof G.sys_reason, "%s", line + 7 + off);
			}
		} else if (strncmp(line, "control ", 8) == 0) {
			G.ctlfd = atoi(line + 8);
		} else if (strncmp(line, "module ", 7) == 0) {
			char name[MF_NAME], st[32], after[32];
			int pid, pidfd, exit_known, exit_status, gave_up, adopted, count_failure, nft, off = 0;
			unsigned long long starttime, since, delay, restart_at, started, ready, mh, wh, stop_deadline;
			unsigned failures;
			if (sscanf(line + 7, "%31s %d %d %llu %31s %llu %u %llu %llu %llu %llu %d %d %d %d %llx %llx %llu %31s %d %d%n",
				   name, &pid, &pidfd, &starttime, st, &since, &failures, &delay, &restart_at, &started,
				   &ready, &exit_known, &exit_status, &gave_up, &adopted, &mh, &wh, &stop_deadline, after,
				   &count_failure, &nft, &off) < 21)
				continue;
			struct module *m = module_add(name);
			if (!m)
				continue;
			m->from_registry = 1;
			int si = state_from_name(st), ai = state_from_name(after);
			m->st = si >= 0 ? (enum state)si : ST_STOPPED;
			m->after_stop = ai >= 0 ? (enum state)ai : ST_STOPPED;
			m->since_ns = since;
			m->failures = failures;
			m->delay_ns = delay;
			m->restart_at = restart_at;
			m->started_ns = started;
			m->ready_ns = ready;
			m->exit_known = exit_known;
			m->exit_status = exit_status;
			m->gave_up = gave_up;
			m->adopted = adopted;
			m->manifest_hash = mh;
			m->wiring_hash = wh;
			m->stop_deadline = stop_deadline;
			m->count_failure = count_failure;
			const char *p = line + 7 + off;
			for (int k = 0; k < nft && k < HD_MAX_FAILS; k++) {
				char *e;
				unsigned long long t = strtoull(p, &e, 10);
				if (e == p)
					break;
				m->fail_times[m->n_fail_times++] = t;
				p = e;
			}
			if (pid > 0 && pidfd >= 0) {
				m->pid = (pid_t)pid;
				m->pidfd = pidfd;
				m->starttime = starttime;
				if (page_open_existing(m) < 0)
					hd_log("%s: re-exec without its page: %s", m->name, strerror(errno));
				if (epoll_add(m->pidfd, TAG_MODULE | (uint64_t)(m - G.modules)) < 0)
					hd_die("epoll_ctl: %s", strerror(errno));
			} else if (running_state(m->st)) {
				m->st = ST_BACKOFF;
				m->restart_at = hg_now_ns();
			}
		}
	}
	fclose(f);
	if (G.ctlfd >= 0) {
		int fl = fcntl(G.ctlfd, F_GETFD);
		if (fl >= 0)
			fcntl(G.ctlfd, F_SETFD, fl | FD_CLOEXEC);
	}
	hd_log("re-executed: %d modules kept as children", G.n_modules);
	return ok;
}

/* ---- Shutdown --------------------------------------------------------------- */

static int any_running(int neutral_only)
{
	for (int i = 0; i < G.n_modules; i++) {
		const struct module *m = &G.modules[i];
		if (!m->pid)
			continue;
		if (!neutral_only || (m->mf && m->mf->has_neutral))
			return 1;
	}
	return 0;
}

static void shutdown_phase(int phase)
{
	for (int i = 0; i < G.n_modules; i++) {
		struct module *m = &G.modules[i];
		int neutral = m->mf && m->mf->has_neutral;
		if (phase == 1 && !neutral)
			continue;
		if (m->pid) {
			if (!m->stop_deadline)
				module_stop(m, ST_STOPPED, 0, "shutdown");
		} else if (m->st == ST_BACKOFF || m->st == ST_STARTING) {
			set_state(m, ST_STOPPED, "shutdown");
		}
	}
}

void begin_shutdown(void)
{
	if (G.shutting_down)
		return;
	G.shutting_down = 1;
	hd_log("shutting down: modules with a Neutral command first");
	notify("STOPPING=1\n");
	shutdown_phase(1);
	if (!any_running(1)) {
		G.shutting_down = 2;
		shutdown_phase(2);
	}
}

static void shutdown_progress(void)
{
	if (G.shutting_down == 1 && !any_running(1)) {
		G.shutting_down = 2;
		hd_log("shutting down: the other modules");
		shutdown_phase(2);
	}
}

/* ---- main ---------------------------------------------------------------------- */

static void usage(void)
{
	fputs("usage: hangard [-c hangard.conf]\n"
	      "Paths are relocated under $HANGAR_ROOT when it is set.\n",
	      stderr);
	exit(2);
}

int main(int argc, char **argv)
{
	saved_argv = argv;
	const char *conf_file = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
			conf_file = argv[++i];
		else
			usage();
	}
	memset(&G, 0, sizeof G);
	G.notify_fd = -1;
	G.ctlfd = -1;
	paths_init(&G.paths, conf_file);
	char err[MF_ERR];
	if (conf_load(G.paths.conf, &G.paths, &G.conf, err, sizeof err) < 0)
		hd_die("%s", err);
	G.is_root = geteuid() == 0;
	G.uid = getuid();
	G.sys_since_ns = hg_now_ns();
	/* Topic files 0640, wake files and the socket 0660, the registry 0600:
	 * nothing the manager creates is for others; modules get the usual
	 * umask back before exec. */
	umask(0007);
	G.have_gid_hangar = lookup_group("hangar", &G.gid_hangar) == 0;
	G.have_gid_admin = lookup_group("hangar-admin", &G.gid_admin) == 0;
	if (!G.have_gid_hangar)
		G.gid_hangar = getgid();
	read_boot_id();
	find_cgroup();
	hd_log("starting: role %s, modules %s, topics %s%s%s", G.conf.role, G.conf.module_dir, G.conf.topic_dir,
	       G.paths.root[0] ? ", root " : "", G.paths.root);
	if (!G.is_root)
		hd_log("not running as root: user switching, scheduling, capabilities and cgroups are skipped");

	/* An inherited SIG_IGN or SA_NOCLDWAIT would have the kernel reap every
	 * module at exit: no status, and its group id free for reuse. */
	struct sigaction dfl = { .sa_handler = SIG_DFL };
	sigemptyset(&dfl.sa_mask);
	sigaction(SIGCHLD, &dfl, NULL);
	signal(SIGPIPE, SIG_IGN);
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGHUP);
	sigaddset(&mask, SIGCHLD);
	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
		hd_die("sigprocmask: %s", strerror(errno));
	if ((G.sigfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC)) < 0)
		hd_die("signalfd: %s", strerror(errno));
	if ((G.timfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)) < 0)
		hd_die("timerfd_create: %s", strerror(errno));
	if ((G.epfd = epoll_create1(EPOLL_CLOEXEC)) < 0)
		hd_die("epoll_create1: %s", strerror(errno));
	if (epoll_add(G.sigfd, TAG_SIGNAL) < 0 || epoll_add(G.timfd, TAG_TIMER) < 0)
		hd_die("epoll_ctl: %s", strerror(errno));

	static char dead[HD_MAX_MODULES][MF_NAME];
	int n_dead = 0;
	int reexec = startup_from_reexec();
	if (!reexec)
		startup_from_registry(&n_dead, dead); /* step 1 (neutralize) and the adoption list */
	prepare_dirs();                                /* step 2 */
	/* Step 3's flagging of dead writers happened in startup_from_registry;
	 * leftover cgroups of the dead are killed here. */
	for (int i = 0; i < n_dead && G.cgroup_base[0]; i++) {
		char path[HD_PATH + 16];
		mf_fmt(path, sizeof path, "%s/%s/cgroup.kill", G.cgroup_base, dead[i]);
		int fd = open(path, O_WRONLY | O_CLOEXEC);
		if (fd >= 0) {
			ssize_t w = write(fd, "1", 1);
			(void)w;
			close(fd);
		}
	}
	/* Step 4: manifests. */
	if (load_set(&G.set, err, sizeof err) < 0)
		hd_die("%s", err);
	log_set(&G.set);
	/* Step 5: our own topics, before any module can take their names. */
	open_own_topics();
	notify_open();
	/* Step 6. */
	apply_manifests(&G.set, dead, n_dead);
	prune_modules();
	if (G.ctlfd < 0 && (G.ctlfd = control_open()) < 0)
		hd_die("cannot open the control socket %s: %s", G.paths.control, strerror(errno));
	if (epoll_add(G.ctlfd, TAG_CONTROL) < 0)
		hd_die("epoll_ctl: %s", strerror(errno));
	registry_write();
	uint64_t now = hg_now_ns();
	G.next_publish_ns = now;
	G.next_watchdog_ns = now + 2000000000ull;
	publish_own_topics();
	notify("READY=1\n");
	arm_timer();
	hd_log("ready: %d modules", G.n_modules);

	for (;;) {
		if (G.shutting_down) {
			shutdown_progress();
			if (G.shutting_down == 2 && !any_running(0))
				break;
		}
		struct epoll_event evs[32];
		int n = epoll_wait(G.epfd, evs, 32, -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			hd_die("epoll_wait: %s", strerror(errno));
		}
		for (int i = 0; i < n; i++) {
			uint64_t tag = evs[i].data.u64;
			if (tag == TAG_SIGNAL) {
				struct signalfd_siginfo ssi;
				while (read(G.sigfd, &ssi, sizeof ssi) == (ssize_t)sizeof ssi) {
					if (ssi.ssi_signo == SIGTERM || ssi.ssi_signo == SIGINT) {
						hd_log("signal %u: shutting down", ssi.ssi_signo);
						begin_shutdown();
					} else if (ssi.ssi_signo == SIGHUP) {
						if (do_reload(err, sizeof err) < 0)
							hd_log("%s", err);
					}
					/* SIGCHLD carries nothing the pidfds do not. */
				}
			} else if (tag == TAG_TIMER) {
				uint64_t ticks;
				while (read(G.timfd, &ticks, sizeof ticks) < 0 && errno == EINTR)
					;
				now = hg_now_ns();
				health_tick(now);
				if (G.next_tick_ns && now >= G.next_tick_ns)
					G.next_tick_ns = now + G.conf.health_tick_ns;
				if (G.publish_now || now >= G.next_publish_ns)
					publish_own_topics();
				if (G.notify_fd >= 0 && now >= G.next_watchdog_ns) {
					notify("WATCHDOG=1\n");
					G.next_watchdog_ns = now + 2000000000ull;
				}
			} else if (tag == TAG_CONTROL) {
				control_accept();
			} else if (tag & TAG_CONN) {
				control_handle((int)(tag & 0xffff));
			} else if (tag & TAG_MODULE) {
				int idx = (int)(tag & 0xffff);
				if (idx < G.n_modules && G.modules[idx].pid)
					on_module_exit(&G.modules[idx]);
			}
		}
		if (G.need_prune) {
			G.need_prune = 0;
			prune_modules();
			request_publish();
		}
		if (G.publish_now)
			publish_own_topics();
		arm_timer();
	}
	/* Shutdown: the registry and the Neutral records go, then we do. */
	hg_pub_close(G.pub_system);
	hg_pub_close(G.pub_modules);
	hg_pub_close(G.pub_admin);
	for (int i = 0; i < G.n_modules; i++)
		neutral_record_remove(G.modules[i].name);
	unlink(G.paths.registry);
	unlink(G.paths.control);
	hd_log("stopped");
	return 0;
}
