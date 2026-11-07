# mavgen: the MAVLink header generator

`mavgen.py` turns the MAVLink XML definitions in this directory into
`modules/mavlink/mavlink_gen.h`: for each message `messages.txt` names, its id,
`CRC_EXTRA`, payload lengths, a C struct in wire order and pack/unpack functions over
byte arrays; for each enum named, `#define`s. The header is checked in; `make
check-mavgen` (part of `check`) regenerates it in memory and fails if it differs.
Nothing of pymavlink or libmavlink is used at build or run time: the XML is the source,
this generator reads it, and the framing, CRC and signing are written in
`modules/mavlink/mav.c` from the specification.

## The pinned definitions

The ArduPilot tree at tag `Sub-4.7.2` (commit `fb773567896014da007bca58358704d4a0d8516f`,
the SITL of `tools/sim/`) pins its `modules/mavlink` submodule
(`https://github.com/ArduPilot/mavlink`) at commit
**`288b907c384a892c8519bfe271682424b1e1a3a0`**. The files here are
`message_definitions/v1.0/{ardupilotmega,common,standard,minimal}.xml` of that commit,
fetched from `raw.githubusercontent.com` on 2026-11-07 and not edited:

| File | bytes | sha256 (first 16 hex) |
|---|---|---|
| `common.xml` | 599932 | `43b194d1d025d07d` |
| `ardupilotmega.xml` | 120716 | `46ad5a0a9a90b3b7` |
| `minimal.xml` | 46788 | `b3627df397e3354f` |
| `standard.xml` | 10304 | `1c92c50e0a4d4cc2` |

`ardupilotmega.xml` also includes `uAvionix.xml`, `icarous.xml`, `loweheiser.xml`,
`cubepilot.xml` and `csAirLink.xml`: dialects hangar does not use, not fetched; the
generator says so and skips them. Every message hangar uses is in `common.xml` or
`minimal.xml`.

**Licence.** `COPYING.mavlink` is the repository's `COPYING` at that commit: the
generator there is (L)GPL v3, and the message definitions, as embedded in a product
through generated code, fall under the MIT exception it states. hangar's own generator
and the header it writes are GPL-3.0-or-later like the module; the MIT notice is kept
in the header's first lines. The downloaded files are data: read by the generator,
never executed.

## What the generator implements

From the MAVLink serialization guide (https://mavlink.io/en/guide/serialization.html,
read 2026-11-07):

- **Field order on the wire**: fields sorted by native size, largest first, stably;
  extension fields (after `<extensions/>`) are not sorted and follow in XML order.
- **CRC_EXTRA**: the CRC-16/MCRF4XX of `"<MESSAGE_NAME> "`, then for each base field in
  wire order `"<type> "` and `"<name> "` (the type without its array suffix;
  `uint8_t_mavlink_version` counts as `uint8_t`), then one byte with the array length
  for an array; folded to `(crc & 0xff) ^ (crc >> 8)`.
- **Lengths**: `MAV_LEN_X` with extensions, `MAV_MIN_LEN_X` without; the sender trims
  trailing zero bytes (never the first), the receiver zero-fills.

The values the generator computes were checked against the reference C headers of
`mavlink/c_library_v2` (`MAVLINK_MSG_ID_*_CRC`, `_LEN`, `_MIN_LEN`) for every message in
`messages.txt`, and against pymavlink 2.4.50's `crc_extra` in a one-off run while this
was written; the byte-exact packets pymavlink produced in that run are the vectors of
`modules/mavlink/tests/test_mav_frame.c`. Neither pymavlink nor the C library is a
dependency of anything here.

## Running it

```sh
python3 -I tools/mavgen/mavgen.py --out modules/mavlink/mavlink_gen.h    # regenerate
python3 -I tools/mavgen/mavgen.py --check modules/mavlink/mavlink_gen.h  # what check-mavgen runs
```

`-I` runs the interpreter isolated, so that nothing beside the downloaded files is
imported by accident. Add a message or an enum to `messages.txt`, regenerate, commit
both.
