// SPDX-License-Identifier: GPL-3.0-or-later
/* What fuzz_bridge.c and the seed generator agree on: the configuration and
 * key the harness's bridge runs with, and the input's framing. */
#ifndef HANGAR_FUZZ_BRIDGE_H
#define HANGAR_FUZZ_BRIDGE_H

#define FZB_CONF \
	"[Bridge]\nPair=fuzz\nKey=/dev/null\nPort=5614\nPeer=auto\n" \
	"[Mirror nav.* power.battery]\nDirection=up\nRate=10\n" \
	"[Mirror pilot.wrench]\nDirection=down\nPriority=command\nExpectRate=50\nStaleAfter=100ms\nMaxAge=100ms\n" \
	"[Mirror ardusub.param *.req *.rep]\nDirection=up\nDelivery=reliable\n" \
	"[Mirror *.req *.rep]\nDirection=down\nDelivery=reliable\n" \
	"[Calls]\nRemote=safety.* mux.*\n" \
	"[Files w]\nDirection=down\nMaxSize=64KB\nMaxFiles=2\nAllow=admin\n" \
	"[Files logs]\nDirection=up\nMaxSize=64KB\nAllow=admin\n"

/* The first byte of an input: the state the bridge is in when the first
 * datagram arrives. */
#define FZB_VEHICLE 1u      /* this side is the vehicle (else the topside) */
#define FZB_KEYED 2u        /* session keys in force, the link up */
#define FZB_CLOCK 4u        /* an estimate of the peer's clock (offset 0) */
#define FZB_OFFERED 8u      /* our nonce has been on the wire */
#define FZB_MIRROR_OK 16u   /* the peer's info record came and matched */

/* Then records: one op byte, a 16-bit little-endian length, that many bytes. */
enum {
	FZB_RAW = 0,        /* a datagram as it is: on_dgram */
	FZB_SEALED = 1,     /* kind, priority, body: given a header, the next sequence number and a tag
			     * under the key a peer would use now */
	FZB_BEACON = 2,     /* a discovery line as it is: on_beacon */
	FZB_BEACON_TAG = 3, /* a discovery line, given the tag of the shared key */
	FZB_STREAM = 4,     /* bytes of the reliable stream, as a sealed K_DATA at the offset expected */
	FZB_TICK = 5,       /* what the 10 ms timer runs on the receiving side's state */
	FZB_HELLO = 6,      /* fix byte, 92-byte hello body: sealed, with (fix bit 0) the right pair, (bit 1)
			     * our nonce echoed, (bit 2) the opposite role */
	FZB_PENDING = 7,    /* as FZB_SEALED, under the pending handshake's key */
	FZB_PONG = 8,       /* t2, t3: a sealed pong for the last ping sent */
	FZB_SEALED_AT = 9,  /* a signed byte, then as FZB_SEALED: sealed with the sequence number that far from
			     * the last one used (0: the same again), for the replay window */
	FZB_SEND = 10,      /* bridge.send by an operator: a byte (bit 0: the destination logs, else w; bit 1: the
			     * name is the next 8 bytes as they are, else f.bin), then the file's bytes, put in
			     * the outbox under that name when it can be one */
	FZB_FSTATUS = 11,   /* a status record for the file being sent, on the stream: state, u64 offset, reason */
	FZB_OPS
};

#endif
