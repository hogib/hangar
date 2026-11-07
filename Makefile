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
#   make clean
#
# Written for GNU make 3.81 and a C99 compiler (GCC 4.7+ or clang), so it
# builds on old distros as well as new ones -- Debian 7 and up. Nothing
# here needs make 4.x: no !=, no .ONESHELL, no $(file).
#
# Everything is overridable from the command line or environment: CC,
# HOSTCC, CFLAGS, CPPFLAGS, LDFLAGS, LDLIBS, BUILDDIR, PKG_CONFIG, and
#   OPENMP=0|1                   default: on if $(CC) can build with it
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
LIBDIRS := lib/common lib/vision lib/camera lib/proc lib/net lib/video
LEGACYDIRS := legacy/eeye legacy/hellion
LIB_CPPFLAGS := $(addprefix -I,$(LIBDIRS))
LEGACY_CPPFLAGS := $(LIB_CPPFLAGS) $(addprefix -I,$(LEGACYDIRS))

CFLAGS ?= $(OPTFLAGS)
# _FILE_OFFSET_BITS=64: recordings pass 2GB quickly, and on 32-bit ARM
# (most Pis) off_t is 32 bits without it.
EE_CPPFLAGS := -D_FILE_OFFSET_BITS=64
EE_CFLAGS := -std=gnu99 -Wall -Wextra -pedantic -pthread

# OpenMP: the #pragma omp loops in conv.c/point_opps.c are written to be
# race-free, but without -fopenmp every one of them is silently ignored and
# runs single-threaded. Probed with omp.h included, since clang can accept
# -fopenmp yet have no OpenMP runtime installed to go with it.
# (HASH: make 4.3 changed how "#" inside $(shell ...) is escaped; a
# variable holding one reads the same in every version.)
HASH := \#
# The probe must link into a real file: zig cc (and clang behind it) skips
# the link when the output is /dev/null, so a compiler with no OpenMP
# runtime passed the old probe and then failed at the real link with
# undefined __kmpc_* symbols. Tried plain first (gcc adds -lgomp itself),
# then with -lomp (clang/zig with LLVM's libomp installed, which neither
# adds on its own). OPENMP_LIBS carries what the link needs.
OMP_PROBE_SRC := $(HASH)include <omp.h>\nint main(void) { return omp_get_max_threads() < 1; }\n
OMP_PROBE_BIN := $(BUILDDIR)/.omp-probe
ifeq ($(origin OPENMP),undefined)
OPENMP_LIBS := $(shell mkdir -p $(BUILDDIR); \
  if printf '$(OMP_PROBE_SRC)' | $(CC) -fopenmp -x c - -o $(OMP_PROBE_BIN) >/dev/null 2>&1; then echo; \
  elif printf '$(OMP_PROBE_SRC)' | $(CC) -fopenmp -x c - -o $(OMP_PROBE_BIN) -lomp >/dev/null 2>&1; then echo -lomp; \
  else echo none; fi; rm -f $(OMP_PROBE_BIN))
ifeq ($(OPENMP_LIBS),none)
OPENMP := 0
OPENMP_LIBS :=
else
OPENMP := 1
endif
endif
ifeq ($(OPENMP),1)
EE_CFLAGS += -fopenmp
else
$(warning OpenMP not available: conv.c/point_opps.c will build, but their \
  loops will run single-threaded.)
endif

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
EE_LDLIBS := $(TURBOJPEG_LIBS) $(OPENMP_LIBS) -pthread -lrt -lm

ALL_CPPFLAGS = $(EE_CPPFLAGS) $(TURBOJPEG_CFLAGS) $(CPPFLAGS)
ALL_CFLAGS = $(EE_CFLAGS) $(CFLAGS)
ALL_LDFLAGS = $(EE_LDFLAGS) $(LDFLAGS)
ALL_LDLIBS = $(EE_LDLIBS) $(LDLIBS)

# eeye: its glue from legacy/eeye/, plus the library code it is built on.
SRC := \
	legacy/eeye/eeye.c \
	legacy/eeye/bus_in.c \
	legacy/eeye/config.c \
	legacy/eeye/effect_chain.c \
	legacy/eeye/fbus.c \
	legacy/eeye/frame_ring_buffer.c \
	legacy/eeye/health.c \
	legacy/eeye/recorder.c \
	legacy/eeye/rpicam_in.c \
	legacy/eeye/v4l2_in.c \
	legacy/eeye/video_threads.c \
	lib/camera/camera_ctrl.c \
	lib/camera/jpeg_decode.c \
	lib/camera/v4l2_out.c \
	lib/camera/virtual_cam.c \
	lib/proc/child_proc.c \
	lib/vision/auto_vis.c \
	lib/vision/conv.c \
	lib/vision/img_stats.c \
	lib/vision/point_opps.c \
	lib/vision/video_frame.c

# hellion: its own glue, plus the modules it shares with eeye (the frame
# bus and the two camera inputs from legacy/eeye/; child processes, camera
# capture and the loopback output from lib/).
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
	legacy/eeye/rpicam_in.c \
	legacy/eeye/v4l2_in.c \
	lib/camera/camera_ctrl.c \
	lib/camera/jpeg_decode.c \
	lib/camera/v4l2_out.c \
	lib/camera/virtual_cam.c \
	lib/net/discovery.c \
	lib/net/http.c \
	lib/net/jpeg.c \
	lib/proc/child_proc.c \
	lib/video/ffmpeg.c \
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
VISION_TESTS := canny log_edges auto_vis
CAMERA_TESTS := jpeg_decode camera_ctrl virtual_cam
PROC_TESTS := child_proc
NET_TESTS := http
EEYE_TESTS := config ring_buffer jpeg_framing negotiation health recorder \
	effect_chain fbus
HELLION_TESTS := hellion
TESTS := $(VISION_TESTS) $(CAMERA_TESTS) $(PROC_TESTS) $(NET_TESTS) \
	$(EEYE_TESTS) $(HELLION_TESTS)

# Extra translation units each test needs beyond the one it includes:
# rpicam_in.c and v4l2_in.c both reference the shared JPEG decoder and the
# frame pool, so those get linked in alongside.
test_config_SRC := lib/camera/camera_ctrl.c legacy/eeye/fbus.c
test_ring_buffer_SRC :=
test_jpeg_framing_SRC := lib/camera/camera_ctrl.c lib/proc/child_proc.c \
	lib/camera/jpeg_decode.c lib/vision/video_frame.c
test_negotiation_SRC := lib/camera/jpeg_decode.c lib/vision/video_frame.c
test_jpeg_decode_SRC := lib/camera/jpeg_decode.c lib/vision/video_frame.c
test_log_edges_SRC := lib/vision/conv.c lib/vision/video_frame.c
test_canny_SRC := lib/vision/conv.c lib/vision/video_frame.c
test_health_SRC :=
test_camera_ctrl_SRC :=
test_recorder_SRC := legacy/eeye/frame_ring_buffer.c legacy/eeye/health.c \
	lib/vision/video_frame.c
test_child_proc_SRC :=
test_fbus_SRC :=
test_http_SRC :=
test_auto_vis_SRC := lib/vision/img_stats.c
test_hellion_SRC := legacy/hellion/hconfig.c legacy/hellion/hlog.c \
	legacy/hellion/hstatus.c legacy/hellion/pipeline.c legacy/hellion/hrecord.c \
	legacy/hellion/sinks.c legacy/eeye/fbus.c legacy/eeye/rpicam_in.c \
	legacy/eeye/v4l2_in.c lib/camera/camera_ctrl.c lib/camera/jpeg_decode.c \
	lib/camera/v4l2_out.c lib/camera/virtual_cam.c lib/net/discovery.c \
	lib/net/http.c lib/net/jpeg.c lib/proc/child_proc.c lib/video/ffmpeg.c \
	lib/video/udp_relay.c lib/vision/video_frame.c
test_virtual_cam_SRC := lib/proc/child_proc.c lib/camera/v4l2_out.c \
	lib/vision/video_frame.c
test_effect_chain_SRC := legacy/eeye/config.c legacy/eeye/fbus.c \
	lib/camera/camera_ctrl.c lib/vision/auto_vis.c lib/vision/conv.c \
	lib/vision/img_stats.c lib/vision/point_opps.c lib/vision/video_frame.c

# $(1): suite name, $(2): the directory whose tests/ holds its source.
define test_rule
$(TESTDIR)/test_$(1): $(OBJDIR)/$(2)/tests/test_$(1).o $(call obj,$(test_$(1)_SRC))
	@mkdir -p $$(dir $$@)
	$$(CC) $$(ALL_CFLAGS) $$(ALL_LDFLAGS) -o $$@ $$^ $$(ALL_LDLIBS)
endef
$(foreach t,$(VISION_TESTS),$(eval $(call test_rule,$(t),lib/vision)))
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

-include $(shell find $(BUILDDIR) -name '*.d' 2>/dev/null)
