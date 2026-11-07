# Module manifest

A module's unit file: what it runs, what it publishes and reads, how it is restarted
and how it is scheduled. The manager reads every manifest at start (and on reload),
builds the wiring graph from them, refuses a graph that is wrong, and supervises each
module by what its manifest declares.

## Where manifests live

| Path | |
|---|---|
| `/usr/share/hangar/modules/<name>.module` | Templates, installed with the modules. Never run from here |
| `/etc/hangar/modules/<name>.module` | Enabled on this machine: a symlink to a template, or a copy edited for this machine |

A module runs on a machine if and only if its manifest is in `/etc/hangar/modules/`.
The vehicle and the topside differ in which modules are enabled and in the manager's
`Role` (`vehicle` or `topside`, in `hangard.conf`, [03-manager](03-manager.md#running-it)).
There is no other notion of a profile.

The module's **name** is the file name without `.module`: `[a-z0-9_]`, 1 to 31
bytes. The manager reports and logs the module by that name.

**One namespace for the pair.** Module names and topic names are unique across the
vehicle and the topside together, so that a name means one thing on either end of the
tether: the topside's recorder, for instance, is `topside_recorder`, not `recorder`.
The two bridges exchange their module and topic lists, with the topic ids, when they
connect, and refuse to mirror a name that collides; the collision is reported in
`config_match` and by `hangar check` ([08-bridge](08-bridge.md#the-link)). Per-machine
infrastructure is the one exception: the `bridge` module has the same name on both
sides, and the manager's and the bridge's own topics carry the machine's role
(`hangar.<role>.system`, `hangar.<role>.modules`, `bridge.<role>.link`,
`bridge.<role>.budget`, `bridge.<role>.files`).

## Syntax

```ini
# A comment: a line whose first non-blank character is '#' or ';'.
[Section]
Key=Value

[Section argument]
Key=Value
```

- **Full-line comments only.** `#` inside a value is part of the value (an `Exec`
  argument may contain one).
- **Strict.** An unknown section or key, a key given twice (other than the
  repeatable ones marked below), or a value that does not parse stops the manager from
  loading that manifest, with the file, line and reason. The other manifests load.
- **Structure only.** A program's own settings live in its own file, named on its
  `Exec` line (`bridge.conf`, `recorder.conf`, `pairer.conf`, a policy spec), never in
  its manifest.
- Blank space around `=` and at the ends of a value is ignored.

| Value | Written as |
|---|---|
| boolean | `yes` / `no` |
| duration | a number with `ms`, `s` or `min`: `100ms`, `2.5s`, `1min` |
| rate | hertz, decimals allowed: `50`, `0.5` |
| duration range | `0.1s..10s` |
| burst | `<count>/<duration>`: `5/60s` |
| glob | a topic name in which `*` stands for any run of characters, dots included: `nav.*`, `*.params` |

## Example

```ini
# /etc/hangar/modules/depth.module
# A Bar30 on a bus of its own: in phase 1 the Navigator's buses belong to ArduSub.
[Module]
Description=Bar30 depth and temperature
Exec=/usr/lib/hangar/bar30 --i2c /dev/i2c-6
SupplementaryGroups=i2c6

[Publish nav.depth]
Type=depth
Rate=50
Slots=16
```

```ini
# /etc/hangar/modules/depth_hold.module
[Module]
Description=Depth hold controller
Exec=/usr/lib/hangar/depth_hold

[Subscribe nav.depth]
[Subscribe nav.imu]

[Publish ctl.depth_hold]
Type=wrench
Rate=50
Inputs=nav.depth nav.imu

# The vertical motion the controller wants, accel_z among it.
[Publish ctl.depth_hold.target]
Type=depth_target
Rate=50
Inputs=nav.depth
```

```ini
# /etc/hangar/modules/mux.module
[Module]
Description=Control source mux and envelope
Exec=/usr/lib/hangar/mux

[Subscribe pilot.wrench]
[Subscribe ctl.depth_hold]
[Subscribe policy.depth_ff.wrench]
[Subscribe policy.depth_ff.active]

[Publish ctl.wrench]
Type=wrench
Rate=50
# Each message references the sources it was computed from, at most 4.
Sources=pilot.wrench ctl.depth_hold policy.depth_ff.wrench

# Serving a method gives the module the implicit reply topic mux.rep.
[Serve set_mode]
Request=set_mode_req
Reply=set_mode_rep
Allow=topside:pilot_console admin topside:admin
```

```ini
# /etc/hangar/modules/thrusters.module
[Module]
Description=Thruster output, Navigator PWM
Exec=/usr/lib/hangar/navigator_pwm
User=hg-thrusters
SupplementaryGroups=pwm

[Subscribe ctl.wrench]
[Subscribe safety.state]

[Publish thr.cmd]
Type=thruster_cmd
Rate=100
Actuator=yes
Inputs=ctl.wrench

[Actuator]
Command=ctl.wrench
Neutral=/usr/lib/hangar/navigator_pwm --neutral
Groups=pwm

[Schedule]
Policy=fifo
Priority=60
CPUs=3
LockMemory=yes
```

The `depth` module reads a Bar30 on a bus of its own, and the `thrusters` module drives
the Navigator's PWM: in phase 1, while ArduSub drives the thrusters, the `ardusub`
module holds the Navigator's buses and GPIO alone (`[Module] ExclusiveGroups`, below),
and depth comes from ArduSub's telemetry ([10-mavlink](10-mavlink.md#telemetry)). These
two examples are for a separate bus, or for the later phase in which hangar drives the
thrusters itself.

## Sections and keys

### `[Module]` (required, once)

| Key | Default | |
|---|---|---|
| `Description` | `""` | One line, shown by `hangar top` |
| `Legacy` | `no` | A program that does not use hangar's library: no topics, calls or parameters; ready while it runs; restarted when it exits. Not allowed with an actuator output; one that drives hardware has a `Neutral` command instead ([11-electric-eye](11-electric-eye.md#legacy-modules)) |
| `Neutral` | — | Only with `Legacy=yes`, for a program that drives hardware (ArduSub): a command that puts that hardware to neutral by itself. The manager runs it whenever the module exits, as it runs an actuator's `[Actuator] Neutral` ([03-manager](03-manager.md#states)), and stops the module first in a shutdown. Same form as `Exec` |
| `ExclusiveGroups` | `""` | Only with `Legacy=yes` and a `Neutral`, for a program that drives hardware: the groups that give access to that hardware (its buses, its GPIO), each also one of its `SupplementaryGroups`. No other module may be in them, as for an actuator's `[Actuator] Groups`; a udev rule gives the devices to these groups alone |
| `Exec` | — (required) | The program and its arguments. The first word is an absolute path. Split on blanks; `"..."` keeps blanks inside one argument. No shell, no variables |
| `Environment` | — | `NAME=value`; repeatable |
| `User` | `hangar` | The user the module runs as. Required, and not shared with any other module, when the module publishes an actuator topic. Modules sharing a user are protected from each other only by being non-dumpable ([01-topics](01-topics.md#permissions)) |
| `Group` | `hangar` | The primary group. Every module is also in `hangar`, which can read every topic |
| `SupplementaryGroups` | `""` | For device access: `i2c gpio video` |
| `Capabilities` | none | Capabilities the module keeps after switching user, from the manager's own set only: `CAP_SYS_NICE` for a program that sets its own threads' priorities (ArduSub). Anything else is refused |
| `Privileged` | `no` | The module runs as root with the manager's full capability set; no `User`, `Group`, `SupplementaryGroups` or `Capabilities`. The manager accepts it for exactly one module per machine, the one `PrivilegedModule=` in `hangard.conf` names ([03-manager](03-manager.md#running-it)), and refuses it on any other. Such a module has no `[Publish]` other than its implicit `.rep`, no `[Subscribe]`, no `[Call]` and no `[Params]`, and serves only `apply` and `rollback`; the manager wires in one subscription for it, `safety.state`, so that it can refuse while the vehicle is not `disarmed`. It is the `installer` module ([13-deployment](13-deployment.md#upgrading-the-vehicle-over-the-tether)) |

Each module gets a state directory, `/var/lib/hangar/<name>`, created and owned by its
user, as its working directory.

### `[Publish <topic>]` (one per output)

| Key | Default | |
|---|---|---|
| `Type` | — (required) | A message type (`depth` for `msg/depth.msg`); `frame`: a frame type whose size and format the module decides when it opens the topic; or `dataset:<name>`: the row type of the dataset spec `/etc/hangar/datasets/<name>.dataset`, which the manager generates from the spec and its parts' types, as it generates the types of the implicit `<module>.req` and `<module>.rep` topics ([06-recorder](06-recorder.md#the-row), [04-calls](04-calls.md#the-two-topics)) |
| `Rate` | `0` | Declared rate in Hz. 0 is an event topic: no staleness check |
| `Slots` | 16 (4 for `frame`) | Ring depth, a power of two from 2 to 65536 |
| `Inputs` | `""` | Topics every message of this output is computed from (provenance), at most 4, each one of this module's subscriptions or another of its own outputs, or a topic of the other side written `<role>:<topic>` (`vehicle:cam.pilot`), which is not a subscription and which the module references itself with `hg_publish_with`. Every message references each of them |
| `Sources` | `""` | Topics a message of this output may be computed from, any number, each one of this module's subscriptions. The module names, per message, the ones it used (`hg_publish_with`). A message holds at most 4 references in all, its `Inputs` included ([01-topics](01-topics.md#provenance)) |
| `StaleAfter` | 3 periods | How long without a message before the topic is stale. Only with a rate |
| `Actuator` | `no` | The topic commands hardware. Requires a dedicated `User` and an `[Actuator]` section, makes `OnGiveUp=failsafe`, `StopTimeout=500ms` and an empty `Allow` the defaults ([`[Params]`](#params), [04-calls](04-calls.md#declaring-calls)), and puts the module first in a shutdown ([03-manager](03-manager.md#shutdown)) |

`Inputs` and `Sources` name topics, never globs. A `<role>:<topic>` entry is never
checked for staleness: that topic does not exist on this machine
([01-topics](01-topics.md#stale-inputs)). Only the references a message
actually records count for its stale flag ([01-topics](01-topics.md#stale-inputs)): a
source the module did not name in a message cannot flag it. `Inputs` are always
recorded, so for a module with `Inputs` alone every one of them counts.

### `[Subscribe <topic or glob>]` (one per input)

| Key | Default | |
|---|---|---|
| `Type` | the publisher's | The type this module expects. If given, it must match the publisher's declared type. Not allowed with a glob |

Subscribing declares the wiring; it does not order starts. A subscription to a topic
no enabled module publishes is allowed (a legacy electric-eye stream, a topic the
bridge receives, a module enabled later) and reported as a warning by `hangar check`.

**Glob subscriptions** (`[Subscribe nav.*]`) are for modules that read whatever
exists, such as the recorder. The wiring carries them as `subglob` lines
([03-manager](03-manager.md#wiring)), and the library opens any topic that matches,
including topics created after the module started: a mirrored topic, a module enabled
by a later reload. `hg_wait` does not wait on glob subscriptions but polls them, every
10 ms at most, so they do not count toward the 127 topics one wait can hold
([01-topics](01-topics.md#reading)); the implicit `sub` lines for `.req` and `.rep`
topics below do.

### `[Serve <method>]` and `[Call <module>.<method>]`

The methods a module answers, and the methods it calls; keys and rules in
[04-calls](04-calls.md#declaring-calls). A call is two messages on two topics, both
implicit outputs the manager adds ([04-calls](04-calls.md#the-two-topics)):

| Implicit topic | Added for | |
|---|---|---|
| `<module>.req` | a module with any `[Call]` | Its requests, written by the module, each naming the method it calls. Event topic, 16 slots; the payload is the 112-byte request envelope followed by the largest request type among its `[Call]`s |
| `<module>.rep` | a module with any `[Serve]`, or with `[Params]` | Its replies, written by the module. Event topic, 16 slots; the payload is the 96-byte reply envelope followed by the largest reply type among the methods it serves |

Their types are generated by the manager, as a dataset row's is
([`[Publish]`](#publish-topic-one-per-output)). From the `[Call]` and `[Serve]`
sections of every enabled manifest the manager also derives the subscriptions: a
server's wiring gets a `sub <caller>.req` line for each module on this machine that
calls one of its methods and `sub hangar.<role>.admin.req`, the manager's own request
topic ([04-calls](04-calls.md#admin-calls)), and a caller's wiring a `sub <server>.rep`
line for each module it calls ([03-manager](03-manager.md#wiring)). A server whose
method the bridge may carry from the other side also gets `subglob *.req`, since the
other side's callers are not known before the handshake
([08-bridge](08-bridge.md#calls-across-the-tether)). None of them is written in the
manifest. A server reads its callers' requests in order with `hg_next`, and a caller
reads the server's replies the same way, keeping those addressed to it; the named ones
are waited on in `hg_wait` like any subscription and count toward the 127.

```ini
# safety.module
[Call mavlink.arm]
```

```ini
# mavlink.module
[Serve arm]
Request=arm_req
Reply=arm_rep
Allow=safety
```

gives `safety` the output `safety.req` and the subscription `mavlink.rep`, and
`mavlink` the output `mavlink.rep` and the subscriptions `safety.req` and
`hangar.vehicle.admin.req`.

**The caller is the writer.** A request's caller is the module that writes the topic it
arrived on, which the manager guarantees: a topic has one writer, named in its header.
`Allow` lists name callers by the tokens of [04-calls](04-calls.md#allow-tokens):
module names, `<role>:<module>` for a module of the other side
(`topside:pilot_console`), `*` for every module on this machine, `<role>:*` for every
module of the other side, `admin` and `<role>:admin`. Every server reads
`hangar.<role>.admin.req`, the request topic the manager writes for `hangar call`
([04-calls](04-calls.md#admin-calls)), so an operator its list does not name is refused
`HG_E_DENIED` on its `.rep` topic, visibly, rather than left to time out. A call to a
method of the other side is declared with the types the caller was built with,
`Request=` and `Reply=` on the `[Call]`, since the server's manifest is not on this
machine ([04-calls](04-calls.md#declaring-calls)): the bridge mirrors the caller's
`.req` across and the server's `.rep` back where its configuration says so
([08-bridge](08-bridge.md#calls-across-the-tether)), and the mirrored topic's
`origin_boot` and the mirror's direction make the caller `<role>:<module>`.

`set_params` is a method like any other ([05-parameters](05-parameters.md#setting-parameters)):
declaring `[Params]` serves it, and a module that sets another module's parameters
declares `[Call <module>.set_params]`.

When it loads the manifests, the manager checks that every caller on this machine is
in the `Allow` list of each method it calls, and refuses the graph otherwise. A `[Call]`
to a method that no module enabled here serves, and that the bridge's `[Calls]` does not
route to the other side, is reported as a warning by `hangar check`; such a call
answers `HG_E_NOROUTE` ([04-calls](04-calls.md#status-codes)).

### `[Bridge]`

No keys. Marks the tether bridge, and gives it what no other module may do: read the
topics its configuration sends and publish the topics its configuration receives,
without declaring them, with any type and the sender's sequence numbers
([08-bridge](08-bridge.md#the-bridge-module)). A call across the tether is one more
pair of mirrored topics, the caller's `.req` one way and the server's `.rep` back
([08-bridge](08-bridge.md#calls-across-the-tether)); the bridge takes no other part in
it. The manager puts the configuration's mirrored names in the bridge's wiring, globs
as `subglob` lines: the topics of `safety` and `command` mirrors, and the `.req` and
`.rep` topics it sends for its `[Calls]`, are named, never globbed, so the bridge waits
on them by futex, and reads the topics its globs match on a 10 ms timer of its own
([01-topics](01-topics.md#reading)); what it receives for
`[Calls]` it takes by glob, `*.req *.rep`, since the other side's callers are not known
before the handshake. It also adds the
bridge's own outputs, `bridge.<role>.link` (rated), `bridge.<role>.budget` (rated, the
rate left for video: [08-bridge](08-bridge.md#video-and-the-tether)) and
`bridge.<role>.files`, and its method `send` with the `bridge.rep` that serving it
implies; `bridge.rep`, like the module's name, exists on both machines and is never
mirrored. At most one module per machine.

The topics the bridge receives keep the rate and staleness its `[Mirror]` sections give
them, for their readers, but count neither for the bridge's readiness nor against its
health: the bridge is judged by its own `bridge.<role>.link` and `bridge.<role>.budget`
and its `Watchdog`, never by the staleness of what arrives over the tether.

### `[Actuator]`

Required for, and only for, a module with an actuator output. Details in
[07-safety](07-safety.md#the-actuator-gate).

| Key | Default | |
|---|---|---|
| `Command` | — (required) | The topic the module turns into outputs; one of its subscriptions |
| `Neutral` | — (required) | A command that puts the hardware to neutral by itself; the manager runs it whenever the module exits ([03-manager](03-manager.md#states)) |
| `Groups` | `""` | The groups that reach the actuator's hardware; no other module may be in them |
| `CommandTimeout` | `200ms` | A command older than this is not acted on |
| `SafetyTimeout` | `300ms` | `safety.state` older than this counts as disarmed |

The gate reads `safety.state`, so an actuator module subscribes to it as well as to its
`Command` topic.

### `[Params]`

The module's run-time parameters: `Type` and `Allow`
([05-parameters](05-parameters.md#declaring-parameters)). `Allow` defaults to
`* admin`, every module on this machine and `admin`; it defaults to none for the `safety` and `mux` modules and for a
module with an actuator output, which name each caller they accept.

### `[Restart]`

| Key | Default | |
|---|---|---|
| `Policy` | `always` | `always`: after any exit or health failure. `on-failure`: not after exit status 0. `never` |
| `Backoff` | `0.1s..10s` | First delay and largest delay; the delay doubles on each failure |
| `ResetAfter` | `30s` | Healthy this long, and the delay and the burst count start over |
| `Burst` | `5/60s` | More failures than this within the period, and the manager gives up |
| `OnGiveUp` | `degrade` (`failsafe` with an actuator output) | `degrade`: the module stays down, the system is marked degraded. `failsafe`: the system enters its failsafe state ([07-safety](07-safety.md)) |
| `StopTimeout` | `5s` (`500ms` with an actuator output or a `[Module] Neutral`) | After `SIGTERM`, how long before `SIGKILL` |

### `[Health]`

| Key | Default | |
|---|---|---|
| `StartGrace` | `5s` | Time after start before staleness counts against the module |
| `StaleGrace` | `1s` | How long a topic may stay stale before the module is restarted |
| `Watchdog` | off | For modules with only event topics, and for the bridge beside its rated `bridge.<role>.link` and `bridge.<role>.budget`: the module must call `hg_alive()` at least this often |

### `[Schedule]` (optional)

| Key | Default | |
|---|---|---|
| `Policy` | `other` | `other`, `fifo` or `rr` |
| `Priority` | — | 1–99 for `fifo` and `rr` |
| `Nice` | `0` | For `other` |
| `CPUs` | all | A list or ranges: `3`, `2-3`, `0,2` |
| `LockMemory` | `no` | Lock the module's memory (`mlockall`, by the library at `hg_init`), so a page fault cannot stall it. For a `Legacy` module the manager only leaves `RLIMIT_MEMLOCK` raised; the program locks its own memory |

## How the manager uses manifests

**Everything starts at once.** There is no start ordering. A module whose inputs do
not exist yet starts anyway: its subscriptions read `HG_ABSENT` until their topics
appear, and the outputs that reference them are flagged as computed from missing
inputs meanwhile ([01-topics](01-topics.md#stale-inputs)). Boot takes as long as the
slowest module, no ordering can be configured wrongly, and one missing module upstream
does not take down the modules that depend on it.

**Ready is having published.** A module is ready once it has published at least once
on every output with a rate; event outputs, the implicit `<module>.req`,
`<module>.rep` and `<module>.params`, and the topics the bridge receives do not count,
since they may stay silent for a whole dive. A module with no rated output calls `hg_ready()`. There is
no other notification protocol.

**Staleness is the watchdog.** A topic with a rate that stays stale for `StaleGrace`,
after `StartGrace`, makes its module unhealthy, and the manager restarts it under its
`[Restart]` rules. The topics the bridge receives are the exception
([`[Bridge]`](#bridge)). `Watchdog` covers modules with nothing periodic to publish.

**The graph is checked as a whole.** The manager refuses a graph in which:

- two modules publish the same topic;
- two topic names have the same topic id;
- an `Inputs` entry is neither one of the module's subscriptions, nor another of its
  own outputs, nor `<role>:<topic>` with the other machine's role, or there are more
  than 4;
- a `Sources` entry is not one of the module's subscriptions;
- an `Inputs` or `Sources` entry names a topic the module subscribes to only by glob;
- a subscriber's `Type` differs from its publisher's;
- a `dataset:<name>` type names no valid spec in `/etc/hangar/datasets/`;
- a module with an actuator output shares its user with any other module, or runs as
  `hangar`;
- a module with an actuator output does not subscribe to its `[Actuator] Command` topic
  and to `safety.state`;
- a module other than an actuator is in a group listed in that actuator's
  `[Actuator] Groups` (the groups that reach the actuator's hardware,
  [07-safety](07-safety.md#the-actuator-gate));
- a module is in a group listed in another module's `[Module] ExclusiveGroups`, or a
  group listed there is not one of that module's own `SupplementaryGroups`;
- a `Legacy` module declares topics, calls or parameters, or a module that is not
  `Legacy` has a `[Module] Neutral` or `[Module] ExclusiveGroups`, or a `Legacy` module
  has `ExclusiveGroups` without a `Neutral`;
- a module is named `hangar`;
- a module has `Privileged=yes` and is not the one `PrivilegedModule=` in
  `hangard.conf` names, or two manifests have it; or a `Privileged` module names a
  `User`, `Group`, `SupplementaryGroups` or `Capabilities`, has a `[Publish]`, a
  `[Subscribe]`, a `[Call]` or a `[Params]`, or serves a method other than `apply` and
  `rollback` ([`[Module]`](#module-required-once));
- a module declares a topic whose name starts with `hangar.` or ends in `.req`, `.rep`
  or `.params`, or a method named `set_params`: these are reserved for the manager and
  for the implicit outputs ([04-calls](04-calls.md#the-two-topics),
  [05-parameters](05-parameters.md#declaring-parameters));
- a module subscribes by name to more than 127 topics, what one wait can hold, the
  implicit `.req` and `.rep` subscriptions counted ([01-topics](01-topics.md#reading));
  glob subscriptions do not count;
- a caller calls a method whose `Allow` list does not name it;
- a `[Call]` to a method no module enabled here serves lacks `Request=` or `Reply=`,
  or one to a method served here names types other than the server's
  ([04-calls](04-calls.md#declaring-calls));
- a module runs `/usr/lib/hangar/hangar-policy` under a name other than
  `policy_<Name>`, `Name` being its policy spec's, or does not publish
  `policy.<Name>.wrench`, `.active` and `.load`
  ([09-learning](09-learning.md#policy-modules)).

An entry of the bridge configuration's `[Calls] Remote=` stands for the `[Mirror]`
entries that carry a call across: the caller's `.req` one way and the server's `.rep`
back ([08-bridge](08-bridge.md#calls-across-the-tether)). An entry that matches a
module enabled on this machine mirrors nothing here: the call stays local. A remote
method is always one of the other side's; with the same `bridge.conf` on both machines,
`safety.*` is remote on the topside only. A module enabled on both machines is found at
the handshake and reported.

A refused module is not started, and `hangar check` says why. When the refusal is a
conflict between two modules (two publishers of one topic), neither is started.

**The library takes its settings from the manager.** At start the manager hands each
module its validated wiring (its topics, types, slot counts, rates, provenance links)
over an inherited descriptor ([03-manager](03-manager.md)). The module's code names its
topics and types; slot counts, rates, inputs and sources come from the manifest and
cannot drift from it:

```c
hg_init(argc, argv);                                      // reads the wiring
hg_pub *p = hg_pub_open("nav.depth", &DEPTH_TYPE, NULL);  // slots, rate, inputs: manifest
```

A module opening a topic its manifest does not declare, and no glob subscription of
its own matches, gets an error. Run by hand outside the manager (`hangar run <name>`,
[12-tools](12-tools.md)), the same wiring comes from the manifest file directly.

## Reloading

`hangar reload` (or `SIGHUP` to the manager) reads the manifests again. Modules whose
manifest is unchanged keep running untouched; changed ones are restarted, removed ones
stopped, new ones started. A reload that produces a refused graph changes nothing and
says why.

## Tests

- Parser: an unknown section or key, a repeated key, a bad value: that manifest
  refused with file, line and reason; the others load.
- Graph: each refusal in the list above, staged alone, refused with its reason; a
  conflict between two publishers starts neither.
- `Sources`: a mux message naming one fresh source is not flagged while an unnamed
  source is stale or absent; naming a stale source flags it; a fifth reference is
  refused by the library.
- Glob subscriptions: a topic created after start, by a reload or by the bridge, is
  read; more than 127 matching topics are accepted.
- Calls: a module with a `[Call]` gets `<module>.req`, one with a `[Serve]` or
  `[Params]` gets `<module>.rep`, both with the generated types and 16 slots; the
  server's wiring holds `sub <caller>.req` for each local caller,
  `sub hangar.<role>.admin.req`, and `subglob *.req` when the bridge's `[Calls]` names
  one of its methods, and the caller's `sub <server>.rep`; a `[Call]` to a method of
  the other side refused without `Request=` and `Reply=`; a manifest declaring a topic
  ending in `.req` or `.rep` refused; the implicit subscriptions counted toward the 127, so a module at the limit
  is refused by one more `[Call]`.
- A `[Call]` to a method no enabled module serves and the bridge's `[Calls]` does not
  route is warned by `hangar check`; a `[Calls] Remote=` entry matching a local module
  mirrors nothing, and the call reaches the local module.
- `dataset:<name>`: the pairer opens its row topic with the manager's generated type,
  and a row type that does not match the spec is refused.
- Actuator defaults: `StopTimeout` 500 ms, `OnGiveUp=failsafe`, every call and
  parameter change denied unless listed.
- `Legacy` with `[Module] Neutral`: the command runs when the program is killed;
  `StopTimeout` defaults to 500 ms.
- `ExclusiveGroups`: a second module in one of the `ardusub` module's exclusive groups
  refused with both names; the key refused on a module that is not `Legacy`, or without
  a `Neutral`.
- `Privileged=yes`: accepted for the module `PrivilegedModule=` names and refused for
  any other, and for a second manifest with the key; refused with a `User`,
  `Capabilities`, `[Publish]`, `[Subscribe]`, `[Call]` or `[Params]`, or a `[Serve]`
  other than `apply` and `rollback`; the accepted module's wiring holds
  `sub safety.state`, its callers' request topics and `pub installer.rep`, nothing else.
- `Allow` tokens: `*` and `topside:*` parsed and passed to the wiring; an unknown token
  refused.
- Policy modules: a name other than `policy_<Name>`, or one of the three policy
  outputs missing, refused.
- `Inputs=vehicle:cam.pilot` on the topside accepted without a subscription; the same
  entry naming this machine's role refused.
- The bridge: its received topics going stale during a tether loss do not restart it.
