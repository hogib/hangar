// SPDX-License-Identifier: LGPL-3.0-or-later
/* The wiring text the manager hands a module (docs/03-manager.md "Wiring"):
 * what it may subscribe to, publish, serve and call, with the slots, rates,
 * provenance links and types the library takes from here and never from the
 * module's code. Read once by hg_init() from the sealed memfd in
 * $HANGAR_WIRING, or from the file $HANGAR_WIRING_FILE for standalone runs.
 *
 * Beyond docs/03's table the text carries, for the call library (docs/04):
 * a `role <vehicle|topside>` line (the module's own role, which a server
 * needs to name a mirrored caller `<role>:<module>`), the bare word
 * `actuator` on a pub line (hg_call() is refused in such a module), and
 * `noroute=1` on a call line nothing serves on either side. */
#ifndef HG_WIRING_H
#define HG_WIRING_H

#include <stddef.h>
#include <stdint.h>

#define HG_WIRING_NAME 48        /* a topic name and its NUL */
#define HG_WIRING_TYPE 64        /* a type name: "req:depth_hold", "dataset:nav_rows" */
#define HG_WIRING_MAX_SUBS 127   /* one futex_waitv, minus the directory word */
#define HG_WIRING_MAX_GLOBS 16
#define HG_WIRING_MAX_PUBS 32
#define HG_WIRING_MAX_SOURCES 16 /* per output */
#define HG_WIRING_MAX_SERVES 32
#define HG_WIRING_MAX_CALLS 32
#define HG_WIRING_MAX_MIRRORS 64
#define HG_WIRING_MAX_ALLOW 8

struct hg_wiring_sub {
	char topic[HG_WIRING_NAME];
	char type[HG_WIRING_TYPE];
	uint64_t hash;           /* 0 if the line gives none */
	uint32_t rate_mhz;
	uint32_t slots;
	int implicit;
};

struct hg_wiring_glob {
	char pattern[HG_WIRING_NAME];
	int implicit;
};

struct hg_wiring_pub {
	char topic[HG_WIRING_NAME];
	char type[HG_WIRING_TYPE];
	uint64_t hash;
	uint32_t slots;
	uint32_t rate_mhz;
	uint64_t stale_after_ns;
	int implicit;
	int actuator;            /* the manifest's Actuator=yes: hg_call() is refused in this module */
	int n_inputs;
	char inputs[4][HG_WIRING_NAME];          /* may be "<role>:<topic>" */
	int n_sources;
	char sources[HG_WIRING_MAX_SOURCES][HG_WIRING_NAME]; /* a topic or a glob */
};

struct hg_wiring_method {
	char req_type[HG_WIRING_TYPE];
	uint64_t req_hash;
	uint32_t req_size;
	char rep_type[HG_WIRING_TYPE];
	uint64_t rep_hash;
	uint32_t rep_size;
};

struct hg_wiring_serve {
	char method[32];
	struct hg_wiring_method m;
	uint64_t deadline_ns;
	int n_allow;
	char allow[HG_WIRING_MAX_ALLOW][HG_WIRING_NAME];
};

struct hg_wiring_call {
	char target[HG_WIRING_NAME + 32]; /* <module>.<method> */
	struct hg_wiring_method m;
	int remote;
	int noroute;             /* nothing serves it on either side: HG_E_NOROUTE at once */
};

struct hg_wiring_mirror {
	char pattern[HG_WIRING_NAME];
	uint32_t rate_mhz;
	uint64_t stale_after_ns;
};

struct hg_wiring {
	char module[32];
	char role[16];           /* this machine's Role (vehicle, topside); "" if the line is absent */
	int n_subs, n_globs, n_pubs, n_serves, n_calls, n_mirrors;
	struct hg_wiring_sub subs[HG_WIRING_MAX_SUBS];
	struct hg_wiring_glob globs[HG_WIRING_MAX_GLOBS];
	struct hg_wiring_pub pubs[HG_WIRING_MAX_PUBS];
	struct hg_wiring_serve serves[HG_WIRING_MAX_SERVES];
	struct hg_wiring_call calls[HG_WIRING_MAX_CALLS];
	struct hg_wiring_mirror mirrors[HG_WIRING_MAX_MIRRORS];
};

/* Parses len bytes of wiring text. 0 on success; -1 with errno EINVAL and, if
 * err is given, one line naming the line and the fault. A 128th sub line is
 * refused here as the manager refuses it. */
int hg_wiring_parse(const char *text, size_t len, struct hg_wiring *w, char *err, size_t errlen);

/* Reads a whole descriptor (from offset 0) or a file and parses it. */
int hg_wiring_read_fd(int fd, struct hg_wiring *w, char *err, size_t errlen);
int hg_wiring_read_file(const char *path, struct hg_wiring *w, char *err, size_t errlen);

/* Lookups; NULL when not listed. */
const struct hg_wiring_sub *hg_wiring_find_sub(const struct hg_wiring *w, const char *topic);
const struct hg_wiring_glob *hg_wiring_find_glob(const struct hg_wiring *w, const char *pattern);
const struct hg_wiring_pub *hg_wiring_find_pub(const struct hg_wiring *w, const char *topic);
const struct hg_wiring_mirror *hg_wiring_match_mirror(const struct hg_wiring *w, const char *topic);
/* 1 if topic matches any subglob line. */
int hg_wiring_glob_matches(const struct hg_wiring *w, const char *topic);

/* Shell-style match with * and ? over a topic name (no character classes,
 * '*' crosses dots: "nav.*" matches "nav.state.depth"). */
int hg_glob_match(const char *pattern, const char *name);

/* "60ms", "1s", "2.5s", "250us", "0": nanoseconds; -1 if malformed. */
int64_t hg_parse_duration(const char *s);
/* "50", "0.5", "12.345": millihertz; -1 if malformed. */
int64_t hg_parse_rate_mhz(const char *s);

#endif
