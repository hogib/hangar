// SPDX-License-Identifier: GPL-3.0-or-later
/* prov_chain: a toy publisher for the recorder's and the pairer's suites, a
 * provenance chain in one process, wired by a file it writes itself:
 *
 *   <p>.a  depth, rated          the sensor
 *   <p>.b  imu, rated            computed from <p>.a (Inputs); HG_STALE on a range of seqs
 *   <p>.c  wrench, rated         computed from <p>.a and <p>.b (Inputs) and, every
 *                                K-th message, from <p>.x (Sources)
 *   <p>.x  depth, event          referenced by every K-th <p>.c
 *   <p>.s  depth, rated at R/5   stamped --s-lag-ms before it is published: the
 *                                asof part whose lateness matters
 *
 * Each tick publishes a(n), b(n) (unless skipped), c(n), and s every fifth
 * tick, after c. Options: --prefix P (chain) --rate R (50) --count N (0:
 * forever) --slots S (16) --skip-b-every M (0) --stale-b-from A --stale-b-to B
 * (0 0) --x-every K (4) --s-lag-ms L (0) --stamp-lag-ms (1). SIGTERM: closes
 * and exits 0. Exits 0 after --count ticks. */
#include "toy.h"

#include "imu.h"
#include "wrench.h"

static const char *P;

static void topic(char *out, size_t len, const char *suffix)
{
	snprintf(out, len, "%s.%s", P, suffix);
}

int main(int argc, char **argv)
{
	toy_signals();
	P = toy_arg(argc, argv, "--prefix", "chain");
	long rate = toy_num(argc, argv, "--rate", 50);
	long count = toy_num(argc, argv, "--count", 0);
	long slots = toy_num(argc, argv, "--slots", 16);
	long skip_b = toy_num(argc, argv, "--skip-b-every", 0);
	long stale_from = toy_num(argc, argv, "--stale-b-from", 0);
	long stale_to = toy_num(argc, argv, "--stale-b-to", 0);
	long x_every = toy_num(argc, argv, "--x-every", 4);
	long s_lag = toy_num(argc, argv, "--s-lag-ms", 0);
	long stamp_lag = toy_num(argc, argv, "--stamp-lag-ms", 1);
	const char *dir = getenv("HANGAR_TOPIC_DIR");
	if (!dir) {
		fprintf(stderr, "prov_chain: HANGAR_TOPIC_DIR is not set\n");
		return 2;
	}
	char a[64], b[64], c[64], x[64], s[64], wpath[600];
	topic(a, sizeof a, "a");
	topic(b, sizeof b, "b");
	topic(c, sizeof c, "c");
	topic(x, sizeof x, "x");
	topic(s, sizeof s, "s");
	snprintf(wpath, sizeof wpath, "%s/.prov_chain-%s.wiring", dir, P);
	FILE *f = fopen(wpath, "w");
	if (!f) {
		perror("prov_chain: wiring");
		return 2;
	}
	fprintf(f, "hangar-wiring 1\nmodule prov_chain\n");
	fprintf(f, "pub %s type=depth hash=%016llx slots=%ld rate=%ld\n", a, (unsigned long long)DEPTH_TYPE.hash, slots, rate);
	fprintf(f, "pub %s type=imu hash=%016llx slots=%ld rate=%ld inputs=%s\n", b, (unsigned long long)IMU_TYPE.hash,
		slots, rate, a);
	fprintf(f, "pub %s type=wrench hash=%016llx slots=%ld rate=%ld inputs=%s,%s sources=%s\n", c,
		(unsigned long long)WRENCH_TYPE.hash, slots, rate, a, b, x);
	fprintf(f, "pub %s type=depth hash=%016llx slots=%ld\n", x, (unsigned long long)DEPTH_TYPE.hash, slots);
	fprintf(f, "pub %s type=depth hash=%016llx slots=%ld rate=%g\n", s, (unsigned long long)DEPTH_TYPE.hash, slots,
		(double)rate / 5.0);
	fclose(f);
	setenv("HANGAR_WIRING_FILE", wpath, 1);
	unsetenv("HANGAR_CONTROL");
	if (hg_init() < 0) {
		fprintf(stderr, "prov_chain: hg_init: %s (%s)\n", hg_init_error(), strerror(errno));
		return 3;
	}
	hg_pub *pa = hg_pub_open(a, &DEPTH_TYPE, NULL);
	hg_pub *pb = hg_pub_open(b, &IMU_TYPE, NULL);
	hg_pub *pc = hg_pub_open(c, &WRENCH_TYPE, NULL);
	hg_pub *px = hg_pub_open(x, &DEPTH_TYPE, NULL);
	hg_pub *ps = hg_pub_open(s, &DEPTH_TYPE, NULL);
	if (!pa || !pb || !pc || !px || !ps) {
		fprintf(stderr, "prov_chain: hg_pub_open: %s\n", strerror(errno));
		return 4;
	}
	hg_ready();
	long n = 0;
	uint64_t period_ns = rate > 0 ? 1000000000ull / (uint64_t)rate : 100000000ull;
	uint64_t next = hg_now_ns();
	while (!toy_stop && (count <= 0 || n < count)) {
		n++;
		uint64_t now = hg_now_ns();
		uint64_t stamp = now - (uint64_t)stamp_lag * 1000000u;
		struct depth da = { .depth_m = (float)n * 0.01f, .temp_c = 20, .pressure_mbar = (float)n };
		hg_publish(pa, &da, sizeof da, stamp);
		uint64_t a_seq = hg_pub_next_seq(pa) - 1;
		if (!(skip_b > 0 && n % skip_b == 0)) {
			struct imu im = { .gyro = { (float)n, 1, 2 }, .accel = { 0, 0, 9.81f }, .temp_c = 30 };
			int stale = n >= stale_from && n < stale_to;
			struct hg_input ra = { .topic_id = hg_pub_topic_id(pa), .seq = a_seq };
			hg_publish_with(pb, &im, sizeof im, stamp, stale ? HG_STALE : 0, &ra, 1);
		}
		int with_x = x_every > 0 && n % x_every == 0;
		if (with_x) {
			struct depth dx = { .depth_m = (float)n, .temp_c = 1, .pressure_mbar = 2 };
			hg_publish(px, &dx, sizeof dx, stamp);
		}
		struct wrench w = { .surge = (float)n, .heave = (float)(n % 7) * 0.1f, .yaw = 0.5f };
		if (with_x) {
			struct hg_input rx = { .topic_id = hg_pub_topic_id(px), .seq = hg_pub_next_seq(px) - 1 };
			hg_publish_with(pc, &w, sizeof w, stamp, 0, &rx, 1);
		} else {
			hg_publish(pc, &w, sizeof w, stamp);
		}
		if (n % 5 == 0) {
			struct depth ds = { .depth_m = (float)n, .temp_c = 2, .pressure_mbar = 3 };
			hg_publish(ps, &ds, sizeof ds, hg_now_ns() - (uint64_t)s_lag * 1000000u);
		}
		next += period_ns;
		now = hg_now_ns();
		if (next > now)
			toy_sleep_ms((long)((next - now) / 1000000u) + 1);
	}
	hg_pub_close(pa);
	hg_pub_close(pb);
	hg_pub_close(pc);
	hg_pub_close(px);
	hg_pub_close(ps);
	unlink(wpath);
	return 0;
}
