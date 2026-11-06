# Tools

What people use to run, inspect, debug, measure and learn from a hangar vehicle: one
static command, `hangar`, on the vehicle and the topside; a Python package for the
topside and for training; and a benchmark that makes the case against ROS with
numbers.

## The rules

- **No daemon behind the tools.** `hangar` reads topics, the registry and module pages
  directly, like `hellion top` reads its status file: a wedged manager still shows its
  last state, and how old it is.
- **Reading needs group `hangar`; changing anything needs `hangar-admin`**
  ([03-manager](03-manager.md#control-socket)). A caller with uid 0 counts as
  `hangar-admin`, so `systemctl reload` can run `hangar restart-manager`.
- **An operator passes only where `Allow` names one.** `hangar call` and
  `hangar param` act as `admin:<user>`, which matches the `Allow` token `admin`; a
  method of the vehicle called from the topside arrives as `topside:admin:<user>`,
  which matches `topside:admin` ([04-calls](04-calls.md#callers)). A method or a
  parameter set whose `Allow` does not name the operator refuses the operator, as it
  refuses any module.
- **One code path for one job.** `hangar dataset build` uses the pairer's own join code,
  so offline rows equal live rows; the Python package never re-implements a join.
- **Nothing on the vehicle needs Python.**

## The `hangar` command

One static binary (musl), with subcommands:

| Command | |
|---|---|
| `hangar top` | Live dashboard, for a terminal or SSH: modules (state, restarts, status line), topics (rate, age, writer, stale flags), the link, the safety state, the mux mode, the recorder, the policies (running model, load progress). On the topside it shows both machines ([below](#both-machines)). `--once` prints one frame for scripts |
| `hangar check` | The pre-dive check ([below](#hangar-check)) |
| `hangar show <module>` | Its wiring as handed to it, its state, last exit, status line, `Neutral` command, parameters |
| `hangar topics` | Every topic: type, rate declared and measured, age, writer, slots, flags; for a mirrored topic, the sender's boot id and the messages lost and skipped |
| `hangar echo <topic>` | Messages decoded from the schema in the topic's header; `--rate`, `--fields`, `--json` |
| `hangar hz <topic>` | Measured rate and jitter |
| `hangar call <module>.<method> k=v …` | A call, as `admin:<user>`, blocking (`hg_call`); the request built from the type's schema |
| `hangar param get\|set\|load\|reset` | Parameters ([05-parameters](05-parameters.md#the-hangar-param-command)) |
| `hangar start\|stop\|restart <module>` | |
| `hangar clear-failsafe` | Ask the manager to leave system failsafe; refused, naming the module, while a module given up with `OnGiveUp=failsafe` is still `failed` ([03-manager](03-manager.md#system-state-and-the-managers-topics)) |
| `hangar reload` | Read the manifests again ([02-manifest](02-manifest.md#reloading)) |
| `hangar restart-manager` | Re-execute the manager in place, modules kept ([03-manager](03-manager.md#restarting-the-manager)) |
| `hangar run <module>` | A module in the foreground, for development: its wiring from its manifest, a private topic directory ([01-topics](01-topics.md#writing)) |
| `hangar graph` | The wiring graph in DOT: modules, topics, calls, provenance links (`Inputs` and `Sources`), with live rates and states |
| `hangar dataset build\|check\|pairs\|loop-latency` | Datasets from recordings, with the pairer's code ([below](#datasets-and-analysis)) |
| `hangar msg gen` | `.msg` files to the language's types and to Python dtypes, at build time |
| `hangar bench` | The budgets of [00-overview](00-overview.md#budgets), measured, as a report ([below](#the-benchmark)) |

`hangar top` follows `hellion top`: plain ANSI, no curses library, four refreshes a
second, readable over a slow SSH session.

### Both machines

`hangar top` and `hangar check` read the machine they run on. On the topside they
also show the vehicle, from the topics the bridge mirrors up: `hangar.vehicle.system`,
`hangar.vehicle.modules`, `bridge.vehicle.link`, `safety.state`, `recorder.status`
([08-bridge](08-bridge.md#configuration)). Each line names its machine, `vehicle` or
`topside`. The topside's own manager and link are `hangar.topside.*` and
`bridge.topside.link`, and its recorder, when it runs one, is `topside_recorder`. What
the vehicle shows on the topside is as old as its mirror: with the link down, it is
stale, and the line says so.

## `hangar check`

One line per check, `OK`, `WARN` or `FAIL`, naming what is wrong; exit status 0
healthy, 1 degraded, 2 down, as `hellion check`. Run on the topside, it checks both
machines ([Both machines](#both-machines)); its vehicle checks that need a call
(`safety.check`) are made across the tether, as `topside:admin:<user>`.

| Check | Fails (2) when | Warns (1) when |
|---|---|---|
| Manager | not running; `hangar.<role>.system` is `failsafe` | its heartbeat is late; the system is `degraded` |
| Modules | one is `failed` or `refused` | one has restarted in the last 10 minutes |
| Topics | a rated topic is stale | a topic is flagged as computed from stale inputs (a policy with no running model is reported under Policies); a subscription is absent |
| Link | the bridge is down, `ambiguous` or `refused`; a module or topic name, or a topic id, exists on both machines, so its mirror or call is refused | the two configurations differ; clock uncertainty over 5 ms; datagram loss over 1% (`loss_up`, `loss_down`, counted from `packet_seq`) |
| Safety | the arming checklist would fail (`safety.check`, a dry run of `arm`) | the vehicle is `arming` or `armed` |
| Autopilot | ArduSub has sent no message for 3 s; `FS_GCS_ENABLE` or `FS_LEAK_ENABLE` is off; the pilot gain is not pinned (`JS_GAIN_DEFAULT`, `JS_GAIN_MIN`, `JS_GAIN_MAX` and `JS_THR_GAIN` not all 1); the signing key `/etc/hangar/mavlink.key` is missing, or readable by anyone outside group `hg-autopilot` ([10-mavlink](10-mavlink.md#signing)) | |
| Actuators | an actuator has no `Neutral` command, or a `Neutral` command (an actuator's, or a `Legacy` module's `[Module] Neutral`) is not executable | the hardware watchdog is not armed |
| Recorder | the black box is off; its `Dir` is not group `hangar-rec` with mode 2770 | free space under twice `MinFree`; the black box covers a rated topic of class `high` or `normal` for less than `BlackBox` (from `recorder.status`) |
| Policies | | a policy is in `policy:` or `residual:` mode; a policy has no running model |
| Configuration | a module named by a `[Calls] Remote=` entry is enabled on both machines (found at the handshake); a policy module's name is not `policy_<Name>` of its spec | a manifest, dataset spec, policy spec, `bridge.conf`, `recorder.conf` or `pairer.conf` changed on disk since the last reload or start |

The Autopilot line reads ArduSub's state and parameters as the vehicle's `mavlink`
module reports them (`ardusub.state`, `ardusub.param`); it applies while ArduSub drives
the thrusters.

## Datasets and analysis

`hangar dataset build <spec> <session>` runs the pairer's join code over a session's
files, so its rows are byte-identical to the live rows, `boot_id`, `reset` and
`masked` included ([06-recorder](06-recorder.md#offline-the-same-rows-from-a-recording)).

`hangar dataset check <spec> [<policy spec>] [<session>]` checks a spec before it is
used, one line per check, `OK`, `WARN` or `FAIL`, with the exit status of
`hangar check`. Without a policy spec it says whether the spec is usable by a policy
at all, and names the `input` joins that would need a substitution
([09-learning](09-learning.md#features-at-run-time)).

| Check | Fails (2) when | Warns (1) when |
|---|---|---|
| Spec | it does not parse; a field, or a `provenance` join, does not match the manifests; an `asof` join has `offset + Lateness` above `MaxDelay` | |
| Policy | with a policy spec: an `input` join has `offset + Lateness` above 0 and the policy does not substitute it; for a recurrent policy (an `lstm` op), `Filter` is not `mask`, `Missing` is not `flag`, or `Rate` is not `every:1` | |
| Labels | | a `label` part taken from `pilot.wrench`, or from another wrench topic whose axes the mux sources per axis (`ctl.wrench`), without a `mux.state` `filter` part: such a label is valid only on the axes `mux.state` says the pilot drove ([09-learning](09-learning.md#example-imitating-the-pilot)). A label from `ardusub.manual_control` (type `manual_control`) needs none: it is the command sent, whoever drove each axis ([09-learning](09-learning.md#example-adaptive-depth-hold)) |
| Session | with a session: a part's topic was recorded `meta` or `off` in it, so the spec cannot be built from that session | |

`hangar dataset pairs <session> <module>` builds the spec a manifest implies: each
output of the module, its `Inputs` joined by provenance.

`hangar dataset loop-latency <session>` reports, per model, the time from the newest
row it was trained on to its first promoted output ([09-learning](09-learning.md#lineage));
a model trained on no rows of the vehicle boot it ran in has none.

Everything else done with recordings and datasets afterwards is analysis, and lives in
the Python package.

## The Python package

`hangar`, for the topside and for training; installed as a wheel with one small compiled
helper: the seqlock read on ARM ([01-topics](01-topics.md#reading-from-python)), and
the atomic increment and `FUTEX_WAKE` of a wake word, which a call needs
([04-calls](04-calls.md#protocol)).

| Module | |
|---|---|
| `hangar.topics` | Read topics: newest, in order, wait; `dtype_from_schema` |
| `hangar.recordings` | Sessions and `.hgr` files as `numpy.memmap` |
| `hangar.datasets` | Dataset topics and files; rows with their `boot_id` and their `reset` and `masked` flags ([06-recorder](06-recorder.md#the-row)) |
| `hangar.learn` | Stateful streaming for recurrent models (`carry`), replay, export, deploy, revert, watch; run as the topside module `trainer` ([09-learning](09-learning.md#training)) |
| `hangar.export` | MCAP (for Foxglove) and Parquet |
| `hangar.report` | Loop latency, call records, gaps, plots |

Reading needs no identity: topics and recordings are files, readable by group
`hangar`. Calls do. The trainer is the topside module `trainer`, started by the
topside's manager, whose manifest declares `[Call bridge.send]` and the
`load_weights`, `revert` and `runtime_info` calls of its policy
([09-learning](09-learning.md#training)). Inside it, `hangar.learn` does what `hg_init`
does: it makes the process non-dumpable, reads the wiring from `HANGAR_WIRING`, and
fetches its module page and call socket from the manager with `attach`
([03-manager](03-manager.md#starting-a-module)). Its calls are then `trainer`'s, and
reach the vehicle's policies as `topside:trainer`. A script run from a shell, outside
the manager, is not `trainer`: it calls as `admin:<user>` if its user is in
`hangar-admin`, and the vehicle sees it as `topside:admin:<user>`, which passes only
where `Allow` names `topside:admin`; any other user is refused.

## The benchmark

The reason hangar exists, measured: `hangar bench` on hangar, and the scripts in
`bench/ros2/` on ROS 2 (a talker and a listener with its default middleware), on the
same machine, for the same message sizes and rates. ROS 2 runs only on the comparison
machine, in a container or on a separate SD card image, never on the vehicle.

Both measure:

| | |
|---|---|
| Resident memory | per process and in total, at idle and at rate |
| CPU | at idle, and at 100 Hz, 1 kHz and 10 kHz |
| Latency | publish to the reader's wake-up, median and 99th percentile |
| Publish cost | per message, by size |
| Size | binaries and libraries installed |
| Start | time from start to the first message received |

and write one report, with the machine, kernel and versions, so that the comparison can
be reproduced by anyone.

## Tests

- `hangar check` against staged faults, one for each line of its table, on the vehicle
  and on the topside; run on the topside, a vehicle fault reported through the
  mirrored topics and named `vehicle`, and the vehicle's lines shown stale with the
  link down.
- `hangar check` Autopilot against ArduSub SITL: each failsafe parameter off, each
  gain parameter changed, the key file's group and mode wrong.
- `hangar dataset check` against specs staged for each line of its table, with and
  without a policy spec, and against a session that recorded a part `meta`; a
  `pilot.wrench` label without `mux.state` warned; the `pilot_imitation` and
  `depth_ff` specs of [09-learning](09-learning.md) pass with no warning.
- `hangar clear-failsafe` refused while a module given up with `OnGiveUp=failsafe` is
  `failed`, accepted once it is restarted.
- `hangar echo`, `call` and `param` driven by schemas alone, against every message type
  in `msg/`.
- `hangar call` and `hangar param`: allowed where `Allow` names `admin`, denied where
  it does not; from the topside, allowed only where `Allow` names `topside:admin`.
- `hangar dataset build` equal to the live pairer on a recorded session, the `reset`
  and `masked` flags included.
- The Python package's readers against the C writer, on x86-64 and ARM.
- The trainer as a module: its calls reach the vehicle as `topside:trainer`; the same
  script run from a shell calls as `admin:<user>`.
- `hangar bench` and the ROS 2 scripts run in CI on x86-64, and on a Pi before each
  release.
