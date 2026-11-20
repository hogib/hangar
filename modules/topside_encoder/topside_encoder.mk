# topside_encoder (modules/topside_encoder/README.md), binary hangar-topside-encoder,
# and its suite. Included by the root Makefile after modules/video_relay/video_relay.mk;
# relies on OBJDIR, TESTDIR, BUILDDIR, HG_SRC, HG_CFLAGS, HG_LDLIBS, TIMEOUT,
# ALL_LDFLAGS, PKG_CONFIG, AVCODEC_LIBS and the generic $(OBJDIR)/%.o rule.
# GPL-3.0-or-later; C11 over lib/hg and libx264 (GPL-2.0-or-later), in process: no
# ffmpeg child (docs/11 "hangar's own").
#
#   make topside_encoder-all     builddir/hangar-topside-encoder
#   make check-topside_encoder   the suite (part of check): the module under a
#                                manager, its RTP decoded back with libavcodec
#
# libx264: X264_CFLAGS / X264_LIBS, by default from pkg-config (x264: the
# system's, for development and the tests); the release links a static build
# (tools/release.sh, tools/cross/x264.sh). Without libx264 the module and its
# suite are skipped, and say so. The suite decodes with libavcodec too
# (AVCODEC_LIBS, video_relay.mk's); without it the suite is skipped.

ifeq ($(origin X264_CFLAGS),undefined)
X264_CFLAGS := $(shell $(PKG_CONFIG) --cflags x264 2>/dev/null)
endif
ifeq ($(origin X264_LIBS),undefined)
X264_LIBS := $(shell $(PKG_CONFIG) --libs x264 2>/dev/null)
endif

TOPSIDE_ENCODER_LIB_SRC := msg/video_index.c msg/topside_encoder_params.c lib/hg/hg_ini.c
TOPSIDE_ENCODER_CPPFLAGS := -D_FILE_OFFSET_BITS=64 -Ilib/hg -Ilib/common -Imsg -Imodules/common \
	-Imodules/topside_encoder -Imodules/encoder -Imodules/video_relay -Ihangard/tests $(X264_CFLAGS) $(AVCODEC_CFLAGS)
TOPSIDE_ENCODER_BIN := $(BUILDDIR)/hangar-topside-encoder
TOPSIDE_ENCODER_TEST := $(TESTDIR)/test_topside_encoder

$(OBJDIR)/modules/topside_encoder/%.o: EE_CFLAGS := $(HG_CFLAGS)
$(OBJDIR)/modules/topside_encoder/%.o: EE_CPPFLAGS := $(TOPSIDE_ENCODER_CPPFLAGS)

ifneq ($(strip $(X264_LIBS)),)
$(TOPSIDE_ENCODER_BIN): $(OBJDIR)/modules/topside_encoder/topside_encoder.o $(OBJDIR)/modules/encoder/h264.o \
		$(call obj,$(HG_SRC)) $(call obj,$(TOPSIDE_ENCODER_LIB_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(X264_LIBS) $(HG_LDLIBS) $(LDLIBS) -lm

.PHONY: topside_encoder-all
topside_encoder-all: $(TOPSIDE_ENCODER_BIN)
all: $(TOPSIDE_ENCODER_BIN)

ifneq ($(strip $(AVCODEC_LIBS)),)
# The suite plays the frame source under a manager and decodes the module's RTP
# back with video_relay's depacketizer and libavcodec.
$(TOPSIDE_ENCODER_TEST): $(OBJDIR)/modules/topside_encoder/tests/test_topside_encoder.o \
		$(OBJDIR)/modules/video_relay/depack.o $(OBJDIR)/modules/encoder/h264.o \
		$(call obj,$(HG_SRC)) $(call obj,$(TOPSIDE_ENCODER_LIB_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(AVCODEC_LIBS) $(HG_LDLIBS) $(LDLIBS) -lm

.PHONY: check-topside_encoder
check-topside_encoder: $(TOPSIDE_ENCODER_TEST) $(TOPSIDE_ENCODER_BIN) $(BUILDDIR)/hangard $(BUILDDIR)/hangar
	@if TOPSIDE_ENCODER_BIN=$(abspath $(TOPSIDE_ENCODER_BIN)) HANGARD_BIN=$(abspath $(BUILDDIR)/hangard) \
	    HANGAR_BIN=$(abspath $(BUILDDIR)/hangar) HANGARD_TOYS=$(abspath $(HANGARD_TOYDIR)) HANGAR_MSG_SRC=$(abspath msg) \
	    $(if $(TIMEOUT),$(TIMEOUT) 120) $(abspath $<) > $(TESTDIR)/topside_encoder.log 2>&1; then \
	  echo "ok    topside_encoder"; \
	else \
	  echo "FAIL  topside_encoder"; cat $(TESTDIR)/topside_encoder.log; exit 1; \
	fi
else
.PHONY: check-topside_encoder
check-topside_encoder:
	@echo "skip  topside_encoder (its suite decodes with libavcodec: pkg-config libavcodec libavutil)"
endif
else
.PHONY: topside_encoder-all check-topside_encoder
topside_encoder-all check-topside_encoder:
	@echo "skip  topside_encoder (no libx264: pkg-config x264, or X264_CFLAGS/X264_LIBS)"
endif
