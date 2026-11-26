# hangar -- GNU make build.
#
# The start of hangar's build. For now it builds the two legacy electric-eye
# programs from legacy/ (their glue) and lib/ (the library code hangar
# keeps), and runs their test suites. Ported from electric-eye's Makefile;
# its comments were kept where they still apply.
#
#   make                   builddir/eeye and builddir/hellion, optimized
#                          with debug info (-O2 -g)
#   make BUILD=release     -O3, no debug info
#   make BUILD=debug       builddir/eeye-d and hellion-d, unoptimized
#   make check             build and run every test suite (-j is fine)
#   make check-hg          only lib/hg's suites (the hangar module library)
#   make check-tsan        lib/hg's suites again under ThreadSanitizer
#   make check-asan        lib/hg's suites again under AddressSanitizer + UBSan
#   make check-asan-modules  the suites of the modules that read frames and
#                          packets (SAN_MODULE_CHECKS), manager and modules
#                          included, under AddressSanitizer + UBSan
#   make check-rov         tools/rov's guard: nothing stopped under an armed vehicle;
#                          and its preflight, judged over written facts
#   make equivalence       hash every lib/vision operator's output and
#                          compare with electric-eye v0.3.1's (tools/equivalence/)
#   make fuzz              every fuzz harness for FUZZ_SECONDS each (fuzz/README.md)
#   make analyze           the static analysers (tools/analyze/README.md)
#   make clean
#
# Written for GNU make 3.81 and a C99 compiler (GCC 4.7+ or clang), so it
# builds on old distros as well as new ones -- Debian 7 and up. Nothing
# here needs make 4.x: no !=, no .ONESHELL, no $(file).
#
# Everything is overridable from the command line or environment: CC,
# HOSTCC, CFLAGS, CPPFLAGS, LDFLAGS, LDLIBS, BUILDDIR, PKG_CONFIG, and
#   NEON=0|1                     default: on (matters on 32-bit ARM only)
#   TURBOJPEG_CFLAGS/_LIBS       default: pkg-config, else -lturbojpeg
#   TIMEOUT                      default: coreutils timeout, where present
# e.g. a libjpeg-turbo built from source into /opt/ljt:
#   make TURBOJPEG_CFLAGS=-I/opt/ljt/include \
#        TURBOJPEG_LIBS='/opt/ljt/lib/libturbojpeg.a'
# A static aarch64 build whose suites run under qemu (see tools/cross/):
#   make BUILD=release CC="zig cc -target aarch64-linux-musl -mcpu=cortex_a76" \
#        HOSTCC="zig cc" LDFLAGS=-static TURBOJPEG_CFLAGS=... TURBOJPEG_LIBS=... \
#        TIMEOUT="qemu-aarch64 -0" check

BUILDDIR ?= builddir
BUILD ?= debugoptimized
PKG_CONFIG ?= pkg-config
# Builds tools that run during the build (the page embedder); differs from
# CC only when cross-compiling.
HOSTCC ?= cc

ifeq ($(BUILD),debugoptimized)
OPTFLAGS := -O2 -g
EXE := eeye
HEXE := hellion
else ifeq ($(BUILD),release)
OPTFLAGS := -O3
EXE := eeye
HEXE := hellion
else ifeq ($(BUILD),debug)
OPTFLAGS := -O0 -g
EXE := eeye-d
HEXE := hellion-d
else
$(error BUILD must be debugoptimized, release or debug, not '$(BUILD)')
endif

# One object directory per build type, so switching BUILD never links
# objects compiled with another type's flags.
OBJDIR := $(BUILDDIR)/obj-$(BUILD)
TESTDIR := $(BUILDDIR)/tests-$(BUILD)

# Layout. lib/<x>/ holds the library code hangar keeps: a file there
# includes only lib/, the system headers and libturbojpeg. legacy/eeye/ and
# legacy/hellion/ hold the two programs' glue (config, the frame bus,
# status, the threads), which may include lib/; lib/ never includes
# legacy/. Each directory's tests sit in its tests/.
LIBDIRS := lib/common lib/vision lib/camera lib/proc lib/net lib/video lib/hg
LEGACYDIRS := legacy/eeye legacy/hellion

# lib/vision's operators run their loops on lib/common's thread pool
# (lib/vision/vision.mk: VISION_SRC, VISION_POOL_SRC, the pool's suite).
include lib/vision/vision.mk
LIB_CPPFLAGS := $(addprefix -I,$(LIBDIRS))
LEGACY_CPPFLAGS := $(LIB_CPPFLAGS) $(addprefix -I,$(LEGACYDIRS))

CFLAGS ?= $(OPTFLAGS)
# _FILE_OFFSET_BITS=64: recordings pass 2GB quickly, and on 32-bit ARM
# (most Pis) off_t is 32 bits without it.
EE_CPPFLAGS := -D_FILE_OFFSET_BITS=64
EE_CFLAGS := -std=gnu99 -Wall -Wextra -pedantic -pthread

# (HASH: make 4.3 changed how "#" inside $(shell ...) is escaped; a
# variable holding one reads the same in every version.)
HASH := \#

# NEON on 32-bit ARM. conv.c and point_opps.c have NEON paths, compiled in
# only when the compiler defines __ARM_NEON -- and the compilers 32-bit Pi
# OS and Debian armhf ship do not by default (armv6 + VFP, or armv7 with
# VFPv3-D16), so a plain `make` there ran every effect scalar: ~3.5-6x the
# instructions for sobel, blur and LoG. On 32-bit hard-float ARM without
# NEON already on, this adds what the release armv7 build uses, if the
# compiler takes it. The binary then needs an ARMv7 with NEON and VFPv4:
# every Pi from the Pi 2 v1.1 on, not the Pi 1 or Zero. NEON=0 leaves the
# compiler's defaults alone. 64-bit ARM always has NEON; x86 is unaffected.
ifeq ($(origin NEON),undefined)
NEON := 1
endif
ifeq ($(NEON),1)
ARM_NEON_FLAGS := -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard
NEED_NEON_FLAGS := $(shell echo | $(CC) $(CFLAGS) -dM -E - 2>/dev/null | \
  grep -q '__ARM_PCS_VFP' && ! (echo | $(CC) $(CFLAGS) -dM -E - 2>/dev/null | \
  grep -q '__ARM_NEON') && echo 'int main(void){return 0;}' | \
  $(CC) $(ARM_NEON_FLAGS) -x c - -o /dev/null >/dev/null 2>&1 && echo 1)
ifeq ($(NEED_NEON_FLAGS),1)
EE_CFLAGS += $(ARM_NEON_FLAGS)
endif
endif

# libturbojpeg (libjpeg-turbo 1.4 or newer: that's where the planar-YUV
# API this uses first appeared). Older distros' pkg-config may not know the
# library even when it is installed, hence the bare -lturbojpeg fallback.
ifeq ($(origin TURBOJPEG_CFLAGS),undefined)
TURBOJPEG_CFLAGS := $(shell $(PKG_CONFIG) --cflags libturbojpeg 2>/dev/null)
endif
ifeq ($(origin TURBOJPEG_LIBS),undefined)
TURBOJPEG_LIBS := $(shell $(PKG_CONFIG) --libs libturbojpeg 2>/dev/null || echo -lturbojpeg)
endif

# -lrt: clock_gettime() lived in librt until glibc 2.17; on anything newer
# it's an empty stub, so linking it unconditionally costs nothing.
EE_LDLIBS := $(TURBOJPEG_LIBS) -pthread -lrt -lm

ALL_CPPFLAGS = $(EE_CPPFLAGS) $(TURBOJPEG_CFLAGS) $(CPPFLAGS)
ALL_CFLAGS = $(EE_CFLAGS) $(CFLAGS)
ALL_LDFLAGS = $(EE_LDFLAGS) $(LDFLAGS)
ALL_LDLIBS = $(EE_LDLIBS) $(LDLIBS)

# eeye: its glue from legacy/eeye/, plus the library code it is built on.
SRC := \
	legacy/eeye/eeye.c \
	legacy/eeye/bus_in.c \
	legacy/eeye/config.c \
	legacy/eeye/fbus.c \
	legacy/eeye/frame_ring_buffer.c \
	legacy/eeye/health.c \
	legacy/eeye/recorder.c \
	legacy/eeye/video_threads.c \
	lib/camera/camera_ctrl.c \
	lib/camera/jpeg_decode.c \
	lib/camera/rpicam_in.c \
	lib/camera/v4l2_in.c \
	lib/camera/v4l2_out.c \
	lib/camera/virtual_cam.c \
	lib/proc/child_proc.c \
	$(VISION_SRC)

# hellion: its own glue, plus the modules it shares with eeye (the frame
# bus from legacy/eeye/; child processes, the two camera inputs, the
# loopback output and the MP4 recorder from lib/).
HSRC := \
	legacy/hellion/check.c \
	legacy/hellion/control.c \
	legacy/hellion/hconfig.c \
	legacy/hellion/hlog.c \
	legacy/hellion/hrecord.c \
	legacy/hellion/hstatus.c \
	legacy/hellion/main.c \
	legacy/hellion/pipeline.c \
	legacy/hellion/sinks.c \
	legacy/hellion/top.c \
	legacy/eeye/fbus.c \
	lib/camera/camera_ctrl.c \
	lib/camera/jpeg_decode.c \
	lib/camera/rpicam_in.c \
	lib/camera/v4l2_in.c \
	lib/camera/v4l2_out.c \
	lib/camera/virtual_cam.c \
	lib/net/discovery.c \
	lib/net/http.c \
	lib/net/jpeg.c \
	lib/proc/child_proc.c \
	lib/video/ffmpeg.c \
	lib/video/mp4_record.c \
	lib/video/udp_relay.c \
	lib/vision/video_frame.c

obj = $(patsubst %.c,$(OBJDIR)/%.o,$(1))

.PHONY: all check clean
all: $(BUILDDIR)/$(EXE) $(BUILDDIR)/$(HEXE)

$(BUILDDIR)/$(EXE): $(call obj,$(SRC))
	$(CC) $(ALL_CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(ALL_LDLIBS)

$(BUILDDIR)/$(HEXE): $(call obj,$(HSRC)) $(OBJDIR)/gen/page.o
	$(CC) $(ALL_CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(ALL_LDLIBS)

# The web page goes inside the binary: legacy/hellion/web/index.html becomes
# a C array through a small tool built for the build machine (HOSTCC), so a
# cross build still runs it here.
$(BUILDDIR)/embed: legacy/hellion/embed.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -o $@ $<

$(OBJDIR)/gen/page.c: legacy/hellion/web/index.html $(BUILDDIR)/embed
	@mkdir -p $(dir $@)
	$(BUILDDIR)/embed page_html $< $@

$(OBJDIR)/gen/page.o: $(OBJDIR)/gen/page.c
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

# Include paths follow the layout: lib/ code (and its tests) sees only
# lib/; legacy code (and its tests) sees lib/ and legacy/. Objects keep
# their source path under $(OBJDIR), so same-named files in different
# directories cannot collide.
$(OBJDIR)/lib/%.o: EE_CPPFLAGS += $(LIB_CPPFLAGS)
$(OBJDIR)/legacy/%.o: EE_CPPFLAGS += $(LEGACY_CPPFLAGS)

# -MMD -MP: each object also writes a .d file listing the headers it read,
# included below, so editing a header rebuilds exactly what uses it.
$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CPPFLAGS) $(ALL_CFLAGS) -MMD -MP -c -o $@ $<

# Unit tests. Each includes the translation unit it exercises directly
# (rather than linking against it) so the file-static helpers -- the config
# parser, the JPEG frame splitter, the negotiation ranker -- are reachable
# without widening their visibility in the shipping build just to test them.
# A suite's source lives in the tests/ of the directory it exercises.
VISION_TESTS := canny log_edges auto_vis effect_chain
CAMERA_TESTS := jpeg_decode camera_ctrl virtual_cam jpeg_framing negotiation
PROC_TESTS := child_proc
NET_TESTS := http
EEYE_TESTS := config ring_buffer health recorder fbus
HELLION_TESTS := hellion
HG_TESTS := hg_layout hg_pubsub hg_wait hg_torn hg_inplace hg_replace hg_prov hg_wiring hg_waitfd hg_call hg_gate hg_params hg_control
TESTS := $(VISION_TESTS) $(COMMON_TESTS) $(CAMERA_TESTS) $(PROC_TESTS) $(NET_TESTS) \
	$(EEYE_TESTS) $(HELLION_TESTS) $(HG_TESTS)

# Extra translation units each test needs beyond the one it includes:
# rpicam_in.c and v4l2_in.c both reference the shared JPEG decoder and the
# frame pool, so those get linked in alongside.
test_config_SRC := lib/camera/camera_ctrl.c legacy/eeye/fbus.c
test_ring_buffer_SRC :=
test_jpeg_framing_SRC := lib/camera/camera_ctrl.c lib/proc/child_proc.c \
	lib/camera/jpeg_decode.c lib/vision/video_frame.c
test_negotiation_SRC := lib/camera/jpeg_decode.c lib/vision/video_frame.c
test_jpeg_decode_SRC := lib/camera/jpeg_decode.c lib/vision/video_frame.c
test_log_edges_SRC := lib/vision/conv.c lib/vision/video_frame.c $(VISION_POOL_SRC)
test_canny_SRC := lib/vision/conv.c lib/vision/video_frame.c $(VISION_POOL_SRC)
test_health_SRC :=
test_camera_ctrl_SRC :=
test_recorder_SRC := legacy/eeye/frame_ring_buffer.c legacy/eeye/health.c \
	lib/vision/video_frame.c
test_child_proc_SRC :=
test_fbus_SRC :=
test_http_SRC :=
test_auto_vis_SRC := lib/vision/img_stats.c $(VISION_POOL_SRC)
test_hellion_SRC := legacy/hellion/hconfig.c legacy/hellion/hlog.c \
	legacy/hellion/hstatus.c legacy/hellion/pipeline.c legacy/hellion/hrecord.c \
	legacy/hellion/sinks.c legacy/eeye/fbus.c lib/camera/camera_ctrl.c \
	lib/camera/jpeg_decode.c lib/camera/rpicam_in.c lib/camera/v4l2_in.c \
	lib/camera/v4l2_out.c lib/camera/virtual_cam.c lib/net/discovery.c \
	lib/net/http.c lib/net/jpeg.c lib/proc/child_proc.c lib/video/ffmpeg.c \
	lib/video/mp4_record.c lib/video/udp_relay.c lib/vision/video_frame.c
test_virtual_cam_SRC := lib/proc/child_proc.c lib/camera/v4l2_out.c \
	lib/vision/video_frame.c
test_effect_chain_SRC := lib/vision/auto_vis.c lib/vision/conv.c \
	lib/vision/img_stats.c lib/vision/point_opps.c lib/vision/video_frame.c \
	$(VISION_POOL_SRC)

# lib/hg: the hangar module library (LGPL-3.0-or-later; lib/hg/PLAN.md). C11
# with <stdatomic.h>, not gnu99, so its objects take their own flags; it
# needs nothing but libc and the kernel, so its suites link without
# libturbojpeg and build where a cross toolchain has none. The manager stub
# is test-only: it goes into the suites, never into HG_SRC.
HG_SRC := lib/hg/hg_topic.c lib/hg/hg_type.c lib/hg/hg_wiring.c lib/hg/hg_control.c \
	lib/hg/hg_call.c lib/hg/hg_params.c lib/hg/hg_gate.c
HG_CFLAGS := -std=c11 -Wall -Wextra -pedantic -pthread
HG_LDLIBS := -pthread -lrt
$(OBJDIR)/lib/hg/%.o: EE_CFLAGS := $(HG_CFLAGS)
$(foreach t,$(HG_TESTS),$(eval test_$(t)_SRC := $(HG_SRC) lib/hg/hg_manager_stub.c))

define hg_test_rule
$(TESTDIR)/test_$(1): $(OBJDIR)/lib/hg/tests/test_$(1).o $(call obj,$(test_$(1)_SRC))
	@mkdir -p $$(dir $$@)
	$$(CC) $$(HG_CFLAGS) $$(CFLAGS) $$(ALL_LDFLAGS) $$(test_$(1)_LDFLAGS) -o $$@ $$^ $$(HG_LDLIBS) $$(LDLIBS)
endef
# The gate's suite stands in for hg_latest where the gate calls it, to hand it
# the HG_BUSY of a read overtaken eight times, which no test can time.
test_hg_gate_LDFLAGS := -Wl,--wrap=hg_latest
$(foreach t,$(HG_TESTS),$(eval $(call hg_test_rule,$(t))))

# $(1): suite name, $(2): the directory whose tests/ holds its source.
define test_rule
$(TESTDIR)/test_$(1): $(OBJDIR)/$(2)/tests/test_$(1).o $(call obj,$(test_$(1)_SRC))
	@mkdir -p $$(dir $$@)
	$$(CC) $$(ALL_CFLAGS) $$(ALL_LDFLAGS) -o $$@ $$^ $$(ALL_LDLIBS)
endef
$(foreach t,$(VISION_TESTS),$(eval $(call test_rule,$(t),lib/vision)))
$(foreach t,$(COMMON_TESTS),$(eval $(call test_rule,$(t),lib/common)))
$(foreach t,$(CAMERA_TESTS),$(eval $(call test_rule,$(t),lib/camera)))
$(foreach t,$(PROC_TESTS),$(eval $(call test_rule,$(t),lib/proc)))
$(foreach t,$(NET_TESTS),$(eval $(call test_rule,$(t),lib/net)))
$(foreach t,$(EEYE_TESTS),$(eval $(call test_rule,$(t),legacy/eeye)))
$(foreach t,$(HELLION_TESTS),$(eval $(call test_rule,$(t),legacy/hellion)))

# A hung suite fails instead of stalling the run, where coreutils has
# timeout (8.0+, so Debian 6 on). Overridable: TIMEOUT="qemu-aarch64 -0"
# runs a cross build's suites under qemu user-mode emulation (qemu then
# takes the 60 as the program's argv[0], which the suites ignore).
TIMEOUT := $(shell command -v timeout >/dev/null 2>&1 && echo timeout)

# Suites run from the repository root, except one: test_hellion loads the
# shipped example configs by the path they had in electric-eye's own tree
# ("hellion/examples/..."), so it runs from legacy/, which is that tree's
# root now.
test_hellion_CWD := legacy

# Each suite's output goes to a log beside its binary and is printed only
# on failure, so a parallel run stays readable.
define run_suite
	@if ($(if $(test_$(1)_CWD),cd $(test_$(1)_CWD) &&) \
	     $(if $(TIMEOUT),$(TIMEOUT) $(2)) $(abspath $(3))) \
	     > $(TESTDIR)/$(1).log 2>&1; then \
	  echo "ok    $(1)"; \
	else \
	  echo "FAIL  $(1)"; cat $(TESTDIR)/$(1).log; exit 1; \
	fi
endef

CHECKS := $(addprefix check-,$(TESTS))
.PHONY: $(CHECKS)
$(CHECKS): check-%: $(TESTDIR)/test_%
	$(call run_suite,$*,60,$<)

check: $(CHECKS)

.PHONY: check-hg check-tsan check-asan check-asan-modules
check-hg: $(addprefix check-,$(HG_TESTS))

# The hg suites again under a sanitizer, in their own build directory so the
# instrumented objects never mix with the plain ones. No suppression file:
# the seqlock copy is atomic words, so there is nothing to suppress. A
# compiler without the runtime fails the probe and the target is skipped, not
# failed (gcc, clang and zig cc 0.17 all have it here); the probe binary stays
# in $(BUILDDIR), which `make clean` removes. The suites find HANGAR_SANITIZED
# in their environment: a CPU budget is ten times as wide there
# (lib/common/test_harness.h, th_cpu_within). $(1): the -fsanitize= list,
# $(2): the directory, $(3): the targets run there.
comma := ,
define sanitized_check
	@mkdir -p $(BUILDDIR); \
	if echo 'int main(void){return 0;}' | $(CC) -fsanitize=$(1) -x c - -o $(BUILDDIR)/.san-probe-$(2) >/dev/null 2>&1; then \
	  HANGAR_SANITIZED=$(2) $(MAKE) --no-print-directory BUILDDIR=$(BUILDDIR)/$(2) \
	    CFLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=$(1)" LDFLAGS="-fsanitize=$(1)" $(3); \
	else \
	  echo "skip  $(2): $(CC) cannot build with -fsanitize=$(1)"; \
	fi
endef
check-tsan:
	$(call sanitized_check,thread,tsan,check-hg)
check-asan:
	$(call sanitized_check,address$(comma)undefined,asan,check-hg)

# The modules that take bytes they did not make and index memory by them: the
# frame readers (vision, topside_encoder), the RTP depacketizer and decoder
# (video_relay) and the tether's datagrams (bridge). Their suites start the
# real manager and the real module, so both run instrumented, and a report in
# either fails the suite: a module that ASan stops exits, and the suite waits
# for an answer that does not come. libx264 and libavcodec are the system's,
# not instrumented; what ASan sees of them is what they are handed. In a
# directory of its own, so that it can run beside check-asan. Not under
# ThreadSanitizer: the views of hg_latest_view are plain reads of memory
# another process writes, which is the documented exception of docs/01.
SAN_MODULE_CHECKS ?= check-vision check-topside_encoder check-video_relay check-bridge
check-asan-modules:
	$(call sanitized_check,address$(comma)undefined,asan-modules,$(SAN_MODULE_CHECKS))

# Equivalence with electric-eye v0.3.1 (tools/equivalence/README.md): equiv
# hashes every lib/vision operator's output over a fixed input set, one
# line per case, and the run fails on any line that differs from the hashes
# recorded once from v0.3.1. It runs at 1, 3 and 4 threads (the pool reads
# OMP_NUM_THREADS by OpenMP's rule), since a thread count that changes a hash is a bug the
# paper's figures rest on not having. TIMEOUT runs it as it runs the
# suites, so TIMEOUT="qemu-aarch64 -0" checks a cross build's NEON paths
# against the same reference. equiv needs no libturbojpeg.
EQUIV_SRC := tools/equivalence/equiv.c $(VISION_SRC)
EQUIV_REF := tools/equivalence/hashes-v0.3.1.txt
EQUIV_THREADS ?= 1 3 4
$(OBJDIR)/tools/%.o: EE_CPPFLAGS += $(LIB_CPPFLAGS)
$(BUILDDIR)/equiv: $(call obj,$(EQUIV_SRC))
	$(CC) $(ALL_CFLAGS) $(ALL_LDFLAGS) -o $@ $^ -pthread -lrt -lm $(LDLIBS)

.PHONY: equivalence
equivalence: $(BUILDDIR)/equiv
	@for t in $(EQUIV_THREADS); do \
	  out=$(BUILDDIR)/equiv-$$t.txt; \
	  OMP_NUM_THREADS=$$t $(if $(TIMEOUT),$(TIMEOUT) 1800) $< > $$out || exit 1; \
	  if cmp -s $$out $(EQUIV_REF); then \
	    echo "ok    equivalence  threads=$$t  $$(grep -vc '^$(HASH)' $$out) cases match $(EQUIV_REF)"; \
	  else \
	    echo "FAIL  equivalence  threads=$$t: cases differing from $(EQUIV_REF):"; \
	    diff $(EQUIV_REF) $$out | sed -n 's/^[<>] \([^ ]*\) .*/\1/p' | sort -u; \
	    exit 1; \
	  fi; \
	done

# Fuzzing (`make fuzz`, and `make fuzz-http` for the HTTP parsers alone) is in
# fuzz/fuzz.mk, included at the end of this file.

# Static analysis (tools/analyze/README.md): clang's analyzer, gcc -fanalyzer,
# cppcheck and clang-tidy, those that are installed, over the C code that is
# not a test; fails on any finding that tools/analyze/suppress-<tool>.txt does
# not name. Not part of check: it takes minutes. ANALYZE_TOOLS=clang,cppcheck
# runs some of them; ANALYZE_JOBS=n bounds the processes (default: every core).
.PHONY: analyze
analyze:
	@$(PYTHON) tools/analyze/analyze.py $(if $(ANALYZE_TOOLS),--tools $(ANALYZE_TOOLS)) $(if $(ANALYZE_JOBS),--jobs $(ANALYZE_JOBS))

clean:
	rm -rf $(BUILDDIR)

# Message types (tools/msggen/README.md). msg/<name>.msg is the source;
# msg/<name>.h and .c are generated and checked in, so a build needs no
# generator. `make msg` regenerates them (a *_params.msg is a parameter
# type: --params); `make check-msggen`, part of check, runs the generator's
# suite, msggen --check over msg/ (a stale or missing file fails), compares
# the C and Python hashes for every .msg, and runs the Python unit tests
# (python/tests/run.py: every test of python/tests; the tests that need numpy
# or PyTorch skip without them, and the line it prints says how many and
# why; with HANGAR_PYTHON_NO_SKIP=1 in the environment a skip fails, which
# is what CI's gcc job sets, with PYTHON= a python that has both).
# One of them, python/tests/test_deploy_tether.py, runs two managers, two
# bridges and a policy: modules/policy/policy.mk makes this target depend on
# those programs, and the test skips, saying which, where they are not built.
# The generator is C11 and runs during the build, so HOSTCC builds it. The
# suite compiles the generated .c against tools/msggen/tests/hg_type.h, a
# stand-in with the three ABI types, so it does not depend on lib/hg.
PYTHON ?= python3
MSGGEN := $(BUILDDIR)/msggen
MSG_SRC := $(sort $(wildcard msg/*.msg))
MSG_PARAMS := $(filter %_params.msg,$(MSG_SRC))
MSG_PLAIN := $(filter-out %_params.msg,$(MSG_SRC))
MSGGEN_CFLAGS := -std=c11 -Wall -Wextra -pedantic
MSGGEN_OBJDIR := $(OBJDIR)/msggen
MSGGEN_CPPFLAGS := -Itools/msggen -Itools/msggen/tests -Imsg -Ilib/common

$(MSGGEN): tools/msggen/msggen.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 $(MSGGEN_CFLAGS) -o $@ $<

.PHONY: msg
msg: $(MSGGEN)
	$(MSGGEN) --out msg $(MSG_PLAIN)
	$(MSGGEN) --out msg --params $(MSG_PARAMS)

$(MSGGEN_OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(MSGGEN_CPPFLAGS) $(CPPFLAGS) $(MSGGEN_CFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(TESTDIR)/test_msggen: $(MSGGEN_OBJDIR)/tools/msggen/tests/test_msggen.o \
		$(patsubst %.msg,$(MSGGEN_OBJDIR)/msg/%.o,$(notdir $(MSG_SRC)))
	@mkdir -p $(dir $@)
	$(CC) $(MSGGEN_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

.PHONY: check-msggen
CHECKS += check-msggen
check: check-msggen
# lib/hg's run-time parser on the same files, for the three-way comparison of
# hashes below (lib/hg/tests/hg_schema_hash.c).
$(TESTDIR)/hg_schema_hash: lib/hg/tests/hg_schema_hash.c lib/hg/hg_type.c lib/hg/hg_type.h
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) -Ilib/hg -o $@ lib/hg/tests/hg_schema_hash.c lib/hg/hg_type.c

check-msggen: $(TESTDIR)/test_msggen $(MSGGEN) $(TESTDIR)/hg_schema_hash
	$(call run_suite,msggen,60,$<)
	@if $(MSGGEN) --check --out msg $(MSG_PLAIN) && \
	    $(MSGGEN) --check --out msg --params $(MSG_PARAMS); then \
	  echo "ok    msggen --check  $(words $(MSG_SRC)) types in msg/"; \
	else echo "FAIL  msggen --check: run make msg"; exit 1; fi
	@mkdir -p $(TESTDIR)/msg-odd; \
	printf 'u8 x[1]\n' > $(TESTDIR)/msg-odd/one_element.msg; \
	printf 'f32 kP\nu16 Raw_ADC[2]\n' > $(TESTDIR)/msg-odd/capitals.msg; \
	odd="$(TESTDIR)/msg-odd/one_element.msg $(TESTDIR)/msg-odd/capitals.msg"; \
	{ $(MSGGEN) --print-hash $(MSG_PLAIN) $$odd && $(MSGGEN) --print-hash --params $(MSG_PARAMS); } \
	  | sort > $(TESTDIR)/msggen-hash-c.txt || exit 1; \
	PYTHONPATH=python $(PYTHON) -m hangar.schema --print-hash $(MSG_SRC) $$odd \
	  | sort > $(TESTDIR)/msggen-hash-py.txt || exit 1; \
	$(TESTDIR)/hg_schema_hash $(MSG_SRC) $$odd | sort > $(TESTDIR)/msggen-hash-hg.txt; \
	if cmp -s $(TESTDIR)/msggen-hash-c.txt $(TESTDIR)/msggen-hash-py.txt && \
	   cmp -s $(TESTDIR)/msggen-hash-c.txt $(TESTDIR)/msggen-hash-hg.txt; then \
	  echo "ok    msggen hashes  msggen, Python and lib/hg agree on $(words $(MSG_SRC)) types and 2 odd ones"; \
	else echo "FAIL  msggen hashes: msggen (<) and Python (>) or lib/hg (>) differ:"; \
	  diff $(TESTDIR)/msggen-hash-c.txt $(TESTDIR)/msggen-hash-py.txt; \
	  diff $(TESTDIR)/msggen-hash-c.txt $(TESTDIR)/msggen-hash-hg.txt; exit 1; fi
	@HANGAR_BUILDDIR=$(abspath $(BUILDDIR)) HANGARD_TOYS=$(abspath $(TESTDIR)/hangard-modules) \
	  $(PYTHON) python/tests/run.py --log $(TESTDIR)/python.log


# lib/sim: the vehicle simulator's physics (lib/sim/README.md; GPL-3.0-or-later).
# Pure arithmetic in C11, nothing but libm, so like lib/hg it takes its own
# flags and links without libturbojpeg. `make check-sim`, part of check, runs
# its suite from the repository root (the suite reads lib/sim/data/*.csv to
# check the generated thrust table against it). data/t200_thrust.inc is
# checked in; `make sim-data` regenerates it from the CSV.
SIM_SRC := lib/sim/sim_model.c lib/sim/sim_thrust.c lib/sim/sim_noise.c lib/sim/sim_fit.c
SIM_CFLAGS := -std=c11 -Wall -Wextra -pedantic
SIM_CPPFLAGS := -Ilib/sim -Ilib/common
SIM_OBJDIR := $(OBJDIR)/sim
SIM_LDLIBS := -lm

$(SIM_OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(SIM_CPPFLAGS) $(CPPFLAGS) $(SIM_CFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(TESTDIR)/test_sim: $(SIM_OBJDIR)/lib/sim/tests/test_sim.o $(patsubst %.c,$(SIM_OBJDIR)/%.o,$(SIM_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(SIM_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(SIM_LDLIBS) $(LDLIBS)

.PHONY: check-sim sim-data
CHECKS += check-sim
check: check-sim
check-sim: $(TESTDIR)/test_sim
	$(call run_suite,sim,60,$<)

sim-data:
	sh lib/sim/data/gen_t200_inc.sh > lib/sim/data/t200_thrust.inc

# lib/nn, the inference runtime (lib/nn/nn.mk carries its own rules).
include lib/nn/nn.mk
check: check-nn

# bench/hg_bench, hangar's side of the benchmark (bench/bench.mk).
include bench/bench.mk

# hangard, the manager, and the hangar command (hangard/README.md,
# hangard/hangard.mk carries the rules): GPL-3.0-or-later, C11 over lib/hg.
include hangard/hangard.mk
check: check-hangard

# The hangar command's own suite (hangar/hangar.mk; the command itself is
# built by hangard.mk's rules).
include hangar/hangar.mk
check: check-hangar

# Dawn Patrol, the console module (modules/console/README.md,
# modules/console/console.mk carries the rules): GPL-3.0-or-later, C11 over lib/hg and lib/net.
include modules/console/console.mk
check: check-console

# The safety module (modules/safety/README.md, modules/safety/safety.mk
# carries the rules): GPL-3.0-or-later, C11 over lib/hg.
include modules/safety/safety.mk
check: check-safety

# The mux (modules/mux/README.md, modules/mux/mux.mk carries the rules):
# GPL-3.0-or-later, C11 over lib/hg.
include modules/mux/mux.mk
check: check-mux

# The MAVLink modules (modules/mavlink/README.md, modules/mavlink/mavlink.mk
# carries the rules): GPL-3.0-or-later, C11 over lib/hg; the generated header
# comes from tools/mavgen (Python at generation time only).
include modules/mavlink/mavlink.mk
check: check-mavlink

-include $(shell find $(BUILDDIR) -name '*.d' 2>/dev/null)

# The recorder, the pairer and the dataset pieces (modules/recorder/README.md,
# modules/recorder/recorder.mk carries the rules): GPL-3.0-or-later modules over lib/hg.
include modules/recorder/recorder.mk
check: check-recording

# The encoder (modules/encoder/README.md, modules/encoder/encoder.mk carries
# the rules): GPL-3.0-or-later, C11 over lib/hg; the camera's own H.264 to the
# topside over RTP, packetized here (RFC 6184), nothing encoded on the vehicle.
include modules/encoder/encoder.mk
check: check-encoder

# The joystick module (modules/joystick/README.md, modules/joystick/joystick.mk
# carries the rules): GPL-3.0-or-later, C11 over lib/hg; the pilot's controller
# read natively, pilot.wrench and pilot.buttons without QGroundControl (hangar#3).
include modules/joystick/joystick.mk
check: check-joystick

# video_relay (modules/video_relay/README.md, .mk carries the rules): the
# topside's end of the vehicle's video, decoded in process with libavcodec.
include modules/video_relay/video_relay.mk

# The tether bridge (modules/bridge/README.md, modules/bridge/bridge.mk carries
# the build and the suite).
include modules/bridge/bridge.mk
check: check-bridge
check: check-video_relay

# vision (modules/vision/README.md, .mk carries the rules): the pilot's picture,
# lib/vision's visibility correction and effect chain as a module (docs/11 B3).
include modules/vision/vision.mk
check: check-vision

# topside_encoder (modules/topside_encoder/README.md, .mk carries the rules): the
# pilot's picture to QGroundControl, encoded in process with libx264.
include modules/topside_encoder/topside_encoder.mk
check: check-topside_encoder

# sim_vehicle (modules/sim_vehicle/README.md, .mk carries the rules): the
# vehicle simulator over lib/sim, standalone or as ArduSub SITL's physics.
include modules/sim_vehicle/sim_vehicle.mk
check: check-sim_vehicle

# thrusters (modules/thrusters/README.md, .mk carries the rules): the actuator
# of a vehicle without ArduSub, ctl.wrench mixed to thr.cmd, which sim_vehicle's
# standalone mode reads.
include modules/thrusters/thrusters.mk
check: check-thrusters

# nav and depth_hold (modules/nav/README.md, modules/depth_hold/README.md; each
# .mk carries its rules): the vehicle's state estimate, nav.state, and the
# classical depth controller, ctl.depth_hold and ctl.depth_hold.target: the
# two modules docs/09's adaptive depth hold needs. GPL-3.0-or-later, C11 over
# lib/hg. depth_hold.mk after mux.mk and nav.mk: its suite runs both.
include modules/nav/nav.mk
check: check-nav
include modules/depth_hold/depth_hold.mk
check: check-depth_hold

# The policy program (modules/policy/README.md, .mk carries the rules): the
# learning loop's vehicle side, version 1 (docs/09-learning.md).
include modules/policy/policy.mk
check: check-policy

# tools/rov's guard (tools/rov/README.md, "The guard"): rov.sh stops nothing under an
# armed vehicle. A shell test of every case against a stub of the hangar command: no
# vehicle, no hangar, no port (tools/rov/tests/test_guard.sh). And the preflight
# (README "Preflight"): its judge over written facts, its probe on this machine
# (tools/rov/tests/test_preflight.sh).
.PHONY: check-rov
check-rov:
	@mkdir -p $(TESTDIR); \
	if bash tools/rov/tests/test_guard.sh > $(TESTDIR)/rov_guard.log 2>&1; then \
	  echo "ok    rov guard"; \
	else echo "FAIL  rov guard"; cat $(TESTDIR)/rov_guard.log; exit 1; fi
	@if bash tools/rov/tests/test_preflight.sh > $(TESTDIR)/rov_preflight.log 2>&1; then \
	  echo "ok    rov preflight"; \
	else echo "FAIL  rov preflight"; cat $(TESTDIR)/rov_preflight.log; exit 1; fi
check: check-rov

# Fuzzing (fuzz/README.md, fuzz/fuzz.mk carries the rules): a harness for every
# place that parses bytes the program does not control, and their corpora
# replayed by check.
include fuzz/fuzz.mk
