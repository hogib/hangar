#pragma once
#include "udp_relay.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * Recording an H.264 RTP stream exactly as it arrives, copied into an MP4
 * by an ffmpeg child, without re-encoding. (hellion's topside recorder;
 * which of its pipelines is recorded, and the hrec_* names the rest of
 * hellion uses, are legacy/hellion/hrecord.c's.)
 *
 * Starting a recording attaches a local port to the stream's UDP relay
 * (udp_relay_attach), so the recorder gets the same packets as the decoder
 * and QGC, and takes nothing from them. The file is fragmented MP4 (see
 * ff_recorder_args): one cut off by a crash or a power loss still plays up
 * to its last keyframe.
 *
 * One recording at a time: "am I recording?" has one answer. A recording
 * whose stream stops is paused, not ended: it continues into a new part
 * when packets return, until it is stopped.
 */

typedef struct Mp4Rec Mp4Rec;

// Where the packets come from: the relay the stream is received through,
// or NULL while it is not set up yet ("not receiving yet"). Asked on every
// start and resume rather than taken once, since the receiver may be
// restarted underneath a paused recording.
typedef UdpRelay *(*Mp4RecRelayFn)(void *ctx);

typedef struct {
  char tag[32];      // log-line prefix and child_proc tag, e.g. "hellion"
  char dir[256];     // where the files go; created on the first start
  char sdp_dir[256]; // where the SDP file ffmpeg reads its input from goes
  char source[96];   // what is recorded, for the log: "pipeline \"down\""
  // Why nothing can be recorded here; "" when something can. Set by the
  // caller when it found no stream to record. When empty the recorder adds
  // the one reason of its own (no ffmpeg installed).
  char why[160];
  Mp4RecRelayFn relay;
  void *relay_ctx;
} Mp4RecParams;

// Never NULL except on allocation failure: a recorder with nothing to
// record reports itself unavailable, with the reason.
Mp4Rec *mp4rec_create(const Mp4RecParams *p);

// Starts or stops. *detail is the file name on success, else why not.
bool mp4rec_start(Mp4Rec *r, char *detail, size_t cap);
bool mp4rec_stop(Mp4Rec *r, char *detail, size_t cap);

// The page's "recorder" object: available, why, recording, paused, part,
// file, elapsed_s, bytes, disk_free_bytes, error.
void mp4rec_status_json(Mp4Rec *r, char *out, size_t cap);

// One line for top and the status file; "" when idle and fine.
void mp4rec_summary(Mp4Rec *r, char *out, size_t cap);

// Stops any recording (finalizing the file) and frees.
void mp4rec_destroy(Mp4Rec *r);
