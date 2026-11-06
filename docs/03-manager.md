# Manager

`hangard` starts the modules, watches them, restarts them when they fail, and keeps
the system's state. It is the control plane only: it moves no data, sits in no call,
and modules keep running and exchanging data while it restarts. It is involved in the
data plane at exactly one moment: when a module opens a topic for writing, the manager
creates the file and hands the module the descriptor.

## Running it

`hangard` is one systemd service. systemd restarts the manager; the manager restarts
the modules.

```ini
# /etc/systemd/system/hangard.service
[Unit]
Description=hangar module manager
# Never give up restarting the manager.
StartLimitIntervalSec=0

[Service]
Type=notify
ExecStart=/usr/bin/hangard
# A planned restart or an upgrade: re-execute in place, keeping every module.
ExecReload=/usr/bin/hangar restart-manager
# Listening call sockets. With Restart=always, the store is kept across a crash
# of the manager; it is emptied when the service is stopped.
FileDescriptorStoreMax=256
# Modules outlive a manager crash and are adopted again.
KillMode=process
# The manager's own cgroup subtree, for one cgroup per module (optional).
Delegate=yes
CapabilityBoundingSet=CAP_SETUID CAP_SETGID CAP_SETPCAP CAP_SYS_NICE CAP_IPC_LOCK CAP_KILL CAP_CHOWN CAP_FOWNER
# Inherited by modules, which lower it unless LockMemory=yes.
LimitMEMLOCK=infinity
# journald limits the whole service as one: every module's lines together.
LogRateLimitIntervalSec=30s
LogRateLimitBurst=20000
AmbientCapabilities=
NoNewPrivileges=no
RuntimeDirectory=hangar
RuntimeDirectoryPreserve=yes
Restart=always
RestartSec=1

[Install]
WantedBy=multi-user.target
```

It runs as root with that capability set and no other: it can switch to module users,
give their files to them, set real-time scheduling and memory locking, and signal
modules, but cannot load kernel modules, mount, trace processes, or change the network. On a system without systemd any supervisor that
restarts it will do (BusyBox init `respawn`, runit); the per-module cgroups are then
not available and process groups are used ([Stopping](#stopping-a-module)).

Its own settings, `/etc/hangar/hangard.conf`:

| Key | Default | |
|---|---|---|
| `Role` | — (required) | `vehicle` or `topside`. Names the manager's own topics and the bridge's ([System state](#system-state-and-the-managers-topics)) |
| `ModuleDir` | `/etc/hangar/modules` | Enabled manifests |
| `MsgDir` | `/usr/share/hangar/msg` | Message types, for checking declared types |
| `TopicDir` | `/dev/shm/hangar` | |
| `BridgeConf` | `/etc/hangar/bridge.conf` | The bridge's configuration, read when a module has `[Bridge]` |
| `DatasetDir` | `/etc/hangar/datasets` | Dataset specs, for `Type=dataset:<name>` outputs |
| `RecordingDir` | `/var/lib/hangar/recordings` | Created at start for the recorder's sessions ([Start-up](#start-up)) |
| `HealthTick` | `100ms` | How often staleness is checked |
| `MaxModules` | 64 | |
| `LogFallbackDir` | `/run/hangar/log` | Module logs when journald is not running |
| `LogFallbackCap` | 256 KB | Per module |

Besides the manifests and the message types, the manager reads three kinds of file,
each only for the modules that need it:

| File | Read for | |
|---|---|---|
| `BridgeConf` | the module with `[Bridge]` | the topics it receives, with their `ExpectRate` and `StaleAfter`, and `[Calls] Remote=` ([08-bridge](08-bridge.md#configuration)) |
| `DatasetDir/<name>.dataset` | an output declared `Type=dataset:<name>` | the row schema, which the manager generates ([06-recorder](06-recorder.md#live-the-pairer)) |
| the policy spec named in `Exec` | a module whose program is `/usr/lib/hangar/hangar-policy` | its `Name=`: the module must be `policy_<Name>`, or it is refused ([09-learning](09-learning.md#policy-modules)) |

### Restarting the manager

Three different things, kept apart:

| | How | Modules |
|---|---|---|
| **Planned restart, upgrade** | `hangar restart-manager`, or `systemctl reload hangard` | keep running. The manager re-executes its (possibly new) binary in place, passing its state and every descriptor it holds (pidfds, call sockets, the control socket) to the new image: the modules stay its children, nothing is adopted, nothing is lost |
| **Crash** | systemd restarts it | keep running, and are adopted ([Adoption](#adoption)); call sockets come back from systemd's descriptor store |
| **Stop** | `systemctl stop` or `restart`, shutdown of the computer: `SIGTERM` | are stopped, those with a `Neutral` command first ([Shutdown](#shutdown)) |

`systemctl restart hangard` therefore stops the vehicle's modules; installers and
upgrades use `reload`, never `restart`. `systemctl reload` runs `hangar
restart-manager` as root, which the control socket accepts as `hangar-admin`
([Control socket](#control-socket)).

## Event loop

One thread. It waits in `epoll` on:

| Descriptor | For |
|---|---|
| `signalfd` | `SIGTERM`/`SIGINT` (shut down the vehicle's modules: [Shutdown](#shutdown)), `SIGHUP` (reload the manifests), `SIGCHLD` (ignored: pidfds carry exits) |
| one `pidfd` per module | readable when the module exits |
| one `timerfd` | the earliest of: back-off delays, stop timeouts, the health tick, and the once-a-second publication of its own topics |
| the control socket | requests from modules and from the `hangar` command |

The health tick runs only while some module has a rated topic or a watchdog, or is in
its start grace; with none, the manager wakes once a second, to publish
`hangar.<role>.system` and `hangar.<role>.modules`, and for nothing else. Staleness is
therefore detected to within one tick (100 ms). Deadlines that need to be tighter, such
as an actuator's command deadman, are enforced inside the module that owns them
([07-safety](07-safety.md)), not by the manager.

## Start-up

1. Read the registry, `/run/hangar/registry`, and **neutralize first**: for every
   entry in a running state whose process is gone (no such PID, or its start time
   differs) and that has a Neutral record ([Neutral commands](#neutral-commands)), run
   its `Neutral` command, all of them in parallel, each with a 1 s limit, before
   anything else. A command that fails or cannot be found puts the system in failsafe.
   The log says that the module died at an unknown time after the registry was last
   written, and gives that time.
2. Create `TopicDir` (owner the manager, group `hangar`, mode 0750), its directory
   word `.dir` if missing, `/run/hangar/{modules,call,log,neutral}`, and `RecordingDir`
   if missing (owner root, group `hangar-rec`, mode 2770;
   [06-recorder](06-recorder.md#sessions)).
3. **Adopt** the modules still running from before ([Adoption](#adoption)), and take
   back the system state from the registry. For every module in it that is *not*
   running any more, set the dead-writer flag on its topics, wake their readers, and
   kill whatever is left in its cgroup.
4. Load the manifests, the message types and the files above; check the graph
   ([02-manifest](02-manifest.md#how-the-manager-uses-manifests)). Create each
   module's wake word in `/run/hangar/call/` if it is missing. For each module that
   serves calls, and for each stand-in the bridge serves ([Wiring](#wiring)), take back
   its listening socket from systemd's descriptor store (after a crash), or create it
   and hand a copy to the store (`FDSTORE=1`, `FDNAME=<module>`), so that the socket
   outlives any crash of the manager ([04-calls](04-calls.md)). A stored entry whose
   name is no longer an enabled module's or a stand-in's is closed and removed from the
   store (`FDSTOREREMOVE=1`, `FDNAME=<name>`); so is the socket of a module that a
   reload removes. Without systemd there is no store: after a crash of the manager, an
   adopted module's socket lasts only until that module's next restart, and calls
   queued at that moment are lost.
5. Create and publish its own topics, `hangar.<role>.system` and
   `hangar.<role>.modules`, so that no module can take their names first.
6. Start every accepted module that is not already running, all at once.

## Starting a module

From electric-eye's `child_proc.c`, every one of these a way it has gone wrong before:

- The program is resolved before the fork; an exec failure in the child is reported
  back over a close-on-exec pipe, so "not found" is one clear error.
- `clone3` with `CLONE_PIDFD`: the pidfd exists from the first instant, with no window
  in which the PID could be reused.
- Only the descriptors listed below cross into the child; signal dispositions and the
  signal mask are reset.
- The child is the leader of its own process group, and, when the manager has a
  delegated cgroup, alone in its own cgroup `hangar/<name>`.
- No `PDEATHSIG`: the module does not die with the manager.

Then, in the child, in this order, because switching user drops the capabilities the
earlier steps need:

1. wait on a pipe until the manager has written the module's registry entry and, if it
   has a `Neutral` command, its Neutral record, and exit if the pipe closes first (the
   manager died): no module ever runs without an entry;
2. move into its cgroup;
3. resource limits: `RLIMIT_MEMLOCK` left unlimited if `LockMemory=yes`, lowered to the
   default otherwise. The library calls `mlockall` at `hg_init`; a `Legacy` module,
   which has no library, gets only the raised limit and must lock its own memory
   (ArduPilot's Linux HAL does);
4. scheduling policy, priority, nice and CPU affinity;
5. `/var/lib/hangar/<name>`, created if missing and given to the module's user, as
   working directory;
6. stdout and stderr connected to journald ([Logs](#logs)), so that journald sees the
   module's own process and cgroup;
7. for `Capabilities=` in the manifest: `PR_SET_KEEPCAPS` 1, and the listed
   capabilities added to the inheritable set (`capset`; the permitted set already holds
   them), so that both sets keep them across the user switch;
8. supplementary groups, group, and user (`setgroups`, `setresgid`, `setresuid`);
9. each listed capability raised as ambient (`PR_CAP_AMBIENT_RAISE`). This comes after
   the user switch because a switch from uid 0 to another uid clears the ambient set,
   with or without `PR_SET_KEEPCAPS`; the raise needs the capability in the permitted
   and inheritable sets, which step 7 kept;
10. exec. The program's permitted and effective sets are then its ambient set: the
    listed capabilities and nothing else.

What the module inherits:

| Descriptor | Environment | |
|---|---|---|
| wiring | `HANGAR_WIRING=<fd>` | A sealed `memfd`: the module's validated wiring ([Wiring](#wiring)) |
| stdout, stderr | | journald streams, or the fallback log file ([Logs](#logs)) |
| stdin | | `/dev/null` |

`HANGAR_MODULE=<name>`, `HANGAR_CONTROL=/run/hangar/control` and
`HANGAR_TOPIC_DIR=<TopicDir>` are set too.

**The module page and the call socket are not inherited.** A process is dumpable again
after exec, and until `hg_init` runs, another process of the same user could copy a
descriptor out of it (`pidfd_getfd`, `/proc/<pid>/fd`). `hg_init` therefore first
makes the module non-dumpable (`PR_SET_DUMPABLE` 0), then sends `attach` on the
control socket. The manager identifies the module by `SO_PEERCRED` (the PID must be
the main process of a module instance it started or adopted), refuses a process that
is being traced (`TracerPid` in `/proc/<pid>/status` is not 0), and replies with, by
`SCM_RIGHTS`: the writable module page, the listening call socket if the module serves
calls, and, for the bridge, one stand-in socket per remote module. A module that
attaches while the manager is restarting waits until it answers. A `Legacy` module
never attaches and has neither. The wiring memfd is still inherited: it is sealed, and
holds nothing another module cannot read in the manifests
([01-topics](01-topics.md#permissions)).

User and group names are resolved from `/etc/passwd` and `/etc/group`, which the
manager reads itself: the release binaries are static (musl), with no name service
modules.

## Wiring

Text, one line per item, so it can be read in a debugger or by `hangar show`:

```
hangar-wiring 1
module depth_hold
sub nav.depth type=depth hash=9f3a61c20b7e4d15 rate=50
sub nav.imu type=imu hash=0c41e2aa93f81d70 rate=50
pub ctl.depth_hold type=wrench hash=77d0b3a1c5e29f04 slots=16 rate=50 stale_after=60ms inputs=nav.depth,nav.imu
pub depth_hold.calls type=calls:depth_hold hash=5c0e97a2d4b81f63 slots=64 implicit
serve reset req=empty hash=… size=8 rep=empty hash=… size=8 deadline=100ms allow=topside:pilot_console
call safety.disarm req=empty hash=… size=8 rep=empty hash=… size=8 remote=0
```

| Line | |
|---|---|
| `sub <topic>` | A subscription. At most 126 per module, what one wait can hold ([01-topics](01-topics.md#reading)) |
| `subglob <pattern>` | A glob subscription (`[Subscribe nav.*]`): any topic matching it may be opened, including topics created after start. Not counted toward the 126: the module reads its matches on a timer instead of waiting on them |
| `pub <topic>` | An output, with `inputs=` (referenced by every message) and `sources=` (referenced per message, with `hg_publish_with`; [01-topics](01-topics.md#provenance)) |
| `serve <method>` | A method the module serves, with its types, deadline and `Allow` tokens ([04-calls](04-calls.md#declaring-calls)) |
| `call <module>.<method>` | A method it calls; `remote=1` when the module is on the other side of the tether |
| `mirror <topic or glob> type=any rate=… stale_after=…` | The bridge only: topics its configuration receives, with the receiving file's rate and staleness from `[Mirror] ExpectRate` and `StaleAfter` (rate 0, an event topic, without them) |
| `standin <module>` | The bridge only: a module of the other side whose calls it serves here |

`hg_init()` reads it once. The library refuses to open a topic not listed or make a
call not listed, and takes slots, rates, inputs, call types, sizes, deadlines and
`Allow` lists from here, never from the module's code. **Implicit outputs** are those
the manager adds itself: `<module>.calls` for a module that serves calls (its type
generated from the largest request and reply,
[04-calls](04-calls.md#the-call-record)) and `<module>.params` for one with
`[Params]`. The bridge's are `bridge.<role>.link` (10 Hz, its one rated output),
`bridge.<role>.files` (event) and the method `send`
([08-bridge](08-bridge.md#the-bridge-module)). They are checked, created and flagged
like any declared output. An output
declared `Type=dataset:<name>` also gets a generated type: the row schema, which the
manager builds from `DatasetDir/<name>.dataset` and the types of the topics it joins
([06-recorder](06-recorder.md#the-row)).

**Remote calls and stand-ins.** Module and topic names are unique across the vehicle
and the topside together ([08-bridge](08-bridge.md#calls-across-the-tether)). A
declared call is remote when it matches the bridge's `[Calls] Remote=` and its module
is not enabled on this machine. For each module named by a remote call, the manager
creates a stand-in: the listening socket `/run/hangar/call/<module>` and its wake word,
stored like any module's socket and handed to the bridge at `attach`, with a `standin`
line in the bridge's wiring. A `Remote=` entry that matches a module enabled on this
machine is refused as remote here: no stand-in is made for it, and the call stays
local. With the same `bridge.conf` on both machines this is normal (`safety.*` is
remote on the topside only), so it is not reported. A module enabled on both machines
is found at the handshake, from the module lists the bridges exchange, and reported by
`hangar check` ([08-bridge](08-bridge.md#calls-across-the-tether)).

## Opening topics

When the module calls `hg_pub_open`, the library sends one request on the control
socket: the topic name, the type hash, the schema text and the payload size (the size
matters for frame topics, whose size the module decides). The bridge also sends the
sender's boot id and the sequence number of the first message it will publish. The
manager:

1. identifies the module from the socket's peer credentials (`SO_PEERCRED`: the PID
   must be a running module's main process);
2. checks the topic is one of that module's declared outputs, that the schema hashes
   to the type the manifest declares or the manager generated (`frame` accepts any
   frame type), and that no other process holds it open for writing. For the bridge,
   a topic matching one of its `mirror` lines is accepted too, with any type, provided
   no local module declares it and its topic id collides with no local topic's;
3. creates the topic file ([01-topics](01-topics.md#writing)), continuing the
   sequence numbers of any file it replaces. A mirrored topic continues them only
   within one boot of the sender: when there is no file, or the existing file's
   `origin_boot` differs from the sender's boot id, the new file's `first_seq` is the
   bridge's first sequence number − 1 and its `origin_boot` the sender's boot id, and
   readers of the old file start a new epoch ([01-topics](01-topics.md#header)). A
   local topic's `origin_boot` is all zero. The rate and `StaleAfter` of a mirrored
   topic are those of its `mirror` line;
4. replies with the descriptor (`SCM_RIGHTS`), or with the reason it refused.

The manager creates a topic's wake file before it renames the topic file into place,
and increments and wakes the directory word only after the rename. The directory word
`.dir`, every topic's wake file and every module's wake word are created once per boot
(`O_CREAT`, never by renaming, never truncated) and are never replaced or deleted
during the boot: a restarted or re-executed manager opens the existing files, and a
replaced topic keeps its wake file, so no sleeper is left waiting on an old inode.

Subscribers never ask the manager: they open topic files read-only by path. A module
that opens its outputs while the manager is restarting waits in `hg_pub_open` until the
manager answers.

## Module page

`/run/hangar/modules/<name>`, 4 KB, created by the manager (group `hangar`, mode 0640);
the module holds the only writable descriptor, which it gets at `attach`. It is how a
module tells the manager things without a system call:

| Offset | Type | Field | Written by | |
|---|---|---|---|---|
| 0 | char[8] | `magic` | manager | `"HGPAGE01"` |
| 8 | u64 | `instance` | manager | the start time of the instance this page belongs to |
| 16 | u64 | `ready_instance` | library | set to `instance` when every rated output has published once, or by `hg_ready()` |
| 24 | u64 | `alive_ns` | library | `hg_alive()`: the watchdog (atomic) |
| 64 | char[128] | `status` | module | `hg_status("waiting for camera")`: one line, shown by `hangar top` |

Before each start the manager writes the new `instance` and clears the rest, so a
restarted module is not taken as ready on the strength of its predecessor: it is ready
only when `ready_instance` equals `instance`. The manager reads the page on the health
tick. Being a file rather than a descriptor the manager holds, it survives a manager
restart. Only the module and the manager can write it, so no other module can mark it
ready or alive.

Every module also has a **wake word**, `/run/hangar/call/<name>.wake` (4 bytes, group
`hangar`, mode 0660). A client increments a server's to wake it for a request; a
server increments a client's after sending it a reply, so that a module waiting in
`hg_wait` learns of the reply to an `hg_call_start` ([04-calls](04-calls.md#protocol)).
It is a separate file because every module must be able to write it, and a spurious
wake costs nothing.

## States

| State | Meaning | Next |
|---|---|---|
| `starting` | spawned, not yet ready | `ready`; `backoff` on exit; `unhealthy` if not ready after `StartGrace` |
| `ready` | published on every rated output (event outputs, `.calls`, `.params` and the bridge's received topics excluded), or `hg_ready()` | `unhealthy`; `backoff` on exit |
| `unhealthy` | a rated topic stale for `StaleGrace`, or the watchdog missed. A message flagged as computed from stale inputs is not stale: a policy with no model, publishing flagged neutral outputs, stays `ready` | stopped, then `backoff` |
| `backoff` | waiting to restart | `starting` |
| `failed` | gave up: more failures than `Burst` | stays down until `hangar restart <name>` or a reload |
| `stopped` | stopped on purpose, or `Policy` says not to restart | |
| `refused` | its manifest or its place in the graph was refused | |

**The bridge's received topics** never count toward its readiness or its health. They
keep their rate and `StaleAfter` for their readers, but a tether outage makes them
stale, and must not restart the bridge: the bridge is judged by its own watchdog and
its own rated output, `bridge.<role>.link`, which it publishes whether the link is up
or not ([08-bridge](08-bridge.md#link-status)).

**Restarting.** The delay starts at the first value of `Backoff` and doubles on each
failure up to the last. After `ResetAfter` in `ready`, the delay and the failure count
start over. A failure that brings the count within the `Burst` period above its limit
moves the module to `failed` and applies `OnGiveUp`: `degrade` marks the system
degraded; `failsafe` puts it in failsafe
([System state](#system-state-and-the-managers-topics)).

**Exit status 78** (`EX_CONFIG`) means the module refused its own configuration (its
saved parameters, [05-parameters](05-parameters.md#saved-values)): restarting would
not help, so the module goes straight to `failed` with its status line as the reason,
and `OnGiveUp` applies.

**A restart the module asks for** (a `@restart` parameter changed) is a stop and a
start that does not count as a failure. The library repeats `restart-self` until the
manager answers, so a request made while the manager restarts is not lost.

**When a module exits**, the manager reads its status without reaping it
(`waitid(P_PIDFD, …, WEXITED | WNOWAIT)`), runs its `Neutral` command if it has one
([Neutral commands](#neutral-commands)), sets the dead-writer flag on each of its
topics and wakes their readers, kills anything left in its cgroup or process group
(the unreaped leader keeps the group's id from being reused), then reaps it, logs the
exit code or signal, and schedules the restart. For an adopted module, which is not the
manager's child and has no such guarantee, only its cgroup is killed; its `Neutral`
command runs all the same.

### Neutral commands

A module that drives hardware has a `Neutral` command: every actuator module
(`[Actuator] Neutral`, [07-safety](07-safety.md#the-actuator-gate)), and a `Legacy`
module that drives hardware (`[Module] Neutral`, such as ArduSub in phase 1:
[10-mavlink](10-mavlink.md#ardusub-as-a-module)). A crashed module cannot write neutral
itself, and some output hardware keeps its last command.

When such a module exits, for any reason, the manager first runs its `Neutral`
command, as the module's user and groups, with a 1 s limit, before anything else; if
that command fails or is missing, the system enters failsafe. These modules also
default to `StopTimeout=500ms`.

The command run is the one recorded when the module was started, not the manifest as
it is on disk now. At each start the manager writes a **Neutral record**,
`/run/hangar/neutral/<name>` (owner root, mode 0600): the command, the user id, the
group id and the supplementary group ids. It is written before the child is released
(step 1 above), so a module never runs without one, and it lets a manager that
restarted neutralize a module that died while no manager was running
([Start-up](#start-up), step 1).

## Stopping a module

`SIGTERM` to the module's process group; after `StopTimeout`, `SIGKILL` to its cgroup
(`cgroup.kill`) or, without one, to its process group. The leader is not reaped before
the group is signalled, so its process group id cannot have been reused. A module's
children that left its process group (`setsid`) are only caught with cgroups.

## Shutdown

Modules with a `Neutral` command first: actuator modules set their outputs to neutral
on `SIGTERM` ([07-safety](07-safety.md)), and each one's `Neutral` command runs as it
exits. Once they have exited or been killed, every other module in parallel. Then the
manager removes the registry and the Neutral records, and exits.

## Adoption

A manager that crashes or is restarted leaves its modules running (`KillMode=process`,
no `PDEATHSIG`). The registry, `/run/hangar/registry`, rewritten atomically
(temporary file and rename) on every state change, holds the system state and, for each
module, its PID, its process start time from `/proc/<pid>/stat`, its state, its
failure count and its current delay.

The registry is readable by group `hangar`: a module serving calls uses it to tell
which module is calling ([04-calls](04-calls.md#callers)). The manager writes a
module's entry as soon as `clone3` returns, before anything else.

The registry is text, one module per line, so that any language can read it:

```
hangar-registry 2
system: failsafe 98231544000000 thrusters given up
depth 1234 98231544 ready 0 0.1
thrusters 1240 98231551 failed 6 10
```

(The `system:` line: the system state, since when in `CLOCK_MONOTONIC` nanoseconds,
and why, to the end of the line. Module lines: name, PID, start time in clock ticks
from `/proc/<pid>/stat`, state, failures, current delay in seconds.)

On start the manager opens a pidfd for each PID in it (`pidfd_open`), reads the start
time again, and adopts the process only if both match; a PID now belonging to another
process is treated as a module that died ([Start-up](#start-up)). An adopted module
keeps its restart history, and the system keeps its state. Its topics, module page and
call socket are files and descriptors the module already holds, and need nothing from
the old manager. What is lost: the exit status of an adopted module, which is not the
new manager's child, is logged as unknown.

## Logs

A module's stdout and stderr are each a stream to journald
(`/run/systemd/journal/stdout`), connected by the child itself after it has moved into
its cgroup and before it switches user, with the module's name as identifier:
`journalctl -t depth`. journald therefore records the module's own PID and cgroup. Its
rate limit, though, is per systemd unit, and every module is in `hangard.service`'s
cgroup: all modules and the manager share one limit, set in the unit at 20 000 lines
in 30 s. A module that floods the journal can get the others' lines dropped for the
rest of that interval; journald logs how many it dropped. Nothing safety depends on is
only in the log: gate reasons are in the module's status line, triggers in
`safety.state`, and refusals in the call records. The manager is not in the path, so
its restart cannot leave a module writing into a closed pipe.

Without journald, both go to `/run/hangar/log/<name>.log`, opened in append mode. A
file the module holds open in append mode cannot be shortened without losing lines,
so the manager only cuts it, to its last `LogFallbackCap`, when it starts the module.

## System state and the manager's topics

| Topic | Rate | Content |
|---|---|---|
| `hangar.<role>.system` | on change, and 1 Hz | `nominal`, `degraded` or `failsafe`, since when, and why |
| `hangar.<role>.modules` | on change, and 1 Hz | per module: name, state, PID, failures, last exit, time in state, status line |

`<role>` is the `Role` of `hangard.conf`: `hangar.vehicle.system` on the vehicle,
`hangar.topside.system` on the topside. Since names are unique across the two machines,
the vehicle's topics keep their names when the bridge mirrors them up. They are
ordinary topics: the recorder keeps them, `hangar top` reads them, and a network can
take the health of the system as an input.

**Failsafe** is entered when a module with `OnGiveUp=failsafe` is given up, or when a
`Neutral` command fails or is missing. It is latched: it ends only on a
`clear-failsafe` request on the control socket, which the safety module makes during
`safety.clear` ([07-safety](07-safety.md#failsafe)) and a member of `hangar-admin` can
make. The request is refused, naming the module, while any module given up with
`OnGiveUp=failsafe` is still `failed`: it is restarted first (`hangar restart <name>`).
The system then becomes `degraded` or `nominal`.

**Degraded** holds while a module given up with `OnGiveUp=degrade` is `failed`, and
ends by itself when none is.

The state and its time are kept in the registry, so a manager that adopts the modules
after a crash, or re-executes, keeps it.

## Control socket

`/run/hangar/control`, `SOCK_SEQPACKET`, group `hangar`, mode 0660. One request per
packet, one reply per request, text.

| Request | From | |
|---|---|---|
| `attach` | the module's main process, at `hg_init` | its module page, its call socket, and the bridge's stand-ins ([Starting a module](#starting-a-module)) |
| `open <topic> …` | the module's main process | [Opening topics](#opening-topics) |
| `restart-self` | the module's main process | a `@restart` parameter changed |
| `param-write <module> …` | `hangar-admin` | parameters of a module that is not running, written and `fsync`ed as its user, by the rules of [05-parameters](05-parameters.md#saved-values) |
| `clear-failsafe` | the `safety` module, `hangar-admin` (`hangar clear-failsafe`) | leave system failsafe ([System state](#system-state-and-the-managers-topics)) |
| `status` | anyone in `hangar` | |
| `start`, `stop`, `restart <name>` | `hangar-admin` | |
| `reload` | `hangar-admin` | as `SIGHUP` |
| `reexec` | `hangar-admin` | re-execute the manager in place ([Restarting the manager](#restarting-the-manager)) |

Callers are identified by `SO_PEERCRED` and their groups by `SO_PEERGROUPS`. The
group `hangar-admin` holds the operators; its members are also in `hangar`, which the
sockets need. A caller with uid 0 counts as `hangar-admin`: `systemctl reload` runs
`hangar restart-manager` as root, outside that group.

## Tests

- Spawning: unresolvable programs, exec failure, descriptors and signals in the
  child; user switching in a user namespace with a mapped range of ids; scheduling,
  memory locking and capabilities as real root (a virtual machine or the Pi), since a
  user namespace cannot grant them.
- Capabilities: a module with `Capabilities=CAP_SYS_NICE` has exactly that capability
  in `CapAmb`, `CapPrm` and `CapEff` of `/proc/<pid>/status` after exec, and can set a
  thread's real-time priority; a module without `Capabilities` has none.
- A manager killed between `clone3` and the registry write: the child never runs.
- Attach: the page and the call socket are not open in the module before `hg_init`;
  `attach` refused from a process that is not a module's main process, and from a
  traced one; a module attaching while the manager re-executes gets its descriptors.
- Restart: back-off doubling, reset after `ResetAfter`, give up after `Burst`, both
  `OnGiveUp` actions.
- System state: failsafe on a give-up and on a failing `Neutral` command;
  `clear-failsafe` refused while the given-up module is `failed`, accepted after it
  is restarted; the state kept across a manager crash and a re-execution.
- Health: a module that stops publishing is restarted; one that is slow to start is
  not, within `StartGrace`; the watchdog. The bridge is not restarted when the topics
  it receives go stale.
- Topics: open refused for an undeclared output, a wrong type, a second writer;
  sequence continued across a module restart; dead-writer flag set and readers woken.
  `.dir`, the wake files and the wake words keep their inodes across a manager restart
  and a topic replacement.
- Bridge topics: a received topic of any type accepted; refused when a local module
  declares it or its topic id collides with a local one; a new sender boot id gives a
  new file with `first_seq` one below the bridge's first seq; the same boot id
  continues the old file.
- Stand-ins: created for remote modules, stored and handed over at `attach`; a
  `Remote=` entry matching a local module makes no stand-in, and the call reaches the
  local module.
- Generated types: a `Type=dataset:<name>` output opened with the spec's row schema,
  refused with any other; a policy module whose spec's `Name=` differs from its name
  refused.
- Adoption: kill the manager under load; the modules keep publishing; a new manager
  adopts them, flags the topics of a module that died meanwhile, and treats a registry
  PID that now belongs to another process as dead.
- An actuator killed with `SIGKILL` while the manager is stopped: the restarted manager
  runs its `Neutral` command before anything else.
- Shutdown order: modules with a `Neutral` command stopped and gone before the others
  are signalled.
- Re-execution: every module keeps running and stays a child; no call is lost.
  `systemctl reload` as root is accepted.
- Crash: call sockets come back from the descriptor store; a call queued through the
  crash and a module restart is answered. The store entry of a module removed by a
  reload is removed.
- An actuator killed with `SIGKILL`: its `Neutral` command runs before anything else.
  The same for a `Legacy` module with `[Module] Neutral`.
- The budgets of [00-overview](00-overview.md#budgets).
