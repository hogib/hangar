// SPDX-License-Identifier: GPL-3.0-or-later
/* hangar-bridge: the tether bridge (docs/08-bridge.md), stage 1. One module per
 * machine; it finds the other side, authenticates it, and carries what
 * bridge.conf lists: topics up and down, and calls as their request and reply
 * topics. modules/bridge/README.md says what this stage leaves out.
 *
 * One UDP socket. Every datagram: a 24-byte header, a body of at most 1200
 * bytes, and a 16-byte BLAKE2s tag (docs/08 "The link"). Two deliveries:
 * `latest` (one datagram per message, never resent) and `reliable` (records
 * on an ordered byte stream, acknowledged and resent). The loop waits in one
 * epoll on the socket, the eventfd of hg_wait_fd (the named topics it sends)
 * and a 10 ms timer (glob matches, pings, the link status, resends). */
#define _GNU_SOURCE
#include "blake2s.h"
#include "bridge_conf.h"
#include "bridge_link.h"
#include "hg_call.h"
#include "hg_type.h"
#include "hg_wiring.h"
#include "mod.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>

#define WHO "hangar-bridge"
#ifndef HANGAR_VERSION
#define HANGAR_VERSION "dev"
#endif

#define HDR 24
#define TAG 16
#define BODY_MAX 1200
#define DGRAM_MAX (HDR + BODY_MAX + TAG)
#define MAX_TX 256
#define MAX_RX 256
#define MSG_MAX (1u << 20)        /* mirrored messages are at most 1 MB */
/* What the mirrors may hold of /dev/shm here: the peer says how many slots
 * each of its rings has and how large its messages are, and the manager
 * reserves that. A ring over RING_MAX gets fewer slots; past RINGS_MAX in all,
 * a topic is not mirrored. (4096 slots of 1 MB, which a peer could ask for
 * 256 times, would be a terabyte.) */
#define RING_MAX (32ull << 20)
#define RINGS_MAX (256ull << 20)
#define SLOT_EXTRA 128u           /* a slot's header beside its payload (lib/hg) */
#define REC_MAX (MSG_MAX + 600)   /* the largest record taken from the stream: a message as build_msg makes it */
#define SBUF_CAP (4u << 20)       /* the reliable stream's unacknowledged bytes */
#define WINDOW (64u << 10)        /* bytes in flight */
#define LINK_TIMEOUT_NS 3000000000ull
#define PING_NS 100000000ull
#define STATUS_NS 100000000ull
#define HELLO_NS 250000000ull
#define NONCE_NS 5000000000ull    /* a nonce is offered for this long, then drawn again */
#define HELLO_BODY 92
#define WINDOW_NS 10000000000ull  /* the clock's sample window */

enum { K_HELLO = 1, K_CONFIRM, K_PING, K_PONG, K_DATA, K_ACK, K_MSG };
enum { R_INFO = 1, R_SCHEMA, R_MSG };
enum { ST_OFF, ST_SEARCHING, ST_CONNECTING, ST_UP, ST_AMBIGUOUS, ST_REFUSED };

struct tx_topic {
	char name[HG_NAME_MAX + 1];
	hg_sub *sub;
	int reliable, priority;
	uint32_t rate_mhz;
	uint64_t last_sent_ns;
	int decimated;           /* messages skipped by Rate= since the last sent */
	int schema_sent;         /* in this generation of the stream */
	uint64_t acked_seq;      /* reliable: the last seq the peer acknowledged */
	/* Reliable: the messages read and not yet acknowledged, oldest first,
	 * each as the record that goes on the stream. At most the topic's ring
	 * of them (docs/08 "Delivery"); the oldest goes when one more comes. */
	struct held {
		uint64_t seq;
		uint32_t len;
		int queued;      /* on the stream of this generation */
		unsigned char *rec;
	} *held;
	int nheld, cap_held;
	uint32_t max_held;       /* the ring's slots, as last read from the file */
	uint64_t reopens;        /* the subscription's, when max_held was read */
	int call_kind;           /* 1 a request topic, 2 a reply topic */
};

struct rx_topic {
	char name[HG_NAME_MAX + 1];
	char writer[33];
	uint32_t slots;          /* the sender's ring */
	char *schema;
	uint32_t schema_len;
	struct hg_schema *sch;
	hg_pub *pub;
	int refused;             /* the manager would not create it: said once */
	uint64_t ring_bytes;     /* of the file it has here, counted in B.rings */
	uint64_t last_stamp;     /* translated, for clamping */
	const struct bc_mirror *mc;
};

struct sample {
	uint64_t at;
	int64_t off;
	uint64_t rtt;
};

static struct {
	struct bridge_conf conf;
	unsigned char key[32];
	int vehicle;             /* this machine's role */
	char role[16];
	int sock, ep, evfd, tfd, dsock;
	struct sockaddr_in peer;
	int have_peer, peer_fixed;
	char peer_txt[48];
	uint32_t id;
	unsigned char boot[16];
	/* This side's nonce for the next handshake: drawn again once keys were
	 * made in force with it, and NONCE_NS after it first went on the wire,
	 * so no pair of nonces, and so no session key, comes twice. */
	unsigned char nonce[16];
	uint64_t nonce_since;    /* when it was first sent; 0: not yet */
	int64_t fake_ns;         /* tests: this side's clock on the wire runs ahead by it */
	/* Keys in force, and a handshake under way. */
	int keyed;
	unsigned char k_tx[32], k_rx[32], peer_boot[16];
	uint32_t peer_id, link_id;
	int pending;
	unsigned char p_tx[32], p_rx[32], p_nonce[16], p_boot[16];
	uint32_t p_id, p_link;
	uint64_t last_hello_ns;
	int up;
	uint64_t last_rx_ns;
	uint64_t tx_seq, rx_hi, rx_win, rx_count;
	/* The reliable stream, out and in. */
	unsigned char *sbuf;
	uint64_t s_base, s_end, s_next, s_progress_ns;
	struct prec { uint64_t end; int tx; uint64_t seq; } *precs;
	int nprec, cap_prec;
	uint64_t r_expect;
	size_t rlen;
	uint64_t r_skip;         /* bytes still to come of a record too large to take */
	int r_held;              /* a message record waits for the clock's estimate */
	/* The exchange after the handshake (docs/08 "The link"). */
	int info_sent, peer_info, mirror_ok, config_match;
	char detail[96];
	/* Clocks. */
	struct sample samp[128];
	int nsamp;
	uint64_t pings[16];
	int npings;
	int64_t offset;
	uint64_t unc, rtt, rtt_min;
	int have_offset;
	/* Topics. */
	struct tx_topic tx[MAX_TX];
	int ntx;
	hg_sub *globs[HG_WIRING_MAX_GLOBS];
	int nglobs;
	struct rx_topic rx[MAX_RX];
	int nrx;
	hg_pub *link_pub;
	uint64_t rings, ring_max; /* the mirrored rings' bytes; the bound on one (RING_MAX) */
	uint64_t last_ping_ns, last_status_ns;
	struct bridge_link st;
} B;

/* recbuf: at most one record not yet whole, and one datagram more. */
static unsigned char msgbuf[MSG_MAX + 64], recbuf[5 + REC_MAX + BODY_MAX];
static unsigned char msg_hash[32]; /* of the .msg files of MsgDir */

/* ---- Small helpers ------------------------------------------------------ */

static uint64_t now_ns(void)
{
	return hg_now_ns();
}

static void put32(unsigned char *p, uint32_t v)
{
	memcpy(p, &v, 4);
}

static void put64(unsigned char *p, uint64_t v)
{
	memcpy(p, &v, 8);
}

static uint32_t get32(const unsigned char *p)
{
	uint32_t v;
	memcpy(&v, p, 4);
	return v;
}

static uint64_t get64(const unsigned char *p)
{
	uint64_t v;
	memcpy(&v, p, 8);
	return v;
}

static void random_bytes(void *out, size_t n)
{
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0 || read(fd, out, n) != (ssize_t)n) {
		uint64_t t = now_ns() ^ (uint64_t)getpid() << 32;
		for (size_t i = 0; i < n; i++)
			((unsigned char *)out)[i] = (unsigned char)(t >> (8 * (i % 8)) ^ i * 131);
	}
	if (fd >= 0)
		close(fd);
}

static void read_boot_id(unsigned char out[16])
{
	char txt[64] = "";
	FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
	if (f) {
		if (!fgets(txt, sizeof txt, f))
			txt[0] = 0;
		fclose(f);
	}
	/* A uuid with dashes; hashed, so any text gives 16 bytes. */
	blake2s(out, 16, NULL, 0, txt, strlen(txt));
	const char *t = getenv("HANGAR_BRIDGE_TEST_BOOT"); /* tests: a reboot of the sender */
	if (t)
		blake2s(out, 16, NULL, 0, t, strlen(t));
}

static int is_call_topic(const char *name)
{
	size_t n = strlen(name);
	if (n > 4 && strcmp(name + n - 4, ".req") == 0)
		return 1;
	if (n > 4 && strcmp(name + n - 4, ".rep") == 0)
		return 2;
	return 0;
}

/* ---- Datagrams ------------------------------------------------------------ */

static void tag_of(const unsigned char *key, const unsigned char *d, size_t len, unsigned char out[TAG])
{
	blake2s(out, TAG, key, 32, d, len);
}

/* Whether two tags are the same, in a time that does not say where they
 * first differ (memcmp stops there). */
static int tag_eq(const unsigned char *a, const unsigned char *b, size_t n)
{
	unsigned char x = 0;
	for (size_t i = 0; i < n; i++)
		x |= (unsigned char)(a[i] ^ b[i]);
	return x == 0;
}

/* To the peer; or to the address given, for the answers of a handshake with
 * someone who is not the peer yet. */
static int send_raw(const unsigned char *d, size_t len, const struct sockaddr_in *to)
{
	if (!to && !B.have_peer)
		return -1;
	if (!to)
		to = &B.peer;
	return (int)sendto(B.sock, d, len, MSG_DONTWAIT, (const struct sockaddr *)to, sizeof *to);
}

/* Header, body, tag: with the sending key in force, the shared key for a
 * hello, or the handshake's pending key for a confirm. */
static int send_dgram(int kind, int prio, const void *body, size_t blen, const unsigned char *key,
		      const struct sockaddr_in *to)
{
	unsigned char d[DGRAM_MAX];
	if (blen > BODY_MAX)
		return -1;
	memcpy(d, "HGB1", 4);
	d[4] = (unsigned char)kind;
	d[5] = (unsigned char)prio;
	d[6] = d[7] = 0;
	put32(d + 8, kind == K_HELLO ? 0 : B.link_id);
	put32(d + 12, (uint32_t)blen);
	put64(d + 16, ++B.tx_seq);
	memcpy(d + HDR, body, blen);
	tag_of(key, d, HDR + blen, d + HDR + blen);
	return send_raw(d, HDR + blen + TAG, to);
}

static int send_keyed(int kind, int prio, const void *body, size_t blen)
{
	if (!B.keyed)
		return -1;
	return send_dgram(kind, prio, body, blen, B.k_tx, NULL);
}

/* ---- Handshake (docs/08 "The link") ---------------------------------------- */

static void derive(const unsigned char *nv, const unsigned char *nt, unsigned char up[32], unsigned char down[32],
		   uint32_t *link_id)
{
	unsigned char in[4 + 32];
	memcpy(in, "up", 2);
	memcpy(in + 2, nv, 16);
	memcpy(in + 18, nt, 16);
	blake2s(up, 32, B.key, 32, in, 34);
	memcpy(in, "down", 4);
	memcpy(in + 4, nv, 16);
	memcpy(in + 20, nt, 16);
	blake2s(down, 32, B.key, 32, in, 36);
	unsigned char id[4];
	blake2s(id, 4, NULL, 0, in + 4, 32);
	*link_id = get32(id) | 1;
}

/* A new nonce of ours: whatever handshake was under way with the old one
 * is over. */
static void new_nonce(void)
{
	random_bytes(B.nonce, 16);
	B.nonce_since = 0;
	B.pending = 0;
}

/* Role, id, pair, our nonce, our boot id; then the peer's nonce this hello
 * answers (zeros: none, a first hello), and the id of the link whose keys we
 * hold (0: none). A hello is tagged under the shared key and can be recorded
 * and sent again for as long as that key lives: the echo is what makes one
 * fresh. Only a hello that echoes the nonce we offer now was made after we
 * offered it, by someone who holds the key. */
static void send_hello(const unsigned char *echo, const struct sockaddr_in *to)
{
	unsigned char b[HELLO_BODY];
	memset(b, 0, sizeof b);
	b[0] = (unsigned char)(B.vehicle ? 1 : 2);
	put32(b + 4, B.id);
	memcpy(b + 8, B.conf.pair, strnlen(B.conf.pair, 31));
	memcpy(b + 40, B.nonce, 16);
	memcpy(b + 56, B.boot, 16);
	if (echo)
		memcpy(b + 72, echo, 16);
	put32(b + 88, B.keyed ? B.link_id : 0);
	send_dgram(K_HELLO, BC_SAFETY, b, sizeof b, B.key, to);
	B.last_hello_ns = now_ns();
	if (!B.nonce_since)
		B.nonce_since = B.last_hello_ns;
}

static void send_confirm(const unsigned char *k_tx, const struct sockaddr_in *to)
{
	unsigned char b[32];
	memcpy(b, B.vehicle ? B.nonce : B.p_nonce, 16);
	memcpy(b + 16, B.vehicle ? B.p_nonce : B.nonce, 16);
	uint32_t keep = B.link_id;
	B.link_id = B.p_link;
	send_dgram(K_CONFIRM, BC_SAFETY, b, sizeof b, k_tx, to);
	B.link_id = keep;
}

static void reset_generation(void);

/* A hello, its tag checked under the shared key: which proves the key, at
 * some time, and nothing about now. Three kinds:
 *
 * - from the peer whose keys are in force (it names our link): it lost the
 *   link in an outage; the keys stand, and a confirm under them brings it
 *   back;
 * - one that does not echo the nonce we offer: a first hello, or an old one
 *   sent again. Nothing is kept of it and nothing changes; our hello,
 *   echoing its nonce, asks whoever sent it to answer now;
 * - one that echoes our nonce: fresh. Its keys become the pending ones, and
 *   a confirm under them goes back; the link is theirs when a datagram
 *   checks under them (on_dgram).
 *
 * So a recorded hello never sets a key, and the keys a fresh one sets are
 * made from a nonce of ours that no earlier session used.
 *
 * No hello sets the peer's address either: only a datagram that checks under
 * a session key does (on_dgram). The answers go to the peer when there is one
 * to keep (a link up, or Peer= fixed), and to where the hello came from
 * otherwise (Peer=auto, no link: the handshake that makes it the peer). */
static void on_hello(const unsigned char *b, size_t n, const struct sockaddr_in *from)
{
	if (n < HELLO_BODY) {
		B.st.rejected++;
		return;
	}
	int role = b[0];
	char pair[33];
	memcpy(pair, b + 8, 32);
	pair[32] = 0;
	const unsigned char *nonce = b + 40, *echo = b + 72;
	if (role == (B.vehicle ? 1 : 2) || role < 1 || role > 2 || strcmp(pair, B.conf.pair) != 0 ||
	    memcmp(nonce, B.nonce, 16) == 0) {
		B.st.rejected++; /* our own role or nonce, another pair: refused (docs/08) */
		return;
	}
	const struct sockaddr_in *to = B.peer_fixed || B.up ? &B.peer : from;
	if (B.keyed && get32(b + 88) == B.link_id) {
		send_dgram(K_CONFIRM, BC_SAFETY, "", 0, B.k_tx, to);
		return;
	}
	if (!B.nonce_since || memcmp(echo, B.nonce, 16) != 0) {
		send_hello(nonce, to);
		return;
	}
	int again = B.pending && memcmp(nonce, B.p_nonce, 16) == 0;
	if (!again) {
		memcpy(B.p_nonce, nonce, 16);
		memcpy(B.p_boot, b + 56, 16);
		B.p_id = get32(b + 4);
		unsigned char up[32], down[32];
		if (B.vehicle)
			derive(B.nonce, B.p_nonce, up, down, &B.p_link);
		else
			derive(B.p_nonce, B.nonce, up, down, &B.p_link);
		memcpy(B.p_tx, B.vehicle ? up : down, 32);
		memcpy(B.p_rx, B.vehicle ? down : up, 32);
		B.pending = 1;
		send_hello(nonce, to); /* the peer needs its nonce echoed too */
	}
	send_confirm(B.p_tx, to);
}

/* The pending keys become the keys in force: a new link, new streams. */
static void promote(void)
{
	memcpy(B.k_tx, B.p_tx, 32);
	memcpy(B.k_rx, B.p_rx, 32);
	int new_boot = !B.keyed || memcmp(B.peer_boot, B.p_boot, 16) != 0;
	memcpy(B.peer_boot, B.p_boot, 16);
	B.peer_id = B.p_id;
	B.link_id = B.p_link;
	B.keyed = 1;
	new_nonce(); /* this one made the keys in force: never offered again */
	/* tx_seq runs on for the life of the process: never a seq twice */
	B.rx_hi = B.rx_win = B.rx_count = 0;
	/* The clock starts over with every link: nothing is translated with
	 * the estimate of an earlier one (another boot of the peer has another
	 * clock, and so has another process under the tests' offset). */
	B.nsamp = 0;
	B.npings = 0;
	memset(B.pings, 0, sizeof B.pings);
	B.have_offset = 0;
	B.offset = 0;
	B.unc = 0;
	if (new_boot) {
		/* Its topics start their seqs over: each mirror is opened again
		 * with the new boot id, a new epoch for its readers (docs/08
		 * "Mirrored topics"). */
		for (int i = 0; i < B.nrx; i++) {
			if (B.rx[i].pub)
				hg_pub_close(B.rx[i].pub);
			B.rx[i].pub = NULL;
			B.rx[i].last_stamp = 0;
		}
	}
	reset_generation();
	mod_log(WHO, "link up with %s (peer id %08x)", B.peer_txt, B.peer_id);
}

/* ---- Clocks (docs/08 "Clocks") ------------------------------------------- */

/* Every time on the wire is the sender's CLOCK_MONOTONIC (plus the tests'
 * fake offset); the receiver alone translates (docs/08 "Clocks"). */
static uint64_t wire(uint64_t local_t)
{
	return local_t ? (uint64_t)((int64_t)local_t + B.fake_ns) : 0;
}

static uint64_t pclock(void)
{
	return wire(now_ns());
}

static void send_ping(void)
{
	unsigned char b[8];
	uint64_t t1 = pclock();
	put64(b, t1);
	B.pings[B.npings++ % 16] = t1;
	send_keyed(K_PING, BC_SAFETY, b, 8);
	B.last_ping_ns = now_ns();
}

static void estimate(void)
{
	uint64_t now = now_ns();
	int best = -1;
	uint64_t best_unc = UINT64_MAX;
	int k = 0;
	for (int i = 0; i < B.nsamp; i++) {
		if (now - B.samp[i].at > WINDOW_NS)
			continue;
		B.samp[k++] = B.samp[i];
	}
	B.nsamp = k;
	uint64_t rmin = UINT64_MAX;
	for (int i = 0; i < B.nsamp; i++) {
		uint64_t u = B.samp[i].rtt / 2 + (now - B.samp[i].at) / 2000; /* + age × 500 ppm */
		if (u < best_unc) {
			best_unc = u;
			best = i;
		}
		if (B.samp[i].rtt < rmin)
			rmin = B.samp[i].rtt;
	}
	if (best >= 0) {
		B.offset = B.samp[best].off;
		B.unc = best_unc;
		B.rtt_min = rmin;
		B.have_offset = 1;
	} else {
		/* Every sample older than the window: no estimate, and nothing
		 * translated until a pong gives one (docs/08 "Clocks"). */
		B.have_offset = 0;
		B.offset = 0;
		B.unc = 0;
	}
}

static void drain_records(void);

static void on_pong(const unsigned char *b, size_t n)
{
	if (n < 24)
		return;
	uint64_t t1 = get64(b), t2 = get64(b + 8), t3 = get64(b + 16), t4 = pclock();
	int mine = 0;
	for (int i = 0; i < 16 && i < B.npings; i++)
		if (B.pings[i] == t1)
			mine = 1;
	int64_t rtt = (int64_t)(t4 - t1) - (int64_t)(t3 - t2);
	if (!mine || rtt <= 0)
		return; /* a pong for no ping of ours, or a round trip that is not positive */
	int64_t off = ((int64_t)(t2 - t1) + (int64_t)(t3 - t4)) / 2;
	if (B.nsamp == (int)(sizeof B.samp / sizeof B.samp[0]))
		memmove(B.samp, B.samp + 1, sizeof B.samp - sizeof B.samp[0]), B.nsamp--;
	B.samp[B.nsamp++] = (struct sample){ .at = now_ns(), .off = off, .rtt = (uint64_t)rtt };
	B.rtt = (uint64_t)rtt;
	estimate();
	if (B.r_held)
		drain_records(); /* the messages that waited for this */
}

/* A time of the peer's to ours: the offset is theirs minus ours, both on the
 * wire. Only with an estimate of this link (B.have_offset): the callers hold
 * or drop what they cannot translate. */
static uint64_t to_local(uint64_t peer_t)
{
	if (!peer_t)
		return 0;
	int64_t t = (int64_t)peer_t - B.offset - B.fake_ns;
	return t > 0 ? (uint64_t)t : 1;
}

/* ---- The reliable stream --------------------------------------------------- */

static size_t s_used(void)
{
	return (size_t)(B.s_end - B.s_base);
}

/* Appends a record (u32 length, u8 type, body); -1 when the backlog is full. */
static int s_append(int type, const unsigned char *body, size_t n, int tx, uint64_t seq)
{
	if (s_used() + 5 + n > SBUF_CAP)
		return -1;
	unsigned char h[5];
	put32(h, (uint32_t)(1 + n));
	h[4] = (unsigned char)type;
	size_t at = s_used();
	memcpy(B.sbuf + at, h, 5);
	memcpy(B.sbuf + at + 5, body, n);
	B.s_end += 5 + n;
	if (tx >= 0) {
		if (B.nprec == B.cap_prec) {
			int cap = B.cap_prec ? B.cap_prec * 2 : 256;
			void *np = realloc(B.precs, (size_t)cap * sizeof *B.precs);
			if (!np)
				return 0; /* the ack of this one is not tracked: resent after a reconnect */
			B.precs = np;
			B.cap_prec = cap;
		}
		B.precs[B.nprec++] = (struct prec){ .end = B.s_end, .tx = tx, .seq = seq };
	}
	return 0;
}

static void s_pump(void)
{
	if (!B.up)
		return;
	while (B.s_next < B.s_end && B.s_next - B.s_base < WINDOW) {
		unsigned char b[BODY_MAX];
		size_t n = (size_t)(B.s_end - B.s_next);
		if (n > BODY_MAX - 8)
			n = BODY_MAX - 8;
		put64(b, B.s_next);
		memcpy(b + 8, B.sbuf + (B.s_next - B.s_base), n);
		send_keyed(K_DATA, BC_CALL, b, 8 + n);
		if (B.s_next == B.s_base)
			B.s_progress_ns = now_ns();
		B.s_next += n;
	}
}

static void s_resend_check(void)
{
	if (!B.up || B.s_next == B.s_base)
		return;
	uint64_t rto = B.rtt_min ? 4 * B.rtt_min : 50000000ull;
	if (rto < 20000000ull)
		rto = 20000000ull;
	if (rto > 1000000000ull)
		rto = 1000000000ull;
	if (now_ns() - B.s_progress_ns > rto) {
		B.st.retransmits += (B.s_next - B.s_base + BODY_MAX - 9) / (BODY_MAX - 8);
		B.s_next = B.s_base; /* go back: everything not acknowledged goes again */
		B.s_progress_ns = now_ns();
		s_pump();
	}
}

struct tx_topic;
static void held_acked(struct tx_topic *t);

static void on_ack(const unsigned char *b, size_t n)
{
	if (n < 8)
		return;
	uint64_t off = get64(b);
	if (off <= B.s_base || off > B.s_end)
		return;
	size_t drop = (size_t)(off - B.s_base);
	memmove(B.sbuf, B.sbuf + drop, s_used() - drop);
	B.s_base = off;
	if (B.s_next < B.s_base)
		B.s_next = B.s_base;
	B.s_progress_ns = now_ns();
	int k = 0;
	for (int i = 0; i < B.nprec; i++) {
		if (B.precs[i].end <= off) {
			struct tx_topic *t = &B.tx[B.precs[i].tx];
			if (B.precs[i].seq > t->acked_seq)
				t->acked_seq = B.precs[i].seq;
			held_acked(t);
		} else {
			B.precs[k++] = B.precs[i];
		}
	}
	B.nprec = k;
	s_pump();
}

static void handle_record(int type, const unsigned char *b, size_t n);

/* The whole records received so far, in order. A message record stops here
 * while the clock has no estimate: its stamp, and a request's deadline,
 * cannot be put into this clock yet. It waits, and what follows it on the
 * stream waits behind it, unacknowledged in the sender's backlog. */
static void drain_records(void)
{
	size_t at = 0;
	B.r_held = 0;
	while (B.rlen - at >= 5) {
		uint32_t rl = get32(recbuf + at);
		if (rl < 1) {
			at += 4; /* a record of nothing */
			continue;
		}
		if (rl - 1 > REC_MAX) {
			/* Larger than any record this side takes: passed over whole,
			 * by its length, so that the records after it are found
			 * where they start. */
			size_t have = B.rlen - at - 4;
			if (have >= rl) {
				at += 4 + rl;
				continue;
			}
			B.r_skip = rl - have;
			B.rlen = at;
			break;
		}
		if (B.rlen - at - 4 < rl)
			break;
		if (recbuf[at + 4] == R_MSG && !B.have_offset) {
			B.r_held = 1;
			break;
		}
		handle_record(recbuf[at + 4], recbuf + at + 5, rl - 1);
		at += 4 + rl;
	}
	memmove(recbuf, recbuf + at, B.rlen - at);
	B.rlen -= at;
}

static void on_data(const unsigned char *b, size_t n)
{
	if (n < 8)
		return;
	uint64_t off = get64(b);
	size_t len = n - 8;
	if (off == B.r_expect && !B.r_held && B.rlen + len <= sizeof recbuf) {
		const unsigned char *p = b + 8;
		B.r_expect += len;
		if (B.r_skip) {
			size_t k = len < B.r_skip ? len : (size_t)B.r_skip;
			B.r_skip -= k;
			p += k;
			len -= k;
		}
		memcpy(recbuf + B.rlen, p, len);
		B.rlen += len;
		drain_records();
	}
	unsigned char a[8];
	put64(a, B.r_expect);
	send_keyed(K_ACK, BC_CALL, a, 8);
}

/* ---- What is sent ---------------------------------------------------------- */

static void conf_of(const char *name, int up, int *reliable, int *prio, uint32_t *rate)
{
	const struct bc_mirror *m = bc_find(&B.conf, name, up);
	if (m) {
		*reliable = m->reliable;
		*prio = m->priority;
		*rate = m->rate_mhz;
	} else {
		/* The call topics [Calls] Remote= expands into (docs/08 "Calls"). */
		*reliable = 1;
		*prio = BC_CALL;
		*rate = 0;
	}
	if (is_call_topic(name)) {
		*reliable = 1;
		*prio = BC_CALL;
	}
}

static int add_tx(const char *name, hg_sub *s)
{
	if (B.ntx == MAX_TX)
		return -1;
	struct tx_topic *t = &B.tx[B.ntx];
	memset(t, 0, sizeof *t);
	snprintf(t->name, sizeof t->name, "%s", name);
	t->sub = s;
	conf_of(name, B.vehicle, &t->reliable, &t->priority, &t->rate_mhz);
	t->call_kind = is_call_topic(name);
	return B.ntx++;
}

/* Where a call envelope keeps its one time (docs/08 "Calls across the
 * tether"): a request's deadline, a reply's received time; 0 for none. */
static size_t call_time_at(int kind, uint32_t plen)
{
	if (kind == 1 && plen >= sizeof(struct hg_call_request_envelope))
		return offsetof(struct hg_call_request_envelope, deadline_ns);
	if (kind == 2 && plen >= sizeof(struct hg_call_reply_envelope))
		return offsetof(struct hg_call_reply_envelope, received_ns);
	return 0;
}

/* name len, name, seq, flags, stamp, publish time, refs, full and sent length,
 * the payload without its trailing zeros. */
static size_t build_msg(unsigned char *o, const struct tx_topic *t, const struct hg_info *in, const unsigned char *pl,
			uint32_t plen, uint32_t extra_flags)
{
	size_t nl = strlen(t->name), k = 0;
	o[k++] = (unsigned char)nl;
	memcpy(o + k, t->name, nl);
	k += nl;
	put64(o + k, in->seq);
	k += 8;
	put32(o + k, in->flags | extra_flags);
	k += 4;
	put64(o + k, wire(in->stamp_ns));
	k += 8;
	put64(o + k, wire(in->publish_ns));
	k += 8;
	o[k++] = (unsigned char)in->n_inputs;
	for (uint32_t i = 0; i < in->n_inputs && i < HG_MAX_REFS; i++) {
		put32(o + k, in->inputs[i].topic_id);
		put32(o + k + 4, in->inputs[i].flags);
		put64(o + k + 8, in->inputs[i].seq);
		k += 16;
	}
	uint32_t used = plen;
	while (used && pl[used - 1] == 0)
		used--;
	/* A call envelope's time goes whole, whatever follows it: an OK reply
	 * with an empty body ends inside received_ns, whose top bytes are zero
	 * for the first two years of an uptime. */
	size_t at = call_time_at(t->call_kind, plen);
	if (at && used < at + 8)
		used = (uint32_t)at + 8;
	put32(o + k, plen);
	put32(o + k + 4, used);
	k += 8;
	unsigned char *payload = o + k;
	memcpy(payload, pl, used);
	if (at)
		put64(payload + at, wire(get64(payload + at)));
	return k + used;
}

static void send_schema(struct tx_topic *t)
{
	struct hg_topic_meta m;
	uint32_t slen = 0;
	const char *schema = hg_sub_schema(t->sub, &slen);
	if (!schema || hg_sub_meta(t->sub, &m) < 0)
		return;
	unsigned char b[2 + 2 * 64 + 8 + HG_SCHEMA_MAX];
	size_t k = 0, nl = strlen(t->name), wl = strnlen(m.writer, 32);
	b[k++] = (unsigned char)nl;
	memcpy(b + k, t->name, nl);
	k += nl;
	b[k++] = (unsigned char)wl;
	memcpy(b + k, m.writer, wl);
	k += wl;
	put32(b + k, m.slot_count);
	k += 4;
	put32(b + k, slen);
	k += 4;
	memcpy(b + k, schema, slen);
	k += slen;
	if (s_append(R_SCHEMA, b, k, -1, 0) == 0)
		t->schema_sent = 1;
}

/* A topic of this machine, not one mirrored here from the other side. */
static int local_topic(hg_sub *s)
{
	struct hg_topic_meta m;
	if (hg_sub_meta(s, &m) < 0)
		return 0;
	for (int i = 0; i < 16; i++)
		if (m.origin_boot[i])
			return 0;
	return 1;
}

static unsigned char rd[MSG_MAX];
static unsigned char obuf[MSG_MAX + 600]; /* a message as sent */

/* The latest topics: every message read, each sent once as one datagram
 * (Rate= skips some, marked). */
static void pump_latest(struct tx_topic *t)
{
	struct hg_info in;
	for (;;) {
		enum hg_result r = hg_next(t->sub, rd, sizeof rd, &in);
		if (r != HG_MSG)
			break;
		if (!B.up || !B.mirror_ok || !local_topic(t->sub))
			continue; /* nothing to send it to: read and gone, as a lost one */
		uint64_t now = now_ns();
		if (t->rate_mhz && t->last_sent_ns &&
		    now - t->last_sent_ns < 1000000000000ull / t->rate_mhz) {
			t->decimated = 1;
			continue;
		}
		if (!t->schema_sent)
			send_schema(t);
		if (!t->schema_sent)
			continue;
		struct hg_topic_meta m;
		if (hg_sub_meta(t->sub, &m) < 0)
			continue;
		size_t n = build_msg(obuf, t, &in, rd, m.payload_bytes, t->decimated ? HG_SLOT_DECIMATED : 0);
		t->decimated = 0;
		if (n > BODY_MAX) {
			B.st.dropped_old++; /* stage 1: a latest message must fit one datagram */
			continue;
		}
		send_keyed(K_MSG, t->priority, obuf, n);
		t->last_sent_ns = now;
		B.st.msgs_tx++;
	}
	s_pump();
}

/* What the peer has acknowledged is held no longer. */
static void held_acked(struct tx_topic *t)
{
	int k = 0;
	while (k < t->nheld && t->held[k].seq <= t->acked_seq)
		free(t->held[k++].rec);
	if (k) {
		memmove(t->held, t->held + k, (size_t)(t->nheld - k) * sizeof *t->held);
		t->nheld -= k;
	}
}

/* Keeps a copy of a message's record until it is acknowledged. */
static void hold(struct tx_topic *t, uint64_t seq, const unsigned char *rec, size_t len)
{
	if (t->max_held == 0 || t->reopens != hg_sub_reopens(t->sub)) {
		struct hg_topic_meta m;
		t->max_held = hg_sub_meta(t->sub, &m) == 0 && m.slot_count ? m.slot_count : 16;
		t->reopens = hg_sub_reopens(t->sub);
	}
	unsigned char *copy = malloc(len ? len : 1);
	if (!copy)
		return; /* as a message the ring lost: a gap the peer sees */
	memcpy(copy, rec, len);
	if (t->nheld >= (int)t->max_held) {
		/* More unsent than the ring holds: the oldest goes, as it would
		 * have gone from the ring. The peer sees the gap in the seqs. */
		free(t->held[0].rec);
		memmove(t->held, t->held + 1, (size_t)(t->nheld - 1) * sizeof *t->held);
		t->nheld--;
	}
	if (t->nheld == t->cap_held) {
		int cap = t->cap_held ? t->cap_held * 2 : 16;
		struct held *nh = realloc(t->held, (size_t)cap * sizeof *nh);
		if (!nh) {
			free(copy);
			return;
		}
		t->held = nh;
		t->cap_held = cap;
	}
	t->held[t->nheld++] = (struct held){ .seq = seq, .len = (uint32_t)len, .rec = copy };
}

/* The reliable topics: every message read as it comes (nothing left unread
 * in the wait set) and held, as the record to send, until the peer
 * acknowledges it; what is held and not yet on the stream is queued in
 * order: after a reconnect, from the first the peer did not acknowledge;
 * while the backlog is full, from where it stopped. The copy is the
 * bridge's own because the ring is not its to keep: a writer that restarts
 * gets a new file, and the messages the bridge had read from the old one
 * and could not send yet (the link down, the backlog full) are in no file
 * any more. */
static void pump_reliable(struct tx_topic *t)
{
	struct hg_info in;
	while (hg_next(t->sub, rd, sizeof rd, &in) == HG_MSG) {
		if (!local_topic(t->sub))
			continue; /* mirrored here from the other side: not sent back */
		size_t n = build_msg(obuf, t, &in, rd, hg_sub_payload_bytes(t->sub), 0);
		hold(t, in.seq, obuf, n);
	}
	if (!B.up || !B.mirror_ok || !t->nheld || t->held[t->nheld - 1].queued)
		return;
	if (!t->schema_sent)
		send_schema(t);
	if (!t->schema_sent)
		return;
	for (int i = 0; i < t->nheld; i++) {
		struct held *h = &t->held[i];
		if (h->queued)
			continue;
		if (s_append(R_MSG, h->rec, h->len, (int)(t - B.tx), h->seq) < 0)
			break;
		h->queued = 1;
		B.st.msgs_tx++;
	}
	s_pump();
}

static void pump_tx(void)
{
	for (int g = 0; g < B.nglobs; g++) {
		hg_glob_poll(B.globs[g]);
		int k = hg_glob_count(B.globs[g]);
		for (int i = 0; i < k; i++) {
			hg_sub *s = hg_glob_at(B.globs[g], i);
			int have = 0;
			for (int j = 0; j < B.ntx; j++)
				if (B.tx[j].sub == s)
					have = 1;
			if (!have && local_topic(s))
				add_tx(hg_sub_name(s), s);
		}
	}
	for (int i = 0; i < B.ntx; i++) {
		if (B.tx[i].reliable)
			pump_reliable(&B.tx[i]);
		else
			pump_latest(&B.tx[i]);
	}
}

/* ---- What is received ------------------------------------------------------ */

static struct rx_topic *rx_find(const char *name, int create)
{
	for (int i = 0; i < B.nrx; i++)
		if (strcmp(B.rx[i].name, name) == 0)
			return &B.rx[i];
	if (!create || B.nrx == MAX_RX)
		return NULL;
	struct rx_topic *r = &B.rx[B.nrx++];
	memset(r, 0, sizeof *r);
	snprintf(r->name, sizeof r->name, "%s", name);
	r->mc = bc_find(&B.conf, name, !B.vehicle);
	return r;
}

static int take_name(const unsigned char **p, size_t *n, char *out, size_t cap)
{
	if (*n < 1)
		return -1;
	size_t l = (*p)[0];
	if (l == 0 || l >= cap || *n < 1 + l)
		return -1;
	memcpy(out, *p + 1, l);
	out[l] = 0;
	*p += 1 + l;
	*n -= 1 + l;
	return 0;
}

static void on_schema(const unsigned char *b, size_t n)
{
	char name[HG_NAME_MAX + 1], writer[33];
	if (take_name(&b, &n, name, sizeof name) < 0 || !hg_valid_name(name))
		return;
	if (n < 1)
		return;
	size_t wl = b[0];
	if (wl > 32 || n < 1 + wl + 8)
		return;
	memcpy(writer, b + 1, wl);
	writer[wl] = 0;
	b += 1 + wl;
	n -= 1 + wl;
	uint32_t slots = get32(b);
	b += 4;
	n -= 4;
	if (slots < 2 || slots > 4096 || (slots & (slots - 1)))
		slots = 16;
	uint32_t sl = get32(b);
	if (sl > HG_SCHEMA_MAX || n < 4 + sl)
		return;
	struct rx_topic *r = rx_find(name, 1);
	if (!r)
		return;
	if (r->schema && r->schema_len == sl && memcmp(r->schema, b + 4, sl) == 0 && !strcmp(r->writer, writer))
		return;
	struct hg_schema *sch = malloc(sizeof *sch);
	char *txt = malloc(sl + 1);
	if (!sch || !txt) {
		free(sch);
		free(txt);
		return;
	}
	memcpy(txt, b + 4, sl);
	txt[sl] = 0;
	char err[160];
	if (hg_schema_parse(txt, sl, sch, err, sizeof err) < 0) {
		mod_log(WHO, "%s: the peer's schema does not parse: %s", name, err);
		free(sch);
		free(txt);
		return;
	}
	if (r->pub) {
		hg_pub_close(r->pub); /* a new schema: the next message opens it again */
		r->pub = NULL;
	}
	free(r->sch);
	free(r->schema);
	r->sch = sch;
	r->schema = txt;
	r->schema_len = sl;
	r->slots = slots;
	r->refused = 0;
	snprintf(r->writer, sizeof r->writer, "%s", writer);
}

static void on_message(const unsigned char *b, size_t n)
{
	char name[HG_NAME_MAX + 1];
	if (!B.mirror_ok)
		return;
	if (take_name(&b, &n, name, sizeof name) < 0 || n < 8 + 4 + 8 + 8 + 1)
		return;
	uint64_t seq = get64(b), stamp = get64(b + 12), pubt = get64(b + 20);
	uint32_t flags = get32(b + 8);
	int nref = b[28];
	b += 29;
	n -= 29;
	if (nref > HG_MAX_REFS || n < (size_t)nref * 16 + 8)
		return;
	struct hg_input refs[HG_MAX_REFS];
	for (int i = 0; i < nref; i++) {
		refs[i].topic_id = get32(b);
		refs[i].flags = get32(b + 4);
		refs[i].seq = get64(b + 8);
		b += 16;
		n -= 16;
	}
	uint32_t full = get32(b), used = get32(b + 4);
	b += 8;
	n -= 8;
	if (used > full || full > MSG_MAX || n < used)
		return;
	struct rx_topic *r = rx_find(name, 0);
	if (!r || !r->sch || r->refused)
		return; /* no schema yet: the first messages of a latest topic can come before it */
	if (full != r->sch->type.payload_bytes)
		return;
	if (!r->pub) {
		uint64_t slot = (uint64_t)full + SLOT_EXTRA;
		uint32_t slots = r->slots;
		while (slots > 2 && slots * slot > B.ring_max)
			slots >>= 1;
		if (slots != r->slots)
			mod_log(WHO, "%s: %u slots of %u bytes there; %u here, the most that fit %llu KB", name, r->slots, full,
				slots, (unsigned long long)(B.ring_max >> 10));
		if (B.rings - r->ring_bytes + slots * slot > RINGS_MAX) {
			mod_log(WHO, "%s: not mirrored here: the mirrored topics' rings would pass %llu MB", name,
				(unsigned long long)(RINGS_MAX >> 20));
			r->refused = 1;
			return;
		}
		struct hg_pub_opts o = { .slots = slots };
		r->pub = hg_pub_open_mirror_as(name, &r->sch->type, B.peer_boot, seq, r->writer[0] ? r->writer : NULL,
					       &o);
		if (!r->pub) {
			mod_log(WHO, "%s: not opened (%s): not mirrored here", name, strerror(errno));
			r->refused = 1;
			return;
		}
		/* The file of before, if any, is replaced by this one. */
		B.rings = B.rings - r->ring_bytes + slots * slot;
		r->ring_bytes = slots * slot;
		B.st.topics_rx++;
	}
	uint64_t now = now_ns();
	uint64_t ls = to_local(stamp), lp = to_local(pubt);
	if (r->mc && r->mc->max_age_ns) {
		uint64_t at = ls ? ls : lp;
		if (at && now > at && now - at + B.unc > r->mc->max_age_ns) {
			B.st.dropped_late++;
			return;
		}
	}
	if (ls && ls <= r->last_stamp) {
		ls = r->last_stamp + 1;
		B.st.clamped++;
	}
	memset(msgbuf, 0, full);
	memcpy(msgbuf, b, used);
	/* A request's deadline into this clock, shortened by the clock's
	 * uncertainty; a reply's received time into this clock. Decided on the
	 * payload's full length, not on what was sent of it: the sender leaves
	 * trailing zeros out, and the time may be the last thing that is not. */
	int kind = is_call_topic(name);
	size_t at = call_time_at(kind, full);
	if (at) {
		uint64_t v = to_local(get64(msgbuf + at));
		if (kind == 1)
			v = v > B.unc ? v - B.unc : 1;
		put64(msgbuf + at, v);
	}
	if (hg_publish_seq(r->pub, seq, flags, msgbuf, full, ls, refs, nref) < 0) {
		if (errno == ERANGE)
			B.st.dropped_old++;
		return;
	}
	if (ls)
		r->last_stamp = ls;
	B.st.msgs_rx++;
}

static void send_info(void)
{
	unsigned char b[32 * 3];
	memset(b, 0, sizeof b);
	snprintf((char *)b, 32, "%s", HANGAR_VERSION);
	memcpy(b + 32, msg_hash, 32);
	memcpy(b + 64, B.conf.hash, 32);
	s_append(R_INFO, b, sizeof b, -1, 0);
	B.info_sent = 1;
	/* tests: once, a record larger than any the other side takes, of a type
	 * it does not know. It must pass over it and keep its place. */
	static int big_sent;
	if (!big_sent && getenv("HANGAR_BRIDGE_TEST_BIG_RECORD")) {
		size_t n = MSG_MAX + 4096;
		unsigned char *z = calloc(1, n);
		if (z)
			s_append(99, z, n, -1, 0);
		free(z);
		big_sent = 1;
	}
}

static void on_info(const unsigned char *b, size_t n)
{
	if (n < 96)
		return;
	char ver[33];
	memcpy(ver, b, 32);
	ver[32] = 0;
	B.peer_info = 1;
	B.config_match = 1;
	B.mirror_ok = 1;
	B.detail[0] = 0;
	if (strcmp(ver, HANGAR_VERSION) != 0 || memcmp(b + 32, msg_hash, 32) != 0) {
		/* No compatibility mode between versions (docs/13): nothing mirrored. */
		B.config_match = 0;
		B.mirror_ok = 0;
		snprintf(B.detail, sizeof B.detail, "%s: release %s here, %s there",
			 memcmp(b + 32, msg_hash, 32) ? "msg/ trees differ" : "releases differ", HANGAR_VERSION, ver);
	} else if (memcmp(b + 64, B.conf.hash, 32) != 0) {
		B.config_match = 0;
		B.mirror_ok = 0;
		snprintf(B.detail, sizeof B.detail, "bridge.conf differs between the two sides");
	}
	mod_log(WHO, "the peer: release %s, %s%s%s", ver, B.config_match ? "configurations match" : "config_match false",
		B.detail[0] ? ": " : "", B.detail);
}

static void handle_record(int type, const unsigned char *b, size_t n)
{
	if (type == R_INFO)
		on_info(b, n);
	else if (type == R_SCHEMA)
		on_schema(b, n);
	else if (type == R_MSG)
		on_message(b, n);
}

/* A new link: new streams, the info exchange again, and every reliable topic
 * from what the peer last acknowledged. */
static void reset_generation(void)
{
	B.s_base = B.s_end = B.s_next = 0;
	B.nprec = 0;
	B.r_expect = 0;
	B.rlen = 0;
	B.r_skip = 0;
	B.r_held = 0;
	B.info_sent = B.peer_info = B.mirror_ok = 0;
	B.config_match = 0;
	for (int i = 0; i < B.ntx; i++) {
		struct tx_topic *t = &B.tx[i];
		t->schema_sent = 0;
		for (int k = 0; k < t->nheld; k++)
			t->held[k].queued = 0; /* not acknowledged on the old stream: sent again */
	}
	for (int i = 0; i < B.nrx; i++)
		B.rx[i].refused = 0;
	B.up = 1;
	B.last_rx_ns = now_ns();
	send_info();
	s_pump();
	send_ping(); /* the first estimate of this link's clock, a round trip from now */
}

/* ---- Receiving datagrams --------------------------------------------------- */

/* The peer is where a datagram came from whose tag checked under a session
 * key, and nothing else moves it (Peer=auto; a fixed peer stays). */
static void peer_is(const struct sockaddr_in *from)
{
	if (B.peer_fixed)
		return;
	if (B.have_peer && B.peer.sin_addr.s_addr == from->sin_addr.s_addr && B.peer.sin_port == from->sin_port &&
	    B.peer_txt[0])
		return;
	B.peer = *from;
	B.have_peer = 1;
	snprintf(B.peer_txt, sizeof B.peer_txt, "%s:%u", inet_ntoa(B.peer.sin_addr), ntohs(B.peer.sin_port));
}

static int replay_ok(uint64_t seq)
{
	if (seq == 0)
		return 0;
	if (seq > B.rx_hi) {
		uint64_t d = seq - B.rx_hi;
		B.rx_win = d >= 64 ? 0 : B.rx_win << d;
		B.rx_win |= 1;
		B.rx_hi = seq;
		return 1;
	}
	uint64_t d = B.rx_hi - seq;
	if (d >= 64 || (B.rx_win >> d) & 1)
		return 0;
	B.rx_win |= 1ull << d;
	return 1;
}

static void on_dgram(const unsigned char *d, size_t len, const struct sockaddr_in *from)
{
	if (len < HDR + TAG || memcmp(d, "HGB1", 4) != 0) {
		B.st.rejected++;
		return;
	}
	uint32_t blen = get32(d + 12);
	if (blen > BODY_MAX || len != HDR + blen + TAG) {
		B.st.rejected++;
		return;
	}
	int kind = d[4];
	unsigned char t[TAG];
	const unsigned char *body = d + HDR;
	if (kind == K_HELLO) {
		tag_of(B.key, d, HDR + blen, t);
		if (!tag_eq(t, d + HDR + blen, TAG)) {
			B.st.rejected++;
			return;
		}
		on_hello(body, blen, from);
		return;
	}
	int under_pending = 0;
	if (B.keyed) {
		tag_of(B.k_rx, d, HDR + blen, t);
		if (!tag_eq(t, d + HDR + blen, TAG))
			under_pending = 1;
	} else {
		under_pending = 1;
	}
	if (under_pending) {
		if (!B.pending) {
			B.st.rejected++;
			return;
		}
		tag_of(B.p_rx, d, HDR + blen, t);
		if (!tag_eq(t, d + HDR + blen, TAG)) {
			B.st.rejected++; /* a wrong key, a changed byte, our own datagram sent back */
			return;
		}
		if (get64(d + 16) == 0) {
			B.st.rejected++; /* no sequence number: checked before anything changes */
			return;
		}
		peer_is(from);
		promote(); /* the peer holds the new keys: the link is up on them */
	}
	if (!replay_ok(get64(d + 16))) {
		B.st.rejected++;
		return;
	}
	B.rx_count++;
	B.st.loss_rx = B.rx_hi > B.rx_count ? B.rx_hi - B.rx_count : 0;
	B.last_rx_ns = now_ns();
	if (!B.up) {
		B.up = 1;
		mod_log(WHO, "link back up with %s", B.peer_txt);
	}
	peer_is(from);
	switch (kind) {
	case K_CONFIRM:
		break;
	case K_PING: {
		if (blen < 8)
			break;
		unsigned char b[24];
		uint64_t t2 = pclock();
		memcpy(b, body, 8);
		put64(b + 8, t2);
		put64(b + 16, pclock());
		send_keyed(K_PONG, BC_SAFETY, b, 24);
		break;
	}
	case K_PONG:
		on_pong(body, blen);
		break;
	case K_DATA:
		on_data(body, blen);
		break;
	case K_ACK:
		on_ack(body, blen);
		break;
	case K_MSG:
		/* No estimate of the peer's clock: a stamp cannot be translated
		 * nor an age judged, and a latest message is not worth keeping
		 * for later. Dropped, as one too old is. */
		if (!B.have_offset)
			B.st.dropped_late++;
		else
			on_message(body, blen);
		break;
	default:
		B.st.rejected++;
	}
}

/* ---- Discovery (docs/08 "Discovery") --------------------------------------- */

struct cand {
	uint32_t id;
	struct sockaddr_in addr;
	uint64_t heard;
};
static struct cand cands[8];
static int ncands;

static void beacon(void)
{
	char line[200], tagx[40];
	int n = snprintf(line, sizeof line, "HANGAR/1 role=%s pair=%s port=%u id=%08x", B.role, B.conf.pair,
			 B.conf.port, B.id);
	unsigned char tg[16];
	blake2s(tg, 16, B.key, 32, line, (size_t)n);
	for (int i = 0; i < 16; i++)
		snprintf(tagx + 2 * i, 3, "%02x", tg[i]);
	n += snprintf(line + n, sizeof line - (size_t)n, " tag=%s", tagx);
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(B.conf.discovery_port) };
	to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	(void)sendto(B.dsock, line, (size_t)n, MSG_DONTWAIT, (struct sockaddr *)&to, sizeof to);
}

static void on_beacon(const char *line, size_t n, const struct sockaddr_in *from)
{
	char buf[256], role[16], pair[64], tagx[40];
	unsigned port;
	unsigned id;
	if (n >= sizeof buf)
		return;
	memcpy(buf, line, n);
	buf[n] = 0;
	char *t = strstr(buf, " tag=");
	if (!t || sscanf(buf, "HANGAR/1 role=%15s pair=%63s port=%u id=%x", role, pair, &port, &id) != 4 ||
	    sscanf(t, " tag=%32s", tagx) != 1)
		return;
	unsigned char tg[16];
	blake2s(tg, 16, B.key, 32, buf, (size_t)(t - buf));
	char want[40];
	for (int i = 0; i < 16; i++)
		snprintf(want + 2 * i, 3, "%02x", tg[i]);
	if (strlen(tagx) != 32 || !tag_eq((const unsigned char *)want, (const unsigned char *)tagx, 32) ||
	    strcmp(pair, B.conf.pair) != 0 || strcmp(role, B.role) == 0 || id == B.id)
		return;
	struct sockaddr_in a = *from;
	a.sin_port = htons((uint16_t)port);
	for (int i = 0; i < ncands; i++)
		if (cands[i].id == id) {
			cands[i].addr = a;
			cands[i].heard = now_ns();
			return;
		}
	if (ncands < 8)
		cands[ncands++] = (struct cand){ .id = id, .addr = a, .heard = now_ns() };
}

/* Peer=auto: the one candidate of the opposite role heard in the last 5 s. */
static void choose_peer(void)
{
	if (B.peer_fixed || B.up)
		return;
	int live = 0, which = -1;
	for (int i = 0; i < ncands; i++)
		if (now_ns() - cands[i].heard < 5000000000ull) {
			live++;
			which = i;
		}
	if (live == 1) {
		B.peer = cands[which].addr;
		B.have_peer = 1;
		snprintf(B.peer_txt, sizeof B.peer_txt, "%s:%u", inet_ntoa(B.peer.sin_addr), ntohs(B.peer.sin_port));
	} else if (live > 1) {
		B.have_peer = 0;
	}
	B.st.state = live > 1 ? ST_AMBIGUOUS : live ? ST_CONNECTING : ST_SEARCHING;
}

/* ---- The status topic ------------------------------------------------------ */

static void publish_status(void)
{
	struct bridge_link *s = &B.st;
	if (B.up)
		s->state = ST_UP;
	else if (B.peer_fixed || B.have_peer)
		s->state = ST_CONNECTING;
	s->config_match = (uint8_t)(B.up && B.peer_info && B.config_match);
	s->peer_role = B.keyed ? (uint8_t)(B.vehicle ? 2 : 1) : 0;
	s->peer_id = B.peer_id;
	memcpy(s->peer_boot_id, B.peer_boot, 16);
	snprintf(s->peer_addr, sizeof s->peer_addr, "%s", B.peer_txt);
	s->last_rx_ns = B.last_rx_ns;
	s->rtt_ns = B.rtt;
	s->rtt_min_ns = B.rtt_min;
	s->clock_offset_ns = B.offset;
	s->clock_uncertainty_ns = B.have_offset ? B.unc : 0;
	snprintf(s->detail, sizeof s->detail, "%s",
		 B.detail[0] ? B.detail :
		 B.up && !B.peer_info ? "exchanging versions" :
		 B.up && !B.have_offset ? "no clock estimate: nothing mirrored in" : "");
	hg_publish(B.link_pub, s, sizeof *s, now_ns());
	static char last[160];
	char line[160];
	static const char *names[] = { "off", "searching", "connecting", "up", "ambiguous", "refused" };
	if (B.up && B.peer_info && !s->config_match)
		mod_fmt(line, sizeof line, "%s %s, config_match false: %s", names[s->state], B.peer_txt, B.detail);
	else
		mod_fmt(line, sizeof line, "%s %s, rtt %.1f ms, offset %+.3f ms, %u topics in, %llu msgs out, %llu in",
			names[s->state], B.peer_txt[0] ? B.peer_txt : "-", (double)B.rtt / 1e6, (double)B.offset / 1e6,
			s->topics_rx, (unsigned long long)s->msgs_tx, (unsigned long long)s->msgs_rx);
	if (strcmp(line, last) != 0) {
		hg_status(line);
		snprintf(last, sizeof last, "%s", line);
	}
	hg_alive();
}

/* ---- Start-up and the loop ------------------------------------------------- */

static void hash_msg_dir(const char *dir)
{
	struct dirent **list = NULL;
	int n = scandir(dir, &list, NULL, alphasort);
	struct blake2s h;
	blake2s_init(&h, 32, NULL, 0);
	for (int i = 0; i < n; i++) {
		const char *nm = list[i]->d_name;
		size_t l = strlen(nm);
		if (l > 4 && strcmp(nm + l - 4, ".msg") == 0) {
			char path[1024];
			snprintf(path, sizeof path, "%s/%s", dir, nm);
			FILE *f = fopen(path, "rb");
			if (f) {
				blake2s_update(&h, nm, l + 1);
				unsigned char b[4096];
				size_t r;
				while ((r = fread(b, 1, sizeof b, f)) > 0)
					blake2s_update(&h, b, r);
				fclose(f);
			}
		}
		free(list[i]);
	}
	free(list);
	if (n < 0)
		mod_log(WHO, "cannot read MsgDir %s: %s; the msg/ hash is of nothing", dir, strerror(errno));
	blake2s_final(&h, msg_hash);
}

static int parse_peer(const char *txt, struct sockaddr_in *out)
{
	char host[64];
	unsigned port = B.conf.port;
	const char *colon = strrchr(txt, ':');
	if (colon) {
		snprintf(host, sizeof host, "%.*s", (int)(colon - txt), txt);
		port = (unsigned)atoi(colon + 1);
	} else {
		snprintf(host, sizeof host, "%s", txt);
	}
	memset(out, 0, sizeof *out);
	out->sin_family = AF_INET;
	out->sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &out->sin_addr) == 1)
		return 0;
	struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res;
	if (getaddrinfo(host, NULL, &hints, &res) != 0)
		return -1;
	out->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
	freeaddrinfo(res);
	return 0;
}

static int udp_socket(uint16_t port, int broadcast)
{
	int s = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (s < 0)
		return -1;
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (broadcast) {
		setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
		setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
	}
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
		close(s);
		return -1;
	}
	return s;
}

static void ep_add(int fd)
{
	struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
	epoll_ctl(B.ep, EPOLL_CTL_ADD, fd, &ev);
}

int main(int argc, char **argv)
{
	mod_init(WHO);
	if (argc < 2) {
		mod_log(WHO, "usage: hangar-bridge <bridge.conf>");
		return 2;
	}
	char err[300];
	if (bc_load(argv[1], &B.conf, err, sizeof err) < 0) {
		mod_log(WHO, "%s", err);
		hg_status(err);
		return 78;
	}
	if (bc_load_key(B.conf.key_path, B.key, err, sizeof err) < 0) {
		mod_log(WHO, "the key: %s", err);
		hg_status("no key: see the log");
		return 78;
	}
	const struct hg_wiring *w = hg_wiring();
	snprintf(B.role, sizeof B.role, "%s", w && w->role[0] ? w->role : "vehicle");
	B.vehicle = strcmp(B.role, "vehicle") == 0;
	const char *fo = getenv("HANGAR_BRIDGE_TEST_OFFSET_NS");
	B.fake_ns = fo ? strtoll(fo, NULL, 10) : 0;
	const char *rm = getenv("HANGAR_BRIDGE_TEST_RING_MAX"); /* tests: a bound a test can reach */
	B.ring_max = rm ? strtoull(rm, NULL, 10) : RING_MAX;
	random_bytes(&B.id, sizeof B.id);
	new_nonce();
	read_boot_id(B.boot);
	hash_msg_dir(B.conf.msg_dir);
	B.sbuf = malloc(SBUF_CAP);
	if (!B.sbuf)
		return 3;

	/* The topics this side sends: the wiring's sub and subglob lines. */
	hg_sub *named[HG_MAX_WAIT_FD];
	int nnamed = 0;
	for (int i = 0; w && i < w->n_subs; i++) {
		int rel, prio;
		uint32_t rate;
		conf_of(w->subs[i].topic, B.vehicle, &rel, &prio, &rate);
		hg_sub *s = hg_sub_open_from(w->subs[i].topic, NULL, rel ? HG_OLDEST : HG_NEWEST);
		if (!s) {
			mod_log(WHO, "%s: cannot subscribe: %s", w->subs[i].topic, strerror(errno));
			continue;
		}
		if (add_tx(w->subs[i].topic, s) >= 0 && nnamed < HG_MAX_WAIT_FD)
			named[nnamed++] = s;
	}
	for (int i = 0; w && i < w->n_globs && B.nglobs < HG_WIRING_MAX_GLOBS; i++) {
		hg_sub *g = hg_sub_open_from(w->globs[i].pattern, NULL, HG_NEWEST);
		if (g)
			B.globs[B.nglobs++] = g;
	}
	char link[48];
	snprintf(link, sizeof link, "bridge.%s.link", B.role);
	B.link_pub = hg_pub_open(link, &BRIDGE_LINK_TYPE, NULL);
	if (!B.link_pub) {
		mod_log(WHO, "%s: %s", link, strerror(errno));
		return 3;
	}

	B.sock = udp_socket(B.conf.port, 0);
	if (B.sock < 0) {
		snprintf(err, sizeof err, "cannot bind UDP %u: %s", B.conf.port, strerror(errno));
		mod_log(WHO, "%s", err);
		hg_status(err);
		return 3;
	}
	B.dsock = -1;
	if (strcmp(B.conf.peer, "auto") == 0) {
		B.dsock = udp_socket(B.conf.discovery_port, 1);
		if (B.dsock < 0)
			mod_log(WHO, "discovery on UDP %u: %s", B.conf.discovery_port, strerror(errno));
		B.st.state = ST_SEARCHING;
	} else {
		if (parse_peer(B.conf.peer, &B.peer) < 0) {
			mod_log(WHO, "Peer=%s: not an address", B.conf.peer);
			return 78;
		}
		B.have_peer = B.peer_fixed = 1;
		snprintf(B.peer_txt, sizeof B.peer_txt, "%s:%u", inet_ntoa(B.peer.sin_addr), ntohs(B.peer.sin_port));
		B.st.state = ST_CONNECTING;
	}
	B.ep = epoll_create1(EPOLL_CLOEXEC);
	B.evfd = nnamed ? hg_wait_fd(named, nnamed) : -1;
	B.tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	struct itimerspec its = { .it_interval = { 0, 10000000 }, .it_value = { 0, 10000000 } };
	timerfd_settime(B.tfd, 0, &its, NULL);
	ep_add(B.sock);
	ep_add(B.tfd);
	if (B.evfd >= 0)
		ep_add(B.evfd);
	if (B.dsock >= 0)
		ep_add(B.dsock);
	mod_log(WHO, "%s, UDP %u, %s, %d topics and %d globs to send, %d mirror lines in", B.role, B.conf.port,
		B.peer_fixed ? B.peer_txt : "Peer=auto", B.ntx, B.nglobs, w ? w->n_mirrors : 0);
	publish_status();

	uint64_t last_beacon = 0;
	while (!mod_stop) {
		struct epoll_event evs[8];
		int n = epoll_wait(B.ep, evs, 8, 1000);
		if (n < 0 && errno != EINTR)
			break;
		int tick = 0, topics = 0;
		for (int i = 0; i < n; i++) {
			int fd = evs[i].data.fd;
			if (fd == B.sock) {
				for (;;) {
					unsigned char d[DGRAM_MAX + 16];
					struct sockaddr_in from;
					socklen_t fl = sizeof from;
					ssize_t r = recvfrom(B.sock, d, sizeof d, 0, (struct sockaddr *)&from, &fl);
					if (r < 0)
						break;
					on_dgram(d, (size_t)r, &from);
				}
			} else if (fd == B.dsock) {
				for (;;) {
					char d[256];
					struct sockaddr_in from;
					socklen_t fl = sizeof from;
					ssize_t r = recvfrom(B.dsock, d, sizeof d, 0, (struct sockaddr *)&from, &fl);
					if (r < 0)
						break;
					on_beacon(d, (size_t)r, &from);
				}
			} else if (fd == B.evfd) {
				uint64_t v;
				if (read(B.evfd, &v, sizeof v) < 0)
					(void)0;
				topics = 1;
			} else if (fd == B.tfd) {
				uint64_t v;
				if (read(B.tfd, &v, sizeof v) < 0)
					(void)0;
				tick = 1;
			}
		}
		if (topics || tick)
			pump_tx();
		if (!tick)
			continue;
		uint64_t now = now_ns();
		if (B.dsock >= 0 && now - last_beacon >= 1000000000ull) {
			beacon();
			last_beacon = now;
		}
		choose_peer();
		if (B.up && now - B.last_rx_ns > LINK_TIMEOUT_NS) {
			B.up = 0;
			mod_log(WHO, "link down: nothing heard for %.1f s", (double)(now - B.last_rx_ns) / 1e9);
		}
		if (B.nonce_since && now - B.nonce_since > NONCE_NS)
			new_nonce();
		if (!B.up && B.have_peer && now - B.last_hello_ns >= (B.pending ? HELLO_NS : 1000000000ull)) {
			send_hello(B.pending ? B.p_nonce : NULL, NULL);
			if (B.pending)
				send_confirm(B.p_tx, NULL);
			else if (B.keyed)
				send_keyed(K_CONFIRM, BC_SAFETY, "", 0);
		}
		if (B.up && now - B.last_ping_ns >= PING_NS)
			send_ping();
		if (B.have_offset && B.nsamp && now - B.samp[B.nsamp - 1].at > PING_NS * 5)
			estimate(); /* no pong for a while: the estimate ages, and ends with its window */
		s_resend_check();
		if (now - B.last_status_ns >= STATUS_NS) {
			B.last_status_ns = now;
			publish_status();
		}
	}
	mod_log(WHO, "stopping");
	return 0;
}
