# Tools

What people use to run, inspect, debug, measure and learn from a hangar vehicle: one
static command, `hangar`, on the vehicle and the topside; a Python package for the
topside and for training; and a benchmark that makes the case against ROS with
numbers.

## The rules

- **No daemon behind the tools.** `hangar` reads topics and module pages directly,
  like `hellion top` reads its status file: a wedged manager still shows its last
  state, on `hangar.<role>.modules`, and how old it is. The registry is the manager's
  alone ([03-manager](03-manager.md#adoption)); everything a tool needs of it is on
  that topic.
- **Reading needs group `hangar`; changing anything needs `hangar-admin`**
  ([03-manager](03-manager.md#control-socket)). A caller with uid 0 counts as
  `hangar-admin`, so `systemctl reload` can run `hangar restart-manager`.
- **An operator's identity is written by the manager, never by the tool.** `hangar
  call` and `hangar param` ask the manager over the control socket; the manager, the
  one writer of `hangar.<role>.admin.req`, publishes the request there with
  `admin:<user>` in the envelope's `caller` field, which servers trust on that topic
  alone ([04-calls](04-calls.md#admin-calls)). The command then reads the reply on the
  server's `<module>.rep` topic, like any caller. A method of the vehicle called from
  the topside goes on `hangar.topside.admin.req`, which the bridge mirrors down, and
  arrives as `topside:admin:<user>` ([08-bridge](08-bridge.md#calls-across-the-tether)).
- **An operator passes only where `Allow` names one.** `admin:<user>` matches the
  `Allow` token `admin`; `topside:admin:<user>` matches `topside:admin`
  ([04-calls](04-calls.md#allow-tokens)). A method or a parameter set whose `Allow`
  does not name the operator refuses the operator, as it refuses any module.
- **One code path for one job.** `hangar dataset build` uses the pairer's own join code,
  so offline rows equal live rows except where a row's `late` flag is set or one row
  alone marks a part `missing`
  ([06-recorder](06-recorder.md#offline-the-same-rows-from-a-recording)); the Python
  package never re-implements a join.
- **Nothing on the vehicle needs Python.**

## The `hangar` command

One static binary (musl), with subcommands:

| Command | |
|---|---|
| `hangar top` | Live dashboard, for a terminal or SSH: modules (state, restarts, status line), topics (rate, age, writer, stale flags), the link, the safety state, the mux mode, the recorder, the policies (running model, load progress). On the topside it shows both machines ([below](#both-machines)). `--once` prints one frame for scripts |
| `hangar top --sim vehicle\|topside` | The same dashboard attached to one side of a running simulation, in its namespace and topic directory ([14-simulation](14-simulation.md#running-it)) |
| `hangar check` | The pre-dive check ([below](#hangar-check)) |
| `hangar show <module>` | Its wiring as handed to it, the implicit `.req`, `.rep` and `.params` topics and subscriptions included, its state, last exit, status line, `Neutral` command, parameters |
| `hangar topics` | Every topic: type, rate declared and measured, age, writer, slots, flags; for a mirrored topic, the sender's boot id and the messages lost and skipped |
| `hangar echo <topic>` | Messages decoded from the schema in the topic's header; `--rate`, `--fields`, `--json`. On a `.req` or `.rep` topic, the envelope and the body decoded by the body's type hash |
| `hangar hz <topic>` | Measured rate and jitter |
| `hangar call <module>.<method> k=v …` | A call as `admin:<user>` ([below](#hangar-call)); the request built from the type's schema |
| `hangar param get\|set\|load\|reset` | Parameters ([05-parameters](05-parameters.md#the-hangar-param-command)) |
| `hangar start\|stop\|restart <module>` | |
| `hangar clear-failsafe` | Ask the manager to leave system failsafe; refused, naming the module, while a module given up with `OnGiveUp=failsafe` is still `failed` ([03-manager](03-manager.md#system-state-and-the-managers-topics)) |
| `hangar reload` | Read the manifests again ([02-manifest](02-manifest.md#reloading)) |
| `hangar restart-manager` | Re-execute the manager in place, modules kept ([03-manager](03-manager.md#restarting-the-manager)) |
| `hangar run <module>` | A module in the foreground, for development: its wiring from its manifest, a private topic directory ([01-topics](01-topics.md#writing)) |
| `hangar graph` | The wiring graph in DOT: modules, topics, the request and reply topics of calls, provenance links (`Inputs` and `Sources`), with live rates and states |
| `hangar ardusub-setup` | Set ArduSub's parameters from `/etc/hangar/ardusub.params` before a dive ([below](#hangar-ardusub-setup), [10-mavlink](10-mavlink.md#ardusubs-parameters)) |
| `hangar upgrade-vehicle <tarball>` | Upgrade the vehicle from the topside over the tether, with no shell on the vehicle: send the release, call `installer.apply`, confirm both sides match ([13-deployment](13-deployment.md#upgrading-the-vehicle-over-the-tether)) |
| `hangar sim up\|down [...]` | The whole system on one computer: two managers in two network namespaces, the simulators, a virtual tether with loss, delay and rate from the command line ([14-simulation](14-simulation.md#running-it)) |
| `hangar dataset build\|check\|pairs\|loop-latency` | Datasets from recordings, with the pairer's code ([below](#datasets-and-analysis)) |
| `hangar msg gen` | `.msg` files to the language's types and to Python dtypes, at build time |
| `hangar bench` | The targets of [00-overview](00-overview.md#targets), measured, as a report ([below](#the-benchmark)) |

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

## `hangar call`

`hangar call <module>.<method> k=v …` is one request on the control socket
([03-manager](03-manager.md#control-socket)):

1. the command builds the request from the method's request type, as the server's
   wiring on `hangar.<role>.modules` declares it, and sends `call` to the manager with
   the method, the request and a timeout (`--timeout`, 500 ms by default);
2. the manager refuses it from a process outside `hangar-admin`, and when no module
   serves the method on either side (`HG_E_NOROUTE`); otherwise it publishes the
   request on `hangar.<role>.admin.req` with `caller` `admin:<user>`, the deadline set
   from the timeout, and answers with the `call_id`;
3. the command reads `<module>.rep` with `hg_next` from its newest message on,
   keeping the reply whose `call_id` and `caller_id` are its own, prints the reply
   decoded from its type, or the status and `reason`, and exits 0 on `HG_OK`, 1 on
   any other status; a reply that does not arrive within the timeout is
   `HG_E_TIMEOUT`, and the server's `.rep` topic, recorded, says what happened.

A module of the other side is reached the same way: the topside's manager publishes on
`hangar.topside.admin.req`, the bridge mirrors it down and the server's `.rep` up where
`[Calls] Remote=` names the method ([08-bridge](08-bridge.md#calls-across-the-tether)),
and the server sees `topside:admin:<user>`. `hangar param set` is `hangar call
<module>.set_params` with the struct filled in ([05-parameters](05-parameters.md#setting-parameters)).
The command is not a module and has no `.req` topic of its own; nothing it writes
reaches any server except through the manager.

## `hangar check`

One line per check, `OK`, `WARN` or `FAIL`, naming what is wrong; exit status 0
healthy, 1 degraded, 2 down, as `hellion check`. Run on the topside, it checks both
machines ([Both machines](#both-machines)); its vehicle checks that need a call
(`safety.check`) go through the topside's manager and arrive as
`topside:admin:<user>` ([`hangar call`](#hangar-call)).

| Check | Fails (2) when | Warns (1) when |
|---|---|---|
| Manager | not running; `hangar.<role>.system` is `failsafe` | its heartbeat is late; the system is `degraded` |
| Modules | one is `failed` or `refused` | one has restarted in the last 10 minutes |
| Topics | a rated topic is stale | a topic is flagged as computed from stale inputs (a policy with no running model is reported under Policies); a subscription is absent; a `[Call]` names a method no module serves on either side |
| Link | the bridge is down, `ambiguous` or `refused`; the two sides run different releases or `msg/` trees, so nothing is mirrored (`config_match` false, the two versions named; [13-deployment](13-deployment.md#version-lock-step-across-the-tether)); a module or topic name, or a topic id, exists on both machines, so its mirror is refused | the two configurations differ; clock uncertainty over 5 ms; datagram loss over 1% (`loss_up`, `loss_down`, counted from `packet_seq`) |
| Safety | the arming checklist would fail (`safety.check`, a dry run of `arm`) | the vehicle is `arming` or `armed` |
| Autopilot | ArduSub has sent no message for 3 s; its mode is not MANUAL (`ardusub.state`); a parameter of `/etc/hangar/ardusub.params` differs from the value ArduSub reports, or ArduSub does not know it (`ardusub.param`; [`hangar ardusub-setup`](#hangar-ardusub-setup)); the signing key `/etc/hangar/mavlink.key` is missing, or readable by anyone outside group `hg-autopilot` ([10-mavlink](10-mavlink.md#signing)) | |
| Actuators | an actuator has no `Neutral` command, or a `Neutral` command (an actuator's, or a `Legacy` module's `[Module] Neutral`) is not executable | the hardware watchdog is not armed |
| Recorder | the black box is off; its `Dir` is not group `hangar-rec` with mode 2770 | free space under twice `MinFree`; the black box covers a rated topic of class `high` or `normal` for less than `BlackBox` (from `recorder.status`) |
| Policies | | a policy is in `residual:<name>` mode; a policy has no running model |
| Configuration | a module named by a `[Calls] Remote=` entry is enabled on both machines (found at the handshake); a policy module's name is not `policy_<Name>` of its spec | a manifest, dataset spec, policy spec, `bridge.conf`, `recorder.conf`, `pairer.conf` or `ardusub.params` changed on disk since the last reload, start or setup |

The Autopilot line reads ArduSub's state and parameters as the vehicle's `mavlink`
module reports them (`ardusub.state`, `ardusub.param`, which carries every parameter
the module read at start and each change since); it applies while ArduSub drives the
thrusters, held in MANUAL ([10-mavlink](10-mavlink.md#the-vehicles-mavlink-module)).

## `hangar ardusub-setup`

ArduSub's parameters are set once before a dive, from a file checked into the
vehicle's configuration, never from QGroundControl, which is not connected to ArduSub
([10-mavlink](10-mavlink.md)). `/etc/hangar/ardusub.params` is text, one `NAME = value`
per line, with the comment rules of a manifest ([02-manifest](02-manifest.md#syntax)),
and holds everything hangar relies on:

| Parameters | Value | |
|---|---|---|
| `JS_GAIN_DEFAULT`, `JS_GAIN_MIN`, `JS_GAIN_MAX`, `JS_THR_GAIN` | 1 | the pilot gain pinned, so a `wrench` axis means the same thrust at every moment ([07-safety](07-safety.md#the-mux)) |
| `BTNn_FUNCTION`, `BTNn_SFUNCTION`, n = 0 to 31 | 0 (none) | no button does anything inside ArduSub; the topside module handles buttons ([10-mavlink](10-mavlink.md#buttons)) |
| `FS_GCS_ENABLE`, `FS_LEAK_ENABLE`, `FS_BATT_ENABLE`, and the other `FS_*` | as required | ArduSub's own failsafes, the backstop behind hangar's ([07-safety](07-safety.md#failsafe)) |
| `SYSID_MYGCS` (`MAV_GCS_SYSID` in later versions) | the id the vehicle's `mavlink` module sends under | ArduSub takes pilot input from that id alone |
| `MAVn_OPTIONS`, the mode and arming parameters the deployed build needs | as required | |

`hangar ardusub-setup` is one call, `mavlink.setup`, as `admin:<user>` (or
`topside:admin:<user>` from the topside), to the vehicle's `mavlink` module, the only
MAVLink peer ArduSub has. The module refuses it unless `safety.state` is `disarmed` and
ArduSub is disarmed; otherwise it answers `accepted` and, beside its loop, sends a
`PARAM_SET` for each line of the file, waits for ArduSub's `PARAM_VALUE`, and publishes
each on `ardusub.param`. The command reads `ardusub.param` until every name in the file
has been reported, or 30 s have passed, and prints each parameter whose value still
differs or that ArduSub does not know; exit status as `hangar check`. `hangar check`
compares the same file with the same topic, so a parameter changed on ArduSub by any
other means shows before the dive.

## Datasets and analysis

`hangar dataset build <spec> <session>` runs the pairer's join code over a session's
files, so its rows are the live rows, `boot_id`, `reset` and `masked` included, except
where the build set a row's `late` flag: a message that would have been joined, by its
stamp, was published after the live row's `emitted_ns`; and where one row alone marks
a part `missing`: gone from its ring before the live pairer read it, or lost by the
recorder ([06-recorder](06-recorder.md#offline-the-same-rows-from-a-recording)).

`hangar dataset check <spec> [<policy spec>] [<session>]` checks a spec before it is
used, one line per check, `OK`, `WARN` or `FAIL`, with the exit status of
`hangar check`. Without a policy spec it says whether the spec is usable by a policy
at all, and names the `input` joins that would have to wait
([09-learning](09-learning.md#features-at-run-time)).

| Check | Fails (2) when | Warns (1) when |
|---|---|---|
| Spec | it does not parse; a field, or a `provenance` join, does not match the manifests; `Rate=rate:<Hz>` on an event-topic anchor | |
| Policy | with a policy spec: the spec is not the policy's `Dataset`; an `input` join has `offset + Lateness` above 0; for a recurrent policy (an `lstm` op in its current weights), `Filter` is not `mask`, `Missing` is not `flag`, or `Rate` is not `every:1` | |
| Session | with a session: a part's topic was recorded `meta`, `every:N`, `rate:<Hz>` or `off` in it, so the spec cannot be built from that session | |

A warning on a wrench label joined without a `mux.state` part comes with per-axis
label masks ([09-learning](09-learning.md#later)); the version 1 example's labels,
`thr.out` (what the thrusters were sent, whoever drove each axis) and the controller's
own `ctl.depth_hold`, need none.

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
the publish on a module's `.req` topic (the seqlock write and the futex wake), which
`hangar.learn` needs to make calls as a module ([04-calls](04-calls.md#the-two-topics)).

| Module | |
|---|---|
| `hangar.topics` | Read topics: newest, in order, wait; `dtype_from_schema` |
| `hangar.recordings` | Sessions and `.hgr` files as `numpy.memmap` |
| `hangar.datasets` | Dataset topics and files; rows with their `boot_id`, `emitted_ns` and their `reset`, `masked` and `late` flags ([06-recorder](06-recorder.md#the-row)) |
| `hangar.learn` | Stateful streaming for recurrent models (`carry`), replay, export, deploy, revert, watch; run as the topside module `trainer` ([09-learning](09-learning.md#training)) |
| `hangar.export` | MCAP (for Foxglove) and Parquet |
| `hangar.report` | Loop latency, requests and replies paired by `call_id`, gaps, plots |

Reading needs no identity: topics and recordings are files, readable by group
`hangar`. Calls do. The trainer is the topside module `trainer`, started by the
topside's manager, whose manifest declares `[Call bridge.send]` and the
`load_weights`, `revert` and `runtime_info` calls of its policy
([09-learning](09-learning.md#training)). Inside it, `hangar.learn` does what `hg_init`
does: it makes the process non-dumpable, reads the wiring from `HANGAR_WIRING`, fetches
its module page from the manager over the control socket, and opens its request topic
`trainer.req` through the manager, which hands the only writable descriptor to the
module ([03-manager](03-manager.md#starting-a-module)). Its calls are then messages on
`trainer.req`, which the bridge mirrors down, so the vehicle's policies see them as
`topside:trainer` ([01-topics](01-topics.md#mirrored-topics)); their replies come back
on the mirrored `policy_<name>.rep`. A script run from a shell, outside the manager, is
not `trainer`: it calls through the manager as `admin:<user>` if its user is in
`hangar-admin`, and the vehicle sees it as `topside:admin:<user>`, which passes only
where `Allow` names `topside:admin`; any other user is refused by the control socket.

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
be reproduced by anyone. The report states each target of
[00-overview](00-overview.md#targets) beside the measured value, and, for every line
both systems measure, the factor by which hangar is lighter than ROS 2: that factor is
the pass/fail, not the absolute numbers. In CI the report is compared with the previous
release's own report, and a line more than 10% worse fails.

## Tests

- `hangar check` against staged faults, one for each line of its table, on the vehicle
  and on the topside; run on the topside, a vehicle fault reported through the
  mirrored topics and named `vehicle`, and the vehicle's lines shown stale with the
  link down.
- `hangar check` Autopilot against ArduSub SITL: each parameter of `ardusub.params`
  changed on SITL in turn, and one SITL does not know; SITL put in STABILIZE; the key
  file's group and mode wrong.
- `hangar ardusub-setup` against SITL: every parameter of the file set and read back,
  and the command exits 0; a parameter SITL refuses named, exit status 2; refused
  while `safety.state` is `armed` or SITL is armed; from the topside, allowed as
  `topside:admin:<user>`.
- `hangar dataset check` against specs staged for each line of its table, with and
  without a policy spec, and against a session that recorded a part `meta`; the
  `depth_ff` spec of
  [09-learning](09-learning.md#example-adaptive-depth-hold) and the `depth_hold_clone`
  spec of [06-recorder](06-recorder.md#the-spec) pass with no warning.
- `hangar clear-failsafe` refused while a module given up with `OnGiveUp=failsafe` is
  `failed`, accepted once it is restarted.
- `hangar echo`, `call` and `param` driven by schemas alone, against every message type
  in `msg/`; `hangar echo` of a `.req` and a `.rep` topic decodes the envelope and the
  body.
- `hangar call`: the request appears on `hangar.<role>.admin.req` with `caller`
  `admin:<user>`, written by the manager; the command keeps the reply with its
  `call_id` and ignores the others on `<module>.rep`; allowed where `Allow` names
  `admin`, denied where it does not; a method nobody serves refused at the control
  socket; a server stopped during the call gives `HG_E_TIMEOUT` and exit status 1;
  from the topside, the request on the mirrored `hangar.topside.admin.req` allowed only
  where `Allow` names `topside:admin`. The same for `hangar param set`.
- A process outside `hangar-admin` refused by the control socket for `call`,
  `param-write`, `start`, `stop`, `restart`, `reload`, `reexec` and `clear-failsafe`.
- `hangar dataset build` against the live pairer on a recorded session: rows identical
  wherever the build set no `late` flag and neither side missed a message, apart from
  the build's first `reset` flag; every other differing row carries `late`; `boot_id`,
  `emitted_ns`, `reset` and `masked` equal throughout.
- The Python package's readers against the C writer, on x86-64 and ARM.
- The trainer as a module: its requests on `trainer.req`, written by it alone, reach
  the vehicle on the mirrored topic as `topside:trainer`; the same script run from a
  shell calls through the manager as `admin:<user>`, and a user outside `hangar-admin`
  is refused.
- `hangar bench` and the ROS 2 scripts run in CI on x86-64, and on a Pi before each
  release; the report names each target and the factor against ROS 2, and CI fails on
  a line more than 10% worse than the previous release's.
