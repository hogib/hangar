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
#   make equivalence       hash every lib/vision operator's output and
#                          compare with electric-eye v0.3.1's (tools/equivalence/)
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
HG_TESTS := hg_layout hg_pubsub hg_wait hg_torn hg_replace hg_prov hg_wiring hg_waitfd hg_call hg_gate
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
	$$(CC) $$(HG_CFLAGS) $$(CFLAGS) $$(ALL_LDFLAGS) -o $$@ $$^ $$(HG_LDLIBS) $$(LDLIBS)
endef
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

.PHONY: check-hg check-tsan check-asan
check-hg: $(addprefix check-,$(HG_TESTS))

# The hg suites again under a sanitizer, in their own build directory so the
# instrumented objects never mix with the plain ones. No suppression file:
# the seqlock copy is atomic words, so there is nothing to suppress. A
# compiler without the runtime fails the probe and the target is skipped, not
# failed (gcc, clang and zig cc 0.17 all have it here); the probe binary stays
# in $(BUILDDIR), which `make clean` removes. $(1): the -fsanitize= list,
# $(2): the directory.
comma := ,
define sanitized_check
	@mkdir -p $(BUILDDIR); \
	if echo 'int main(void){return 0;}' | $(CC) -fsanitize=$(1) -x c - -o $(BUILDDIR)/.san-probe-$(2) >/dev/null 2>&1; then \
	  $(MAKE) --no-print-directory BUILDDIR=$(BUILDDIR)/$(2) \
	    CFLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=$(1)" LDFLAGS="-fsanitize=$(1)" check-hg; \
	else \
	  echo "skip  $(2): $(CC) cannot build with -fsanitize=$(1)"; \
	fi
endef
check-tsan:
	$(call sanitized_check,thread,tsan)
check-asan:
	$(call sanitized_check,address$(comma)undefined,asan)

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

# Fuzzes hellion's HTTP parsers with libFuzzer (needs clang): the one part
# of hellion that parses bytes from the network. Not part of `check` --
# it runs until FUZZ_SECONDS is up. Seeds live in lib/net/tests/fuzz_http_seeds/;
# anything new it finds goes into $(BUILDDIR)/fuzz-http-corpus/.
FUZZ_CC ?= clang
FUZZ_SECONDS ?= 60
.PHONY: fuzz-http
fuzz-http:
	@mkdir -p $(BUILDDIR)/fuzz-http-corpus
	$(FUZZ_CC) -g -O1 -std=gnu99 $(LIB_CPPFLAGS) \
	  -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
	  -o $(BUILDDIR)/fuzz_http lib/net/tests/fuzz_http.c
	$(BUILDDIR)/fuzz_http -max_total_time=$(FUZZ_SECONDS) -max_len=9000 \
	  $(BUILDDIR)/fuzz-http-corpus lib/net/tests/fuzz_http_seeds

clean:
	rm -rf $(BUILDDIR)

# Message types (tools/msggen/README.md). msg/<name>.msg is the source;
# msg/<name>.h and .c are generated and checked in, so a build needs no
# generator. `make msg` regenerates them (a *_params.msg is a parameter
# type: --params); `make check-msggen`, part of check, runs the generator's
# suite, msggen --check over msg/ (a stale or missing file fails), compares
# the C and Python hashes for every .msg, and runs the Python unit tests.
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
check-msggen: $(TESTDIR)/test_msggen $(MSGGEN)
	$(call run_suite,msggen,60,$<)
	@if $(MSGGEN) --check --out msg $(MSG_PLAIN) && \
	    $(MSGGEN) --check --out msg --params $(MSG_PARAMS); then \
	  echo "ok    msggen --check  $(words $(MSG_SRC)) types in msg/"; \
	else echo "FAIL  msggen --check: run make msg"; exit 1; fi
	@mkdir -p $(TESTDIR); \
	{ $(MSGGEN) --print-hash $(MSG_PLAIN) && $(MSGGEN) --print-hash --params $(MSG_PARAMS); } \
	  | sort > $(TESTDIR)/msggen-hash-c.txt || exit 1; \
	PYTHONPATH=python $(PYTHON) -m hangar.schema --print-hash $(MSG_SRC) \
	  | sort > $(TESTDIR)/msggen-hash-py.txt || exit 1; \
	if cmp -s $(TESTDIR)/msggen-hash-c.txt $(TESTDIR)/msggen-hash-py.txt; then \
	  echo "ok    msggen hashes  C and Python agree on $(words $(MSG_SRC)) types"; \
	else echo "FAIL  msggen hashes: C and Python differ:"; \
	  diff $(TESTDIR)/msggen-hash-c.txt $(TESTDIR)/msggen-hash-py.txt; exit 1; fi
	@if $(PYTHON) -m unittest discover -s python/tests > $(TESTDIR)/schema_py.log 2>&1; then \
	  echo "ok    schema.py"; \
	else echo "FAIL  schema.py"; cat $(TESTDIR)/schema_py.log; exit 1; fi


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
