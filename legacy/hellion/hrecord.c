#define _POSIX_C_SOURCE 200809L

#include "hrecord.h"
#include "fbus.h"
#include <stdio.h>
#include <string.h>

// The relay the recorded pipeline receives through, or NULL until it is
// set up; mp4_record asks on every start and resume.
static UdpRelay *pipe_relay(void *ctx) {
  return pipeline_rtp_relay((Pipeline *)ctx);
}

HRec *hrec_create(const HellionConfig *cfg, Pipeline *const *pipes,
                  size_t pipe_count) {
  Pipeline *pipe = NULL;
  for (size_t i = 0; i < pipe_count && !pipe; i++) {
    const PipelineConfig *pc = pipeline_config(pipes[i]);
    if (!pc || pc->source != SRC_RTP)
      continue;
    if (!cfg->record_from[0] || strcmp(pc->name, cfg->record_from) == 0)
      pipe = pipes[i];
  }

  Mp4RecParams p;
  memset(&p, 0, sizeof p);
  snprintf(p.tag, sizeof p.tag, "hellion");
  snprintf(p.dir, sizeof p.dir, "%s", cfg->record_dir);
  // The SDP file ffmpeg reads its input from goes where the frame bus
  // keeps its files: in RAM, in the directory every hellion already has.
  snprintf(p.sdp_dir, sizeof p.sdp_dir, "%s", fbus_dir());
  if (pipe) {
    snprintf(p.source, sizeof p.source, "pipeline \"%s\"",
             pipeline_config(pipe)->name);
    p.relay = pipe_relay;
    p.relay_ctx = pipe;
  } else {
    snprintf(p.why, sizeof p.why,
             "no RTP source here to record (recording is topside, from the "
             "stream arriving over the tether)");
  }
  return mp4rec_create(&p);
}

bool hrec_start(HRec *r, char *detail, size_t cap) {
  return mp4rec_start(r, detail, cap);
}

bool hrec_stop(HRec *r, char *detail, size_t cap) {
  return mp4rec_stop(r, detail, cap);
}

void hrec_status_json(HRec *r, char *out, size_t cap) {
  mp4rec_status_json(r, out, cap);
}

void hrec_summary(HRec *r, char *out, size_t cap) {
  mp4rec_summary(r, out, cap);
}

void hrec_destroy(HRec *r) { mp4rec_destroy(r); }
