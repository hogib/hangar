# sim_vehicle (modules/sim_vehicle/README.md), binary hangar-sim-vehicle, and
# its suite. Included by the root Makefile after lib/sim's rules (SIM_SRC,
# SIM_OBJDIR); relies on OBJDIR, TESTDIR, BUILDDIR, HG_SRC, HG_CFLAGS,
# HG_LDLIBS, TIMEOUT, ALL_LDFLAGS and the generic $(OBJDIR)/%.o rule.
# GPL-3.0-or-later; C11; libc (libm), lib/hg, lib/sim and the generated msg/
# types only, so it builds static for musl.
#
#   make sim_vehicle-all     builddir/hangar-sim-vehicle
#   make check-sim_vehicle   the suite (part of check): the physics through the
#                            module's core, the module standalone under a
#                            manager, and SITL's JSON interface against a fake
#                            SITL; HANGAR_SITL=1 adds the real container (README)

SV_MSG_SRC := msg/sim_pose.c msg/sim_velocity.c msg/sim_thrust_truth.c msg/thruster_cmd.c \
	msg/sim_vehicle_params.c msg/imu.c msg/attitude.c msg/depth.c msg/battery.c
SV_CPPFLAGS := -D_FILE_OFFSET_BITS=64 -Ilib/hg -Ilib/common -Ilib/sim -Imsg -Imodules/common \
	-Imodules/sim_vehicle -Ihangard/tests
SV_SIM_OBJ := $(patsubst %.c,$(SIM_OBJDIR)/%.o,$(SIM_SRC))
SV_BIN := $(BUILDDIR)/hangar-sim-vehicle
SV_TEST := $(TESTDIR)/test_sim_vehicle

$(OBJDIR)/modules/sim_vehicle/%.o: EE_CFLAGS := $(HG_CFLAGS)
$(OBJDIR)/modules/sim_vehicle/%.o: EE_CPPFLAGS := $(SV_CPPFLAGS)

$(SV_BIN): $(OBJDIR)/modules/sim_vehicle/sim_vehicle.o $(OBJDIR)/modules/sim_vehicle/sv.o $(SV_SIM_OBJ) \
		$(call obj,$(HG_SRC)) $(call obj,$(SV_MSG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) -lm $(LDLIBS)

.PHONY: sim_vehicle-all
sim_vehicle-all: $(SV_BIN)
all: $(SV_BIN)

$(SV_TEST): $(OBJDIR)/modules/sim_vehicle/tests/test_sim_vehicle.o $(OBJDIR)/modules/sim_vehicle/sv.o $(SV_SIM_OBJ) \
		$(call obj,$(HG_SRC)) $(call obj,$(SV_MSG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) -lm $(LDLIBS)

.PHONY: check-sim_vehicle
check-sim_vehicle: $(SV_TEST) $(SV_BIN) $(BUILDDIR)/hangard $(BUILDDIR)/hangar
	@if HANGARD_BIN=$(abspath $(BUILDDIR)/hangard) HANGAR_BIN=$(abspath $(BUILDDIR)/hangar) \
	    HANGARD_TOYS=$(abspath $(HANGARD_TOYDIR)) HANGAR_MSG_SRC=$(abspath msg) \
	    SV_BIN=$(abspath $(SV_BIN)) \
	    $(if $(TIMEOUT),$(TIMEOUT) 120) $(abspath $<) > $(TESTDIR)/sim_vehicle.log 2>&1; then \
	  echo "ok    sim_vehicle"; \
	else \
	  echo "FAIL  sim_vehicle"; cat $(TESTDIR)/sim_vehicle.log; exit 1; \
	fi
