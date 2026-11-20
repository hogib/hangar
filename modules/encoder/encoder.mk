# The encoder (modules/encoder/README.md), binary hangar-encoder, and its
# suite. Included by the root Makefile after modules/recorder/recorder.mk;
# relies on OBJDIR, TESTDIR, BUILDDIR, HG_SRC, HG_CFLAGS, HG_LDLIBS, TIMEOUT,
# ALL_LDFLAGS and the generic $(OBJDIR)/%.o rule. GPL-3.0-or-later; C11; libc,
# lib/hg and the generated msg/ types only, so it builds static for musl.
#
#   make encoder-all      builddir/hangar-encoder
#   make check-encoder    the suite (part of check): the packetizer, the
#                         module from an Annex B file to a UDP socket, and,
#                         where ffmpeg is installed, ffmpeg decoding its RTP

# The message type it publishes and the INI walker encoder.conf is read with.
ENCODER_LIB_SRC := msg/video_index.c lib/hg/hg_ini.c
ENCODER_CPPFLAGS := -D_FILE_OFFSET_BITS=64 -Ilib/hg -Ilib/common -Imsg -Imodules/common -Imodules/encoder
ENCODER_BIN := $(BUILDDIR)/hangar-encoder
ENCODER_TEST := $(TESTDIR)/test_encoder

$(OBJDIR)/modules/encoder/%.o: EE_CFLAGS := $(HG_CFLAGS)
$(OBJDIR)/modules/encoder/%.o: EE_CPPFLAGS := $(ENCODER_CPPFLAGS)

$(ENCODER_BIN): $(OBJDIR)/modules/encoder/encoder.o $(OBJDIR)/modules/encoder/h264.o $(call obj,$(HG_SRC)) \
		$(call obj,$(ENCODER_LIB_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) $(LDLIBS)

.PHONY: encoder-all
encoder-all: $(ENCODER_BIN)
all: $(ENCODER_BIN)

$(ENCODER_TEST): $(OBJDIR)/modules/encoder/tests/test_encoder.o $(OBJDIR)/modules/encoder/h264.o \
		$(call obj,$(HG_SRC)) $(call obj,$(ENCODER_LIB_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) $(LDLIBS)

.PHONY: check-encoder
check-encoder: $(ENCODER_TEST) $(ENCODER_BIN)
	@if ENCODER_BIN=$(abspath $(ENCODER_BIN)) \
	    $(if $(TIMEOUT),$(TIMEOUT) 120) $(abspath $<) > $(TESTDIR)/encoder.log 2>&1; then \
	  echo "ok    encoder"; \
	else \
	  echo "FAIL  encoder"; cat $(TESTDIR)/encoder.log; exit 1; \
	fi
