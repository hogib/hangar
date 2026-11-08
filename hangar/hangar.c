// SPDX-License-Identifier: GPL-3.0-or-later
/* hangar: the command-line tool (docs/12-tools.md), stages 2a and 2b:
 *   status, start|stop|restart <module>, reload, restart-manager,
 *   clear-failsafe        over the control socket
 *   show <module>         its wiring (from the manager) and its page
 *   topics                every topic file, with its header fields
 *   echo <topic>          messages decoded by the schema in the header
 *   hz <topic>            measured rate and jitter
 *   run <module>          a module in the foreground, wired from its manifest
 *   call <module>.<method> [field=value ...]
 *                         a call as admin:<user>, through the manager's
 *                         hangar.<role>.admin.req (docs/04 "Admin calls");
 *                         the reply read from the server's .rep topic
 *   param get|set|load|reset <module> ...
 *                         parameters (docs/05): the topic, set_params
 *                         through the manager, or param-write for a module
 *                         that is not running
 * No daemon behind it: topics and pages are read directly. Plain ANSI
 * output. Paths follow $HANGAR_ROOT like the manager's. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hg_call.h"
#include "hg_control.h"
#include "hg_params.h"
#include "hg_topic.h"
#include "manifest.h"
#include "wiring.h"

static const char *root(void)
{
	const char *r = getenv("HANGAR_ROOT");
	return r && *r ? r : "";
}

static const char *control_path(void)
{
	static char buf[512];
	const char *c = getenv("HANGAR_CONTROL");
	if (c && *c)
		return c;
	snprintf(buf, sizeof buf, "%s/run/hangar/control", root());
	return buf;
}

static const char *topic_dir(void)
{
	static char buf[512];
	const char *d = getenv("HANGAR_TOPIC_DIR");
	if (d && *d)
		return d;
	/* The manager knows: its configuration may keep the topics on a tmpfs
	 * outside $HANGAR_ROOT (the release's hangard.conf says /dev/shm/hangar
	 * so that a root under $HOME does not put them on the SD card). Without
	 * a manager, the relocation rule. */
	char reply[2048];
	ssize_t n = hg_control_call(control_path(), "paths", 5, reply, sizeof reply, NULL, 0);
	if (n > 0) {
		reply[n < (ssize_t)sizeof reply ? n : (ssize_t)sizeof reply - 1] = 0;
		const char *t = strstr(reply, "\ntopics ");
		if (t) {
			t += 8;
			const char *e = strchr(t, '\n');
			size_t l = e ? (size_t)(e - t) : strlen(t);
			if (l > 0 && l < sizeof buf) {
				memcpy(buf, t, l);
				buf[l] = 0;
				setenv("HANGAR_TOPIC_DIR", buf, 1);
				return buf;
			}
		}
	}
	snprintf(buf, sizeof buf, "%s/dev/shm/hangar", root());
	setenv("HANGAR_TOPIC_DIR", buf, 1);
	return buf;
}

static int fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int fail(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("hangar: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	return 1;
}

/* One request; prints the reply body (after "ok\n") when print is set. */
static int request(const char *req, int print, char *reply, size_t len)
{
	static char buf[HG_CONTROL_MAX];
	if (!reply) {
		reply = buf;
		len = sizeof buf;
	}
	unsetenv("HANGAR_CONTROL"); /* the tool is not a module */
	ssize_t n = hg_control_call(control_path(), req, strlen(req), reply, len, NULL, 0);
	if (n < 0)
		return fail("cannot reach the manager at %s: %s", control_path(), strerror(errno));
	if (!hg_control_ok(reply)) {
		char *nl = strchr(reply, '\n');
		if (nl)
			*nl = 0;
		return fail("%s", strncmp(reply, "err ", 4) == 0 ? reply + 4 : reply);
	}
	if (print) {
		const char *body = strchr(reply, '\n');
		if (body && body[1])
			fputs(body + 1, stdout);
	}
	return 0;
}

static const char *ago(uint64_t since_ns, char *buf, size_t len)
{
	uint64_t now = hg_now_ns();
	if (!since_ns || since_ns > now) {
		snprintf(buf, len, "-");
		return buf;
	}
	uint64_t s = (now - since_ns) / 1000000000u;
	if (s < 60)
		snprintf(buf, len, "%llus", (unsigned long long)s);
	else if (s < 3600)
		snprintf(buf, len, "%llum%02llus", (unsigned long long)s / 60, (unsigned long long)s % 60);
	else
		snprintf(buf, len, "%lluh%02llum", (unsigned long long)s / 3600, (unsigned long long)(s % 3600) / 60);
	return buf;
}

/* ---- status --------------------------------------------------------------- */

static int cmd_status(void)
{
	static char reply[HG_CONTROL_MAX];
	if (request("status", 0, reply, sizeof reply))
		return 1;
	char *save;
	char *line = strtok_r(reply, "\n", &save); /* "ok" */
	int header = 0;
	while ((line = strtok_r(NULL, "\n", &save))) {
		if (strncmp(line, "system ", 7) == 0) {
			char st[32];
			unsigned long long since;
			int off = 0;
			if (sscanf(line + 7, "%31s %llu %n", st, &since, &off) >= 2) {
				char a[32];
				printf("system   %s  (for %s)%s%s\n", st, ago(since, a, sizeof a),
				       line[7 + off] ? "  " : "", line + 7 + off);
			}
			continue;
		}
		if (strncmp(line, "module ", 7) != 0)
			continue;
		if (!header) {
			printf("%-20s %-10s %6s %5s %-10s %6s  %s\n", "MODULE", "STATE", "PID", "FAILS", "LAST EXIT",
			       "SINCE", "STATUS");
			header = 1;
		}
		char name[32], st[32], ex[32];
		int pid;
		unsigned fails;
		unsigned long long since;
		int off = 0;
		if (sscanf(line + 7, "%31s %31s %d %u %31s %llu %n", name, st, &pid, &fails, ex, &since, &off) < 6)
			continue;
		char a[32];
		printf("%-20s %-10s %6d %5u %-10s %6s  %s\n", name, st, pid, fails, ex, ago(since, a, sizeof a),
		       line + 7 + off);
	}
	return 0;
}

/* ---- show ----------------------------------------------------------------- */

static int cmd_show(const char *name)
{
	static char reply[HG_CONTROL_MAX];
	char req[80];
	snprintf(req, sizeof req, "wiring %s", name);
	printf("# wiring of %s, as handed to it\n", name);
	int rc = request(req, 1, reply, sizeof reply);
	char path[512];
	snprintf(path, sizeof path, "%s/run/hangar/modules/%s", root(), name);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		printf("# no module page at %s (%s)\n", path, strerror(errno));
		return rc;
	}
	unsigned char page[4096];
	ssize_t n = read(fd, page, sizeof page);
	close(fd);
	if (n < 192 || memcmp(page, "HGPAGE01", 8) != 0) {
		printf("# %s is not an HGPAGE01 page\n", path);
		return rc;
	}
	uint64_t instance, ready, alive;
	memcpy(&instance, page + 8, 8);
	memcpy(&ready, page + 16, 8);
	memcpy(&alive, page + 24, 8);
	char a1[32], a2[32];
	printf("# page %s\n", path);
	printf("instance   %llu (started %s ago)\n", (unsigned long long)instance, ago(instance, a1, sizeof a1));
	printf("ready      %s\n", ready && ready == instance ? "yes" : ready ? "an earlier instance" : "no");
	printf("alive      %s\n", alive ? ago(alive, a2, sizeof a2) : "never");
	printf("status     %.127s\n", (const char *)page + 64);
	return rc;
}

/* ---- topics --------------------------------------------------------------- */

static unsigned char *map_header(const char *path, size_t *len_out)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	struct stat st;
	unsigned char *h = NULL;
	if (fstat(fd, &st) == 0 && st.st_size >= 4096) {
		void *m = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
		if (m != MAP_FAILED) {
			if (memcmp(m, "HGTOPIC1", 8) == 0)
				h = m;
			else
				munmap(m, 4096);
		}
	}
	close(fd);
	if (h && len_out)
		*len_out = (size_t)st.st_size;
	return h;
}

static uint64_t u64(const unsigned char *h, size_t off)
{
	uint64_t v;
	memcpy(&v, h + off, 8);
	return v;
}

static uint32_t u32(const unsigned char *h, size_t off)
{
	uint32_t v;
	memcpy(&v, h + off, 4);
	return v;
}

static void fmt_rate(uint32_t mhz, char *buf, size_t len)
{
	if (!mhz)
		snprintf(buf, len, "event");
	else if (mhz % 1000 == 0)
		snprintf(buf, len, "%u Hz", mhz / 1000);
	else
		snprintf(buf, len, "%.3g Hz", mhz / 1000.0);
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static int cmd_topics(void)
{
	const char *dir = topic_dir();
	DIR *d = opendir(dir);
	if (!d)
		return fail("cannot read %s: %s", dir, strerror(errno));
	char *names[4096];
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) && n < 4096)
		if (e->d_name[0] != '.')
			names[n++] = strdup(e->d_name);
	closedir(d);
	qsort(names, (size_t)n, sizeof names[0], cmp_str);
	printf("%-32s %-16s %-8s %10s %8s %-16s %5s %s\n", "TOPIC", "TYPE HASH", "RATE", "SEQ", "AGE", "WRITER",
	       "SLOTS", "FLAGS");
	uint64_t now = hg_now_ns();
	for (int i = 0; i < n; i++) {
		char path[1024];
		snprintf(path, sizeof path, "%s/%s", dir, names[i]);
		unsigned char *h = map_header(path, NULL);
		if (!h)
			continue;
		char rate[16], age[32], flags[32] = "", writer[33];
		fmt_rate(u32(h, 60), rate, sizeof rate);
		uint64_t last = u64(h, 208);
		if (!last)
			snprintf(age, sizeof age, "-");
		else if (now > last)
			snprintf(age, sizeof age, "%llu ms", (unsigned long long)((now - last) / 1000000u));
		else
			snprintf(age, sizeof age, "0 ms");
		uint32_t f = u32(h, 36);
		if (f & 1)
			strcat(flags, "closed ");
		if (f & 2)
			strcat(flags, "died ");
		if (f & 4)
			strcat(flags, "replaced ");
		if (u32(h, 60) && last && now > last && now - last > u64(h, 176))
			strcat(flags, "stale");
		memcpy(writer, h + 120, 32);
		writer[32] = 0;
		printf("%-32s %016llx %-8s %10llu %8s %-16s %5u %s\n", names[i], (unsigned long long)u64(h, 64), rate,
		       (unsigned long long)u64(h, 200), age, writer, u32(h, 24), flags);
		munmap(h, 4096);
		free(names[i]);
	}
	return 0;
}

/* ---- echo and hz ------------------------------------------------------------ */

static void print_value(const struct hg_field *f, const unsigned char *p, int json)
{
	if (f->kind == HG_CHAR) {
		printf(json ? "\"%.*s\"" : "\"%.*s\"", (int)f->count, (const char *)p);
		return;
	}
	if (f->count > 1)
		printf("[");
	for (uint32_t i = 0; i < f->count; i++) {
		const unsigned char *q = p + i * hg_field_elem_size(f->kind);
		if (i)
			printf(json ? "," : " ");
		switch (f->kind) {
		case HG_U8: printf("%u", *q); break;
		case HG_I8: printf("%d", *(const signed char *)q); break;
		case HG_U16: { uint16_t v; memcpy(&v, q, 2); printf("%u", v); break; }
		case HG_I16: { int16_t v; memcpy(&v, q, 2); printf("%d", v); break; }
		case HG_U32: { uint32_t v; memcpy(&v, q, 4); printf("%u", v); break; }
		case HG_I32: { int32_t v; memcpy(&v, q, 4); printf("%d", v); break; }
		case HG_U64: { uint64_t v; memcpy(&v, q, 8); printf("%llu", (unsigned long long)v); break; }
		case HG_I64: { int64_t v; memcpy(&v, q, 8); printf("%lld", (long long)v); break; }
		case HG_F32: { float v; memcpy(&v, q, 4); printf("%g", (double)v); break; }
		case HG_F64: { double v; memcpy(&v, q, 8); printf("%g", v); break; }
		default: break;
		}
	}
	if (f->count > 1)
		printf("]");
}

static volatile sig_atomic_t stopping;

static void on_int(int sig)
{
	(void)sig;
	stopping = 1;
	hg_wait_interrupt();
}

static int field_wanted(const char *fields, const char *name)
{
	if (!fields)
		return 1;
	size_t n = strlen(name);
	for (const char *p = fields; *p;) {
		const char *c = strchr(p, ',');
		size_t len = c ? (size_t)(c - p) : strlen(p);
		if (len == n && strncmp(p, name, n) == 0)
			return 1;
		if (!c)
			break;
		p = c + 1;
	}
	return 0;
}

static int cmd_echo(int argc, char **argv)
{
	const char *topic = NULL, *fields = NULL;
	int json = 0, show_rate = 0;
	long count = -1;
	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--json") == 0)
			json = 1;
		else if (strcmp(argv[i], "--rate") == 0)
			show_rate = 1;
		else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc)
			count = atol(argv[++i]);
		else if (strcmp(argv[i], "--fields") == 0 && i + 1 < argc)
			fields = argv[++i];
		else if (argv[i][0] != '-' && !topic)
			topic = argv[i];
		else
			return fail("echo: unknown option %s", argv[i]);
	}
	if (!topic)
		return fail("usage: hangar echo <topic> [--rate] [--json] [--fields a,b] [--count N]");
	topic_dir();
	hg_sub *s = hg_sub_open(topic, NULL);
	if (!s)
		return fail("cannot subscribe to %s: %s", topic, strerror(errno));
	signal(SIGINT, on_int);
	signal(SIGTERM, on_int);
	struct hg_schema *schema = calloc(1, sizeof *schema);
	unsigned char *buf = NULL;
	size_t buflen = 0;
	long seen = 0;
	uint64_t last_pub = 0;
	while (!stopping && (count < 0 || seen < count)) {
		unsigned r = hg_wait(&s, 1, 500);
		if (r & HG_READY_INTERRUPT)
			break;
		struct hg_info info;
		for (;;) {
			uint32_t pl = hg_sub_payload_bytes(s);
			if (!pl)
				break;
			if (pl > buflen) {
				buf = realloc(buf, pl);
				buflen = pl;
			}
			enum hg_result res = hg_next(s, buf, buflen, &info);
			if (res != HG_MSG) {
				if (res == HG_CLOSED && !json)
					printf("# writer %s\n", "closed or died");
				break;
			}
			uint32_t slen;
			const char *text = hg_sub_schema(s, &slen);
			if (!schema->type.n_fields || schema->type.schema_bytes != slen) {
				char err[128];
				if (hg_schema_parse(text, slen, schema, err, sizeof err) < 0) {
					fail("schema: %s", err);
					free(buf);
					return 1;
				}
			}
			if (json)
				printf("{\"seq\":%llu,\"stamp_ns\":%llu,\"publish_ns\":%llu,\"stale\":%s",
				       (unsigned long long)info.seq, (unsigned long long)info.stamp_ns,
				       (unsigned long long)info.publish_ns, info.flags & HG_SLOT_STALE ? "true" : "false");
			else
				printf("--- seq %llu  stamp %llu  gap %llu%s\n", (unsigned long long)info.seq,
				       (unsigned long long)info.stamp_ns, (unsigned long long)info.gap,
				       info.flags & HG_SLOT_STALE ? "  [stale inputs]" : "");
			if (show_rate && last_pub && info.publish_ns > last_pub)
				printf(json ? ",\"interval_ms\":%.3f" : "    interval %.3f ms\n",
				       (double)(info.publish_ns - last_pub) / 1e6);
			last_pub = info.publish_ns;
			for (uint32_t i = 0; i < schema->type.n_fields; i++) {
				const struct hg_field *f = &schema->fields[i];
				if (!field_wanted(fields, f->name))
					continue;
				if (json)
					printf(",\"%s\":", f->name);
				else
					printf("%-20s ", f->name);
				print_value(f, buf + f->offset, json);
				if (!json)
					printf("%s%s\n", f->comment[0] ? "    # " : "", f->comment);
			}
			if (json)
				printf("}\n");
			fflush(stdout);
			if (++seen == count)
				break;
		}
	}
	free(buf);
	free(schema);
	hg_sub_close(s);
	return 0;
}

static int cmd_hz(int argc, char **argv)
{
	const char *topic = NULL;
	long seconds = -1;
	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
			seconds = atol(argv[++i]);
		else if (argv[i][0] != '-' && !topic)
			topic = argv[i];
		else
			return fail("hz: unknown option %s", argv[i]);
	}
	if (!topic)
		return fail("usage: hangar hz <topic> [--seconds N]");
	topic_dir();
	hg_sub *s = hg_sub_open(topic, NULL);
	if (!s)
		return fail("cannot subscribe to %s: %s", topic, strerror(errno));
	signal(SIGINT, on_int);
	signal(SIGTERM, on_int);
	long windows = 0;
	uint64_t window_start = hg_now_ns(), last = 0;
	uint64_t n = 0, min_dt = UINT64_MAX, max_dt = 0, sum_dt = 0;
	while (!stopping && (seconds < 0 || windows < seconds)) {
		unsigned r = hg_wait(&s, 1, 100);
		if (r & HG_READY_INTERRUPT)
			break;
		struct hg_info info;
		unsigned char buf[65536];
		for (;;) {
			uint32_t pl = hg_sub_payload_bytes(s);
			if (!pl || pl > sizeof buf)
				break;
			if (hg_next(s, buf, pl, &info) != HG_MSG)
				break;
			if (last && info.publish_ns > last) {
				uint64_t dt = info.publish_ns - last;
				if (dt < min_dt)
					min_dt = dt;
				if (dt > max_dt)
					max_dt = dt;
				sum_dt += dt;
			}
			last = info.publish_ns;
			n++;
		}
		uint64_t now = hg_now_ns();
		if (now - window_start >= 1000000000ull) {
			double secs = (double)(now - window_start) / 1e9;
			if (n > 1)
				printf("%s: %.2f Hz  interval mean %.2f ms  min %.2f  max %.2f  jitter ±%.2f ms  (%llu messages)\n",
				       topic, (double)n / secs, (double)sum_dt / (double)(n - 1) / 1e6, (double)min_dt / 1e6,
				       (double)max_dt / 1e6, (double)(max_dt - min_dt) / 2e6, (unsigned long long)n);
			else
				printf("%s: %.2f Hz  (%llu messages)\n", topic, (double)n / secs, (unsigned long long)n);
			fflush(stdout);
			windows++;
			window_start = now;
			n = 0;
			min_dt = UINT64_MAX;
			max_dt = sum_dt = 0;
		}
	}
	hg_sub_close(s);
	return 0;
}

/* ---- run ------------------------------------------------------------------ */

/* hangard.conf's ModuleDir, MsgDir and Role, if the file exists. */
static void read_conf(char *module_dir, size_t mdl, char *msg_dir, size_t msl, char *role, size_t rl)
{
	snprintf(module_dir, mdl, "%s/etc/hangar/modules", root());
	snprintf(msg_dir, msl, "%s/usr/share/hangar/msg", root());
	snprintf(role, rl, "vehicle");
	char path[512];
	snprintf(path, sizeof path, "%s/etc/hangar/hangard.conf", root());
	FILE *f = fopen(path, "re");
	if (!f)
		return;
	char line[512];
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		char *k = line, *v = eq + 1;
		while (*k == ' ')
			k++;
		size_t n = strlen(v);
		while (n && (v[n - 1] == '\n' || v[n - 1] == ' '))
			v[--n] = 0;
		while (*v == ' ')
			v++;
		if (strcmp(k, "ModuleDir") == 0)
			snprintf(module_dir, mdl, "%s", v);
		else if (strcmp(k, "MsgDir") == 0)
			snprintf(msg_dir, msl, "%s", v);
		else if (strcmp(k, "Role") == 0)
			snprintf(role, rl, "%s", v);
	}
	fclose(f);
}

static int cmd_run(int argc, char **argv)
{
	if (argc < 1)
		return fail("usage: hangar run <module | path/to/name.module> [--msgdir DIR] [--role vehicle|topside]");
	char module_dir[512], msg_dir[512], role[16];
	read_conf(module_dir, sizeof module_dir, msg_dir, sizeof msg_dir, role, sizeof role);
	const char *what = argv[0];
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--msgdir") == 0 && i + 1 < argc)
			snprintf(msg_dir, sizeof msg_dir, "%s", argv[++i]);
		else if (strcmp(argv[i], "--role") == 0 && i + 1 < argc)
			snprintf(role, sizeof role, "%s", argv[++i]);
		else
			return fail("run: unknown option %s", argv[i]);
	}
	char path[600];
	if (strchr(what, '/'))
		snprintf(path, sizeof path, "%s", what);
	else
		snprintf(path, sizeof path, "%s/%s.module", module_dir, what);
	struct mf_module *m = calloc(1, sizeof *m);
	struct mf_set *set = calloc(1, sizeof *set);
	if (!m || !set)
		return fail("out of memory");
	char err[MF_ERR];
	if (mf_parse_file(path, m, err, sizeof err) < 0)
		return fail("%s", err);
	mf_set_init(set, role, "", msg_dir);
	mf_set_add(set, m);
	mf_set_check(set);
	for (int i = 0; i < set->n_warnings; i++)
		fprintf(stderr, "hangar: warning: %s\n", set->warnings[i]);
	if (m->refused)
		return fail("%s", m->reason);
	if (m->legacy)
		fprintf(stderr, "hangar: %s is a legacy module: run without wiring\n", m->name);
	/* A private topic directory under the user's runtime directory. */
	char tdir[512];
	const char *env_dir = getenv("HANGAR_TOPIC_DIR");
	if (env_dir && *env_dir) {
		snprintf(tdir, sizeof tdir, "%s", env_dir);
	} else {
		const char *xdg = getenv("XDG_RUNTIME_DIR");
		if (xdg && *xdg)
			snprintf(tdir, sizeof tdir, "%s/hangar-run", xdg);
		else
			snprintf(tdir, sizeof tdir, "/dev/shm/hangar-run-%lu", (unsigned long)getuid());
		mkdir(tdir, 0700);
		size_t l = strlen(tdir);
		snprintf(tdir + l, sizeof tdir - l, "/%s", m->name);
	}
	if (mkdir(tdir, 0700) < 0 && errno != EEXIST)
		return fail("cannot create %s: %s", tdir, strerror(errno));
	char wpath[600];
	snprintf(wpath, sizeof wpath, "%s/.wiring", tdir);
	static char text[65536];
	int len = wiring_text(set, m, text, sizeof text);
	if (len < 0)
		return fail("the wiring does not fit");
	int fd = open(wpath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0 || write(fd, text, (size_t)len) != len)
		return fail("cannot write %s: %s", wpath, strerror(errno));
	close(fd);
	fprintf(stderr, "hangar run: %s, topics in %s, wiring %s\n", m->name, tdir, wpath);
	fputs(text, stderr);
	setenv("HANGAR_TOPIC_DIR", tdir, 1);
	setenv("HANGAR_MODULE", m->name, 1);
	setenv("HANGAR_WIRING_FILE", wpath, 1);
	setenv("HANGAR_STATE_DIR", tdir, 1); /* a [Params] module's saved file, beside its topics */
	unsetenv("HANGAR_WIRING");
	unsetenv("HANGAR_CONTROL");
	for (int i = 0; i < m->n_env; i++)
		putenv(m->env[i]);
	execv(m->exec.argv[0], m->exec.argv);
	return fail("cannot run %s: %s", m->exec.argv[0], strerror(errno));
}

/* ---- call and param ---------------------------------------------------------- */

/* One request of any bytes; the reply's length, or -1 after a message. */
static ssize_t request_bin(const void *req, size_t len, char *reply, size_t replylen)
{
	unsetenv("HANGAR_CONTROL");
	ssize_t n = hg_control_call(control_path(), req, len, reply, replylen, NULL, 0);
	if (n < 0) {
		fail("cannot reach the manager at %s: %s", control_path(), strerror(errno));
		return -1;
	}
	if (!hg_control_ok(reply)) {
		char *nl = strchr(reply, '\n');
		if (nl)
			*nl = 0;
		fail("%s", strncmp(reply, "err ", 4) == 0 ? reply + 4 : reply);
		return -1;
	}
	return n;
}

/* What the manager says about a method: its types, with their schemas. */
struct method_info {
	char req_name[64], rep_name[64];
	unsigned long long req_hash, rep_hash;
	unsigned req_size, rep_size, req_schema_len, rep_schema_len;
	unsigned long long deadline_ms;
	int running;
	struct hg_schema req, rep;
};

static int method_info(const char *target, struct method_info *mi)
{
	static char reply[HG_CONTROL_MAX];
	char req[128];
	snprintf(req, sizeof req, "method %s", target);
	ssize_t n = request_bin(req, strlen(req), reply, sizeof reply);
	if (n < 0)
		return -1;
	memset(mi, 0, sizeof *mi);
	char *line = strchr(reply, '\n');
	char *end = line ? strchr(line + 1, '\n') : NULL;
	if (!line || !end)
		return fail("method: malformed reply");
	*end = 0;
	if (sscanf(line + 1, "req=%63s hash=%llx size=%u rep=%63s hash=%llx size=%u deadline=%llu running=%d reqschema=%u repschema=%u",
		   mi->req_name, &mi->req_hash, &mi->req_size, mi->rep_name, &mi->rep_hash, &mi->rep_size,
		   &mi->deadline_ms, &mi->running, &mi->req_schema_len, &mi->rep_schema_len) != 10)
		return fail("method: malformed reply line: %s", line + 1);
	const char *rq = end + 1, *rp = rq + mi->req_schema_len;
	if (rp + mi->rep_schema_len > reply + n)
		return fail("method: the schemas are short");
	char err[128];
	if (hg_schema_parse(rq, mi->req_schema_len, &mi->req, err, sizeof err) < 0 ||
	    hg_schema_parse(rp, mi->rep_schema_len, &mi->rep, err, sizeof err) < 0)
		return fail("method: schema: %s", err);
	mi->req.type.name = mi->req_name;
	mi->rep.type.name = mi->rep_name;
	return 0;
}

/* field=value into a request: numbers as written, arrays comma-separated,
 * text quoted or bare. */
static int set_field(const struct hg_type *t, unsigned char *buf, const char *kv)
{
	const char *eq = strchr(kv, '=');
	if (!eq || eq == kv)
		return fail("'%s' is not field=value", kv);
	char name[80];
	snprintf(name, sizeof name, "%.*s", (int)(eq - kv), kv);
	const struct hg_field *f = hg_type_field(t, name);
	if (!f)
		return fail("%s: no field %s", t->name, name);
	const char *v = eq + 1;
	char quoted[HG_SCHEMA_MAX];
	if (f->kind == HG_CHAR && v[0] != '"') {
		snprintf(quoted, sizeof quoted, "\"%s\"", v);
		v = quoted;
	}
	char err[160];
	if (hg_param_parse(f, v, strlen(v), buf, err, sizeof err) < 0)
		return fail("%s", err);
	return 0;
}

static void print_fields(const struct hg_type *t, const unsigned char *buf, uint32_t from, int json,
			 const char *const *only, int n_only)
{
	int first = 1;
	for (uint32_t i = from; i < t->n_fields; i++) {
		const struct hg_field *f = &t->fields[i];
		if (n_only) {
			int want = 0;
			for (int k = 0; k < n_only; k++)
				if (strcmp(only[k], f->name) == 0)
					want = 1;
			if (!want)
				continue;
		}
		if (json) {
			printf("%s\"%s\":", first ? "" : ",", f->name);
			print_value(f, buf + f->offset, 1);
		} else {
			printf("%-20s ", f->name);
			print_value(f, buf + f->offset, 0);
			printf("%s%s\n", f->comment[0] ? "    # " : "", f->comment);
		}
		first = 0;
	}
}

/* Sends the call and waits for its reply on the server's .rep topic. The
 * subscription is opened before the request is published, so the reply
 * cannot be missed. Returns the status, HG_E_TIMEOUT included; -100 when
 * the manager refused the request. */
static int do_call(const char *target, const struct method_info *mi, const unsigned char *body, size_t body_len,
		   long timeout_ms, struct hg_call_reply_envelope *env, unsigned char *rep_body, size_t rep_max)
{
	const char *dot = strchr(target, '.');
	char rep_topic[64];
	snprintf(rep_topic, sizeof rep_topic, "%.*s.rep", (int)(dot - target), target);
	topic_dir();
	hg_sub *sub = hg_sub_open(rep_topic, NULL);
	if (!sub) {
		fail("cannot subscribe to %s: %s", rep_topic, strerror(errno));
		return -100;
	}
	static unsigned char req[HG_CONTROL_MAX];
	int n = snprintf((char *)req, sizeof req, "call %s deadline=%ld type=%016llx body=%zu\n", target, timeout_ms,
			 mi->req_hash, body_len);
	memcpy(req + n, body, body_len);
	static char reply[HG_CONTROL_MAX];
	if (request_bin(req, (size_t)n + body_len, reply, sizeof reply) < 0) {
		hg_sub_close(sub);
		return -100;
	}
	unsigned long long call_id;
	unsigned caller_id;
	if (sscanf(reply, "ok %llu %u", &call_id, &caller_id) != 2) {
		hg_sub_close(sub);
		fail("call: malformed reply %s", reply);
		return -100;
	}
	uint64_t deadline = hg_now_ns() + (uint64_t)timeout_ms * 1000000u;
	static unsigned char buf[HG_CALL_REP_ENVELOPE + HG_CALL_MAX_PAYLOAD];
	int status = HG_E_TIMEOUT;
	memset(env, 0, sizeof *env);
	for (;;) {
		struct hg_info info;
		enum hg_result r = hg_next(sub, buf, sizeof buf, &info);
		if (r == HG_MSG) {
			struct hg_call_reply_envelope e;
			memcpy(&e, buf, sizeof e);
			if (e.call_id == call_id && e.caller_id == caller_id) {
				*env = e;
				status = e.status;
				size_t have = hg_sub_payload_bytes(sub) - HG_CALL_REP_ENVELOPE;
				if (have > rep_max)
					have = rep_max;
				memcpy(rep_body, buf + HG_CALL_REP_ENVELOPE, have);
				break;
			}
			continue;
		}
		uint64_t now = hg_now_ns();
		if (now >= deadline) {
			snprintf(env->reason, sizeof env->reason, "no reply within %ld ms", timeout_ms);
			break;
		}
		hg_wait(&sub, 1, (int)((deadline - now) / 1000000u) + 1);
	}
	hg_sub_close(sub);
	return status;
}

static int cmd_call(int argc, char **argv)
{
	const char *target = NULL;
	long timeout_ms = 1000;
	int json = 0;
	char *kv[128];
	int nkv = 0;
	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
			timeout_ms = atol(argv[++i]);
		else if (strcmp(argv[i], "--json") == 0)
			json = 1;
		else if (!target && argv[i][0] != '-')
			target = argv[i];
		else if (strchr(argv[i], '=') && nkv < 128)
			kv[nkv++] = argv[i];
		else
			return fail("call: unknown argument %s", argv[i]);
	}
	if (!target || !strchr(target, '.') || timeout_ms <= 0)
		return fail("usage: hangar call <module>.<method> [field=value ...] [--timeout MS] [--json]");
	static struct method_info mi;
	if (method_info(target, &mi) < 0)
		return 1;
	static unsigned char body[HG_CALL_MAX_PAYLOAD], rep_body[HG_CALL_MAX_PAYLOAD];
	memset(body, 0, sizeof body);
	for (int i = 0; i < nkv; i++)
		if (set_field(&mi.req.type, body, kv[i]))
			return 1;
	struct hg_call_reply_envelope env;
	int st = do_call(target, &mi, body, mi.req.type.payload_bytes, timeout_ms, &env, rep_body, sizeof rep_body);
	if (st == -100)
		return 1;
	if (json) {
		printf("{\"status\":\"%s\",\"call_id\":%llu,\"reason\":\"%.63s\"", hg_status_name(st),
		       (unsigned long long)env.call_id, env.reason);
		if (st == HG_OK) {
			printf(",\"reply\":{");
			print_fields(&mi.rep.type, rep_body, 0, 1, NULL, 0);
			printf("}");
		}
		printf("}\n");
	} else {
		printf("%s%s%.63s\n", hg_status_name(st), env.reason[0] ? ": " : "", env.reason);
		if (st == HG_OK)
			print_fields(&mi.rep.type, rep_body, 0, 0, NULL, 0);
	}
	return st == HG_OK ? 0 : 1;
}

/* ---- param ---- */

/* The newest <module>.params message and its schema; -1 if the topic is
 * absent (the module has not run). */
static int params_topic(const char *module, struct hg_schema *schema, unsigned char *buf, size_t buflen,
			uint32_t *payload)
{
	char name[64];
	snprintf(name, sizeof name, "%s.params", module);
	topic_dir();
	hg_sub *s = hg_sub_open(name, NULL);
	if (!s)
		return -1;
	struct hg_info info;
	enum hg_result r = hg_latest(s, buf, buflen, &info);
	if (r != HG_MSG) {
		hg_sub_close(s);
		return -1;
	}
	uint32_t slen;
	const char *text = hg_sub_schema(s, &slen);
	char err[128];
	if (!text || hg_schema_parse(text, slen, schema, err, sizeof err) < 0) {
		hg_sub_close(s);
		return -1;
	}
	*payload = hg_sub_payload_bytes(s);
	hg_sub_close(s);
	return 0;
}

/* param-read: the type's schema (attributes included) and the saved file. */
struct saved_params {
	char type_name[64];
	int running;
	struct hg_schema schema;
	char file[HG_SCHEMA_MAX * 4];
	unsigned file_len;
};

static int params_saved(const char *module, struct saved_params *sp)
{
	static char reply[HG_CONTROL_MAX];
	char req[128];
	snprintf(req, sizeof req, "param-read %s", module);
	ssize_t n = request_bin(req, strlen(req), reply, sizeof reply);
	if (n < 0)
		return -1;
	memset(sp, 0, sizeof *sp);
	char *line = strchr(reply, '\n');
	char *end = line ? strchr(line + 1, '\n') : NULL;
	unsigned long long hash;
	unsigned slen = 0;
	if (!line || !end)
		return fail("param-read: malformed reply");
	*end = 0;
	if (sscanf(line + 1, "type=%63s hash=%llx running=%d schema=%u file=%u", sp->type_name, &hash, &sp->running,
		   &slen, &sp->file_len) != 5)
		return fail("param-read: malformed reply line");
	const char *schema = end + 1, *file = schema + slen;
	if (file + sp->file_len > reply + n || sp->file_len >= sizeof sp->file)
		return fail("param-read: short reply");
	char err[128];
	if (hg_schema_parse(schema, slen, &sp->schema, err, sizeof err) < 0)
		return fail("param-read: schema: %s", err);
	sp->schema.type.name = sp->type_name;
	memcpy(sp->file, file, sp->file_len);
	sp->file[sp->file_len] = 0;
	return 0;
}

static int cmd_param_get(const char *module, int argc, char **argv, int json)
{
	static struct hg_schema schema;
	static unsigned char buf[64 + HG_SCHEMA_MAX * 8];
	uint32_t payload;
	/* The topic, while the module runs; a stopped module's topic still
	 * holds its last message, but the saved file may have moved on. */
	static struct saved_params sp;
	if (params_saved(module, &sp) < 0)
		return 1;
	if (sp.running && params_topic(module, &schema, buf, sizeof buf, &payload) == 0) {
		uint64_t version;
		memcpy(&version, buf, 8);
		if (json) {
			printf("{\"version\":%llu,\"changed_by\":\"%.47s\",\"restart_pending\":%s,\"params\":{",
			       (unsigned long long)version, (const char *)buf + 8, buf[56] ? "true" : "false");
			print_fields(&schema.type, buf, 4, 1, (const char *const *)argv, argc);
			printf("}}\n");
		} else {
			printf("# %s.params: version %llu, changed by %.47s%s\n", module, (unsigned long long)version,
			       (const char *)buf + 8, buf[56] ? ", restart pending" : "");
			print_fields(&schema.type, buf, 4, 0, (const char *const *)argv, argc);
		}
		return 0;
	}
	/* Not running: the saved file, or the defaults when it has not run yet. */
	if (!sp.file_len) {
		printf("# %s has no %s.params topic and no saved file: it has not run yet; the defaults apply\n", module,
		       module);
		static struct hg_param_attr attrs[HG_SCHEMA_MAX_FIELDS];
		static unsigned char vals[HG_SCHEMA_MAX * 8];
		char err[160];
		if (hg_params_attrs(&sp.schema.type, attrs, err, sizeof err) < 0 ||
		    hg_params_defaults(&sp.schema.type, attrs, vals, err, sizeof err) < 0)
			return fail("%s", err);
		print_fields(&sp.schema.type, vals, 0, json, (const char *const *)argv, argc);
		return 0;
	}
	printf("# %s has no %s.params topic (not running); the saved file:\n", module, module);
	fputs(sp.file, stdout);
	return 0;
}

/* Read, modify, set (docs/05 "Setting parameters"), retried up to three
 * times when refused as stale; or param-write for a module not running.
 * The changes are field=value pairs, or the lines of a saved file. */
static int param_apply(const char *module, char **kv, int nkv, const char *lines, size_t lines_len, int json)
{
	char target[96];
	snprintf(target, sizeof target, "%s.set_params", module);
	static struct method_info mi;
	if (method_info(target, &mi) < 0)
		return 1;
	if (!mi.running) {
		/* param-write: the manager applies the lines to the saved values. */
		static char req[HG_CONTROL_MAX];
		int n = snprintf(req, sizeof req, "param-write %s\n", module);
		for (int i = 0; i < nkv; i++) {
			const char *eq = strchr(kv[i], '=');
			if (!eq)
				return fail("'%s' is not field=value", kv[i]);
			char name[80];
			snprintf(name, sizeof name, "%.*s", (int)(eq - kv[i]), kv[i]);
			const struct hg_field *f = hg_type_field(&mi.req.type, name);
			if (!f || strcmp(name, "base_version") == 0)
				return fail("%s: no parameter %s", module, name);
			if (f->kind == HG_CHAR && eq[1] != '"')
				n += snprintf(req + n, sizeof req - (size_t)n, "%s = \"%s\"\n", name, eq + 1);
			else
				n += snprintf(req + n, sizeof req - (size_t)n, "%s = %s\n", name, eq + 1);
			if (n >= (int)sizeof req - 1)
				return fail("too many changes for one request");
		}
		if (lines) {
			if ((size_t)n + lines_len >= sizeof req)
				return fail("the file is too large for one request");
			memcpy(req + n, lines, lines_len);
			n += (int)lines_len;
		}
		static char reply[HG_CONTROL_MAX];
		if (request_bin(req, (size_t)n, reply, sizeof reply) < 0)
			return 1;
		unsigned long long v = 0;
		sscanf(reply, "ok version=%llu", &v);
		if (json)
			printf("{\"written\":true,\"version\":%llu}\n", v);
		else
			printf("%s is not running: saved file written, version %llu (applied at its next start)\n", module, v);
		return 0;
	}
	static unsigned char topic[64 + HG_SCHEMA_MAX * 8], body[HG_CALL_MAX_PAYLOAD], rep_body[HG_CALL_MAX_PAYLOAD];
	static struct hg_schema tschema;
	for (int attempt = 1; attempt <= 3; attempt++) {
		uint32_t payload;
		if (params_topic(module, &tschema, topic, sizeof topic, &payload) < 0)
			return fail("no %s.params topic: is %s running?", module, module);
		if (payload < 64 || payload - 64 + 8 > mi.req.type.payload_bytes)
			return fail("%s.params does not match %s", module, mi.req_name);
		/* base_version, then the values in force, then the changes. */
		memset(body, 0, sizeof body);
		memcpy(body, topic, 8);
		memcpy(body + 8, topic + 64, payload - 64);
		for (int i = 0; i < nkv; i++) {
			if (strncmp(kv[i], "base_version=", 13) == 0)
				return fail("base_version is not a parameter");
			if (set_field(&mi.req.type, body, kv[i]))
				return 1;
		}
		if (lines) {
			struct hg_params_header hdr;
			char err[160];
			if (hg_params_file_parse(&mi.req.type, lines, lines_len, body, &hdr, NULL, 0, err, sizeof err) < 0)
				return fail("%s", err);
		}
		struct hg_call_reply_envelope env;
		int st = do_call(target, &mi, body, mi.req.type.payload_bytes, (long)mi.deadline_ms * 10 + 1000, &env,
				 rep_body, sizeof rep_body);
		if (st == -100)
			return 1;
		if (st == HG_E_REFUSED && strncmp(env.reason, "stale", 5) == 0 && attempt < 3) {
			fprintf(stderr, "hangar: %s; retrying\n", env.reason);
			continue;
		}
		uint64_t version = 0;
		memcpy(&version, rep_body, 8);
		if (json)
			printf("{\"status\":\"%s\",\"reason\":\"%.63s\",\"version\":%llu,\"restarting\":%s}\n",
			       hg_status_name(st), env.reason, (unsigned long long)version, rep_body[8] ? "true" : "false");
		else if (st == HG_OK)
			printf("%s: version %llu%s\n", module, (unsigned long long)version,
			       rep_body[8] ? " saved; the module restarts to apply it" : "");
		else
			printf("%s%s%.63s\n", hg_status_name(st), env.reason[0] ? ": " : "", env.reason);
		return st == HG_OK ? 0 : 1;
	}
	return 1;
}

/* The defaults of the named fields (all when none), as field=value text. */
static int param_reset(const char *module, int argc, char **argv, int json)
{
	static struct hg_schema tschema;
	static unsigned char topic[64 + HG_SCHEMA_MAX * 8];
	static struct saved_params sp;
	static struct hg_param_attr attrs[HG_SCHEMA_MAX_FIELDS];
	const struct hg_type *t;
	uint32_t from = 0, payload;
	if (params_topic(module, &tschema, topic, sizeof topic, &payload) == 0) {
		t = &tschema.type;
		from = 4; /* version, changed_by, restart_pending, reserved */
	} else {
		if (params_saved(module, &sp) < 0)
			return 1;
		t = &sp.schema.type;
	}
	char err[160];
	if (hg_params_attrs(t, attrs, err, sizeof err) < 0)
		return fail("%s", err);
	static char text[HG_SCHEMA_MAX * 2];
	static char *kv[HG_SCHEMA_MAX_FIELDS];
	int n = 0, nkv = 0;
	for (uint32_t i = from; i < t->n_fields; i++) {
		const struct hg_field *f = &t->fields[i];
		if (argc) {
			int want = 0;
			for (int k = 0; k < argc; k++)
				if (strcmp(argv[k], f->name) == 0)
					want = 1;
			if (!want)
				continue;
		}
		kv[nkv++] = text + n;
		if (attrs[i].dflt_len)
			n += snprintf(text + n, sizeof text - (size_t)n, "%s=%.*s", f->name, (int)attrs[i].dflt_len,
				      attrs[i].dflt) + 1;
		else
			n += snprintf(text + n, sizeof text - (size_t)n, "%s=%s", f->name, f->kind == HG_CHAR ? "\"\"" : "0") + 1;
	}
	for (int k = 0; k < argc; k++)
		if (!hg_type_field(t, argv[k]))
			return fail("%s: no parameter %s", module, argv[k]);
	return param_apply(module, kv, nkv, NULL, 0, json);
}

static int cmd_param(int argc, char **argv)
{
	int json = 0;
	char *args[128];
	int n = 0;
	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--json") == 0)
			json = 1;
		else if (n < 128)
			args[n++] = argv[i];
	}
	if (n < 2)
		return fail("usage: hangar param get|set|load|reset <module> ...");
	const char *what = args[0], *module = args[1];
	if (strcmp(what, "get") == 0)
		return cmd_param_get(module, n - 2, args + 2, json);
	if (strcmp(what, "set") == 0) {
		if (n < 3)
			return fail("usage: hangar param set <module> field=value ...");
		return param_apply(module, args + 2, n - 2, NULL, 0, json);
	}
	if (strcmp(what, "load") == 0) {
		if (n != 3)
			return fail("usage: hangar param load <module> <file>");
		static char text[HG_SCHEMA_MAX * 4];
		int fd = open(args[2], O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			return fail("cannot read %s: %s", args[2], strerror(errno));
		ssize_t len = read(fd, text, sizeof text - 1);
		close(fd);
		if (len < 0)
			return fail("cannot read %s: %s", args[2], strerror(errno));
		return param_apply(module, NULL, 0, text, (size_t)len, json);
	}
	if (strcmp(what, "reset") == 0)
		return param_reset(module, n - 2, args + 2, json);
	return fail("param: %s is not get, set, load or reset", what);
}

/* ---- main ---------------------------------------------------------------- */

int hangar_dataset_main(int argc, char **argv); /* hangar_dataset.c */

static void usage(void)
{
	fputs("usage: hangar <command> [args]\n"
	      "  status                      the manager's view of every module\n"
	      "  start|stop|restart <module>\n"
	      "  reload                      read the manifests again\n"
	      "  restart-manager             re-execute the manager, modules kept\n"
	      "  clear-failsafe\n"
	      "  show <module>               its wiring and its page\n"
	      "  topics                      every topic, from the topic directory\n"
	      "  echo <topic> [--rate] [--json] [--fields a,b] [--count N]\n"
	      "  hz <topic> [--seconds N]\n"
	      "  run <module>                a module in the foreground, wired from its manifest\n"
	      "  ardusub-setup               set ArduSub's parameters from /etc/hangar/ardusub.params (mavlink.setup)\n"
	      "  check                       the arming checklist, without arming (safety.check)\n"
	      "  call <module>.<method> [field=value ...] [--timeout MS] [--json]\n"
	      "                              a call as admin:<user> through the manager; the reply decoded\n"
	      "  param get <module> [field ...]           the values in force, from <module>.params\n"
	      "  param set <module> field=value ...       read, modify, set_params; retried when stale\n"
	      "  param load <module> <file>               a saved file, set as one change\n"
	      "  param reset <module> [field ...]         back to the defaults\n"
	      "                              (a module that is not running: its saved file, through the manager)\n"
	      "  dataset build|check|schema|pairs ...     datasets from recordings (hangar dataset)\n"
	      "Paths follow $HANGAR_ROOT; $HANGAR_CONTROL and $HANGAR_TOPIC_DIR override.\n",
	      stderr);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage();
		return 2;
	}
	const char *cmd = argv[1];
	if (strcmp(cmd, "status") == 0)
		return cmd_status();
	if (strcmp(cmd, "start") == 0 || strcmp(cmd, "stop") == 0 || strcmp(cmd, "restart") == 0) {
		if (argc < 3)
			return fail("usage: hangar %s <module>", cmd);
		char req[80];
		snprintf(req, sizeof req, "%s %s", cmd, argv[2]);
		return request(req, 1, NULL, 0);
	}
	if (strcmp(cmd, "reload") == 0)
		return request("reload", 1, NULL, 0);
	if (strcmp(cmd, "restart-manager") == 0)
		return request("reexec", 1, NULL, 0);
	if (strcmp(cmd, "clear-failsafe") == 0)
		return request("clear-failsafe", 1, NULL, 0);
	if (strcmp(cmd, "show") == 0)
		return argc < 3 ? fail("usage: hangar show <module>") : cmd_show(argv[2]);
	if (strcmp(cmd, "topics") == 0)
		return cmd_topics();
	if (strcmp(cmd, "echo") == 0)
		return cmd_echo(argc - 2, argv + 2);
	if (strcmp(cmd, "hz") == 0)
		return cmd_hz(argc - 2, argv + 2);
	if (strcmp(cmd, "run") == 0)
		return cmd_run(argc - 2, argv + 2);
	if (strcmp(cmd, "call") == 0)
		return cmd_call(argc - 2, argv + 2);
	if (strcmp(cmd, "ardusub-setup") == 0) {
		/* docs/12: one call, mavlink.setup, as admin:<user>; the module sets
		 * the file's parameters beside its loop and ardusub.state.params says
		 * whether every one reads as the file says (hangar echo ardusub.state). */
		char *args[] = { (char *)"mavlink.setup", (char *)"--timeout", (char *)"3000" };
		int rc = cmd_call(3, args);
		if (rc == 0)
			puts("accepted: the module sets the file's parameters; ardusub.state.params says when they all read as the file (1), or which differs (2, the module's status line)");
		return rc;
	}
	if (strcmp(cmd, "check") == 0) {
		/* docs/12's hangar check, the phase 1 subset: the safety module's
		 * arming checklist, run without arming (safety.check). */
		char *args[] = { (char *)"safety.check", (char *)"--timeout", (char *)"3000" };
		return cmd_call(3, args);
	}
	if (strcmp(cmd, "param") == 0)
		return cmd_param(argc - 2, argv + 2);
	if (strcmp(cmd, "dataset") == 0)
		return hangar_dataset_main(argc - 2, argv + 2); /* hangar_dataset.c (docs/06, docs/12) */
	usage();
	return 2;
}
