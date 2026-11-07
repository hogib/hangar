// SPDX-License-Identifier: GPL-3.0-or-later
/* server: serves the methods its wiring lists among echo (reply = request,
 * both depth), refuse (HG_E_REFUSED, reason "no: <depth_m>"), nap (sleeps
 * --nap-ms (150) then echoes: a handler that overruns its deadline),
 * secret and adminonly (echo, with restrictive Allow lists in the test
 * manifests). Optionally reads a data topic (--data T, depth) beside its
 * callers' request topics; --lazy MS sleeps before each wait so that a
 * request and a data message are both ready in one pass. Status line:
 * "served N data M both K". */
#include "toy.h"

#include "hg_call.h"

static long served, data_n, both;
static long nap_ms;

static int on_echo(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	(void)c;
	(void)reason;
	memcpy(rep, req, sizeof(struct depth));
	served++;
	return HG_OK;
}

static int on_refuse(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	(void)c;
	(void)rep;
	const struct depth *d = req;
	snprintf(reason, HG_CALL_REASON, "no: %g", (double)d->depth_m);
	served++;
	return HG_E_REFUSED;
}

static int on_nap(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	toy_sleep_ms(nap_ms);
	return on_echo(c, req, rep, reason);
}

int main(int argc, char **argv)
{
	toy_init("server");
	nap_ms = toy_num(argc, argv, "--nap-ms", 150);
	long lazy = toy_num(argc, argv, "--lazy", 0);
	const char *data = toy_arg(argc, argv, "--data", NULL);
	hg_serve_method("echo", on_echo, NULL);
	hg_serve_method("secret", on_echo, NULL);
	hg_serve_method("adminonly", on_echo, NULL);
	hg_serve_method("refuse", on_refuse, NULL);
	hg_serve_method("nap", on_nap, NULL);
	hg_sub *subs[HG_MAX_WAIT];
	int n = hg_call_subs(subs, HG_MAX_WAIT - 1);
	if (n < 0) {
		fprintf(stderr, "server: hg_call_subs: %s\n", strerror(errno));
		return 4;
	}
	int ncall = n;
	hg_sub *ds = NULL;
	if (data) {
		ds = hg_sub_open(data, &DEPTH_TYPE);
		if (!ds) {
			fprintf(stderr, "server: hg_sub_open(%s): %s\n", data, strerror(errno));
			return 4;
		}
		subs[n++] = ds;
	}
	hg_ready();
	unsigned char ready[HG_MAX_WAIT];
	char line[128];
	while (!toy_stop) {
		if (lazy)
			toy_sleep_ms(lazy);
		unsigned r = hg_wait_ready(subs, n, 100, ready);
		if (r & HG_READY_INTERRUPT)
			break;
		int call_ready = 0;
		for (int i = 0; i < ncall; i++)
			call_ready |= ready[i];
		if (ds && ready[n - 1] && call_ready)
			both++;
		hg_serve();
		if (ds) {
			struct depth d;
			struct hg_info info;
			while (hg_next(ds, &d, sizeof d, &info) == HG_MSG)
				data_n++;
		}
		snprintf(line, sizeof line, "served %ld data %ld both %ld", served, data_n, both);
		hg_status(line);
		hg_alive();
	}
	fprintf(stderr, "server: SIGTERM after %ld requests\n", served);
	return 0;
}
