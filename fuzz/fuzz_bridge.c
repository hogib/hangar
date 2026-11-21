// SPDX-License-Identifier: GPL-3.0-or-later
/* The tether bridge's wire protocol (modules/bridge/bridge.c): everything the
 * bridge does with a datagram from its UDP socket and a line from its
 * discovery socket. This is network input: on_dgram (the header, the tag, the
 * replay window), on_hello and the handshake's state, on_pong and the clock
 * estimate, on_data and the reliable stream's reassembly (drain_records), the
 * records on it (on_info, on_schema, on_message, and through them
 * hg_schema_parse and the mirrored topic's file), on_ack, on_beacon; and the
 * files (docs/08 "Files"): the records of a file arriving (on_fbegin,
 * on_fdata, on_fend, on_fabort) and, for a file being sent, the status the
 * other side answers with (on_fstatus) and bridge.send itself (on_send).
 * Whatever the peer names, nothing may appear on disk but under the
 * directory of a [Files] section of this side, under a name that could be a
 * file's, or the hidden name of a transfer under way (check_files).
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

static char dir[256], state[300];
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

/* The bridge's state directory: outbox/ and files/<dest>/, nothing else, and
 * in a destination's directory only names a file may have and the hidden
 * names of transfers. With wipe, emptied. */
static void check_files(int wipe)
{
	static const char *top[] = { "outbox", "files", NULL }, *dests[] = { "w", "logs", NULL };
	char p[700], q[1100];
	struct dirent *e;
	DIR *d = opendir(state);
	while (d && (e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		int known = 0;
		for (int i = 0; top[i]; i++)
			known |= !strcmp(e->d_name, top[i]);
		FZ_CHECK(known);
	}
	if (d)
		closedir(d);
	snprintf(p, sizeof p, "%s/files", state);
	d = opendir(p);
	while (d && (e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		int known = 0;
		for (int i = 0; dests[i]; i++)
			known |= !strcmp(e->d_name, dests[i]);
		FZ_CHECK(known);
	}
	if (d)
		closedir(d);
	for (int i = 0; dests[i]; i++) {
		int visible = 0;
		snprintf(p, sizeof p, "%s/files/%s", state, dests[i]);
		d = opendir(p);
		while (d && (e = readdir(d))) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			int part = strncmp(e->d_name, ".part-", 6) == 0 && strlen(e->d_name) == 22;
			FZ_CHECK(part || bc_file_name_ok(e->d_name));
			visible += !part;
			struct stat st;
			snprintf(q, sizeof q, "%s/%s", p, e->d_name);
			FZ_CHECK(lstat(q, &st) == 0 && S_ISREG(st.st_mode) && st.st_size <= (64 << 10));
			if (wipe)
				unlink(q);
		}
		if (d)
			closedir(d);
		FZ_CHECK(visible <= 2); /* MaxFiles=2 */
	}
	snprintf(p, sizeof p, "%s/outbox", state);
	d = wipe ? opendir(p) : NULL;
	while (d && (e = readdir(d))) {
		snprintf(q, sizeof q, "%s/%s", p, e->d_name);
		unlink(q);
	}
	if (d)
		closedir(d);
}

static void cleanup(void)
{
	static const char *sub[] = { "files/w", "files/logs", "files", "outbox", "", NULL };
	char p[700];
	files_reset();
	check_files(1);
	for (int i = 0; sub[i]; i++) {
		snprintf(p, sizeof p, "%s/%s", state, sub[i]);
		rmdir(p);
	}
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
	/* The state directory beside the topics, not in them: empty_dir()
	 * unlinks what the topic directory holds. */
	snprintf(state, sizeof state, "%s-state", dir);
	setenv("HANGAR_STATE_DIR", state, 1);
	unsetenv("HANGAR_BRIDGE_TEST_FILE_CORRUPT");
	unsetenv("HANGAR_BRIDGE_TEST_FILE_STALL_MS");
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
	files_reset();
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
	files_init();
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
	hello_tick(now_ns());
	if (B.have_offset && B.nsamp)
		estimate();
	s_resend_check();
	s_pump();
	F.tokens = 1e9; /* FileRate= accrues with time, and an input takes none */
	files_tick();
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
	FZ_CHECK(F.nq >= 0 && F.nq <= FILE_QUEUE);
	FZ_CHECK(F.ndone >= 0 && F.ndone <= 4);
	FZ_CHECK(!F.in.active || (F.in.fd >= 0 && F.in.dirfd >= 0 && F.in.fc && F.in.have <= F.in.size &&
				  F.in.size <= F.in.fc->max_size && bc_file_name_ok(F.in.name)));
	for (int i = 0; i < F.nq; i++)
		FZ_CHECK(F.q[i].fd >= 0 && F.q[i].pos <= F.q[i].size && F.q[i].hashed <= F.q[i].size &&
			 F.q[i].pos <= F.q[i].hashed && F.q[i].size <= F.q[i].fc->max_size);
}

/* A beacon, tagged or not, decides nothing (docs/08 "Discovery"): not the
 * peer, not its address, not the link's keys or whether it is up. */
static void a_beacon(const char *line, size_t n)
{
	int have_peer = B.have_peer, up = B.up, keyed = B.keyed, pending = B.pending;
	struct sockaddr_in peer = B.peer;
	uint32_t link_id = B.link_id;
	on_beacon(line, n, &from);
	FZ_CHECK(B.have_peer == have_peer && B.up == up && B.keyed == keyed && B.pending == pending);
	FZ_CHECK(B.link_id == link_id && memcmp(&B.peer, &peer, sizeof peer) == 0);
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
			a_beacon((const char *)p, len);
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
			a_beacon(line, len + (size_t)n);
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
		case FZB_SEND: {
			/* An operator's bridge.send, as hg_serve would hand it over. */
			struct bridge_send_req rq;
			struct bridge_send_rep rp;
			char reason[HG_CALL_REASON] = "", path[700];
			hg_call_ctx c = { .caller = "admin:fuzz", .method = "send" };
			if (len < 1)
				break;
			memset(&rq, 0, sizeof rq);
			memset(&rp, 0, sizeof rp);
			snprintf(rq.dest, sizeof rq.dest, "%s", p[0] & 1 ? "logs" : "w");
			size_t at = 1;
			if (p[0] & 2) {
				size_t nl = len - 1 < 8 ? len - 1 : 8;
				memcpy(rq.name, p + 1, nl);
				at += nl;
			} else {
				snprintf(rq.name, sizeof rq.name, "f.bin");
			}
			char name[sizeof rq.name + 1];
			memcpy(name, rq.name, sizeof rq.name);
			name[sizeof rq.name] = 0;
			if (bc_file_name_ok(name)) {
				snprintf(path, sizeof path, "%s/outbox/%s", state, name);
				int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
				if (fd >= 0) {
					if (write(fd, p + at, len - at) < 0)
						(void)0;
					close(fd);
				}
			}
			int nq = F.nq;
			int rc = on_send(&c, &rq, &rp, reason);
			FZ_CHECK(rc == HG_OK ? F.nq == nq + 1 && rp.id != 0 : F.nq == nq && reason[0]);
			break;
		}
		case FZB_FSTATUS: {
			/* What the other side answers about the file being sent: its id is
			 * drawn here, so the input cannot name it. */
			unsigned char rec[5 + 18 + HG_CALL_REASON], b[BODY_MAX];
			if (len < 9 || !F.nq)
				break;
			size_t rl = len - 9 < HG_CALL_REASON - 1 ? len - 9 : HG_CALL_REASON - 1;
			put32(rec, (uint32_t)(1 + 18 + rl));
			rec[4] = R_FSTATUS;
			put64(rec + 5, F.q[0].id);
			rec[13] = p[0];
			memcpy(rec + 14, p + 1, 8);
			rec[22] = (unsigned char)rl;
			memcpy(rec + 23, p + 9, rl);
			put64(b, B.r_expect);
			memcpy(b + 8, rec, 23 + rl);
			sealed(K_DATA, BC_CALL, b, 8 + 23 + rl, key_now(K_DATA));
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
		"%d candidates, stream at %llu; files: %d made visible, one %s, %d to send",
		B.keyed, B.up, B.peer_info, B.mirror_ok, B.have_offset, (unsigned long long)B.st.rejected, B.st.topics_rx,
		(unsigned long long)B.st.msgs_rx, (unsigned long long)B.st.dropped_late, ncands,
		(unsigned long long)B.r_expect, F.ndone, F.in.active ? "arriving" : "not arriving", F.nq);
	check_files(0);
	files_reset();
	check_files(1);
	reset(0);
	empty_dir(0);
	return 0;
}
