# Recording a flight of the development demo

What `recorder.module` and `recorder.conf` in this profile do under the vehicle's
manager, and how to see it. Every command below was run, as written, under a
throwaway root laid out from this profile by `tools/sim/hangar-up.sh`'s rules
(`lay_out`, the same `sed` rewrites), with these differences from the live demo:
no `mavlink.module` (it binds 14560) and no `console.module` (8080); the toy
`talker` of `hangard/tests/modules` as a `depth` module publishing `nav.depth` at
50 Hz in the mavlink module's place; `safety.params` with `required = "mux"` and
`battery_required = 0`, and `[Call mavlink.arm]` dropped from `safety.module`, as
its own comment says to do without a mavlink module, so that `hangar call
safety.arm` leads to `armed` at once. On the live demo the same commands run with
`HANGAR_ROOT=/dev/shm/hangar-vehicle`, and arming is QGroundControl's or
`builddir/pilot arm`; the session then also holds `nav.attitude`, `nav.imu`,
`thr.out`, `power.battery`, `ardusub.*`, `mavlink.rep`, and the topside's
`pilot.wrench`, `pilot.buttons` and `topside_mavlink.req` through the shim.

## The recorder under the manager

```
$ HANGAR_ROOT=$R builddir/hangar status
system   nominal  (for 3s)
MODULE               STATE         PID FAILS LAST EXIT   SINCE  STATUS
depth                ready        9495     0 -              2s  publishing
mux                  ready        9496     0 -              2s  manual: sources 000000 limits 0
recorder             ready        9497     0 -              2s  idle 0 bytes, 14 topics, 0 gaps
safety               ready        9498     0 -              2s  disarmed: start

$ cat $R/run/hangar/log/recorder.log
recorder: no group hangar-rec on this machine: the group of .../var/lib/hangar/recorder/sessions is not checked
recorder: recording to .../var/lib/hangar/recorder/sessions; black box 60s in 32MB; AutoStart=armed
recorder: mux.rep: full, black box high
recorder: mux.state: full, black box high
recorder: ctl.wrench: full, black box normal
recorder: mux.params: full, black box high
recorder: nav.depth: full, black box normal
recorder: safety.rep: full, black box high
recorder: safety.state: full, black box high
recorder: safety.params: full, black box high
recorder: hangar.vehicle.admin.req: full, black box high
recorder: hangar.vehicle.modules: full, black box normal
recorder: hangar.vehicle.system: full, black box normal
recorder: recorder.rep: full, black box high
recorder: recorder.session: full, black box high
recorder: recorder.status: full, black box high
```

The manager accepted the manifest as the profile writes it (`[Subscribe *]`, the
three `Serve` sections with `Allow=admin topside:admin topside:console`,
`SupplementaryGroups=hangar-rec`, `Watchdog=10s`); the recorder made `Dir` itself,
mode 2770, and, with no `hangar-rec` group on the machine, checked the mode alone.

## A session by arming

```
$ hangar echo recorder.status --count 1 --fields state,topics,blackbox_ns
state                0    # 0 idle, 1 recording, 2 dumping the black box
topics               14   # topics being recorded
blackbox_ns          14001923765

$ hangar call safety.arm
HG_OK

$ hangar echo safety.state --count 1 --fields state,by
state                1    # 0 disarmed, 1 armed, 2 failsafe, 3 arming
by                   "admin:oguzb"

$ hangar echo recorder.session --count 1
active               1    # 1 while the session records
id                   "20261108-133945-mousb"
path                 ".../var/lib/hangar/recorder/sessions/20261108-133945-mousb"
started_ns           796375933867

(5 s later)
$ hangar status
recorder             ready        9497     0 -             53s  recording 20261108-133945-mousb 188976 bytes, 14 topics, 0 gaps
safety               ready        9498     0 -             53s  armed by admin:oguzb

$ hangar call safety.disarm
HG_OK

$ hangar echo recorder.session --count 1 --fields active,id
active               0
id                   "20261108-133945-mousb"
```

## The session directory

```
$ ls $R/var/lib/hangar/recorder/sessions/20261108-133945-mousb
ctl.wrench.hgr                 mux.params.hgr      safety.params.hgr
hangar.clock.hgr               mux.state.hgr       safety.rep.hgr
hangar.vehicle.admin.req.hgr   nav.depth.hgr       safety.state.hgr
hangar.vehicle.modules.hgr     recorder.session.hgr   session.ini
hangar.vehicle.system.hgr      recorder.status.hgr    video/  weights/
manifests/                     msg/

$ ls manifests msg
manifests: depth.module  mux.module  recorder.module  safety.module
msg: 33 files, the root's usr/share/hangar/msg

$ cat session.ini
# session.ini: written by the recorder (docs/06-recorder.md, Sessions)
[Session]
Id=20261108-133945-mousb
BootId=c5a8bc2f077c4a3aa90002655c85c856
Host=mousb
Recorder=hangar recorder (modules/recorder)
StartedBy=autostart:armed
Note=
StartedMonotonicNs=796375933867
StartedRealNs=1794145185443864161
StartedUtc=20261108-133945
StoppedBy=autostart:disarmed
StoppedMonotonicNs=802375352275
StoppedRealNs=1794145191443872640
StoppedUtc=20261108-133951
Records=758
Bytes=195984
WriteErrors=0
```

Read back with `python/hangar/record.py` (standard library alone; `numpy` is not
installed here, so `Recording.array()` was not used):

```
$ python3 -c 'import sys, os; sys.path.insert(0, "python"); from hangar import record
for f in sorted(os.listdir(S)):
    if f.endswith(".hgr"):
        r = record.open_recording(os.path.join(S, f)); n = len(r)
        print(f, n, r.header.policy_name, r[0].stamp_ns, r[n-1].stamp_ns, sum(1 for x in r if x.flags & record.REC_GAP))'
topic file                       records policy first stamp_ns       last stamp_ns        gaps
ctl.wrench.hgr                       300 full   796382118807         802362121234         0
hangar.clock.hgr                       7 full   796376086815         802125278296         0
hangar.vehicle.admin.req.hgr           1 full   796350602522         796350602522         0
hangar.vehicle.modules.hgr            24 full   797207952550         802208006890         0
hangar.vehicle.system.hgr              6 full   797207952550         802208006890         0
mux.params.hgr                         1 full   749122652684         749122652684         0
mux.state.hgr                         54 full   796462118226         802322121542         0
nav.depth.hgr                        297 full   796393189881         802365313046         0
recorder.session.hgr                   1 full   796376094149         796376094149         0
recorder.status.hgr                    8 full   796125282653         802125285319         0
safety.params.hgr                      1 full   749103490116         749103490116         0
safety.rep.hgr                         2 full   796350694926         802357415600         0
safety.state.hgr                      62 full   796350692371         802357409428         0
13 files, 764 records
```

Six seconds armed: 300 `ctl.wrench` and 297 `nav.depth` at 50 Hz, 54 `mux.state`
and 62 `safety.state` at 10 Hz, 6 `hangar.vehicle.system`, 24
`hangar.vehicle.modules` (one per module per second), the clock once a second, no
gaps. The first records are the newest event messages at the start: the `arm`
request on `hangar.vehicle.admin.req`, the parameters in force (`mux.params`,
`safety.params`, stamped long before the session), the `arm` reply, and the
`safety.state` that armed (stamp 796350692371, before `StartedMonotonicNs`); the
last `safety.state` is the one that disarmed and stopped it. `session.ini`'s
`Records=758` counts what the drain loop handed the writer; the 764 above include
the clock records the writer makes itself.

## A session by call, and the black box

```
$ hangar call recorder.start note="by hand, from RECORDING.md"
HG_OK
$ hangar call recorder.stop
HG_OK
$ grep -E '^(Id|StartedBy|StoppedBy|Note|Records)' .../20261108-133952-mousb/session.ini
Id=20261108-133952-mousb
StartedBy=call:admin:oguzb
Note=by hand, from RECORDING.md
StoppedBy=call:admin:oguzb
Records=387

$ hangar call recorder.dump
HG_OK
$ ls .../sessions/blackbox-20261108-133955-1
ctl.wrench.hgr  hangar.vehicle.admin.req.hgr  hangar.vehicle.modules.hgr  hangar.vehicle.system.hgr
mux.params.hgr  mux.state.hgr  nav.depth.hgr  recorder.rep.hgr  recorder.session.hgr
recorder.status.hgr  safety.params.hgr  safety.rep.hgr  safety.state.hgr  session.ini
$ head -12 session.ini
# session.ini: a black-box dump (docs/06-recorder.md, The black box)
[Session]
Id=blackbox-20261108-133955-1
Kind=blackbox
BootId=c5a8bc2f077c4a3aa90002655c85c856
Host=mousb
Reason=call:admin:oguzb
DumpedMonotonicNs=806403765789
DumpedRealNs=1794145195472287787
Session=
[BlackBox]
mux.state=57199992418
(one <topic>=<reach ns> line per topic follows)
```

The dump holds the last 57 s of the manager's 57 s of life: 2862 `ctl.wrench`,
2849 `nav.depth`, 576 `safety.state`, 60 `hangar.vehicle.modules`, every request
and reply so far (`recorder.rep`, `safety.rep`, three `hangar.vehicle.admin.req`),
13 files, 6948 records, 0 gaps.

## Shutdown

```
$ kill -TERM $(cat $R/hangard.pid)
hangard: signal 15: shutting down
hangard: shutting down: modules with a Neutral command first
hangard: depth: stopping: shutdown
hangard: mux: stopping: shutdown
hangard: recorder: stopping: shutdown
hangard: safety: stopping: shutdown
hangard: depth: stopped: pid 9495 exited with status 0
hangard: mux: stopped: pid 9496 exited with status 0
hangard: safety: stopped: pid 9498 exited with status 0
hangard: recorder: stopped: pid 9497 exited with status 0
hangard: stopped
$ tail -1 $R/run/hangar/log/recorder.log
recorder: stopped
```

## On the live demo

`tools/sim/hangar-up.sh up` lays the recorder out with the rest of the profile
(`recorder.module` through the `.module` rewrite, `recorder.conf` through the
`.conf` rewrite, which turns `Dir=/dev/shm/hangar-vehicle/...` into the vehicle
root's `var/lib/hangar/recorder/sessions`, as it does `mavlink.conf`'s `Tlog`).
Arm in QGroundControl, or `builddir/pilot arm hold 5 disarm`, and a session
appears under `/dev/shm/hangar-vehicle/var/lib/hangar/recorder/sessions/`;
`HANGAR_ROOT=/dev/shm/hangar-vehicle builddir/hangar echo recorder.status` while
it records. Through the same-host shim the glob also attaches the topside root's
topics (`pilot.wrench`, `pilot.buttons`, `topside_mavlink.req`,
`hangar.topside.*`), so they are in the session too; with the bridge they will be
the mirrored topics docs/06 describes.
