# Handover: e2e-depth-ff (docs/09 adaptive depth hold, end to end in simulation)

Stopped on 2026-11-20 for a reboot. The CPU was throttled to about 400 MHz for the
last hour or so, so every timing number from that period is invalid. No test was
changed to accommodate the slowness.

## Done (committed on e2e-depth-ff)

- `353b288` **thrusters module** (`modules/thrusters`): ctl.wrench through the
  actuator gate → `thr.cmd` + `thr.out` at 50 Hz, ArduSub's vectored mixing.
  Suite `make check-thrusters` checks the mixing against lib/sim's
  `sim_applied_wrench`. The WIP commit adds `User=hg-thrusters` to its manifest:
  hangard refuses `Actuator=yes` without a dedicated User.
- `436fc75` **depth_ff.py label fix**: the heavy frame's four verticals (outputs
  5–8, factor −0.25: heave up is below 1500 µs). The old label had the wrong sign
  and only two thrusters. Also new `--label trim` (applied heave + ff·accel_z).
  docs/09's "shortfall" label is identically 0 when only the controller drives,
  because the integrator already supplies the trim.
- `635c0e5` **policy bug fix**: a dataset part whose topic appears after the
  policy starts was never attached, so the policy stayed unbound and was
  restarted at StartGrace. The suite now has `policy_late`; it fails without the
  fix.
- `32eea4a` **hangar.topic** honors `$HANGAR_ROOT` when `$HANGAR_TOPIC_DIR` is
  unset (test in test_learn).
- PyTorch 2.15 CPU in **`~/.cache/hangar-e2e-venv`** (survives the reboot). The
  Python suite passes with torch, which runs the export test for the first time.

## WIP (committed as "WIP: ...", not finished)

- `profiles/sim-depth-ff/`: sim_vehicle standalone, nav, depth_hold, thrusters,
  safety (no mavlink; `required="mux thrusters"`, `leak_required=0`), mux (+ the
  policy's topics), pairer (dataset depth_ff with nav_state's real field names),
  recorder, policy_depth_ff.
- `tools/sim/e2e-depth-ff.sh [up|run|down|status]`: lays out
  `/dev/shm/hangar-e2e`, rewrites the policy's `Arch` from an export of the
  example's model, starts hangard, runs the orchestrator, tears it down.
- `tools/sim/e2e_depth_ff.py`: arms, goes assisted, trains in a thread, deploys
  M1, then runs blocks B1–B5 (assisted/residual × nominal/heavier, 3→4→3 m
  steps), buoyancy transients T1–T3, an M2 deploy mid-hold (swap cost) and
  `reset_state` (mismatch). It then analyses the recorder session: steps,
  transients, latency through the reference chain, swap cost, mismatch with
  python/hangar's reference on the pairer's rows, lineage, demotions. **The
  analysis code has never run to the end.** Expect bugs in it.
- `torch.set_num_threads(1)` was added after the first smoke run: the default
  pool took 5.5 cores on a 16-unit LSTM, and training managed only about
  1.2 steps/s.

## Results so far (THROTTLED CPU: no timing number is valid)

`bench/e2e-depth-ff/smoke-2026-11-20-throttled.events.log` is a partial
`--quick` run.

- **What worked:**
  - the closed loop holds 3.00 m with integrator −0.08: that is the trim, with net buoyancy +2 N;
  - the arm, assisted, the train stream, M1 deploy (done, 1.1 s), `set_mode residual:depth_ff` with the hash, and the live buoyancy change all worked;
  - the recorder started its session on arming.
- **Observations:**
  - the dive at 0.13 m/s is slow. depth_hold's `ff=0.1` assumes linear thrust, but heave 0.1 is only 40 µs from stop, in the T200 deadband;
  - the trainer's stream lost 279 rows by 180 s. Probably the slow CPU, but recheck it;
  - the run was killed during T2. No analysis output exists.

## Next steps

1. After the reboot: `make -j8 all`, `make check-policy check-thrusters`, and the
   Python suite.
2. Smoke run:
   `E2E_ARGS=--quick E2E_OUT=$PWD/bench/e2e-depth-ff/quick tools/sim/e2e-depth-ff.sh run`
   (about 6 min). Fix the analysis until `report.json` is complete. The analysis
   can be rerun alone with `--analyse <session dir>` (events.json is saved there),
   but the session lives in /dev/shm, so copy it out first.
3. The full run: the same command without `--quick` (about 15 min). Check `lost`
   is 0 and the training rate is now well above 1 step/s. Run it on an otherwise
   idle machine for latency and swap numbers.
4. Report: assisted vs residual steady-state and step error, nominal vs heavier;
   the transients; latency; swap cost; mismatch; lineage; demotions. Expect
   residual ≈ assisted at steady state, because the integrator already removes
   the trim. Any gain should show on steps and right after a deploy.
5. Then: a README note for profiles/sim-depth-ff and a check of `release.sh` /
   install lists for `hangar-thrusters`.
