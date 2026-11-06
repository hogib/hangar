# hangar: a lightweight module system for underwater vehicles, with a recorded learning loop

*Research proposal. H. Oğuz Bolat, Department of Geomatics Engineering, Yıldız Technical
University, İstanbul.*

## Summary

hangar is a module system for remotely operated vehicles (ROVs) of the BlueROV2 class,
native to Linux and small enough for the vehicle's Raspberry Pi. Every function of the
vehicle (a sensor driver, a camera, a controller, the tether link, a neural network) is
its own process; one manager starts, supervises and restarts them; they exchange
fixed-size messages through shared memory, and no reader can delay a writer. Its purpose
is a learning loop. Every message records the input messages it was computed from.
Training samples stream live from the vehicle to a trainer on a topside GPU. Trained
weights return to the running vehicle, where they are checked, run in shadow, and drive
only when an operator promotes them, and only within an envelope. I propose to implement
hangar, compare it with ROS 2 on the same Pi, test its failure behaviour on the bench,
and evaluate the loop on two tasks: imitating the pilot, and adapting a depth controller
during a dive.

## Motivation and problem

On every dive the pilot demonstrates how the vehicle should be driven, and the sensors
record how it responded. Learning from this data meets three obstacles. A recording
usually holds messages and timestamps, not which inputs a module used for each output;
training pairs are reconstructed by matching times, and features built offline can
differ from those a model meets on the vehicle. Moving a model trained topside onto a
running vehicle is, in the setups we know of, a manual step outside the middleware, with
nothing to check the weights or remove a misbehaving model. And ROS 2, the usual base,
brings a DDS middleware and a runtime type system to a computer that also runs the
autopilot and encodes video.

## Proposed approach

**Transport.** A topic is one shared-memory ring per output, with one writer and any
number of readers; a sequence lock per slot detects torn copies. The design budgets are
under 1 µs to publish and under 0.5 µs to read a 256-byte message on a Pi 4, without a
system call, and a static manager under 200 KB using under 2 MB for 20 modules. The
manager moves no data; if it crashes, the modules keep running.

**Provenance and datasets.** Each message lists up to four input messages as (topic,
sequence number), filled in by the library; an output computed from a stale input is
flagged, transitively. A dataset spec names an anchor topic and the inputs joined to it,
by provenance or by time, with bounds that make time joins deterministic. The same join
code runs in the live pairer on the vehicle, in the offline build from recordings, and in
the policy that builds its inputs on the vehicle.

**Tether.** A bridge mirrors topics over authenticated UDP, keeping sequence numbers, so
a provenance link means the same message on both sides. A strict priority order keeps
dataset rows and weight files from delaying pilot commands.

**Policies.** A policy module runs a small inference engine (dense, convolution, LSTM and
a few other operators) that allocates all memory at load. A weights file carries an
architecture hash, probe inputs with expected outputs, and the lineage of its training
rows. The vehicle checks hashes, architecture, finite values, probes and step time, warms
the model up beside the running one, and swaps it between two ticks; `revert` restores
the previous one.

**Safety.** All control sources pass through one multiplexer (the mux), which enforces
thrust, rate and depth limits. The pilot wins on any axis. A new model is recorded but not
applied until an operator promotes it, and is demoted automatically if its output goes
stale or sits at a limit for more than 20% of the last 5 s. In residual mode a network
may add at most 20% of the envelope to a classical controller. Every actuator passes
through one gate that outputs neutral unless the vehicle is armed and the command is
fresh. In the first version ArduSub still drives the thrusters, with its own failsafes on.

## Contribution

The claimed contribution is the learning loop as a property of the module system, with
three guarantees: one definition of a sample, giving byte-identical rows live, offline
and at run time; a deployment path with checks, shadow operation, promotion only by an
operator, automatic demotion and an envelope; and lineage by identifiers from any
thruster command to the model, its training rows and the sensor messages behind them. To
our knowledge, no existing system for small underwater vehicles combines these. A
literature review (robot middleware, learning from demonstration, adaptive underwater
control) is part of the work, and the claim will be narrowed where prior work covers it.
Secondary claims are a much smaller footprint than ROS 2 and safe failure behaviour.

## Evaluation plan

| Question | Measure | Criterion |
|---|---|---|
| Lighter than ROS 2? | same Pi, same message sizes and rates: memory, CPU, latency, start time, installed size | budgets met; reproducible by a script |
| Same samples everywhere? | live rows vs rows built offline; policy inputs vs row inputs | byte-identical where no message was missed |
| Lineage | recorded thruster commands traced back to sensor messages | by identifiers alone |
| Loop latency | newest row trained on to first promoted output | reported per model |
| Imitation | shadow error per axis against the pilot | reported |
| Adaptation | depth error, controller alone vs with residual, after a payload or ballast change | reported |
| Safe failure | nine bench tests, propellers off (Pi hang, killed modules, tether pulled, override, envelope) | time to neutral recorded; for killed modules, within one timeout plus one output period |
| electric-eye | ported operators hashed against v0.3.1; video latency | identical; no worse than 3.8 / 8.3 ms |

The learning tasks run first on recorded data, then on the bench with propellers off,
and in water only when a vehicle and a test tank are available.

## Work plan (draft, to be adjusted)

1. **Language and literature.** A small comparison of C11 and Rust; the review begins.
2. **Core.** Topics, manager, calls, parameters; budgets as tests; the ROS 2 benchmark.
3. **Vehicle integration.** Safety, mux and gate; ArduSub via MAVLink in simulation;
   tether bridge; electric-eye supervised unchanged.
4. **Data.** Recorder, pairer, offline build; the live-equals-offline test.
5. **Learning loop.** Inference engine, weights, loading, lineage; both tasks on recorded
   data.
6. **Bench and water.** Bench tests on vehicle hardware; both tasks on the bench, then in
   water if possible; electric-eye ported and checked.
7. **Write-up.**

## Risks

- **Scope.** Thirteen design documents for one student. Each phase ends in a measurable
  result, and the loop can be shown without wet runs.
- **Hardware.** The work needs a Raspberry Pi 4 or 5, a topside computer with a GPU, and,
  for wet runs, a vehicle and a tank.
- **Weak policies.** A few dives may not train a good policy. The contribution is the
  loop and its guarantees; results are reported as measured.
- **Footprint.** If hangar is not much lighter than ROS 2, that is reported.
- **Prior work** may cover parts of the contribution; the review decides the claim.

## Current status

The design is complete in thirteen documents. Implementation has not started, and the
language is not yet chosen. Nothing has run on a vehicle. electric-eye v0.3.1, whose frame
bus, process handling, discovery and configuration hangar builds on, is released and
measured on desktop and laptop computers and, for ARM, under emulation: 3.8 ms transport latency at
640×480 and 8.3 ms at 1280×720 over loopback. It has not run on a vehicle either.
