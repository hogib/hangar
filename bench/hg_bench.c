// SPDX-License-Identifier: GPL-3.0-or-later
/* hg_bench: hangar's side of the benchmark of docs/12-tools.md, measured
 * against the targets of docs/00-overview.md, on the committed lib/hg and
 * nothing else (libc and the kernel). It supersedes spike/c/src/bench.c.
 *
 *   hg_bench <subcommand>... [options] [--json <file>] [--bin <path>]...
 *
 * Subcommands: publish, latest, wake, wake_nopin, idle, rss, sizes, torn,
 * lost_wake, all. Every subcommand prints a human line as it finishes; --json writes
 * one report with the machine, the date and lib/hg's commit (HG_COMMIT,
 * given with -D at build). Options: --runs N (5), --count N (1000000
 * publishes per run), --seconds N (20, the wake runs), --idle-seconds N
 * (30), --torn-seconds N (3), --lost-msgs N (20000), --no-pin (leave the
 * wake pair's placement to the scheduler), --quick (short runs of
 * everything, for a smoke test of the harness itself).
 *
 * The topic directory is $HANGAR_TOPIC_DIR if set, else a fresh
 * /dev/shm/hg-bench-<uid>-<pid>, removed at exit. The library runs in its
 * standalone mode (no manager, no wiring): slots and rates come from
 * hg_pub_opts, as a manifest would give them.
 *
 * Each measurement's method is in the comment above its function, as
 * spike/SPEC.md section 3 gave them. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "hg_topic.h"

#ifndef HG_COMMIT
#define HG_COMMIT "unknown"
#endif

#define MAX_RUNS 101
#define WORDS 32                 /* the 256-byte message: 32 words, each the seq */
#define TORN_READERS 4

/* ---- Options ----------------------------------------------------------- */

static int runs = 5;
static unsigned long long count = 1000000;
static int wake_seconds = 20, idle_seconds = 30, torn_seconds = 3;
static unsigned long long lost_msgs = 20000;
static int no_pin;
static const char *json_path;
static const char *bins[64];
static int n_bins;

/* ---- Small helpers ----------------------------------------------------- */

static void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "hg_bench: ");
	vfprintf(stderr, fmt, ap);
	if (errno)
		fprintf(stderr, ": %s", strerror(errno));
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static double median(double *v, int n)
{
	qsort(v, (size_t)n, sizeof *v, cmp_d);
	return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return (x > y) - (x < y);
}

static void sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

static void *shared_zeroed(size_t len)
{
	void *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (m == MAP_FAILED)
		die("mmap");
	memset(m, 0, len);
	return m;
}

/* Waits for a child; its exit status or -1, and its rusage if wanted. */
static int wait_child(pid_t pid, struct rusage *ru)
{
	int st;
	struct rusage r;
	while (wait4(pid, &st, 0, &r) < 0)
		if (errno != EINTR)
			return -1;
	if (ru)
		*ru = r;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Waits until *ready reaches want, failing if a child exits first. */
static void wait_ready(atomic_int *ready, int want, const pid_t *pids, int n)
{
	uint64_t t0 = hg_now_ns();
	while (atomic_load(ready) < want) {
		for (int i = 0; i < n; i++)
			if (waitpid(pids[i], NULL, WNOHANG) != 0) {
				for (int j = 0; j < n; j++)
					if (j != i)
						kill(pids[j], SIGKILL);
				errno = 0;
				die("a child exited before it was ready");
			}
		if (hg_now_ns() - t0 > 10000000000ull) {
			errno = 0;
			die("children not ready after 10 s");
		}
		sleep_ms(1);
	}
}

static int read_file(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	size_t n = 0;
	for (;;) {
		ssize_t r = read(fd, buf + n, cap - 1 - n);
		if (r <= 0)
			break;
		n += (size_t)r;
		if (n >= cap - 1)
			break;
	}
	close(fd);
	buf[n] = 0;
	return (int)n;
}

/* The value of "<key>:" in a /proc text file (kB fields: the number alone). */
static long long proc_field(const char *text, const char *key)
{
	size_t kl = strlen(key);
	for (const char *p = text; (p = strstr(p, key)); p += kl) {
		if ((p == text || p[-1] == '\n') && p[kl] == ':')
			return strtoll(p + kl + 1, NULL, 10);
	}
	return -1;
}

/* utime + stime of a process in clock ticks, from /proc/<pid>/stat. */
static long long cpu_ticks(pid_t pid)
{
	char path[64], buf[1024];
	snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
	if (read_file(path, buf, sizeof buf) < 0)
		return -1;
	const char *p = strrchr(buf, ')'); /* comm may hold spaces */
	if (!p)
		return -1;
	p++;
	long long f[13];
	int n = 0;
	char *end;
	for (; n < 13; n++) {
		while (*p == ' ')
			p++;
		if (n == 0) { /* state */
			p++;
			f[n] = 0;
			continue;
		}
		f[n] = strtoll(p, &end, 10);
		if (end == p)
			return -1;
		p = end;
	}
	return f[11] + f[12]; /* fields 14 and 15 */
}

static int pin_to_cpu(int cpu)
{
	if (cpu < 0)
		return -1;
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	return sched_setaffinity(0, sizeof set, &set);
}

/* ---- Types and the topic directory ------------------------------------- */

struct size_type {
	uint32_t bytes;
	const char *schema;
	struct hg_schema parsed;
	const struct hg_type *type;
};

/* 16, 256, 4096 and 1 MB, as u64 arrays; every word of message n holds n. */
static struct size_type sizes[] = {
	{ .bytes = 16, .schema = "u64 w[2]\n" },
	{ .bytes = 256, .schema = "u64 w[32]\n" },
	{ .bytes = 4096, .schema = "u64 w[512]\n" },
	{ .bytes = 1048576, .schema = "u64 w[131072]\n" },
};
#define N_SIZES 4
static const struct hg_type *WORDS_TYPE; /* the 256-byte one */

static char topic_dir[256];
static int made_topic_dir;

static void setup_types(void)
{
	for (int i = 0; i < N_SIZES; i++) {
		char err[128];
		if (hg_schema_parse(sizes[i].schema, strlen(sizes[i].schema), &sizes[i].parsed, err,
				    sizeof err) < 0)
			die("schema %s: %s", sizes[i].schema, err);
		sizes[i].type = &sizes[i].parsed.type;
		if (sizes[i].type->payload_bytes != sizes[i].bytes)
			die("payload %u, wanted %u", sizes[i].type->payload_bytes, sizes[i].bytes);
	}
	WORDS_TYPE = sizes[1].type;
}

static void remove_topic_dir(void)
{
	if (!made_topic_dir)
		return;
	DIR *d = opendir(topic_dir);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			char path[512];
			snprintf(path, sizeof path, "%s/%s", topic_dir, e->d_name);
			unlink(path);
		}
		closedir(d);
	}
	rmdir(topic_dir);
}

static void setup_topic_dir(void)
{
	const char *env = getenv("HANGAR_TOPIC_DIR");
	if (env && *env) {
		snprintf(topic_dir, sizeof topic_dir, "%s", env);
		return;
	}
	snprintf(topic_dir, sizeof topic_dir, "/dev/shm/hg-bench-%ld-%ld", (long)getuid(),
		 (long)getpid());
	if (mkdir(topic_dir, 0700) < 0 && errno != EEXIST)
		die("mkdir %s", topic_dir);
	made_topic_dir = 1;
	setenv("HANGAR_TOPIC_DIR", topic_dir, 1);
	atexit(remove_topic_dir);
}

static void fill(uint64_t *buf, uint32_t words, uint64_t seq)
{
	for (uint32_t i = 0; i < words; i++)
		buf[i] = seq;
}

static int intact(const uint64_t *buf, uint32_t words, uint64_t seq)
{
	for (uint32_t i = 0; i < words; i++)
		if (buf[i] != seq)
			return 0;
	return 1;
}

static hg_pub *open_pub(const char *name, const struct hg_type *t, uint32_t slots, uint32_t rate_mhz)
{
	char path[512];
	snprintf(path, sizeof path, "%s/%s", topic_dir, name);
	unlink(path); /* every run starts at seq 0 */
	struct hg_pub_opts o = { .slots = slots, .rate_mhz = rate_mhz };
	hg_pub *p = hg_pub_open(name, t, &o);
	if (!p)
		die("hg_pub_open(%s)", name);
	return p;
}

/* ---- JSON -------------------------------------------------------------- */

static FILE *J;          /* the report, built in memory */
static int j_first = 1;  /* for the commas between top-level sections */

static void j_str(const char *s)
{
	fputc('"', J);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\')
			fprintf(J, "\\%c", c);
		else if (c < 0x20)
			fprintf(J, "\\u%04x", c);
		else
			fputc(c, J);
	}
	fputc('"', J);
}

static void j_section(const char *name)
{
	fprintf(J, "%s\n ", j_first ? "" : ",");
	j_first = 0;
	j_str(name);
	fputs(": ", J);
}

static void j_raw(const double *v, int n, const char *fmt)
{
	fputs("[", J);
	for (int i = 0; i < n; i++) {
		fputs(i ? ", " : "", J);
		fprintf(J, fmt, v[i]);
	}
	fputs("]", J);
}

/* ---- publish ----------------------------------------------------------- */

/* publish: ns per hg_publish with no subscriber open (so no reader can be
 * waiting and the wake flag is never set: the no-syscall path of docs/00's
 * target), for payloads of 16, 256, 4096 bytes and 1 MB, each into a
 * 16-slot ring (4 for 1 MB, as a frame topic). One run is `count`
 * publishes timed as a whole with CLOCK_MONOTONIC (count/256 for 1 MB);
 * the figure is the median of `runs` runs. The payload buffer is constant:
 * what is timed is the library's copy and protocol, not filling the
 * buffer. */
static double run_publish(const struct size_type *t, unsigned long long n)
{
	uint64_t *buf = calloc(1, t->bytes);
	if (!buf)
		die("calloc");
	hg_pub *p = open_pub("bench.publish", t->type, t->bytes > 65536 ? 4 : 16, 0);
	uint64_t t0 = hg_now_ns();
	for (unsigned long long i = 0; i < n; i++)
		hg_publish(p, buf, t->bytes, i);
	uint64_t t1 = hg_now_ns();
	hg_pub_close(p);
	free(buf);
	return (double)(t1 - t0) / (double)n;
}

static void bench_publish(void)
{
	j_section("publish");
	fprintf(J, "{\"runs\": %d, \"sizes\": [", runs);
	for (int i = 0; i < N_SIZES; i++) {
		unsigned long long n = sizes[i].bytes > 65536 ? (count + 255) / 256 : count;
		double v[MAX_RUNS];
		for (int r = 0; r < runs; r++)
			v[r] = run_publish(&sizes[i], n);
		double m = median(v, runs);
		printf("publish   %8u B  %10.1f ns/msg   (median of %d x %llu, no reader)\n",
		       sizes[i].bytes, m, runs, n);
		fprintf(J, "%s\n  {\"bytes\": %u, \"count\": %llu, \"ns\": %.2f, \"raw\": ",
			i ? "," : "", sizes[i].bytes, n, m);
		j_raw(v, runs, "%.2f");
		fputs("}", J);
	}
	fputs("]}", J);
	fflush(stdout);
}

/* ---- latest ------------------------------------------------------------ */

/* latest: ns per hg_latest of the same sizes. One subscriber; a loop of
 * `count` iterations each publishing one message then reading it with
 * hg_latest (so every read returns a message and copies it), timed as a
 * whole, minus a second loop of `count` publishes alone (the publish part):
 * the difference per iteration is the read. Median of `runs` runs; 1 MB
 * uses count/256. The subscriber is open while the publish-only loop runs
 * too, but is not waiting, so the writer takes the same path in both. */
static double run_latest(const struct size_type *t, unsigned long long n, unsigned long long *bad_out)
{
	uint64_t *buf = calloc(1, t->bytes), *in = calloc(1, t->bytes);
	if (!buf || !in)
		die("calloc");
	hg_pub *p = open_pub("bench.latest", t->type, t->bytes > 65536 ? 4 : 16, 0);
	hg_sub *s = hg_sub_open("bench.latest", t->type);
	if (!s)
		die("hg_sub_open");
	struct hg_info info;
	unsigned long long bad = 0;
	uint64_t t0 = hg_now_ns();
	for (unsigned long long i = 0; i < n; i++) {
		hg_publish(p, buf, t->bytes, i);
		bad += hg_latest(s, in, t->bytes, &info) != HG_MSG;
	}
	uint64_t t1 = hg_now_ns();
	for (unsigned long long i = 0; i < n; i++)
		hg_publish(p, buf, t->bytes, i);
	uint64_t t2 = hg_now_ns();
	hg_sub_close(s);
	hg_pub_close(p);
	free(buf);
	free(in);
	*bad_out += bad;
	return ((double)(t1 - t0) - (double)(t2 - t1)) / (double)n;
}

static void bench_latest(void)
{
	j_section("latest");
	fprintf(J, "{\"runs\": %d, \"sizes\": [", runs);
	for (int i = 0; i < N_SIZES; i++) {
		unsigned long long n = sizes[i].bytes > 65536 ? (count + 255) / 256 : count;
		unsigned long long bad = 0;
		double v[MAX_RUNS];
		for (int r = 0; r < runs; r++)
			v[r] = run_latest(&sizes[i], n, &bad);
		double m = median(v, runs);
		printf("latest    %8u B  %10.1f ns/read  (median of %d x %llu%s)\n", sizes[i].bytes,
		       m, runs, n, bad ? ", SOME READS RETURNED NO MESSAGE" : "");
		fprintf(J, "%s\n  {\"bytes\": %u, \"count\": %llu, \"ns\": %.2f, \"no_message\": %llu, \"raw\": ",
			i ? "," : "", sizes[i].bytes, n, m, bad);
		j_raw(v, runs, "%.2f");
		fputs("}", J);
	}
	fputs("]}", J);
	fflush(stdout);
}

/* ---- wake, rss: a writer and a reader at a rate ------------------------ */

/* wake: publish-to-wake-up latency through hg_wait. The writer and the
 * reader are separate processes, pinned to the two highest-numbered online
 * CPUs when there are at least two (else, or with --no-pin, unpinned; the
 * report gives the CPUs or -1). Pinning keeps the pair off the CPU the rest
 * of the system favours and makes the runs repeatable; on a busy desktop it
 * also stops the scheduler moving the pair away from a busy neighbour, so a
 * --no-pin run is worth having beside it. The
 * writer publishes a 256-byte message every period on an absolute
 * clock_nanosleep schedule (CLOCK_MONOTONIC, TIMER_ABSTIME) into a 16-slot
 * ring, the manifest default, and counts the periods it finished late. The
 * reader sleeps in hg_wait, reads every new message with hg_next, and for
 * each records now - publish_ns, where now is one clock read taken right
 * after hg_wait returned and publish_ns is the slot's, written by the
 * library at the start of the publish. So a message found in a batch after
 * the one that woke the reader carries its queueing time too. The run is
 * `seconds` long at 100 Hz, 1 kHz and 10 kHz; p50, p99 and max are over
 * all samples of the run. The reader's wake count (hg_wait returns with
 * news) and both processes' CPU time (rusage) and context switches go
 * beside them; the kernel-side count of futex wake calls is not exposed by
 * the library and is left out. "kept_up" is false when the reader lost
 * messages (gap > 0) or the writer was late on more than 1% of its periods.
 *
 * rss: the same pair at 1 kHz for 3 s, with Rss, Pss (and its anon, file
 * and shmem parts) read from /proc/<pid>/smaps_rollup and VmHWM from
 * /proc/<pid>/status while the pair runs, 2 s in. */

struct pair_shared {
	atomic_int pub_open, ready, go;  /* writer opened the topic; reader is in place; start */
	unsigned long long msgs, gaps, torn, wakes, timeouts, late, published;
	uint32_t cap, n;          /* samples: latency in ns, capped at UINT32_MAX */
	uint32_t lat[];
};

static void pair_reader(struct pair_shared *sh, int cpu)
{
	static uint64_t buf[WORDS];
	pin_to_cpu(cpu);
	hg_sub *s = hg_sub_open("bench.wake", WORDS_TYPE);
	if (!s)
		_exit(1);
	struct hg_info info;
	while (hg_next(s, buf, sizeof buf, &info) == HG_MSG) /* nothing published before ready counts */
		;
	atomic_store(&sh->ready, 1);
	for (int done = 0; !done;) {
		unsigned r = hg_wait(&s, 1, 5000);
		uint64_t now = hg_now_ns();
		if (r == (unsigned)-1)
			_exit(2);
		if (r == 0) {
			sh->timeouts++;
			continue;
		}
		sh->wakes++;
		for (;;) {
			enum hg_result res = hg_next(s, buf, sizeof buf, &info);
			if (res == HG_CLOSED) {
				done = 1;
				break;
			}
			if (res != HG_MSG)
				break;
			sh->msgs++;
			sh->gaps += info.gap;
			sh->torn += !intact(buf, WORDS, info.seq);
			if (sh->n < sh->cap) {
				uint64_t d = now > info.publish_ns ? now - info.publish_ns : 0;
				sh->lat[sh->n++] = d > UINT32_MAX ? UINT32_MAX : (uint32_t)d;
			}
		}
	}
	hg_sub_close(s);
	_exit(0);
}

static void pair_writer(struct pair_shared *sh, int cpu, unsigned rate_hz, int seconds)
{
	static uint64_t buf[WORDS];
	pin_to_cpu(cpu);
	prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0); /* a 10 kHz module would; the default slack is 50 us */
	hg_pub *p = open_pub("bench.wake", WORDS_TYPE, 16, rate_hz * 1000000u);
	atomic_store(&sh->pub_open, 1);
	while (!atomic_load(&sh->go))
		sleep_ms(1);
	unsigned long long n_msgs = (unsigned long long)rate_hz * (unsigned long long)seconds;
	uint64_t period = 1000000000ull / rate_hz;
	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);
	for (unsigned long long i = 0; i < n_msgs; i++) {
		next.tv_nsec += (long)period;
		while (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR)
			;
		uint64_t now = hg_now_ns();
		uint64_t due = (uint64_t)next.tv_sec * 1000000000ull + (uint64_t)next.tv_nsec;
		if (now > due + period)
			sh->late++;
		uint64_t seq = hg_pub_next_seq(p);
		fill(buf, WORDS, seq);
		hg_publish(p, buf, sizeof buf, now);
		sh->published = seq;
	}
	hg_pub_close(p); /* wakes the reader with HG_CLOSED */
	_exit(0);
}

struct mem_sample {
	long long rss, pss, pss_anon, pss_file, pss_shmem, hwm;
};

static void sample_mem(pid_t pid, struct mem_sample *m)
{
	char path[64], buf[8192];
	snprintf(path, sizeof path, "/proc/%d/smaps_rollup", (int)pid);
	memset(m, -1, sizeof *m);
	if (read_file(path, buf, sizeof buf) > 0) {
		m->rss = proc_field(buf, "Rss");
		m->pss = proc_field(buf, "Pss");
		m->pss_anon = proc_field(buf, "Pss_Anon");
		m->pss_file = proc_field(buf, "Pss_File");
		m->pss_shmem = proc_field(buf, "Pss_Shmem");
	}
	snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
	if (read_file(path, buf, sizeof buf) > 0)
		m->hwm = proc_field(buf, "VmHWM");
}

static void j_mem(const char *who, const struct mem_sample *m)
{
	fprintf(J, "\"%s\": {\"rss_kb\": %lld, \"pss_kb\": %lld, \"pss_anon_kb\": %lld, "
		   "\"pss_file_kb\": %lld, \"pss_shmem_kb\": %lld, \"hwm_kb\": %lld}",
		who, m->rss, m->pss, m->pss_anon, m->pss_file, m->pss_shmem, m->hwm);
}

static double ru_seconds(const struct rusage *ru)
{
	return (double)ru->ru_utime.tv_sec + (double)ru->ru_utime.tv_usec / 1e6 +
	       (double)ru->ru_stime.tv_sec + (double)ru->ru_stime.tv_usec / 1e6;
}

/* The two CPUs the pair is pinned to, or -1, -1. */
static void pick_cpus(int *writer_cpu, int *reader_cpu)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	cpu_set_t set;
	CPU_ZERO(&set);
	if (no_pin || n < 2 || sched_getaffinity(0, sizeof set, &set) < 0 || CPU_COUNT(&set) < 2) {
		*writer_cpu = *reader_cpu = -1;
		return;
	}
	int picked = 0;
	*writer_cpu = *reader_cpu = -1;
	for (int c = CPU_SETSIZE - 1; c >= 0 && picked < 2; c--)
		if (CPU_ISSET(c, &set)) {
			if (picked == 0)
				*reader_cpu = c;
			else
				*writer_cpu = c;
			picked++;
		}
}

/* Runs a pair for `seconds` at rate_hz; if mem is given, samples both
 * processes 2 s in. Writes the JSON object for the run. */
static int run_pair(unsigned rate_hz, int seconds, struct mem_sample *mem /* [2] or NULL */)
{
	int wcpu, rcpu;
	pick_cpus(&wcpu, &rcpu);
	uint32_t cap = (uint32_t)((unsigned long long)rate_hz * (unsigned long long)seconds + 1024);
	struct pair_shared *sh = shared_zeroed(sizeof *sh + (size_t)cap * sizeof(uint32_t));
	sh->cap = cap;
	/* The writer opens the topic first, so the reader finds it (no "appeared"
	 * wake-up), and publishes only once the reader is asleep in hg_wait. */
	pid_t writer = fork();
	if (writer < 0)
		die("fork");
	if (writer == 0)
		pair_writer(sh, wcpu, rate_hz, seconds);
	wait_ready(&sh->pub_open, 1, &writer, 1);
	pid_t reader = fork();
	if (reader < 0)
		die("fork");
	if (reader == 0)
		pair_reader(sh, rcpu);
	wait_ready(&sh->ready, 1, &reader, 1);
	sleep_ms(20);
	atomic_store(&sh->go, 1);
	if (mem) {
		sleep_ms(2000);
		sample_mem(writer, &mem[0]);
		sample_mem(reader, &mem[1]);
	}
	struct rusage wru, rru;
	int wst = wait_child(writer, &wru);
	int rst = wait_child(reader, &rru);
	if (wst != 0 || rst != 0) {
		errno = 0;
		die("pair at %u Hz: writer exit %d, reader exit %d", rate_hz, wst, rst);
	}
	uint32_t n = sh->n;
	qsort(sh->lat, n, sizeof sh->lat[0], cmp_u32);
	double p50 = n ? sh->lat[n / 2] / 1000.0 : 0;
	double p99 = n ? sh->lat[(size_t)(n * 0.99) < n ? (size_t)(n * 0.99) : n - 1] / 1000.0 : 0;
	double max = n ? sh->lat[n - 1] / 1000.0 : 0;
	double sum = 0;
	for (uint32_t i = 0; i < n; i++)
		sum += sh->lat[i];
	double mean = n ? sum / n / 1000.0 : 0;
	unsigned long long expected = (unsigned long long)rate_hz * (unsigned long long)seconds;
	int kept_up = sh->gaps == 0 && sh->late * 100 <= expected;
	double wcpu_s = ru_seconds(&wru), rcpu_s = ru_seconds(&rru);
	printf("wake  %6u Hz  p50 %7.1f  p99 %7.1f  max %8.1f us   msgs %llu wakes %llu gaps %llu "
	       "late %llu  cpu w %.1f%% r %.1f%%%s\n",
	       rate_hz, p50, p99, max, sh->msgs, sh->wakes, sh->gaps, sh->late,
	       100 * wcpu_s / seconds, 100 * rcpu_s / seconds, kept_up ? "" : "  (did not keep up)");
	fprintf(J, "{\"rate_hz\": %u, \"seconds\": %d, \"payload\": 256, \"slots\": 16, "
		   "\"writer_cpu\": %d, \"reader_cpu\": %d, \"expected\": %llu, \"published\": %llu, "
		   "\"msgs\": %llu, \"wakes\": %llu, \"timeouts\": %llu, \"gaps\": %llu, \"torn\": %llu, "
		   "\"writer_late\": %llu, \"kept_up\": %s, \"samples\": %u, \"p50_us\": %.1f, "
		   "\"p99_us\": %.1f, \"max_us\": %.1f, \"mean_us\": %.1f, "
		   "\"writer_cpu_s\": %.3f, \"reader_cpu_s\": %.3f, "
		   "\"writer_cpu_pct\": %.2f, \"reader_cpu_pct\": %.2f, "
		   "\"writer_ctxsw\": [%ld, %ld], \"reader_ctxsw\": [%ld, %ld]",
		rate_hz, seconds, wcpu, rcpu, expected, sh->published, sh->msgs, sh->wakes,
		sh->timeouts, sh->gaps, sh->torn, sh->late, kept_up ? "true" : "false", n, p50, p99,
		max, mean, wcpu_s, rcpu_s, 100 * wcpu_s / seconds, 100 * rcpu_s / seconds,
		wru.ru_nvcsw, wru.ru_nivcsw, rru.ru_nvcsw, rru.ru_nivcsw);
	if (mem) {
		fputs(", ", J);
		j_mem("writer", &mem[0]);
		fputs(", ", J);
		j_mem("reader", &mem[1]);
	}
	fputs("}", J);
	fflush(stdout);
	munmap(sh, sizeof *sh + (size_t)cap * sizeof(uint32_t));
	return kept_up;
}

static void bench_wake(void)
{
	static const unsigned rates[] = { 100, 1000, 10000 };
	j_section("wake");
	fputs("[", J);
	for (int i = 0; i < 3; i++) {
		fputs(i ? ",\n  " : "\n  ", J);
		run_pair(rates[i], wake_seconds, NULL);
	}
	fputs("]", J);
}

/* wake_nopin: the 1 kHz wake run again with the pair left to the scheduler,
 * beside the pinned one: on a machine with other load the two differ, and
 * which is the honest number depends on what else the vehicle runs. */
static void bench_wake_nopin(void)
{
	int was = no_pin;
	no_pin = 1;
	j_section("wake_nopin");
	run_pair(1000, wake_seconds, NULL);
	no_pin = was;
}

static void bench_rss(void)
{
	struct mem_sample mem[2];
	j_section("rss");
	run_pair(1000, 3, mem);
	printf("rss   1 kHz pair: writer Pss %lld kB (Rss %lld), reader Pss %lld kB (Rss %lld)\n",
	       mem[0].pss, mem[0].rss, mem[1].pss, mem[1].rss);
}

/* ---- idle -------------------------------------------------------------- */

/* idle: a reader asleep in hg_wait (no timeout) on an open topic that
 * publishes nothing, for `idle_seconds`. The parent reads the reader's
 * voluntary and involuntary context switches from /proc/<pid>/status and
 * its CPU ticks (utime + stime of /proc/<pid>/stat) 500 ms after the
 * reader said it was ready, and again at the end. The deltas must be 0: a
 * sleeping module costs nothing. Then the writer closes the topic, which
 * wakes the reader once (a flag change is news), and it exits. */
static void bench_idle(void)
{
	struct pair_shared *sh = shared_zeroed(sizeof *sh);
	hg_pub *p = open_pub("bench.idle", WORDS_TYPE, 16, 0);
	pid_t pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		static uint64_t buf[WORDS];
		hg_sub *s = hg_sub_open("bench.idle", WORDS_TYPE);
		if (!s)
			_exit(1);
		atomic_store(&sh->ready, 1);
		for (;;) {
			unsigned r = hg_wait(&s, 1, -1);
			if (r == (unsigned)-1)
				_exit(2);
			sh->wakes++;
			struct hg_info info;
			if (hg_next(s, buf, sizeof buf, &info) == HG_CLOSED)
				break;
		}
		_exit(0);
	}
	wait_ready(&sh->ready, 1, &pid, 1);
	sleep_ms(500);
	char path[64], buf[4096];
	snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
	read_file(path, buf, sizeof buf);
	long long vol0 = proc_field(buf, "voluntary_ctxt_switches");
	long long nonvol0 = proc_field(buf, "nonvoluntary_ctxt_switches");
	long long rss0 = proc_field(buf, "VmRSS");
	long long ticks0 = cpu_ticks(pid);
	sleep_ms(idle_seconds * 1000L);
	read_file(path, buf, sizeof buf);
	long long vol1 = proc_field(buf, "voluntary_ctxt_switches");
	long long nonvol1 = proc_field(buf, "nonvoluntary_ctxt_switches");
	long long rss1 = proc_field(buf, "VmRSS");
	long long ticks1 = cpu_ticks(pid);
	hg_pub_close(p);
	int st = wait_child(pid, NULL);
	int ok = vol1 == vol0 && nonvol1 == nonvol0 && ticks1 == ticks0 && st == 0;
	printf("idle  %d s asleep in hg_wait: ctxsw +%lld/+%lld, cpu ticks +%lld, rss %lld -> %lld kB%s\n",
	       idle_seconds, vol1 - vol0, nonvol1 - nonvol0, ticks1 - ticks0, rss0, rss1,
	       ok ? "" : "  FAIL");
	j_section("idle");
	fprintf(J, "{\"seconds\": %d, \"vol_t0\": %lld, \"vol_t1\": %lld, \"nonvol_t0\": %lld, "
		   "\"nonvol_t1\": %lld, \"cpu_ticks_t0\": %lld, \"cpu_ticks_t1\": %lld, "
		   "\"rss_kb_t0\": %lld, \"rss_kb_t1\": %lld, \"vol_delta\": %lld, "
		   "\"nonvol_delta\": %lld, \"cpu_ticks_delta\": %lld, \"wakes_total\": %llu, \"ok\": %s}",
		idle_seconds, vol0, vol1, nonvol0, nonvol1, ticks0, ticks1, rss0, rss1, vol1 - vol0,
		nonvol1 - nonvol0, ticks1 - ticks0, sh->wakes, ok ? "true" : "false");
	fflush(stdout);
	munmap(sh, sizeof *sh);
}

/* ---- sizes ------------------------------------------------------------- */

/* sizes: st_size of each binary named with --bin (or after `sizes`), with
 * two facts read from the ELF itself: static (no PT_INTERP program header)
 * and stripped (no SHT_SYMTAB section). 32- and 64-bit ELF, little-endian. */
struct elf_facts {
	long long bytes;
	int is_elf, is_static, stripped;
};

static uint64_t le(const unsigned char *p, int n)
{
	uint64_t v = 0;
	for (int i = n - 1; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

static int elf_facts(const char *path, struct elf_facts *f)
{
	memset(f, 0, sizeof *f);
	struct stat st;
	if (stat(path, &st) < 0)
		return -1;
	f->bytes = (long long)st.st_size;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	unsigned char eh[64];
	if (read(fd, eh, 64) != 64 || memcmp(eh, "\177ELF", 4) != 0 || eh[5] != 1) {
		close(fd);
		return 0;
	}
	f->is_elf = 1;
	int c64 = eh[4] == 2;
	uint64_t phoff = c64 ? le(eh + 32, 8) : le(eh + 28, 4);
	uint64_t shoff = c64 ? le(eh + 40, 8) : le(eh + 32, 4);
	unsigned phentsize = (unsigned)le(eh + (c64 ? 54 : 42), 2);
	unsigned phnum = (unsigned)le(eh + (c64 ? 56 : 44), 2);
	unsigned shentsize = (unsigned)le(eh + (c64 ? 58 : 46), 2);
	unsigned shnum = (unsigned)le(eh + (c64 ? 60 : 48), 2);
	f->is_static = 1;
	f->stripped = 1;
	unsigned char hdr[128];
	for (unsigned i = 0; i < phnum && phentsize >= 8 && phentsize <= sizeof hdr; i++) {
		if (pread(fd, hdr, phentsize, (off_t)(phoff + (uint64_t)i * phentsize)) != (ssize_t)phentsize)
			break;
		if (le(hdr, 4) == 3) /* PT_INTERP */
			f->is_static = 0;
	}
	for (unsigned i = 0; i < shnum && shentsize >= 8 && shentsize <= sizeof hdr; i++) {
		if (pread(fd, hdr, shentsize, (off_t)(shoff + (uint64_t)i * shentsize)) != (ssize_t)shentsize)
			break;
		if (le(hdr + 4, 4) == 2) /* SHT_SYMTAB */
			f->stripped = 0;
	}
	close(fd);
	return 0;
}

static void bench_sizes(void)
{
	j_section("sizes");
	fputs("[", J);
	for (int i = 0; i < n_bins; i++) {
		struct elf_facts f;
		fputs(i ? ",\n  " : "\n  ", J);
		if (elf_facts(bins[i], &f) < 0) {
			printf("sizes     %-40s  cannot stat: %s\n", bins[i], strerror(errno));
			fputs("{\"path\": ", J);
			j_str(bins[i]);
			fputs(", \"error\": true}", J);
			continue;
		}
		printf("sizes     %-40s %9lld bytes  %s%s\n", bins[i], f.bytes,
		       !f.is_elf ? "not ELF" : f.is_static ? "static" : "DYNAMIC",
		       !f.is_elf ? "" : f.stripped ? ", stripped" : ", NOT STRIPPED");
		fputs("{\"path\": ", J);
		j_str(bins[i]);
		fprintf(J, ", \"bytes\": %lld, \"elf\": %s, \"static\": %s, \"stripped\": %s}", f.bytes,
			f.is_elf ? "true" : "false", f.is_static ? "true" : "false",
			f.stripped ? "true" : "false");
	}
	fputs("]", J);
	fflush(stdout);
}

/* ---- torn -------------------------------------------------------------- */

/* torn (the spike's test 4, shortened): one writer publishing patterned
 * 256-byte messages flat out into a 4-slot ring for `torn_seconds`, four
 * reader processes alternating hg_latest and hg_next and checking every
 * word against the seq. Reports messages read, torn copies (must be 0),
 * HG_BUSY results and gaps. A correctness smoke test, not a timing. */
struct torn_shared {
	atomic_int ready, stop;
	unsigned long long read[TORN_READERS], torn[TORN_READERS], busy[TORN_READERS],
		gaps[TORN_READERS];
};

static void torn_reader(struct torn_shared *sh, int id)
{
	static uint64_t buf[WORDS];
	hg_sub *s = hg_sub_open("bench.torn", WORDS_TYPE);
	if (!s)
		_exit(1);
	atomic_fetch_add(&sh->ready, 1);
	for (unsigned long i = 0; !atomic_load_explicit(&sh->stop, memory_order_relaxed); i++) {
		struct hg_info info;
		enum hg_result r = (i & 1) ? hg_next(s, buf, sizeof buf, &info)
					   : hg_latest(s, buf, sizeof buf, &info);
		if (r == HG_MSG) {
			sh->read[id]++;
			sh->torn[id] += !intact(buf, WORDS, info.seq);
			sh->gaps[id] += info.gap;
		} else if (r == HG_BUSY) {
			sh->busy[id]++;
		} else if (r == HG_ERROR) {
			_exit(3);
		}
	}
	hg_sub_close(s);
	_exit(0);
}

static void bench_torn(void)
{
	static uint64_t buf[WORDS];
	struct torn_shared *sh = shared_zeroed(sizeof *sh);
	hg_pub *p = open_pub("bench.torn", WORDS_TYPE, 4, 0);
	pid_t pids[TORN_READERS];
	for (int i = 0; i < TORN_READERS; i++) {
		if ((pids[i] = fork()) < 0)
			die("fork");
		if (pids[i] == 0)
			torn_reader(sh, i);
	}
	wait_ready(&sh->ready, TORN_READERS, pids, TORN_READERS);
	uint64_t end = hg_now_ns() + (uint64_t)torn_seconds * 1000000000ull, n = 0;
	do {
		for (int k = 0; k < 1024; k++) {
			n = hg_pub_next_seq(p);
			fill(buf, WORDS, n);
			hg_publish(p, buf, sizeof buf, n);
		}
	} while (hg_now_ns() < end);
	atomic_store(&sh->stop, 1);
	int bad_exit = 0;
	for (int i = 0; i < TORN_READERS; i++)
		bad_exit += wait_child(pids[i], NULL) != 0;
	hg_pub_close(p);
	unsigned long long read = 0, torn = 0, busy = 0, gaps = 0;
	for (int i = 0; i < TORN_READERS; i++) {
		read += sh->read[i];
		torn += sh->torn[i];
		busy += sh->busy[i];
		gaps += sh->gaps[i];
	}
	printf("torn  %d s, %d readers, 4 slots: published %llu, read %llu, torn %llu, busy %llu%s\n",
	       torn_seconds, TORN_READERS, (unsigned long long)n, read, torn, busy,
	       torn || bad_exit ? "  FAIL" : "");
	j_section("torn");
	fprintf(J, "{\"seconds\": %d, \"readers\": %d, \"slots\": 4, \"published\": %llu, "
		   "\"msgs_read\": %llu, \"torn\": %llu, \"busy\": %llu, \"gaps\": %llu, \"ok\": %s}",
		torn_seconds, TORN_READERS, (unsigned long long)n, read, torn, busy, gaps,
		torn || bad_exit ? "false" : "true");
	fflush(stdout);
	munmap(sh, sizeof *sh);
}

/* ---- lost_wake --------------------------------------------------------- */

/* lost_wake (the spike's test 5, shortened): a writer publishing
 * `lost_msgs` messages at random gaps of 20 to 200 us into a 1024-slot
 * ring (so a gap would mean a reader never woken, not one late); a reader
 * in hg_wait(5 s) + hg_next. Reports messages received (must equal sent),
 * gaps, waits that ended by timeout (must be 0), torn copies (0) and
 * wake-ups. */
struct lost_shared {
	atomic_int ready;
	unsigned long long msgs, gaps, timeouts, torn, wakes;
};

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint64_t rng(void)
{
	uint64_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return rng_state = x;
}

static void lost_reader(struct lost_shared *sh)
{
	static uint64_t buf[WORDS];
	hg_sub *s = hg_sub_open("bench.lost_wake", WORDS_TYPE);
	if (!s)
		_exit(1);
	atomic_store(&sh->ready, 1);
	for (int done = 0; !done;) {
		unsigned r = hg_wait(&s, 1, 5000);
		if (r == (unsigned)-1)
			_exit(2);
		if (r == 0)
			sh->timeouts++;
		else
			sh->wakes++;
		for (;;) {
			struct hg_info info;
			enum hg_result res = hg_next(s, buf, sizeof buf, &info);
			sh->gaps += info.gap;
			if (res == HG_CLOSED)
				done = 1;
			if (res != HG_MSG)
				break;
			sh->msgs++;
			sh->torn += !intact(buf, WORDS, info.seq);
		}
	}
	hg_sub_close(s);
	_exit(0);
}

static void bench_lost_wake(void)
{
	static uint64_t buf[WORDS];
	struct lost_shared *sh = shared_zeroed(sizeof *sh);
	hg_pub *p = open_pub("bench.lost_wake", WORDS_TYPE, 1024, 0);
	pid_t pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0)
		lost_reader(sh);
	wait_ready(&sh->ready, 1, &pid, 1);
	for (unsigned long long i = 0; i < lost_msgs; i++) {
		uint64_t n = hg_pub_next_seq(p);
		fill(buf, WORDS, n);
		hg_publish(p, buf, sizeof buf, n);
		struct timespec ts = { 0, 20000 + (long)(rng() % 180001) };
		while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
			;
	}
	hg_pub_close(p);
	int st = wait_child(pid, NULL);
	int ok = st == 0 && sh->msgs == lost_msgs && sh->gaps == 0 && sh->timeouts == 0 && sh->torn == 0;
	printf("lost_wake sent %llu, got %llu, gaps %llu, timeouts %llu, torn %llu, wake-ups %llu%s\n",
	       lost_msgs, sh->msgs, sh->gaps, sh->timeouts, sh->torn, sh->wakes, ok ? "" : "  FAIL");
	j_section("lost_wake");
	fprintf(J, "{\"sent\": %llu, \"msgs\": %llu, \"gaps\": %llu, \"timeouts\": %llu, "
		   "\"torn\": %llu, \"wakes\": %llu, \"ok\": %s}",
		lost_msgs, sh->msgs, sh->gaps, sh->timeouts, sh->torn, sh->wakes, ok ? "true" : "false");
	fflush(stdout);
	munmap(sh, sizeof *sh);
}

/* ---- machine ----------------------------------------------------------- */

static void trim(char *s)
{
	size_t n = strlen(s);
	while (n && (s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == 0))
		s[--n] = 0;
}

/* The first line "<key>\t*: value" of /proc/cpuinfo. */
static int cpuinfo_line(const char *text, const char *key, char *out, size_t cap)
{
	size_t kl = strlen(key);
	for (const char *p = text; (p = strstr(p, key)); p += kl) {
		if (p != text && p[-1] != '\n')
			continue;
		const char *c = strchr(p, ':');
		const char *nl = strchr(p, '\n');
		if (!c || (nl && c > nl))
			continue;
		c++;
		while (*c == ' ' || *c == '\t')
			c++;
		size_t n = nl ? (size_t)(nl - c) : strlen(c);
		if (n >= cap)
			n = cap - 1;
		memcpy(out, c, n);
		out[n] = 0;
		return 0;
	}
	return -1;
}

static void write_machine(int argc, char **argv)
{
	struct utsname u;
	uname(&u);
	static char cpuinfo[65536], cpu[256], model[256], governor[64], date[64];
	read_file("/proc/cpuinfo", cpuinfo, sizeof cpuinfo);
	if (cpuinfo_line(cpuinfo, "model name", cpu, sizeof cpu) < 0 &&
	    cpuinfo_line(cpuinfo, "Model", cpu, sizeof cpu) < 0 &&
	    cpuinfo_line(cpuinfo, "Hardware", cpu, sizeof cpu) < 0) {
		/* AArch64 gives implementer and part only. */
		char imp[32] = "", part[32] = "";
		cpuinfo_line(cpuinfo, "CPU implementer", imp, sizeof imp);
		cpuinfo_line(cpuinfo, "CPU part", part, sizeof part);
		snprintf(cpu, sizeof cpu, "implementer %s part %s", imp, part);
	}
	if (read_file("/proc/device-tree/model", model, sizeof model) <= 0)
		model[0] = 0;
	trim(model);
	if (read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", governor,
		      sizeof governor) <= 0)
		snprintf(governor, sizeof governor, "none");
	trim(governor);
	time_t now = time(NULL);
	strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

	fputs("{\n \"hg_bench\": {\"hg_commit\": ", J);
	j_str(HG_COMMIT);
	fputs(", \"compiler\": ", J);
	j_str(__VERSION__);
	fputs(", \"args\": \"", J);
	for (int i = 1; i < argc; i++)
		fprintf(J, "%s%s", i > 1 ? " " : "", argv[i]);
	fputs("\"}", J);
	j_first = 0;
	fputs(",\n \"date\": ", J);
	j_str(date);
	fputs(",\n \"machine\": {\"arch\": ", J);
	j_str(u.machine);
	fputs(", \"kernel\": ", J);
	j_str(u.release);
	fputs(", \"cpu\": ", J);
	j_str(cpu);
	fputs(", \"model\": ", J);
	j_str(model);
	fprintf(J, ", \"cpus\": %ld, \"governor\": ", sysconf(_SC_NPROCESSORS_ONLN));
	j_str(governor);
	fputs(", \"topic_dir\": ", J);
	j_str(topic_dir);
	fputs("}", J);
	printf("machine   %s %s, %ld cpus, %s%s%s, governor %s, lib/hg %s\n", u.machine, u.release,
	       sysconf(_SC_NPROCESSORS_ONLN), cpu, model[0] ? ", " : "", model, governor, HG_COMMIT);
}

/* ---- main -------------------------------------------------------------- */

static int usage(void)
{
	fprintf(stderr,
		"usage: hg_bench {publish|latest|wake|wake_nopin|idle|rss|sizes|torn|lost_wake|all}...\n"
		"       [--json <file>] [--bin <path>]... [--runs N] [--count N] [--seconds N]\n"
		"       [--idle-seconds N] [--torn-seconds N] [--lost-msgs N] [--no-pin] [--quick]\n");
	return 2;
}

int main(int argc, char **argv)
{
	enum { T_PUB = 1, T_LATEST = 2, T_WAKE = 4, T_IDLE = 8, T_RSS = 16, T_SIZES = 32, T_TORN = 64,
	       T_LOST = 128, T_NOPIN = 256, T_ALL = 511 };
	static const char *const names[] = { "publish", "latest", "wake", "idle", "rss", "sizes",
					     "torn", "lost_wake", "wake_nopin" };
	int tests = 0, sizes_mode = 0;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--json") || !strcmp(a, "--bin") || !strcmp(a, "--runs") ||
		    !strcmp(a, "--count") || !strcmp(a, "--seconds") || !strcmp(a, "--idle-seconds") ||
		    !strcmp(a, "--torn-seconds") || !strcmp(a, "--lost-msgs")) {
			if (i + 1 >= argc)
				return usage();
			const char *v = argv[++i];
			char *end;
			unsigned long long n = strtoull(v, &end, 10);
			if (!strcmp(a, "--json")) {
				json_path = v;
				continue;
			}
			if (!strcmp(a, "--bin")) {
				if (n_bins < 64)
					bins[n_bins++] = v;
				continue;
			}
			if (*end || n == 0)
				return usage();
			if (!strcmp(a, "--runs") && n <= MAX_RUNS)
				runs = (int)n;
			else if (!strcmp(a, "--count"))
				count = n;
			else if (!strcmp(a, "--seconds"))
				wake_seconds = (int)n;
			else if (!strcmp(a, "--idle-seconds"))
				idle_seconds = (int)n;
			else if (!strcmp(a, "--torn-seconds"))
				torn_seconds = (int)n;
			else if (!strcmp(a, "--lost-msgs"))
				lost_msgs = n;
			else
				return usage();
			continue;
		}
		if (!strcmp(a, "--no-pin")) {
			no_pin = 1;
			continue;
		}
		if (!strcmp(a, "--quick")) {
			runs = 3;
			count = 200000;
			wake_seconds = 3;
			idle_seconds = 3;
			torn_seconds = 1;
			lost_msgs = 3000;
			continue;
		}
		if (!strcmp(a, "all")) {
			tests |= T_ALL;
			continue;
		}
		int t = 0;
		for (int k = 0; k < 9; k++)
			if (!strcmp(a, names[k]))
				t = 1 << k;
		if (t) {
			tests |= t;
			sizes_mode = t == T_SIZES;
			continue;
		}
		if (sizes_mode && a[0] != '-') { /* `sizes a b c` */
			if (n_bins < 64)
				bins[n_bins++] = a;
			continue;
		}
		return usage();
	}
	if (!tests)
		return usage();
	if ((tests & T_SIZES) && n_bins == 0 && tests != T_SIZES)
		tests &= ~T_SIZES; /* `all` without --bin: nothing to size */
	if ((tests & T_SIZES) && n_bins == 0) {
		fprintf(stderr, "hg_bench: sizes needs binaries (--bin <path> or after `sizes`)\n");
		return 2;
	}

	char *jbuf = NULL;
	size_t jlen = 0;
	J = open_memstream(&jbuf, &jlen);
	if (!J)
		die("open_memstream");
	setup_topic_dir();
	setup_types();
	rng_state ^= hg_now_ns();
	write_machine(argc, argv);

	if (tests & T_PUB)
		bench_publish();
	if (tests & T_LATEST)
		bench_latest();
	if (tests & T_WAKE)
		bench_wake();
	if (tests & T_NOPIN)
		bench_wake_nopin();
	if (tests & T_IDLE)
		bench_idle();
	if (tests & T_RSS)
		bench_rss();
	if (tests & T_SIZES)
		bench_sizes();
	if (tests & T_TORN)
		bench_torn();
	if (tests & T_LOST)
		bench_lost_wake();

	fputs("\n}\n", J);
	fclose(J);
	if (json_path) {
		FILE *f = fopen(json_path, "w");
		if (!f)
			die("%s", json_path);
		fwrite(jbuf, 1, jlen, f);
		if (fclose(f) != 0)
			die("%s", json_path);
		printf("report    %s\n", json_path);
	}
	free(jbuf);
	return 0;
}
