// SPDX-License-Identifier: GPL-3.0-or-later
/* safety (binary hangar-safety): the one module that owns the vehicle's
 * state (docs/07-safety.md "Vehicle states", "Failsafe"). It arms, disarms,
 * enters failsafe and publishes safety.state at 10 Hz and on every change;
 * serves arm, disarm, estop, clear and check; watches its inputs for the
 * triggers; latches a failsafe in its state directory so that a restart comes
 * back in it; and arms ArduSub through mavlink.arm asynchronously, after the
 * `arming` message is out (docs/10 "Arming and failsafe"). Parameters:
 * msg/safety_params.msg. What waits for the bridge is marked
 * `until the bridge`; modules/safety/README.md lists every deviation. */
#include "mod.h"

#include <ctype.h>
#include <fcntl.h>

#include "ardusub_state.h"
#include "arm_rep.h"
#include "arm_req.h"
#include "battery.h"
#include "check_rep.h"
#include "depth.h"
#include "empty.h"
#include "hangar_modules.h"
#include "hangar_system.h"
#include "hg_call.h"
#include "hg_control.h"
#include "hg_params.h"
#include "leak.h"
#include "mux_params.h"
#include "safety_params.h"
#include "safety_state.h"

#define WHO "hangar-safety"

enum { ST_DISARMED = 0, ST_ARMED = 1, ST_FAILSAFE = 2, ST_ARMING = 3 };
enum { ACT_NEUTRAL = 0, ACT_SURFACE = 1, ACT_HOLD = 2 };
enum {
	TRG_MODULE = 1u << 0, TRG_TETHER = 1u << 1, TRG_LEAK = 1u << 2, TRG_BATTERY = 1u << 3,
	TRG_DEPTH = 1u << 4, TRG_AUTOPILOT = 1u << 5, TRG_ESTOP = 1u << 6, TRG_INPUT = 1u << 7,
};
#define MANUAL_MODE 19u
#define PERIOD_NS 100000000ull       /* 10 Hz */
#define MODULES_STALE_NS 3000000000ull
#define MAX_MODULES 64
#define HISTORY 64

static safety_params P;
static safety_state S;
static hg_pub *pub_state;
static char state_dir[600], latch_path[700];
static int have_autopilot;   /* the wiring routes mavlink.arm: ArduSub is behind the actuators */

/* ---- Inputs ----------------------------------------------------------------- */

static hg_sub *sub_modules, *sub_system, *sub_leak, *sub_battery, *sub_depth, *sub_ardusub, *sub_mux_params;

static struct {
	struct hangar_system sys;
	struct hg_info sys_info;
	int have_sys;
	struct leak leak;
	struct hg_info leak_info;
	int have_leak;
	struct battery bat;
	struct hg_info bat_info;
	int have_bat;
	struct depth depth;
	struct hg_info depth_info;
	int have_depth;
	struct ardusub_state ap;
	struct hg_info ap_info;
	int have_ap;
	unsigned char mux_params_buf[64 + sizeof(mux_params)];
	struct hg_info mux_info;
	int have_mux;
	struct {
		char name[32];
		uint8_t state;
		uint64_t published_ns;
	} mods[MAX_MODULES];
	int n_mods;
} IN;

/* What this module published, by seq: the ardusub.state messages name the
 * safety.state they were judged against (docs/10 "Armed agreement"). */
static struct {
	uint64_t seq;
	uint8_t state, outputs_enabled;
} history[HISTORY];
static int history_n;

/* The failsafe's own memory. */
static struct {
	uint32_t latched;            /* edge triggers that fired: estop, autopilot's disagreement */
	uint64_t entered_ns;         /* when failsafe was entered */
	uint64_t hold_since_ns;      /* 0: not holding */
	int hold_expired;
	int disarmed_in_failsafe;    /* a disarm during failsafe: outputs off until clear */
	uint64_t battery_low_since;  /* 0: not low */
	uint64_t arming_since;
	hg_pending *arm_call;
	char ap_reason[64];          /* why the autopilot trigger is active */
	int ap_disagree;             /* latched until ArduSub reports disarmed in MANUAL */
	uint64_t module_cleared_ns;  /* when the manager accepted clear-failsafe */
	char last_status[128];
} F;

static int validate(const void *candidate, const void *current, char reason[64])
{
	(void)current;
	const safety_params *c = candidate;
	/* The required list: blank-separated module names. */
	for (const char *p = c->required; *p; p++)
		if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_' || *p == ' ')) {
			mod_fmt(reason, 64, "required: not module names");
			return HG_E_REFUSED;
		}
	if (c->required[sizeof c->required - 1] != 0) {
		mod_fmt(reason, 64, "required: not terminated");
		return HG_E_REFUSED;
	}
	return HG_OK;
}

/* ---- The latch (docs/07 "The latch") ---------------------------------------- */

static void latch_write(void)
{
	char text[512];
	mod_fmt(text, sizeof text, "# hangar safety failsafe latch\nstate 2\naction %u\ntriggers %u\nlatched %u\nreason %s\n",
		S.action, S.triggers, F.latched, S.reason);
	if (mod_write_file(latch_path, text) < 0)
		mod_log(WHO, "cannot write the latch %s: %s", latch_path, strerror(errno));
}

static void latch_remove(void)
{
	if (unlink(latch_path) < 0 && errno != ENOENT)
		mod_log(WHO, "cannot remove the latch %s: %s", latch_path, strerror(errno));
}

/* 1 if a latch was read: S holds its state. */
static int latch_read(void)
{
	FILE *f = fopen(latch_path, "r");
	if (!f)
		return 0;
	char line[256];
	unsigned action = 0, triggers = 0, latched = 0;
	char reason[64] = "";
	int seen = 0;
	while (fgets(line, sizeof line, f)) {
		unsigned v;
		if (sscanf(line, "state %u", &v) == 1 && v == ST_FAILSAFE)
			seen = 1;
		else if (sscanf(line, "action %u", &v) == 1)
			action = v;
		else if (sscanf(line, "triggers %u", &v) == 1)
			triggers = v;
		else if (sscanf(line, "latched %u", &v) == 1)
			latched = v;
		else if (strncmp(line, "reason ", 7) == 0) {
			mod_fmt(reason, sizeof reason, "%s", line + 7);
			char *nl = strchr(reason, '\n');
			if (nl)
				*nl = 0;
		}
	}
	fclose(f);
	if (!seen)
		return 0;
	S.state = ST_FAILSAFE;
	S.action = (uint8_t)(action > ACT_HOLD ? ACT_NEUTRAL : action);
	S.outputs_enabled = S.action != ACT_NEUTRAL;
	S.triggers = triggers;
	F.latched = latched;
	F.entered_ns = hg_now_ns();
	mod_fmt(S.by, sizeof S.by, "latch");
	mod_fmt(S.reason, sizeof S.reason, "%s", reason);
	return 1;
}

/* ---- Publishing --------------------------------------------------------------- */

static const char *state_name(uint8_t st)
{
	static const char *const names[] = { "disarmed", "armed", "failsafe", "arming" };
	return st < 4 ? names[st] : "?";
}

static const char *action_name(uint8_t a)
{
	static const char *const names[] = { "neutral", "surface", "hold" };
	return a < 3 ? names[a] : "?";
}

static const char *trigger_names(uint32_t t, char *buf, size_t len)
{
	static const char *const names[8] = { "module", "tether", "leak", "battery", "depth", "autopilot", "estop", "input" };
	buf[0] = 0;
	for (int i = 0; i < 8; i++)
		if (t & (1u << i)) {
			size_t n = strlen(buf);
			mod_fmt(buf + n, len - n, "%s%s", n ? "," : "", names[i]);
		}
	return buf;
}

static void publish_state(const struct hg_input *req)
{
	uint64_t seq = hg_pub_next_seq(pub_state);
	history[history_n % HISTORY].seq = seq;
	history[history_n % HISTORY].state = S.state;
	history[history_n % HISTORY].outputs_enabled = S.outputs_enabled;
	history_n++;
	int rc = req ? hg_publish_with(pub_state, &S, sizeof S, hg_now_ns(), 0, req, 1)
		     : hg_publish(pub_state, &S, sizeof S, hg_now_ns());
	if (rc < 0 && req)
		rc = hg_publish(pub_state, &S, sizeof S, hg_now_ns()); /* a wiring without the request Source */
	if (rc < 0)
		mod_log(WHO, "hg_publish(safety.state): %s", strerror(errno));
	char line[128], t[80];
	if (S.state == ST_FAILSAFE)
		mod_fmt(line, sizeof line, "failsafe (%s): %s: %s", action_name(S.action), trigger_names(S.triggers, t, sizeof t),
			S.reason);
	else
		mod_fmt(line, sizeof line, "%s%s%s%s%s", state_name(S.state), S.by[0] ? " by " : "", S.by,
			S.reason[0] ? ": " : "", S.reason);
	if (strcmp(line, F.last_status) != 0) {
		mod_fmt(F.last_status, sizeof F.last_status, "%s", line);
		hg_status(line);
		mod_log(WHO, "%s", line);
	}
}

/* The safety.state message with this seq, as published: 1 if known. */
static int history_lookup(uint64_t seq, uint8_t *state, uint8_t *enabled)
{
	for (int i = 0; i < HISTORY && i < history_n; i++) {
		int k = (history_n - 1 - i) % HISTORY;
		if (history[k].seq == seq) {
			*state = history[k].state;
			*enabled = history[k].outputs_enabled;
			return 1;
		}
	}
	return 0;
}

/* ---- Reading the inputs -------------------------------------------------------- */

static void read_modules(uint64_t now)
{
	struct hangar_modules m;
	struct hg_info info;
	while (hg_next(sub_modules, &m, sizeof m, &info) == HG_MSG) {
		int k = -1;
		for (int i = 0; i < IN.n_mods; i++)
			if (strncmp(IN.mods[i].name, m.name, sizeof m.name) == 0)
				k = i;
		if (k < 0 && IN.n_mods < MAX_MODULES)
			k = IN.n_mods++;
		if (k < 0)
			continue;
		mod_fmt(IN.mods[k].name, sizeof IN.mods[k].name, "%.31s", m.name);
		IN.mods[k].state = m.state;
		IN.mods[k].published_ns = info.publish_ns;
	}
	(void)now;
}

static int module_ready(const char *name, uint64_t now)
{
	for (int i = 0; i < IN.n_mods; i++)
		if (strcmp(IN.mods[i].name, name) == 0)
			return IN.mods[i].state == 1 && now <= IN.mods[i].published_ns + MODULES_STALE_NS;
	return 0;
}

static void read_inputs(uint64_t now)
{
	read_modules(now);
	mod_latest(sub_system, &IN.sys, sizeof IN.sys, &IN.sys_info, &IN.have_sys);
	mod_latest(sub_leak, &IN.leak, sizeof IN.leak, &IN.leak_info, &IN.have_leak);
	mod_latest(sub_battery, &IN.bat, sizeof IN.bat, &IN.bat_info, &IN.have_bat);
	mod_latest(sub_depth, &IN.depth, sizeof IN.depth, &IN.depth_info, &IN.have_depth);
	mod_latest(sub_mux_params, IN.mux_params_buf, sizeof IN.mux_params_buf, &IN.mux_info, &IN.have_mux);
	/* ardusub.state: every message is judged, not only the newest, since a
	 * disagreement is "the first such message" (docs/10). */
	struct ardusub_state ap;
	struct hg_info info;
	enum hg_result r;
	while ((r = hg_next(sub_ardusub, &ap, sizeof ap, &info)) == HG_MSG) {
		IN.ap = ap;
		IN.ap_info = info;
		IN.have_ap = 1;
		if (!have_autopilot)
			continue;
		/* Armed agreement: the safety.state the module acted on says
		 * neither arming nor outputs_enabled, and ArduSub is armed. */
		if (ap.armed == 1) {
			uint64_t ref_seq = 0;
			for (uint32_t i = 0; i < info.n_inputs && i < HG_MAX_REFS; i++)
				if (info.inputs[i].topic_id == hg_topic_id("safety.state"))
					ref_seq = info.inputs[i].seq;
			uint8_t st, en;
			if (ref_seq && history_lookup(ref_seq, &st, &en) && st != ST_ARMING && !en) {
				F.ap_disagree = 1;
				mod_fmt(F.ap_reason, sizeof F.ap_reason, "ArduSub armed while hangar was %s", state_name(st));
			}
		}
		if (ap.armed == 0 && S.state == ST_ARMED) {
			F.ap_disagree = 1;
			mod_fmt(F.ap_reason, sizeof F.ap_reason, "ArduSub disarmed itself: %s",
				ap.failsafe[0] ? ap.failsafe : ap.arm_reason[0] ? ap.arm_reason : "no reason given");
		}
		if (ap.armed == 0 && ap.mode == MANUAL_MODE && F.ap_disagree &&
		    !(S.state == ST_ARMED))
			F.ap_disagree = 0; /* the disagreement is over: ArduSub is disarmed, in MANUAL */
	}
	if (r == HG_ABSENT || r == HG_CLOSED)
		IN.have_ap = IN.have_ap && r != HG_ABSENT;
}

static const mux_params *mux_p(void)
{
	return IN.have_mux ? (const mux_params *)(IN.mux_params_buf + 64) : NULL;
}

/* ---- The triggers (docs/07 "Failsafe") ----------------------------------------- */

/* The triggers active now, with one reason per bit in reasons[8]. The
 * module and estop bits count in every state; the rest while arming, armed
 * or in failsafe (a vehicle on the bench with a sensor off is not a
 * failsafe, until it arms). */
static uint32_t active_triggers(uint64_t now, char reasons[8][64], int all)
{
	uint32_t t = 0;
	for (int i = 0; i < 8; i++)
		reasons[i][0] = 0;
	/* The manager's topic follows a clear-failsafe within its next publish:
	 * a system message from before the clear it accepted is not news. */
	if (IN.have_sys && IN.sys.state == 2 && IN.sys_info.publish_ns > F.module_cleared_ns) {
		t |= TRG_MODULE;
		mod_fmt(reasons[0], 64, "system failsafe: %s", IN.sys.reason);
	}
	/* The armed agreement holds in every state: ArduSub armed while hangar
	 * allows neither is a failsafe, disarmed or not (docs/10). */
	if (have_autopilot && F.ap_disagree) {
		t |= TRG_AUTOPILOT;
		mod_fmt(reasons[5], 64, "autopilot: %s", F.ap_reason);
	}
	if (!all)
		return t;
	if (P.tether_enabled) {
		/* until the bridge: there is no bridge.vehicle.link to watch. */
		t |= TRG_TETHER;
		mod_fmt(reasons[1], 64, "tether: no bridge.vehicle.link (until the bridge)");
	}
	int leak_fresh = IN.have_leak && mod_fresh(sub_leak, &IN.leak_info, now);
	int bat_fresh = IN.have_bat && mod_fresh(sub_battery, &IN.bat_info, now);
	int depth_fresh = IN.have_depth && mod_fresh(sub_depth, &IN.depth_info, now);
	if (leak_fresh && IN.leak.wet) {
		t |= TRG_LEAK;
		mod_fmt(reasons[2], 64, "leak: water at the probe");
	}
	if (bat_fresh && IN.bat.voltage_v < P.battery_min) {
		if (!F.battery_low_since)
			F.battery_low_since = now;
		if (now >= F.battery_low_since + mod_s_to_ns(P.battery_time)) {
			t |= TRG_BATTERY;
			mod_fmt(reasons[3], 64, "battery: %.2f V below %.2f V for %.1f s", (double)IN.bat.voltage_v,
				(double)P.battery_min, (double)P.battery_time);
		}
	} else {
		F.battery_low_since = 0;
	}
	const mux_params *mp = mux_p();
	if (depth_fresh && mp) {
		if (IN.depth.depth_m > mp->depth_max + P.depth_margin) {
			t |= TRG_DEPTH;
			mod_fmt(reasons[4], 64, "depth: %.2f m beyond %.2f m + %.2f", (double)IN.depth.depth_m,
				(double)mp->depth_max, (double)P.depth_margin);
		} else if (IN.depth.depth_m < mp->depth_min - P.depth_margin) {
			t |= TRG_DEPTH;
			mod_fmt(reasons[4], 64, "depth: %.2f m above %.2f m - %.2f", (double)IN.depth.depth_m,
				(double)mp->depth_min, (double)P.depth_margin);
		}
	}
	if (have_autopilot && !F.ap_disagree) {
		if (!IN.have_ap) {
			t |= TRG_AUTOPILOT;
			mod_fmt(reasons[5], 64, "autopilot: no ardusub.state");
		} else if (now > IN.ap.last_rx_ns + mod_s_to_ns(P.autopilot_silence)) {
			t |= TRG_AUTOPILOT;
			mod_fmt(reasons[5], 64, "autopilot: silent for %.1f s",
				(double)(now - IN.ap.last_rx_ns) / 1e9);
		} else if (IN.ap.mode != MANUAL_MODE) {
			t |= TRG_AUTOPILOT;
			mod_fmt(reasons[5], 64, "autopilot: mode %u is not MANUAL", IN.ap.mode);
		}
	}
	int input = 0;
	char *r = reasons[7];
	r[0] = 0;
	if (P.leak_required && !leak_fresh) {
		input = 1;
		mod_fmt(r, 64, "input: vehicle.leak %s", IN.have_leak ? "stale" : "absent");
	}
	if (P.battery_required && !bat_fresh) {
		input = 1;
		if (!r[0])
			mod_fmt(r, 64, "input: power.battery %s", IN.have_bat ? "stale" : "absent");
	}
	if (P.depth_required && !depth_fresh) {
		input = 1;
		if (!r[0])
			mod_fmt(r, 64, "input: nav.depth %s", IN.have_depth ? "stale" : "absent");
	}
	if (input)
		t |= TRG_INPUT;
	return t;
}

static uint8_t action_of(uint32_t bit)
{
	switch (bit) {
	case TRG_MODULE: return P.action_module;
	case TRG_TETHER: return P.action_tether;
	case TRG_LEAK: return P.action_leak;
	case TRG_BATTERY: return P.action_battery;
	case TRG_DEPTH: return P.action_depth;
	case TRG_AUTOPILOT: return P.action_autopilot;
	case TRG_INPUT: return P.action_input;
	default: return ACT_NEUTRAL; /* estop */
	}
}

/* Several triggers: the most conservative action wins (neutral over hold
 * over surface). hold needs the depth controller ready, else neutral. */
static uint8_t combine_actions(uint32_t triggers, uint64_t now)
{
	int any_hold = 0, any_surface = 0;
	for (int i = 0; i < 8; i++) {
		if (!(triggers & (1u << i)))
			continue;
		uint8_t a = action_of(1u << i);
		if (a == ACT_NEUTRAL)
			return ACT_NEUTRAL;
		if (a == ACT_HOLD)
			any_hold = 1;
		else
			any_surface = 1;
	}
	if (any_hold)
		return module_ready("depth_hold", now) ? ACT_HOLD : ACT_NEUTRAL;
	return any_surface ? ACT_SURFACE : ACT_NEUTRAL;
}

static void enter_failsafe(uint32_t triggers, const char *reason, const char *by, uint64_t now,
			   const struct hg_input *req)
{
	int fresh = S.state != ST_FAILSAFE;
	S.state = ST_FAILSAFE;
	S.triggers |= triggers;
	if (fresh) {
		F.entered_ns = now;
		F.hold_since_ns = 0;
		F.hold_expired = 0;
		F.disarmed_in_failsafe = 0;
		S.since_ns = now;
	}
	S.action = combine_actions(S.triggers, now);
	if (S.action == ACT_HOLD && !F.hold_since_ns)
		F.hold_since_ns = now;
	if (F.hold_expired || F.disarmed_in_failsafe)
		S.action = ACT_NEUTRAL;
	S.outputs_enabled = S.action != ACT_NEUTRAL;
	if (by)
		mod_fmt(S.by, sizeof S.by, "%s", by);
	if (reason)
		mod_fmt(S.reason, sizeof S.reason, "%s", reason);
	if (F.arm_call) {
		hg_call_result r = hg_call_poll(F.arm_call, NULL);
		(void)r;
		F.arm_call = NULL; /* the reply, when it comes, is discarded by its id */
	}
	latch_write();
	publish_state(req);
}

/* ---- The checklist (docs/07 "Arming") ------------------------------------------- */

static int checklist(uint64_t now, char *detail, size_t len)
{
	int ok = 1;
	detail[0] = 0;
#define FAIL(...)                                                                                     \
	do {                                                                                          \
		size_t n_ = strlen(detail);                                                           \
		mod_fmt(detail + n_, len - n_, "%s", n_ ? "; " : "");                                 \
		n_ = strlen(detail);                                                                  \
		mod_fmt(detail + n_, len - n_, __VA_ARGS__);                                          \
		ok = 0;                                                                               \
	} while (0)
	/* Every module named in `required` is ready. */
	char req[sizeof P.required];
	memcpy(req, P.required, sizeof req);
	req[sizeof req - 1] = 0;
	for (char *tok = strtok(req, " "); tok; tok = strtok(NULL, " "))
		if (!module_ready(tok, now))
			FAIL("%s not ready", tok);
	char reasons[8][64];
	uint32_t t = active_triggers(now, reasons, 1) & ~TRG_INPUT; /* the inputs are checked below, by name */
	for (int i = 0; i < 8; i++)
		if (t & (1u << i))
			FAIL("trigger %s", reasons[i]);
	if (P.leak_required) {
		if (!IN.have_leak || !mod_fresh(sub_leak, &IN.leak_info, now))
			FAIL("vehicle.leak %s", IN.have_leak ? "stale" : "absent");
		else if (IN.leak.wet)
			FAIL("vehicle.leak wet");
	}
	if (P.battery_required) {
		if (!IN.have_bat || !mod_fresh(sub_battery, &IN.bat_info, now))
			FAIL("power.battery %s", IN.have_bat ? "stale" : "absent");
		else if (IN.bat.voltage_v < P.battery_min)
			FAIL("power.battery %.2f V below %.2f V", (double)IN.bat.voltage_v, (double)P.battery_min);
	}
	if (P.depth_required && (!IN.have_depth || !mod_fresh(sub_depth, &IN.depth_info, now)))
		FAIL("nav.depth %s", IN.have_depth ? "stale" : "absent");
	/* until the bridge: bridge.vehicle.link up is not checked; there is none. */
	if (have_autopilot) {
		if (!IN.have_ap)
			FAIL("ardusub.state absent");
		else {
			if (now > IN.ap.last_rx_ns + mod_s_to_ns(P.autopilot_silence))
				FAIL("ArduSub silent");
			if (IN.ap.armed != 0)
				FAIL("ArduSub %s", IN.ap.armed == 1 ? "armed" : "disarming");
			if (IN.ap.mode != MANUAL_MODE)
				FAIL("ArduSub mode %u is not MANUAL", IN.ap.mode);
			if (IN.ap.params != 1)
				FAIL("ArduSub parameters %s", IN.ap.params == 0 ? "not read yet" : "differ from the file");
		}
	}
#undef FAIL
	return ok;
}

/* ---- The calls ------------------------------------------------------------------- */

static int on_arm(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	(void)req;
	(void)rep;
	uint64_t now = hg_now_ns();
	if (S.state == ST_FAILSAFE) {
		char t[80];
		mod_fmt(reason, HG_CALL_REASON, "failsafe (%s): clear first", trigger_names(S.triggers, t, sizeof t));
		return HG_E_REFUSED;
	}
	if (S.state == ST_ARMED || S.state == ST_ARMING)
		return HG_OK; /* idempotent */
	read_inputs(now);
	char detail[sizeof((check_rep *)0)->detail];
	if (!checklist(now, detail, sizeof detail)) {
		mod_fmt(reason, HG_CALL_REASON, "%.63s", detail);
		mod_fmt(S.reason, sizeof S.reason, "arm refused: %.49s", detail);
		publish_state(&c->req);
		return HG_E_REFUSED;
	}
	/* Accepted: arming is published first, then the autopilot is asked. */
	S.state = ST_ARMING;
	S.outputs_enabled = 0;
	S.action = 0;
	S.triggers = 0;
	S.since_ns = now;
	mod_fmt(S.by, sizeof S.by, "%s", c->caller);
	S.reason[0] = 0;
	F.arming_since = now;
	publish_state(&c->req);
	if (!have_autopilot) {
		S.state = ST_ARMED;
		S.outputs_enabled = 1;
		S.since_ns = now;
		publish_state(&c->req);
		return HG_OK;
	}
	arm_req ar = { .arm = 1 };
	int st = hg_call_start("mavlink.arm", &ARM_REQ_TYPE, &ar, &ARM_REP_TYPE, &F.arm_call,
			       (int)(P.arm_timeout * 1000) + 500);
	if (st != HG_OK) {
		S.state = ST_DISARMED;
		mod_fmt(S.reason, sizeof S.reason, "mavlink.arm: %s", hg_status_name(st));
		publish_state(&c->req);
		mod_fmt(reason, HG_CALL_REASON, "%s", S.reason);
		return HG_E_REFUSED;
	}
	return HG_OK;
}

static int on_disarm(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	(void)req;
	(void)rep;
	(void)reason;
	uint64_t now = hg_now_ns();
	if (S.state == ST_FAILSAFE) {
		/* Making the vehicle safer is never refused: outputs off until clear. */
		F.disarmed_in_failsafe = 1;
		S.action = ACT_NEUTRAL;
		S.outputs_enabled = 0;
		mod_fmt(S.reason, sizeof S.reason, "disarmed by %s in failsafe", c->caller);
		latch_write();
		publish_state(&c->req);
		return HG_OK;
	}
	if (S.state == ST_DISARMED)
		return HG_OK;
	S.state = ST_DISARMED;
	S.outputs_enabled = 0;
	S.since_ns = now;
	mod_fmt(S.by, sizeof S.by, "%s", c->caller);
	S.reason[0] = 0;
	if (F.arm_call) {
		(void)hg_call_poll(F.arm_call, NULL);
		F.arm_call = NULL;
	}
	publish_state(&c->req);
	return HG_OK;
}

static int on_estop(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	(void)req;
	(void)rep;
	(void)reason;
	F.latched |= TRG_ESTOP;
	enter_failsafe(TRG_ESTOP, "estop", c->caller, hg_now_ns(), &c->req);
	return HG_OK;
}

static int on_clear(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	(void)req;
	(void)rep;
	uint64_t now = hg_now_ns();
	if (S.state != ST_FAILSAFE)
		return HG_OK;
	read_inputs(now);
	char reasons[8][64];
	uint32_t active = active_triggers(now, reasons, 1);
	if (active & ~TRG_MODULE) {
		char t[80];
		int first = -1;
		for (int i = 0; i < 8 && first < 0; i++)
			if ((active & ~TRG_MODULE) & (1u << i))
				first = i;
		mod_fmt(reason, HG_CALL_REASON, "active: %s: %.40s", trigger_names(active, t, sizeof t),
			first >= 0 ? reasons[first] : "");
		return HG_E_REFUSED;
	}
	if (active & TRG_MODULE) {
		/* The manager leaves its failsafe only on request, and only when
		 * no module given up with OnGiveUp=failsafe is still failed. */
		char reply[256];
		const char *ctl = getenv("HANGAR_CONTROL");
		if (!ctl) {
			mod_fmt(reason, HG_CALL_REASON, "module: no manager to ask clear-failsafe");
			return HG_E_REFUSED;
		}
		if (hg_control_call(ctl, "clear-failsafe", 14, reply, sizeof reply, NULL, 0) < 0) {
			mod_fmt(reason, HG_CALL_REASON, "clear-failsafe: %s", strerror(errno));
			return HG_E_REFUSED;
		}
		if (!hg_control_ok(reply)) {
			char *nl = strchr(reply, '\n');
			if (nl)
				*nl = 0;
			mod_fmt(reason, HG_CALL_REASON, "manager: %.50s", strncmp(reply, "err ", 4) == 0 ? reply + 4 : reply);
			return HG_E_REFUSED;
		}
		F.module_cleared_ns = hg_now_ns();
	}
	S.state = ST_DISARMED;
	S.outputs_enabled = 0;
	S.action = 0;
	S.triggers = 0;
	S.since_ns = now;
	F.latched = 0;
	F.ap_disagree = 0;
	F.hold_since_ns = 0;
	F.hold_expired = 0;
	F.disarmed_in_failsafe = 0;
	mod_fmt(S.by, sizeof S.by, "%s", c->caller);
	mod_fmt(S.reason, sizeof S.reason, "failsafe cleared");
	latch_remove();
	publish_state(&c->req);
	return HG_OK;
}

static int on_check(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	(void)c;
	(void)req;
	(void)reason;
	check_rep *r = rep;
	uint64_t now = hg_now_ns();
	read_inputs(now);
	r->ok = (uint8_t)checklist(now, r->detail, sizeof r->detail);
	if (S.state == ST_FAILSAFE) {
		r->ok = 0;
		char t[80];
		size_t n = strlen(r->detail);
		mod_fmt(r->detail + n, sizeof r->detail - n, "%sfailsafe (%s): clear first", n ? "; " : "",
			trigger_names(S.triggers, t, sizeof t));
	}
	return HG_OK;
}

/* ---- The tick ---------------------------------------------------------------------- */

static void tick(uint64_t now)
{
	char reasons[8][64];
	int changed = 0;
	/* The arm in flight: a refusal, or the autopilot's report. */
	if (S.state == ST_ARMING) {
		if (F.arm_call) {
			hg_call_result r = hg_call_poll(F.arm_call, NULL);
			if (r.status != HG_PENDING) {
				F.arm_call = NULL;
				if (r.status != HG_OK) {
					S.state = ST_DISARMED;
					S.since_ns = now;
					mod_fmt(S.reason, sizeof S.reason, "mavlink.arm %s: %.40s", hg_status_name(r.status),
						r.reason);
					changed = 1;
				}
			}
		}
		if (S.state == ST_ARMING && IN.have_ap && IN.ap.armed == 1 && IN.ap_info.publish_ns >= F.arming_since) {
			S.state = ST_ARMED;
			S.outputs_enabled = 1;
			S.since_ns = now;
			changed = 1;
		} else if (S.state == ST_ARMING && have_autopilot && IN.have_ap && IN.ap.arm_result != 255 &&
			   IN.ap.arm_result != 0 && IN.ap_info.publish_ns >= F.arming_since) {
			S.state = ST_DISARMED;
			S.since_ns = now;
			mod_fmt(S.reason, sizeof S.reason, "ArduSub refused: %.45s", IN.ap.arm_reason);
			changed = 1;
		} else if (S.state == ST_ARMING && now >= F.arming_since + mod_s_to_ns(P.arm_timeout)) {
			S.state = ST_DISARMED;
			S.since_ns = now;
			if (IN.have_ap && IN.ap.arm_reason[0])
				mod_fmt(S.reason, sizeof S.reason, "no armed report in %.1f s: %.30s", (double)P.arm_timeout,
					IN.ap.arm_reason);
			else
				mod_fmt(S.reason, sizeof S.reason, "no armed report from ArduSub in %.1f s",
					(double)P.arm_timeout);
			if (F.arm_call) {
				(void)hg_call_poll(F.arm_call, NULL);
				F.arm_call = NULL;
			}
			changed = 1;
		}
	}
	/* The triggers. */
	uint32_t active = active_triggers(now, reasons, S.state != ST_DISARMED);
	if (active && S.state != ST_FAILSAFE) {
		uint32_t fresh = active;
		int first = -1;
		for (int i = 0; i < 8 && first < 0; i++)
			if (fresh & (1u << i))
				first = i;
		char t[80];
		enter_failsafe(fresh, first >= 0 ? reasons[first] : "", trigger_names(fresh, t, sizeof t), now, NULL);
		return;
	}
	if (S.state == ST_FAILSAFE) {
		uint32_t t = S.triggers | active | F.latched;
		uint8_t action = combine_actions(t, now);
		if (action == ACT_HOLD && !F.hold_since_ns)
			F.hold_since_ns = now;
		if (F.hold_since_ns && now >= F.hold_since_ns + mod_s_to_ns(P.hold_time) && !F.hold_expired) {
			F.hold_expired = 1;
			mod_fmt(S.reason, sizeof S.reason, "hold ended after %.1f s", (double)P.hold_time);
			changed = 1;
		}
		if (F.hold_expired || F.disarmed_in_failsafe)
			action = ACT_NEUTRAL;
		if (t != S.triggers || action != S.action) {
			uint32_t added = t & ~S.triggers;
			if (added) {
				int first = -1;
				for (int i = 0; i < 8 && first < 0; i++)
					if (added & (1u << i))
						first = i;
				if (first >= 0 && reasons[first][0])
					mod_fmt(S.reason, sizeof S.reason, "%s", reasons[first]);
			}
			S.triggers = t;
			S.action = action;
			S.outputs_enabled = action != ACT_NEUTRAL;
			latch_write();
			changed = 1;
		}
	}
	if (changed)
		publish_state(NULL);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	mod_init(WHO);
	const struct hg_wiring *w = hg_wiring();
	if (w)
		for (int i = 0; i < w->n_calls; i++)
			if (strcmp(w->calls[i].target, "mavlink.arm") == 0 && !w->calls[i].noroute)
				have_autopilot = 1;
	if (mod_state_dir(state_dir, sizeof state_dir) < 0)
		mod_log(WHO, "state directory %s: %s", state_dir, strerror(errno));
	mod_fmt(latch_path, sizeof latch_path, "%s/failsafe", state_dir);
	if (hg_params_open(&SAFETY_PARAMS_TYPE, &P, validate, NULL) < 0) {
		mod_log(WHO, "hg_params_open: %s", strerror(errno));
		return 4;
	}
	sub_modules = hg_sub_open(w && w->role[0] && strcmp(w->role, "topside") == 0 ? "hangar.topside.modules"
									 : "hangar.vehicle.modules", &HANGAR_MODULES_TYPE);
	sub_system = hg_sub_open(w && w->role[0] && strcmp(w->role, "topside") == 0 ? "hangar.topside.system"
									: "hangar.vehicle.system", &HANGAR_SYSTEM_TYPE);
	sub_leak = hg_sub_open("vehicle.leak", &LEAK_TYPE);
	sub_battery = hg_sub_open("power.battery", &BATTERY_TYPE);
	sub_depth = hg_sub_open("nav.depth", &DEPTH_TYPE);
	sub_ardusub = hg_sub_open("ardusub.state", &ARDUSUB_STATE_TYPE);
	sub_mux_params = hg_sub_open("mux.params", NULL);
	if (!sub_modules || !sub_system || !sub_leak || !sub_battery || !sub_depth || !sub_ardusub || !sub_mux_params) {
		mod_log(WHO, "hg_sub_open: %s", strerror(errno));
		return 4;
	}
	pub_state = hg_pub_open("safety.state", &SAFETY_STATE_TYPE, &(struct hg_pub_opts){ .rate_mhz = 10000 });
	if (!pub_state) {
		mod_log(WHO, "hg_pub_open(safety.state): %s", strerror(errno));
		return 4;
	}
	if (hg_serve_method("arm", on_arm, NULL) < 0 || hg_serve_method("disarm", on_disarm, NULL) < 0 ||
	    hg_serve_method("estop", on_estop, NULL) < 0 || hg_serve_method("clear", on_clear, NULL) < 0 ||
	    hg_serve_method("check", on_check, NULL) < 0) {
		mod_log(WHO, "hg_serve_method: %s", strerror(errno));
		return 4;
	}
	hg_sub *subs[HG_MAX_WAIT];
	int n = 0;
	subs[n++] = sub_modules;
	subs[n++] = sub_system;
	subs[n++] = sub_leak;
	subs[n++] = sub_battery;
	subs[n++] = sub_depth;
	subs[n++] = sub_ardusub;
	subs[n++] = sub_mux_params;
	int nc = hg_call_subs(subs + n, HG_MAX_WAIT - n);
	if (nc < 0) {
		mod_log(WHO, "hg_call_subs: %s", strerror(errno));
		return 4;
	}
	n += nc;

	memset(&S, 0, sizeof S);
	S.since_ns = hg_now_ns();
	if (latch_read())
		mod_log(WHO, "starting in failsafe from the latch: triggers %u, action %s", S.triggers, action_name(S.action));
	else
		mod_fmt(S.reason, sizeof S.reason, "start");
	mod_log(WHO, "%s autopilot behind the actuators", have_autopilot ? "with an" : "without an");
	uint64_t now = hg_now_ns();
	read_inputs(now);
	publish_state(NULL);
	uint64_t next = now + PERIOD_NS;
	while (!mod_stop) {
		now = hg_now_ns();
		int timeout = next > now ? (int)((next - now) / 1000000u) + 1 : 0;
		unsigned r = hg_wait(subs, n, timeout);
		if (r & HG_READY_INTERRUPT)
			break;
		now = hg_now_ns();
		read_inputs(now);
		hg_serve();
		if (hg_params_changed())
			mod_log(WHO, "parameters v%llu by %s", (unsigned long long)hg_params_version(), hg_params_changed_by());
		tick(now);
		if (now >= next) {
			publish_state(NULL);
			next += PERIOD_NS;
			if (next < now)
				next = now + PERIOD_NS;
		}
	}
	mod_log(WHO, "stopping (%s)", state_name(S.state));
	hg_pub_close(pub_state);
	return 0;
}
