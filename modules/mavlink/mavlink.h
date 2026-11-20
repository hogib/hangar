// SPDX-License-Identifier: GPL-3.0-or-later
/* hangar-mavlink: one program, three modes (modules/mavlink/README.md):
 *   hangar-mavlink <mavlink.conf>              the vehicle's module `mavlink` (docs/10)
 *   hangar-mavlink --topside <conf>            the topside's module `topside_mavlink`
 *   hangar-mavlink --neutral <mavlink.conf>    the vehicle module's Neutral command
 * Shared between the three translation units: the UDP link with its
 * signing and the frame send/receive helpers. */
#ifndef HANGAR_MAVLINK_H
#define HANGAR_MAVLINK_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

#include "mav.h"
#include "mavlink_conf.h"

/* The serial line's transmit queue, in whole frames: a frame partly
 * written stays at the head until the rest goes, so the stream never carries
 * half a frame. See mav_link_send for what happens when it is full. */
#define MAV_TXQ 32
struct mav_txframe {
	uint8_t data[MAV_MAX_FRAME];
	uint16_t len, off;
	uint32_t msgid;
};

struct mav_link {
	int fd;                      /* -1 while a serial line is closed */
	struct sockaddr_in peer;
	int have_peer;
	uint8_t sysid, compid, seq;
	struct mav_signing signing;
	uint64_t sent, received, rejected;   /* rejected: bad CRC, bad signature, truncated */
	uint64_t unknown;                    /* well-formed frames of a message id not generated */
	uint64_t foreign;                    /* vehicle: frames from ArduSub's system id but another component */
	char last_reject[64];
	int tlog_fd;                 /* -1: none */
	/* A serial line (Serial=) instead of UDP: the line is the peer while it
	 * is open (have_peer), reopened with a back-off when it goes away. */
	int serial;
	char dev[512];
	int baud, rtscts, exclusive;
	uint8_t rx[4 * MAV_MAX_FRAME];
	size_t rx_len;
	struct mav_txframe txq[MAV_TXQ];
	int tx_head, tx_count;
	uint64_t tx_dropped;         /* frames not sent: the queue was full, or the line went away under them */
	uint64_t tx_superseded;      /* MANUAL_CONTROL replaced in the queue by a newer one before it went */
	uint64_t garbage;            /* bytes skipped looking for a frame start (line noise, a cut frame) */
	uint64_t opens, drops;       /* the line opened; the line went away */
	uint64_t next_open_ns, backoff_ns;
	char line_state[128];        /* "open", or why it is closed */
};

/* UDP: binds c->bind (SO_REUSEADDR, non-blocking). Serial (c->serial): opens
 * the line raw (8N1, c->baud, RTS/CTS if configured), exclusively (TIOCEXCL)
 * when `exclusive`; a line that cannot be opened yet is not an error here: it
 * is retried by mav_link_service, and line_state says why. Loads the key when
 * signing. 0 or -1 with err (a configuration that cannot work). */
int mav_link_open(struct mav_link *l, const struct mavlink_conf *c, char *err, size_t errlen);
int mav_link_open_ex(struct mav_link *l, const struct mavlink_conf *c, int exclusive, char *err, size_t errlen);
/* Serial: writes what the queue holds and the line takes, and reopens a
 * closed line when its back-off has passed. Call at least every tick. Returns
 * 1 when l->fd changed (a wait set must follow it), else 0. A no-op on UDP. */
int mav_link_service(struct mav_link *l, uint64_t now_ns);
/* Serial: waits up to timeout_ms for the queue and the driver's output to
 * empty; what is left after that is discarded (so close never blocks). */
void mav_link_drain(struct mav_link *l, int timeout_ms);
/* One line for a status: "udp", or "serial <dev> <baud> open|down (<why>)" with
 * the counters that are not zero. */
void mav_link_describe(const struct mav_link *l, char *out, size_t len);
void mav_link_close(struct mav_link *l);
/* Packs and sends one message to the peer; 0, or -1 (no peer yet, or the
 * socket refused). The frame is also written to the tlog when one is open.
 * Serial: the frame joins the transmit queue and the queue is written as far
 * as the line takes it, never waiting. MANUAL_CONTROL and COMMAND_LONG are
 * the control frames: a MANUAL_CONTROL replaces one still unsent in the queue
 * (the newest command is the one that matters), and with the queue full a
 * control frame takes the place of the newest unsent telemetry-side frame
 * (rate requests, parameter traffic); with nothing to evict, or for any other
 * frame, the new frame is dropped and counted. */
int mav_link_send(struct mav_link *l, uint32_t msgid, const uint8_t *payload, size_t full_len, uint8_t crc_extra);
/* Receives every datagram waiting, decoding each frame and calling fn with
 * it; frames that fail their checks are counted, not delivered. The sender
 * of the first valid frame becomes the peer when none is configured. Returns
 * the number of valid frames. */
int mav_link_receive(struct mav_link *l, void (*fn)(void *ctx, const struct mav_frame *f), void *ctx);

/* Convenience: pack a struct through its generated function and send it. */
#define MAV_SEND(link, NAME, lname, msgp)                                                       \
	do {                                                                                   \
		uint8_t pl_[MAV_LEN_##NAME];                                                   \
		mav_pack_##lname((msgp), pl_);                                                 \
		mav_link_send((link), MAV_MSG_ID_##NAME, pl_, sizeof pl_, MAV_CRC_##NAME);     \
	} while (0)

int mavlink_vehicle_main(const char *conf_path);
int mavlink_topside_main(const char *conf_path);
int mavlink_neutral_main(const char *conf_path);

#endif
