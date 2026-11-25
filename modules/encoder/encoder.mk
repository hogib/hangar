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
# lib/camera/camera_ctrl.c: the camera's controls in portable units, shared with eeye.
ENCODER_LIB_SRC := msg/video_index.c msg/encoder_params.c lib/hg/hg_ini.c lib/camera/camera_ctrl.c
ENCODER_CPPFLAGS := -D_FILE_OFFSET_BITS=64 -Ilib/hg -Ilib/common -Ilib/camera -Imsg -Imodules/common -Imodules/encoder
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

# The suite's camera: an LD_PRELOAD library (tests/fake_uvc.c). Built where the
# toolchain makes shared objects; where it does not (a static cross build), the
# file is absent and the suite says that it skipped the unplug test.
ENCODER_FAKE_UVC := $(TESTDIR)/encoder-modules/fake_uvc.so
$(ENCODER_FAKE_UVC): modules/encoder/tests/fake_uvc.c
	@mkdir -p $(dir $@)
	-$(CC) -std=gnu11 -Wall -Wextra -O2 -fPIC -shared -o $@ $< -ldl -lpthread 2>/dev/null || rm -f $@

# The camera's controls, under a manager on the fake camera (tests/test_encoder_controls.c).
ENCODER_CONTROLS_TEST := $(TESTDIR)/test_encoder_controls
$(OBJDIR)/modules/encoder/tests/test_encoder_controls.o: EE_CPPFLAGS := $(ENCODER_CPPFLAGS) -Ihangard/tests
$(ENCODER_CONTROLS_TEST): $(OBJDIR)/modules/encoder/tests/test_encoder_controls.o $(call obj,$(HG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) $(LDLIBS)

.PHONY: check-encoder
check-encoder: $(ENCODER_TEST) $(ENCODER_CONTROLS_TEST) $(ENCODER_BIN) $(ENCODER_FAKE_UVC) $(BUILDDIR)/hangard $(BUILDDIR)/hangar
	@if ENCODER_BIN=$(abspath $(ENCODER_BIN)) ENCODER_FAKE_UVC=$(abspath $(ENCODER_FAKE_UVC)) \
	    $(if $(TIMEOUT),$(TIMEOUT) 120) $(abspath $<) > $(TESTDIR)/encoder.log 2>&1; then \
	  echo "ok    encoder"; \
	else \
	  echo "FAIL  encoder"; cat $(TESTDIR)/encoder.log; exit 1; \
	fi
	@if ENCODER_BIN=$(abspath $(ENCODER_BIN)) ENCODER_FAKE_UVC=$(abspath $(ENCODER_FAKE_UVC)) \
	    HANGARD_BIN=$(abspath $(BUILDDIR)/hangard) HANGAR_BIN=$(abspath $(BUILDDIR)/hangar) \
	    HANGARD_TOYS=$(abspath $(HANGARD_TOYDIR)) HANGAR_MSG_SRC=$(abspath msg) \
	    $(if $(TIMEOUT),$(TIMEOUT) 120) $(abspath $(ENCODER_CONTROLS_TEST)) > $(TESTDIR)/encoder_controls.log 2>&1; then \
	  echo "ok    encoder_controls$$(grep -m1 -o ' *(*skipped[^)]*)*' $(TESTDIR)/encoder_controls.log | sed 's/^ */  /')"; \
	else \
	  echo "FAIL  encoder_controls"; cat $(TESTDIR)/encoder_controls.log; exit 1; \
	fi
