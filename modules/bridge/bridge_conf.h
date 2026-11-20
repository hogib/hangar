// SPDX-License-Identifier: GPL-3.0-or-later
/* bridge.conf (docs/08-bridge.md "Configuration"): one file, identical on both
 * sides, read by the manager (the bridge's wiring, the [Calls] Remote=
 * expansion) and by the bridge (delivery, priority, timeouts). Manifest
 * syntax, through lib/hg's walker. */
#ifndef BRIDGE_CONF_H
#define BRIDGE_CONF_H

#include <stddef.h>
#include <stdint.h>

#define BC_MAX_MIRRORS 32
#define BC_MAX_PATTERNS 16
#define BC_MAX_REMOTE 32
#define BC_NAME 64
#define BC_PATH 512

enum bc_priority { BC_SAFETY, BC_COMMAND, BC_CALL, BC_TELEMETRY, BC_DATASET, BC_BULK };

struct bc_mirror {
	char patterns[BC_MAX_PATTERNS][BC_NAME];
	int n_patterns;
	int up;                  /* Direction=up: vehicle to topside */
	int reliable;            /* Delivery=reliable */
	int priority;            /* enum bc_priority */
	uint32_t rate_mhz;       /* Rate=: the most sent per second; 0 every message */
	uint32_t expect_mhz;     /* ExpectRate=: the receiving file's rate; 0 an event topic */
	uint64_t stale_after_ns; /* StaleAfter=: 0 with ExpectRate is three periods */
	uint64_t max_age_ns;     /* MaxAge= (command only); 0 none */
	int line;
};

struct bridge_conf {
	char pair[BC_NAME];
	char key_path[BC_PATH];
	uint16_t port;           /* data, 5614 */
	uint16_t discovery_port; /* 5613 */
	char peer[BC_NAME];      /* "auto", or host or host:port */
	uint64_t max_rate_bps;   /* accepted; not enforced in this stage */
	char msg_dir[BC_PATH];   /* the msg/ tree whose hash the handshake compares */
	int n_mirrors;
	struct bc_mirror mirrors[BC_MAX_MIRRORS];
	int n_remote;
	char remote[BC_MAX_REMOTE][BC_NAME]; /* [Calls] Remote=: <module>.<method> patterns */
	unsigned char hash[32];  /* BLAKE2s-256 of the text without its Peer= line */
};

/* Parses text; 0, or -1 with "line N: <reason>" in err. */
int bc_parse(const char *text, size_t len, struct bridge_conf *c, char *err, size_t errlen);
/* Reads and parses a file. */
int bc_load(const char *path, struct bridge_conf *c, char *err, size_t errlen);
/* 1 if <module>.<method> is one [Calls] Remote= names. */
int bc_remote_matches(const struct bridge_conf *c, const char *module, const char *method);
/* The mirror whose patterns match topic in the given direction (up: sent by the
 * vehicle), or NULL. A named pattern wins over a glob. */
const struct bc_mirror *bc_find(const struct bridge_conf *c, const char *topic, int up);
/* 1 if the pattern has a glob character. */
int bc_is_glob(const char *pattern);
/* The 32-byte key file: 32 raw bytes, or 64 hex digits and an optional
 * newline. 0, or -1 with err. */
int bc_load_key(const char *path, unsigned char key[32], char *err, size_t errlen);

#endif
