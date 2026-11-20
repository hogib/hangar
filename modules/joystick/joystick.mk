# The joystick module (modules/joystick/README.md; hangar#3), binary
# hangar-joystick, and its suite. Included by the root Makefile; relies on
# OBJDIR, TESTDIR, BUILDDIR, HG_SRC, HG_CFLAGS, HG_LDLIBS, TIMEOUT, ALL_LDFLAGS,
# HANGARD_TOYDIR and the generic $(OBJDIR)/%.o rule. GPL-3.0-or-later; C11;
# libc, lib/hg and the generated msg/ types only, so it builds static for musl.
#
#   make joystick-all     builddir/hangar-joystick
#   make check-joystick   the suite (part of check): joystick.conf, the shaping,
#                         and the module under a manager, fed through a FIFO

JOYSTICK_LIB_SRC := msg/wrench.c msg/pilot_buttons.c msg/empty.c lib/hg/hg_ini.c
JOYSTICK_CPPFLAGS := -D_FILE_OFFSET_BITS=64 -Ilib/hg -Ilib/common -Imsg -Imodules/common -Imodules/joystick \
	-Ihangard/tests
JOYSTICK_BIN := $(BUILDDIR)/hangar-joystick
JOYSTICK_TEST := $(TESTDIR)/test_joystick

$(OBJDIR)/modules/joystick/%.o: EE_CFLAGS := $(HG_CFLAGS)
$(OBJDIR)/modules/joystick/%.o: EE_CPPFLAGS := $(JOYSTICK_CPPFLAGS)

$(JOYSTICK_BIN): $(OBJDIR)/modules/joystick/joystick.o $(OBJDIR)/modules/joystick/joystick_map.o \
		$(call obj,$(HG_SRC)) $(call obj,$(JOYSTICK_LIB_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) -lm $(LDLIBS)

.PHONY: joystick-all
joystick-all: $(JOYSTICK_BIN)
all: $(JOYSTICK_BIN)

$(JOYSTICK_TEST): $(OBJDIR)/modules/joystick/tests/test_joystick.o $(OBJDIR)/modules/joystick/joystick_map.o \
		$(call obj,$(HG_SRC)) $(call obj,$(JOYSTICK_LIB_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) -lm $(LDLIBS)

.PHONY: check-joystick
check-joystick: $(JOYSTICK_TEST) $(JOYSTICK_BIN) $(BUILDDIR)/hangard $(BUILDDIR)/hangar
	@if HANGARD_BIN=$(abspath $(BUILDDIR)/hangard) HANGAR_BIN=$(abspath $(BUILDDIR)/hangar) \
	    HANGARD_TOYS=$(abspath $(HANGARD_TOYDIR)) HANGAR_MSG_SRC=$(abspath msg) \
	    JOYSTICK_BIN=$(abspath $(JOYSTICK_BIN)) \
	    $(if $(TIMEOUT),$(TIMEOUT) 120) $(abspath $<) > $(TESTDIR)/joystick.log 2>&1; then \
	  echo "ok    joystick"; \
	else \
	  echo "FAIL  joystick"; cat $(TESTDIR)/joystick.log; exit 1; \
	fi
