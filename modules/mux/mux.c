// SPDX-License-Identifier: GPL-3.0-or-later
/* mux (binary hangar-mux): the one writer of ctl.wrench (docs/07-safety.md
 * "The mux"). Every control source publishes its own topic; the mux chooses
 * per axis by mode (manual, assisted, residual:<name>), gives the pilot any
 * axis above the deadband for as long as it is held and 1 s after, applies
 * the envelope (the largest command and rate per axis, the depth band) to
 * every source, holds a promoted policy's contribution within the residual
 * envelope and demotes it by the rules, carries out a failsafe's surface or
 * hold, and references the sources that drove each message so that the
 * stale flag follows them (docs/01 "Stale inputs"). mux.state says who
 * drives what. Parameters: msg/mux_params.msg. modules/mux/README.md lists
 * the deviations. */
#include "mod.h"

#include "depth.h"
#include "hg_call.h"
#include "hg_params.h"
#include "mux_params.h"
#include "mux_state.h"
#include "policy_active.h"
#include "safety_state.h"
#include "set_mode_rep.h"
#include "set_mode_req.h"
#include "wrench.h"

#define WHO "hangar-mux"
#define AXES 6
#define PERIOD_NS 20000000ull        /* 50 Hz */
#define STATE_PERIOD_NS 100000000ull /* 10 Hz */
#define MAX_POLICIES 4
#define SAT_RING 1024

enum { SRC_NONE = 0, SRC_PILOT = 1, SRC_CONTROLLER = 2, SRC_AUTO = 3, SRC_POLICY = 4 };
enum { MODE_MANUAL, MODE_ASSISTED, MODE_RESIDUAL };
enum { LIM_ENVELOPE = 0, LIM_RATE = 6, LIM_DEPTH = 12, LIM_RESIDUAL = 13 };

static mux_params P;
static hg_pub *pub_cmd, *pub_state;
static hg_sub *sub_pilot, *sub_ctl, *sub_depth, *sub_safety;

struct policy {
	char name[32];
	hg_sub *wrench, *active;
	struct wrench w;
	struct hg_info w_info;
	int have_w;
	struct policy_active a;
	struct hg_info a_info;
	int have_a;
};
static struct policy policies[MAX_POLICIES];
static int n_policies;

static struct {
	int mode;
	int policy;                 /* index, in residual */
	uint64_t promoted_hash;
	char mode_text[32];
	uint64_t override_until[AXES];
	float last_out[AXES];
	uint64_t last_tick_ns;
	unsigned char sat[SAT_RING];
	int sat_head;
	uint64_t last_policy_seq;
	char demoted_why[96];
	struct mux_state st, last_st;
	int st_published;
	uint64_t next_state_ns;
	char status[128];
} M;

static struct {
	struct wrench pilot, ctl;
	struct hg_info pilot_info, ctl_info;
	int have_pilot, have_ctl;
	struct depth depth;
	struct hg_info depth_info;
	int have_depth;
	struct safety_state safety;
	struct hg_info safety_info;
	int have_safety;
} IN;

static int validate(const void *candidate, const void *current, char reason[64])
{
	(void)current;
	const mux_params *c = candidate;
	if (c->depth_max <= c->depth_min) {
		mod_fmt(reason, 64, "depth_max must be above depth_min");
		return HG_E_REFUSED;
	}
	return HG_OK;
}

static float axis(const struct wrench *w, int a)
{
	return ((const float *)w)[a];
}

static void set_axis(struct wrench *w, int a, float v)
{
	((float *)w)[a] = v;
}

static const char *mode_name(void)
{
	return M.mode_text;
}

static void set_mode_text(void)
{
	if (M.mode == MODE_MANUAL)
		mod_fmt(M.mode_text, sizeof M.mode_text, "manual");
	else if (M.mode == MODE_ASSISTED)
		mod_fmt(M.mode_text, sizeof M.mode_text, "assisted");
	else
		mod_fmt(M.mode_text, sizeof M.mode_text, "residual:%s", policies[M.policy].name);
}

static void demote(const char *why)
{
	if (M.mode != MODE_RESIDUAL)
		return;
	M.mode = MODE_ASSISTED;
	set_mode_text();
	mod_fmt(M.demoted_why, sizeof M.demoted_why, "%s", why);
	mod_log(WHO, "demoted to assisted: %s", why);
	memset(M.sat, 0, sizeof M.sat);
}

/* ---- Reading --------------------------------------------------------------- */

static void read_inputs(uint64_t now)
{
	int was = IN.have_pilot;
	if (!mod_latest(sub_pilot, &IN.pilot, sizeof IN.pilot, &IN.pilot_info, &IN.have_pilot) && was)
		IN.have_pilot = 0; /* closed or dead: the pilot's input is gone */
	mod_latest(sub_ctl, &IN.ctl, sizeof IN.ctl, &IN.ctl_info, &IN.have_ctl);
	mod_latest(sub_depth, &IN.depth, sizeof IN.depth, &IN.depth_info, &IN.have_depth);
	mod_latest(sub_safety, &IN.safety, sizeof IN.safety, &IN.safety_info, &IN.have_safety);
	for (int i = 0; i < n_policies; i++) {
		struct policy *p = &policies[i];
		mod_latest(p->wrench, &p->w, sizeof p->w, &p->w_info, &p->have_w);
		mod_latest(p->active, &p->a, sizeof p->a, &p->a_info, &p->have_a);
	}
	(void)now;
}

/* ---- The choice (docs/07 "The mux") ------------------------------------------ */

static int finite_wrench(const struct wrench *w)
{
	for (int a = 0; a < AXES; a++)
		if (!isfinite(axis(w, a)))
			return 0;
	return 1;
}

/* The policy's output for this tick. 1: it counts, in *out. -1: a demotion
 * rule fired (stale, a skipped tick, not finite), why says which. 0: no
 * contribution this tick without demotion: the message is not from the
 * running model (its reference does not name the newest active message:
 * docs/07 "a message from any other model is no contribution for that
 * tick"), or there is no running model yet. The promoted hash is checked at
 * set_mode; a model swapped in by a load or a revert drives in its place
 * (docs/07 "Which model drives"). */
static int policy_contribution(struct policy *p, uint64_t now, struct wrench *out, char *why, size_t wl)
{
	if (!p->have_w) {
		mod_fmt(why, wl, "policy %s: no output", p->name);
		return -1;
	}
	/* A skipped tick is read as the topic's own staleness (three periods
	 * by default, docs/01), the same threshold every reader applies. */
	if (!mod_fresh(p->wrench, &p->w_info, now)) {
		mod_fmt(why, wl, "policy %s skipped a tick (%llu ms old)", p->name,
			(unsigned long long)((now - p->w_info.publish_ns) / 1000000u));
		return -1;
	}
	if (p->w_info.flags & HG_SLOT_STALE) {
		mod_fmt(why, wl, "policy %s: output computed from stale inputs", p->name);
		return -1;
	}
	if (!finite_wrench(&p->w)) {
		mod_fmt(why, wl, "policy %s: output not finite", p->name);
		return -1;
	}
	if (!p->have_a) {
		mod_fmt(why, wl, "policy %s: no running model (no active message)", p->name);
		return 0;
	}
	uint32_t active_id = hg_sub_topic_id(p->active);
	int refs_active = 0;
	for (uint32_t i = 0; i < p->w_info.n_inputs && i < HG_MAX_REFS; i++)
		if (p->w_info.inputs[i].topic_id == active_id && p->w_info.inputs[i].seq == p->a_info.seq)
			refs_active = 1;
	if (!refs_active) {
		mod_fmt(why, wl, "policy %s: output from a model other than the running one", p->name);
		return 0;
	}
	*out = p->w;
	return 1;
}

static void publish_mux_state(uint64_t now, int force)
{
	if (!force && memcmp(&M.st, &M.last_st, sizeof M.st) == 0 && now < M.next_state_ns)
		return;
	if (hg_publish(pub_state, &M.st, sizeof M.st, now) < 0)
		mod_log(WHO, "hg_publish(mux.state): %s", strerror(errno));
	M.last_st = M.st;
	M.next_state_ns = now + STATE_PERIOD_NS;
	char line[128];
	mod_fmt(line, sizeof line, "%s: sources %u%u%u%u%u%u limits %#x%s%s", M.st.mode, M.st.source_surge, M.st.source_sway,
		M.st.source_heave, M.st.source_roll, M.st.source_pitch, M.st.source_yaw, M.st.limits,
		M.demoted_why[0] ? "; demoted: " : "", M.demoted_why);
	if (strcmp(line, M.status) != 0) {
		mod_fmt(M.status, sizeof M.status, "%s", line);
		hg_status(line);
	}
}

static void tick(uint64_t now)
{
	float dt = M.last_tick_ns ? (float)(now - M.last_tick_ns) / 1e9f : 0.02f;
	if (dt > 0.2f)
		dt = 0.2f;
	M.last_tick_ns = now;
	struct wrench out = { 0 };
	uint8_t src[AXES] = { 0 };
	uint32_t limits = 0;
	struct hg_input refs[HG_MAX_REFS];
	int n_refs = 0;
	int used_pilot = 0, used_ctl = 0, used_policy = 0, used_depth = 0;

	/* The pilot: fresh and finite, else zero and not referenced. */
	int pilot_ok = IN.have_pilot && mod_fresh(sub_pilot, &IN.pilot_info, now) && finite_wrench(&IN.pilot) &&
		       !(IN.pilot_info.flags & HG_SLOT_STALE);
	int ctl_ok = IN.have_ctl && mod_fresh(sub_ctl, &IN.ctl_info, now) && finite_wrench(&IN.ctl);
	int safety_ok = IN.have_safety && mod_fresh(sub_safety, &IN.safety_info, now);
	int failsafe = safety_ok && IN.safety.state == 2;
	uint8_t action = failsafe ? IN.safety.action : 0;

	/* The residual: one policy's contribution on the controller's axes. */
	struct wrench res = { 0 };
	int have_res = 0;
	if (M.mode == MODE_RESIDUAL) {
		char why[96];
		int pc = policy_contribution(&policies[M.policy], now, &res, why, sizeof why);
		if (pc > 0)
			have_res = 1;
		else if (pc < 0)
			demote(why);
	}

	for (int a = 0; a < AXES; a++) {
		float pv = pilot_ok ? axis(&IN.pilot, a) : 0.0f;
		int ctl_axis = (P.controller_axes >> a) & 1;
		if (pilot_ok && fabsf(pv) > P.deadband)
			M.override_until[a] = now + mod_s_to_ns(P.override_hold);
		int override = pilot_ok && now < M.override_until[a];
		float v = 0;
		if (override) {
			v = pv;
			src[a] = SRC_PILOT;
			used_pilot = 1;
		} else if (failsafe && action == 1) {
			v = a == 2 ? P.surface_thrust : 0.0f; /* surface: heave up */
			src[a] = SRC_NONE;
		} else if (failsafe && action == 2) {
			if (a == 2 && ctl_ok) {
				v = axis(&IN.ctl, a); /* hold: the depth controller */
				src[a] = SRC_CONTROLLER;
				used_ctl = 1;
			}
		} else if (failsafe) {
			v = 0; /* neutral: the gate is closed anyway */
		} else if (M.mode == MODE_MANUAL) {
			v = pv;
			src[a] = pilot_ok ? SRC_PILOT : SRC_NONE;
			used_pilot |= pilot_ok;
		} else if (ctl_axis && ctl_ok) {
			v = axis(&IN.ctl, a);
			src[a] = SRC_CONTROLLER;
			used_ctl = 1;
			if (have_res) {
				float lim = P.residual_frac * P.max[a];
				float r = axis(&res, a);
				if (r > lim) {
					r = lim;
					limits |= 1u << (LIM_RESIDUAL + a);
				} else if (r < -lim) {
					r = -lim;
					limits |= 1u << (LIM_RESIDUAL + a);
				}
				v += r;
				used_policy = 1;
				src[a] = SRC_POLICY;
			}
		} else {
			v = pv; /* the pilot keeps the axes no controller holds */
			src[a] = pilot_ok ? SRC_PILOT : SRC_NONE;
			used_pilot |= pilot_ok;
		}
		/* The envelope, applied to every source after the choice. */
		if (v > P.max[a]) {
			v = P.max[a];
			limits |= 1u << (LIM_ENVELOPE + a);
		} else if (v < -P.max[a]) {
			v = -P.max[a];
			limits |= 1u << (LIM_ENVELOPE + a);
		}
		float step = P.rate_max[a] * dt;
		if (v > M.last_out[a] + step) {
			v = M.last_out[a] + step;
			limits |= 1u << (LIM_RATE + a);
		} else if (v < M.last_out[a] - step) {
			v = M.last_out[a] - step;
			limits |= 1u << (LIM_RATE + a);
		}
		set_axis(&out, a, v);
	}
	/* The depth band: no command pushing further out passes. */
	if (IN.have_depth && mod_fresh(sub_depth, &IN.depth_info, now) && isfinite(IN.depth.depth_m)) {
		float h = out.heave;
		if ((IN.depth.depth_m > P.depth_max && h < 0) || (IN.depth.depth_m < P.depth_min && h > 0)) {
			out.heave = 0;
			limits |= 1u << LIM_DEPTH;
			used_depth = 1;
		}
	}
	for (int a = 0; a < AXES; a++)
		M.last_out[a] = axis(&out, a);

	/* Saturation of the residual over the window: demotion. */
	if (M.mode == MODE_RESIDUAL) {
		int sat = (limits >> LIM_RESIDUAL) & 0x3f ? 1 : 0;
		M.sat[M.sat_head] = (unsigned char)sat;
		M.sat_head = (M.sat_head + 1) % SAT_RING;
		int window = (int)(P.demote_window / 0.02f);
		if (window > SAT_RING)
			window = SAT_RING;
		int count = 0;
		for (int i = 0; i < window; i++)
			count += M.sat[(M.sat_head - 1 - i + 2 * SAT_RING) % SAT_RING];
		if (window > 0 && (float)count > P.demote_saturation * (float)window) {
			char why[96];
			mod_fmt(why, sizeof why, "policy %s held at the residual envelope for %d of %d ticks",
				policies[M.policy].name, count, window);
			demote(why);
		}
	}

	/* The references: the sources that drove this message. */
	if (used_pilot && n_refs < HG_MAX_REFS)
		refs[n_refs++] = (struct hg_input){ .topic_id = hg_sub_topic_id(sub_pilot), .seq = IN.pilot_info.seq };
	if (used_ctl && n_refs < HG_MAX_REFS)
		refs[n_refs++] = (struct hg_input){ .topic_id = hg_sub_topic_id(sub_ctl), .seq = IN.ctl_info.seq };
	if (used_policy && n_refs < HG_MAX_REFS)
		refs[n_refs++] = (struct hg_input){ .topic_id = hg_sub_topic_id(policies[M.policy].wrench),
						    .seq = policies[M.policy].w_info.seq };
	if (used_depth && n_refs < HG_MAX_REFS)
		refs[n_refs++] = (struct hg_input){ .topic_id = hg_sub_topic_id(sub_depth), .seq = IN.depth_info.seq };
	unsigned flags = n_refs ? 0 : HG_NO_REFS;
	if (hg_publish_with(pub_cmd, &out, sizeof out, now, flags, refs, n_refs) < 0)
		mod_log(WHO, "hg_publish_with(ctl.wrench): %s", strerror(errno));

	mod_fmt(M.st.mode, sizeof M.st.mode, "%s", mode_name());
	M.st.source_surge = src[0];
	M.st.source_sway = src[1];
	M.st.source_heave = src[2];
	M.st.source_roll = src[3];
	M.st.source_pitch = src[4];
	M.st.source_yaw = src[5];
	M.st.residual_active = (uint8_t)used_policy;
	M.st.limits = limits;
	publish_mux_state(now, 0);
}

/* ---- set_mode -------------------------------------------------------------- */

static int on_set_mode(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	const set_mode_req *r = req;
	set_mode_rep *out = rep;
	char mode[32];
	mod_fmt(mode, sizeof mode, "%.31s", r->mode);
	if (strcmp(mode, "manual") == 0) {
		M.mode = MODE_MANUAL;
	} else if (strcmp(mode, "assisted") == 0) {
		M.mode = MODE_ASSISTED;
	} else if (strncmp(mode, "residual:", 9) == 0) {
		int k = -1;
		for (int i = 0; i < n_policies; i++)
			if (strcmp(policies[i].name, mode + 9) == 0)
				k = i;
		if (k < 0) {
			mod_fmt(reason, HG_CALL_REASON, "no policy %s in the wiring", mode + 9);
			return HG_E_REFUSED;
		}
		read_inputs(hg_now_ns());
		struct policy *p = &policies[k];
		if (!p->have_a) {
			mod_fmt(reason, HG_CALL_REASON, "policy %s has no running model", p->name);
			return HG_E_REFUSED;
		}
		if (p->a.hash != r->hash) {
			mod_fmt(reason, HG_CALL_REASON, "hash %016llx is not the running model's %016llx",
				(unsigned long long)r->hash, (unsigned long long)p->a.hash);
			return HG_E_REFUSED;
		}
		M.mode = MODE_RESIDUAL;
		M.policy = k;
		M.promoted_hash = r->hash;
		memset(M.sat, 0, sizeof M.sat);
	} else if (strcmp(mode, "auto") == 0 || strncmp(mode, "policy:", 7) == 0) {
		mod_fmt(reason, HG_CALL_REASON, "%s comes later: not a mode of version 1", mode);
		return HG_E_REFUSED;
	} else {
		mod_fmt(reason, HG_CALL_REASON, "unknown mode %s", mode);
		return HG_E_REFUSED;
	}
	M.demoted_why[0] = 0;
	set_mode_text();
	mod_log(WHO, "mode %s by %s", M.mode_text, c->caller);
	mod_fmt(out->mode, sizeof out->mode, "%s", M.mode_text);
	return HG_OK;
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	mod_init(WHO);
	if (hg_params_open(&MUX_PARAMS_TYPE, &P, validate, NULL) < 0) {
		mod_log(WHO, "hg_params_open: %s", strerror(errno));
		return 4;
	}
	sub_pilot = hg_sub_open("pilot.wrench", &WRENCH_TYPE);
	sub_ctl = hg_sub_open("ctl.depth_hold", &WRENCH_TYPE);
	sub_depth = hg_sub_open("nav.depth", &DEPTH_TYPE);
	sub_safety = hg_sub_open("safety.state", &SAFETY_STATE_TYPE);
	if (!sub_pilot || !sub_ctl || !sub_depth || !sub_safety) {
		mod_log(WHO, "hg_sub_open: %s", strerror(errno));
		return 4;
	}
	/* The policies: every policy.<name>.wrench the wiring subscribes to. */
	const struct hg_wiring *w = hg_wiring();
	for (int i = 0; w && i < w->n_subs && n_policies < MAX_POLICIES; i++) {
		const char *t = w->subs[i].topic;
		size_t n = strlen(t);
		if (strncmp(t, "policy.", 7) != 0 || n < 15 || strcmp(t + n - 7, ".wrench") != 0)
			continue;
		struct policy *p = &policies[n_policies];
		mod_fmt(p->name, sizeof p->name, "%.*s", (int)(n - 14), t + 7);
		char active[64];
		mod_fmt(active, sizeof active, "policy.%s.active", p->name);
		p->wrench = hg_sub_open(t, &WRENCH_TYPE);
		p->active = hg_sub_open(active, &POLICY_ACTIVE_TYPE);
		if (!p->wrench || !p->active) {
			mod_log(WHO, "policy %s: %s not in the wiring (%s)", p->name, p->wrench ? active : t, strerror(errno));
			return 4;
		}
		n_policies++;
	}
	pub_cmd = hg_pub_open("ctl.wrench", &WRENCH_TYPE, &(struct hg_pub_opts){ .rate_mhz = 50000 });
	pub_state = hg_pub_open("mux.state", &MUX_STATE_TYPE, &(struct hg_pub_opts){ .rate_mhz = 10000 });
	if (!pub_cmd || !pub_state) {
		mod_log(WHO, "hg_pub_open: %s", strerror(errno));
		return 4;
	}
	if (hg_serve_method("set_mode", on_set_mode, NULL) < 0) {
		mod_log(WHO, "hg_serve_method(set_mode): %s", strerror(errno));
		return 4;
	}
	hg_sub *subs[HG_MAX_WAIT];
	int n = 0;
	subs[n++] = sub_pilot;
	subs[n++] = sub_ctl;
	subs[n++] = sub_depth;
	subs[n++] = sub_safety;
	for (int i = 0; i < n_policies; i++) {
		subs[n++] = policies[i].wrench;
		subs[n++] = policies[i].active;
	}
	int nc = hg_call_subs(subs + n, HG_MAX_WAIT - n);
	if (nc < 0) {
		mod_log(WHO, "hg_call_subs: %s", strerror(errno));
		return 4;
	}
	n += nc;
	M.mode = MODE_MANUAL;
	set_mode_text();
	mod_log(WHO, "%d polic%s in the wiring; mode manual", n_policies, n_policies == 1 ? "y" : "ies");
	uint64_t now = hg_now_ns(), next = now + PERIOD_NS;
	M.next_state_ns = now;
	/* The loop ticks at 50 Hz from its own clock; a request or a message
	 * between ticks is read and served at once, the output waits for the tick. */
	while (!mod_stop) {
		now = hg_now_ns();
		int timeout = next > now ? (int)((next - now) / 1000000u) : 0;
		unsigned r = hg_wait(subs, n, timeout);
		if (r & HG_READY_INTERRUPT)
			break;
		hg_serve();
		if (hg_params_changed())
			mod_log(WHO, "parameters v%llu by %s", (unsigned long long)hg_params_version(), hg_params_changed_by());
		now = hg_now_ns();
		if (now + 1000000u >= next) { /* within a millisecond of the tick: take it */
			read_inputs(now);
			tick(now);
			next += PERIOD_NS;
			if (next < now)
				next = now + PERIOD_NS;
		}
	}
	/* A last neutral command: the gate closes behind a dead mux anyway. */
	struct wrench neutral = { 0 };
	hg_publish_with(pub_cmd, &neutral, sizeof neutral, hg_now_ns(), HG_NO_REFS, NULL, 0);
	mod_log(WHO, "stopping");
	return 0;
}
