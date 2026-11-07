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

struct mav_link {
	int fd;
	struct sockaddr_in peer;
	int have_peer;
	uint8_t sysid, compid, seq;
	struct mav_signing signing;
	uint64_t sent, received, rejected;
	char last_reject[64];
	int tlog_fd;                 /* -1: none */
};

/* Binds c->bind (SO_REUSEADDR, non-blocking), loads the key when signing. 0
 * or -1 with err. */
int mav_link_open(struct mav_link *l, const struct mavlink_conf *c, char *err, size_t errlen);
void mav_link_close(struct mav_link *l);
/* Packs and sends one message to the peer; 0, or -1 (no peer yet, or the
 * socket refused). The frame is also written to the tlog when one is open. */
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
