# The MAVLink modules (modules/mavlink/README.md): one binary, hangar-mavlink,
# the vehicle's `mavlink` module, the topside's `topside_mavlink` (--topside)
# and the vehicle module's Neutral command (--neutral); the framing suite, the
# fake ArduSub, the vehicle and topside suites, the two-manager chain, and the
# SITL target. Included by the root Makefile after modules/mux/mux.mk; relies
# on OBJDIR, TESTDIR, BUILDDIR, HG_SRC, HG_CFLAGS, HG_LDLIBS, TIMEOUT,
# ALL_LDFLAGS, PYTHON and the generic $(OBJDIR)/%.o rule. GPL-3.0-or-later; C11;
# libc (libm), lib/hg and the generated msg/ types only: static for musl.
#
#   make mavlink-all          builddir/hangar-mavlink
#   make check-mavgen         mavlink_gen.h is what tools/mavgen generates (part of check)
#   make check-mav_frame      framing, CRC, CRC_EXTRA, signing against known bytes
#   make check-mavlink_vehicle   the vehicle module against the fake ArduSub
#   make check-mavlink_topside   the topside module against a fake QGroundControl
#   make check-phase1_chain      two managers, both roles, the whole chain
#   make check-sitl           the chain against the real SITL container (not in check):
#                             HANGAR_SITL=1 and tools/sim/run.sh up with HANGAR_SIM=1

MAVLINK_SRC := modules/mavlink/mavlink_main.c modules/mavlink/mavlink_vehicle.c modules/mavlink/mavlink_topside.c \
	modules/mavlink/mavlink_conf.c modules/mavlink/mav.c modules/mavlink/sha256.c
MAVLINK_MSG_SRC := msg/wrench.c msg/manual_control.c msg/ardusub_state.c msg/ardusub_param.c msg/attitude.c \
	msg/imu.c msg/depth.c msg/battery.c msg/thruster_out.c msg/arm_req.c msg/arm_rep.c msg/empty.c \
	msg/safety_state.c msg/pilot_buttons.c msg/hangar_system.c
MAVLINK_CPPFLAGS := -D_FILE_OFFSET_BITS=64 -Ilib/hg -Ilib/common -Imsg -Imodules/common -Imodules/mavlink -Ihangard/tests
MAVLINK_BIN := $(BUILDDIR)/hangar-mavlink
MAVLINK_FAKE := $(TESTDIR)/mavlink-modules/fake_ardusub
MAVLINK_TESTS := mav_frame mavlink_vehicle mavlink_topside phase1_chain
MAVLINK_GEN := modules/mavlink/mavlink_gen.h

$(OBJDIR)/modules/mavlink/%.o: EE_CFLAGS := $(HG_CFLAGS)
$(OBJDIR)/modules/mavlink/%.o: EE_CPPFLAGS := $(MAVLINK_CPPFLAGS)

$(MAVLINK_BIN): $(call obj,$(MAVLINK_SRC)) $(call obj,$(HG_SRC)) $(call obj,$(MAVLINK_MSG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) -lm $(LDLIBS)

$(MAVLINK_FAKE): $(OBJDIR)/modules/mavlink/tests/fake_ardusub.o $(OBJDIR)/modules/mavlink/mav.o \
		$(OBJDIR)/modules/mavlink/sha256.o $(OBJDIR)/modules/mavlink/mavlink_conf.o $(OBJDIR)/lib/hg/hg_wiring.o
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ -lm $(LDLIBS)

.PHONY: mavlink-all
mavlink-all: $(MAVLINK_BIN)
all: $(MAVLINK_BIN)

# The framing suite needs no manager.
$(TESTDIR)/test_mav_frame: $(OBJDIR)/modules/mavlink/tests/test_mav_frame.o $(OBJDIR)/modules/mavlink/mav.o \
		$(OBJDIR)/modules/mavlink/sha256.o
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(LDLIBS)

define mavlink_test_rule
$(TESTDIR)/test_$(1): $(OBJDIR)/modules/mavlink/tests/test_$(1).o $(OBJDIR)/modules/mavlink/mav.o \
		$(OBJDIR)/modules/mavlink/sha256.o $(OBJDIR)/modules/mavlink/mavlink_conf.o $(call obj,$(HG_SRC)) \
		$(call obj,$(MAVLINK_MSG_SRC)) $(OBJDIR)/msg/mux_params.o $(OBJDIR)/msg/safety_params.o
	@mkdir -p $$(dir $$@)
	$$(CC) $$(HG_CFLAGS) $$(CFLAGS) $$(ALL_LDFLAGS) -o $$@ $$^ $$(HG_LDLIBS) -lm $$(LDLIBS)
endef
$(foreach t,mavlink_vehicle mavlink_topside phase1_chain,$(eval $(call mavlink_test_rule,$(t))))

MAVLINK_TEST_ENV := HANGARD_BIN=$(abspath $(BUILDDIR)/hangard) HANGAR_BIN=$(abspath $(BUILDDIR)/hangar) \
	HANGARD_TOYS=$(abspath $(HANGARD_TOYDIR)) HANGAR_MSG_SRC=$(abspath msg) \
	MAVLINK_BIN=$(abspath $(MAVLINK_BIN)) FAKE_ARDUSUB_BIN=$(abspath $(MAVLINK_FAKE)) \
	SAFETY_BIN=$(abspath $(BUILDDIR)/hangar-safety) MUX_BIN=$(abspath $(BUILDDIR)/hangar-mux)

.PHONY: check-mavgen check-mav_frame check-mavlink_vehicle check-mavlink_topside check-phase1_chain check-sitl
check-mavgen:
	@if $(PYTHON) -I tools/mavgen/mavgen.py --check $(MAVLINK_GEN) 2>/dev/null; then \
	  echo "ok    mavgen --check  $(MAVLINK_GEN) is what tools/mavgen generates"; \
	else echo "FAIL  mavgen --check: run python3 -I tools/mavgen/mavgen.py --out $(MAVLINK_GEN)"; exit 1; fi

check-mav_frame: $(TESTDIR)/test_mav_frame
	$(call run_suite,mav_frame,60,$<)

check-mavlink_vehicle check-mavlink_topside check-phase1_chain: check-%: $(TESTDIR)/test_% $(MAVLINK_BIN) \
		$(MAVLINK_FAKE) $(BUILDDIR)/hangard $(BUILDDIR)/hangar $(BUILDDIR)/hangar-safety $(BUILDDIR)/hangar-mux
	@if $(MAVLINK_TEST_ENV) $(if $(TIMEOUT),$(TIMEOUT) 120) $(abspath $<) > $(TESTDIR)/$*.log 2>&1; then \
	  echo "ok    $*"; \
	else \
	  echo "FAIL  $*"; cat $(TESTDIR)/$*.log; exit 1; \
	fi

.PHONY: check-mavlink
check-mavlink: check-mavgen check-mav_frame check-mavlink_vehicle check-mavlink_topside check-phase1_chain

# The same chain against the real container (tools/sim/SETUP.md): the vehicle
# module talks to SITL through MAVProxy's second output, 127.0.0.1:14560, which
# launch.sh adds when HANGAR_SIM=1. Not part of check: it needs Docker, the
# container up, and HANGAR_SITL=1 as the operator's say-so.
check-sitl: $(TESTDIR)/test_phase1_chain $(MAVLINK_BIN) $(BUILDDIR)/hangard $(BUILDDIR)/hangar \
		$(BUILDDIR)/hangar-safety $(BUILDDIR)/hangar-mux
	@if [ "$(HANGAR_SITL)" != 1 ]; then \
	  echo "skip  sitl: set HANGAR_SITL=1 with the container up (tools/sim/run.sh up, HANGAR_SIM=1)"; \
	elif ! docker inspect -f '{{.State.Running}}' hangar-sim 2>/dev/null | grep -q true; then \
	  echo "FAIL  sitl: the container hangar-sim is not running (tools/sim/run.sh up)"; exit 1; \
	elif $(MAVLINK_TEST_ENV) HANGAR_SITL=1 $(if $(TIMEOUT),$(TIMEOUT) 180) $(abspath $<) --sitl > $(TESTDIR)/sitl.log 2>&1; then \
	  echo "ok    sitl"; \
	else \
	  echo "FAIL  sitl"; cat $(TESTDIR)/sitl.log; exit 1; \
	fi
