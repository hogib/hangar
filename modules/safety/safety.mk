# The safety module (modules/safety/README.md), binary hangar-safety, and its
# suite. Included by the root Makefile after modules/console/console.mk;
# relies on OBJDIR, TESTDIR, BUILDDIR, HG_SRC, HG_CFLAGS, HG_LDLIBS, TIMEOUT,
# ALL_LDFLAGS, the generic $(OBJDIR)/%.o rule and hangard.mk's toy modules.
# GPL-3.0-or-later; C11; libc, lib/hg and the generated msg/ types only, so it
# builds static for musl.
#
#   make safety-all       builddir/hangar-safety
#   make check-safety     the suite (part of check): a manager, the safety
#                         module, a fake mux and the suite itself as the
#                         vehicle's mavlink module

SAFETY_MSG_SRC := msg/safety_state.c msg/safety_params.c msg/mux_params.c msg/leak.c msg/battery.c \
	msg/depth.c msg/ardusub_state.c msg/arm_req.c msg/arm_rep.c msg/check_rep.c msg/empty.c \
	msg/hangar_modules.c msg/hangar_system.c
SAFETY_CPPFLAGS := -D_FILE_OFFSET_BITS=64 -Ilib/hg -Ilib/common -Imsg -Imodules/common -Ihangard/tests
SAFETY_BIN := $(BUILDDIR)/hangar-safety
SAFETY_FAKE_MUX := $(TESTDIR)/safety-modules/fake_mux
SAFETY_TEST := $(TESTDIR)/test_safety

$(OBJDIR)/modules/safety/%.o: EE_CFLAGS := $(HG_CFLAGS)
$(OBJDIR)/modules/safety/%.o: EE_CPPFLAGS := $(SAFETY_CPPFLAGS)

$(SAFETY_BIN): $(OBJDIR)/modules/safety/safety.o $(call obj,$(HG_SRC)) $(call obj,$(SAFETY_MSG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) -lm $(LDLIBS)

$(SAFETY_FAKE_MUX): $(OBJDIR)/modules/safety/tests/fake_mux.o $(call obj,$(HG_SRC)) $(OBJDIR)/msg/mux_params.o
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) -lm $(LDLIBS)

.PHONY: safety-all
safety-all: $(SAFETY_BIN)
all: $(SAFETY_BIN)

$(SAFETY_TEST): $(OBJDIR)/modules/safety/tests/test_safety.o $(call obj,$(HG_SRC)) $(call obj,$(SAFETY_MSG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) -lm $(LDLIBS)

.PHONY: check-safety
check-safety: $(SAFETY_TEST) $(SAFETY_BIN) $(SAFETY_FAKE_MUX) $(BUILDDIR)/hangard $(BUILDDIR)/hangar \
		$(HANGARD_TOYDIR)/talker $(HANGARD_TOYDIR)/crasher
	@if HANGARD_BIN=$(abspath $(BUILDDIR)/hangard) HANGAR_BIN=$(abspath $(BUILDDIR)/hangar) \
	    HANGARD_TOYS=$(abspath $(HANGARD_TOYDIR)) HANGAR_MSG_SRC=$(abspath msg) \
	    SAFETY_BIN=$(abspath $(SAFETY_BIN)) FAKE_MUX_BIN=$(abspath $(SAFETY_FAKE_MUX)) \
	    $(if $(TIMEOUT),$(TIMEOUT) 120) $(abspath $<) > $(TESTDIR)/safety.log 2>&1; then \
	  echo "ok    safety"; \
	else \
	  echo "FAIL  safety"; cat $(TESTDIR)/safety.log; exit 1; \
	fi
