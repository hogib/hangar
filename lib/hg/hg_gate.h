// SPDX-License-Identifier: LGPL-3.0-or-later
/* The actuator gate (docs/07-safety.md "The actuator gate"): the one check
 * every actuator module runs before each output. hg_gate_check returns the
 * newest command only if `safety.state` is fresh (within SafetyTimeout) and
 * says `outputs_enabled`, the command was published within CommandTimeout,
 * and the command is not flagged as computed from stale inputs (docs/01
 * "Stale inputs"); otherwise NULL, with the reason the module puts in its
 * status line: "disarmed", "safety state stale 410 ms", "command stale 230
 * ms", "command computed from stale inputs". The Command topic and the two
 * timeouts come from the wiring's `actuator` line, which the manager writes
 * from the manifest's [Actuator] section; nothing in the module's code
 * decides them. A neutral output published while the gate is closed names
 * no input (HG_NO_REFS): that is the module's publish, not the gate's. */
#ifndef HG_GATE_H
#define HG_GATE_H

#include <stddef.h>
#include <stdint.h>

#include "hg_topic.h"

typedef struct hg_gate hg_gate;

#define HG_GATE_COMMAND_MAX 256   /* the largest command payload the gate holds */

/* Opens the gate from the wiring: its `actuator` line names the Command
 * topic and the timeouts; the two subscriptions, Command and safety.state,
 * are opened here (the module does not open them again: a topic read
 * through two subscriptions would make its provenance ambiguous). NULL with
 * errno: EACCES (no actuator line, or the topics are not in the wiring),
 * ENOMEM. */
hg_gate *hg_gate_open(void);
/* The same with the settings given, for a module run without a wiring
 * (tests, `hangar run`); 0 for a timeout means its default (200 ms, 300 ms). */
hg_gate *hg_gate_open_with(const char *command, uint64_t command_timeout_ns, uint64_t safety_timeout_ns);
void hg_gate_close(hg_gate *g);

/* The newest command, as a pointer into the gate (valid until the next
 * check), or NULL with *why set to one line (static storage in the gate). */
const void *hg_gate_check(hg_gate *g, const char **why);

/* The subscriptions the gate reads, for the module's wait set: Command first,
 * safety.state second. Returns 2, or -1 with E2BIG. */
int hg_gate_subs(const hg_gate *g, hg_sub **out, int max);
hg_sub *hg_gate_command_sub(const hg_gate *g);
hg_sub *hg_gate_safety_sub(const hg_gate *g);
/* The info of the command the last check returned (its seq for provenance,
 * its stamp); zeroed when the gate has read none. */
const struct hg_info *hg_gate_command_info(const hg_gate *g);
uint32_t hg_gate_command_bytes(const hg_gate *g);
uint64_t hg_gate_command_timeout_ns(const hg_gate *g);
uint64_t hg_gate_safety_timeout_ns(const hg_gate *g);

/* The newest safety.state the gate has read (docs/07 "Vehicle states": the
 * first bytes of msg/safety_state.msg), judged at the time of the last
 * check: `fresh` is 0 when it is older than SafetyTimeout, when the topic is
 * absent, or when its writer is closed or dead. `seq` names the message, so
 * that an actuator can reference the safety.state it acted on (docs/10
 * "Armed agreement"). */
struct hg_gate_safety {
	int present;            /* a message has been read */
	int fresh;
	uint8_t state;          /* 0 disarmed, 1 armed, 2 failsafe, 3 arming */
	uint8_t outputs_enabled;
	uint8_t action;
	uint32_t triggers;
	uint64_t seq;
	uint64_t age_ns;        /* now - publish_ns at the last check */
};
void hg_gate_safety(const hg_gate *g, struct hg_gate_safety *out);

#endif
