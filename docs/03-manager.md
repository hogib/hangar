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
# Listening call sockets, kept by systemd across a crash of the manager.
FileDescriptorStoreMax=256
FileDescriptorStorePreserve=yes
# Modules outlive a manager crash and are adopted again.
KillMode=process
# The manager's own cgroup subtree, for one cgroup per module (optional).
Delegate=yes
CapabilityBoundingSet=CAP_SETUID CAP_SETGID CAP_SETPCAP CAP_SYS_NICE CAP_IPC_LOCK CAP_KILL CAP_CHOWN CAP_FOWNER
# Inherited by modules, which lower it unless LockMemory=yes.
LimitMEMLOCK=infinity
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

Its own settings, `/etc/hangar/hangard.conf`, all optional:

| Key | Default | |
|---|---|---|
| `ModuleDir` | `/etc/hangar/modules` | Enabled manifests |
| `MsgDir` | `/usr/share/hangar/msg` | Message types, for checking declared types |
| `TopicDir` | `/dev/shm/hangar` | |
| `HealthTick` | `100ms` | How often staleness is checked |
| `MaxModules` | 64 | |
| `LogFallbackDir` | `/run/hangar/log` | Module logs when journald is not running |
| `LogFallbackCap` | 256 KB | Per module |

### Restarting the manager

Three different things, kept apart:

| | How | Modules |
|---|---|---|
| **Planned restart, upgrade** | `hangar restart-manager`, or `systemctl reload hangard` | keep running. The manager re-executes its (possibly new) binary in place, passing its state and every descriptor it holds (pidfds, call sockets, the control socket) to the new image: the modules stay its children, nothing is adopted, nothing is lost |
| **Crash** | systemd restarts it | keep running, and are adopted ([Adoption](#adoption)); call sockets come back from systemd's descriptor store |
| **Stop** | `systemctl stop` or `restart`, shutdown of the computer: `SIGTERM` | are stopped, actuators first ([Shutdown](#shutdown)) |

`systemctl restart hangard` therefore stops the vehicle's modules; installers and
upgrades use `reload`, never `restart`.

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
`hangar.system` and `hangar.modules`, and for nothing else. Staleness is therefore
detected to within one tick (100 ms). Deadlines that need to be tighter, such as an
actuator's command deadman, are enforced inside the module that owns them
([07-safety](07-safety.md)), not by the manager.

## Start-up

1. Create `TopicDir` (owner the manager, group `hangar`, mode 0750) and
   `/run/hangar/{modules,call,log}`.
2. Read the registry, `/run/hangar/registry`, and **adopt** the modules still running
   from before ([Adoption](#adoption)). For every module in it that is *not* running
   any more, set the dead-writer flag on its topics, wake their readers, and kill
   whatever is left in its cgroup.
3. Load the manifests and the message types; check the graph
   ([02-manifest](02-manifest.md#how-the-manager-uses-manifests)). For each module
   that serves calls, take back its listening socket from systemd's descriptor store
   (after a crash), or create it and its wake word in `/run/hangar/call/`, and hand a
   copy to the store (`FDSTORE=1`, named after the module) so that the socket outlives
   any crash of the manager ([04-calls](04-calls.md)). Without systemd there is no
   store: after a crash of the manager, an adopted module's socket lasts only until that
   module's next restart, and calls queued at that moment are lost.
4. Create and publish its own topics, `hangar.system` and `hangar.modules`, so that no
   module can take their names first.
5. Start every accepted module that is not already running, all at once.

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

1. wait on a pipe until the manager has written the module's registry entry, and exit
   if the pipe closes first (the manager died): no module ever runs without an entry;
2. move into its cgroup;
3. resource limits: `RLIMIT_MEMLOCK` left unlimited if `LockMemory=yes` (the library
   calls `mlockall` at `hg_init`), lowered to the default otherwise;
4. scheduling policy, priority, nice and CPU affinity;
5. `/var/lib/hangar/<name>`, created if missing and given to the module's user, as
   working directory;
6. stdout and stderr connected to journald ([Logs](#logs)), so that journald sees the
   module's own process and cgroup;
7. for `Capabilities=` in the manifest, keep those capabilities across the user switch
   (`PR_SET_KEEPCAPS`, then raise them as ambient);
8. supplementary groups, group, and user, last;
9. exec.

What the module inherits:

| Descriptor | Environment | |
|---|---|---|
| wiring | `HANGAR_WIRING=<fd>` | A sealed `memfd`: the module's validated wiring ([Wiring](#wiring)) |
| module page | `HANGAR_PAGE=<fd>` | Writable descriptor of `/run/hangar/modules/<name>` ([Module page](#module-page)) |
| call socket | `HANGAR_CALL=<fd>` | The listening socket for the module's calls, if it serves any ([04-calls](04-calls.md)) |
| stdout, stderr | | journald streams, or the fallback log file ([Logs](#logs)) |
| stdin | | `/dev/null` |

`HANGAR_MODULE=<name>`, `HANGAR_CONTROL=/run/hangar/control` and
`HANGAR_TOPIC_DIR=<TopicDir>` are set too.

User and group names are resolved from `/etc/passwd` and `/etc/group`, which the
manager reads itself: the release binaries are static (musl), with no name service
modules.

## Wiring

Text, one line per item, so it can be read in a debugger or by `hangar show`:

```
hangar-wiring 1
module depth_hold
sub nav.depth type=depth hash=9f3a61c20b7e4d15 rate=50
sub nav.imu type=imu hash=0c41e2aa93f81d70 rate=200
pub ctl.depth_hold type=wrench hash=77d0b3a1c5e29f04 slots=16 rate=50 stale_after=60ms inputs=nav.depth,nav.imu
pub depth_hold.calls type=calls:depth_hold hash=5c0e97a2d4b81f63 slots=64 implicit
serve reset req=empty hash=… size=8 rep=empty hash=… size=8 deadline=100ms allow=pilot_console
call safety.disarm req=empty hash=… size=8 rep=empty hash=… size=8 remote=0
```

`hg_init()` reads it once. The library refuses to open a topic not listed or make a
call not listed, and takes slots, rates, inputs, call types, sizes, deadlines and
`Allow` lists from here, never from the module's code. **Implicit outputs** are those
the manager adds itself: `<module>.calls` for a module that serves calls (its type
generated from the largest request and reply,
[04-calls](04-calls.md#the-call-record)) and `<module>.params` for one with
`[Params]`. They are checked, created and flagged like any declared output.

## Opening topics

When the module calls `hg_pub_open`, the library sends one request on the control
socket: the topic name, the type hash, the schema text and the payload size (the size
matters for frame topics, whose size the module decides). The manager:

1. identifies the module from the socket's peer credentials (`SO_PEERCRED`: the PID
   must be a running module's main process);
2. checks the topic is one of that module's declared outputs, that the schema hashes
   to the type the manifest declares (`frame` accepts any frame type), and that no
   other process holds it open for writing;
3. creates the topic file ([01-topics](01-topics.md#writing)), continuing the
   sequence numbers of any file it replaces;
4. replies with the descriptor (`SCM_RIGHTS`), or with the reason it refused.

Subscribers never ask the manager: they open topic files read-only by path. A module
that opens its outputs while the manager is restarting waits in `hg_pub_open` until the
manager answers.

## Module page

`/run/hangar/modules/<name>`, 4 KB, created by the manager; the module holds a writable
descriptor. It is how a module tells the manager things without a system call:

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
tick. Being a file rather than a descriptor the
manager holds, it survives a manager restart. Only the module and the manager can
write it, so no other module can mark it ready or alive.

A module that serves calls also has a **wake word**, `/run/hangar/call/<name>.wake`
(4 bytes, group `hangar`, mode 0660), which any caller increments to wake it
([04-calls](04-calls.md#protocol)). It is a separate file because every module must
be able to write it, and a spurious wake costs nothing.

## States

| State | Meaning | Next |
|---|---|---|
| `starting` | spawned, not yet ready | `ready`; `backoff` on exit; `unhealthy` if not ready after `StartGrace` |
| `ready` | published on every rated output (event outputs, `.calls` and `.params` excluded), or `hg_ready()` | `unhealthy`; `backoff` on exit |
| `unhealthy` | a rated topic stale for `StaleGrace`, or the watchdog missed | stopped, then `backoff` |
| `backoff` | waiting to restart | `starting` |
| `failed` | gave up: more failures than `Burst` | stays down until `hangar restart <name>` or a reload |
| `stopped` | stopped on purpose, or `Policy` says not to restart | |
| `refused` | its manifest or its place in the graph was refused | |

**Restarting.** The delay starts at the first value of `Backoff` and doubles on each
failure up to the last. After `ResetAfter` in `ready`, the delay and the failure count
start over. A failure that brings the count within the `Burst` period above its limit
moves the module to `failed` and applies `OnGiveUp`: `degrade` marks the system
degraded; `failsafe` puts it in failsafe ([07-safety](07-safety.md)).

**Exit status 78** (`EX_CONFIG`) means the module refused its own configuration (its
saved parameters, [05-parameters](05-parameters.md#saved-values)): restarting would
not help, so the module goes straight to `failed` with its status line as the reason,
and `OnGiveUp` applies.

**A restart the module asks for** (a `@restart` parameter changed) is a stop and a
start that does not count as a failure.

**When an actuator module exits**, for any reason, the manager first runs its
`[Actuator] Neutral` command ([07-safety](07-safety.md#the-actuator-gate)), as the
module's user and groups, with a 1 s limit, before anything else; if that command fails
or is missing, the system enters failsafe. A crashed actuator cannot write neutral
itself, and some output hardware keeps its last command. Actuator modules also default
to `StopTimeout=500ms`.

**When a module exits**, the manager reads its status without reaping it
(`waitid(P_PIDFD, …, WEXITED | WNOWAIT)`), sets the dead-writer flag on each of its
topics and wakes their readers, kills anything left in its cgroup or process group
(the unreaped leader keeps the group's id from being reused), then reaps it, logs the
exit code or signal, and schedules the restart. For an adopted module, which is not the
manager's child and has no such guarantee, only its cgroup is killed.

## Stopping a module

`SIGTERM` to the module's process group; after `StopTimeout`, `SIGKILL` to its cgroup
(`cgroup.kill`) or, without one, to its process group. The leader is not reaped before
the group is signalled, so its process group id cannot have been reused. A module's
children that left its process group (`setsid`) are only caught with cgroups.

## Shutdown

Modules with an actuator output first, which set their outputs to neutral on
`SIGTERM` ([07-safety](07-safety.md)); once they have exited or been killed, every
other module in parallel. Then the manager removes the registry and exits.

## Adoption

A manager that crashes or is restarted leaves its modules running (`KillMode=process`,
no `PDEATHSIG`). The registry, `/run/hangar/registry`, rewritten atomically
(temporary file and rename) on every state change, holds for each module its PID, its
process start time from `/proc/<pid>/stat`, its state, its failure count and its
current delay.

The registry is readable by group `hangar`: a module serving calls uses it to tell
which module is calling ([04-calls](04-calls.md#callers)). The manager writes a
module's entry as soon as `clone3` returns, before anything else.

The registry is text, one module per line, so that any language can read it:

```
hangar-registry 1
depth 1234 98231544 ready 0 0.1
thrusters 1240 98231551 ready 0 0.1
```

(name, PID, start time in clock ticks from `/proc/<pid>/stat`, state, failures,
current delay in seconds.)

On start the manager opens a pidfd for each PID in it (`pidfd_open`), reads the start
time again, and adopts the process only if both match; a PID now belonging to another
process is treated as a module that died ([Start-up](#start-up)). An adopted module
keeps its restart history. Its topics, module
page and call socket are files and need nothing from the old manager. What is lost:
the exit status of an adopted module, which is not the new manager's child, is logged
as unknown.

## Logs

A module's stdout and stderr are each a stream to journald
(`/run/systemd/journal/stdout`), connected by the child itself after it has moved into
its cgroup and before it switches user, with the module's name as identifier:
`journalctl -t depth`. journald therefore records the module's own PID and cgroup, and
applies its rate limit to each module separately. The manager is not in the path, so
its restart cannot leave a module writing into a closed pipe.

Without journald, both go to `/run/hangar/log/<name>.log`, opened in append mode. A
file the module holds open in append mode cannot be shortened without losing lines,
so the manager only cuts it, to its last `LogFallbackCap`, when it starts the module.

## System state and the manager's topics

| Topic | Rate | Content |
|---|---|---|
| `hangar.system` | on change, and 1 Hz | `nominal`, `degraded` or `failsafe`, and since when |
| `hangar.modules` | on change, and 1 Hz | per module: name, state, PID, failures, last exit, time in state, status line |

They are ordinary topics: the recorder keeps them, `hangar top` reads them, and a
network can take the health of the system as an input.

## Control socket

`/run/hangar/control`, `SOCK_SEQPACKET`, group `hangar`, mode 0660. One request per
packet, one reply per request, text.

| Request | From | |
|---|---|---|
| `open <topic> …` | the module's main process | [Opening topics](#opening-topics) |
| `restart-self` | the module's main process | a `@restart` parameter changed |
| `param-write <module> …` | `hangar-admin` | parameters of a module that is not running, written as its user ([05-parameters](05-parameters.md#saved-values)) |
| `status` | anyone in `hangar` | |
| `start`, `stop`, `restart <name>` | `hangar-admin` | |
| `reload` | `hangar-admin` | as `SIGHUP` |
| `reexec` | `hangar-admin` | re-execute the manager in place ([Restarting the manager](#restarting-the-manager)) |

Callers are identified by `SO_PEERCRED` and their groups by `SO_PEERGROUPS`. The
group `hangar-admin` holds the operators; its members are also in `hangar`, which the
sockets need.

## Tests

- Spawning: unresolvable programs, exec failure, descriptors and signals in the
  child; user switching in a user namespace with a mapped range of ids; scheduling,
  memory locking and capabilities as real root (a virtual machine or the Pi), since a
  user namespace cannot grant them.
- A manager killed between `clone3` and the registry write: the child never runs.
- Restart: back-off doubling, reset after `ResetAfter`, give up after `Burst`, both
  `OnGiveUp` actions.
- Health: a module that stops publishing is restarted; one that is slow to start is
  not, within `StartGrace`; the watchdog.
- Topics: open refused for an undeclared output, a wrong type, a second writer;
  sequence continued across a module restart; dead-writer flag set and readers woken.
- Adoption: kill the manager under load; the modules keep publishing; a new manager
  adopts them, flags the topics of a module that died meanwhile, and treats a registry
  PID that now belongs to another process as dead.
- Shutdown order: actuator modules stopped and gone before the others are signalled.
- Re-execution: every module keeps running and stays a child; no call is lost.
- Crash: call sockets come back from the descriptor store; a call queued through the
  crash and a module restart is answered.
- An actuator killed with `SIGKILL`: its `Neutral` command runs before anything else.
- The budgets of [00-overview](00-overview.md#budgets).
