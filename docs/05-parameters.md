# Parameters

The settings of a module that can change while it runs: controller gains,
thresholds, an effect chain, camera controls. The manifest holds a module's structure
and changes only with a restart ([02-manifest](02-manifest.md)); parameters change
live, and every change is checked, applied whole, saved, published and recorded.

## The rules

- **A module's parameters are one typed, fixed-size struct**, declared as a `.msg`
  type with a default and an optional range for each field.
- **They change only through a call**, `<module>.set_params`, which the library serves
  for the module. No file is watched.
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
  the manager restart the module.

## Declaring parameters

```ini
# depth_hold.module
[Params]
Type=depth_hold_params
Allow=pilot_console
```

| `[Params]` key | Default | |
|---|---|---|
| `Type` | — (required) | The parameter type |
| `Allow` | every module; none if the module has an actuator output | Modules that may set parameters. `hangar-admin` users always may |

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
  switch (hg_wait(subs, n, 100)) {
  case HG_CALL: hg_serve(); break;   // set_params is applied here
  ...
  }
  if (hg_params_changed()) reset_integrator();
}
```

`hg_params_open` loads the saved values (or the defaults, when nothing is saved),
checks them, publishes them on `<module>.params`, and fills `P`. A change is validated
and applied inside `hg_serve()`, on the module's own thread, between two iterations of
its loop: the module never sees a half-applied struct, and needs no atomics or locks.
A module with several threads hands its threads a copy, as it would any other data.

## Setting parameters

**Request** (type `set_params:<type>`):

```
u64 base_version     # the version this change was based on
<the parameter struct>
```

**Reply:** `u64 version` (the version now in force) and `u8 restarting`.

The library, inside `hg_serve()`:

1. refuses a caller not allowed (`HG_E_DENIED`);
2. refuses a stale request: `base_version` is not the current version
   (`HG_E_REFUSED`, "stale: version is 18");
3. checks every field against its range (`HG_E_REFUSED`, "kp 12 above 10");
4. calls the module's validator with the candidate and the current values;
5. on acceptance: copies the candidate into the module's struct, increments the
   version, saves the file, publishes `<module>.params`, and replies `HG_OK`.

Clients do read, modify and set: they take the current values and version from the
topic, change what they want, and send the whole struct. `hangar param set
depth_hold kp=1.2` does exactly that, and repeats it up to three times if refused as
stale.

Like any call, every attempt, accepted or refused, is on `<module>.calls`, with the
caller and the full request.

## The parameter topic

`<module>.params`, an event topic of 16 slots:

```
u64  version
char changed_by[32]   # module name, "admin:<user>", or "saved" at start
<the parameter struct>
```

## Saved values

`/var/lib/hangar/<module>/params`, text, written to a temporary file and renamed into
place after each accepted change:

```
# hangar parameters: depth_hold_params
# version 17, set by pilot_console, 2026-11-06T14:03:11Z
kp = 1.2
ki = 0.1
max_thrust = 0.6
gains = 1, 1, 0.5
enabled = 1
frame = "ned"
rate_hz = 50
```

The file is not `fsync`ed: a sync on an SD card can take longer than a control loop
period, and the call runs on the module's loop. A change survives a crash of the
module at once, and a power cut once the filesystem commits it (5 s by default on
ext4); the recording shows every change regardless.

**Loading at start** is strict, like every file hangar reads:

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
`load` and `reset` on a stopped or failed module go through the manager, which checks
the ranges (it reads the type from `MsgDir`) and writes the file as the module's user,
from a short-lived child process. The module's own validator runs when it next starts.

## Restart-only fields

A field that cannot change while the module runs (a camera's resolution, an I2C bus,
a loop rate) is marked `@restart`. A change to one is checked like any other (ranges
and the module's validator), saved, published, and answered with `restarting = 1`;
then the library asks the manager to restart the module, which stops and starts it.
That restart is not counted as a failure. A change that touches both kinds of field
is applied by the restart.

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
- Two clients setting at once: one wins, the other is refused as stale and its retry
  succeeds on the new version.
- The module never observes a partially applied struct.
- Saved file: round trip; a missing field takes its default; an unknown field, a bad
  value, or a validator refusal stops the start with exit status 78, and the manager
  does not restart it.
- `@restart`: saved, published, module restarted, not counted as a failure.
- Every accepted and refused change appears on `<module>.calls`; every accepted one on
  `<module>.params`.
