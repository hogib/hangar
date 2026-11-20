# e2e-depth-ff: the learning loop end to end, in simulation

[docs/09](../../docs/09-learning.md#example-adaptive-depth-hold)'s adaptive depth hold,
run for the first time as a whole: `sim_vehicle` standalone, `thrusters`, `safety`, `mux`,
`nav`, `depth_hold`, `pairer`, `recorder` and `policy_depth_ff` under one manager
([profiles/sim-depth-ff](../../profiles/sim-depth-ff/README.md)), a trainer in PyTorch
on the live `dataset.depth_ff`, a deploy, `mux.set_mode residual:depth_ff`, a live
buoyancy change, a second deploy. No ArduSub, no Docker, one machine.

```sh
make -j8 all
python3 -m venv ~/.cache/hangar-e2e-venv
~/.cache/hangar-e2e-venv/bin/pip install --index-url https://download.pytorch.org/whl/cpu torch numpy
E2E_OUT=/tmp/e2e tools/sim/e2e-depth-ff.sh run                 # 14 min; --quick: 6 min, a smoke run
E2E_OUT=/tmp/e2e E2E_ARGS="--label shortfall" tools/sim/e2e-depth-ff.sh run
E2E_OUT=/tmp/e2e E2E_WHERE=armed E2E_BOUND=0.15 tools/sim/e2e-depth-ff.sh run
PYTHONPATH=python ~/.cache/hangar-e2e-venv/bin/python tools/sim/e2e_depth_ff.py \
    --out /tmp/e2e --analyse /tmp/e2e/session                  # the analysis alone, again
```

`tools/sim/e2e-depth-ff.sh` lays out a root (`E2E_ROOT`, by default `/dev/shm/hangar-e2e`),
starts the manager, runs `tools/sim/e2e_depth_ff.py` and stops it. The Python script is
the operator and the trainer: it arms, sets `assisted`, trains
`python/examples/depth_ff.py`'s model in a thread, deploys, promotes, changes
`sim_vehicle`'s `buoyancy`, and then reads everything it reports from the recorder's
session, following the references each message carries. It writes `events.log`,
`report.json`, `report.md` and a copy of the session (200 MB) into `E2E_OUT`.

## The schedule

| t (s) | | |
|---|---|---|
| 0 | arm, `assisted`, dive to 3 m | 40 s |
| 40 | training starts; targets between 2 and 5 m, 15 s each | 180 s, then 20 s at 3 m |
| 240 | **deploy M1** (not promoted) | |
| 241 | **B1** assisted, nominal: hold 3 m (20 s), step to 4 m (25 s), step to 3 m (25 s) | |
| 311 | **T1** buoyancy 114.8 to 110.8 N at the hold, 40 s; **T1:back** to 114.8 N, 40 s | |
| 391 | **H1** `set_mode residual:depth_ff` with M1's hash, 20 s at the hold | |
| 411 | **B2** residual M1, nominal | |
| 481 | **T2** buoyancy to 110.8 N, 40 s | |
| 521 | **B3** residual M1, heavier | |
| 601 | **S2** **deploy M2** at the hold; 10 s later, if the mux had demoted the policy, **H1b** `set_mode residual:depth_ff` with M2's hash | |
| 612 | `policy_depth_ff.reset_state` (for the mismatch check), then **B4** residual M2, heavier | |
| 692 | **H2** `set_mode assisted`, 20 s; **B5** assisted, heavier | |
| 782 | **T3** buoyancy back to 114.8 N, 40 s; disarm | |

With H1b, everything after it is 20 s later. The vehicle floats 2 N up at 114.8 N and
sinks 2 N at 110.8 N; holding it takes heave −0.085 and +0.088 (the "trim" below; heave
1 is 400 µs on the four verticals).

## The runs of 2026-11-20

AMD Ryzen 5 5600H (12 threads, `performance`, 4.28 GHz: not throttled), Linux 7.3.4,
debugoptimized build, Python 3.14.10, PyTorch 2.15.0 (CPU, one thread), numpy 2.5.4.
One run per variant, simulator seed 1, torch seed 0: no repeats, so no spread is known
for any number here. Run B ran alone (another agent's test suites were running on the
machine, load average 0.7 at its start); A with C, and D with E, ran in pairs at the
same time. The reports are in [2026-11-20/](2026-11-20/).

| Run | Rows with loss (`Where`) | Label | Model |
|---|---|---|---|
| **A** shortfall | docs/09's: outputs enabled, \|vel_z\| < 0.05 m/s | docs/09's: heave applied (from `thr.out`) − `ctl.depth_hold.heave` | docs/09's |
| **B** trim | docs/09's | heave applied + `ff`·`accel_z`: the feed-forward that held the vehicle | docs/09's |
| **C** trim, armed | every row with outputs enabled (`E2E_WHERE=armed`) | as B | docs/09's |
| **D** trim, bounded | docs/09's | as B | a `tanh` before the last affine: output within ±0.15 (`E2E_BOUND`) |
| **E** trim, armed, bounded | as C | as B | as D |

A sixth run, **Q**, is the `--quick` smoke run (every phase 0.4 of its length, B's
label, rows and model, 72 s of training before the first deploy instead of 200) that
checked the committed tree from a clean copy. Its holds are 8 s and its steps 10 s, so
its numbers are not comparable with the table's; it is cited where it contradicts.

## Results

**In short.** The machinery works: rows, training, export, load, swap, promotion,
references and demotion all did what docs/07 and docs/09 say, with no row lost and no
tick late. The learned residual did not improve the depth hold in any run. With docs/09's
model (A, B, C) the mux demoted the policy 5 to 54 s after each of the six promotions,
always for sitting at the residual envelope; with a model that cannot reach the
envelope (D, E) it stayed promoted for 301 s, and the depth hold was worse than
`assisted` in every block (rms 3 to 34 cm against 2) and on 10 of the 12 steps.
Whether a model is demoted is not settled by its kind: in Q the model of run B's kind
was never demoted, and all six of its steps were worse than `assisted` too.

### Depth error, with and without the residual

Truth (`sim.pose`) against the target. "Without" is `assisted` in the same run; its
numbers were the same in all five runs to the last digit shown or the one before. The
hold's −2 cm is the simulated depth sensor's bias (`depth_bias` 0.02 m), not the
controller. The percentage is the share of the block's `ctl.wrench` messages that
carried a residual: under 100 % the mux demoted the policy before or during the block.
IAE is the integral of |error| over the segment (25 s for a step).

| | Without | A | B | C | D | E |
|---|---|---|---|---|---|---|
| **Nominal buoyancy** (B1 without, B2 with M1): residual in the command | 0 % | 2 % | 0 % | 49 % | 100 % | 100 % |
| hold at 3 m, mean / rms (cm) | −1.95 / 1.95 | −3.06 / 3.18 | −1.97 / 1.98 | 2.90 / 7.94 | −3.84 / 30.50 | −1.78 / 4.28 |
| step 3 to 4 m: IAE (m s), overshoot | 1.85, 8.7 % | 1.86, 9.4 % | 1.85, 9.4 % | 6.26, 32.8 % | 2.33, 23.4 % | 4.30, 27.3 % |
| step 4 to 3 m: IAE (m s), overshoot | 3.01, 19.7 % | 3.02, 19.6 % | 3.02, 19.7 % | 3.00, 21.4 % | 7.20, 63.5 % | 3.94, 41.0 % |
| **4 N heavier, the model from before the change** (B5 without, B3 with M1) | 0 % | 0 % | 0 % | 0 % | 100 % | 100 % |
| hold at 3 m, mean / rms (cm) | −2.00 / 2.00 | −1.93 / 1.93 | −1.92 / 1.92 | −1.93 / 1.93 | −9.55 / 32.66 | −2.21 / 3.00 |
| step 3 to 4 m: IAE (m s), overshoot | 2.72, 15.9 % | 2.72, 16.3 % | 2.72, 16.1 % | 2.73, 16.2 % | 2.43, 44.5 % | 4.97, 37.2 % |
| step 4 to 3 m: IAE (m s), overshoot | 2.19, 14.1 % | 2.17, 14.0 % | 2.20, 14.1 % | 2.20, 14.4 % | 7.67, 59.8 % | 3.08, 34.7 % |
| **4 N heavier, the model trained since** (B5 without, B4 with M2) | 0 % | 32 % | 31 % | 34 % | 100 % | 100 % |
| hold at 3 m, mean / rms (cm) | −2.00 / 2.00 | −1.95 / 1.95 | −0.53 / 6.13 | −0.38 / 33.78 | −4.68 / 34.31 | −7.24 / 23.07 |
| step 3 to 4 m: IAE (m s), overshoot | 2.72, 15.9 % | 2.20, 12.0 % | 3.43, 33.2 % | 3.04, 30.9 % | 2.29, 19.8 % | 6.56, 57.3 % |
| step 4 to 3 m: IAE (m s), overshoot | 2.19, 14.1 % | 2.18, 14.3 % | 2.21, 14.3 % | 2.18, 14.4 % | 7.77, 42.4 % | 5.60, 32.0 % |

The transients at the 3 m hold: peak error (cm), IAE (m s) over the window, and when
the error was last outside 5 cm ("-": still outside at the window's end). An
undisturbed 20 s hold reads 2.2, 0.40, 0.0 (the bias).

| | Without | A | B | C | D | E |
|---|---|---|---|---|---|---|
| **Buoyancy −4 N** (T1 without, T2 with M1), 40 s | 33.0, 1.97, 8.2 s | demoted before | demoted before | demoted before | 57.2, 12.09, - | 24.2, 2.83, 26.4 s |
| **Promotion** (H1), 20 s | | 16.2, 0.53, - | 22.4, 1.46, 13.6 s | 20.8, 1.97, - | 56.1, 4.35, - | 11.9, 0.94, - |
| the residual then lasted | | 21.6 s | 5.0 s | 54.3 s | 301 s, until `assisted` was set | 301 s |
| **Promotion again, with M2** (H1b), 20 s | | 2.2, 0.39, 0.0 s | 29.4, 1.67, 15.4 s | 47.8, 4.16, - | not needed | not needed |
| the residual then lasted | | 52.5 s | 51.9 s | 54.0 s | | |
| **Swap M1 to M2 while promoted** (S2), 20 s | | | | | 49.6, 5.51, - | 38.4, 3.77, - |
| **Back to assisted** (H2), 20 s | | | | | 24.3, 1.06, 5.6 s | 23.9, 1.70, 10.3 s |
| Buoyancy +4 N, assisted (T1:back, T3), 40 s | 36.3, 2.54, 10.4 s | | | | | |

Who carried the trim, at the end of each block's hold (heave):

| | Without (B1, B5) | B: B2, B3, B4 | D: B2, B3, B4 | E: B2, B3, B4 |
|---|---|---|---|---|
| the integrator | −0.085, +0.089 | −0.085, +0.088, −0.016 | −0.055, +0.037, +0.031 | −0.031, +0.096, +0.023 |
| the policy's output | (not applied) | −0.084 (not applied), −0.084 (not applied), +0.080 | −0.031, −0.062, −0.013 | −0.015, −0.036, +0.088 |

In A the policy's output at the holds was 0.000 to 0.001: it had learned docs/09's
label, which is zero (below).

### Loop latency: from the newest row trained on to the first promoted output

Milliseconds after the stamp of the newest row the trainer held when it exported, which
the weights file's header and `policy.depth_ff.active` name; from the recorded times of
the messages that reference each other. Ranges over the ten loads of the five runs.

| | ms |
|---|---|
| the row published by the pairer | 0.01 to 0.04 |
| the trainer's deploy called (its last step done) | 2.6 to 22.2 |
| exported (13 KB), copied, `load_weights` accepted: checking | 13.3 to 33.0 |
| checks 1 to 7 passed: warming | 19.4 to 40.7 |
| swapped (`policy.depth_ff.active`) | 999.5 to 1020.8 |
| first `policy.depth_ff.wrench` of the model | 1019.5 to 1040.8 |
| first `ctl.wrench` carrying it, the policy promoted (M2 in D, E) | 1039.0, 1058.4 |
| **first `thr.cmd` carrying it** (M2 in D, E) | **1043.6, 1062.1** |

980 ms of the 1.04 to 1.06 s are the warm-up: `hangar.learn.export` sets `warmup_ticks`
to the chunk length, 50 ticks. The first model waits for the operator instead: 151.0 s
here (the schedule's B1 and T1). A model deployed while the policy is demoted drives
only when it is promoted again: 11.0 s in A, B and C (the schedule's 10 s).

The control loop itself, `nav.depth` published to `thr.cmd` published through the
references (run B, 42 102 commands): median **59.5 ms**, p99 60.2, max 62.2. Three of
its four hops are a module waiting for its own 50 Hz timer rather than for its input:
`nav.depth` to `nav.state` 16.9 ms, to `ctl.depth_hold` 19.1 ms, to `ctl.wrench`
19.3 ms, to `thr.cmd` 4.0 ms. The policy's branch: `nav.state` to
`policy.depth_ff.wrench` 0.03 ms (it ticks on its anchor), to `ctl.wrench` 18.7 ms.

### Swap cost: tick jitter during a load

Intervals between messages of each 50 Hz topic from the deploy's start to 200 ms after
the swap (60 ticks: export, checks, 50 ticks of warm-up with two models stepping, the
swap), against the 10 s before. Run B, the second load:

| Topic | Before: min / max (ms), sd | During the load: min / max (ms), sd | Over 30 ms |
|---|---|---|---|
| `nav.state` | 19.01 / 20.79, 0.20 | 19.04 / 20.38, 0.34 | 0 |
| `policy.depth_ff.wrench` | 18.99 / 20.77, 0.21 | 19.04 / 20.38, 0.33 | 0 |
| `ctl.depth_hold` | 19.01 / 20.58, 0.21 | 19.02 / 20.57, 0.38 | 0 |
| `ctl.wrench` | 19.02 / 20.64, 0.22 | 19.04 / 20.47, 0.42 | 0 |
| `thr.cmd` | 19.04 / 20.65, 0.29 | 19.05 / 20.49, 0.39 | 0 |

Anchor to the policy's output: median 0.047 ms, max 0.342 ms during the load; 0.033 and
0.493 ms before. Over all ten loads no interval of any of the five topics exceeded
20.81 ms and no output was missed; the sd during a load was 0.13 to 0.42 ms against
0.14 to 0.29 ms before. Export and send took 8 to 11 ms on the trainer (the same
machine here), the checks 3 to 18 ms. On this machine the swap costs nothing that can
be told from the jitter that is there without it; it has not been measured on the Pi.

### Train/serve mismatch

- **Inputs.** Each `policy.depth_ff.wrench` names the `ctl.depth_hold.target` message
  it read; the pairer's row for the same anchor records the one it joined. Over the
  five runs, 208 512 anchors: the same message in all but **8** (the policy read a newer
  one 6 times, an older one twice; `accel_z` differed by at most 0.028 m/s²). Per run:
  4, 0, 3, 1, 0. Every anchor had both a row and an output, but for the first one to
  three of a session.
- **Outputs.** M2's file from the session, run by `python/hangar`'s reference on the
  pairer's rows from the `reset_state` on, against what the policy published, 3000
  anchors per run: largest difference in heave 2.1e-6 to 1.4e-5, median 3.5e-9 to
  1.5e-8, none over 1e-4 (one anchor over 1e-5 in B and in C). The tick the reset fell
  on is not marked in the stream; it was found as the one of the 4 ticks in the call's
  window that agrees (the others disagree by 0.003 to 0.136).

### Lineage

One thruster command that carried M2's residual, followed back in each run, complete in
all five: `thr.cmd` → `ctl.wrench` → `ctl.depth_hold` and `policy.depth_ff.wrench` →
`ctl.depth_hold.target`, its anchor `nav.state` → `nav.depth`, `nav.attitude`, `nav.imu`;
and `policy.depth_ff.active` (version, hash) → `weights/<hash>.hgw` in the session,
whose header agrees with the announcement (rows, dataset hash, newest row) → the
header's lineage (`live`, first and last `anchor_seq`: the 9000 rows of the 3 min
replay buffer, all of them in the recorded `dataset.depth_ff`) → the newest row → its
`nav.state` → its `nav.depth`, `nav.attitude`, `nav.imu`. Every `thr.cmd` that carried a
residual leads back to a `policy.depth_ff.active`: 3706, 2842, 5420, 15 052, 15 050.

What the lineage does not hold: the header's `steps` and `parent_hash` are 0
(`hangar.learn.deploy` passes neither), and its session is `live`, not the recorder's
session id, so the rows are found by `anchor_seq` in the session that happens to
record them.

### Demotions

Six in A, B and C, none in D and E; every one by the same rule, in the mux's words
"policy depth_ff held at the residual envelope for 51 of 250 ticks" (the envelope is
heave ±0.2, `demote_saturation` 0.2 of 5 s). None for a skipped tick, a stale input or
a non-finite output. `mux.state` and the mux's log agree, and after each the controller
alone held the depth with no transient beyond the one the residual had made.

One run each, so how often a model of each kind is demoted is not known. Q says it
is not always: there docs/09's model with B's label, trained for 72 s, stayed promoted
through all three residual blocks; it was at the envelope in 30 `mux.state` messages,
never for a second in five. The buoyancy change cost it 38.0 cm and 2.54 m s (16 s)
against 32.2 cm and 1.47 without. Its steps cost 2.23 and 3.79 m s against 1.59 and 2.62 without (nominal), and 2.97 to
3.84 against 2.46 and 1.87 (heavier); its holds were within a centimetre of
`assisted`'s but for B4 (11.5 cm rms).

### Training rows lost

None: 40 126 to 41 127 rows reached the trainer in each run, 0 lost; the pairer wrote a
row for every `nav.state` (0 anchors without one); the recorder lost no message on any
topic. The trainer ran 163 to 173 steps/s (batch 32 × 50 ticks), 130 000 to 138 000
steps a run; the throttled attempt managed 1.2 steps/s.

A first full run reported 1032 lost. That was the experiment: it opened the stream
40 s before its first read, and the topic's ring (1024 rows, 20 s) lapped the reader.
`e2e_depth_ff.py` now opens the stream when the trainer starts reading.

## What this says about docs/09's claim

docs/09 says a network learns "the heave the controller's command was short by", the
mux adds it, and "the correction follows the vehicle as it changes". This run does not
support that, for four reasons it shows, each of which would hold on the vehicle too.

1. **The label is zero.** docs/09's label is the heave applied less the controller's
   own command, on rows where the vehicle is still. Where nothing but the controller
   drives, what was applied is its command: its integrator has already put the trim
   there. Over the training rows of the five runs the label's mean was −0.0005 to
   +0.0001, and run A's models output 0.000 at every hold. There is nothing to learn
   unless somebody else supplies heave. In `residual` the label is the residual being
   applied, so a model is trained to repeat itself, whatever it outputs.
2. **What is not zero in it is the loop's latency.** `thr.out` joined at the anchor is
   the command of two to three ticks earlier (the 59.5 ms above; applied against
   commanded: rms 0.017 at the same tick, 0.006 two to three ticks apart). In run B's
   14 402 assisted rows with loss, 59 had a label beyond ±0.05, up to ±0.40, and 58 of
   them lie within 0.2 s after a target or buoyancy command, when the command has
   jumped and the vehicle has not moved yet, so `Where` keeps them. A's second model
   left the envelope at the first step command after its promotion and stayed there
   for the 51 ticks that demote; its first left it at a quiet hold, 18.8 s after its
   promotion, its output between −0.36 and +0.67 where its label is zero.
3. **The rows that carry no loss are the rows the residual drives in.** `Where` masks
   the moving vehicle, so the output there is whatever the LSTM does. In B the first
   model's output, −0.084 at the hold before, ranged from −0.36 to +0.51 once it was
   promoted and its own push had taken `accel_z` out of the range the still rows have;
   the mux demoted it after 5.0 s. Loss on every row (C) did not help: the label on
   moving rows is mostly item 2.
4. **The integrator and the residual hold the same trim.** With the `trim` label the
   network learns the integrator's value (−0.084 for −0.085). Promoted, both push; the
   depth error has to wind the integrator back, at `ki` 0.1 per m s: a change of 0.085
   in the residual takes 0.85 m s of depth error before the hold is back, and a swap
   or a return to `assisted` takes it again (H1, S2 and H2 in D and E: peaks of 11.9
   to 56.1 cm, where the 4 N buoyancy change itself makes 33 cm). After the buoyancy
   change the model from before still pushed the old way (−0.036 and −0.062 where
   +0.088 was needed) and the integrator carried the difference too; Q shows it
   plainly: the integrator at −0.002 with the policy at −0.085 before the change, at
   +0.176 with the policy at −0.087 after it. With the second
   model the trim did move from the integrator to the network (B and E, B4: +0.080
   and +0.088 from the policy), which is what "learns the correction" can mean here,
   and it bought nothing: the integrator was already holding the depth to the sensor's
   bias.

So: the loop's plumbing is proven by this run, and its latency, cost and lineage are
measured; the example it was built for is not. Run A is docs/09 as written and equals
`assisted` when the policy is not being demoted. The `trim` label is a substitute that
has something to learn, and what it learns the controller already has. A residual
would need a label for something the integrator cannot supply (a thrust curve with a
deadband, a buoyancy that changes with depth, the mixer's scaling in phase 1), built
from applied thrust aligned with the command that caused it, and a say on what the
integrator does while a residual is active. None of that is in docs/09 or in this run.

## Found on the way

Fixed, each with a test: `depth_hold` took the mux's source 4 (a policy, in `residual`)
for "not driving" and dropped its target; `hangar-policy` copied only the first model
of a session into the session's `weights/`; and, on this branch before the reboot, a
policy whose dataset part appeared after its start never bound. `release.sh` did not
ship `hangar-thrusters`.

Not fixed, for a decision:

- **`depth_hold`'s defaults do not hold the simulated vehicle through a 4 N buoyancy
  change.** `ff` assumes thrust linear in heave; the simulator's T200s give nothing
  below heave 0.07 (28 µs at 16 V) and 3 N at 0.1. With `accel_max` 1 the feedback is
  at most heave 0.1, the vehicle sinks past `i_band`, the integrator stops, and it
  never comes back. The profile runs with `accel_max = 4`, `i_band = 0.6`:

  | `depth_hold` | Buoyancy −4 N at the 3 m hold, 30 s | Step 3 to 4 m: overshoot |
  |---|---|---|
  | defaults | 4.9 m deeper and sinking | 2.7 cm |
  | `accel_max=4` | 34 cm, stays 32 cm off (just outside `i_band`) | 2.7 cm |
  | **`accel_max=4 i_band=0.6`** | **33 cm, within 5 cm after 8.2 s** | **9.2 cm** |
  | `accel_max=4 i_band=0.6 ki=0.2` | 28 cm, within 5 cm after 9.6 s | 20 cm |
  | `accel_max=4 i_band=1 ki=0.2` | 28 cm, within 5 cm after 9.6 s | 47 cm |

  One run each, not a tuning. The 33 cm is the integrator crossing the deadband, and
  it is the same with every residual tried.
- `nav`, `depth_hold` and the mux each tick on their own timer: three of the control
  loop's four hops are waits of up to a period each, 55 of its 59.5 ms (above).
- `policy.<name>.reset_state` leaves no mark in the output stream (the mismatch check
  had to search for its tick).
- A `hangar.learn` stream counts rows lapped before its first read as lost.
- docs/09's spec names `Fields=depth`; `nav_state`'s field is `depth_m`.
- `E2E_ROOT` must be short: the control socket's path has to fit `sun_path`, and
  `hangard` says "File name too long" only in its log. The script now refuses first.

## What is placeholder

- **The label** in B to E (`--label trim`) is not docs/09's, and `depth_ff.py`'s `FF`
  repeats `depth_hold`'s `ff` by hand. Its "heave applied" is the pulse width scaled
  linearly (400 µs to full): the commanded heave as it left the mux, not thrust. The
  simulator's thrust curve is not used by the training code.
- **`thr.out`** comes from the `thrusters` module (what it commanded), not from
  ArduSub's `SERVO_OUTPUT_RAW`: there is no ArduSub here, so the mixer's scaling,
  docs/09's first reason for a shortfall, does not exist in this run.
- **The simulator**: buoyancy constant with depth, no tether, no current, pitch and
  roll at zero but for sensor noise. Two of the five inputs carry nothing.
- **The deploy** is `hangar.learn.deploy`'s copy into the policy's `incoming/` (until
  the bridge); the trainer, the operator and the vehicle share one machine and one
  clock, and the trainer is the experiment's own process, not a module.
- **The operator** is the script, as `admin`; "a deploy every 3 minutes" is two
  deploys at fixed times.
- **The bounded model** (D, E) is not docs/09's architecture; it is there to see a
  residual stay promoted.
- **`depth_hold.params` and `safety.params`** of the profile (its README).
- **Timing** is a laptop's, with other work on it. Nothing here ran on the Pi or on
  the vehicle, and every number is one run.

## Files

| | |
|---|---|
| `tools/sim/e2e-depth-ff.sh` | the root, the manager, the run; `up`, `run`, `down`, `status` |
| `tools/sim/e2e_depth_ff.py` | the operator, the trainer, the analysis (`--analyse`) and the tables |
| `profiles/sim-depth-ff/` | the manifests, the dataset and policy specs, the parameters |
| `python/examples/depth_ff.py` | docs/09's trainer: the model, both labels, `--bound` |
| `2026-11-20/<run>/` | `report.md` (the tables), `report.json` (everything), `events.log` (the schedule as it ran); `Q-trim-quick` is the smoke run |

# Second design

The owner's decision after the first: the model learns a feed-forward for what the
controller cannot see, the thrusters' deadband and curve and the drag, from the motion
that is commanded; the integrator keeps the trim. What follows was written before any
model of this design drove; the results are under [The runs](#the-runs-of-the-second-design).

```sh
make -j4 all
E2E_OUT=/tmp/c1 E2E_SEED=11 E2E_KEEP_SESSION=1 E2E_ARGS="--variant collect" tools/sim/e2e-depth-ff2.sh run   # 7 min
PYTHONPATH=python ~/.cache/hangar-e2e-venv/bin/python tools/sim/depth_ff2_offline.py /tmp/c1/session         # the offline check
E2E_OUT=/tmp/l1 E2E_SEED=1 tools/sim/e2e-depth-ff2.sh run                                    # learned, 13 min
E2E_OUT=/tmp/o1 E2E_SEED=1 E2E_ARGS="--variant oracle" tools/sim/e2e-depth-ff2.sh run        # the ceiling
PYTHONPATH=python ~/.cache/hangar-e2e-venv/bin/python tools/sim/e2e_depth_ff2.py --summarise /tmp/l? /tmp/o?
```

## The problem

`depth_hold` commands `heave = I − ff · accel_z`: its integrator `I`, and a feed-forward
that takes thrust for linear in the command (160 N at heave 1) and the vehicle for 16 kg
in still water. The simulated vehicle has a deadband of ±0.07 in heave, 1.7 N at 0.085,
12.6 N at 0.2 and 40 N at 0.4 where the line says 13.6, 32 and 64; it weighs 26 kg with
its added mass, and its drag is 5.2 N s/m and 37 N s²/m². So the vehicle does not make
the acceleration the controller asks for, least of all around the hold, where a vehicle
trimmed at −0.085 (it floats by 2 N) gets no thrust at all for the first 0.155 of
command upward.

1. **What runs on the vehicle** (`policy_depth_ff`, 50 Hz, no state):
   `residual = f(accel_z, vel_z, I)`, three numbers the policy has at its tick from
   `ctl.depth_hold.target` and `nav.state`: the acceleration the controller wants, the
   vertical velocity, the integrator. No force sensor, no thrust table.
2. **What is learned** (the trainer, `python/examples/depth_ff2.py`): a model of the
   heave axis, `accel made = φ(heave applied) − φ(heave at rest) − drag(vel_z)`, two
   small networks, `drag(0) = 0`. The acceleration made is `nav.state`'s `accel[2]`; the
   heave applied is `thr.out` two ticks before the anchor, scaled linearly (the command
   as it left the mux, the controller's and any residual's together); the heave at rest
   is the trainer's running mean of the heave applied over the rows in which the vehicle
   was still (|accel| < 0.05 m/s², |vel_z| < 0.02 m/s). No join looks ahead.
3. **The policy is that model inverted**: the heave nearest the trim that makes the
   acceleration wanted at this velocity, less the controller's own command, within
   ±0.18 by a `tanh` (the mux's residual envelope is ±0.2). The trainer fits the policy
   network to the inversion as it fits the plant, and exports the policy. On the
   vehicle the integrator stands for the heave at rest.
4. **Why it does not compete with the integrator.** The plant has no constant: at the
   heave at rest with the vehicle still it is `φ(h) − φ(h) − 0`, zero whatever was
   learned, so nothing in it can hold a trim, and its inversion at `accel_z` 0, `vel_z`
   0 is the trim itself: residual 0. A buoyancy change moves the point at which `φ` is
   read, through `I`, not the function. A loss term pins the policy network to zero
   there too. The first design's label had the trim in it; this one cannot.
5. **Why not the inverse directly** (the command from the acceleration it made, which is
   feedback-error learning's shape too): in the deadband every command makes the same
   acceleration, and the mean of them is no command to give. The offline check below
   has both.

What the simulator gives for free and a vehicle would not: an accelerometer with 0.01
m/s² of noise and no vibration (the acceleration is the whole label); thrust that
depends on the command alone (no inflow, no battery sag within a dive that the model
could not follow, four identical thrusters); buoyancy constant with depth; no tether
and no current, so "still" means no force but the trim; `thr.out` equal to what was
commanded. The additive form `φ − φ − drag` is exact here and an assumption there.

## The offline check, before any model drove

`tools/sim/depth_ff2_offline.py` on two recorded sessions (`--variant collect`: assisted
only, the training phase's steps, the 3 to 4 m block, buoyancy −4 N, steps again; seeds
11 and 12; 43 001 rows with loss, 22 802 of them still). The result is in
[2026-11-20-second/offline/report.json](2026-11-20-second/offline/report.json).

**Does the label carry signal.** The acceleration made has sd 0.146 m/s² (1st and 99th
percentile −0.30 and +0.39); on still rows its mean is 0.015 (the accelerometer's bias)
and its sd 0.014. The heave applied less the heave at rest has sd 0.064, is beyond ±0.02
on 23.5 % of the rows, and on still rows has mean 0.0004: the trim is gone from it.
`nav.state`'s acceleration against the simulator's truth: +0.016 ± 0.011 m/s².

| Variance of the acceleration made explained (R², rows the fit did not see) | |
|---|---|
| a straight line in the three inputs | 0.60 |
| the simulator's own thrust table, mass and drag on the same inputs | 0.96 |
| the plant network, fitted on the first 70 % of each session, on the rest | 0.89 |
| the plant network, fitted on the rows before the buoyancy change, on those after | 0.82 (the simulator's own: 0.84) |

By the lag of the command behind the acceleration, the simulator's own model explains
0.88, 0.95, **0.96**, 0.93, 0.86, 0.78, 0.68, 0.58 at 0 to 7 ticks: the lag is 2.

The direct inverse's label (heave applied less at rest, plus `ff` × acceleration made)
has sd 0.058, and the simulator's own inverse explains 0.49 of it: the rest is the
deadband's ambiguity and the thrusters' lag.

**What an oracle gains in closed loop.** On a heave axis in a few lines (the
simulator's table, mass, drag and 50 ms thruster lag; `depth_hold`'s law with the
profile's parameters; the mux's envelope and rate limit; three ticks of loop delay;
three noise seeds, which differ in the last digit). It reproduces the recorded
baseline: 1.91 m s and 10 % where the stack measured 1.85 and 8.7 %, 3.06 and 20 %
for 3.01 and 19.7 %, 33 cm for 33 cm.

| | Controller alone | Oracle, bounded ±0.18 | Policy learned from the recorded rows | Inverse fitted directly |
|---|---|---|---|---|
| hold rms (cm) | 2.0 | 2.0 | 2.1 | 2.7 |
| nominal, 3 to 4 m: IAE (m s), overshoot | 1.90, 10 % | 1.25, 1 % | 1.25, 0.3 % | 1.52, 5 % |
| nominal, 4 to 3 m | 3.06, 20 % | 2.01, 4 % | 1.85, 3 % | 1.94, 3 % |
| buoyancy −4 N: IAE (m s), peak | 2.0, 33 cm | 0.10, 2.6 cm | 0.27, 2.8 cm | 0.86, 6.8 cm |
| heavier, 3 to 4 m | 2.77, 16 % | 1.15, 0.3 % | 1.27, 0 % | 1.45, 11 % |
| heavier, 4 to 3 m | 2.24, 15 % | 1.30, 1.6 % | 1.45, 1.5 % | 1.61, 5 % |

The oracle is `depth_ff2.oracle`: the same inversion with the simulator's own numbers.
Unbounded it asks for more than the envelope on 170 ticks of the 160 s, 40 a step:
under the mux's rule (51 of 250 ticks) that is close to a demotion at every 1 m step,
which is why the policy is bounded inside the envelope and its saturation is reported
instead ("at its bound").

So the task has something to learn, an oracle beats the controller on every metric but
the hold, and a model fitted to recorded rows gets most of it in this few-line loop.
That was the condition for running it live.

## Metrics and comparison, fixed before the runs

- **Steps**: IAE over 25 s and overshoot, 3 to 4 m and 4 to 3 m, truth (`sim.pose`)
  against the target.
- **Hold**: rms error over the last 8 s of a 20 s hold at 3 m.
- **Buoyancy change**: −4 N live at the hold: peak error, IAE over 40 s, when the error
  was last outside 5 cm.
- **Transients** at the hold: the promotion (H1), the swap to the second model (S2),
  the return to `assisted` (H2): peak and IAE over 20 s. An undisturbed hold reads 2.2 cm
  and 0.40 m s (the depth sensor's 2 cm bias).
- **Demotions**, and the share of ticks the policy was at its own bound.
- The first experiment's checks, reported again: lineage, train/serve mismatch, loop
  latency, rows lost, tick intervals.

The baseline is `assisted` with the same controller parameters in the same run: B1
(nominal) and B5 (heavier) for the steps and holds, T1 for the buoyancy change. Against
it: B2 (M1, nominal), B3 (M1, 4 N heavier: the model from before the change), B4 (M2,
heavier: the model trained since), T2 (the buoyancy change with M1 promoted). Two
variants, five runs each with simulator and trainer seeds 1 to 5: **learned**, and
**oracle** (the policy network fitted to `depth_ff2.oracle` and deployed through the
same load, promotion and envelope, as M1 and again as M2). Per metric the median and
the smallest and largest of the five; a difference between variants or against the
baseline counts only when the two ranges do not overlap, and is called inside the
spread otherwise.

The schedule is the first experiment's with a shorter training phase: settle 30 s;
training 120 s (steps of 0.1 to 1.5 m, 10 s each) and 20 s at 3 m; deploy M1; B1; T1
and back; H1; B2; T2; B3; S2 with M2; B4; H2; B5; T3. 13 min.
