// SPDX-License-Identifier: GPL-3.0-or-later
/* retype: a toy publisher for the recorder's and the pairer's suites, a
 * topic whose writer restarts with another type (a module upgraded and
 * restarted, a dataset topic after its spec changed, a frame topic after the
 * camera renegotiated another size). Unwired: the file is replaced and its
 * seqs go on.
 *
 *   retype --topic T --first "<msg text>" --then "<msg text>"
 *          [--count N (3)] [--rate R (0: an event topic)] [--gap-ms G (100)]
 *
 * Publishes N messages of the first type, closes, reopens T with the second
 * type and publishes N more. Message k (1, 2, ... over both types) has k in
 * its first byte and k + 1 in its last; the rest is zero. --first and --then
 * take \n as written ("f64 x\nf64 y"). Exits 0 when done. */
#include "toy.h"

#include "hg_type.h"

static int parse(const char *text, struct hg_schema *out)
{
	char buf[512], why[128];
	size_t n = 0;
	for (const char *p = text; *p && n + 2 < sizeof buf; p++) {
		if (p[0] == '\\' && p[1] == 'n') {
			buf[n++] = '\n';
			p++;
		} else {
			buf[n++] = *p;
		}
	}
	if (!n || buf[n - 1] != '\n')
		buf[n++] = '\n';
	if (hg_schema_parse(buf, n, out, why, sizeof why) < 0) {
		fprintf(stderr, "retype: %s: %s\n", text, why);
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	toy_signals();
	const char *topic = toy_arg(argc, argv, "--topic", "re.t");
	long count = toy_num(argc, argv, "--count", 3);
	long rate = toy_num(argc, argv, "--rate", 0);
	long gap = toy_num(argc, argv, "--gap-ms", 100);
	static struct hg_schema type[2];
	if (parse(toy_arg(argc, argv, "--first", "u64 v"), &type[0]) < 0 ||
	    parse(toy_arg(argc, argv, "--then", "f64 x\nf64 y"), &type[1]) < 0)
		return 2;
	if (hg_init() < 0) {
		fprintf(stderr, "retype: hg_init: %s (%s)\n", hg_init_error(), strerror(errno));
		return 3;
	}
	struct hg_pub_opts o = { .slots = 16, .rate_mhz = (uint32_t)rate * 1000u };
	unsigned char k = 0;
	for (int t = 0; t < 2 && !toy_stop; t++) {
		hg_pub *p = hg_pub_open(topic, &type[t].type, &o);
		if (!p) {
			fprintf(stderr, "retype: %s: %s\n", topic, strerror(errno));
			return 3;
		}
		uint32_t len = type[t].type.payload_bytes;
		unsigned char *payload = calloc(1, len ? len : 1);
		if (!payload)
			return 3;
		for (long i = 0; i < count && !toy_stop; i++) {
			k++;
			payload[0] = k;
			if (len > 1)
				payload[len - 1] = (unsigned char)(k + 1);
			if (hg_publish(p, payload, len, hg_now_ns()) < 0)
				fprintf(stderr, "retype: publish: %s\n", strerror(errno));
			toy_sleep_ms(gap);
		}
		free(payload);
		hg_pub_close(p);
	}
	return 0;
}
