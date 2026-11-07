# Manager

`hangard` starts the modules, watches them, restarts them when they fail, and keeps
the system's state. It is the control plane only: it moves no data, sits in no call
between modules, and modules keep running and exchanging data while it restarts. It is
involved in the data plane at exactly one moment: when a module opens a topic for
writing, the manager creates the file and hands the module the descriptor.

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
# The manager pings every 2 s from its event loop; hung for 10 s, it is restarted.
WatchdogSec=10
ExecStart=/usr/bin/hangard
# A planned restart or an upgrade: re-execute in place, keeping every module.
ExecReload=/usr/bin/hangar restart-manager
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
modules, but cannot load kernel modules, mount, trace processes, or change the network.

**The watchdog.** `Type=notify` with `WatchdogSec=10`: the manager writes `READY=1`
once and `WATCHDOG=1` every 2 s to `$NOTIFY_SOCKET` from its event loop (a datagram on
a Unix socket; no libsystemd), on the same timer as its other work. A manager whose loop
has hung for 10 s is killed and restarted by systemd; its modules keep running and are
adopted ([Adoption](#adoption)), and on start it neutralizes every actuator whose module
is not healthy ([Start-up](#start-up)). On a system without systemd any supervisor that
restarts it will do (BusyBox init `respawn`, runit); the watchdog is then the
supervisor's, the per-module cgroups are not available and process groups are used
([Stopping](#stopping-a-module)).

Its own settings, `/etc/hangar/hangard.conf`:

| Key | Default | |
|---|---|---|
| `Role` | — (required) | `vehicle` or `topside`. Names the manager's own topics and the bridge's ([System state](#system-state-and-the-managers-topics)) |
| `PrivilegedModule` | none | The one module allowed `[Module] Privileged=yes`, which runs as root with the manager's capability set ([02-manifest](02-manifest.md#module-required-once)): `installer` on the vehicle ([13-deployment](13-deployment.md#upgrading-the-vehicle-over-the-tether)). A manifest with the key under any other name, or a second one with it, is refused |
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
| `BridgeConf` | the module with `[Bridge]` | the topics it receives, with their `ExpectRate` and `StaleAfter`, among them the `.req` and `.rep` topics that carry calls across the tether ([08-bridge](08-bridge.md#calls-across-the-tether)) |
| `DatasetDir/<name>.dataset` | an output declared `Type=dataset:<name>` | the row schema, which the manager generates ([06-recorder](06-recorder.md#live-the-pairer)) |
| the policy spec named in `Exec` | a module whose program is `/usr/lib/hangar/hangar-policy` | its `Name=`: the module must be `policy_<Name>`, or it is refused ([09-learning](09-learning.md#policy-modules)) |

### Restarting the manager

Three different things, kept apart:

| | How | Modules |
|---|---|---|
| **Planned restart, upgrade** | `hangar restart-manager`, or `systemctl reload hangard` | keep running. The manager re-executes its (possibly new) binary in place, passing its state and every descriptor it holds (pidfds, the control socket, its own topics) to the new image: the modules stay its children, nothing is adopted, nothing is lost |
| **Crash, or hung past `WatchdogSec`** | systemd restarts it | keep running, and are adopted ([Adoption](#adoption)). Calls between modules ride on topics, which the manager does not hold, so none is lost |
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
| one `timerfd` | the earliest of: back-off delays, stop timeouts, the health tick, the once-a-second publication of its own topics, and the 2 s watchdog ping to systemd ([Running it](#running-it)) |
| the control socket | requests from modules and from the `hangar` command |

The health tick runs only while some module has a rated topic or a watchdog, or is in
its start grace; with none, the manager wakes once a second, to publish
`hangar.<role>.system` and `hangar.<role>.modules` and, every other second, to ping
systemd's watchdog, and for nothing else. An operator's
call (`hangar call`) is one control request, answered by one publish on
`hangar.<role>.admin.req` ([Control socket](#control-socket)). Staleness is
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
   written, and gives that time. This covers a module that died while no manager was
   running and one that died while the manager hung: a manager that misses its
   watchdog is killed by systemd ([Running it](#running-it)), and its successor finds
   the module gone the same way, up to 10 s and a restart later.
2. Create `TopicDir` (owner the manager, group `hangar`, mode 0750), its directory
   word `.dir` if missing, `/run/hangar/{modules,log,neutral}`, and `RecordingDir`
   if missing (owner root, group `hangar-rec`, mode 2770;
   [06-recorder](06-recorder.md#sessions)).
3. **Adopt** the modules still running from before ([Adoption](#adoption)), and take
   back the system state from the registry. For every module in it that is *not*
   running any more, set the dead-writer flag on its topics, wake their readers, and
   kill whatever is left in its cgroup. An adopted actuator module whose rated output is
   already stale (it stopped publishing while the manager hung) is unhealthy at once,
   without `StartGrace`: it is stopped, its `Neutral` command runs as it exits, and it
   restarts under its `[Restart]` rules. So after a hung manager every actuator whose
   module is not healthy has been put to neutral before the system is `nominal` again.
4. Load the manifests, the message types and the files above; check the graph
   ([02-manifest](02-manifest.md#how-the-manager-uses-manifests)), and derive each
   module's implicit topics and subscriptions ([Wiring](#wiring)).
5. Create and publish its own topics, `hangar.<role>.system`, `hangar.<role>.modules`
   and `hangar.<role>.admin.req`, so that no module can take their names first. The
   files of an earlier instance are continued, as any writer's are
   ([Opening topics](#opening-topics)).
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

A module with `[Module] Privileged=yes`, the one `PrivilegedModule=` names
([02-manifest](02-manifest.md#module-required-once)), skips steps 7 to 9: it stays uid
0, and after exec its permitted and effective sets are the manager's own capability set,
the unit's `CapabilityBoundingSet`, and nothing more. Everything else, the registry
entry, the cgroup, the state directory, the journald streams and the wiring, is as for
any module. This is the `installer` module
([13-deployment](13-deployment.md#upgrading-the-vehicle-over-the-tether)).

What the module inherits:

| Descriptor | Environment | |
|---|---|---|
| wiring | `HANGAR_WIRING=<fd>` | A sealed `memfd`: the module's validated wiring ([Wiring](#wiring)) |
| stdout, stderr | | journald streams, or the fallback log file ([Logs](#logs)) |
| stdin | | `/dev/null` |

`HANGAR_MODULE=<name>`, `HANGAR_CONTROL=/run/hangar/control` and
`HANGAR_TOPIC_DIR=<TopicDir>` are set too.

**The module page is not inherited.** A process is dumpable again after exec, and
until `hg_init` runs, another process of the same user could copy a descriptor out of
it (`pidfd_getfd`, `/proc/<pid>/fd`). `hg_init` therefore first makes the module
non-dumpable (`PR_SET_DUMPABLE` 0), then sends `page` on the control socket. The
manager identifies the module by `SO_PEERCRED` (the PID must be the main process of a
module instance it started or adopted), refuses a process that is being traced
(`TracerPid` in `/proc/<pid>/status` is not 0), and replies with the writable module
page, by `SCM_RIGHTS`. A module that asks while the manager is restarting waits
until it answers. A `Legacy` module never asks and has no page. The wiring memfd
is still inherited: it is sealed, and holds nothing another module cannot read in the
manifests ([01-topics](01-topics.md#permissions)). Its topic descriptors, the request
and reply topics of its calls among them, come later still, from `hg_pub_open`
([Opening topics](#opening-topics)).

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
sub hangar.vehicle.admin.req type=req:hangar hash=… slots=16 implicit
subglob *.req implicit
sub safety.rep type=rep:safety hash=… slots=16 implicit
pub ctl.depth_hold type=wrench hash=77d0b3a1c5e29f04 slots=16 rate=50 stale_after=60ms inputs=nav.depth,nav.imu
pub depth_hold.req type=req:depth_hold hash=… slots=16 implicit
pub depth_hold.rep type=rep:depth_hold hash=… slots=16 implicit
serve reset req=empty hash=… size=8 rep=empty hash=… size=8 deadline=100ms allow=topside:pilot_console
call safety.disarm req=empty hash=… size=8 rep=empty hash=… size=8 remote=0
```

| Line | |
|---|---|
| `sub <topic>` | A subscription, declared or implicit. At most 127 per module, implicit ones included: what one wait can hold beside the directory word ([01-topics](01-topics.md#reading)) |
| `subglob <pattern>` | A glob subscription (`[Subscribe nav.*]`), or the implicit `subglob *.req` of a server whose method the bridge may carry from the other side: any topic matching it may be opened, including topics created after start. Not counted toward the 127: `hg_wait` polls its matches, every 10 ms at most, instead of waiting on them ([01-topics](01-topics.md#reading)) |
| `pub <topic>` | An output, declared or implicit, with `inputs=` (referenced by every message) and `sources=` (referenced per message, with `hg_publish_with`; [01-topics](01-topics.md#provenance)) |
| `serve <method>` | A method the module serves, with its types, deadline and `Allow` tokens ([04-calls](04-calls.md#declaring-calls)) |
| `call <module>.<method>` | A method it calls, with its types: the server's for a module enabled here, the `[Call]`'s own `Request=` and `Reply=` for a method of the other side; `remote=1` when the serving module is on the other side of the tether ([Calls across the tether](#wiring)) |
| `mirror <topic or glob> type=any rate=… stale_after=…` | The bridge only: topics its configuration receives, with the receiving file's rate and staleness from `[Mirror] ExpectRate` and `StaleAfter` (rate 0, an event topic, without them) |

`hg_init()` reads it once. The library refuses to open a topic not listed or make a
call not listed, and takes slots, rates, inputs, call types, sizes, deadlines and
`Allow` lists from here, never from the module's code.

**Implicit outputs** are those the manager adds itself, marked `implicit`:

| Output | For | Type |
|---|---|---|
| `<module>.req` | a module with a `[Call]` | `req:<module>`: the request envelope and the largest request type among the methods it calls ([04-calls](04-calls.md#the-two-topics)) |
| `<module>.rep` | a module with a `[Serve]` or `[Params]` | `rep:<module>`: the reply envelope and the largest reply type among the methods it serves, `set_params` included ([05-parameters](05-parameters.md#setting-parameters)) |
| `<module>.params` | a module with `[Params]` | the parameter topic's type ([05-parameters](05-parameters.md#the-parameter-topic)) |

Each has 16 slots. The bridge's are `bridge.<role>.link` (10 Hz), `bridge.<role>.budget`
(1 Hz, the rate left for video, [08-bridge](08-bridge.md#video-and-the-tether)),
`bridge.<role>.files` (event) and the method `send`, with its `bridge.rep`
([08-bridge](08-bridge.md#the-bridge-module)). They are checked, created and flagged
like any declared output. An output declared `Type=dataset:<name>` also gets a
generated type: the row schema, which the manager builds from
`DatasetDir/<name>.dataset` and the types of the topics it joins
([06-recorder](06-recorder.md#the-row)).

**Implicit subscriptions** wire the calls. A call is a request on the caller's `.req`
topic and a reply on the server's `.rep` topic ([04-calls](04-calls.md#the-rules)), so
from the `[Call]` and `[Serve]` sections of every manifest the manager gives:

- a server `sub <caller>.req` for every module on this machine that declares a `[Call]`
  to one of its methods, and `sub hangar.<role>.admin.req`, the manager's own request
  topic for operators ([Control socket](#control-socket)); and, when the bridge's
  `[Calls] Remote=` names one of its methods, `subglob *.req`: the other side's callers
  are not known before the handshake, so their mirrored request topics, and the other
  side's `hangar.<role>.admin.req`, are opened as the bridge creates them and read
  within 10 ms ([01-topics](01-topics.md#reading),
  [08-bridge](08-bridge.md#calls-across-the-tether));
- a caller `sub <server>.rep` for every module it declares a `[Call]` to, a module on
  this machine or one whose `.rep` topic the bridge receives.

One implicit subscription is not a call's. The `Privileged` module may have no
`[Subscribe]` ([02-manifest](02-manifest.md#module-required-once)), so the manager
gives it `sub safety.state`, marked `implicit` like the others, and nothing else: it
refuses `apply` and `rollback` while the vehicle is not `disarmed`
([13-deployment](13-deployment.md#upgrading-the-vehicle-over-the-tether)).

The caller is the writer of the request topic, which the manager guarantees as it
guarantees every topic's writer ([Opening topics](#opening-topics)): a server takes
the caller's name from the topic it read the request on, and checks `Allow` against
it. No PID is looked up and no registry read.

**Calls across the tether.** Module and topic names are unique across the vehicle and
the topside together ([08-bridge](08-bridge.md#calls-across-the-tether)). A declared
call is remote when no module enabled on this machine serves it and the bridge's
configuration mirrors the caller's `.req` topic to the other side and the server's
`.rep` topic back (the `[Mirror]` entries that `[Calls] Remote=` generates); the `call`
line then carries `remote=1` with the types of the `[Call]`'s own `Request=` and
`Reply=`, which size the caller's `.req` payload, and the caller's `sub <server>.rep`
names a topic the bridge receives. A mirrored `.req` topic has `origin_boot` set ([01-topics](01-topics.md#mirrored-topics)),
so a server identifies its caller as `<role>:<module>`, with nothing asserted by
anyone. A `Remote=` entry that matches a module enabled on this machine is not
applied here: its `.req` and `.rep` topics are local, and the call stays local. With
the same `bridge.conf` on both machines this is normal (`safety.*` is remote on the
topside only), so it is not reported. A declared call that no module serves on either
side is reported by `hangar check`, and the library answers it `HG_E_NOROUTE`
([04-calls](04-calls.md#status-codes)). A module enabled on both machines is found at
the handshake, from the module lists the bridges exchange, and reported by
`hangar check`.

## Opening topics

When the module calls `hg_pub_open`, the library sends one request on the control
socket: the topic name, the type hash, the schema text and the payload size (the size
matters for frame topics, whose size the module decides). The bridge also sends the
sender's boot id and the sequence number of the first message it will publish. The
manager:

1. identifies the module from the socket's peer credentials (`SO_PEERCRED`: the PID
   must be a running module's main process);
2. checks the topic is one of that module's declared or implicit outputs, that the
   schema hashes to the type the manifest declares or the manager generated (`frame`
   accepts any frame type), and that no other process holds it open for writing. For
   the bridge, a topic matching one of its `mirror` lines is accepted too, with any
   type, provided no local module declares it, its topic id collides with no local
   topic's, and it is not the `.req`, `.rep` or `.params` topic of a module enabled on
   this machine, nor this machine's `hangar.<role>.admin.req`: a request topic the
   bridge writes always belongs to a module or an operator of the other side;
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
`.dir` and every topic's wake file are created once per boot (`O_CREAT`, never by
renaming, never truncated) and are never replaced or deleted during the boot: a
restarted or re-executed manager opens the existing files, and a replaced topic keeps
its wake file, so no sleeper is left waiting on an old inode.

Subscribers never ask the manager: they open topic files read-only by path. A module
that opens its outputs while the manager is restarting waits in `hg_pub_open` until the
manager answers.

## Module page

`/run/hangar/modules/<name>`, 4 KB, created by the manager (group `hangar`, mode 0640);
the module holds the only writable descriptor, which it gets with the `page` request at
`hg_init`. It is how a module tells the manager things without a system call:

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

## States

| State | Meaning | Next |
|---|---|---|
| `starting` | spawned, not yet ready | `ready`; `backoff` on exit; `unhealthy` if not ready after `StartGrace` |
| `ready` | published on every rated output (event outputs, `.req`, `.rep`, `.params` and the bridge's received topics excluded), or `hg_ready()` | `unhealthy`; `backoff` on exit |
| `unhealthy` | a rated topic stale for `StaleGrace`, or the watchdog missed. A message flagged as computed from stale inputs is not stale: a policy with no model, publishing flagged neutral outputs, stays `ready` | stopped, then `backoff` |
| `backoff` | waiting to restart | `starting` |
| `failed` | gave up: more failures than `Burst` | stays down until `hangar restart <name>` or a reload |
| `stopped` | stopped on purpose, or `Policy` says not to restart | |
| `refused` | its manifest or its place in the graph was refused | |

**The bridge's received topics** never count toward its readiness or its health. They
keep their rate and `StaleAfter` for their readers, but a tether outage makes them
stale, and must not restart the bridge: the bridge is judged by its own watchdog and
its own rated outputs, `bridge.<role>.link` and `bridge.<role>.budget`, which it
publishes whether the link is up or not ([08-bridge](08-bridge.md#link-status)).

**Restarting.** The delay starts at the first value of `Backoff` and doubles on each
failure up to the last. After `ResetAfter` in `ready`, the delay and the failure count
start over. A failure that brings the count within the `Burst` period above its limit
moves the module to `failed` and applies `OnGiveUp`: `degrade` marks the system
degraded; `failsafe` puts it in failsafe
([System state](#system-state-and-the-managers-topics)).

**Actuator modules** take the defaults, so one that crashes on every start is given up
after about 3 s: `Burst=5/60s` allows five failures, and the back-offs between them
and the sixth, 0.1, 0.2, 0.4, 0.8 and 1.6 s, add up to 3.1 s; `OnGiveUp=failsafe` then
puts the system in failsafe, with the module's `Neutral` command having run at each
exit. This is deliberate: a thruster driver that cannot stay up for three seconds must
not keep taking commands, and a longer sequence of restarts would keep the thrusters
answering to a driver that is not trusted. The operator clears the failsafe after
restarting the module by hand ([System state](#system-state-and-the-managers-topics)).

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

The registry is the manager's alone (owner root, mode 0600): no module reads it, and a
server tells which module is calling from the request topic the request arrived on
([Wiring](#wiring)). What tools and modules need of it is on `hangar.<role>.modules`
([System state](#system-state-and-the-managers-topics)). The manager writes a module's
entry as soon as `clone3` returns, before anything else.

The registry is text, one module per line, so that a new version of the manager, or
a person, can read it:

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
keeps its restart history, and the system keeps its state. Its topics, the request and
reply topics of its calls among them, and its module page are files and descriptors
the module already holds, and need nothing from the old manager. What is lost: the exit status of an adopted module, which is not the
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
`safety.state`, and refusals on the servers' `.rep` topics. The manager is not in the path, so
its restart cannot leave a module writing into a closed pipe.

Without journald, both go to `/run/hangar/log/<name>.log`, opened in append mode. A
file the module holds open in append mode cannot be shortened without losing lines,
so the manager only cuts it, to its last `LogFallbackCap`, when it starts the module.

## System state and the manager's topics

| Topic | Rate | Content |
|---|---|---|
| `hangar.<role>.system` | on change, and 1 Hz | `nominal`, `degraded` or `failsafe`, since when, and why |
| `hangar.<role>.modules` | on change, and 1 Hz | per module: name, state, PID, failures, last exit, time in state, status line |
| `hangar.<role>.admin.req` | event | operators' call requests, one per `hangar call` ([Control socket](#control-socket)); type `req:hangar`, the request envelope and 16 KB, the largest request a method may take ([04-calls](04-calls.md#declaring-calls)), so a request to any method on either side fits |

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
| `page` | the module's main process, at `hg_init` | its module page ([Starting a module](#starting-a-module)) |
| `open <topic> …` | the module's main process | [Opening topics](#opening-topics) |
| `restart-self` | the module's main process | a `@restart` parameter changed |
| `call <module>.<method> …` | `hangar-admin` (`hangar call`, `hangar param set`) | the manager publishes the request on `hangar.<role>.admin.req`, with `admin:<user>` in the envelope's `caller` field, which servers trust on that topic alone, and answers with the `call_id`; the command reads the reply on the server's `.rep` topic ([04-calls](04-calls.md#admin-calls)). Refused when no module serves the method on either side |
| `param-write <module> …` | `hangar-admin` | parameters of a module that is not running, written and `fsync`ed as its user, by the rules of [05-parameters](05-parameters.md#saved-values) |
| `clear-failsafe` | the `safety` module, `hangar-admin` (`hangar clear-failsafe`) | leave system failsafe ([System state](#system-state-and-the-managers-topics)) |
| `status` | anyone in `hangar` | |
| `start`, `stop`, `restart <name>` | `hangar-admin` | |
| `reload` | `hangar-admin` | as `SIGHUP` |
| `reexec` | `hangar-admin` | re-execute the manager in place ([Restarting the manager](#restarting-the-manager)) |

Callers are identified by `SO_PEERCRED` and their groups by `SO_PEERGROUPS`. The
group `hangar-admin` holds the operators; its members are also in `hangar`, which the
socket and the topics need. A caller with uid 0 counts as `hangar-admin`: `systemctl
reload` runs `hangar restart-manager` as root, outside that group. The control socket
is the only way an operator's identity enters the system: `admin:<user>` is written
by the manager, on its own topic, and never by the process that asks.

## Tests

- Spawning: unresolvable programs, exec failure, descriptors and signals in the
  child; user switching in a user namespace with a mapped range of ids; scheduling,
  memory locking and capabilities as real root (a virtual machine or the Pi), since a
  user namespace cannot grant them.
- Capabilities: a module with `Capabilities=CAP_SYS_NICE` has exactly that capability
  in `CapAmb`, `CapPrm` and `CapEff` of `/proc/<pid>/status` after exec, and can set a
  thread's real-time priority; a module without `Capabilities` has none. A `Privileged`
  module runs as uid 0 with the unit's bounding set in `CapPrm` and `CapEff`, and no
  module but the one `PrivilegedModule=` names is accepted with the key.
- Watchdog: a manager stopped with `SIGSTOP` is killed and restarted by systemd after
  `WatchdogSec`; the new manager adopts the modules, runs the `Neutral` command of an
  actuator killed while the old one hung, and stops and neutralizes an adopted actuator
  whose output is already stale, without `StartGrace`; a manager that is merely busy
  keeps pinging every 2 s under load.
- A manager killed between `clone3` and the registry write: the child never runs.
- Module page: the page is not open in the module before `hg_init`; `page` refused
  from a process that is not a module's main process, and from a traced one; a module
  asking while the manager re-executes gets its page.
- Wiring: a module with `[Call]` gets `<module>.req`, one with `[Serve]` or `[Params]`
  gets `<module>.rep`, both `implicit`, 16 slots, with the generated types; a server's
  wiring has `sub <caller>.req` for every local module that calls it and
  `sub hangar.<role>.admin.req`, and `subglob *.req` when the bridge's `[Calls]` names
  one of its methods; a caller's has `sub <server>.rep`; a call to a method served on
  the other side has `remote=1` with the `[Call]`'s own types, and `remote=0` with the
  server's when the same `Remote=` entry names a module enabled here; a call nobody
  serves on either side is reported by `hangar check`; the `Privileged` module's
  wiring holds `sub safety.state`, its callers' request topics and `pub installer.rep`,
  nothing else; a wiring past 127 subscriptions, implicit ones counted, refused.
- Restart: back-off doubling, reset after `ResetAfter`, give up after `Burst`, both
  `OnGiveUp` actions; an actuator module that exits at once on every start is `failed`
  and the system in failsafe within 3.5 s of its first start, its `Neutral` command
  having run six times.
- System state: failsafe on a give-up and on a failing `Neutral` command;
  `clear-failsafe` refused while the given-up module is `failed`, accepted after it
  is restarted; the state kept across a manager crash and a re-execution.
- Health: a module that stops publishing is restarted; one that is slow to start is
  not, within `StartGrace`; the watchdog. The bridge is not restarted when the topics
  it receives go stale.
- Topics: open refused for an undeclared output, a wrong type, a second writer;
  sequence continued across a module restart; dead-writer flag set and readers woken.
  `.dir` and the wake files keep their inodes across a manager restart and a topic
  replacement.
- Bridge topics: a received topic of any type accepted; refused when a local module
  declares it or its topic id collides with a local one; refused for the `.req`,
  `.rep` or `.params` topic of a module enabled here and for this machine's
  `hangar.<role>.admin.req`; a new sender boot id gives a new file with `first_seq`
  one below the bridge's first seq; the same boot id continues the old file.
- Admin calls: `call` on the control socket from `hangar-admin` publishes one request
  on `hangar.<role>.admin.req` with `caller` `admin:<user>` and answers with its
  `call_id`; refused from a process outside `hangar-admin`, and for a method nobody
  serves; the topic published by the manager, so no module can take its name.
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
- Re-execution: every module keeps running and stays a child; a call in flight is
  answered; the manager's own topics continue their sequence numbers. `systemctl
  reload` as root is accepted.
- Crash: a request published while the manager was down, to a server that restarted
  meanwhile, is answered when in time and dropped unexecuted when expired; nothing the
  manager held was needed.
- An actuator killed with `SIGKILL`: its `Neutral` command runs before anything else.
  The same for a `Legacy` module with `[Module] Neutral`.
- The targets of [00-overview](00-overview.md#targets), reported by `hangar bench`.
