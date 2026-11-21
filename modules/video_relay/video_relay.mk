# video_relay (modules/video_relay/README.md), binary hangar-video-relay, and
# its suite. Included by the root Makefile after modules/encoder/encoder.mk;
# relies on OBJDIR, TESTDIR, BUILDDIR, HG_SRC, HG_CFLAGS, HG_LDLIBS, TIMEOUT,
# ALL_LDFLAGS, PKG_CONFIG and the generic $(OBJDIR)/%.o rule. GPL-3.0-or-later;
# C11 over lib/hg, libavcodec and libavutil for the decoding (in process, no
# ffmpeg child: docs/11 "hangar's own").
#
#   make video_relay-all     builddir/hangar-video-relay
#   make check-video_relay   the suite (part of check): the depacketizer, the
#                            MP4 writer, the module fed RTP over loopback
#
# libavcodec: AVCODEC_CFLAGS / AVCODEC_LIBS, by default from pkg-config
# (libavcodec libavutil: the system's, for development and the tests); the
# release links a static build of the H.264 decoder alone (tools/release.sh,
# tools/cross/avcodec.sh). Without libavcodec the module and its suite are
# skipped, and say so.

ifeq ($(origin AVCODEC_CFLAGS),undefined)
AVCODEC_CFLAGS := $(shell $(PKG_CONFIG) --cflags libavcodec libavutil 2>/dev/null)
endif
ifeq ($(origin AVCODEC_LIBS),undefined)
AVCODEC_LIBS := $(shell $(PKG_CONFIG) --libs libavcodec libavutil 2>/dev/null)
endif

VIDEO_RELAY_LIB_SRC := msg/recorder_session.c msg/video_index.c lib/hg/hg_ini.c
VIDEO_RELAY_CPPFLAGS := -D_FILE_OFFSET_BITS=64 -Ilib/hg -Ilib/common -Imsg -Imodules/common -Imodules/video_relay \
	-Imodules/encoder $(AVCODEC_CFLAGS)
VIDEO_RELAY_BIN := $(BUILDDIR)/hangar-video-relay
VIDEO_RELAY_TEST := $(TESTDIR)/test_video_relay
VIDEO_RELAY_OBJS := $(OBJDIR)/modules/video_relay/depack.o $(OBJDIR)/modules/video_relay/mp4.o

$(OBJDIR)/modules/video_relay/%.o: EE_CFLAGS := $(HG_CFLAGS)
$(OBJDIR)/modules/video_relay/%.o: EE_CPPFLAGS := $(VIDEO_RELAY_CPPFLAGS)

ifneq ($(strip $(AVCODEC_LIBS)),)
$(VIDEO_RELAY_BIN): $(OBJDIR)/modules/video_relay/video_relay.o $(VIDEO_RELAY_OBJS) $(call obj,$(HG_SRC)) \
		$(call obj,$(VIDEO_RELAY_LIB_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(AVCODEC_LIBS) $(HG_LDLIBS) $(LDLIBS) -lm

.PHONY: video_relay-all
video_relay-all: $(VIDEO_RELAY_BIN)
all: $(VIDEO_RELAY_BIN)

# The suite links the packetizer of modules/encoder to make the RTP it feeds.
$(VIDEO_RELAY_TEST): $(OBJDIR)/modules/video_relay/tests/test_video_relay.o $(VIDEO_RELAY_OBJS) \
		$(OBJDIR)/modules/encoder/h264.o $(call obj,$(HG_SRC)) $(call obj,$(VIDEO_RELAY_LIB_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) $(LDLIBS)

.PHONY: check-video_relay
check-video_relay: $(VIDEO_RELAY_TEST) $(VIDEO_RELAY_BIN)
	@if VIDEO_RELAY_BIN=$(abspath $(VIDEO_RELAY_BIN)) VIDEO_RELAY_FIXTURES=$(abspath modules/video_relay/tests) \
	    $(if $(TIMEOUT),$(TIMEOUT) 120) $(abspath $<) > $(TESTDIR)/video_relay.log 2>&1; then \
	  echo "ok    video_relay"; \
	else \
	  echo "FAIL  video_relay"; cat $(TESTDIR)/video_relay.log; exit 1; \
	fi
else
.PHONY: video_relay-all check-video_relay
video_relay-all check-video_relay:
	@echo "skip  video_relay (no libavcodec: pkg-config libavcodec libavutil, or AVCODEC_CFLAGS/AVCODEC_LIBS)"
endif
