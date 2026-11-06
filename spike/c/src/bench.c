/* bench [publish|publish16|latest|torn|lost_wake ...] [--runs 5]
 *       [--count 5000000] [--torn-seconds 10] [--lost-msgs 100000]
 * Prints one JSON object; each figure is the median of --runs runs (the
 * must-be-zero counts are summed over the runs instead). */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "hg_topic.h"

#define MAX_RUNS 101
#define TORN_READERS 4
#define PAYLOAD 256

static unsigned long long count = 5000000, torn_seconds = 10, lost_msgs = 100000;
static int runs = 5;

static void fail(const char *what)
{
	fprintf(stderr, "bench: %s: %s\n", what, strerror(errno));
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

/* Removes a previous run's file so that every run starts from seq 0. */
static hg_pub *fresh_pub(const char *name, uint32_t payload, uint32_t slots)
{
	char path[4096];
	snprintf(path, sizeof path, "%s/%s", getenv("HANGAR_TOPIC_DIR"), name);
	unlink(path);
	hg_pub *p = hg_pub_open(name, payload, slots);
	if (!p)
		fail(name);
	return p;
}

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint64_t rng(void)
{
	uint64_t x = rng_state;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	rng_state = x;
	return x * 0x2545f4914f6cdd1dull;
}

/* Every 8-byte word of message n holds n. */
static void fill(uint64_t *buf, uint64_t n)
{
	for (int i = 0; i < PAYLOAD / 8; i++)
		buf[i] = n;
}

static int intact(const uint64_t *buf, const struct hg_info *info)
{
	if (info->stamp_ns != info->seq)
		return 0;
	for (int i = 0; i < PAYLOAD / 8; i++)
		if (buf[i] != info->seq)
			return 0;
	return 1;
}

/* Waits until *ready reaches want; if a child exits first, kills the others
 * and fails. */
static void wait_ready(atomic_int *ready, int want, const pid_t *pids, int n)
{
	while (atomic_load(ready) < want) {
		for (int i = 0; i < n; i++) {
			if (waitpid(pids[i], NULL, WNOHANG) == 0)
				continue;
			for (int j = 0; j < n; j++)
				if (j != i && kill(pids[j], SIGKILL) == 0)
					waitpid(pids[j], NULL, 0);
			fprintf(stderr, "bench: a reader exited before it was ready\n");
			exit(1);
		}
		sched_yield();
	}
}

/* ---- 1, 2: publish ----------------------------------------------------- */

static double run_publish(uint32_t payload)
{
	static uint64_t buf[PAYLOAD / 8];
	hg_pub *p = fresh_pub(payload == PAYLOAD ? "bench.publish" : "bench.publish16", payload, 16);
	uint64_t t0 = hg_now_ns();
	for (unsigned long long i = 0; i < count; i++)
		hg_publish(p, buf, payload, i);
	uint64_t t1 = hg_now_ns();
	hg_pub_close(p);
	return (double)(t1 - t0) / (double)count;
}

/* ---- 3: latest --------------------------------------------------------- */

static double run_latest(void)
{
	static uint64_t buf[PAYLOAD / 8], in[PAYLOAD / 8];
	hg_pub *p = fresh_pub("bench.latest", PAYLOAD, 16);
	hg_sub *s = hg_sub_open("bench.latest");
	if (!s)
		fail("hg_sub_open");
	struct hg_info info;
	unsigned long long bad = 0;
	uint64_t t0 = hg_now_ns();
	for (unsigned long long i = 0; i < count; i++) {
		hg_publish(p, buf, sizeof buf, i);
		bad += hg_latest(s, in, sizeof in, &info) != HG_MSG;
	}
	uint64_t t1 = hg_now_ns();
	for (unsigned long long i = 0; i < count; i++)
		hg_publish(p, buf, sizeof buf, i);
	uint64_t t2 = hg_now_ns();
	if (bad)
		fprintf(stderr, "bench: latest: %llu calls returned no message\n", bad);
	hg_sub_close(s);
	hg_pub_close(p);
	return ((double)(t1 - t0) - (double)(t2 - t1)) / (double)count;
}

/* ---- 4: torn ----------------------------------------------------------- */

struct torn_shared {
	atomic_int ready;
	atomic_int stop;
	unsigned long long read[TORN_READERS], torn[TORN_READERS], busy[TORN_READERS];
};

static void torn_reader(struct torn_shared *sh, int id)
{
	static uint64_t buf[PAYLOAD / 8];
	hg_sub *s = hg_sub_open("bench.torn");
	if (!s)
		_exit(1);
	unsigned long long nread = 0, torn = 0, busy = 0;
	atomic_fetch_add(&sh->ready, 1);
	for (unsigned long i = 0; !atomic_load_explicit(&sh->stop, memory_order_relaxed); i++) {
		struct hg_info info;
		enum hg_result r = (i & 1) ? hg_next(s, buf, sizeof buf, &info)
					   : hg_latest(s, buf, sizeof buf, &info);
		if (r == HG_MSG) {
			nread++;
			torn += !intact(buf, &info);
		} else if (r == HG_BUSY) {
			busy++;
		}
	}
	sh->read[id] = nread;
	sh->torn[id] = torn;
	sh->busy[id] = busy;
	hg_sub_close(s);
	_exit(0);
}

static void run_torn(double *msgs, unsigned long long *torn, unsigned long long *busy,
		     unsigned long long *published)
{
	static uint64_t buf[PAYLOAD / 8];
	struct torn_shared *sh = mmap(NULL, sizeof *sh, PROT_READ | PROT_WRITE,
				      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED)
		fail("mmap");
	memset(sh, 0, sizeof *sh);
	hg_pub *p = fresh_pub("bench.torn", PAYLOAD, 4);
	pid_t pids[TORN_READERS];
	for (int i = 0; i < TORN_READERS; i++) {
		if ((pids[i] = fork()) < 0)
			fail("fork");
		if (pids[i] == 0)
			torn_reader(sh, i);
	}
	wait_ready(&sh->ready, TORN_READERS, pids, TORN_READERS);
	uint64_t end = hg_now_ns() + torn_seconds * 1000000000ull;
	uint64_t n = 0;
	do {
		for (int k = 0; k < 1024; k++) {
			n = hg_pub_next_seq(p);
			fill(buf, n);
			hg_publish(p, buf, sizeof buf, n);
		}
	} while (hg_now_ns() < end);
	atomic_store(&sh->stop, 1);
	for (int i = 0; i < TORN_READERS; i++)
		waitpid(pids[i], NULL, 0);
	hg_pub_close(p);
	*msgs = 0;
	for (int i = 0; i < TORN_READERS; i++) {
		*msgs += (double)sh->read[i];
		*torn += sh->torn[i];
		*busy += sh->busy[i];
	}
	*published = n;
	munmap(sh, sizeof *sh);
}

/* ---- 5: lost_wake ------------------------------------------------------ */

struct lost_shared {
	atomic_int ready;
	unsigned long long msgs, gaps, timeouts, torn;
};

static void lost_reader(struct lost_shared *sh)
{
	static uint64_t buf[PAYLOAD / 8];
	hg_sub *s = hg_sub_open("bench.lost_wake");
	if (!s)
		_exit(1);
	atomic_store(&sh->ready, 1);
	unsigned long long msgs = 0, gaps = 0, timeouts = 0, torn = 0;
	for (int done = 0; !done;) {
		unsigned char rd;
		int r = hg_wait(&s, 1, 10000, &rd);
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0)
			_exit(1);
		if (r == 0)
			timeouts++;
		for (;;) {
			struct hg_info info;
			enum hg_result res = hg_next(s, buf, sizeof buf, &info);
			gaps += info.gap;
			if (res == HG_CLOSED)
				done = 1;
			if (res != HG_MSG)
				break;
			msgs++;
			torn += !intact(buf, &info);
		}
	}
	sh->msgs = msgs;
	sh->gaps = gaps;
	sh->timeouts = timeouts;
	sh->torn = torn;
	hg_sub_close(s);
	_exit(0);
}

static void run_lost_wake(struct lost_shared *out)
{
	static uint64_t buf[PAYLOAD / 8];
	struct lost_shared *sh = mmap(NULL, sizeof *sh, PROT_READ | PROT_WRITE,
				      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED)
		fail("mmap");
	memset(sh, 0, sizeof *sh);
	hg_pub *p = fresh_pub("bench.lost_wake", PAYLOAD, 16);
	pid_t pid = fork();
	if (pid < 0)
		fail("fork");
	if (pid == 0)
		lost_reader(sh);
	wait_ready(&sh->ready, 1, &pid, 1);
	for (unsigned long long i = 0; i < lost_msgs; i++) {
		uint64_t n = hg_pub_next_seq(p);
		fill(buf, n);
		hg_publish(p, buf, sizeof buf, n);
		long gap_ns = (long)(rng() % 200001);
		if (gap_ns) {
			struct timespec ts = { 0, gap_ns };
			while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
				;
		}
	}
	hg_pub_close(p);
	waitpid(pid, NULL, 0);
	*out = *sh;
	munmap(sh, sizeof *sh);
}

/* ---- main -------------------------------------------------------------- */

static int usage(void)
{
	fprintf(stderr, "usage: bench [publish|publish16|latest|torn|lost_wake ...] [--runs 5] "
			"[--count 5000000] [--torn-seconds 10] [--lost-msgs 100000]\n");
	return 2;
}

int main(int argc, char **argv)
{
	enum { T_PUB = 1, T_PUB16 = 2, T_LATEST = 4, T_TORN = 8, T_LOST = 16 };
	static const char *const names[] = { "publish", "publish16", "latest", "torn", "lost_wake" };
	int tests = 0;
	for (int i = 1; i < argc; i++) {
		unsigned long long *opt = NULL, v;
		if (!strcmp(argv[i], "--count"))
			opt = &count;
		else if (!strcmp(argv[i], "--torn-seconds"))
			opt = &torn_seconds;
		else if (!strcmp(argv[i], "--lost-msgs"))
			opt = &lost_msgs;
		if (opt || !strcmp(argv[i], "--runs")) {
			char *end;
			if (i + 1 >= argc)
				return usage();
			v = strtoull(argv[++i], &end, 10);
			if (*end || v == 0)
				return usage();
			if (opt)
				*opt = v;
			else if (v > MAX_RUNS)
				return usage();
			else
				runs = (int)v;
			continue;
		}
		int t = 0;
		for (int k = 0; k < 5; k++)
			if (!strcmp(argv[i], names[k]))
				t = 1 << k;
		if (!t)
			return usage();
		tests |= t;
	}
	if (!tests)
		tests = T_PUB | T_PUB16 | T_LATEST | T_TORN | T_LOST;
	if (!getenv("HANGAR_TOPIC_DIR")) {
		fprintf(stderr, "bench: HANGAR_TOPIC_DIR is not set\n");
		return 1;
	}
	rng_state ^= hg_now_ns();

	double v[MAX_RUNS];
	const char *sep = "";
	printf("{\"runs\":%d", runs);
	sep = ",";
	if (tests & T_PUB) {
		for (int r = 0; r < runs; r++)
			v[r] = run_publish(PAYLOAD);
		printf("%s\"publish_ns\":%.2f", sep, median(v, runs));
	}
	if (tests & T_PUB16) {
		for (int r = 0; r < runs; r++)
			v[r] = run_publish(16);
		printf("%s\"publish16_ns\":%.2f", sep, median(v, runs));
	}
	if (tests & T_LATEST) {
		for (int r = 0; r < runs; r++)
			v[r] = run_latest();
		printf("%s\"latest_ns\":%.2f", sep, median(v, runs));
	}
	fflush(stdout);
	if (tests & T_TORN) {
		unsigned long long torn = 0, busy = 0, pubs = 0;
		double published[MAX_RUNS];
		for (int r = 0; r < runs; r++) {
			run_torn(&v[r], &torn, &busy, &pubs);
			published[r] = (double)pubs;
		}
		printf("%s\"torn\":{\"seconds\":%llu,\"readers\":%d,\"slots\":4,\"published\":%.0f,"
		       "\"msgs_read\":%.0f,\"torn\":%llu,\"busy\":%llu}",
		       sep, torn_seconds, TORN_READERS, median(published, runs), median(v, runs), torn,
		       busy);
	}
	fflush(stdout);
	if (tests & T_LOST) {
		double msgs[MAX_RUNS], gaps[MAX_RUNS];
		unsigned long long timeouts = 0, torn = 0;
		for (int r = 0; r < runs; r++) {
			struct lost_shared res;
			run_lost_wake(&res);
			msgs[r] = (double)res.msgs;
			gaps[r] = (double)res.gaps;
			timeouts += res.timeouts;
			torn += res.torn;
		}
		printf("%s\"lost_wake\":{\"sent\":%llu,\"msgs\":%.0f,\"gaps\":%.0f,\"timeouts\":%llu,"
		       "\"torn\":%llu}",
		       sep, lost_msgs, median(msgs, runs), median(gaps, runs), timeouts, torn);
	}
	printf("}\n");
	return 0;
}
