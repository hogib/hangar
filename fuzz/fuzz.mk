# Fuzzing (fuzz/README.md). Included by the root Makefile last, after every
# module's .mk; relies on BUILDDIR, OBJDIR, TESTDIR, obj, HG_SRC, HG_CFLAGS,
# HGR_SRC, NN_SRC, LIB_CPPFLAGS, ALL_LDFLAGS, TIMEOUT and the generic
# $(OBJDIR)/%.o rule.
#
#   make fuzz-build              every harness with libFuzzer, ASan and UBSan (needs clang)
#   make fuzz [FUZZ_SECONDS=30]  runs each for FUZZ_SECONDS; fails on a crash (-j2 runs two at once)
#   make fuzz-run-<name>         one harness
#   make check-fuzz-corpus       part of check: every harness, without the fuzzer engine and with
#                                $(CC), over its corpus in fuzz/corpus/<name>/
#   make check-asan-fuzz-corpus  part of check-asan: the same under ASan and UBSan
#   make fuzz-seeds              rewrites the generated seeds (fuzz/mkseeds.c)
#
# A harness is fuzz/fuzz_<name>.c. Per harness: fuzz_<name>_SRC (what it links),
# fuzz_<name>_DEPS (the .c files it #includes, for the fuzzer build's rebuilds),
# _CPPFLAGS, _LDLIBS, _STD (default -std=c11), _MAXLEN (the longest input the
# fuzzer makes).

FUZZ_CC ?= clang
FUZZ_SECONDS ?= 30
FUZZ_DIR := $(BUILDDIR)/fuzz
FUZZ_FLAGS := -g -O1 -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined
FUZZ_CPPFLAGS := -D_FILE_OFFSET_BITS=64 -Ifuzz -Ilib/hg -Ilib/common -Imsg -Ihangard -Imodules/common \
	-Imodules/bridge -Imodules/mavlink -Imodules/recorder -Imodules/encoder -Imodules/video_relay -Imodules/joystick -Ihangar -DFUZZ_REPO='"$(CURDIR)"'
FUZZ_HDRS := $(wildcard fuzz/*.h lib/hg/*.h lib/common/*.h hangard/*.h msg/*.h modules/*/*.h lib/net/*.h lib/video/*.h)

FUZZ_HARNESSES := bridge mavlink_vehicle mavlink_topside console control video schema wiring manifest bridge_conf conf params \
	recording topic nn http

# bridge.c is included by the harness (its statics are what is fuzzed).
fuzz_bridge_SRC := $(BRIDGE_LIB_SRC) $(HG_SRC)
fuzz_bridge_DEPS := modules/bridge/bridge.c
fuzz_bridge_MAXLEN := 8192
# The module's file is included by its harness; the other role's and the link
# (mavlink_main.c, its main() renamed by fuzz/mavlink_main_wrap.c) are linked.
FUZZ_MAVLINK_SRC := fuzz/mavlink_main_wrap.c modules/mavlink/mavlink_conf.c modules/mavlink/mav.c \
	modules/mavlink/sha256.c modules/mavlink/param_backup.c $(MAVLINK_MSG_SRC) $(HG_SRC)
fuzz_mavlink_vehicle_SRC := modules/mavlink/mavlink_topside.c $(FUZZ_MAVLINK_SRC)
fuzz_mavlink_vehicle_DEPS := modules/mavlink/mavlink_vehicle.c modules/mavlink/mavlink_main.c
fuzz_mavlink_vehicle_MAXLEN := 2048
fuzz_mavlink_topside_SRC := modules/mavlink/mavlink_vehicle.c $(FUZZ_MAVLINK_SRC)
fuzz_mavlink_topside_DEPS := modules/mavlink/mavlink_topside.c modules/mavlink/mavlink_main.c
fuzz_mavlink_topside_MAXLEN := 2048
fuzz_video_SRC := modules/video_relay/depack.c modules/video_relay/mp4.c modules/encoder/h264.c
fuzz_video_MAXLEN := 16384
# dawn_patrold.c is included by the harness; lib/net/http.c is gnu99 like the rest of lib/net.
fuzz_console_SRC := modules/console/console_api.c msg/hangar_system.c msg/hangar_modules.c lib/net/http.c $(HG_SRC)
fuzz_console_DEPS := modules/console/dawn_patrold.c
fuzz_console_CPPFLAGS := -Ilib/net -Imodules/console
fuzz_console_STD := -std=gnu11
fuzz_console_MAXLEN := 8192
# control.c is included by the harness; the manager around it is stubs there.
fuzz_control_SRC := hangard/topics.c hangard/registry.c $(HANGARD_CORE_SRC) $(HG_SRC)
fuzz_control_DEPS := hangard/control.c
fuzz_control_MAXLEN := 8192
fuzz_schema_SRC := lib/hg/hg_type.c
fuzz_schema_MAXLEN := 4096
fuzz_wiring_SRC := $(HG_SRC)
fuzz_wiring_MAXLEN := 16384
fuzz_manifest_SRC := $(HANGARD_CORE_SRC) $(HG_SRC)
fuzz_manifest_MAXLEN := 16384
fuzz_bridge_conf_SRC := modules/bridge/bridge_conf.c modules/bridge/blake2s.c lib/hg/hg_ini.c $(HG_SRC)
fuzz_bridge_conf_MAXLEN := 8192
# hangar_params.c is included by the harness; the rest is linked.
fuzz_conf_SRC := hangard/conf.c hangard/registry.c $(HANGARD_CORE_SRC) modules/mavlink/mavlink_conf.c \
	modules/mavlink/param_backup.c modules/recorder/recorder_conf.c modules/joystick/joystick_map.c \
	msg/params_job.c $(HG_SRC)
fuzz_conf_DEPS := hangar/hangar_params.c
fuzz_conf_MAXLEN := 8192
fuzz_params_SRC := $(HG_SRC)
fuzz_params_MAXLEN := 8192
fuzz_nn_SRC := $(NN_SRC)
fuzz_nn_CPPFLAGS := -Ilib/nn
fuzz_nn_MAXLEN := 32768
fuzz_recording_SRC := $(HGR_SRC) $(HG_SRC)
fuzz_recording_MAXLEN := 16384
fuzz_topic_SRC := $(HG_SRC)
fuzz_topic_MAXLEN := 16384
# hellion's and the console's HTTP parsers (lib/net/http.c, gnu99 like the rest of lib/net).
fuzz_http_DEPS := lib/net/http.c
fuzz_http_CPPFLAGS := $(LIB_CPPFLAGS)
fuzz_http_STD := -std=gnu99
fuzz_http_MAXLEN := 9000

fuzz_std = $(if $(fuzz_$(1)_STD),$(fuzz_$(1)_STD),-std=c11)

# Two builds of each harness. The fuzzer's: one clang command over the harness
# and its sources, all instrumented. The regression runner's: the harness and
# fuzz/standalone.c compiled by the build's own rule and linked with the
# objects the programs and suites are made of, so `make check` compiles two
# files per harness, with $(CC) and its flags of the day.
$(OBJDIR)/fuzz/%.o: EE_CFLAGS := $(HG_CFLAGS)
$(OBJDIR)/fuzz/%.o: EE_CPPFLAGS := $(FUZZ_CPPFLAGS)

define fuzz_rule
$(FUZZ_DIR)/fuzz_$(1): fuzz/fuzz_$(1).c $$(fuzz_$(1)_SRC) $$(fuzz_$(1)_DEPS) $$(FUZZ_HDRS)
	@mkdir -p $$(dir $$@)
	$$(FUZZ_CC) $$(FUZZ_FLAGS) $$(call fuzz_std,$(1)) $$(FUZZ_CPPFLAGS) $$(fuzz_$(1)_CPPFLAGS) -o $$@ \
	  fuzz/fuzz_$(1).c $$(fuzz_$(1)_SRC) $$(fuzz_$(1)_LDLIBS) -pthread -lrt -lm
$(OBJDIR)/fuzz/fuzz_$(1).o: EE_CFLAGS := $$(call fuzz_std,$(1)) -Wall -Wextra -pthread
$(OBJDIR)/fuzz/fuzz_$(1).o: EE_CPPFLAGS := $$(FUZZ_CPPFLAGS) $$(fuzz_$(1)_CPPFLAGS)
$(TESTDIR)/fuzzreg_$(1): $(OBJDIR)/fuzz/fuzz_$(1).o $(OBJDIR)/fuzz/standalone.o $$(call obj,$$(fuzz_$(1)_SRC))
	@mkdir -p $$(dir $$@)
	$$(CC) $$(HG_CFLAGS) $$(CFLAGS) $$(ALL_LDFLAGS) -o $$@ $$^ $$(fuzz_$(1)_LDLIBS) -pthread -lrt -lm $$(LDLIBS)
endef
$(foreach h,$(FUZZ_HARNESSES),$(eval $(call fuzz_rule,$(h))))

FUZZ_RUNS := $(addprefix fuzz-run-,$(FUZZ_HARNESSES))
FUZZ_CHECKS := $(addprefix check-fuzz-corpus-,$(FUZZ_HARNESSES))
.PHONY: fuzz fuzz-build fuzz-http check-fuzz-corpus check-asan-fuzz-corpus $(FUZZ_RUNS) $(FUZZ_CHECKS)

fuzz-build: $(foreach h,$(FUZZ_HARNESSES),$(FUZZ_DIR)/fuzz_$(h))

# What a run finds beyond the seeds stays in $(FUZZ_DIR)/corpus-<name>/ for the
# next run; a crash is written to $(FUZZ_DIR)/crashes-<name>/ and fails the
# target. The harness's own stderr is closed (-close_fd_mask=3): the code under
# test logs every refusal; the fuzzer's and the sanitizers' reports still reach
# the log.
$(FUZZ_RUNS): fuzz-run-%: $(FUZZ_DIR)/fuzz_%
	@mkdir -p $(FUZZ_DIR)/corpus-$* $(FUZZ_DIR)/crashes-$*
	@if $< -max_total_time=$(FUZZ_SECONDS) -timeout=20 -rss_limit_mb=3072 -close_fd_mask=3 -print_final_stats=1 \
	     $(if $(fuzz_$*_MAXLEN),-max_len=$(fuzz_$*_MAXLEN)) $(if $(wildcard fuzz/dict/$*.dict),-dict=fuzz/dict/$*.dict) \
	     $(FUZZ_ARGS) -artifact_prefix=$(FUZZ_DIR)/crashes-$*/ $(FUZZ_DIR)/corpus-$* fuzz/corpus/$* \
	     > $(FUZZ_DIR)/$*.log 2>&1; then \
	  echo "ok    fuzz $*  $(FUZZ_SECONDS) s  $$(sed -n 's/^stat::number_of_executed_units: *//p' $(FUZZ_DIR)/$*.log) runs  $$(grep -o 'cov: [0-9]*' $(FUZZ_DIR)/$*.log | tail -1)"; \
	else \
	  echo "FAIL  fuzz $*  (log: $(FUZZ_DIR)/$*.log, input: $(FUZZ_DIR)/crashes-$*/)"; tail -60 $(FUZZ_DIR)/$*.log; exit 1; \
	fi

fuzz: $(FUZZ_RUNS)
fuzz-http: fuzz-run-http

# UBSAN_OPTIONS: in a sanitized build (check-asan-fuzz-corpus) the first
# undefined operation ends the replay; it means nothing to a plain build.
$(FUZZ_CHECKS): check-fuzz-corpus-%: $(TESTDIR)/fuzzreg_%
	@if UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(if $(TIMEOUT),$(TIMEOUT) 120) $< fuzz/corpus/$* \
	     > $(TESTDIR)/fuzzreg_$*.log 2>&1; then \
	  echo "ok    fuzz corpus $*  ($$(tail -1 $(TESTDIR)/fuzzreg_$*.log))"; \
	else \
	  echo "FAIL  fuzz corpus $*"; tail -60 $(TESTDIR)/fuzzreg_$*.log; exit 1; \
	fi

check-fuzz-corpus: $(FUZZ_CHECKS)

# The generated seeds (fuzz/mkseeds.c), written into fuzz/corpus/ and checked in.
FUZZ_SEEDS_SRC := $(HGR_SRC) $(HG_SRC) msg/depth.c msg/wrench.c modules/bridge/bridge_conf.c modules/bridge/blake2s.c \
	modules/mavlink/mav.c modules/mavlink/sha256.c modules/encoder/h264.c
$(FUZZ_DIR)/mkseeds: fuzz/mkseeds.c $(FUZZ_SEEDS_SRC) $(FUZZ_HDRS)
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -pthread -O1 -g $(FUZZ_CPPFLAGS) -o $@ fuzz/mkseeds.c $(FUZZ_SEEDS_SRC) -pthread -lrt -lm
.PHONY: fuzz-seeds
fuzz-seeds: $(FUZZ_DIR)/mkseeds
	$<
check: check-fuzz-corpus

# The corpora again under ASan and UBSan with $(CC), as check-asan runs the
# library's suites (same probe, same flags, same build directory).
check-asan-fuzz-corpus:
	@mkdir -p $(BUILDDIR); \
	if echo 'int main(void){return 0;}' | $(CC) -fsanitize=address,undefined -x c - -o $(BUILDDIR)/.san-probe-asan >/dev/null 2>&1; then \
	  $(MAKE) --no-print-directory BUILDDIR=$(BUILDDIR)/asan \
	    CFLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined" \
	    LDFLAGS="-fsanitize=address,undefined" check-fuzz-corpus; \
	else echo "skip  asan fuzz corpus: $(CC) cannot build with -fsanitize=address,undefined"; fi
check-asan: check-asan-fuzz-corpus
