# The recorder, the pairer, the .hgr and dataset pieces of lib/hg, the
# `hangar dataset` subcommand and their suites (modules/recorder/README.md,
# modules/pairer/README.md, docs/06-recorder.md). Included by the root
# Makefile after modules/console/console.mk; relies on OBJDIR, TESTDIR,
# BUILDDIR, HG_SRC, HG_CFLAGS, HG_LDLIBS, TIMEOUT, ALL_LDFLAGS, PYTHON, the
# generic $(OBJDIR)/%.o rule, hangard.mk's $(BUILDDIR)/hangar rule and toy
# modules. The modules are GPL-3.0-or-later; the library pieces
# (lib/hg/hg_record.c, hg_dataset.c, hg_ini.c) LGPL like the rest of lib/hg.
# C11; libc, pthreads and lib/hg only, so everything builds static for musl.
#
#   make recorder-all       builddir/recorder, builddir/pairer, builddir/hangar (with `dataset`)
#   make check-recording    the four suites (part of check): rec_hgr, rec_dataset, rec_recorder, rec_pairer
#                           (the root Makefile has a legacy `recorder` suite, electric-eye's)
#   make check-tsan-recording / check-asan-recording
#                           the hgr and dataset suites under a sanitizer (run by check-tsan/check-asan)

HGR_SRC := lib/hg/hg_record.c lib/hg/hg_dataset.c lib/hg/hg_ini.c
RECORDER_SRC := modules/recorder/recorder.c modules/recorder/recorder_conf.c modules/recorder/writer.c \
	modules/recorder/blackbox.c
RECORDER_MSG_SRC := msg/recorder_status.c msg/recorder_session.c msg/recorder_start.c msg/recorder_clock.c \
	msg/empty.c
PAIRER_SRC := modules/pairer/pairer.c
PAIRER_MSG_SRC := msg/pairer_status.c
RECORDER_CPPFLAGS := -D_FILE_OFFSET_BITS=64 -Ilib/hg -Ilib/common -Imsg -Imodules/recorder -Imodules/pairer -Ihangard/tests -Ihangard/tests/modules
RECORDER_BIN := $(BUILDDIR)/recorder
PAIRER_BIN := $(BUILDDIR)/pairer
RECORDER_TOYDIR := $(TESTDIR)/recorder-modules
RECORDER_TOYS := prov_chain
RECORDER_TESTS := rec_hgr rec_dataset rec_recorder
RECORDER_LIB_TESTS := rec_hgr rec_dataset

$(OBJDIR)/modules/recorder/%.o: EE_CFLAGS := $(HG_CFLAGS)
$(OBJDIR)/modules/recorder/%.o: EE_CPPFLAGS := $(RECORDER_CPPFLAGS)
$(OBJDIR)/modules/pairer/%.o: EE_CFLAGS := $(HG_CFLAGS)
$(OBJDIR)/modules/pairer/%.o: EE_CPPFLAGS := $(RECORDER_CPPFLAGS)

$(RECORDER_BIN): $(call obj,$(RECORDER_SRC)) $(call obj,$(HGR_SRC)) $(call obj,$(HG_SRC)) $(call obj,$(RECORDER_MSG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) $(LDLIBS)

$(PAIRER_BIN): $(call obj,$(PAIRER_SRC)) $(call obj,$(HGR_SRC)) $(call obj,$(HG_SRC)) $(call obj,$(PAIRER_MSG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) $(LDLIBS)

# `hangar dataset` (hangar/hangar_dataset.c): one more object and the library
# pieces, added to hangard.mk's rule for builddir/hangar (make merges the
# prerequisites of a target given in several rules, and $^ names them all).
$(OBJDIR)/hangar/hangar_dataset.o: EE_CPPFLAGS := $(RECORDER_CPPFLAGS) -Ihangard
$(BUILDDIR)/hangar: $(OBJDIR)/hangar/hangar_dataset.o $(call obj,$(HGR_SRC))

# The toy publisher of the suites: a provenance chain a -> b -> c.
$(RECORDER_TOYDIR)/%: $(OBJDIR)/modules/recorder/tests/modules/%.o $(call obj,$(HG_SRC)) $(OBJDIR)/msg/depth.o $(OBJDIR)/msg/imu.o $(OBJDIR)/msg/wrench.o
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) $(LDLIBS)

.SECONDARY: $(addsuffix .o,$(addprefix $(OBJDIR)/modules/recorder/tests/modules/,$(RECORDER_TOYS)))

.PHONY: recorder-all
recorder-all: $(RECORDER_BIN) $(PAIRER_BIN) $(BUILDDIR)/hangar $(addprefix $(RECORDER_TOYDIR)/,$(RECORDER_TOYS))
all: $(RECORDER_BIN) $(PAIRER_BIN)

# The suites. hgr and dataset link the library alone; recorder and pairer
# drive the built binaries by their paths from the environment, each under a
# scratch $HANGAR_ROOT on tmpfs. With HGR_FIXTURE_DIR set, the hgr and
# dataset suites also write the small files python/tests/fixtures/ holds.
define recorder_test_rule
$(TESTDIR)/test_$(1): $(OBJDIR)/modules/recorder/tests/test_$(1).o $(call obj,$(HGR_SRC)) $(call obj,$(HG_SRC)) \
		$(OBJDIR)/lib/hg/hg_manager_stub.o $(OBJDIR)/msg/depth.o $(OBJDIR)/msg/imu.o $(OBJDIR)/msg/wrench.o \
		$(OBJDIR)/msg/empty.o $(OBJDIR)/msg/recorder_start.o $(OBJDIR)/msg/recorder_status.o \
		$(OBJDIR)/msg/recorder_session.o $(OBJDIR)/msg/pairer_status.o $(OBJDIR)/msg/recorder_clock.o \
		$(OBJDIR)/msg/safety_state.o $(OBJDIR)/msg/hangar_system.o $(OBJDIR)/msg/hangar_modules.o
	@mkdir -p $$(dir $$@)
	$$(CC) $$(HG_CFLAGS) $$(CFLAGS) $$(RECORDER_CPPFLAGS) $$(ALL_LDFLAGS) -o $$@ $$^ $$(HG_LDLIBS) $$(LDLIBS)
endef
$(foreach t,$(RECORDER_TESTS),$(eval $(call recorder_test_rule,$(t))))

RECORDER_CHECKS := $(addprefix check-,$(RECORDER_TESTS))
.PHONY: $(RECORDER_CHECKS) check-recording check-tsan-recording check-asan-recording check-recording-py
$(RECORDER_CHECKS): check-%: $(TESTDIR)/test_% $(RECORDER_BIN) $(PAIRER_BIN) $(BUILDDIR)/hangar $(BUILDDIR)/hangard \
		$(addprefix $(RECORDER_TOYDIR)/,$(RECORDER_TOYS)) $(HANGARD_TOYDIR)/talker
	@if HANGARD_BIN=$(abspath $(BUILDDIR)/hangard) HANGAR_BIN=$(abspath $(BUILDDIR)/hangar) \
	    HANGARD_TOYS=$(abspath $(HANGARD_TOYDIR)) HANGAR_MSG_SRC=$(abspath msg) \
	    RECORDER_BIN=$(abspath $(RECORDER_BIN)) PAIRER_BIN=$(abspath $(PAIRER_BIN)) \
	    RECORDER_TOYS=$(abspath $(RECORDER_TOYDIR)) \
	    $(if $(TIMEOUT),$(TIMEOUT) 120) $(abspath $<) > $(TESTDIR)/$*.log 2>&1; then \
	  echo "ok    $*"; \
	else \
	  echo "FAIL  $*"; cat $(TESTDIR)/$*.log; exit 1; \
	fi

# The Python readers against the checked-in fixtures (python/tests/test_record.py,
# test_dataset.py run with the schema tests by check-msggen's discover; this
# target runs them alone, for `make check-recorder`).
check-recording-py:
	@mkdir -p $(TESTDIR); \
	if $(PYTHON) -m unittest python/tests/test_record.py python/tests/test_dataset.py > $(TESTDIR)/recording_py.log 2>&1; then \
	  echo "ok    recording py"; \
	else echo "FAIL  recording py"; cat $(TESTDIR)/recording_py.log; exit 1; fi

check-recording: $(RECORDER_CHECKS) check-recording-py

# The library-side suites under the sanitizers, as check-tsan/check-asan run
# lib/hg's (same probe, same flags, same build directories).
check-tsan-recording:
	@mkdir -p $(BUILDDIR); \
	if echo 'int main(void){return 0;}' | $(CC) -fsanitize=thread -x c - -o $(BUILDDIR)/.san-probe-tsan >/dev/null 2>&1; then \
	  $(MAKE) --no-print-directory BUILDDIR=$(BUILDDIR)/tsan \
	    CFLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=thread" LDFLAGS="-fsanitize=thread" \
	    $(addprefix check-,$(RECORDER_LIB_TESTS)); \
	else echo "skip  tsan recording: $(CC) cannot build with -fsanitize=thread"; fi
check-asan-recording:
	@mkdir -p $(BUILDDIR); \
	if echo 'int main(void){return 0;}' | $(CC) -fsanitize=address,undefined -x c - -o $(BUILDDIR)/.san-probe-asan >/dev/null 2>&1; then \
	  $(MAKE) --no-print-directory BUILDDIR=$(BUILDDIR)/asan \
	    CFLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined" LDFLAGS="-fsanitize=address,undefined" \
	    $(addprefix check-,$(RECORDER_LIB_TESTS)); \
	else echo "skip  asan recording: $(CC) cannot build with -fsanitize=address,undefined"; fi
check-tsan: check-tsan-recording
check-asan: check-asan-recording
