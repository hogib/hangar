// SPDX-License-Identifier: GPL-3.0-or-later
/* caller: calls --call <module>.<method> (server.echo) with a depth
 * request every --every ms (100), --count N times (-1: forever), each with
 * --timeout MS (500), through hg_call (--mode sync) or hg_call_start with
 * up to --inflight K calls at once (--mode async; 16 gives one HG_E_BUSY).
 * Each outcome is appended to --out FILE as "call <id> <status> <reason>".
 * The loop keeps publishing --topic (depth, 50 Hz) meanwhile when given,
 * as a module keeps its rate while a call is in flight. Status line:
 * "calls N ok M". */
#include "toy.h"

#include "hg_call.h"

static FILE *out;

static void record(const hg_call_result *r)
{
	if (!out)
		return;
	fprintf(out, "call %llu %s %s\n", (unsigned long long)r->call_id, hg_status_name(r->status), r->reason);
	fflush(out);
}

int main(int argc, char **argv)
{
	toy_init("caller");
	const char *target = toy_arg(argc, argv, "--call", "server.echo");
	const char *mode = toy_arg(argc, argv, "--mode", "sync");
	const char *path = toy_arg(argc, argv, "--out", NULL);
	const char *topic = toy_arg(argc, argv, "--topic", NULL);
	long every = toy_num(argc, argv, "--every", 100);
	long count = toy_num(argc, argv, "--count", -1);
	long timeout = toy_num(argc, argv, "--timeout", 500);
	long inflight = toy_num(argc, argv, "--inflight", 16);
	if (path)
		out = fopen(path, "a");
	hg_pub *p = NULL;
	if (topic) {
		struct hg_pub_opts o = { .rate_mhz = 50000 };
		p = hg_pub_open(topic, &DEPTH_TYPE, &o);
	}
	hg_ready();
	long n = 0, ok = 0;
	char line[96];
	if (strcmp(mode, "async") == 0) {
		/* A burst: start inflight calls at once, then poll them all. */
		hg_pending *pc[64] = { 0 };
		int k = inflight < 64 ? (int)inflight : 64;
		for (int i = 0; i < k; i++) {
			struct depth d = { .depth_m = (float)i };
			int st = hg_call_start(target, &DEPTH_TYPE, &d, &DEPTH_TYPE, &pc[i], (int)timeout);
			if (st != HG_OK) {
				hg_call_result r = { .status = st };
				snprintf(r.reason, sizeof r.reason, "%s", st == HG_E_BUSY ? "15 calls in flight" : "");
				record(&r);
				pc[i] = NULL;
				n++;
			}
		}
		for (;;) {
			int pending = 0;
			for (int i = 0; i < k; i++) {
				if (!pc[i])
					continue;
				hg_call_result r = hg_call_poll(pc[i], NULL);
				if (r.status == HG_PENDING) {
					pending++;
					continue;
				}
				record(&r);
				pc[i] = NULL;
				n++;
				if (r.status == HG_OK)
					ok++;
			}
			if (!pending || toy_stop)
				break;
			hg_sub *subs[4];
			int ns = hg_call_subs(subs, 4);
			if (ns > 0)
				hg_wait(subs, ns, 50);
		}
		snprintf(line, sizeof line, "calls %ld ok %ld", n, ok);
		hg_status(line);
		while (!toy_stop) {
			hg_alive();
			toy_sleep_ms(100);
		}
		return 0;
	}
	while (!toy_stop && (count < 0 || n < count)) {
		struct depth d = { .depth_m = (float)n, .temp_c = 20, .pressure_mbar = 1013 }, rep;
		if (p)
			hg_publish(p, &d, sizeof d, hg_now_ns());
		hg_call_result r = hg_call(target, &DEPTH_TYPE, &d, &DEPTH_TYPE, &rep, (int)timeout);
		if (r.status == HG_OK && rep.depth_m != d.depth_m) {
			r.status = HG_E_BADTYPE;
			snprintf(r.reason, sizeof r.reason, "echo mismatch");
		}
		record(&r);
		if (r.status == HG_OK)
			ok++;
		n++;
		snprintf(line, sizeof line, "calls %ld ok %ld", n, ok);
		hg_status(line);
		hg_alive();
		toy_sleep_ms(every);
	}
	while (!toy_stop) {
		if (p) {
			struct depth d = { .depth_m = 0 };
			hg_publish(p, &d, sizeof d, hg_now_ns());
		}
		hg_alive();
		toy_sleep_ms(p ? 20 : 100);
	}
	return 0;
}
