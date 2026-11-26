# vision: the pilot's picture

`hangar-vision` is eeye's effects thread as a hangar module (docs/11-electric-eye.md
"vision", stage B3). It reads a frame topic, runs lib/vision's visibility correction
and operator chain on its own thread pool, and publishes `cam.pilot`, each frame
referencing the frame it was made from.

    hangar-vision [vision.conf]

| Placement | Input | Output |
| --- | --- | --- |
| topside, camera H.264 (the default) | `cam.decoded` (video_relay) | `cam.pilot` |
| vehicle, placement B | `cam.raw` (camera) | `cam.pilot` |

To move it, change `[Subscribe]`, `Inputs=` in `vision.module` and `Input=` in
`vision.conf` together.

## How it runs

One thread reads and publishes; the pool's threads (`lib/hg/hg_pool`, no OpenMP)
run the operators' loops and touch no topic. The wait set is the input topic and the
call topics. At every wake the input is read with `hg_latest` (the newest frame; an
older one still waiting is skipped and counted, never queued) and calls are served.
Nothing in the wait set stays unread (hangar#1). At rest, with no frames, it
measured 0.0 % of a core.

Each frame: the header is checked (`hg_frame_check`: I422, the size the first frame
fixed, every plane inside the payload; a frame that fails is dropped), the planes
are copied into a `VideoFrame`, the image statistics computed
when the chain wants auto visibility, `apply_effect_chain_auto` run, and the result
published with the input's capture stamp. When the chain ends in grayscale or an
edge map (`chroma_neutral`), the chroma planes are written as 128 by `memset`
instead of copied (the frame bus's shortcut).

The status line, once a second:
`edges (blur+sobel): 1234 frames, 3 skipped, 2.10 ms a frame (max 3.40), 12 threads`.

## The chain: parameters

`vision_params` (msg/vision_params.msg) holds the chain as eight fixed stages
(`stage_count`, `effect[8]` and each operator's settings per stage), `auto_visibility`,
`threads` (`@restart`; 0 = one per core) and `preset`.

`preset` picks the chain: a preset's name means that preset's chain, and the stage
arrays are not used. `custom` means the arrays as given. One `set_params` therefore
switches the whole chain atomically, which is how the Fly view's buttons work. A name
that is neither a preset nor `custom` is refused by the validator
(`no preset nosuch; the presets are normal auto edges ...`), and so are an effect
above 10 and `stage_count` above 8. The values in force stay as they were.

    hangar param set vision preset=edges
    hangar param set vision preset=custom stage_count=2 effect=6,5,0,0,0,0,0,0 \
        blur_strength=3,1,1,1,1,1,1,1 sobel_threshold=0,40,0,0,0,0,0,0

## Presets (issue #2)

| Key | Preset | Chain |
| --- | --- | --- |
| 1 | `normal` | nothing: the picture passes through |
| 2 | `auto` | the visibility correction alone |
| 3 | `edges` | `blur:strength=2 sobel:threshold=16` |
| 4 | `sharpen` | `contrast` (see deviation 1) |
| 5 | `lowlight` | visibility correction, then `light:level=176` |

`vision.conf` can redefine these presets or add its own, up to 16 in all:

    [Preset murky]
    Auto=yes
    Chain=contrast light:level=160

Chain grammar: operators separated by spaces, settings after a colon, comma-separated,
for example `op:k=v,k=v`. The operators are `grayscale`, `invert`, `threshold(value)`,
`tint(u,v,strength)`, `sobel(threshold)`, `blur(strength)`, `contrast`,
`light(level)`, `log(strength,threshold)` and `canny(strength,low,high)`. A chain can
have at most 8 stages. A preset name is `a-z 0-9 _ -` and cannot be `custom`.

## Tests

`make check-vision` (about 10 s). Pure functions first: the built-in presets, the
grammar, conf presets and refusals, and the effective chain from the parameters.
Then the module runs under a manager, with the suite publishing `cam.decoded` at
1280×720 I422:

- For each preset, and then a custom chain, 10 frames. Every `cam.pilot` frame is
  byte-identical to lib/vision's own result on the same frame, chain and capture
  stamp (0 bytes differ). Each frame references its `cam.decoded` frame's seq, and
  the capture stamp goes through.
- Refusals: an unknown preset, an effect of 11 and `stage_count=9`. After them the
  custom chain is still in force.
- `threads=2` restarts the module. The status line then says 2 threads and the
  output is still identical.
- A header that lies: a last plane starting 16 bytes before the payload's end, and a
  stride of 2³¹. Neither is processed (`hg_frame_check`, docs/01 "Frames"; the log
  says so once), the module is the same process afterwards, and the frames after
  them are identical again. Without the check the module died on the first.
- At rest: 0.0 % of a core.

`make check-asan-modules` runs this suite, module and manager included, under
AddressSanitizer and UBSan.

Measured on the development machine (12 threads, 1280×720), from publish to
`cam.pilot`, median of 10 frames: normal 1.5 ms, edges 3.7 ms, sharpen 2.4 ms,
auto 2.5 ms, lowlight 2.5 ms. With 2 threads, blur+sobel took 5.6 ms. The module's
own measure for blur+sobel at 12 threads was 1.64 ms a frame (max 2.85). Not
measured on the Pi or the topside laptop.

The console's suite (modules/console) runs the real module too, for the Fly view's
preset call.

## Deviations

1. **Sharpen is a contrast stretch.** lib/vision has no sharpening kernel. The
   `sharpen` preset is `contrast` until there is one; redefine it in vision.conf
   if that serves better.
2. **Provenance is implicit.** `Inputs=cam.decoded` is declared, and `hg_publish`
   takes the reference from the last read (`hg_latest`). The module does not
   pass it explicitly.
3. **The frame size is fixed by the first frame**, as it is for `cam.decoded`. A
   frame of another size is dropped, and this is logged once. A replaced writer
   with another size is re-attached.
4. **The newest frame only.** When processing falls behind, older frames are
   skipped (counted in the status line), never queued.
5. **A saved preset vision.conf no longer defines** makes the picture pass
   through, and this is logged. It stays that way until a `set_params` names a
   preset that exists.
