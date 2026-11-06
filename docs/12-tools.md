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
  ([03-manager](03-manager.md#control-socket)).
- **One code path for one job.** `hangar dataset build` uses the pairer's own join code,
  so offline rows equal live rows; the Python package never re-implements a join.
- **Nothing on the vehicle needs Python.**

## The `hangar` command

One static binary (musl), with subcommands:

| Command | |
|---|---|
| `hangar top` | Live dashboard, for a terminal or SSH: modules (state, restarts, status line), topics (rate, age, writer, stale flags), the link, the safety state, the mux mode, the recorder, the policies. `--once` prints one frame for scripts |
| `hangar check` | The pre-dive check ([below](#hangar-check)) |
| `hangar show <module>` | Its wiring as handed to it, its state, last exit, status line, `Neutral` command, parameters |
| `hangar topics` | Every topic: type, rate declared and measured, age, writer, slots, flags |
| `hangar echo <topic>` | Messages decoded from the schema in the topic's header; `--rate`, `--fields`, `--json` |
| `hangar hz <topic>` | Measured rate and jitter |
| `hangar call <module>.<method> k=v …` | A call, as `admin:<user>`; the request built from the type's schema |
| `hangar param get\|set\|load\|reset` | Parameters ([05-parameters](05-parameters.md#the-hangar-param-command)) |
| `hangar start\|stop\|restart <module>` | |
| `hangar reload` | Read the manifests again ([02-manifest](02-manifest.md#reloading)) |
| `hangar restart-manager` | Re-execute the manager in place, modules kept ([03-manager](03-manager.md#restarting-the-manager)) |
| `hangar run <module>` | A module in the foreground, for development: its wiring from its manifest, a private topic directory ([01-topics](01-topics.md#writing)) |
| `hangar graph` | The wiring graph in DOT: modules, topics, calls, provenance links, with live rates and states |
| `hangar dataset build\|check\|pairs` | Datasets from recordings, with the pairer's code ([06-recorder](06-recorder.md#offline-the-same-rows-from-a-recording)) |
| `hangar msg gen` | `.msg` files to the language's types and to Python dtypes, at build time |
| `hangar bench` | The budgets of [00-overview](00-overview.md#budgets), measured, as a report ([below](#the-benchmark)) |

`hangar top` follows `hellion top`: plain ANSI, no curses library, four refreshes a
second, readable over a slow SSH session.

## `hangar check`

One line per check, `OK`, `WARN` or `FAIL`, naming what is wrong; exit status 0
healthy, 1 degraded, 2 down, as `hellion check`.

| Check | Fails (2) when | Warns (1) when |
|---|---|---|
| Manager | not running | its heartbeat is late |
| Modules | one is `failed` or `refused` | one has restarted in the last 10 minutes |
| Topics | a rated topic is stale | a topic is flagged as computed from stale inputs; a subscription is absent |
| Link | the bridge is down or `ambiguous` | the two configurations differ; clock uncertainty over 5 ms; loss over 1% |
| Safety | the arming checklist would fail (`safety.check`, a dry run of `arm`) | the vehicle is armed |
| Autopilot | ArduSub is silent or its failsafes are disabled | |
| Actuators | an actuator has no `Neutral` command, or it is not executable | the hardware watchdog is not armed |
| Recorder | the black box is off | free space under twice `MinFree` |
| Policies | | a policy is in `policy:` or `residual:` mode |
| Configuration | | a manifest or policy spec changed on disk since the last reload |

## Datasets and analysis

`hangar dataset build` runs the pairer's join code over a session's files, so its rows
are byte-identical to the live rows ([06-recorder](06-recorder.md)). Everything else
done with recordings and datasets afterwards is analysis, and lives in the Python
package.

## The Python package

`hangar`, for the topside and for training; installed as a wheel with one small compiled
helper (the seqlock read on ARM, [01-topics](01-topics.md#reading-from-python)).

| Module | |
|---|---|
| `hangar.topics` | Read topics: newest, in order, wait; `dtype_from_schema` |
| `hangar.recordings` | Sessions and `.hgr` files as `numpy.memmap` |
| `hangar.datasets` | Dataset topics and files; runs of rows for recurrent models |
| `hangar.learn` | Streaming, export, deploy, revert, watch ([09-learning](09-learning.md#training)) |
| `hangar.export` | MCAP (for Foxglove) and Parquet |
| `hangar.report` | Loop latency, call records, gaps, plots |

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

- `hangar check` against staged faults, one for each line of its table.
- `hangar echo`, `call` and `param` driven by schemas alone, against every message type
  in `msg/`.
- `hangar dataset build` equal to the live pairer on a recorded session.
- The Python package's readers against the C writer, on x86-64 and ARM.
- `hangar bench` and the ROS 2 scripts run in CI on x86-64, and on a Pi before each
  release.
