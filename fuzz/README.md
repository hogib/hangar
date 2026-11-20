# fuzz/

A harness for every place hangar parses bytes it does not control, run with
libFuzzer under AddressSanitizer and UndefinedBehaviorSanitizer, and the inputs
that matter (the seeds, and every input that ever crashed) replayed by
`make check` without the fuzzer.

    make fuzz-build                  every harness (needs clang with its fuzzer runtime)
    make fuzz                        each harness for 30 s; fails on a crash
    make fuzz FUZZ_SECONDS=600 -j2   longer, two at a time
    make fuzz-run-bridge             one harness
    make check-fuzz-corpus           part of `make check`: the corpora, with $(CC), no fuzzer
    make check-asan-fuzz-corpus      part of `make check-asan`: the same under ASan and UBSan
    make fuzz-seeds                  rewrites the generated seeds (mkseeds.c)
    make fuzz-cov-bridge             what a harness's corpus reaches, per source file (llvm-cov)

A run keeps what it finds beyond the seeds in `builddir/fuzz/corpus-<name>/`
for the next run, writes its log to `builddir/fuzz/<name>.log`, and a crashing
input to `builddir/fuzz/crashes-<name>/`.

## The harnesses

Network and device input:

| harness | what it covers | input from |
|---|---|---|
| `bridge` | `modules/bridge/bridge.c`: `on_dgram` (header, tag, replay window), the handshake (`on_hello`, pending keys, `promote`), `on_pong` and the clock estimate, `on_data` and the reliable stream's reassembly (`drain_records`), the records on it (`on_info`, `on_schema`, `on_message`, the mirrored topic's file), `on_ack`, `on_beacon` | the tether's UDP ports |
| `mavlink_vehicle` | `modules/mavlink`: `mav_decode`, the signing check, the serial and UDP receive loops of `mavlink_main.c`, `on_frame` of `mavlink_vehicle.c` and what it publishes, the parameter job of `param_backup.c` | ArduSub's serial line or UDP port |
| `mavlink_topside` | the same link, `on_frame` of `mavlink_topside.c` | QGroundControl's UDP port |
| `console` | `modules/console/dawn_patrold.c`: `conn_read`, `route` (Host and Origin checks, every path), the JSON bodies of `console_api.c`; the request parser of `lib/net/http.c` | the console's TCP port |
| `http` | `lib/net/http.c`: `http_parse_request`, `http_parse_response`, with what a successful parse promises | hellion's and the console's TCP ports |
| `video` | `modules/video_relay/depack.c` (RTP to access units), `mp4.c` (the MP4 writer, `h264_sps_size`); `modules/encoder/h264.c` (`h264_au_len`, `h264_nals`, the RTP packetizer) and its packets back through the depacketizer | the video UDP port; the camera's H.264 from V4L2 |

Local processes of lower privilege:

| harness | what it covers | input from |
|---|---|---|
| `control` | `hangard/control.c`: `control_handle` and every request (`open`, also for mirrored topics, `call`, `param-read`, `param-write`, `method`, `wiring`, `status`, `start`, ...), `hangard/topics.c` `topic_create` | the manager's control socket: modules, group hangar, hangar-admin |
| `topic` | `lib/hg/hg_topic.c` as a subscriber: `read_layout`, attach, `hg_latest`, `hg_next`, `hg_read_seq`, `hg_find_stamp`, `hg_sub_meta`, `hg_sub_schema` | a topic file another process wrote |

Files:

| harness | what it covers | input from |
|---|---|---|
| `manifest` | `hangard/manifest.c`: `mf_parse_text`, `mf_set_check` over up to four modules, then `wiring_text`, which `hg_wiring_parse` must accept | `/etc/hangar/modules/*.module` |
| `wiring` | `lib/hg/hg_wiring.c`: `hg_wiring_parse`, `hg_parse_duration`, `hg_parse_rate_mhz`, `hg_glob_match` | the wiring the manager hands a module |
| `schema` | `lib/hg/hg_type.c`: `hg_schema_parse`, `hg_schema_canonical` | a topic's or recording's header, a `.msg`, the bridge's peer |
| `params` | `lib/hg/hg_params.c`: attributes and defaults of a parameter type, `hg_params_file_parse`, range checks, `hg_params_file_format` and back | `/var/lib/hangar/<module>/params` |
| `recording` | `lib/hg/hg_record.c` (the `.hgr` reader), `hg_dataset.c` (`hg_ds_parse`, `hg_ds_bind`, the joins and `Where` of an offline build), `hg_ini.c` | recordings, `.dataset` specs |
| `bridge_conf` | `modules/bridge/bridge_conf.c`: `bc_parse`, `bc_find`, `bc_load_key` | `bridge.conf`, the key file |
| `conf` | `hangard.conf` (`conf_load`), the registry (`registry_read`), `mavlink.conf` both roles, `ardusub.params` (`mavlink_params_load`), `recorder.conf`, `joystick.conf`, an ArduSub parameter dump as `hangar ardusub-params` reads it, a parameter backup as the mavlink module loads it and sets it on an autopilot that answers | the operator's and the manager's files |
| `nn` | `lib/nn`: `nn_hgw_parse`, `nn_graph_build`, the loader's checks 2 to 6, `nn_step` | a weights file given to `load_weights` |

Each harness file starts with a comment that says what it calls, what its
input's bytes mean, and what it checks beyond "no crash".

## Three things the harnesses do that plain bytes cannot

**They include the module.** The bridge, the MAVLink modules, the console, the
manager's control socket and `hangar ardusub-params` keep their parsing in
static functions next to `main`. The harness `#include`s the `.c` file (with
`main` renamed) and calls the statics, as the unit suites of `legacy/` do;
no module was changed to be fuzzed. Sends go into a socket pair or a closed
socket; topics and files are real, in a directory under `$TMPDIR` that the
harness empties after each input.

**They seal what cannot be guessed.** A fuzzer does not produce a BLAKE2s tag,
a CRC-16 with a per-message seed, a SHA-256 signature or an FNV hash of a
graph, so raw bytes stop at the first of those checks. The inputs that are raw
test the check. For everything behind it, the harness computes the value a
real peer would: `bridge` seals datagrams with the key in force (its input is
a script, `fuzz_bridge.h`), the MAVLink harnesses repair CRCs and signatures
when a mode bit says so (`fuzz_mavlink.h`), `nn` writes the architecture hash.
What those inputs show is what a peer that holds the key, or a correct frame
with a hostile payload, can do.

**They check properties.** Where the code promises something, the harness
asserts it (`FZ_CHECK`): a parsed schema's fields lie inside its payload; the
wiring the manager writes for an accepted manifest is accepted by lib/hg; a
parameter file the formatter wrote parses to the same bytes; every control
request gets one reply that starts `ok` or `err`; what a parameter load hands
to the sender is a finite number; counters stay inside their arrays. Six of
the bugs found in the first campaign were property failures, one was a
reader that never returned, and the rest were sanitizer reports.

## Adding a harness

1. `fuzz/fuzz_<name>.c` defining `int LLVMFuzzerTestOneInput(const uint8_t *,
   size_t)`. Include `fuzz.h`: `fz_text` (a NUL-terminated copy of exactly the
   input), `fz_file` (the input as a file by path, for readers that take
   one), `FZ_CHECK`, `FZ_NOTE`. Copy an input into a block of its own length
   before handing it over, so that a read past it is a read past a heap block.
2. In `fuzz.mk`: the name in `FUZZ_HARNESSES`, `fuzz_<name>_SRC` (what it
   links), `fuzz_<name>_DEPS` (the `.c` files it includes), `_MAXLEN`.
3. Seeds in `fuzz/corpus/<name>/`: copies of real files, or made by
   `mkseeds.c` with the code that writes the real thing. Check that each is
   accepted: `FUZZ_TRACE=1 builddir/tests-debugoptimized/fuzzreg_<name> <seed>`
   prints the harness's `FZ_NOTE` lines.
4. A crash: reproduce with `builddir/fuzz/fuzz_<name> <file>`, fix the code,
   and add the input (minimised by hand or with `-minimize_crash=1`) as
   `fuzz/corpus/<name>/crash-<what>`. `make check` replays it from then on.

## What is not covered, and why

- **Decoding and encoding video.** libavcodec (video_relay) and libx264
  (topside_encoder) are other people's code with their own fuzzing; the
  harness stops at the access unit handed to the decoder. `convert()` and
  `publish()` of `video_relay.c`, which read the decoder's frame, are not run.
- **The V4L2 and joystick device layers.** `encoder.c`'s ioctl sequence
  (`uvc_open`, `uvc_read`) and `joystick.c`'s read of kernel event structs
  need a device; the bytes they yield are covered (`video`, and `conf` runs
  `js_apply` with two events), the ioctl handling is not. `lib/camera`
  (electric-eye's capture code, used by the legacy programs) is not fuzzed
  here; its JPEG framing and negotiation have unit suites.
- **The manager beyond its control socket.** Spawning, health, adoption,
  reload and re-exec are stubs in `control`. The re-exec state file
  (`startup_from_reexec`) and the Neutral records are read from the manager's
  own files and have no harness.
- **Calls and the gate.** `hg_call.c` (request and reply envelopes read from
  topics) and `hg_gate.c` run only as far as the modules' harnesses reach
  them without a manager: an arm or a console call is refused where it
  starts. A hostile `.req` or `.rep` topic file is covered as a file
  (`topic`), not as an envelope.
- **Module handlers of topic payloads.** What safety, mux, nav, depth_hold,
  thrusters, vision and policy do with the messages they subscribe to is
  not fuzzed: their inputs are topics of declared types written by other
  modules. NaN and range handling there is the unit suites' and the review's.
- **Small configuration readers that live beside a `main`.** `encoder.conf`,
  `video_relay.conf`, `topside_encoder.conf`, `pairer.conf` and a `.policy`
  file are walked by `hg_ini_parse` (fuzzed through `recording`,
  `bridge_conf` and `conf`), but their own key handlers are not run.
- **`sv_parse_servo`** (sim_vehicle, SITL's UDP packet): thirty lines with an
  exact length check, read by hand, no harness.
- **`legacy/`.** eeye's and hellion's own parsers (configuration JSON, the
  frame bus, hellion's control API) apart from the HTTP parser they share
  with the console. `lib/video/udp_relay.c` forwards datagrams without
  parsing them.
- **Python.** `python/hangar` reads recordings and schemas too; nothing here
  fuzzes it.
- **Concurrency.** Every harness is one thread and one input at a time. A
  topic file that changes under its reader (a writer overtaking, a replaced
  file) is `check-tsan`'s and the torn-read suites' subject, not this one's.
- **32-bit.** Everything ran on x86-64. Sizes that are `size_t` products
  (`slots * stride`) behave differently where `size_t` is 32 bits.

## Notes for whoever runs it

- The harnesses put their scratch directories under `$TMPDIR` (default `/tmp`)
  and remove them at exit; a run that is killed leaves `hg-fuzz-*` behind.
- `control` sets RLIMIT_FSIZE to 64 MB for its process: `open` makes the
  manager reserve `slots x payload` bytes, and the request chooses both.
- The topic seeds carry the time and pid of their making, so `make fuzz-seeds`
  leaves existing ones alone; delete one to have it made again.
- `fuzz/corpus/**` is `-text` in `.gitattributes`: the files are bytes.
