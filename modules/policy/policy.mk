# hangar-policy (modules/policy/README.md; docs/09-learning.md "Policy
# modules") and its suite. Included by the root Makefile after lib/nn/nn.mk
# and modules/recorder/recorder.mk (HGR_SRC: the dataset join code); relies
# on OBJDIR, TESTDIR, BUILDDIR, HG_SRC, HG_CFLAGS, HG_LDLIBS, NN_SRC, TIMEOUT,
# ALL_LDFLAGS and the generic $(OBJDIR)/%.o rule. GPL-3.0-or-later; C11;
# libc (libm, pthreads), lib/hg, lib/nn and the generated msg/ types only.
#
#   make policy-all       builddir/hangar-policy
#   make check-policy     the suite (part of check): a manager, a policy on a
#                         toy dataset, toy models made by tests/make_models.py

POLICY_MSG_SRC := msg/wrench.c msg/policy_active.c msg/policy_load.c msg/load_weights_req.c \
	msg/load_weights_rep.c msg/policy_version.c msg/runtime_info.c msg/recorder_session.c msg/empty.c
POLICY_CPPFLAGS := -D_FILE_OFFSET_BITS=64 -Ilib/hg -Ilib/common -Ilib/nn -Imsg -Imodules/common -Ihangard/tests
POLICY_TEST_MSG_SRC := msg/mux_state.c msg/set_mode_req.c msg/set_mode_rep.c msg/depth.c msg/safety_state.c
POLICY_BIN := $(BUILDDIR)/hangar-policy
POLICY_TEST := $(TESTDIR)/test_policy

$(OBJDIR)/modules/policy/%.o: EE_CFLAGS := $(HG_CFLAGS)
$(OBJDIR)/modules/policy/%.o: EE_CPPFLAGS := $(POLICY_CPPFLAGS)

$(POLICY_BIN): $(OBJDIR)/modules/policy/policy.o $(call obj,$(HG_SRC)) $(call obj,$(HGR_SRC)) \
		$(call obj,$(NN_SRC)) $(call obj,$(POLICY_MSG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) -lm $(LDLIBS)

.PHONY: policy-all
policy-all: $(POLICY_BIN)
all: $(POLICY_BIN)

$(POLICY_TEST): $(OBJDIR)/modules/policy/tests/test_policy.o $(call obj,$(HG_SRC)) $(call obj,$(HGR_SRC)) \
		$(call obj,$(NN_SRC)) $(call obj,$(POLICY_MSG_SRC) $(POLICY_TEST_MSG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) -lm $(LDLIBS)

.PHONY: check-policy
check-policy: $(POLICY_TEST) $(POLICY_BIN) $(BUILDDIR)/hangard $(BUILDDIR)/hangar $(BUILDDIR)/hangar-mux $(PAIRER_BIN)
	@if HANGARD_BIN=$(abspath $(BUILDDIR)/hangard) HANGAR_BIN=$(abspath $(BUILDDIR)/hangar) \
	    HANGARD_TOYS=$(abspath $(HANGARD_TOYDIR)) HANGAR_MSG_SRC=$(abspath msg) \
	    POLICY_BIN=$(abspath $(POLICY_BIN)) MUX_BIN=$(abspath $(BUILDDIR)/hangar-mux) \
	    POLICY_PYTHON=$(abspath python) PAIRER_BIN=$(abspath $(PAIRER_BIN)) \
	    $(if $(TIMEOUT),$(TIMEOUT) 120) $(abspath $<) > $(TESTDIR)/policy.log 2>&1; then \
	  echo "ok    policy"; \
	else \
	  echo "FAIL  policy"; cat $(TESTDIR)/policy.log; exit 1; \
	fi
