# thrusters (modules/thrusters/README.md), binary hangar-thrusters, and its
# suite. Included by the root Makefile after lib/sim's rules (SIM_SRC,
# SIM_OBJDIR: the allocation is built from lib/sim's frame, allocation matrix
# and T200 table, so the binary links those two files of lib/sim; the suite
# checks both mixings against lib/sim's thrusters and flies its model).
# GPL-3.0-or-later; C11; libc (libm), lib/hg, lib/sim and the generated msg/
# types.
#
#   make thrusters-all     builddir/hangar-thrusters
#   make check-thrusters   the suite (part of check): the mixing
#   make check-sim_direct  profiles/sim-direct flown under a manager, no ArduSub
#   make check-thrusters_run  the binary under a manager with the real safety
#                          module: its use of the gate, neutral on SIGTERM

THR_MSG_SRC := msg/thruster_cmd.c msg/thruster_out.c msg/wrench.c
THR_CPPFLAGS := -D_FILE_OFFSET_BITS=64 -Ilib/hg -Ilib/common -Ilib/sim -Imsg -Imodules/common -Imodules/thrusters
THR_BIN := $(BUILDDIR)/hangar-thrusters
THR_TEST := $(TESTDIR)/test_thrusters

$(OBJDIR)/modules/thrusters/%.o: EE_CFLAGS := $(HG_CFLAGS)
$(OBJDIR)/modules/thrusters/%.o: EE_CPPFLAGS := $(THR_CPPFLAGS)

THR_SIM_OBJ := $(SIM_OBJDIR)/lib/sim/sim_thrust.o $(SIM_OBJDIR)/lib/sim/sim_frame.o
THR_OBJ := $(OBJDIR)/modules/thrusters/thr_mix.o $(OBJDIR)/modules/thrusters/thr_alloc.o

$(THR_BIN): $(OBJDIR)/modules/thrusters/thrusters.o $(THR_OBJ) $(THR_SIM_OBJ) $(call obj,$(HG_SRC)) \
		$(call obj,$(THR_MSG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) -lm $(LDLIBS)

.PHONY: thrusters-all
thrusters-all: $(THR_BIN)
all: $(THR_BIN)

$(OBJDIR)/modules/thrusters/tests/test_thrusters.o: EE_CPPFLAGS := $(THR_CPPFLAGS) -Imodules/depth_hold

$(THR_TEST): $(OBJDIR)/modules/thrusters/tests/test_thrusters.o $(THR_OBJ) $(OBJDIR)/modules/depth_hold/dh_ctl.o \
		$(patsubst %.c,$(SIM_OBJDIR)/%.o,$(SIM_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ -lm $(LDLIBS)

.PHONY: check-thrusters
check-thrusters: $(THR_TEST)
	@if $(if $(TIMEOUT),$(TIMEOUT) 30) $(abspath $<) > $(TESTDIR)/thrusters.log 2>&1; then \
	  echo "ok    thrusters"; \
	else \
	  echo "FAIL  thrusters"; cat $(TESTDIR)/thrusters.log; exit 1; \
	fi

# The binary under a manager (hangard/tests/hd_test.h), with hangar-safety.
THR_RUN_TEST := $(TESTDIR)/test_thrusters_run
THR_RUN_MSG := msg/thruster_cmd.c msg/wrench.c msg/safety_state.c
$(OBJDIR)/modules/thrusters/tests/test_thrusters_run.o: EE_CPPFLAGS := $(THR_CPPFLAGS) -Ihangard/tests

$(THR_RUN_TEST): $(OBJDIR)/modules/thrusters/tests/test_thrusters_run.o $(call obj,$(HG_SRC)) $(call obj,$(THR_RUN_MSG))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) -lm $(LDLIBS)

.PHONY: check-thrusters_run
check-thrusters_run: $(THR_RUN_TEST) $(THR_BIN) $(BUILDDIR)/hangar-safety $(BUILDDIR)/hangard $(BUILDDIR)/hangar
	@if HANGARD_BIN=$(abspath $(BUILDDIR)/hangard) HANGAR_BIN=$(abspath $(BUILDDIR)/hangar) \
	    HANGAR_MSG_SRC=$(abspath msg) THRUSTERS_BIN=$(abspath $(THR_BIN)) \
	    SAFETY_BIN=$(abspath $(BUILDDIR)/hangar-safety) \
	    $(if $(TIMEOUT),$(TIMEOUT) 120) $(abspath $<) > $(TESTDIR)/thrusters_run.log 2>&1; then \
	  echo "ok    thrusters_run"; \
	else \
	  echo "FAIL  thrusters_run"; cat $(TESTDIR)/thrusters_run.log; exit 1; \
	fi
check: check-thrusters_run

# profiles/sim-direct under a manager (modules/thrusters/tests/test_sim_direct.c):
# the profile's manifests, parameters and frame file, with this build's
# safety, mux, nav, depth_hold, thrusters and sim_vehicle. The other modules'
# makefiles come after this one, so their binaries are named here by path.
THR_DIRECT_TEST := $(TESTDIR)/test_sim_direct
THR_DIRECT_MSG := msg/thruster_cmd.c msg/wrench.c msg/safety_state.c msg/sim_pose.c msg/mux_state.c
THR_DIRECT_BINS := $(THR_BIN) $(BUILDDIR)/hangar-safety $(BUILDDIR)/hangar-mux $(BUILDDIR)/hangar-nav \
	$(BUILDDIR)/hangar-depth-hold $(BUILDDIR)/hangar-sim-vehicle $(BUILDDIR)/hangard $(BUILDDIR)/hangar
$(OBJDIR)/modules/thrusters/tests/test_sim_direct.o: EE_CPPFLAGS := $(THR_CPPFLAGS) -Ihangard/tests

$(THR_DIRECT_TEST): $(OBJDIR)/modules/thrusters/tests/test_sim_direct.o $(OBJDIR)/modules/thrusters/thr_alloc.o \
		$(THR_SIM_OBJ) $(call obj,$(HG_SRC)) $(call obj,$(THR_DIRECT_MSG))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) -lm $(LDLIBS)

.PHONY: check-sim_direct
check-sim_direct: $(THR_DIRECT_TEST) $(THR_DIRECT_BINS)
	@if HANGARD_BIN=$(abspath $(BUILDDIR)/hangard) HANGAR_BIN=$(abspath $(BUILDDIR)/hangar) \
	    HANGAR_MSG_SRC=$(abspath msg) HANGAR_BUILD=$(abspath $(BUILDDIR)) \
	    $(if $(TIMEOUT),$(TIMEOUT) 300) $(abspath $<) > $(TESTDIR)/sim_direct.log 2>&1; then \
	  echo "ok    sim_direct"; \
	else \
	  echo "FAIL  sim_direct"; cat $(TESTDIR)/sim_direct.log; exit 1; \
	fi
check: check-sim_direct
