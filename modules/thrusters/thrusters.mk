# thrusters (modules/thrusters/README.md), binary hangar-thrusters, and its
# suite. Included by the root Makefile after lib/sim's rules (SIM_SRC,
# SIM_OBJDIR: the suite checks the mixing against lib/sim's thrusters; the
# binary does not link lib/sim). GPL-3.0-or-later; C11; libc (libm), lib/hg
# and the generated msg/ types.
#
#   make thrusters-all     builddir/hangar-thrusters
#   make check-thrusters   the suite (part of check): the mixing
#   make check-thrusters_run  the binary under a manager with the real safety
#                          module: its use of the gate, neutral on SIGTERM

THR_MSG_SRC := msg/thruster_cmd.c msg/thruster_out.c msg/wrench.c
THR_CPPFLAGS := -D_FILE_OFFSET_BITS=64 -Ilib/hg -Ilib/common -Ilib/sim -Imsg -Imodules/common -Imodules/thrusters
THR_BIN := $(BUILDDIR)/hangar-thrusters
THR_TEST := $(TESTDIR)/test_thrusters

$(OBJDIR)/modules/thrusters/%.o: EE_CFLAGS := $(HG_CFLAGS)
$(OBJDIR)/modules/thrusters/%.o: EE_CPPFLAGS := $(THR_CPPFLAGS)

$(THR_BIN): $(OBJDIR)/modules/thrusters/thrusters.o $(OBJDIR)/modules/thrusters/thr_mix.o $(call obj,$(HG_SRC)) \
		$(call obj,$(THR_MSG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) -lm $(LDLIBS)

.PHONY: thrusters-all
thrusters-all: $(THR_BIN)
all: $(THR_BIN)

$(THR_TEST): $(OBJDIR)/modules/thrusters/tests/test_thrusters.o $(OBJDIR)/modules/thrusters/thr_mix.o \
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
