// SPDX-License-Identifier: GPL-3.0-or-later
/* The tether bridge's wire protocol (modules/bridge/bridge.c): everything the
 * bridge does with a datagram from its UDP socket and a line from its
 * discovery socket. This is network input: on_dgram (the header, the tag, the
 * replay window), on_hello and the handshake's state, on_pong and the clock
 * estimate, on_data and the reliable stream's reassembly (drain_records), the
 * records on it (on_info, on_schema, on_message, and through them
 * hg_schema_parse and the mirrored topic's file), on_ack, on_beacon.
 *
 * bridge.c is included whole, so the harness calls its static functions and
 * sets its state; nothing in the module was changed for it. Sends go to a
 * closed socket and fail; mirrored topics are created without a manager in a
 * directory of this process's own, emptied after every input.
 *
 * A tag cannot be guessed, so an input that is only bytes never gets past the
 * tag check. The input is therefore a script (fuzz_bridge.h): raw datagrams,
 * which test everything before the check, and datagrams the harness seals with
 * the key a real peer would hold at that moment, which test everything after
 * it: what a peer that holds the shared key, or its implementation's bugs,
 * can make this side do. */
#include "fuzz.h"
#include "fuzz_bridge.h"

#include <dirent.h>

#define main bridge_main
#include "bridge.c"
#undef main

static char dir[256];
static uint64_t peer_seq;
static const struct sockaddr_in from = { .sin_family = AF_INET, .sin_port = 0x1234, .sin_addr = { 0x0100007f } };

static void empty_dir(int all)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		char p[600];
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		/* The directory word and the link topic stay for the whole run. */
		if (!all && (!strcmp(e->d_name, ".dir") || strstr(e->d_name, "bridge.fuzz.link")))
			continue;
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		unlink(p);
	}
	closedir(d);
}

static void cleanup(void)
{
	empty_dir(1);
	rmdir(dir);
}

static struct bridge_conf conf0;
static hg_pub *link_pub0;
static unsigned char *sbuf0;

static void setup(void)
{
	const char *tmp = getenv("TMPDIR");
	snprintf(dir, sizeof dir, "%s/hg-fuzz-bridge-XXXXXX", tmp && *tmp ? tmp : "/tmp");
	if (!mkdtemp(dir))
		abort();
	setenv("HANGAR_TOPIC_DIR", dir, 1);
	unsetenv("HANGAR_CONTROL");
	unsetenv("HANGAR_WIRING");
	unsetenv("HANGAR_WIRING_FILE");
	unsetenv("HANGAR_PEER_TOPIC_DIR");
	atexit(cleanup);
	char err[200];
	if (bc_parse(FZB_CONF, strlen(FZB_CONF), &conf0, err, sizeof err) < 0) {
		fprintf(stderr, "the harness's bridge.conf: %s\n", err);
		abort();
	}
	link_pub0 = hg_pub_open("bridge.fuzz.link", &BRIDGE_LINK_TYPE, NULL);
	sbuf0 = malloc(SBUF_CAP);
	if (!link_pub0 || !sbuf0)
		abort();
}

/* The bridge as main() leaves it before its loop, in the state the mode asks. */
static void reset(unsigned mode)
{
	for (int i = 0; i < B.nrx; i++) {
		if (B.rx[i].pub)
			hg_pub_close(B.rx[i].pub);
		free(B.rx[i].sch);
		free(B.rx[i].schema);
	}
	struct prec *precs = B.precs;
	int cap_prec = B.cap_prec;
	memset(&B, 0, sizeof B);
	B.precs = precs;
	B.cap_prec = cap_prec;
	B.conf = conf0;
	B.link_pub = link_pub0;
	B.sbuf = sbuf0;
	B.sock = B.dsock = B.ep = B.evfd = B.tfd = -1;
	memset(B.key, 0x5a, sizeof B.key);
	B.vehicle = (mode & FZB_VEHICLE) != 0;
	snprintf(B.role, sizeof B.role, "%s", B.vehicle ? "vehicle" : "topside");
	B.ring_max = 64u << 10; /* a mirrored ring is a file per input here: kept small */
	B.id = 0x11223344;
	memset(B.boot, 0xb0, sizeof B.boot);
	new_nonce();
	ncands = 0;
	peer_seq = 0;
	if (mode & FZB_OFFERED)
		B.nonce_since = now_ns();
	if (mode & FZB_KEYED) {
		/* As promote() leaves it: keys, the peer's boot id, new streams. */
		memset(B.k_tx, 0x11, 32);
		memset(B.k_rx, 0x22, 32);
		memset(B.peer_boot, 0xbb, 16);
		B.peer_id = 0x55667788;
		B.link_id = 0x1001;
		B.keyed = 1;
		B.peer = from;
		B.have_peer = 1;
		reset_generation();
	}
	if (mode & FZB_CLOCK) {
		B.have_offset = 1;
		B.samp[0] = (struct sample){ .at = now_ns(), .off = 0, .rtt = 1000000 };
		B.nsamp = 1;
	}
	if (mode & FZB_MIRROR_OK)
		B.peer_info = B.config_match = B.mirror_ok = 1;
}

/* A datagram as a peer holding `key` would send it, with the sequence number seq. */
static void sealed_at(uint64_t seq, int kind, int prio, const unsigned char *body, size_t blen, const unsigned char *key)
{
	unsigned char d[DGRAM_MAX];
	if (blen > BODY_MAX)
		blen = BODY_MAX;
	memcpy(d, "HGB1", 4);
	d[4] = (unsigned char)kind;
	d[5] = (unsigned char)prio;
	d[6] = d[7] = 0;
	put32(d + 8, kind == K_HELLO ? 0 : B.link_id);
	put32(d + 12, (uint32_t)blen);
	put64(d + 16, seq);
	if (blen)
		memcpy(d + HDR, body, blen);
	tag_of(key, d, HDR + blen, d + HDR + blen);
	on_dgram(d, HDR + blen + TAG, &from);
}

/* The same with the next sequence number, as a peer sends one after another. */
static void sealed(int kind, int prio, const unsigned char *body, size_t blen, const unsigned char *key)
{
	sealed_at(++peer_seq, kind, prio, body, blen, key);
}

static const unsigned char *key_now(int kind)
{
	return kind == K_HELLO ? B.key : B.keyed ? B.k_rx : B.p_rx;
}

static void tick(void)
{
	choose_peer();
	if (B.have_offset && B.nsamp)
		estimate();
	s_resend_check();
	s_pump();
	publish_status();
}

static void check_state(void)
{
	FZ_CHECK(B.nrx >= 0 && B.nrx <= MAX_RX);
	FZ_CHECK(B.rlen <= sizeof recbuf);
	FZ_CHECK(B.s_base <= B.s_next && B.s_next <= B.s_end);
	FZ_CHECK(B.s_end - B.s_base <= SBUF_CAP);
	FZ_CHECK(B.nsamp >= 0 && B.nsamp <= (int)(sizeof B.samp / sizeof B.samp[0]));
	FZ_CHECK(ncands >= 0 && ncands <= 8);
	FZ_CHECK(B.rings <= RINGS_MAX);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (!dir[0])
		setup();
	if (size < 1)
		return 0;
	reset(data[0]);
	data++;
	size--;
	while (size >= 3) {
		unsigned op = data[0] % FZB_OPS;
		size_t len = data[1] | (size_t)data[2] << 8;
		data += 3;
		size -= 3;
		if (len > size)
			len = size;
		/* Each record in a block of its own length: a read past a datagram
		 * is a read past a heap block. */
		unsigned char *p = malloc(len ? len : 1);
		if (!p)
			abort();
		memcpy(p, data, len);
		switch (op) {
		case FZB_RAW:
			on_dgram(p, len, &from);
			break;
		case FZB_SEALED:
		case FZB_PENDING:
			if (len >= 2)
				sealed(p[0], p[1], p + 2, len - 2, op == FZB_PENDING ? B.p_rx : key_now(p[0]));
			break;
		case FZB_SEALED_AT:
			if (len >= 3)
				sealed_at(peer_seq + (uint64_t)(int64_t)(int8_t)p[0], p[1], p[2], p + 3, len - 3, key_now(p[1]));
			break;
		case FZB_BEACON:
			on_beacon((const char *)p, len, &from);
			break;
		case FZB_BEACON_TAG: {
			char line[256], want[40];
			unsigned char tg[16];
			if (len > 180)
				break;
			memcpy(line, p, len);
			blake2s(tg, 16, B.key, 32, line, len);
			for (int i = 0; i < 16; i++)
				snprintf(want + 2 * i, 3, "%02x", tg[i]);
			int n = snprintf(line + len, sizeof line - len, " tag=%s", want);
			on_beacon(line, len + (size_t)n, &from);
			break;
		}
		case FZB_STREAM: {
			unsigned char b[BODY_MAX];
			size_t n = len > BODY_MAX - 8 ? BODY_MAX - 8 : len;
			put64(b, B.r_expect);
			memcpy(b + 8, p, n);
			sealed(K_DATA, BC_CALL, b, 8 + n, key_now(K_DATA));
			break;
		}
		case FZB_TICK:
			tick();
			break;
		case FZB_HELLO: {
			unsigned char b[HELLO_BODY];
			memset(b, 0, sizeof b);
			if (len < 1)
				break;
			memcpy(b, p + 1, len - 1 < sizeof b ? len - 1 : sizeof b);
			if (p[0] & 1) {
				memset(b + 8, 0, 32);
				memcpy(b + 8, B.conf.pair, strnlen(B.conf.pair, 31));
			}
			if (p[0] & 2)
				memcpy(b + 72, B.nonce, 16);
			if (p[0] & 4)
				b[0] = (unsigned char)(B.vehicle ? 2 : 1);
			sealed(K_HELLO, BC_SAFETY, b, sizeof b, B.key);
			break;
		}
		case FZB_PONG: {
			unsigned char b[24];
			memset(b, 0, sizeof b);
			put64(b, B.npings ? B.pings[(B.npings - 1) % 16] : 0);
			memcpy(b + 8, p, len < 16 ? len : 16);
			sealed(K_PONG, BC_SAFETY, b, sizeof b, key_now(K_PONG));
			break;
		}
		}
		free(p);
		check_state();
		data += len;
		size -= len;
	}
	FZ_NOTE("keyed %d up %d peer_info %d mirror_ok %d clock %d: %llu rejected, %u topics in, %llu msgs in, %llu late, "
		"%d candidates, stream at %llu",
		B.keyed, B.up, B.peer_info, B.mirror_ok, B.have_offset, (unsigned long long)B.st.rejected, B.st.topics_rx,
		(unsigned long long)B.st.msgs_rx, (unsigned long long)B.st.dropped_late, ncands,
		(unsigned long long)B.r_expect);
	reset(0);
	empty_dir(0);
	return 0;
}
