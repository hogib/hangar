# lib/nn: the inference runtime (LGPL-3.0-or-later; docs/09-learning.md,
# lib/nn/README.md). A GNU make fragment: variables and rules only, meant to
# be included from the root Makefile after OBJDIR, TESTDIR, CFLAGS,
# ALL_LDFLAGS, LDLIBS, obj and run_suite are defined (i.e. after the check
# rules), e.g.
#
#   include lib/nn/nn.mk
#   check: check-nn
#
# C11, libc and libm only: no pthreads, no librt, no turbojpeg, so it builds
# wherever a cross toolchain has a C library (zig cc -target
# aarch64-linux-musl links the suite statically). Its objects take their own
# flags, as lib/hg's do. The suite reads lib/nn/tests/vectors/ from the
# repository root; HANGAR_NN_VECTORS overrides the directory. check-nn runs
# it through run_suite, like the root's check-% targets, since CHECKS is
# expanded before this fragment is read.

NN_SRC := lib/nn/nn_ops.c lib/nn/nn_graph.c lib/nn/nn_hgw.c lib/nn/blake2s.c
NN_CFLAGS := -std=c11 -Wall -Wextra -pedantic
NN_CPPFLAGS := -Ilib/nn -Ilib/common
NN_LDLIBS := -lm
NN_TESTS := nn

$(OBJDIR)/lib/nn/%.o: EE_CFLAGS := $(NN_CFLAGS)
$(OBJDIR)/lib/nn/%.o: EE_CPPFLAGS += $(NN_CPPFLAGS)
test_nn_SRC := $(NN_SRC)

define nn_test_rule
$(TESTDIR)/test_$(1): $(OBJDIR)/lib/nn/tests/test_$(1).o $(call obj,$(test_$(1)_SRC))
	@mkdir -p $$(dir $$@)
	$$(CC) $$(NN_CFLAGS) $$(CFLAGS) $$(ALL_LDFLAGS) -o $$@ $$^ $$(NN_LDLIBS) $$(LDLIBS)
endef
$(foreach t,$(NN_TESTS),$(eval $(call nn_test_rule,$(t))))

.PHONY: check-nn
check-nn: $(TESTDIR)/test_nn
	$(call run_suite,nn,60,$<)
