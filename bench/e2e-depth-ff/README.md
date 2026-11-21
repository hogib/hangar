# e2e-depth-ff: the learning loop end to end, in simulation

Two designs of docs/09's adaptive depth hold were run here. **The first** (this half of
the page) proved the loop's machinery and gave a negative result for the example: its
label was zero or the trim, and the residual improved nothing. **The second**
([Second design](#second-design), below) learns a model of the heave axis around the
trim and inverts it: on 1 m steps the integral of absolute error falls by a third to a
half and a 4 N buoyancy change moves the vehicle 2.7 cm instead of 33, in each of
five runs, as much as an oracle gains. Whether a later model follows a vehicle that
changes was not shown by those runs; [Adaptation](#adaptation-a-change-the-policy-cannot-read)
tests it with a change none of the policy's inputs shows, and in five runs on a
quiet machine the model trained since is better than the one from before, by 6 %, at the
price of more thruster reversals and of the vehicle it was first trained on. docs/09
describes the second design; what the first half quotes from docs/09 is its text at
the time.

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

docs/09 said, when this was run, that a network learns "the heave the controller's command was short by", the
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
variants, five runs each with simulator and trainer seeds 1 to 5 (a seed fixes the
simulator's noise and the trainer's draws, not the run: below): **learned**, and
**oracle** (the policy network fitted to `depth_ff2.oracle` and deployed through the
same load, promotion and envelope, as M1 and again as M2). Per metric the median and
the smallest and largest of the five; a difference between variants or against the
baseline counts only when the two ranges do not overlap, and is called inside the
spread otherwise.

The schedule is the first experiment's with a shorter training phase: settle 30 s;
training 120 s (steps of 0.1 to 1.5 m, 10 s each) and 20 s at 3 m; deploy M1; B1; T1
and back; H1; B2; T2; B3; S2 with M2; B4; H2; B5; T3. 13 min.

**A supplementary run, added after the first learned and oracle runs and before it was
made.** A buoyancy change turned out to ask nothing new of a model: the trim is one of
its inputs, and the oracle, which is the same function before and after, does as well
on the heavier vehicle as on the nominal one. Whether a second model *learns* a changed
vehicle needs a change the policy cannot read. `--change supply` runs the same schedule
with the thrusters' supply going from 16.8 to 12.6 V wherever the buoyancy went down
4 N (and back where it came back): the thrust curve itself is then another. The same
metrics; B3 is the model from before the change on the weaker thrusters, B4 the model
trained since (its 3 min buffer holds 2 min of rows from after the change and 1 min
from before), B5 `assisted`; the oracle variant keeps reading the table at 16.8 V and
so is stale by construction. Five learned runs, two oracle runs.

## The runs of the second design

2026-11-20, the same machine and build as the first (Ryzen 5 5600H, `performance`,
3.8 to 4.1 GHz at each start: not throttled; PyTorch 2.15.0 on one thread), every
process of a run pinned to the same four cores. **The machine was not idle**: other
agents were building and testing on it (load average 7 to 12 of 12 threads throughout),
and the runs were made four at a time (learned 1 to 4; learned 5 with oracle 1 to 3),
then in smaller batches. What that did to the timing is under
[Timing](#timing-of-the-second-designs-runs); two runs were repeated for it.
Reports: [2026-11-20-second/](2026-11-20-second/), one directory a run, and the tables
below are `e2e_depth_ff2.py --summarise` over them.

### In short

The learned feed-forward helps, on every step and on the buoyancy change, by far more
than the runs' spread, and it is as good as the oracle: no difference between the two
is outside the spread. It does nothing for the hold, which the controller already
holds to the depth sensor's bias. It was never demoted and never at the mux's envelope
in ten runs, and promoting it, swapping it and taking it away moved the vehicle by
2 to 4 cm. **That a later model follows a vehicle that changes is not shown by these
runs.** A second
model was not needed after the buoyancy change, since the trim is an input; after a
change of the thrusters' supply, which is not, the model trained since did no better
than the one from before, and by a few-line model of the loop could not have: the
policy's bound, not what it knows, is the limit there. A later experiment with a
change that leaves a model something to gain is under
[Adaptation](#adaptation-a-change-the-policy-cannot-read).

### Depth error

Median of five runs (smallest to largest). "Assisted" is B1, B5 and T1 of the learned
runs; the oracle runs' own baselines are the same to the last digit or the one before.

| | Assisted, the same runs | Learned | Oracle |
|---|---|---|---|
| **Nominal buoyancy** (B1; B2 with M1) | | | |
| hold at 3 m, rms (cm) | 2.00 (1.98 to 2.05) | 1.98 (1.96 to 2.03) | 2.03 (2.02 to 2.06) |
| step 3 to 4 m: IAE (m s) | 1.85 (1.81 to 1.88) | **1.24** (1.22 to 1.25) | 1.23 (1.22 to 1.23) |
| overshoot (%) | 9.2 (8.8 to 9.5) | **1.0** (0.8 to 1.4) | 0.9 (0.8 to 1.2) |
| step 4 to 3 m: IAE (m s) | 3.02 (3.00 to 3.04) | **1.86** (1.82 to 1.90) | 1.90 (1.89 to 1.92) |
| overshoot (%) | 19.8 (19.5 to 19.9) | **3.5** (3.3 to 3.6) | 3.6 (3.5 to 3.7) |
| **4 N heavier, the model from before the change** (B5; B3 with M1) | | | |
| hold, rms (cm) | 1.98 (1.96 to 2.08) | 0.86 (0.54 to 1.02), not a gain: below | 1.04 (1.01 to 1.13), likewise |
| step 3 to 4 m: IAE (m s) | 2.73 (2.70 to 2.76) | **1.34** (1.32 to 1.35) | 1.34 (1.32 to 1.35) |
| overshoot (%) | 16.2 (16.0 to 16.3) | **0.0** | 0.0 |
| step 4 to 3 m: IAE (m s) | 2.19 (2.18 to 2.20) | **1.40** (1.34 to 1.43) | 1.34 (1.32 to 1.36) |
| overshoot (%) | 14.2 (13.8 to 14.3) | **1.7** (1.5 to 1.8) | 1.3 (1.3 to 1.6) |
| **4 N heavier, the model trained since** (B5; B4 with M2) | | | |
| hold, rms (cm) | 1.98 (1.96 to 2.08) | 1.93 (1.81 to 1.97) | 1.91 (1.89 to 1.97) |
| step 3 to 4 m: IAE (m s) | 2.73 (2.70 to 2.76) | **1.49** (1.43 to 1.55) | 1.48 (1.45 to 1.51) |
| overshoot (%) | 16.2 (16.0 to 16.3) | **0.0** | 0.0 |
| step 4 to 3 m: IAE (m s) | 2.19 (2.18 to 2.20) | **1.61** (1.55 to 1.63) | 1.60 (1.58 to 1.61) |
| overshoot (%) | 14.2 (13.8 to 14.3) | **3.2** (2.8 to 3.7) | 3.3 (3.2 to 3.6) |

Against the spread: on every step the residual's range and the assisted range do not
overlap (bold), and the gap between them is 7 to 22 times the wider of the two for the
IAE; it holds in each of the five runs
(the run's own difference, residual less assisted: −0.60 m s (−0.63 to −0.59) and
−1.16 (−1.22 to −1.11) nominal; −1.38 and −0.79 with M1 heavier; −1.26 and −0.57
with M2). The IAE falls by 33 % and 38 % nominal and by 51 % and 36 % heavier (M1).
The hold with the residual is inside the spread (nominal and B4). Learned against
oracle: the ranges overlap on every row of this table and the next; the medians differ
by 0.06 m s at most.

B3's hold reads 0.9 cm because the vehicle sat about 1 cm from where the biased sensor
would have it, not because it was held better: see [the trim](#who-carried-the-trim).

### Transients at the 3 m hold

Peak error (cm), IAE (m s) over the window, when the error was last outside 5 cm. An
undisturbed 20 s hold reads 2.2 cm and 0.40 m s.

| | Assisted | Learned | Oracle |
|---|---|---|---|
| **Buoyancy −4 N**, 40 s (T1; T2 with M1) | 32.7 (32.6 to 32.9), 1.97, 8.2 s | **2.7** (2.5 to 2.9), **0.23** (0.20 to 0.25), never outside | 2.8 (2.7 to 2.9), 0.26 (0.24 to 0.28), never outside |
| Promotion (H1), 20 s | | 2.4 (2.3 to 2.5), 0.41 | 2.5 (2.4 to 2.6), 0.42 |
| Swap M1 to M2 while promoted (S2), 20 s | | 2.1 (1.8 to 2.4), 0.31 | 2.0 (1.9 to 2.1), 0.32 |
| Back to `assisted` (H2), 20 s | | 3.0 (2.4 to 4.0), 0.45 | 3.4 (3.3 to 3.5), 0.46 |
| Buoyancy +4 N, assisted (T1:back, T3), 40 s | 36.4 (36.3 to 36.7), 2.53, 10.2 s | | |

The 33 cm of the first experiment, "the integrator crossing the deadband, the same
with every residual tried", is 2.7 cm here: the policy takes the command across the
deadband at once, for whatever the feedback asks. The first design's promotions, swaps
and returns cost 12 to 56 cm; these cost nothing that can be told from an undisturbed
hold but the return (3.0 cm against 2.2).

### Who carried the trim

At the end of each block's hold (heave; median and range of the learned runs):

| | B1 assisted | B2, M1 | B3, M1, heavier | B4, M2, heavier | B5 assisted, heavier |
|---|---|---|---|---|---|
| the integrator | −0.085 | −0.086 (−0.089 to −0.085) | **+0.013** (0.010 to 0.016) | +0.086 (0.080 to 0.090) | +0.089 |
| the policy's output | (not applied) | +0.002 (0.000 to 0.003) | **+0.064** (0.060 to 0.069) | 0.000 (−0.004 to 0.004) | (not applied) |

Settled, the integrator has the trim and the policy adds nothing (B2, B4): what the
first design could not do. After the buoyancy change it is otherwise for a while. The
policy holds the vehicle so close (2.7 cm at worst) that the integrator, which learns
from depth error at 0.1 per m s, moves slowly: 55 s after the change (the end of B3's
hold) it has come from −0.085 to +0.013, inside the deadband, and the 0.064 that is
missing comes from the policy answering a feedback that still asks for a little: the
vehicle sits 1.1 cm deep of where the sensor's bias would put it. By the end of B4's
hold, 155 s after the change, the integrator has it all. The oracle does the same (+0.009 and +0.063 in B3),
so this is the scheme, not the learning: a residual that is zero when nothing is asked
still lets the feedback ask for less, and the integrator learns from what the
feedback is left with. Nothing unwinds through depth error on the way.

### What it costs the thrusters

| | Assisted | Learned | Oracle |
|---|---|---|---|
| heave command crossing zero, per second: B2 | 0.03 | 0.20 (0.09 to 0.29) | 0.09 (0.06 to 0.17) |
| B3 (the integrator inside the deadband) | 0.06 | 1.63 (1.23 to 2.46) | 1.80 (1.47 to 2.37) |
| B4 | 0.06 | 0.31 (0.29 to 0.97) | 0.17 (0.14 to 0.20) |
| policy at its own bound, ±0.18 (% of ticks): B2, B3, B4 | | 2.9, 2.6, 2.9 | 2.1, 2.4, 2.7 |

With the residual the verticals reverse three to seven times as often as without, and
30 to 50 times as often while the integrator is inside the deadband. The simulator's
thrusters do not mind; an ESC and a propeller would have to be asked. The learned
policy reverses them more than the oracle in B4 and is at its bound more often in B2
(both outside the spread); neither shows in the depth. It was at its bound on 3 % of
the ticks (a second of each 1 m step) and the mux saw no tick at its envelope.

### Demotions

None in ten runs, and no `mux.state` message at the residual envelope. The policy's
bound is why: the offline check has the unbounded oracle beyond the envelope for 40
ticks of each 1 m step, where 51 in 5 s demote.

### Does a second model adapt

**After the buoyancy change: there was nothing to adapt to.** M1, trained before the
change, is as good on the heavier vehicle as M2, trained since (B3 against B4: 1.34
and 1.40 m s against 1.49 and 1.61). M2 is not worse because it learned worse: the
oracle, one function in both blocks, shows the same difference (1.34 and 1.34 against 1.48 and 1.60), which is
the integrator's place (inside the deadband in B3, at the trim in B4). The trim is an
input of the policy, and the offline check had said so: the plant fitted on rows from
before the change explained 0.82 of the rows after it.

**After a change of the thrusters' supply** (the supplementary runs, `--change
supply`: 16.8 to 12.6 V wherever the buoyancy went down; five learned runs, and two
with the oracle left at 16.8 V; [2026-11-20-second/supply/](2026-11-20-second/supply/),
whose tables still say "buoyancy" and "heavier"). On the weaker thrusters:

| | Assisted (B5) | M1, from before (B3) | M2, trained since (B4) | Oracle left at 16.8 V (B3; B4), 2 runs |
|---|---|---|---|---|
| step 3 to 4 m: IAE (m s) | 1.98 (1.98 to 2.01) | 1.44 (1.43 to 1.48) | 1.41 (1.37 to 1.46) | 1.46; 1.44 |
| overshoot (%) | 10.5 (10.3 to 10.6) | 5.9 (5.1 to 6.1) | 5.8 (5.3 to 6.2) | 6.2; 6.4 |
| step 4 to 3 m: IAE (m s) | 3.47 (3.46 to 3.49) | 2.20 (2.15 to 2.23) | 2.22 (2.17 to 2.26) | 2.21; 2.20 |
| overshoot (%) | 22.5 (22.4 to 22.7) | 4.8 (4.3 to 5.3) | 5.2 (4.5 to 5.8) | 5.3; 5.5 |
| hold, rms (cm) | 1.99 (1.97 to 2.06) | 2.03 (2.01 to 2.13) | 2.04 (1.98 to 2.06) | 1.98; 2.06 |
| policy at its bound (% of ticks) | | 5.8 (5.4 to 6.5) | 6.6 (6.0 to 7.2) | 4.7; 4.9 |

The residual still helps on the weaker vehicle (IAE down 27 % and 37 %, far outside
the spread), less than on the nominal one (overshoot 6 % and 5 % where it was 1 % and
3.5 %). **The second model adds nothing to the first**: inside the spread on every
row, and the same as an oracle that was never told of the change. The few-line loop of
the offline check says there was little to add: at 12.6 V the oracle left at 16.8 V
gives 1.44 and 2.26 m s and the oracle for 12.6 V 1.41 and 2.22, which is the spread
here; the correction is at its bound twice as often as at 16.8 V, and it is the bound
that limits the weaker vehicle. Whether the second model had learned the weaker
thrusters at all was looked at in one run (seed 1): at the new trim its output is
0.025 rms from the oracle for 12.6 V and 0.028 from the one for 16.8 V, where the first
model's is 0.029 and 0.020; the two oracles are 0.040 apart. It moved towards the new
curve, part of the way: its buffer still held 50 s of rows from before.

The supply change itself, at the hold (the trim goes from −0.085 to −0.104): 6.4 cm
(6.4 to 6.6) and 0.98 m s `assisted`; 4.2 cm (3.8 to 4.2) and **1.13 m s** (1.12 to
1.17) with M1 promoted. The peak is smaller and the IAE larger, in all five runs: as
after the buoyancy change, the integrator takes longer to find the new trim when the
policy leaves it little error to learn from.

So docs/09's "the correction follows the vehicle as it changes" has two answers here.
The deploy, the load and the swap of a second model work during the dive and cost
nothing in depth (S2). That the second model is *better* than the first on the changed
vehicle was not seen with either change, and with these two could not be: one is
carried by an input, and the other is beyond the residual's bound. A change that
would show it (a thrust curve or a drag that moves by more than the first model's
error and within the envelope) was not run then; it was run afterwards, with a
payload's drag: [Adaptation](#adaptation-a-change-the-policy-cannot-read).

### The first experiment's checks, again

- **Lineage**: one thruster command carrying M2's residual followed back in each run,
  complete in all ten, by the same chain as before (`thr.cmd` → `ctl.wrench` →
  `ctl.depth_hold` and `policy.depth_ff.wrench` → `ctl.depth_hold.target`, the anchor
  `nav.state` → `nav.depth`, `nav.attitude`, `nav.imu`; `policy.depth_ff.active` →
  `weights/<hash>.hgw`, whose header agrees with the announcement → the rows → the
  newest row's messages). Every `thr.cmd` that carried a residual leads back to a
  `policy.depth_ff.active`: 15 013 to 15 059 a run. Not in the lineage: the plant
  network. The file names the rows; the policy was fitted to a plant fitted to those
  rows, and the plant is not kept. For the oracle the header's rows are the stream's,
  which trained nothing. `steps` and `parent_hash` are still 0.
- **Train/serve mismatch, inputs**: 375 721 anchors with an output and a row in the ten
  runs; the policy read another `ctl.depth_hold.target` message than the pairer joined
  at **49** (17, 2, 10, 19, 0 learned; 1, 0, 0, 0, 0 oracle), `accel_z` differing by
  at most 0.027 m/s². The first experiment had 8 in 208 512; the runs with the most are
  the four made at once, each with a trainer on the same four cores.
- **Train/serve mismatch, outputs**: M2's file run by `python/hangar`'s reference on
  the pairer's rows against what the policy published, 3000 anchors a run: largest
  difference 5.3e-8 to 1.8e-7 in eight runs; **4.9e-3 and 1.1e-2** in learned 3 and 4,
  on one and two anchors. This policy is steep where the first was flat: across the
  deadband it moves 0.15 for 0.1 m/s² of `accel_z`, so an anchor read with the
  neighbouring message shows. The analysis now
  compares apart the anchors read with the same message and with another
  (`mismatch`, "outputs, by the message read"): in learned 1, 6000 anchors with the
  same message agree to 1.3e-7 and the 8 of M2's with another differ by up to 6.8e-4;
  in the three runs made after it, 6000 anchors each agree to 1.9e-7. Learned 3 and 4
  kept no session, so that their three anchors were of that kind is likely and not
  checked.
- **Loop latency**, newest row trained on to the first `thr.cmd` carrying the model
  (M2): **1.12 s** (0.96 to 1.14) learned, of which 0.72 to 0.94 s the policy's last 300
  steps on the plant and the export, 0.18 s the warm-up (10 ticks: the model has no
  state, `export` warms it up all the same) and 0.04 s from the swap to the thrusters;
  0.27 s for the oracle, which has nothing to bring up. Checks 1 to 7: 1 to 19 ms.
- **Swap cost**: over the 20 loads no interval of the five 50 Hz topics during a load
  exceeded 26.4 ms, against up to 29.4 ms in the 10 s before one: a load cannot be
  told from the machine's other work, which the first experiment's runs did not have
  (20.8 ms at most there).
- **Rows lost**: none on the way to the trainer in nine runs; **one** in learned 3,
  counted at the stream's opening (the ring's oldest row lapped while it was being
  opened: the item "a stream counts rows lapped before its first read as lost" above).
  The pairer wrote a row for every anchor in all ten. The trainer ran 57 steps/s
  (its pace is set to 60), 41 372 to 42 186 a run.
- **The control loop**, `nav.depth` to `thr.cmd`: median 34 to 60 ms from run to run
  (the modules' timers fall differently at each start), p99 39 to 72.

### Timing of the second design's runs

The first experiment's runs had no interval of a 50 Hz topic over 20.8 ms. These had,
over the five topics of a run (188 000 intervals):

| Runs, made at the same time | Largest interval (ms) | Over 25 ms | Over 30 ms |
|---|---|---|---|
| learned 1 to 4 (a trainer each) | 28.2 to 29.6 | 271 to 314 | 0 |
| learned 5, oracle 1 to 3 | 24.6 to 25.3 | 0 to 2 | 0 |
| oracle 4 and 5, the first time | 41.7, 57.2 | 27, 35 | **5, 4**; the recorder lost 3 messages in run 4 |
| their repeats, and learned 1 again | 27.8, 36.3; 35.2 | 10, 9; 11 | 0, **2**; **2** |
| supply: learned 1 to 4 | 29.7 to 34.4 | 367 to 417 | 2, 2, 0, 0 |
| supply: learned 5, oracle 1 and 2 | 32.0; 27.6, 28.9 | 16; 15, 11 | 1; 0, 0 |

Oracle 4 and 5 were repeated (the tables above hold the repeats; the first attempts are
in `oracle-s4-disturbed` and `oracle-s5-disturbed`), and the repeat of 5 was disturbed
again, as was the repeat of learned 1 that was meant as the check (`learned-s1-repeat`;
the tables hold the first). The machine was never quiet, and a tick or two late by 10 to
37 ms in 13 minutes is what it gave; where in a run they fell was not looked at. The
last three runs ended after midnight, on 2026-11-21.

The depth numbers do not follow it. The two disturbed runs and learned 1's repeat fall
outside the five runs' smallest-to-largest on several rows, by 0.05 m s of IAE at most
(learned 1 again: B3 3 to 4 m 1.29 where the five gave 1.32 to 1.35) and 0.4 points of
overshoot, against effects of 0.55 to 1.4 m s. So the same seeds differ from one run to
the next by as much as different seeds do: the spread in the tables is where the
modules' timers happened to fall (the loop's latency above) as much as the simulator's
noise, and "smallest to largest of five" is a little narrower than what a sixth run can
give. The supplementary runs were not repeated.

**What a seed fixes.** The simulator's noise sequence and the trainer's draws; not
the run. `sim_vehicle` steps on a 200 Hz timer and `nav`, `depth_hold`, the `mux` and
`thrusters` each on a 50 Hz loop that starts when its process does, so the delay from a
depth reading to the thrust it causes is set by the processes' start times: constant
within a run, different between runs, unrelated to the seed (docs/14 "Determinism and
replay"). Each report's `control_latency` has it: the median from `nav.depth` to
`thr.cmd` was 33.8 to 60.5 ms over the thirteen runs of this section (learned 1: 39.0,
its repeat 36.9; oracle 4: 37.9, disturbed 33.8), and 59.2 to 59.6 ms in all ten runs
of the quiet repeat below (presumably because a manager on a quiet machine starts the
processes at the same intervals every time; that was not tested). So the quiet runs
share one loop delay, and the loaded runs do not: part of the loaded runs' spread, and of the difference between the two sets, is
the delay. Within a run the blocks share it, which is why every comparison here is
against `assisted` in the same run. `e2e_depth_ff2.py --summarise` prints each run's
delay from now on. The plant model's `LAG = 2` ticks is one value for all of them.

**Again on a quiet machine, 2026-11-21** (`2026-11-21-quiet/`, main at `1aa7d17`). Much of
the load above was not the other agents' but six busy loops left running by an earlier
test. With those stopped, the ten runs (learned and oracle, seeds 1 to 5) were made
again, two at a time, each on three physical cores of its own (`taskset -c 0-5` and
`6-11`; load average 0.2 to 3.4 at each pair's start). No run had an interval over 30 ms
on any of the five topics, the recorder lost nothing, no row was lost, and the policy
read another `ctl.depth_hold.target` than the pairer at 1 anchor of 375 455 (49 of
375 721 before). The depth numbers are the loaded runs' to within their spread:

| Median of 5 (smallest to largest) | Assisted | Learned | Oracle |
|---|---|---|---|
| Nominal, 3 to 4 m: IAE (m s) | 1.86 (1.86 to 1.87) | 1.22 (1.21 to 1.26) | 1.22 (1.21 to 1.23) |
| Nominal, 4 to 3 m: IAE (m s) | 3.03 (2.99 to 3.04) | 1.86 (1.81 to 1.90) | 1.89 (1.87 to 1.91) |
| 4 N heavier, M1, 3 to 4 m | 2.72 (2.69 to 2.73) | 1.31 (1.25 to 1.35) | 1.34 (1.32 to 1.36) |
| 4 N heavier, M1, 4 to 3 m | 2.19 (2.17 to 2.20) | 1.42 (1.38 to 1.44) | 1.34 (1.33 to 1.35) |
| Buoyancy −4 N: peak (cm) | 32.9 (32.7 to 33.0) | 2.7 (2.7 to 2.8) | 3.0 (2.8 to 3.3) |
| Nominal hold, rms (cm) | 2.01 (1.98 to 2.05) | 2.00 (1.98 to 2.03) | 2.03 (1.99 to 2.06) |

`2026-11-21-quiet/summary.md` is the whole table. Nothing in the verdict below changes;
these are the runs to quote. The supplementary supply runs were not made again.

### What this says about docs/09's claim, second design

docs/09 now describes this design, and what it says of it was measured: a network that
learns during the dive, from rows the pairer joins, a bounded correction that makes the
vehicle do what the controller asks, deployed, promoted and swapped without a
transient, never demoted, as good as the simulator's own model inverted, and leaving
the trim to the integrator. That holds in simulation, over five seeds, by a margin
many times the spread.

It says less than the first version claimed, on purpose. The network does not learn
"what the command was short by" from the thrusters' outputs; it learns the heave axis
from the acceleration and inverts it, and without an acceleration worth the name there
is no label. And "the correction follows the vehicle as it changes" is shown for the
loop's mechanics and for a change the policy reads from an input. For a change it has
to learn there is one case, run twice, the second time on a quiet machine: a
payload's drag, after which the model trained since was better than the one from
before in each of five runs, by 0.27 m s in 4.5 (the oracles: 0.23), all of it on the
step down, with two to four times the thruster reversals, and worse than the first
model once the payload is off again. The changes tried before it (buoyancy, supply)
and most of those tried on the few-line loop (mass, supply) leave a later model
nothing to gain: the feedback and the bound see to that. So "follows the vehicle" is
true here in a narrow sense, and "one model for a vehicle that changes back and
forth" is not: nothing the policy reads says which vehicle it is on.

## Adaptation: a change the policy cannot read

What the runs above left open: a second model, trained on rows recorded while the
first drove a vehicle that had changed, doing better on it than the first. This
section's design and criterion were written and committed on 2026-11-21 before any
run of it was made; the results follow them.

```sh
make -j4 all
PYTHONPATH=python ~/.cache/hangar-e2e-venv/bin/python tools/sim/depth_ff2_offline.py --adaptation --seeds 5
tools/sim/e2e-adapt.sh        # ten runs, two at a time, 1 h 30; writes bench/e2e-depth-ff/<today>-adapt/
```

### The change

A payload that weighs nothing in water: **10 kg more to move and five times the heave
drag** (25.9 N s/m and 185 N s²/m² where the bare vehicle has 5.18 and 36.99; a flat
tray of about 0.5 by 0.6 m slung under the vehicle would do it). In `sim_vehicle`'s
parameters, set in one `hangar param set`: `payload_mass=10`, `buoyancy` up by the
payload's weight (98.1 N), the heave entries of `d_lin` and `d_quad` times five
(`depth_ff2.payload_params`). The simulator has no parameter for added mass, so the
10 kg stand for the payload's mass and its added mass together: 36.07 kg to move in
heave where it was 26.07.

The vehicle floats by the same 2 N with and without it, so the trim stays at −0.085
and the integrator does not move. The policy's inputs are the acceleration the
controller wants, the vertical velocity and the integrator: at the same three numbers
the vehicle with the payload needs another heave, and **nothing the policy reads says
which vehicle it is on**. A buoyancy change was carried by an input; this one is
carried by none, so the only way a model can be right for the changed vehicle is to
have been trained on it. For the same reason no single model can be right for both:
M2 is meant for the vehicle with the payload, and what it costs on the vehicle
without is measured (N2 below), not hoped away. An input that would let one model
cover both does not exist in the system: it would be the payload itself (a flag the
operator sets, as a field of some topic the dataset joins) or an estimate of mass and
drag from the last seconds' rows, which is what the trainer is. Adding a field to a
dataset spec and a policy is supported without touching lib/hg; there is nothing to
put in it.

**Why this change and this size.** The change of supply above could not have shown
adaptation, because the policy's bound limited both models. So before any live run the
candidates were put on the offline check's few-line loop
(`depth_ff2_offline.closed_loop`, which now takes a vehicle), with oracles only, no
learned model: on the changed vehicle, the oracle of the unchanged one (the best a
model from before the change can be) against the oracle of the changed one (the best
a model trained since can be), both within the bound; the sum of the two 1 m steps'
IAE, mean of three noise seeds, which differ by 0.05 at most.

| Change | Oracle of the unchanged vehicle (m s) | Oracle of the changed vehicle | To gain |
|---|---|---|---|
| mass +5 kg; +10 kg; −5 kg | 3.30; 3.44; 3.24 | 3.27; 3.37; 3.27 | 0.03; 0.07; −0.03 |
| supply 12.6 V; 20 V | 3.69; 3.18 | 3.64; 3.27 | 0.05; −0.09 |
| heave drag ×2; ×3; ×5 | 3.71; 4.05; 4.63 | 3.61; 3.89; 4.38 | 0.10; 0.16; 0.25 |
| +10 kg and drag ×3 | 4.10 | 3.83 | 0.27 |
| **+10 kg and drag ×5** | **4.68** | **4.30** | **0.38** |
| +15 kg and drag ×5; +20 kg and drag ×4 | 4.69; 4.46 | 4.29; 4.09 | 0.40; 0.37 |

Mass alone and the supply leave a later model nothing to gain that five live runs
could tell from their spread (0.05 to 0.1 m s on such a sum): the feedback asks for
more when the vehicle does less, and the bound caps what either model can add. Drag
does leave something, and a payload with drag is a thing that happens to a vehicle.
The row in bold was taken: `--adaptation` reruns it (five seeds: controller alone 8.21
to 8.26 m s, the oracle of the bare vehicle 4.65 to 4.69, the oracle of the vehicle
with the payload 4.27 to 4.31; of the 0.38 to gain, 0.28 are on the step down and 0.10
on the step up). So at best a later model is **8 % better** than an earlier one here.
That is what this vehicle and this scheme offer; a change was not looked for until the
number was large.

### The schedule

| t (s) | | |
|---|---|---|
| 0 | arm, `assisted`, 3 m; training phase as before (120 s of steps, 20 s at 3 m) | the bare vehicle |
| 170 | **deploy M1**; **A1** `assisted`: hold 3 m (20 s), step to 4 m (25 s), step to 3 m (25 s) | |
| 240 | **H1** `residual:depth_ff` with M1; **A2** the block with M1 | |
| 330 | **C1** the payload on, at the hold, M1 driving | from here the payload |
| 350 | **record**: the training phase's steps again, 180 s, and 20 s at 3 m, M1 driving, the trainer training | |
| 550 | **P1** the block with M1 | |
| 620 | **S2** **deploy M2** at the hold, 30 s | |
| 650 | **P2** the block with M2 | |
| 720 | **C2** the payload off; **N2** the block with M2 | the bare vehicle |
| 810 | **C3** the payload on; **S3** `policy_depth_ff.revert`: M1 again | the payload |
| 850 | **P3** the block with M1 again | |
| 920 | **H2** `assisted`; **P4** the block with the controller alone; **C4** the payload off | |

**What M1 sees**: 140 s of rows from the bare vehicle under the controller alone.
**What M2 sees**: the trainer goes on from M1's plant and policy networks, as it would
on a dive, on a replay buffer of 3 min; at S2 the payload has been on for 300 s, so
every row in the buffer is from the vehicle with the payload and with M1's residual
in the command (the last 80 s of the recording phase's steps, its 20 s hold, P1). M2 is
exported, sent and loaded by `hangar.learn.deploy` and `load_weights` as M1 was,
during the dive, with nothing restarted; the return to M1 is the policy's `revert`.
**The oracle runs** go through the same schedule and the same deploys with two
networks fitted before the dive: as M1 the oracle of the bare vehicle, as M2 the
oracle of the vehicle with the payload (`depth_ff2.oracle(..., **PAYLOAD)`). They say
what the two models could be at best, on the real stack instead of the few-line loop.

P1 and P3 are the same model on the same vehicle before and after M2's block: if they
differ, the difference between M1 and M2 has an order or a drift in it. Each run's
analysis checks from the recorded references which model's residual every block's
commands carried, and that every command of a residual block carried one.

### The criterion, fixed before the comparison

`J` is the sum of the two steps' IAE (3 to 4 m and 4 to 3 m, 25 s each, truth against
the target) in a block on the vehicle with the payload. Five runs a variant, simulator
and trainer seeds 1 to 5, named here before they were run; every run made is reported
and none is repeated by itself. `e2e_depth_ff2.py --summarise` applies the criterion
(`adapt_verdict`) and prints it under the table.

1. **Whether there was anything to see**: in the oracle runs, `J` with the oracle of
   the vehicle with the payload (P2) is lower than with the oracle of the bare vehicle
   before and after it (P1, P3) in each of the five runs, and the largest of the five
   P2 values is below the smallest of the ten P1 and P3 values. If not, the result is
   **undecided**: no later model could have been seen to do better.
2. **Adaptation is shown** if 1 holds and the same holds for the learned runs: `J`
   with M2 lower than with M1 before and after it in each of the five runs, and the
   largest of the five below the smallest of the ten. This is "lower on every seed,
   by more than the seed-to-seed spread" with the spread taken as the whole range.
3. **Not shown** if 1 holds and 2 does not.

Also **undecided**: fewer than five runs of a variant, or a run in which a block ran
under another model than the schedule names, a command of a residual block carried
no residual, or the mux demoted the policy. Runs with the machine's marks on them (a
50 Hz interval over 30 ms, a message the recorder lost, a row the trainer lost) are
listed with the verdict and kept in.

Reported beside it and not part of it: each step by itself, overshoot, the hold, the
thrusters' reversals and the policy's share of ticks at its bound in every block; and
**forgetting**: M2 on the bare vehicle (N2) against M1 on it (A2) and the controller
alone (A1). By the argument above M2 is expected to be worse there than M1; by how
much is the cost of having one model at a time.

### Again on a quiet machine, 2026-11-21: shown

The same command (`E2E_LABEL=quiet tools/sim/e2e-adapt.sh`, the tree at `517dade`),
once, seeds 1 to 5, with nothing else running: the load average was 0.1 to 0.6 at the
start of each pair and 1.0 at its highest. No interval of a 50 Hz topic was over
25 ms in any run, no message or row was lost, nothing was demoted, and no run was
unfit. Reports: [2026-11-21-adapt-quiet/](2026-11-21-adapt-quiet/). **These are the
numbers to quote**; the set made first, under load, is kept below with its analysis,
and the two agree.

With the payload, both steps together (`J`, m s):

| Seed | Controller alone (P4) | M1 (P1) | **M2 (P2)** | M1 again (P3) | M1's mean less M2 | Reversals per second: P4, P1, P2, P3 |
|---|---|---|---|---|---|---|
| 1 | 8.19 | 4.54 | **4.31** | 4.56 | 0.24 | 0.03, 0.09, 0.49, 0.11 |
| 2 | 8.20 | 4.52 | **4.26** | 4.54 | 0.27 | 0.03, 0.23, 0.91, 0.14 |
| 3 | 8.18 | 4.57 | **4.19** | 4.55 | 0.37 | 0.03, 0.03, 0.66, 0.11 |
| 4 | 8.20 | 4.57 | **4.19** | 4.58 | 0.38 | 0.03, 0.31, 0.54, 0.23 |
| 5 | 8.17 | 4.44 | **4.25** | 4.48 | 0.21 | 0.03, 0.37, 0.26, 0.14 |
| median (smallest to largest) | 8.19 (8.17 to 8.20) | 4.54 (4.44 to 4.57) | **4.25** (4.19 to 4.31) | 4.55 (4.48 to 4.58) | 0.27 (0.21 to 0.38) | 0.03, 0.23, 0.54, 0.14 |

The oracle runs: the bare vehicle's oracle 4.43 (4.40 to 4.46) and 4.45 (4.41 to
4.46), the payload's **4.21** (4.19 to 4.23), a gain of 0.23 (0.17 to 0.27) m s.

By the criterion written down before any run: the oracles differ in each of the five
runs, M2 is lower than M1 before and after it in each of the five, and the largest M2
value, 4.31, is below the smallest of the ten M1 values, 4.44. **Adaptation is
shown.** What the loaded set said of it holds here too:

- **It is small**: 6 % of `J`, where the residual as such gives 45 % over the
  controller alone. The gap between the two ranges is 0.13 m s, against ranges 0.12 and
  0.14 wide.
- **It is all on the step from 3 to 4 m**: M1 1.86 (1.84 to 1.92) and 1.89 (1.82 to
  1.93), M2 **1.63** (1.62 to 1.71). From 4 to 3 m, 2.65 and 2.65 against 2.57 (2.56
  to 2.68): inside the spread.
- **M2 forgets the bare vehicle**: with the payload off, 3.23 (3.19 to 3.27) against
  M1's 3.10 (3.04 to 3.14), worse in every run, the ranges apart; its overshoot on the
  3 to 4 m step is 7.0 % (4.3 to 10.6) against 0.9.
- **M2 reverses the thrusters more**: 0.54 a second (0.26 to 0.91) with the payload
  against M1's 0.23 and 0.14, and 0.80 (0.23 to 1.37) without it against 0.17; the
  controller alone 0.03. The payload's oracle does not (0.49 against 0.66 and 0.57).
- **Not separated here either**: M2 was trained longer and on richer rows than M1,
  and no model trained as long on the bare vehicle was run.

One thing differs from the other nine runs and is not explained: in learned seed 1
the median `nav.depth` to `thr.cmd` latency was 40.7 ms (p99 61.0), where every other
run of both sets has 58 to 60. Its `J` values are inside the other seeds' ranges.

### The runs of 2026-11-21, under load: the first set

**These numbers were the provisional ones.** The machine was not quiet: three other agents
were building and running test suites on it. The load average was 0.3 to 4.0 at the
start of each pair of runs and reached 5.3 during the first (12 threads); the table
has it per run. The runs were made two at a time, a learned and an oracle run of one
seed together, each on six threads of its own (`taskset -c 0-5` and `6-11`), by
`tools/sim/e2e-adapt.sh` at `ce24ed5`, once, seeds 1 to 5, none repeated. The set is
made again on a quiet machine since (above), with the same verdict.
Reports: [2026-11-21-adapt-provisional/](2026-11-21-adapt-provisional/), whose
`summary.md` has every metric per seed.

| Seed | Learned: load average at the start, highest, at the end | `nav.depth` to `thr.cmd`: median, p99 (ms) | 50 Hz intervals over 25 ms, over 30 ms | Oracle: load average | Median, p99 (ms) | Over 25 ms, over 30 ms |
|---|---|---|---|---|---|---|
| 1 | 0.91, 5.33, 4.02 | 59.6, 60.2 | 5, 0 | 1.01, 5.33, 4.01 | 59.5, 60.1 | 3, 0 |
| 2 | 4.09, 4.83, 1.39 | 58.2, 59.0 | 1, 0 | 4.42, 4.83, 1.39 | 59.2, 60.0 | 0, 0 |
| 3 | 1.43, 3.75, 1.48 | 59.6, 60.3 | 3, **1** | 1.91, 3.75, 1.15 | 59.6, 60.3 | 0, 0 |
| 4 | 1.14, 1.50, 0.44 | 59.4, 60.0 | 0, 0 | 1.18, 1.50, 0.34 | 59.7, 60.3 | 0, 0 |
| 5 | 0.47, 1.44, 0.56 | 59.4, 60.1 | 3, 0 | 0.59, 1.44, 0.44 | 59.5, 60.2 | 1, 0 |

The load average is the 1 min one, of the whole machine, the run's own processes
included (two runs by themselves make about 0.5). No message was lost by the
recorder and no row by a trainer in any run; the mux demoted nothing; every block ran
under the model the schedule names, with a residual in every command of a residual
block, by the recorded references. One interval of one 50 Hz topic was over 30 ms, in
learned 3.

**With the payload, both steps together** (`J`, m s; the reversals are the heave
command's zero crossings per second over the block):

| Seed | Controller alone (P4) | M1 (P1) | **M2 (P2)** | M1 again (P3) | M1's mean less M2 | Reversals per second: P4, P1, P2, P3 |
|---|---|---|---|---|---|---|
| 1 | 8.18 | 4.45 | **4.32** | 4.44 | 0.13 | 0.03, 0.37, 0.69, 0.53 |
| 2 | 8.19 | 4.49 | **4.32** | 4.51 | 0.18 | 0.03, 0.31, 0.37, 0.29 |
| 3 | 8.16 | 4.56 | **4.15** | 4.58 | 0.41 | 0.03, 0.09, 0.74, 0.14 |
| 4 | 8.18 | 4.54 | **4.20** | 4.55 | 0.35 | 0.06, 0.17, 0.49, 0.06 |
| 5 | 8.19 | 4.45 | **4.25** | 4.48 | 0.21 | 0.03, 0.71, 0.94, 0.31 |
| median (smallest to largest) | 8.18 (8.16 to 8.19) | 4.49 (4.45 to 4.56) | **4.25** (4.15 to 4.32) | 4.51 (4.44 to 4.58) | 0.21 (0.13 to 0.41) | 0.03, 0.31, 0.69, 0.29 |

The oracle runs, the oracle of the bare vehicle in M1's place and the oracle of the
vehicle with the payload in M2's:

| Seed | Controller alone (P4) | Bare vehicle's oracle (P1) | **Payload's oracle (P2)** | Bare vehicle's again (P3) | The mean less P2 | Reversals per second: P4, P1, P2, P3 |
|---|---|---|---|---|---|---|
| 1 | 8.21 | 4.45 | **4.19** | 4.46 | 0.26 | 0.03, 0.54, 0.40, 0.57 |
| 2 | 8.20 | 4.44 | **4.20** | 4.45 | 0.25 | 0.03, 0.57, 0.34, 0.60 |
| 3 | 8.20 | 4.43 | **4.20** | 4.42 | 0.22 | 0.03, 0.94, 0.51, 1.04 |
| 4 | 8.19 | 4.44 | **4.23** | 4.47 | 0.23 | 0.03, 0.83, 0.29, 0.53 |
| 5 | 8.19 | 4.43 | **4.20** | 4.42 | 0.23 | 0.03, 0.63, 0.54, 0.43 |
| median (smallest to largest) | 8.20 (8.19 to 8.21) | 4.44 (4.43 to 4.45) | **4.20** (4.19 to 4.23) | 4.45 (4.42 to 4.47) | 0.23 (0.22 to 0.26) | 0.03, 0.63, 0.40, 0.57 |

**The criterion, as `--summarise` applied it: adaptation is shown**, in this set and
in the quiet one above.

1. The oracles differ: 4.19 to 4.23 m s against 4.42 to 4.47, lower in each of the five
   runs than before and after. There was something to see: 0.23 m s, 5 % of `J`.
2. M2 is lower than M1 before and after it in each of the five runs, and the largest
   M2 value, 4.32, is below the smallest of the ten M1 values, 4.44.

What that is and is not:

- **It is small.** The residual takes `J` on the vehicle with the payload from 8.18 to
  4.49 m s with a model that has never met the payload; the model trained on it takes
  it to 4.25. The first is 45 %, the second 5 % more. The gap between the two ranges
  is 0.12 m s, against ranges 0.14 and 0.17 wide: the ranges do not overlap, and that
  is all. A sixth run could close the gap; the loaded runs above differed from their
  quiet repeats by up to 0.05 m s on one step.
- **It is all on the step down.** 3 to 4 m: M1 1.86 (1.86 to 1.88) and 1.90 (1.86 to
  1.91), M2 **1.70** (1.63 to 1.71), overshoot 2.0 and 3.6 % against 0.0 (0.0 to 0.7).
  4 to 3 m: 2.63 (2.57 to 2.68) and 2.61 (2.58 to 2.69) against 2.57 (2.52 to 2.62),
  inside the spread; the two oracles do not differ there at all (2.56 and 2.59
  against 2.57). The vehicle floats, so going up the command crosses the deadband and
  the correction is at its bound, whatever the model knows: the supply runs' finding
  again. The few-line loop had promised 0.28 m s on the step down and 0.10 on the step
  up, 0.38 together; the stack gave the oracles 0.25 and nothing, 0.23 together.
- **M2 is as good as the oracle of the vehicle it was trained on, M1 as good as the
  oracle of the one it was trained on**: 4.25 (4.15 to 4.32) against 4.20 (4.19 to
  4.23), and 4.49 and 4.51 against 4.44 and 4.45, the learned ones with three to four
  times the oracles' spread. In seeds 3 and 4 the learned gain (0.41, 0.35) is larger
  than the oracles' (0.22, 0.23) because M1 was worse there than its oracle (4.56 and
  4.54) and M2 as good as or better than its own (4.15, 4.20).
- **Order and drift**: M1 before and after M2 gives the same `J` (4.49 and 4.51;
  within a run they differ by 0.03 at most), so what separates M2 from M1 is not when
  its block was run. The overshoot of the step down does differ between the two M1
  blocks (2.0 % and 3.6 %; the oracle's 1.8 and 3.3), and so does the hold (below):
  the integrator was on its way back to the trim through all three.
- **What M2 had that M1 had not**, besides the payload: 35 700 training steps where M1
  had 8350, and rows with a residual in the command, which reach further (commands
  −0.37 to +0.34 where M1's rows had −0.23 to +0.12). A model trained as long on the
  bare vehicle with M1 driving was not run, so the runs do not separate "trained on
  the changed vehicle" from "trained longer, on richer rows" by themselves. Two
  things speak for the first: M2 is worse than M1 on the bare vehicle (below), and
  each of the two sits at its own vehicle's oracle.
- **M2 is trained on the live rows**, the pairer's `dataset.depth_ff` through the
  trainer's 3 min replay buffer, going on from M1's networks: the loop as built. It is
  not trained from the recorder's file of the same rows after the fact. From the
  newest row M2 was trained on to the first thruster command carrying it: 0.70 to
  0.74 s. No interval of a 50 Hz topic during a load was over 20.8 ms.

**Forgetting.** M2 on the bare vehicle (N2) against M1 on it (A2), both steps, m s:

| Seed | Controller alone (A1) | M1 (A2) | M2 (N2) | M2 less M1 | Reversals per second: A1, A2, N2 |
|---|---|---|---|---|---|
| 1 | 4.89 | 3.01 | 3.15 | +0.14 | 0.03, 0.31, 1.17 |
| 2 | 4.87 | 3.09 | 3.15 | +0.06 | 0.03, 0.26, 0.54 |
| 3 | 4.86 | 3.10 | 3.23 | +0.13 | 0.03, 0.20, 0.86 |
| 4 | 4.88 | 3.06 | 3.26 | +0.21 | 0.03, 0.20, 0.60 |
| 5 | 4.90 | 3.03 | 3.20 | +0.17 | 0.06, 0.20, 0.97 |
| median (smallest to largest) | 4.88 (4.86 to 4.90) | 3.06 (3.01 to 3.10) | 3.20 (3.15 to 3.26) | +0.14 (0.06 to 0.21) | 0.03, 0.20, 0.86 |

M2 is worse than M1 on the vehicle M1 was trained on, in each run, the ranges apart:
it has forgotten, as a model with these inputs must. The cost is on the step down
(1.43 m s (1.37 to 1.44) against 1.22 (1.20 to 1.24), overshoot 4.7 % (3.4 to 8.9)
against 0.9), where it pushes for drag that is not there; the step up is inside the
spread (1.78 against 1.85). The two oracles: 3.31 against 3.12, overshoot 10 % against
1 %. M2 is still far better there than the controller alone (4.88). So a model for
the vehicle with the payload gains 0.21 m s with it and loses 0.14 without it; which
one drives is the operator's `load_weights` or `revert`, there being no input to
choose by. N2 has no block of M1 after it, so its 0.14 has whatever the order adds
in it; the integrator was at −0.067 in N2's hold and at −0.084 in A2's.

**Thruster reversals.** Controller alone: 0.03 per second, the two steps' own. With
M1: 0.20 (0.20 to 0.31) on the bare vehicle, 0.31 and 0.29 (0.06 to 0.71) with the
payload. **With M2: 0.69 (0.37 to 0.94) with the payload and 0.86 (0.54 to 1.17)
without**, more than M1 before and after it in each of the five runs: the later model
reverses the verticals two to four times as often as the earlier one, and twenty
to thirty times as often as the controller. The oracles do not show that: the payload's oracle
reverses less than the bare vehicle's on the vehicle with the payload (0.40 against
0.63 and 0.57) and as little on the bare one (0.11 and 0.11). So the extra reversals
are the learned M2's, not the price of knowing the payload; what in M2 makes them
(the policy's fit near zero acceleration is where to look first) was not looked into.
The policy was at its own bound on 7.9 % of the ticks with M2 and the payload, 5.7 %
and 5.3 % with M1 (the oracles: 7.6 %, 3.7 %, 3.6 %).

**Seen on the way, not part of the criterion.**

- The payload moved nothing at the hold, as designed: switching it on or off with a
  model driving, 2.8 to 3.3 cm peak (the medians of C1, C2, C3); off under the controller alone (C4), 2.2 cm, an
  undisturbed hold. The controller alone on the vehicle with the payload gives 8.16 to
  8.21 m s, where the few-line loop has 8.21 to 8.26: the simulator's payload is the
  one the oracle was told of.
- The hold with a residual and the payload reads 2.85, 2.39 and 2.11 cm rms in P1, P2
  and P3 (the oracles 3.21, 2.56, 2.02; the controller alone 2.00): the block order,
  not the model. The recording phase's steps leave the integrator off the trim
  (−0.059 in P1's hold, −0.070 in P2's, −0.083 in P3's, the trim being −0.085) and
  the policy supplies the rest, with the vehicle up to a centimetre from where the
  sensor's bias would put it, as after the buoyancy change above.
- **Taking the residual away cost 11.4 cm** (9.9 to 12.1; the oracle runs 12.4) at H2,
  where it cost 3.0 cm in the runs above. H2 here follows a block of steps on the
  vehicle with the payload; the integrator's place at that moment is the likely
  reason and was not checked (the runs kept no session).
- The swap to M2, the revert to M1 and the promotion: 3.2, 2.7 and 2.4 cm.
- Train against serve: the policy read another `ctl.depth_hold.target` message than
  the pairer joined at one anchor in learned 2 and one in oracle 5 (there the outputs
  differ by 1.9e-3 on that anchor; elsewhere by 1.5e-7 at most).

**What is placeholder in it.** Everything the second design's list below names, and:
one payload, chosen on the few-line loop so that an oracle could gain something; one
block of steps; a payload that comes on at once, with the operator knowing when; the
added mass folded into the mass; M2 deployed at a time the schedule set, not when the
trainer's loss said the vehicle had changed; and the machine's load.

## What is placeholder in the second design

- **Everything the simulator gives for free** (the list under [The problem](#the-problem)):
  the accelerometer, thrust that depends on the command alone, still water, `thr.out`
  equal to what was commanded. On a vehicle `thr.out` is ArduSub's servo output after
  its mixer, and "still" has a tether and a current in it.
- **The lag of 2 ticks** and the other constants of `depth_ff2.py` (what counts as
  still, 50 rows for the heave at rest, the weight of moving rows, the bound at 90 %)
  were set from the offline check on this simulator and not varied.
- **The plant network is not recorded.** The weights file holds the policy and names the
  rows; the plant the policy was fitted to lives in the trainer and is gone with it.
- **The oracle's weights file** names rows that trained nothing, and its supply is a
  constant 16.8 V where the battery went from 16.8 to 16.7 V.
- **The controller's parameters** are the profile's (`accel_max = 4`, `i_band = 0.6`),
  for the baseline and the residual alike: the defaults do not hold this vehicle
  through the buoyancy change (above). What the residual does under the defaults was
  not run.
- **One step size and one depth.** Steps of 1 m between 3 and 4 m, a hold at 3 m, one
  buoyancy change of 4 N; the training phase's steps are 0.1 to 1.5 m.
- **The deploy, the trainer and the operator** are as in the first experiment: a copy
  into `incoming/`, a thread of the experiment's script, two deploys at fixed times.
- **Timing**: a laptop under other load (above); nothing ran on the Pi or the vehicle.
- **The direct inverse** was ruled out by the offline check's few-line loop and never
  run live.

## Files of the second design

| | |
|---|---|
| `tools/sim/e2e-depth-ff2.sh` | the root, the manager, the run; `E2E_SEED`, `E2E_ARGS` |
| `tools/sim/e2e_depth_ff2.py` | the operator and the trainer's thread; `--variant learned`, `oracle`, `collect`; `--change supply`; `--adapt`; `--analyse`; `--summarise` |
| `tools/sim/e2e-adapt.sh` | the adaptation experiment whole: ten runs with `--adapt` and their table, into a dated directory here |
| `tools/sim/depth_ff2_offline.py` | the offline check; `--adaptation` for the payload |
| `profiles/sim-depth-ff2/` | the dataset and policy specs; the rest is `sim-depth-ff`'s |
| `python/examples/depth_ff2.py` | the plant, the heave at rest, the inversion, the policy, the oracle |
| `python/tests/test_depth_ff2.py` | their tests |
| `2026-11-20-second/<run>/` | `report.md`, `report.json`, `events.log`; `offline/` the offline check's result and the two collect runs; `supply/` the supplementary runs |
| `2026-11-21-quiet/` | the ten runs again on a quiet machine |
| `<date>-adapt*/` | a set of `e2e-adapt.sh`: `summary.md` (the machine, every run's load and loop delay, every metric per seed, the criterion's verdict), `machine.txt`, and a directory a run |
