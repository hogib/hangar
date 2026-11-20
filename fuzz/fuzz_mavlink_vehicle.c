// SPDX-License-Identifier: GPL-3.0-or-later
/* The vehicle's MAVLink module against the bytes ArduSub's link gives it
 * (modules/mavlink: mav_decode and the signing check in mav.c, the serial and
 * UDP receive loops in mavlink_main.c, on_frame and what it calls in
 * mavlink_vehicle.c, the parameter job in param_backup.c). Serial or UDP
 * input: a Pixhawk on a UART or USB, or anything that can send to the module's
 * UDP port. fuzz_mavlink.h has the input's framing.
 *
 * mavlink_vehicle.c is included whole (on_frame and the module's state are
 * static); its outputs are real topics, written without a manager into a
 * directory of this process's own. The gate, the manager and the methods are
 * not here: tick() and the call handlers are not run. */
#include "fuzz_mavlink.h"

#include "mavlink_vehicle.c"

static int ready;
static uint64_t now0;

static void setup(void)
{
	char err[256];
	fzm_topic_dir();
	if (mavlink_conf_load(&C, NULL, 0, err, sizeof err) < 0)
		abort();
	A.n_params = mavlink_params_load(FUZZ_REPO "/modules/mavlink/ardusub.params", A.params, MC_MAX_PARAMS, err, sizeof err);
	if (A.n_params < 0)
		A.n_params = 0;
	pub_mc = hg_pub_open("ardusub.manual_control", &MANUAL_CONTROL_TYPE, NULL);
	pub_state = hg_pub_open("ardusub.state", &ARDUSUB_STATE_TYPE, NULL);
	pub_param = hg_pub_open("ardusub.param", &ARDUSUB_PARAM_TYPE, NULL);
	pub_att = hg_pub_open("nav.attitude", &ATTITUDE_TYPE, NULL);
	pub_imu = hg_pub_open("nav.imu", &IMU_TYPE, NULL);
	pub_depth = hg_pub_open("nav.depth", &DEPTH_TYPE, NULL);
	pub_bat = hg_pub_open("power.battery", &BATTERY_TYPE, NULL);
	pub_thr = hg_pub_open("thr.out", &THRUSTER_OUT_TYPE, NULL);
	pub_job = hg_pub_open("ardusub.params_job", &PARAMS_JOB_TYPE, NULL);
	if (!pub_mc || !pub_state || !pub_param || !pub_att || !pub_imu || !pub_depth || !pub_bat || !pub_thr || !pub_job)
		abort();
	struct pj_send send = { pj_send_list, pj_send_read_index, pj_send_set };
	pj_init(fzm_dir, &send);
	now0 = hg_now_ns();
	ready = 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (!ready)
		setup();
	if (size < 1)
		return 0;
	unsigned mode = data[0];
	data++;
	size--;
	fzm_decode_alone(data, size, 0);
	fzm_decode_alone(data, size, 1);

	/* The module as mavlink_vehicle_main leaves it before its loop. */
	struct mc_param params[MC_MAX_PARAMS];
	int n_params = A.n_params;
	memcpy(params, A.params, sizeof params);
	memset(&A, 0, sizeof A);
	memcpy(A.params, params, sizeof params);
	A.n_params = n_params;
	C.target_sysid = (mode & FZM_ANY_ID) ? 0 : 1;
	C.target_compid = (mode & FZM_ANY_ID) ? 0 : 1;
	A.sysid = 1;
	A.arm_result = 255;
	A.collecting = (mode & FZM_ANY_ID) != 0; /* an arm attempt under way: PreArm texts are collected */
	clock_reset();
	pj_abort("a new input", now0);
	char reason[128];
	if (mode & FZM_SERIAL)
		(void)pj_start_dump("fuzz", now0, reason, sizeof reason); /* a dump under way: every PARAM_VALUE counts */

	uint64_t now = now0;
	fzm_feed(&L, mode, data, size, on_frame, &now);
	publish_state(now);
	struct params_job j;
	for (int i = 0; i < 4; i++) {
		now += 400000000;
		pj_tick(now);
		(void)pj_status(&j, now);
	}
	FZ_CHECK(A.n_params == n_params);
	FZ_CHECK(strnlen(A.arm_reason, sizeof A.arm_reason) < sizeof A.arm_reason);
	FZ_CHECK(strnlen(A.failsafe, sizeof A.failsafe) < sizeof A.failsafe);
	FZ_CHECK(strnlen(A.mismatch, sizeof A.mismatch) < sizeof A.mismatch);
	return 0;
}
