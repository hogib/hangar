// SPDX-License-Identifier: GPL-3.0-or-later
/* Module manifests (docs/02-manifest.md): the strict INI parser, every
 * section and key, the graph checks, and the implicit topics and
 * subscriptions the manager derives (docs/03 "Wiring"). A manifest is parsed
 * alone into a struct mf_module; a set of them is checked as a whole, which
 * resolves the message types from MsgDir, generates the req:/rep:/params:
 * types, and marks each refused module with its reason. */
#ifndef HANGARD_MANIFEST_H
#define HANGARD_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#include "hg_type.h"

#define MF_NAME 32            /* module, method, user and group names: 31 bytes + NUL */
#define MF_TOPIC 48           /* a topic name + NUL */
#define MF_REF 64             /* "<role>:<topic>" */
#define MF_TYPE 64
#define MF_PATH 512
#define MF_MAX_ARGS 64
#define MF_MAX_ENV 32
#define MF_MAX_GROUPS 16
#define MF_MAX_CAPS 8
#define MF_MAX_PUBS 32
#define MF_MAX_SUBS 160       /* above 127 so that the limit is ours to refuse */
#define MF_MAX_SERVES 32
#define MF_MAX_CALLS 32
#define MF_MAX_ALLOW 8
#define MF_MAX_INPUTS 4
#define MF_MAX_SOURCES 16
#define MF_MAX_MODULES 128
#define MF_ERR 320

enum mf_policy { MF_RESTART_ALWAYS, MF_RESTART_ON_FAILURE, MF_RESTART_NEVER };
enum mf_giveup { MF_GIVEUP_DEGRADE, MF_GIVEUP_FAILSAFE };
enum mf_sched { MF_SCHED_OTHER, MF_SCHED_FIFO, MF_SCHED_RR };

/* A command line: argv[0] an absolute path, split on blanks, "..." kept
 * whole, no shell. */
struct mf_exec {
	int argc;
	char *argv[MF_MAX_ARGS + 1];
	char buf[1024];
};

/* A resolved message type: from MsgDir, or generated. */
struct mf_type {
	char name[MF_TYPE];
	uint64_t hash;           /* 0 for "frame": any frame type */
	uint32_t payload_bytes;
	uint32_t schema_bytes;
	char *schema;            /* the .msg text, as the header carries it */
	int frame;
};

struct mf_pub {
	char topic[MF_TOPIC];
	char type[MF_TYPE];
	uint32_t rate_mhz;
	uint32_t slots;
	uint64_t stale_after_ns;
	int stale_set;
	int actuator;
	int n_inputs;
	char inputs[MF_MAX_INPUTS][MF_REF];
	int n_sources;
	char sources[MF_MAX_SOURCES][MF_TOPIC];
	int implicit;
	int line;
	int type_idx;            /* into mf_set.types once checked; -1 before */
};

struct mf_sub {
	char topic[MF_TOPIC];    /* a name or a glob */
	char type[MF_TYPE];      /* "" if not given */
	int glob;
	int implicit;
	int line;
	/* Filled by the check from the publisher, for the wiring line. */
	char pub_type[MF_TYPE];
	uint64_t hash;
	uint32_t rate_mhz;
	uint32_t slots;
};

struct mf_serve {
	char method[MF_NAME];
	char req[MF_TYPE];
	char rep[MF_TYPE];
	uint64_t deadline_ns;
	int allow_set;
	int n_allow;
	char allow[MF_MAX_ALLOW][MF_REF];
	int line;
	int req_idx, rep_idx;
};

struct mf_call {
	char server[MF_NAME];
	char method[MF_NAME];
	char req[MF_TYPE];       /* "" unless given (a method of the other side) */
	char rep[MF_TYPE];
	int remote;
	int line;
	int req_idx, rep_idx;
};

struct mf_module {
	char name[MF_NAME];
	char path[MF_PATH];
	uint64_t text_hash;      /* FNV-1a 64 of the file's bytes */

	/* [Module] */
	char description[128];
	int legacy;
	int has_module_neutral;
	struct mf_exec module_neutral;
	int n_excl;
	char excl[MF_MAX_GROUPS][MF_NAME];
	struct mf_exec exec;
	int n_env;
	char env[MF_MAX_ENV][256];
	char user[MF_NAME];
	char group[MF_NAME];
	int user_set, group_set;
	int n_groups;
	char groups[MF_MAX_GROUPS][MF_NAME];
	int n_caps;
	char caps[MF_MAX_CAPS][24];
	int privileged;

	int n_pubs;
	struct mf_pub pubs[MF_MAX_PUBS];
	int n_subs;
	struct mf_sub subs[MF_MAX_SUBS];
	int n_serves;
	struct mf_serve serves[MF_MAX_SERVES];
	int n_calls;
	struct mf_call calls[MF_MAX_CALLS];
	int bridge;

	/* [Actuator] */
	int has_actuator;
	char act_command[MF_TOPIC];
	struct mf_exec act_neutral;
	int n_act_groups;
	char act_groups[MF_MAX_GROUPS][MF_NAME];
	uint64_t command_timeout_ns;
	uint64_t safety_timeout_ns;

	/* [Params] */
	int has_params;
	char params_type[MF_TYPE];
	int params_allow_set;
	int n_params_allow;
	char params_allow[MF_MAX_ALLOW][MF_REF];
	int params_idx;

	/* [Restart] */
	enum mf_policy policy;
	uint64_t backoff_first_ns, backoff_max_ns;
	uint64_t reset_after_ns;
	uint32_t burst_count;
	uint64_t burst_period_ns;
	enum mf_giveup giveup;
	int giveup_set;
	uint64_t stop_timeout_ns;
	int stop_timeout_set;

	/* [Health] */
	uint64_t start_grace_ns;
	uint64_t stale_grace_ns;
	uint64_t watchdog_ns;    /* 0: off */

	/* [Schedule] */
	enum mf_sched sched;
	int priority;            /* 0: none */
	int nice;
	int cpus_set;
	char cpus[64];
	uint64_t cpu_mask[4];    /* up to 256 CPUs */
	int lock_memory;

	/* Derived */
	int has_actuator_output; /* a [Publish] with Actuator=yes */
	int has_neutral;         /* [Actuator] Neutral or [Module] Neutral */
	int refused;
	char reason[MF_ERR];
	int lines[16];           /* section header lines, for refusals: see manifest.c */
};

struct mf_set {
	char role[16];
	char other_role[16];
	char privileged_module[MF_NAME];
	char msg_dir[MF_PATH];
	int n_modules;
	struct mf_module *modules[MF_MAX_MODULES];
	int n_types, cap_types;
	struct mf_type *types;
	/* Lines that are not refusals (hangar check's WARN): one per entry. */
	int n_warnings;
	char warnings[64][MF_ERR];
};

/* Parses one manifest file. 0, or -1 with err = "<file>:<line>: <reason>"
 * (errno EINVAL for a parse refusal, else the open error). m is zeroed
 * first; its name is the file name without .module. */
int mf_parse_file(const char *path, struct mf_module *m, char *err, size_t errlen);
/* The same from text, named name, for tests and for `hangar run`. */
int mf_parse_text(const char *text, size_t len, const char *name, const char *path,
		  struct mf_module *m, char *err, size_t errlen);

/* A set: role "vehicle" or "topside", the PrivilegedModule (or ""), MsgDir. */
void mf_set_init(struct mf_set *s, const char *role, const char *privileged, const char *msg_dir);
/* Adds a parsed module (the set keeps the pointer). -1 with ENOSPC when full,
 * EEXIST for a name already in the set. */
int mf_set_add(struct mf_set *s, struct mf_module *m);
/* Reads every <name>.module in dir: each that parses is added; one that does not is
 * added refused, with the parse error as its reason (so that status lists it).
 * Returns the number read, -1 if the directory cannot be read. */
int mf_set_load_dir(struct mf_set *s, const char *dir);
/* Resolves the types, derives the implicit topics and subscriptions, and
 * runs every graph check of docs/02; refused modules get .refused and
 * .reason. Returns the number of accepted modules. */
int mf_set_check(struct mf_set *s);
void mf_set_free(struct mf_set *s);

const struct mf_type *mf_type_at(const struct mf_set *s, int idx);
const struct mf_type *mf_type_find(const struct mf_set *s, const char *name);
struct mf_module *mf_find(const struct mf_set *s, const char *name);

/* snprintf with the same contract, through a va_list: a bounded copy that
 * may truncate is the intent everywhere a name goes into a fixed field, and
 * this keeps the compiler from flagging each one. */
int mf_fmt(char *dst, size_t len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Value grammar of docs/02 (also used by hangard.conf). -1 if malformed. */
int64_t mf_parse_duration(const char *s);   /* "100ms", "2.5s", "1min": ns */
int mf_parse_bool(const char *s);           /* yes/no: 1/0, -1 otherwise */
int mf_parse_exec(const char *s, struct mf_exec *out);
/* A single-token test of the module/method name rule: [a-z0-9_], 1..31. */
int mf_valid_module_name(const char *s);
/* 1 if token is a valid Allow token for a machine of role `role`. */
int mf_valid_allow_token(const char *token, const char *role);

#endif
