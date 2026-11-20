# The hangar command's own suite (hangar/tests/). Included by the root
# Makefile after hangard/hangard.mk, whose rules build builddir/hangar and
# the toy modules, and whose HANGARD_CPPFLAGS, HANGARD_MSG_OBJS and
# HANGARD_TOYDIR this relies on. GPL-3.0-or-later, C11 over lib/hg.
#
#   make check-hangar       the suite (part of check): hangar echo, hz, call
#                           and param on topics the suite publishes itself,
#                           against a stand-in manager, and under hangard
#                           with the toy modules

HANGAR_TESTS := hangar_cli
HANGAR_TOYS := bigparams
HANGAR_TOYDIR := $(TESTDIR)/hangar-modules

$(OBJDIR)/hangar/tests/%.o: EE_CPPFLAGS := $(HANGARD_CPPFLAGS) -Ihangard/tests -Ihangard/tests/modules

$(HANGAR_TOYDIR)/%: $(OBJDIR)/hangar/tests/modules/%.o $(call obj,$(HG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) $(LDLIBS)

.SECONDARY: $(addsuffix .o,$(addprefix $(OBJDIR)/hangar/tests/modules/,$(HANGAR_TOYS)))

define hangar_test_rule
$(TESTDIR)/test_$(1): $(OBJDIR)/hangar/tests/test_$(1).o $(call obj,$(HG_SRC)) $(HANGARD_MSG_OBJS)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(HG_CFLAGS) $$(CFLAGS) $$(ALL_LDFLAGS) -o $$@ $$^ $$(HG_LDLIBS) $$(LDLIBS)
endef
$(foreach t,$(HANGAR_TESTS),$(eval $(call hangar_test_rule,$(t))))

HANGAR_CHECKS := $(addprefix check-,$(HANGAR_TESTS))
.PHONY: $(HANGAR_CHECKS) check-hangar
$(HANGAR_CHECKS): check-%: $(TESTDIR)/test_% $(BUILDDIR)/hangard $(BUILDDIR)/hangar \
		$(addprefix $(HANGARD_TOYDIR)/,$(HANGARD_TOYS)) $(addprefix $(HANGAR_TOYDIR)/,$(HANGAR_TOYS))
	@if HANGARD_BIN=$(abspath $(BUILDDIR)/hangard) HANGAR_BIN=$(abspath $(BUILDDIR)/hangar) \
	    HANGARD_TOYS=$(abspath $(HANGARD_TOYDIR)) HANGAR_TOYS=$(abspath $(HANGAR_TOYDIR)) \
	    HANGAR_MSG_SRC=$(abspath msg) \
	    $(if $(TIMEOUT),$(TIMEOUT) 120) $(abspath $<) > $(TESTDIR)/$*.log 2>&1; then \
	  echo "ok    $*"; \
	else \
	  echo "FAIL  $*"; cat $(TESTDIR)/$*.log; exit 1; \
	fi

check-hangar: $(HANGAR_CHECKS)
