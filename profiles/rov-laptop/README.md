# profiles/rov-laptop: the laptop's half of the lab's video

What `tools/rov/lab.sh up --video` runs on the laptop: electric-eye's `hellion` and
`eeye` (hangar's `legacy/` builds, `builddir/hellion` and `builddir/eeye`) in placement A,
taking the camera's own H.264 that the vehicle's `encoder` sends to UDP 5610, running the
effects here, and sending the result to QGroundControl on 127.0.0.1:5600. Until hangar's
own topside video (docs/11 B2–B4) replaces them.

- `hellion-a-topside.json`: `legacy/hellion/examples/a-topside.json` without the
  `v4l2:/dev/video10` webcam sink (it needs the v4l2loopback module). `lab.sh` writes the
  control port (`@CONTROL_PORT@`, 8092 by default: 8080 is often taken on a laptop) and
  the path of eeye's configuration (`@EEYE_CONFIG@`), which hellion's page edits.
- `eeye-topside.json`: `legacy/eeye/eeye_config.json` reading the frame bus stream
  `camera` (`capture_source` `bus`) and writing no device. Its chain (blur, then sobel: an
  edge picture) is the example's; change the effects on hellion's page, or here before
  `up`.

`lab.sh` copies both to `~/.cache/hangar-lab/` for the session, so the page's edits do
not change the files in the repository.
