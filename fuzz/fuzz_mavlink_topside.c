// SPDX-License-Identifier: GPL-3.0-or-later
/* The topside's MAVLink module against the bytes a ground station sends it
 * (modules/mavlink/mavlink_topside.c on_frame, behind mav_decode and the UDP
 * receive loop): QGroundControl's port on the topside computer, UDP input.
 * fuzz_mavlink.h has the input's framing; the serial bit is ignored here (the
 * topside has no serial line).
 *
 * mavlink_topside.c is included whole. Its two outputs are real topics in a
 * directory of this process's own; there is no manager, so an arm or disarm
 * (hg_call_start) is refused where it starts and its refusal sent back. */
#include "fuzz_mavlink.h"

#include "mavlink_topside.c"

static int ready;
static uint64_t now0;

static void setup(void)
{
	char err[256];
	fzm_topic_dir();
	if (mavlink_conf_load(&C, NULL, 1, err, sizeof err) < 0)
		abort();
	pub_wrench = hg_pub_open("pilot.wrench", &WRENCH_TYPE, NULL);
	pub_buttons = hg_pub_open("pilot.buttons", &PILOT_BUTTONS_TYPE, NULL);
	if (!pub_wrench || !pub_buttons)
		abort();
	now0 = hg_now_ns();
	ready = 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (!ready)
		setup();
	if (size < 1)
		return 0;
	unsigned mode = data[0] & ~FZM_SERIAL;
	data++;
	size--;
	/* Calls still pending from the input before: answered or timed out by now, or left. */
	for (int i = 0; i < MAX_PENDING; i++)
		if (T.pending[i].in_use)
			return 0; /* a call that started: only with a manager, which is not here */
	memset(&T, 0, sizeof T);
	for (int i = 0; i < MC_BUTTONS; i++)
		C.buttons[i] = (enum mc_button)(i % 4); /* none, arm, disarm, pass */
	uint64_t now = now0;
	fzm_feed(&L, mode, data, size, on_frame, &now);
	publish_pilot(now);
	FZ_CHECK(T.mc_count <= size);
	return 0;
}
