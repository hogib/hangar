#pragma once
#include "hconfig.h"
#include "mp4_record.h"
#include "pipeline.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * Topside recording: the H.264 stream exactly as it arrives over the
 * tether, copied into an MP4 by an ffmpeg child, without re-encoding. The
 * recorder itself is lib/video/mp4_record.h; this is hellion's wiring of
 * it -- which pipeline is recorded, where the files and the SDP go -- and
 * the hrec_* names control.c and main.c use.
 */

typedef Mp4Rec HRec;

// Picks the pipeline to record ("record_from", else the first with an rtp
// source) from `pipes`. Never NULL: a recorder with nothing to record
// reports itself unavailable, with the reason.
HRec *hrec_create(const HellionConfig *cfg, Pipeline *const *pipes,
                          size_t pipe_count);

// Starts or stops. *detail is the file name on success, else why not.
bool hrec_start(HRec *r, char *detail, size_t cap);
bool hrec_stop(HRec *r, char *detail, size_t cap);

// The page's "recorder" object: available, why, recording, file,
// elapsed_s, bytes, disk_free_bytes, error.
void hrec_status_json(HRec *r, char *out, size_t cap);

// One line for top and the status file; "" when idle and fine.
void hrec_summary(HRec *r, char *out, size_t cap);

// Stops any recording (finalizing the file) and frees.
void hrec_destroy(HRec *r);
