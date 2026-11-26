// SPDX-License-Identifier: GPL-3.0-or-later
/* thrusters: the actuator of docs/07 for a vehicle without ArduSub (phase 2,
 * and the simulator's standalone mode): ctl.wrench through the actuator gate,
 * mixed, published at 50 Hz as thr.cmd and as thr.out (the pulse widths sent,
 * as the mavlink module publishes ArduSub's servo outputs). Neutral, 1500 us,
 * whenever the gate is closed.
 *
 *   hangar-thrusters                               the module, mixing as ArduSub mixes (thr_mix.h)
 *   hangar-thrusters --frame FILE [--voltage V]    allocating through the frame's geometry (thr_alloc.h)
 *   hangar-thrusters --neutral                     the manager's Neutral command
 *
 * It waits on nothing but its 50 Hz tick: the gate's two topics are read at
 * each tick, and a subscription waited on but not read would spin (hangar#1). */
#include "mod.h"
#include "mod_frame.h"
#include "thr_alloc.h"
#include "thr_mix.h"

#include "hg_gate.h"
#include "thruster_cmd.h"
#include "thruster_out.h"
#include "wrench.h"

#define WHO "hangar-thrusters"
#define PERIOD_NS 20000000ull

static void neutral(uint16_t pwm[THR_N])
{
	for (int i = 0; i < THR_N; i++)
		pwm[i] = 1500;
}

int main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--neutral") == 0) {
		/* This module drives no hardware: its output is the topic thr.cmd,
		 * whose reader (the simulator) stops the thrusters when it is stale.
		 * A Neutral command runs outside the module's wiring (the manager
		 * gives it HANGAR_MODULE and the module's user, nothing else), so it
		 * could not publish 1500 us on thr.cmd either. Nothing to write.
		 *
		 * NOT a model for a driver of real ESCs. docs/07 "The actuator
		 * gate": a Neutral command puts the hardware to neutral by itself,
		 * since a PWM generator keeps its last pulse widths when the module
		 * that set them is dead. That driver's --neutral writes 1500 us to
		 * the hardware and exits non-zero when it cannot (the manager then
		 * puts the system in failsafe). */
		fprintf(stderr, "%s --neutral: no hardware here; thr.cmd goes stale and its reader stops the thrusters\n", WHO);
		return 0;
	}
	const char *frame_path = NULL;
	double voltage = 16.0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--frame") == 0 && i + 1 < argc) {
			frame_path = argv[++i];
			continue;
		}
		if (strcmp(argv[i], "--voltage") == 0 && i + 1 < argc) {
			char *end = NULL;
			voltage = strtod(argv[++i], &end);
			/* The table's range (lib/sim/sim_thrust.h). */
			if (end != argv[i] && !*end && voltage >= 10 && voltage <= 20)
				continue;
		}
		fprintf(stderr, "usage: %s [--frame FILE [--voltage 10..20]] | --neutral\n", argv[0]);
		return 64;
	}
	mod_init(WHO);
	struct thr_alloc A;
	if (frame_path) {
		struct sim_frame F;
		char ferr[160];
		if (mod_frame_load(frame_path, &F, ferr, sizeof ferr) < 0) {
			mod_log(WHO, "frame %s: %s", frame_path, ferr);
			hg_status("the frame file cannot be used (the log says why)");
			return 78;
		}
		if (thr_alloc_init(&A, &F, voltage) < 0) {
			mod_log(WHO, "frame %s: its thrusters produce no wrench", frame_path);
			hg_status("the frame file cannot be used (the log says why)");
			return 78;
		}
		mod_log(WHO, "frame %s: %d thrusters, rank %d, at %.1f V a command of 1 is surge %.1f N, sway %.1f N, "
			"heave %.1f N, roll %.2f N m, pitch %.2f N m, yaw %.2f N m",
			F.name, A.n, A.rank, voltage, A.full[0], A.full[1], A.full[2], A.full[3], A.full[4], A.full[5]);
		static const char *const axis[6] = { "surge", "sway", "heave", "roll", "pitch", "yaw" };
		for (int m = 0; m < 6; m++)
			if (A.full[m] <= 0)
				mod_log(WHO, "frame %s cannot produce %s: that axis of the command is ignored", F.name,
					axis[m]);
	}
	hg_gate *G = hg_gate_open();
	if (!G) {
		mod_log(WHO, "hg_gate_open: %s (an [Actuator] section with Command=ctl.wrench is required)",
			strerror(errno));
		return 4;
	}
	hg_pub *pub_cmd = hg_pub_open("thr.cmd", &THRUSTER_CMD_TYPE, &(struct hg_pub_opts){ .rate_mhz = 50000 });
	hg_pub *pub_out = hg_pub_open("thr.out", &THRUSTER_OUT_TYPE, &(struct hg_pub_opts){ .rate_mhz = 50000 });
	if (!pub_cmd || !pub_out) {
		mod_log(WHO, "hg_pub_open: %s", strerror(errno));
		return 4;
	}
	mod_log(WHO, "thr.cmd and thr.out at 50 Hz from ctl.wrench through the gate, %s",
		frame_path ? "allocated through the frame" : "ArduSub's vectored mixing");
	char shown[160] = "";
	unsigned long long open_ticks = 0, scaled = 0;
	uint64_t next = hg_now_ns();
	while (!mod_stop) {
		uint64_t now = hg_now_ns();
		int timeout = next > now ? (int)((next - now + 999999u) / 1000000u) : 0;
		if (mod_sleep_ms(timeout) & HG_READY_INTERRUPT)
			break;
		now = hg_now_ns();
		if (now < next)
			continue;
		next += PERIOD_NS;
		if (next < now)
			next = now + PERIOD_NS;
		const char *why = NULL;
		const struct wrench *w = hg_gate_check(G, &why);
		struct thruster_cmd c;
		if (w) {
			const float in[6] = { w->surge, w->sway, w->heave, w->roll, w->pitch, w->yaw };
			if (frame_path) {
				struct thr_alloc_out o;
				thr_alloc_mix(&A, in, &o);
				memcpy(c.pwm, o.pwm, sizeof c.pwm);
				scaled += o.scale_hold < 1 || o.scale_move < 1;
			} else {
				scaled += thr_mix(in, c.pwm) < 1;
			}
			open_ticks++;
		} else {
			neutral(c.pwm);
		}
		struct thruster_out o;
		memcpy(o.pwm, c.pwm, sizeof o.pwm);
		/* A neutral output with the gate closed has no input behind it and
		 * names none (docs/07 "The actuator gate", docs/01 "Provenance"):
		 * otherwise it would reference the command it did not act on, and
		 * carry that command's stale flag. */
		unsigned flags = w ? 0 : HG_NO_REFS;
		hg_publish_with(pub_cmd, &c, sizeof c, now, flags, NULL, 0);
		hg_publish_with(pub_out, &o, sizeof o, now, flags, NULL, 0);
		char line[160];
		if (w)
			mod_fmt(line, sizeof line, "gate open; %llu ticks open, %llu scaled down", open_ticks, scaled);
		else
			mod_fmt(line, sizeof line, "gate closed (%s); %llu ticks open, %llu scaled down", why ? why : "?",
				open_ticks, scaled);
		/* The count changes every tick: the line is refreshed once a second. */
		if ((open_ticks % 50) == 0 || strncmp(line, shown, 9) != 0) {
			mod_fmt(shown, sizeof shown, "%s", line);
			hg_status(line);
		}
	}
	struct thruster_cmd c;
	neutral(c.pwm);
	hg_publish_with(pub_cmd, &c, sizeof c, hg_now_ns(), HG_NO_REFS, NULL, 0);
	mod_log(WHO, "stopping: neutral sent");
	return 0;
}
