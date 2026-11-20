# vision (modules/vision/README.md), binary hangar-vision, and its suite.
# Included by the root Makefile after lib/vision/vision.mk; relies on OBJDIR,
# TESTDIR, BUILDDIR, HG_SRC, HG_CFLAGS, HG_LDLIBS, VISION_SRC, TIMEOUT,
# ALL_LDFLAGS and the generic $(OBJDIR)/%.o rule. GPL-3.0-or-later; C11 over
# lib/hg and lib/vision (the operators and their thread pool), so it builds
# static for musl.
#
#   make vision-all     builddir/hangar-vision
#   make check-vision   the suite (part of check): the presets and the chain
#                       grammar; the module under a manager, its output
#                       compared with lib/vision's own on the same frames

VISION_MOD_LIB_SRC := msg/vision_params.c lib/hg/hg_ini.c
VISION_MOD_CPPFLAGS := -D_FILE_OFFSET_BITS=64 -Ilib/hg -Ilib/common -Imsg -Imodules/common -Imodules/vision \
	-Ilib/vision -Ihangard/tests
VISION_BIN := $(BUILDDIR)/hangar-vision
VISION_TEST := $(TESTDIR)/test_vision

$(OBJDIR)/modules/vision/%.o: EE_CFLAGS := $(HG_CFLAGS)
$(OBJDIR)/modules/vision/%.o: EE_CPPFLAGS := $(VISION_MOD_CPPFLAGS)

$(VISION_BIN): $(OBJDIR)/modules/vision/vision.o $(OBJDIR)/modules/vision/presets.o $(call obj,$(HG_SRC)) \
		$(call obj,$(VISION_MOD_LIB_SRC)) $(call obj,$(VISION_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) $(LDLIBS) -lm

.PHONY: vision-all
vision-all: $(VISION_BIN)
all: $(VISION_BIN)

$(VISION_TEST): $(OBJDIR)/modules/vision/tests/test_vision.o $(OBJDIR)/modules/vision/presets.o \
		$(call obj,$(HG_SRC)) $(call obj,$(VISION_MOD_LIB_SRC)) $(call obj,$(VISION_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) $(LDLIBS) -lm

.PHONY: check-vision
check-vision: $(VISION_TEST) $(VISION_BIN) $(BUILDDIR)/hangard $(BUILDDIR)/hangar
	@if HANGARD_BIN=$(abspath $(BUILDDIR)/hangard) HANGAR_BIN=$(abspath $(BUILDDIR)/hangar) \
	    HANGARD_TOYS=$(abspath $(HANGARD_TOYDIR)) HANGAR_MSG_SRC=$(abspath msg) \
	    VISION_BIN=$(abspath $(VISION_BIN)) \
	    $(if $(TIMEOUT),$(TIMEOUT) 120) $(abspath $<) > $(TESTDIR)/vision.log 2>&1; then \
	  echo "ok    vision"; \
	else \
	  echo "FAIL  vision"; cat $(TESTDIR)/vision.log; exit 1; \
	fi
