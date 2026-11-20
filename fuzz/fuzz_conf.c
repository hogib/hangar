// SPDX-License-Identifier: GPL-3.0-or-later
/* The small text files the programs read at start or on a command, each from
 * the reader its program uses. All are local files the operator or the
 * manager writes (most of them root's); none is network input. They are here
 * because a reader that misbehaves on a damaged file does so at boot.
 *
 * Input: one byte choosing the reader, then the file's bytes:
 *
 *   0  hangard.conf                    hangard/conf.c conf_load
 *   1  the manager's registry          hangard/registry.c registry_read (/run/hangar/registry,
 *                                      read back at adoption after a re-exec or a crash)
 *   2  mavlink.conf, vehicle           modules/mavlink/mavlink_conf.c mavlink_conf_load
 *   3  topside_mavlink.conf            the same, topside
 *   4  ardusub.params                  mavlink_params_load (the parameters the module checks)
 *   5  recorder.conf                   modules/recorder/recorder_conf.c rec_conf_load, rec_conf_resolve
 *   6  joystick.conf                   modules/joystick/joystick_map.c js_conf_parse
 *   7  an ArduSub parameter dump       hangar/hangar_params.c read_table and compare
 *                                      (`hangar ardusub-params diff|restore <file>`; QGC and MAVROS files)
 *   8  a parameter backup              modules/mavlink/param_backup.c: pj_start_load reads it, then
 *                                      the job runs against an autopilot that reports two parameters
 *                                      at -10: what differs is sent, and must be a finite number */
#include "fuzz.h"

#include <fcntl.h>
#include <sys/stat.h>

#include "hangard.h"
#include "joystick_map.h"
#include "mavlink_conf.h"
#include "param_backup.h"
#include "recorder.h"

/* hangar_params.c is included for read_table and compare, which are static;
 * what it takes from hangar.c is stood in for. */
int hangar_call(const char *target, char **kv, int nkv, long timeout_ms, char reason[64], void *rep, size_t rep_max)
{
	(void)target, (void)kv, (void)nkv, (void)timeout_ms, (void)reason, (void)rep, (void)rep_max;
	return -1;
}

const char *hangar_topic_dir(void)
{
	return "/nonexistent";
}

#include "hangar_params.c"

/* registry.c's view of the manager. */
struct hangard G;

void hd_log(const char *fmt, ...)
{
	(void)fmt;
}

void hd_die(const char *fmt, ...)
{
	(void)fmt;
	abort();
}

static char dir[256], backup[320];

static void cleanup(void)
{
	unlink(backup);
	rmdir(dir);
}

static void no_send_list(void)
{
}

static void no_send_read(int index)
{
	(void)index;
}

/* What a load sends to the autopilot: never a value that is not a number. */
static void no_send_set(const char *id, float value, uint8_t type)
{
	(void)id, (void)type;
	FZ_CHECK(value == value && value - value == 0);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (!dir[0]) {
		const char *tmp = getenv("TMPDIR");
		snprintf(dir, sizeof dir, "%s/hg-fuzz-conf-XXXXXX", tmp && *tmp ? tmp : "/tmp");
		if (!mkdtemp(dir))
			abort();
		snprintf(backup, sizeof backup, "%s/fuzz.params", dir);
		atexit(cleanup);
		struct pj_send send = { no_send_list, no_send_read, no_send_set };
		pj_init(dir, &send);
	}
	if (size < 1)
		return 0;
	unsigned which = data[0] % 9;
	data++;
	size--;
	char path[64], err[400] = "";
	int fd = fz_file(data, size, path);
	switch (which) {
	case 0: {
		static struct paths pp;
		static struct conf c;
		unsetenv("HANGAR_ROOT");
		paths_init(&pp, path);
		int rc = conf_load(path, &pp, &c, err, sizeof err);
		FZ_NOTE("hangard.conf: %d %s", rc, err);
		if (rc == 0) {
			FZ_CHECK(strnlen(c.role, sizeof c.role) < sizeof c.role);
			FZ_CHECK(strnlen(c.module_dir, sizeof c.module_dir) < sizeof c.module_dir);
			FZ_CHECK(c.max_modules >= 0);
		}
		break;
	}
	case 1: {
		static struct reg_entry e[HD_MAX_MODULES];
		enum sys_state sys = SYS_NOMINAL;
		uint64_t since = 0;
		char reason[160];
		int n = registry_read(path, e, HD_MAX_MODULES, &sys, &since, reason, sizeof reason);
		FZ_NOTE("registry: %d entries", n);
		FZ_CHECK(n <= HD_MAX_MODULES);
		FZ_CHECK((int)sys >= 0 && (int)sys < 3);
		for (int i = 0; i < n; i++) {
			FZ_CHECK(strnlen(e[i].name, sizeof e[i].name) < sizeof e[i].name);
			FZ_CHECK((int)e[i].st >= 0 && (int)e[i].st < ST_COUNT);
		}
		break;
	}
	case 2:
	case 3: {
		static struct mavlink_conf c;
		int rc = mavlink_conf_load(&c, path, which == 3, err, sizeof err);
		FZ_NOTE("mavlink.conf: %d %s", rc, err);
		if (rc == 0) {
			FZ_CHECK(c.n_telemetry >= 0 && c.n_telemetry <= MC_TELEMETRY);
			FZ_CHECK(strnlen(c.serial, sizeof c.serial) < sizeof c.serial);
		}
		break;
	}
	case 4: {
		static struct mc_param p[MC_MAX_PARAMS];
		int n = mavlink_params_load(path, p, MC_MAX_PARAMS, err, sizeof err);
		FZ_NOTE("ardusub.params: %d %s", n, err);
		FZ_CHECK(n <= MC_MAX_PARAMS);
		for (int i = 0; i < n; i++)
			FZ_CHECK(strnlen(p[i].id, sizeof p[i].id) < sizeof p[i].id);
		break;
	}
	case 5: {
		static struct rec_conf c;
		int rc = rec_conf_load(path, &c, err, sizeof err);
		FZ_NOTE("recorder.conf: %d %s", rc, err);
		if (rc == 0) {
			struct rec_policy pol;
			FZ_CHECK(c.n_rules >= 0 && c.n_rules <= REC_MAX_RULES);
			rec_conf_resolve(&c, "nav.depth", 0, 50000, &pol);
			rec_conf_resolve(&c, "cam.pilot", 1, 30000, &pol);
			rec_conf_resolve(&c, "safety.req", 0, 0, &pol);
			FZ_CHECK(strnlen(pol.text, sizeof pol.text) < sizeof pol.text);
		}
		break;
	}
	case 6: {
		static struct js_conf c;
		char *text = fz_text(data, size);
		js_conf_defaults(&c);
		int rc = js_conf_parse(text, size, &c, err, sizeof err);
		FZ_NOTE("joystick.conf: %d %s", rc, err);
		if (rc == 0) {
			struct js_state st;
			float out[JS_WRENCH_AXES];
			memset(&st, 0, sizeof st);
			struct js_ev ev = { .value = 32767, .type = JS_EV_AXIS, .number = 1 };
			js_apply(&st, &ev);
			ev = (struct js_ev){ .value = 1, .type = JS_EV_BUTTON, .number = 3 };
			js_apply(&st, &ev);
			js_wrench(&c, &st, out);
			(void)js_pass_bits(&c, &st);
			for (int i = 0; i < JS_WRENCH_AXES; i++)
				FZ_CHECK(out[i] >= -1.0f && out[i] <= 1.0f);
		}
		free(text);
		break;
	}
	case 7: {
		struct table want = { 0 }, now = { 0 };
		int rc = read_table(path, &want);
		FZ_NOTE("parameter dump: %d, %d parameters", rc, want.n);
		if (rc == 0 && read_table(path, &now) == 0) {
			FZ_CHECK(want.n <= MAXP);
			FZ_CHECK(compare(&want, &now, 1) >= 0);
			if (now.n > 1)
				now.p[now.n / 2].value += 1.0f;
			(void)compare(&want, &now, 1);
		}
		free(want.p);
		free(now.p);
		break;
	}
	case 8: {
		int out = open(backup, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
		if (out < 0 || (size && write(out, data, size) != (ssize_t)size))
			abort();
		close(out);
		uint64_t now = 1000000000;
		char reason[200] = "";
		pj_abort("a new input", now);
		int rc = pj_start_load("fuzz", now, reason, sizeof reason);
		FZ_NOTE("parameter backup: %d %s", rc, reason);
		struct params_job j;
		for (int i = 0; rc == 0 && i < 8; i++) {
			now += 1000000000;
			pj_param_value("SURFACE_DEPTH", -10.0f, 9, (uint16_t)i, 2, now);
			pj_tick(now);
			(void)pj_status(&j, now);
		}
		pj_abort("done", now);
		break;
	}
	}
	close(fd);
	return 0;
}
