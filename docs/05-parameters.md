# Parameters

The settings of a module that can change while it runs: controller gains,
thresholds, an effect chain, camera controls. The manifest holds a module's structure
and changes only with a restart ([02-manifest](02-manifest.md)); parameters change
live, and every change is checked, applied whole, saved, published and recorded.

## The rules

- **A module's parameters are one typed, fixed-size struct**, declared as a `.msg`
  type with a default and an optional range for each field.
- **They change only through a call**, `<module>.set_params`, which the library serves
  for the module. A call is two messages on two topics ([04-calls](04-calls.md)): the
  request on the caller's `<caller>.req`, the reply on `<module>.rep`. No file is
  watched.
- **A change is a transaction.** Ranges are checked, then the module's own validator;
  accepted, the whole struct is swapped in at once; refused, nothing changes and the
  reply says why.
- **Changes cannot be lost to a race.** A request names the version it was based on;
  if another change came first, it is refused as stale.
- **The current values are a topic**, `<module>.params`, published at start and on
  every change, so the recorder holds the parameters in force at every moment.
- **Accepted values are saved** and loaded at the next start. Saved values that fail
  validation stop the module from starting, visibly.
- **Fields marked `@restart`** cannot be applied live: changing one saves it and has
  the manager restart the module. Until the new instance runs, the values in force
  are the old ones, and the topic says a restart is pending.

## Declaring parameters

```ini
# depth_hold.module
[Params]
Type=depth_hold_params
Allow=topside:pilot_console admin topside:admin
```

| `[Params]` key | Default | |
|---|---|---|
| `Type` | — (required) | The parameter type |
| `Allow` | `* admin`: every module on this machine, and `admin`; none for the `safety` and `mux` modules and for a module with an actuator output | Callers that may set parameters, as the tokens of [04-calls](04-calls.md#allow-tokens): `depth_hold`, `topside:pilot_console`, `*`, `topside:*`, `admin`, `topside:admin` |

Nothing in `Allow` is implicit. A caller is the writer of the request topic the request
arrived on, which the manager guarantees; nothing is asserted by the caller itself. An
operator (`hangar param`, as `admin:<user>`) passes only where the list names `admin`:
the `hangar` command asks the manager over the control socket, and the manager, the
one writer of `hangar.<role>.admin.req`, publishes the request there with the operator's
name in the envelope. A module or operator of the other side arrives on a mirrored
`.req` topic and passes only where the list names that module, `topside:*` or
`topside:admin`. The default list admits nobody from the other side.

Declaring `[Params]` gives the module the `set_params` method and the
`<module>.params` topic; both names are reserved and cannot be declared otherwise.
Reading parameters needs nothing: the topic is readable by every module.

## Parameter types

A parameter type is a `.msg` file whose fields may carry a default, a range and the
`@restart` mark:

```
# msg/depth_hold_params.msg
f32  kp = 1.5          [0, 10]   # N/m
f32  ki = 0.1          [0, 2]    # N/(m s)
f32  max_thrust = 0.6  [0, 1]    # fraction of full thrust
f32  gains[3] = 1, 1, 0.5        # one value applies to every element
u8   enabled = 0       [0, 1]
char frame[16] = "ned"
u32  rate_hz = 50      [10, 200] @restart
```

| Attribute | Written | |
|---|---|---|
| default | `= value`; for arrays a list, or one value for every element; for `char`, quoted text | Without one, 0 (or `""`) |
| range | `[min, max]`, inclusive; for arrays it applies to each element | Not for `char`. Checked by the library before the module sees a value |
| `@restart` | after the range | Changing it restarts the module ([Restart-only fields](#restart-only-fields)) |

These attributes are allowed in parameter types only. They are not part of the type
hash, which covers the layout alone (field types, names and order): changing a default
or a range does not make old clients incompatible. A UI can build its controls,
sliders with their limits included, from the schema in the topic's header.

## In the module

```c
static depth_hold_params P;   // the values in force; the module reads it freely

static int validate(const void *candidate, const void *current, char reason[64]) {
  const depth_hold_params *c = candidate;
  if (c->enabled && c->max_thrust == 0) {
    strcpy(reason, "enabled with max_thrust 0");
    return HG_E_REFUSED;
  }
  return HG_OK;
}

hg_params_open(&DEPTH_HOLD_PARAMS_TYPE, &P, validate, NULL);  // after hg_init

for (;;) {
  hg_wait(subs, n, 100);        // the set includes the callers' .req topics
  hg_serve();                   // reads them; set_params is applied here
  read_topics();
  if (hg_params_changed()) reset_integrator();
}
```

`hg_params_open` loads the saved values, checks them, publishes them on
`<module>.params`, and fills `P`. At the module's first start, when no file exists
yet, it takes the defaults and writes them as the saved file before it publishes
([Saved values](#saved-values)). The `.req` topics of the module's callers are in its
wiring, so `hg_wait` wakes on a request as on any message
([04-calls](04-calls.md#the-two-topics)). A change is validated and applied inside
`hg_serve()`, on the module's own thread, between two iterations of its loop: the
module never sees a half-applied struct, and needs no atomics or locks. A module with
several threads hands its threads a copy, as it would any other data.

## Setting parameters

**Request** (type `set_params:<type>`), published on the caller's `<caller>.req` topic
with method `set_params`:

```
u64 base_version     # the version this change was based on
<the parameter struct>
```

**Reply** (type `set_params_rep`), on `<module>.rep`: `u64 version` (the version now
in force, or with `restarting`, the version the new instance will start with) and
`u8 restarting`.

The library, inside `hg_serve()`, reading the request from the caller's ring as for
any call (a request past its deadline is dropped unexecuted,
[04-calls](04-calls.md#the-rules)):

1. refuses a caller not allowed (`HG_E_DENIED`): the caller is the writer of the
   request topic, or, on `hangar.<role>.admin.req`, the operator the manager named;
2. refuses any change while a restart is pending (`HG_E_REFUSED`, "restart pending",
   [Restart-only fields](#restart-only-fields));
3. refuses a stale request: `base_version` is not the current version
   (`HG_E_REFUSED`, "stale: version is 18");
4. checks every field against its range (`HG_E_REFUSED`, "kp 12 above 10");
5. calls the module's validator with the candidate and the current values;
6. on acceptance of a change that touches no `@restart` field: copies the candidate into the module's struct, increments the
   version, saves the file, publishes `<module>.params`, and replies `HG_OK`.

Clients do read, modify and set: they take the current values and version from the
topic, change what they want, and send the whole struct. `hangar param set
depth_hold kp=1.2` does exactly that, through the manager's `hangar.<role>.admin.req`,
and repeats it up to three times if refused as stale.

Like any call, every attempt, accepted or refused, is in the recording: the request,
with the full struct, on the caller's `.req` topic, and the reply, with its status and
reason, on `<module>.rep`, referencing the request message it answers. Nothing else is
written for it.

## The parameter topic

`<module>.params`, an event topic of 16 slots:

```
u64  version
char changed_by[48]   # the caller ("<module>", "admin:<user>", "<role>:<module>",
                      # "<role>:admin:<user>"), or "saved" at start
u8   restart_pending  # a change to a @restart field is saved; the values below
                      # are still those in force until the restart
u8   reserved[7]
<the parameter struct>
```

The struct is always the values in force. A message with `restart_pending` set has the
version and the struct of the values in force, and `changed_by` of the change that
asked for the restart.

## Saved values

`/var/lib/hangar/<module>/params`, text, written to a temporary file and renamed into
place after each accepted change. The module writes it at its first start too, with
the defaults, or with the values of the file installed with the module's template
when there is one, so that the file always exists once the module has run:

```
# hangar parameters: depth_hold_params
# version 17, set by topside:pilot_console, 2026-11-06T14:03:11Z
kp = 1.2
ki = 0.1
max_thrust = 0.6
gains = 1, 1, 0.5
enabled = 1
frame = "ned"
rate_hz = 50
```

The second line gives the version, which the module takes at start, and the caller.
The module does not `fsync` the file: a sync on an SD card can take longer than a
control loop period, and the call runs on the module's loop. Since the file exists
from the first start on, every save replaces an existing file, and ext4 (with its
default `auto_da_alloc`) writes the new file's data before it commits the rename: after
a power cut the file holds either the old values or the new ones, never an empty or
partial file. The first write, at the first start, replaces nothing and is not
covered: a power cut just after it can leave an empty file, which loading refuses
(below). A change survives a crash of the module at once, and a power cut once the
filesystem commits it (5 s by default on ext4); the recording shows every change
regardless.

**Loading at start** is strict, like every file hangar reads:

- no file at all is the first start: the defaults are taken and saved (above);
- a file without its header line (`# hangar parameters: <type>`, with the module's
  parameter type), an empty file included, stops the start: it is never read as all
  defaults;
- a field missing from the file takes its default, and the log says so (a field added
  by an update);
- an unknown field, a value that does not parse, a value out of range, or a refusal
  by the module's validator stops the start.

A module whose saved parameters are refused exits with status 78 (`EX_CONFIG`) and the
reason as its status line. The manager does not restart it: it is `failed`, with the
reason, until the parameters are corrected (below) and the module restarted
([03-manager](03-manager.md#states)). Falling back to defaults silently would give a
vehicle gains nobody chose.

**Correcting the parameters of a module that is not running.** `hangar param set`,
`load` and `reset` on a stopped or failed module go through the manager
(`param-write`, [03-manager](03-manager.md#control-socket)), which checks the ranges
(it reads the type from `MsgDir`) and writes the file as the module's user, from a
short-lived child process, with the saved version plus one and the operator as
`admin:<user>`. That child does `fsync` the file, since no control loop waits on it.
The module's own validator runs when it next starts, and it publishes that version,
`changed_by` `saved`.

## Restart-only fields

A field that cannot change while the module runs (a camera's resolution, an I2C bus,
a loop rate) is marked `@restart`. A change that touches one, alone or with other
fields, is checked like any other (ranges and the module's validator) and saved, but
not applied and not published as in force: the module keeps running on the old
values. The library publishes `<module>.params` again with `restart_pending` set (the
version and the struct still those in force), answers `restarting = 1`, and asks the
manager to restart the module (`restart-self`), repeating the request until the
manager answers, so that a request made while the manager restarts is not lost; the
manager stops and starts the module. That restart is not counted as a failure.

While a restart is pending, every further `set_params` is refused ("restart pending"):
a second change would be based on values that are not in force. The new instance
loads the saved file and publishes the new values at start, with the saved version
and with `changed_by` naming the caller that asked for the change (from the file's
second line), not `saved`. A field marked `@restart` therefore never appears in
`<module>.params` with a value the running module is not using.

## The `hangar param` command

| Command | |
|---|---|
| `hangar param get <module> [field ...]` | from the topic |
| `hangar param set <module> field=value ...` | read, modify, set; retried when stale |
| `hangar param load <module> <file>` | a whole saved file, set as one change |
| `hangar param reset <module> [field ...]` | back to the defaults |

## Tests

- Ranges, the validator, and stale versions each refuse, and the values in force do not
  change.
- `Allow`: `admin` from a shell, arriving on `hangar.<role>.admin.req`, allowed only
  where listed, and the operator's name honoured on no other topic;
  `topside:pilot_console` allowed only from a mirrored `.req` topic and only where
  listed; a local module named `pilot_console` never matches it; the `safety`, `mux`
  and actuator modules refuse every caller they do not list.
- A `set_params` request published while the module restarts is applied by the new
  instance if its deadline has not passed, and dropped unexecuted otherwise.
- `param-write` on a stopped module: the file is synced, its version one above the
  saved one, and the module starts with it.
- Two clients setting at once: one wins, the other is refused as stale and its retry
  succeeds on the new version.
- The module never observes a partially applied struct.
- Saved file: round trip; a missing field takes its default; an unknown field, a bad
  value, or a validator refusal stops the start with exit status 78, and the manager
  does not restart it.
- `@restart`: the change saved and not published as in force; `restart_pending` set
  with the old values; a further `set_params` refused until the new instance runs; the
  new instance publishes the new values with `changed_by` naming the caller; the
  restart not counted as a failure.
- First start: the file written with the defaults (or the template's values) before
  the first publish; a later save replaces it.
- A parameters file without its header line, or empty (as a power cut during a first
  write could leave it): the start refused with exit status 78, never loaded as
  defaults.
- Every accepted and refused change appears as a request on the caller's `.req` topic
  and a reply on `<module>.rep` that references it; every accepted one on
  `<module>.params`.
