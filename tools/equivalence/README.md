# Equivalence with electric-eye v0.3.1

`make equivalence` proves that the operators in `lib/vision` still produce, byte for
byte, what electric-eye v0.3.1 produced: `equiv.c` runs every operator over a fixed
input set, hashes each output (FNV-1a 64 over the Y, U and V planes), and the target
fails on any line that differs from `hashes-v0.3.1.txt`, printing the differing case
names. It is the paper's method ([docs/11-electric-eye.md](../../docs/11-electric-eye.md),
"Proving the port") made permanent, so that the phase B rewrite -- the module's own
thread pool, parameter structs in place of `Config` -- is held to the same bytes.

## What is hashed

The output's first lines spell the case set out, so a reference made from a different
set fails the diff before any hash is compared. In short:

| | |
|---|---|
| Sizes | 1280×720, 640×480, 65×48, 79×33, 31×9, 2×2, 1×1 (the small ones are the SIMD remainder tails and the degenerate cases) |
| Content | random (xorshift64, fixed seeds), flat, all-max, checkerboard, ramp |
| `gaussian_blur`, `gaussian_blur_luma` | strength 0..20 |
| `sobel_edges` | threshold 0..240 step 16, and 255 |
| `log_edges` | strength 0..4 × threshold 0..224 step 32, and 255 |
| `canny_edges` | strength 1..4 × low {0,20,40,80,120,160,200} × high {40,60,90,130,180,255} |
| `gs_contrast_normalize` | direct, and as a one-stage chain |
| point ops (through `apply_effect_chain`) | none, grayscale, invert; threshold and light 0..224 step 32 and 255; tint u, v in {0,64,128,192,255} × strength 0..224 step 32 and 255 |
| chains | 8 fixed multi-stage chains (LUT fusion, the raw/work/spare ping-pong, dead-chroma skipping), and one with auto visibility ahead of it |
| auto visibility | a 60-frame synthetic sequence per size and pattern, every frame hashed (it smooths over time, so each frame depends on the ones before) |

20615 cases; 1690 are `skip` -- canny (and the one chain holding it) refuses frames
smaller than 3×3, and a case prints `skip` where the operator wrote nothing. Output
buffers are poisoned (0xA5) before every case, so an operator that stops writing a
plane changes its hash instead of passing on stale memory. The run takes ~25 s at one
thread on a desktop (gcc -O2); the three thread counts together ~50 s.

With OpenMP compiled in, the Makefile runs the program at 1, 3 and 4 threads
(`OMP_NUM_THREADS`; `EQUIV_THREADS` overrides) and every run must match: a thread
count that changes a hash is a bug. `TIMEOUT` is applied as for the suites, so

```sh
make ... TIMEOUT="qemu-aarch64 -0" equivalence
```

checks a static aarch64 build's NEON paths against the same reference under
emulation (`tools/cross/README.md` has the build). The reference is the same file on
every platform: there is no per-architecture set of hashes to drift.

## How the reference was made

`hashes-v0.3.1.txt` was produced once, from electric-eye v0.3.1's own sources -- the
subtree import commit `6a77a69` (= electric-eye `96b3eb1`), checked out into a
worktree -- by compiling this same `equiv.c` against `modules/electric-eye/src` there:

```sh
git worktree add --detach /tmp/v031 6a77a69
V=/tmp/v031/modules/electric-eye/src
gcc -std=gnu99 -Wall -Wextra -pedantic -pthread -fopenmp -O2 -g -D_FILE_OFFSET_BITS=64 \
    -DEQUIV_V031 -I$V -o equiv-v031 tools/equivalence/equiv.c \
    $V/conv.c $V/point_opps.c $V/effect_chain.c $V/auto_vis.c $V/img_stats.c $V/video_frame.c -lm -lrt
OMP_NUM_THREADS=1 ./equiv-v031 > tools/equivalence/hashes-v0.3.1.txt
git worktree remove /tmp/v031
```

`-DEQUIV_V031` is the one concession to the old API: v0.3.1's chain took the whole
`Config`, whose `stages`, `stage_count` and `auto_visibility` are the three fields
hangar's `EffectChainConfig` kept under the same names, so the shim is a typedef. The
case list is identical either way. gcc 16.2.1, x86-64, 2026-11-07; the reference was
run at 1, 3 and 4 threads and the three outputs were identical before it was kept.

## Regenerating it

Do not. The point of the file is that it was made from v0.3.1 and has not been touched
since; a hash that changes is a behaviour that changed, and the right response is to
understand why, then either fix the code or decide, deliberately, that the new
behaviour is wanted.

If it is wanted -- a bug in an operator fixed, say -- regenerate from the current
tree (`make equivalence` leaves the outputs in `$(BUILDDIR)/equiv-<threads>.txt`; copy
the 1-thread one over `hashes-v0.3.1.txt`), rename the file for what it now records if
it no longer records v0.3.1, and explain in the commit message which cases changed and
why. A regeneration that is not explained is a failed equivalence check with the
evidence deleted.

Changing the case set (a new operator, another size) changes the first lines and so
fails the check too. Add the new cases, regenerate, and say so in the commit: the old
cases' hashes must be unchanged in the diff, which is the proof the addition broke
nothing.
