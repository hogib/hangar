# bench/bench.mk -- hg_bench, hangar's side of the benchmark (docs/12-tools.md).
#
# A GNU make fragment. The root Makefile includes it after lib/hg's rules:
#
#   include bench/bench.mk
#
# and then knows `make bench` ($(BUILDDIR)/hg_bench) and `make bench-quick`
# (a short run of everything, a smoke test). It uses the root's BUILDDIR,
# OBJDIR, CC, CFLAGS, LDFLAGS, LDLIBS, HG_SRC, HG_CFLAGS, HG_LDLIBS, obj and
# LIB_CPPFLAGS. It can also run on its own, for a build against another
# checkout of lib/hg (HG_DIR), with defaults for all of them:
#
#   make -f bench/bench.mk BUILDDIR=/tmp/b HG_DIR=/tmp/hg-head/lib/hg bench
#
# HG_COMMIT is the commit lib/hg was last changed in, from git at make time
# (`unknown` outside a checkout); it goes into hg_bench's report.

BENCH_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
BENCH_ROOT := $(abspath $(BENCH_DIR)..)

# Standalone defaults (the root Makefile defines all of these).
BUILDDIR ?= builddir
BUILD ?= debugoptimized
CC ?= cc
HG_DIR ?= $(BENCH_ROOT)/lib/hg
ifeq ($(origin HG_SRC),undefined)
HG_SRC := $(HG_DIR)/hg_topic.c $(HG_DIR)/hg_type.c $(HG_DIR)/hg_wiring.c
HG_CFLAGS := -std=c11 -Wall -Wextra -pedantic -pthread
HG_LDLIBS := -pthread -lrt
LIB_CPPFLAGS := -I$(HG_DIR) -I$(BENCH_ROOT)/lib/common
OBJDIR := $(BUILDDIR)/obj-$(BUILD)
CFLAGS ?= -O2 -g
ALL_LDFLAGS = $(LDFLAGS)
obj = $(patsubst %.c,$(OBJDIR)/%.o,$(subst /,_,$(patsubst $(BENCH_ROOT)/%,%,$(1))))
# Objects for lib/hg sources given by absolute path (the standalone case):
# one rule per source, since the path does not sit under the working tree.
define bench_hg_obj_rule
$(call obj,$(1)): $(1)
	@mkdir -p $$(dir $$@)
	$$(CC) $(LIB_CPPFLAGS) $$(CPPFLAGS) $$(HG_CFLAGS) $$(CFLAGS) -MMD -MP -c -o $$@ $$<
endef
$(foreach s,$(HG_SRC),$(eval $(call bench_hg_obj_rule,$(s))))
endif

HG_COMMIT := $(shell git -C $(BENCH_ROOT) log -1 --format=%h -- lib/hg 2>/dev/null || echo unknown)
ifeq ($(strip $(HG_COMMIT)),)
HG_COMMIT := unknown
endif

BENCH_SRC := $(BENCH_DIR)hg_bench.c
BENCH_OBJ := $(OBJDIR)/bench/hg_bench.o

$(BENCH_OBJ): $(BENCH_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(LIB_CPPFLAGS) $(CPPFLAGS) -DHG_COMMIT='"$(HG_COMMIT)"' $(HG_CFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILDDIR)/hg_bench: $(BENCH_OBJ) $(call obj,$(HG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) $(LDLIBS)

.PHONY: bench bench-quick
bench: $(BUILDDIR)/hg_bench

# Short runs of every measurement: proves the harness, not the numbers.
bench-quick: $(BUILDDIR)/hg_bench
	$< all --quick --bin $< --json $(BUILDDIR)/hg_bench-quick.json

-include $(BENCH_OBJ:.o=.d)
