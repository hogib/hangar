# lib/vision -- GNU make fragment: the variables and rules the root Makefile
# needs for the operators' thread pool (lib/common/hg_pool.c) that replaced
# OpenMP. Variables and rules only; nothing here is a default goal.
#
# How it is meant to be included, in the root Makefile:
#
#   include lib/vision/vision.mk
#
# just after LIBDIRS/LEGACYDIRS are set and before SRC, HSRC, EQUIV_SRC and
# the test_*_SRC lists are read (a prerequisite list is expanded where its
# rule is parsed, so a `+=` from a fragment included later would be too
# late). Then:
#
#   - SRC and EQUIV_SRC: replace their lib/vision/*.c entries with
#     $(VISION_SRC), which includes the pool. HSRC keeps lib/vision/video_frame.c
#     alone: hellion never calls an operator and links no pool.
#   - test_canny_SRC, test_log_edges_SRC, test_auto_vis_SRC,
#     test_effect_chain_SRC: append $(VISION_POOL_SRC) -- every suite that
#     links an operator TU links the pool it calls.
#   - TESTS: add $(COMMON_TESTS), with
#       $(foreach t,$(COMMON_TESTS),$(eval $(call test_rule,$(t),lib/common)))
#     beside the other test_rule lines; test_hg_pool_SRC is set below.
#   - The OpenMP probe (OPENMP, OPENMP_LIBS, -fopenmp in EE_CFLAGS, the
#     `ifeq ($(OPENMP),1)` around EQUIV_THREADS, $(OPENMP_LIBS) in EE_LDLIBS
#     and the equiv link): delete it. lib/ no longer includes <omp.h> or
#     carries a `#pragma omp`; lib/camera's two pragmas (v4l2_in.c,
#     rpicam_in.c) are the exception and were already dead without
#     -fopenmp. EQUIV_THREADS becomes the unconditional `?= 1 3 4`:
#     equiv's pool reads OMP_NUM_THREADS with OpenMP's rule, so the
#     `OMP_NUM_THREADS=$$t` loop in the equivalence recipe stays as it is.
#   - check-tsan-pool below wants the sanitized_check machinery (BUILDDIR,
#     CFLAGS, LDFLAGS overrides): it re-enters make for the one suite.

# The operators, the chain, the visibility correction, and the pool they
# run their loops on. lib/common/hg_pool.c is C99 + pthreads, nothing else.
VISION_POOL_SRC := lib/common/hg_pool.c lib/vision/vision_pool.c
VISION_SRC := \
	lib/vision/auto_vis.c \
	lib/vision/conv.c \
	lib/vision/effect_chain.c \
	lib/vision/img_stats.c \
	lib/vision/point_opps.c \
	lib/vision/video_frame.c \
	$(VISION_POOL_SRC)

# lib/common's suites. test_hg_pool checks the partition against OpenMP's
# static schedule (live, when the compiler has OpenMP; else the documented
# rule), that no band is lost over 10 000 runs, and that one participant
# runs inline.
COMMON_TESTS := hg_pool
test_hg_pool_SRC := lib/common/hg_pool.c

# The pool suite again under ThreadSanitizer, in its own build directory.
# Same shape as check-tsan: skipped, not failed, when $(CC) has no TSan.
.PHONY: check-tsan-pool
check-tsan-pool:
	@mkdir -p $(BUILDDIR); \
	if echo 'int main(void){return 0;}' | $(CC) -fsanitize=thread -x c - -o $(BUILDDIR)/.san-probe-tsan-pool >/dev/null 2>&1; then \
	  $(MAKE) --no-print-directory BUILDDIR=$(BUILDDIR)/tsan-pool \
	    CFLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=thread" LDFLAGS="-fsanitize=thread" check-hg_pool; \
	else \
	  echo "skip  tsan-pool: $(CC) cannot build with -fsanitize=thread"; \
	fi

# The timing harness (tools/equivalence/bench.c): the visibility correction
# and blur strength 4 per 1280x720 frame, at OMP_NUM_THREADS threads.
# `make bench` builds it; run $(BUILDDIR)/bench [-n runs] [-p] by hand.
BENCH_SRC := tools/equivalence/bench.c $(VISION_SRC)
# patsubst rather than $(call obj,...): obj is defined later in the Makefile
# than this fragment is included, and a prerequisite expands at once.
$(BUILDDIR)/bench: $(patsubst %.c,$(OBJDIR)/%.o,$(BENCH_SRC))
	$(CC) $(ALL_CFLAGS) $(ALL_LDFLAGS) -o $@ $^ -pthread -lrt -lm $(LDLIBS)
.PHONY: bench
bench: $(BUILDDIR)/bench
