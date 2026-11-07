// SPDX-License-Identifier: GPL-3.0-or-later
/* hangar: the command-line tool (docs/12-tools.md), stage 2a subset:
 *   status, start|stop|restart <module>, reload, restart-manager,
 *   clear-failsafe        over the control socket
 *   show <module>         its wiring (from the manager) and its page
 *   topics                every topic file, with its header fields
 *   echo <topic>          messages decoded by the schema in the header
 *   hz <topic>            measured rate and jitter
 *   run <module>          a module in the foreground, wired from its manifest
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

#include "hg_control.h"
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
	unsetenv("HANGAR_WIRING");
	unsetenv("HANGAR_CONTROL");
	for (int i = 0; i < m->n_env; i++)
		putenv(m->env[i]);
	execv(m->exec.argv[0], m->exec.argv);
	return fail("cannot run %s: %s", m->exec.argv[0], strerror(errno));
}

/* ---- main ---------------------------------------------------------------- */

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
	usage();
	return 2;
}
