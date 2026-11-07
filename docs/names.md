# Names

Every component has two names. The **module identifier** is the plain functional word
written into manifests, topic names, `Allow` lists, recordings and `hangar check`
output; it never changes, because recordings and datasets name topics by it. The
**program name** is what the binary, the package and the paper call the component, and
what `hangar top` shows beside the identifier. Programs are named after songs, as
electric-eye and hellion were (Judas Priest) and hangar is (Megadeth, *Rust in Peace*).

| Module identifier | Program | Named for | Does |
|---|---|---|---|
| `console` | **Dawn Patrol** (`dawn-patrol`) | Megadeth, *Rust in Peace* | the operator's page: every managed process, topic and the recent log; later the pilot's controls ([12-tools](12-tools.md), [11-electric-eye](11-electric-eye.md#pilot_console)) |
| `hangard` | hangar | Megadeth, "Hangar 18" | the manager |

Unnamed as yet: `safety`, `mux`, `bridge`, `recorder`, `pairer`, `camera`, `vision`,
`encoder`, `video_relay`, `mavlink`, `leak`, `installer`, the policies and the
simulators. `eeye` and `hellion` keep their names until phase B retires them.

A manifest joins the two:

```ini
# /etc/hangar/modules/console.module
[Module]
Description=Dawn Patrol: the operator's page
Exec=/usr/lib/hangar/dawn-patrol
```
