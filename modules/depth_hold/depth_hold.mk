# depth_hold (modules/depth_hold/README.md), binary hangar-depth-hold, and its
# suite. Included by the root Makefile after modules/mux/mux.mk and
# modules/nav/nav.mk (the suite runs both); relies on OBJDIR, TESTDIR,
# BUILDDIR, HG_SRC, HG_CFLAGS, HG_LDLIBS, TIMEOUT, ALL_LDFLAGS and the generic
# $(OBJDIR)/%.o rule. GPL-3.0-or-later; C11; libc (libm), lib/hg, msg/.
#
#   make depth_hold-all    builddir/hangar-depth-hold
#   make check-depth_hold  the suite (part of check): the controller against a
#                          simulated vehicle, then a manager with nav,
#                          depth_hold and the mux, the suite as the vehicle

DEPTH_HOLD_MSG_SRC := msg/wrench.c msg/nav_state.c msg/mux_state.c msg/safety_state.c msg/depth_target.c \
	msg/depth_control_params.c msg/depth_set_req.c msg/empty.c
DEPTH_HOLD_CPPFLAGS := -D_FILE_OFFSET_BITS=64 -Ilib/hg -Ilib/common -Imsg -Imodules/common -Imodules/depth_hold \
	-Imodules/nav -Ihangard/tests
DEPTH_HOLD_BIN := $(BUILDDIR)/hangar-depth-hold
DEPTH_HOLD_TEST := $(TESTDIR)/test_depth_hold
DEPTH_HOLD_TEST_MSG_SRC := $(DEPTH_HOLD_MSG_SRC) msg/depth.c msg/attitude.c msg/imu.c msg/set_mode_req.c \
	msg/set_mode_rep.c msg/policy_active.c

$(OBJDIR)/modules/depth_hold/%.o: EE_CFLAGS := $(HG_CFLAGS)
$(OBJDIR)/modules/depth_hold/%.o: EE_CPPFLAGS := $(DEPTH_HOLD_CPPFLAGS)

$(DEPTH_HOLD_BIN): $(OBJDIR)/modules/depth_hold/depth_hold.o $(OBJDIR)/modules/depth_hold/dh_ctl.o \
		$(call obj,$(HG_SRC)) $(call obj,$(DEPTH_HOLD_MSG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) -lm $(LDLIBS)

.PHONY: depth_hold-all
depth_hold-all: $(DEPTH_HOLD_BIN)
all: $(DEPTH_HOLD_BIN)

$(DEPTH_HOLD_TEST): $(OBJDIR)/modules/depth_hold/tests/test_depth_hold.o $(OBJDIR)/modules/depth_hold/dh_ctl.o \
		$(call obj,$(HG_SRC)) $(call obj,$(DEPTH_HOLD_TEST_MSG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) -lm $(LDLIBS)

.PHONY: check-depth_hold
check-depth_hold: $(DEPTH_HOLD_TEST) $(DEPTH_HOLD_BIN) $(NAV_BIN) $(MUX_BIN) $(BUILDDIR)/hangard $(BUILDDIR)/hangar
	@if HANGARD_BIN=$(abspath $(BUILDDIR)/hangard) HANGAR_BIN=$(abspath $(BUILDDIR)/hangar) \
	    HANGARD_TOYS=$(abspath $(HANGARD_TOYDIR)) HANGAR_MSG_SRC=$(abspath msg) \
	    DEPTH_HOLD_BIN=$(abspath $(DEPTH_HOLD_BIN)) NAV_BIN=$(abspath $(NAV_BIN)) MUX_BIN=$(abspath $(MUX_BIN)) \
	    $(if $(TIMEOUT),$(TIMEOUT) 120) $(abspath $<) > $(TESTDIR)/depth_hold.log 2>&1; then \
	  echo "ok    depth_hold"; \
	else \
	  echo "FAIL  depth_hold"; cat $(TESTDIR)/depth_hold.log; exit 1; \
	fi
