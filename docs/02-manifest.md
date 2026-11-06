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
The vehicle and the topside differ only in which modules are enabled; there is no
separate notion of a profile or role.

The module's **name** is the file name without `.module`: `[a-z0-9_]`, 1 to 31
bytes. The manager reports and logs the module by that name.

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
- Blank space around `=` and at the ends of a value is ignored.

| Value | Written as |
|---|---|
| boolean | `yes` / `no` |
| duration | a number with `ms`, `s` or `min`: `100ms`, `2.5s`, `1min` |
| rate | hertz, decimals allowed: `50`, `0.5` |
| duration range | `0.1s..10s` |
| burst | `<count>/<duration>`: `5/60s` |

## Example

```ini
# /etc/hangar/modules/depth.module
[Module]
Description=Bar30 depth and temperature
Exec=/usr/lib/hangar/bar30 --i2c /dev/i2c-1

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
```

```ini
# /etc/hangar/modules/thrusters.module
[Module]
Description=Thruster output, Navigator PWM
Exec=/usr/lib/hangar/navigator_pwm
User=hg-thrusters

[Subscribe ctl.wrench]

[Publish thr.cmd]
Type=thruster_cmd
Rate=100
Actuator=yes
Inputs=ctl.wrench

[Schedule]
Policy=fifo
Priority=60
CPUs=3
LockMemory=yes
```

## Sections and keys

### `[Module]` (required, once)

| Key | Default | |
|---|---|---|
| `Description` | `""` | One line, shown by `hangar top` |
| `Legacy` | `no` | A program that does not use hangar's library: no topics, calls or parameters; ready while it runs; restarted when it exits. Not allowed with an actuator output ([11-electric-eye](11-electric-eye.md#legacy-modules)) |
| `Exec` | — (required) | The program and its arguments. The first word is an absolute path. Split on blanks; `"..."` keeps blanks inside one argument. No shell, no variables |
| `Environment` | — | `NAME=value`; repeatable |
| `User` | `hangar` | The user the module runs as. Required, and not shared with any other module, when the module publishes an actuator topic. Modules sharing a user are protected from each other only by being non-dumpable ([01-topics](01-topics.md#permissions)) |
| `Group` | `hangar` | The primary group. Every module is also in `hangar`, which can read every topic |
| `SupplementaryGroups` | `""` | For device access: `i2c gpio video` |
| `Capabilities` | none | Capabilities the module keeps after switching user, from the manager's own set only: `CAP_SYS_NICE` for a program that sets its own threads' priorities (ArduSub). Anything else is refused |

Each module gets a state directory, `/var/lib/hangar/<name>`, created and owned by its
user, as its working directory.

### `[Publish <topic>]` (one per output)

| Key | Default | |
|---|---|---|
| `Type` | — (required) | A message type (`depth` for `msg/depth.msg`), or `frame`: a frame type whose size and format the module decides when it opens the topic |
| `Rate` | `0` | Declared rate in Hz. 0 is an event topic: no staleness check |
| `Slots` | 16 (4 for `frame`) | Ring depth, a power of two from 2 to 65536 |
| `Inputs` | `""` | Topics this output is computed from (provenance), at most 4, each one of this module's subscriptions or another of its own outputs |
| `StaleAfter` | 3 periods | How long without a message before the topic is stale. Only with a rate |
| `Actuator` | `no` | The topic commands hardware. Requires a dedicated `User`, makes `OnGiveUp=failsafe` the default, and puts the module first in a shutdown ([03-manager](03-manager.md)) |

### `[Subscribe <topic>]` (one per input)

| Key | Default | |
|---|---|---|
| `Type` | the publisher's | The type this module expects. If given, it must match the publisher's declared type |

Subscribing declares the wiring; it does not order starts. A subscription to a topic
no enabled module publishes is allowed (a legacy electric-eye stream, a module enabled
later) and reported as a warning by `hangar check`.

### `[Serve <method>]` and `[Call <module>.<method>]`

The calls a module answers, and the calls it makes; keys and rules in
[04-calls](04-calls.md#declaring-calls).

### `[Bridge]`

No keys. Marks the tether bridge, and gives it what no other module may do: publish
the topics its configuration receives, with any type and the sender's sequence
numbers; serve stand-in sockets for remote methods; and name the remote caller of a
forwarded call ([08-bridge](08-bridge.md#the-bridge-module)). At most one module per
machine.

### `[Actuator]`

Required for, and only for, a module with an actuator output: the topic it turns into
outputs and the timeouts of its safety gate (`Command`, `CommandTimeout`,
`SafetyTimeout`; [07-safety](07-safety.md#the-actuator-gate)).

### `[Params]`

The module's run-time parameters: `Type` and `Allow`
([05-parameters](05-parameters.md#declaring-parameters)).

### `[Restart]`

| Key | Default | |
|---|---|---|
| `Policy` | `always` | `always`: after any exit or health failure. `on-failure`: not after exit status 0. `never` |
| `Backoff` | `0.1s..10s` | First delay and largest delay; the delay doubles on each failure |
| `ResetAfter` | `30s` | Healthy this long, and the delay and the burst count start over |
| `Burst` | `5/60s` | More failures than this within the period, and the manager gives up |
| `OnGiveUp` | `degrade` (`failsafe` with an actuator output) | `degrade`: the module stays down, the system is marked degraded. `failsafe`: the system enters its failsafe state ([07-safety](07-safety.md)) |
| `StopTimeout` | `5s` | After `SIGTERM`, how long before `SIGKILL` |

### `[Health]`

| Key | Default | |
|---|---|---|
| `StartGrace` | `5s` | Time after start before staleness counts against the module |
| `StaleGrace` | `1s` | How long a topic may stay stale before the module is restarted |
| `Watchdog` | off | For modules with only event topics: the module must call `hg_alive()` at least this often |

### `[Schedule]` (optional)

| Key | Default | |
|---|---|---|
| `Policy` | `other` | `other`, `fifo` or `rr` |
| `Priority` | — | 1–99 for `fifo` and `rr` |
| `Nice` | `0` | For `other` |
| `CPUs` | all | A list or ranges: `3`, `2-3`, `0,2` |
| `LockMemory` | `no` | Lock the module's memory (`mlockall`), so a page fault cannot stall it |

## How the manager uses manifests

**Everything starts at once.** There is no start ordering. A module whose inputs do
not exist yet starts anyway: its subscriptions read `HG_ABSENT` until their topics
appear, and its outputs are flagged as computed from missing inputs meanwhile
([01-topics](01-topics.md#stale-inputs)). Boot takes as long as the slowest module, no
ordering can be configured wrongly, and one missing module upstream does not take
down the modules that depend on it.

**Ready is having published.** A module is ready once it has published at least once
on every output with a rate; event outputs, and the implicit `<module>.calls` and
`<module>.params`, do not count, since they may stay silent for a whole dive. A module
with no rated output calls `hg_ready()`. There is
no other notification protocol.

**Staleness is the watchdog.** A topic with a rate that stays stale for `StaleGrace`,
after `StartGrace`, makes its module unhealthy, and the manager restarts it under its
`[Restart]` rules. `Watchdog` covers modules with nothing periodic to publish.

**The graph is checked as a whole.** The manager refuses a graph in which:

- two modules publish the same topic;
- two topic names have the same topic id;
- an `Inputs` entry is neither one of the module's subscriptions nor another of its
  own outputs, or there are more than 4;
- a subscriber's `Type` differs from its publisher's;
- a module with an actuator output shares its user with any other module, or runs as
  `hangar`;
- a module other than an actuator is in a group listed in that actuator's
  `[Actuator] Groups` (the groups that reach the actuator's hardware,
  [07-safety](07-safety.md#the-actuator-gate));
- a module is named `hangar`;
- a module declares a topic whose name starts with `hangar.` or ends in `.calls` or
  `.params`, or a method named `set_params`: these are reserved for the manager and for
  the implicit outputs ([04-calls](04-calls.md#the-call-record),
  [05-parameters](05-parameters.md#declaring-parameters));
- a module subscribes to more than 126 topics, what one wait can hold
  ([01-topics](01-topics.md#reading));
- a client calls a method whose `Allow` list does not name it.

A refused module is not started, and `hangar check` says why. When the refusal is a
conflict between two modules (two publishers of one topic), neither is started.

**The library takes its settings from the manager.** At start the manager hands each
module its validated wiring (its topics, types, slot counts, rates, provenance links)
over an inherited descriptor ([03-manager](03-manager.md)). The module's code names its
topics and types; slot counts, rates and inputs come from the manifest and cannot drift
from it:

```c
hg_init(argc, argv);                                      // reads the wiring
hg_pub *p = hg_pub_open("nav.depth", &DEPTH_TYPE, NULL);  // slots, rate, inputs: manifest
```

A module opening a topic its manifest does not declare gets an error. Run by hand
outside the manager (`hangar run <name>`, [12-tools](12-tools.md)), the same wiring
comes from the manifest file directly.

## Reloading

`hangar reload` (or `SIGHUP` to the manager) reads the manifests again. Modules whose
manifest is unchanged keep running untouched; changed ones are restarted, removed ones
stopped, new ones started. A reload that produces a refused graph changes nothing and
says why.
