#pragma once
#include "camera_ctrl.h"
#include "capture.h" // CaptureSource, CaptureFormat
#include "effects.h" // EffectType, EffectStage, max_chain_stages
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What the recording tap writes -- see Config.record_format.
typedef enum {
  RECORD_RAW = 0, // decoded I422 planes, frames back to back (~55MB/s at 720p)
  RECORD_MJPEG,   // the camera's own JPEG bytes, as received
} RecordFormat;

// Shared with consumer_loop's own local copy of the current recording
// path (see video_threads.c), so the two can never drift out of sync.
enum { max_record_path_len = 256 };

// capture_device / output_device. /dev/video<N> and /dev/v4l/by-id/...
// names both fit comfortably.
enum { max_device_path_len = 128 };

// capture_stream / bus_pilot / bus_raw; matches fbus_max_stream_name.
enum { max_stream_name_len = 32 };

typedef struct {
  EffectStage stages[max_chain_stages];
  size_t stage_count; // may be 0: an empty chain is a valid pass-through

  // Recording tap: the producer-filled, untouched camera frame (raw_data
  // in video_frame.h) is written here on every frame once this is
  // non-empty, independent of whatever the effect chain above does to the
  // frame that's actually sent to the virtual camera -- see consumer_loop.
  // Empty string means recording is off. Written as raw I422 with no
  // container by default; see recorder.c for how to play it back.
  char record_path[max_record_path_len];

  // "raw" (default) or "mjpeg". "mjpeg" writes the JPEG bytes the camera
  // sent instead of the decoded frame: roughly an order of magnitude
  // smaller, no encode cost, and one decode closer to the sensor. Only an
  // MJPEG source has those bytes; with YUYV or yuv420 capture the recording
  // falls back to raw and says so. Read when a recording starts.
  RecordFormat record_format;

  // Camera controls -- exposure, gain, white balance and friends. Unlike
  // the geometry fields these ARE hot-reloadable: light changes as the
  // drone descends, and restarting to adjust exposure mid-dive is not an
  // option. Every field defaults to "unset", meaning leave the camera
  // alone; see camera_ctrl.h for why that is a sentinel rather than 0.
  CameraControls camera;

  // Auto visibility (auto_vis.h): continuously corrects colour cast,
  // brightness range and haze on the pilot's picture, ahead of the chain.
  // Off by default -- it changes what the pilot sees, so it is switched on
  // deliberately (the page's big toggle) -- and hot-reloadable.
  bool auto_visibility;

  // --- Geometry: the one group of fields here that is NOT hot-reloadable ---
  //
  // Read once, at startup, by config_load_once(); the running pipeline
  // never re-reads them. They still live in Config (rather than in some
  // separate startup-only struct) for one concrete reason: parse_config
  // treats an unknown key as a hard error, so the watcher has to
  // understand these keys anyway or every reload of a geometry-carrying
  // file would fail outright. config_publish() compares them against the
  // startup values on each reload and warns when they differ, so a change
  // is visibly ignored rather than silently so.
  //
  // Changing them live would mean draining and reallocating the frame pool
  // while three threads hold frames from it, plus renegotiating the format
  // on both V4L2 devices -- and v4l2loopback can't change format at all
  // while a consumer has the device open. Restart is the honest answer.

  // What to ask the camera for. The camera must grant these exactly (see
  // try_set_format in v4l2_in.c) -- they are not a hint.
  uint16_t capture_width;
  uint16_t capture_height;

  // Integer divisor applied to the captured frame before anything else
  // touches it: the frame pool, the whole effect chain, the v4l2loopback
  // output, and the preview JPEG all run at capture/downscale. 1 means no
  // downscaling.
  //
  // Restricted to 1, 2, 4, or 8 rather than an arbitrary target
  // resolution, because both capture paths need the ratio to be exact:
  // the MJPEG path hands it to libjpeg-turbo, which only scales by one of
  // 16 discrete factors and silently picks the largest one that *fits* a
  // requested size (so an unsupported ratio would quietly leave a smaller
  // image in a larger frame), and the YUYV path box-averages NxN source
  // blocks, which only divides evenly for an integer N.
  uint8_t downscale;

  // Which capture backend to use. "auto" (the default) probes for a Pi CSI
  // camera via rpicam_in_available() and falls back to V4L2; the explicit
  // values skip that probe, which is what you want when a machine has both
  // a ribbon-cable camera and a USB webcam attached and the choice would
  // otherwise depend on probe order.
  CaptureSource capture_source;

  // Which pixel format to demand from the camera. Startup-only, like
  // capture_source -- the format is fixed for the lifetime of the open
  // device. "auto", "mjpeg" and "yuyv" are V4L2 choices (rpicam treats
  // them all as MJPEG); "yuv420" is rpicam-only.
  CaptureFormat capture_format;

  // The V4L2 camera to open (unused by the rpicam backend, which has no
  // device node). Startup-only: the frame pool is sized from what this
  // device negotiates.
  char capture_device[max_device_path_len];

  // The v4l2loopback device the pilot's picture is written to, for any
  // program that wants a webcam (OBS, ffplay, a browser). "" turns it off.
  // Optional output: when the device is missing, or goes away mid-run,
  // eeye logs it and keeps retrying while everything else carries on.
  // Startup-only, like the rest of this group.
  char output_device[max_device_path_len];

  // Frame bus (fbus.h). capture_stream is the stream read when
  // capture_source is "bus"; its geometry is the pipeline's, so
  // capture_width/height are ignored and downscale must be 1. bus_pilot
  // and bus_raw are the names eeye publishes the processed and the
  // untouched frame under; "" publishes nothing. All startup-only.
  char capture_stream[max_stream_name_len];
  char bus_pilot[max_stream_name_len];
  char bus_raw[max_stream_name_len];
} Config;

/*
 * Watches a JSON config file and republishes a new Config every time it
 * changes on disk, so a separate process (a web UI on another device,
 * writing over scp/sftp) can retune the running pipeline without a
 * restart.
 *
 * Expected file:
 *   {
 *     "chain": [
 *       {"effect": "blur"},
 *       {"effect": "sobel"},
 *       {"effect": "tint", "tint_u": 90, "tint_v": 150, "tint_strength": 180}
 *     ],
 *     "record_path": "/opt/electric-eye/recordings/session.raw",
 *     "camera": {"auto_exposure": false, "exposure": 25000},
 *     "capture_width": 1280,
 *     "capture_height": 720,
 *     "downscale": 2,
 *     "capture_source": "auto",
 *     "capture_format": "auto",
 *     "capture_device": "/dev/video0",
 *     "output_device": "/dev/video10",
 *     "capture_stream": "camera",
 *     "bus_pilot": "pilot",
 *     "bus_raw": "raw"
 *   }
 *
 * "chain" is a list of stages, applied in order; each is one of
 * none|grayscale|invert|threshold|tint|sobel|blur|contrast|light|log|canny,
 * plus that
 * effect's own parameters where it takes any (threshold_value for
 * threshold; tint_u/tint_v/tint_strength for tint; sobel_threshold for
 * sobel; blur_strength for blur; light_level for light -- all 0-255).
 * contrast takes no parameters: it's a full-frame auto min/max luma
 * stretch (see gs_contrast_normalize in point_opps.h), recomputed fresh
 * every frame, so there's nothing to configure beyond having it in the
 * chain or not. light_level (default 128, the only field here that
 * doesn't default to 0 when omitted -- see EffectStage's own comment on
 * light_level) moves brightness and color saturation together: below 128
 * is darker/desaturated, above 128 is brighter/more saturated, unlike
 * contrast this is a fixed function of the level rather than the frame's
 * actual content (see color_light in point_opps.h). An empty chain ([])
 * is a valid pass-through.
 * "record_path" is optional; omit it (or set "") to leave recording off.
 * "camera" is optional; it carries camera controls (auto_exposure,
 * exposure in microseconds, gain, auto_white_balance, white_balance in
 * Kelvin, brightness, contrast, saturation, sharpness). Any field left
 * out leaves that control as the camera had it. These are hot-reloadable.
 * See camera_ctrl.h for the units and ranges, which are deliberately
 * device-independent so one config works on both capture backends.
 * "auto_visibility" is optional (default false), hot-reloadable: true runs
 * auto_vis.h's continuous correction ahead of the chain, pilot only.
 * The old preview keys "stream_raw", "stream_frame_interval" and
 * "stream_quality" are accepted and ignored (logged once): the preview is
 * hellion's now, and a config written before that must still load.
 * "capture_width"/"capture_height" (default 1280x720) and "downscale"
 * (default 1; must be 1, 2, 4, or 8) are the geometry fields -- unlike
 * everything above, they only take effect at startup, and a reload that
 * changes them warns and keeps running at the original size.
 * "capture_source" (default "auto"; one of "auto", "v4l2", "rpicam")
 * chooses the capture backend and is likewise startup-only -- see
 * CaptureSource above and rpicam_in.h. "capture_format" (default "auto";
 * one of "auto", "mjpeg", "yuyv") picks the V4L2 pixel format and is also
 * startup-only; "yuyv" is the zero-decode path and is worth pinning
 * explicitly on a camera chosen for it, since "auto" prefers MJPEG.
 * "capture_device" (default "/dev/video0") is the V4L2 camera and
 * "output_device" (default "/dev/video10"; "" for none) the v4l2loopback
 * device the pilot's picture is written to. Both are startup-only.
 * "capture_source": "bus" reads frames from frame bus stream
 * "capture_stream" (default "camera") instead of a camera. "bus_pilot"
 * (default "pilot") and "bus_raw" (default "raw") name the streams eeye
 * publishes; "" turns either off. All startup-only.
 *
 * Writer contract: write to a temp file in the same directory, then
 * rename() it onto the target path. A plain in-place overwrite can be
 * observed mid-write and read as a truncated, invalid file -- rename is
 * atomic from a reader's point of view. inotify watches the directory
 * rather than the file itself for exactly this reason: a watch on the
 * file's inode goes stale the moment rename() replaces it.
 *
 * Reload feedback: after every load attempt (startup or reload), a
 * sibling file "<path>.status" is written (same atomic rename contract as
 * above) containing a single line: {"ok":true} if that attempt's config
 * is now what's running, {"ok":false} if it was rejected and the previous
 * config is still active. This exists for a remote writer (hellion, for
 * the page) that has no other way to know whether what it just
 * wrote actually took effect -- the detailed reason for a rejection is
 * only ever on this process's own stdout/journalctl, not in the status
 * file, to avoid needing a second place that must agree with parse_config()
 * about how to phrase every possible error.
 */
typedef struct ConfigWatcher ConfigWatcher;

/*
 * Loads `path` once, synchronously, before returning -- config_current()
 * is guaranteed to return a valid snapshot immediately, never NULL. If the
 * file is missing or malformed at startup, logs why and falls back to a
 * hardcoded default (a single-stage EFFECT_SOBEL chain) rather than
 * failing.
 *
 * Then starts a background thread that reloads on every subsequent write.
 * If the watch itself can't be established (e.g. the containing directory
 * doesn't exist), that failure degrades gracefully too: the process keeps
 * running with whatever was loaded at startup, just without live reload,
 * and says so on stdout. The only way this returns NULL is an allocation
 * failure.
 *
 * *is_running follows the same convention as ProducerArgs/WorkerArgs/
 * ConsumerArgs: the caller flips it to false to signal every thread,
 * including this one, to shut down.
 */
ConfigWatcher *config_watch_start(const char *path, bool *is_running);

void config_watch_stop(ConfigWatcher *watcher);

/*
 * Reads `path` once, before any thread exists, purely to answer "what
 * geometry should this run use?" -- see Config's geometry fields. Fills
 * *out with the parsed config on success.
 *
 * Returns false only if the file exists but is invalid: that is fatal at
 * startup (unlike the same failure during a live reload, which keeps the
 * previous config and carries on) because there is no previous geometry to
 * fall back to, and guessing one would mean allocating a frame pool the
 * operator never asked for. A file that simply doesn't exist is *not* a
 * failure -- *out gets the defaults and this returns true, matching how
 * config_watch_start treats a missing file.
 *
 * Also records the geometry it read, so config_publish() can warn on any
 * later reload that tries to change it.
 */
bool config_load_once(const char *path, Config *out);

/*
 * The pipeline geometry implied by a Config's capture_width/capture_height
 * and downscale: the size of every frame in the pool, and so the size the
 * effect chain, the v4l2loopback output, and the preview JPEG all run at.
 *
 * Kept as one function rather than open-coded divisions at each call site
 * so the rounding can never drift between them -- config_load_once has
 * already validated that the division is exact, so this is a plain divide
 * with no rounding to disagree about.
 */
void config_output_geometry(const Config *cfg, uint32_t *out_width,
                            uint32_t *out_height);

/*
 * The live config, as of whenever this is called. Call once per frame, not
 * once per effect stage -- one snapshot per frame guarantees a config push
 * can't land mid-frame and split a single output frame between old and new
 * parameters (both for the effect chain and for consumer_loop's own
 * separate read of record_path).
 */
const Config *config_current(const ConfigWatcher *watcher);
