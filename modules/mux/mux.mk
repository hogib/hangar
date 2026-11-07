# The mux (modules/mux/README.md), binary hangar-mux, and its suite. Included
# by the root Makefile after modules/safety/safety.mk; relies on OBJDIR,
# TESTDIR, BUILDDIR, HG_SRC, HG_CFLAGS, HG_LDLIBS, TIMEOUT, ALL_LDFLAGS and the
# generic $(OBJDIR)/%.o rule. GPL-3.0-or-later; C11; libc (libm), lib/hg and
# the generated msg/ types only, so it builds static for musl.
#
#   make mux-all          builddir/hangar-mux
#   make check-mux        the suite (part of check): a manager, the mux, and
#                         the suite itself as every source and the caller

MUX_MSG_SRC := msg/wrench.c msg/mux_state.c msg/mux_params.c msg/depth.c msg/safety_state.c \
	msg/policy_active.c msg/set_mode_req.c msg/set_mode_rep.c msg/empty.c
MUX_CPPFLAGS := -D_FILE_OFFSET_BITS=64 -Ilib/hg -Ilib/common -Imsg -Imodules/common -Ihangard/tests
MUX_BIN := $(BUILDDIR)/hangar-mux
MUX_TEST := $(TESTDIR)/test_mux

$(OBJDIR)/modules/mux/%.o: EE_CFLAGS := $(HG_CFLAGS)
$(OBJDIR)/modules/mux/%.o: EE_CPPFLAGS := $(MUX_CPPFLAGS)

$(MUX_BIN): $(OBJDIR)/modules/mux/mux.o $(call obj,$(HG_SRC)) $(call obj,$(MUX_MSG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) -lm $(LDLIBS)

.PHONY: mux-all
mux-all: $(MUX_BIN)
all: $(MUX_BIN)

$(MUX_TEST): $(OBJDIR)/modules/mux/tests/test_mux.o $(call obj,$(HG_SRC)) $(call obj,$(MUX_MSG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) -lm $(LDLIBS)

.PHONY: check-mux
check-mux: $(MUX_TEST) $(MUX_BIN) $(BUILDDIR)/hangard $(BUILDDIR)/hangar
	@if HANGARD_BIN=$(abspath $(BUILDDIR)/hangard) HANGAR_BIN=$(abspath $(BUILDDIR)/hangar) \
	    HANGARD_TOYS=$(abspath $(HANGARD_TOYDIR)) HANGAR_MSG_SRC=$(abspath msg) \
	    MUX_BIN=$(abspath $(MUX_BIN)) \
	    $(if $(TIMEOUT),$(TIMEOUT) 120) $(abspath $<) > $(TESTDIR)/mux.log 2>&1; then \
	  echo "ok    mux"; \
	else \
	  echo "FAIL  mux"; cat $(TESTDIR)/mux.log; exit 1; \
	fi
