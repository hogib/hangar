# nav (modules/nav/README.md), binary hangar-nav, and its suite. Included by
# the root Makefile; relies on OBJDIR, TESTDIR, BUILDDIR, HG_SRC, HG_CFLAGS,
# HG_LDLIBS, TIMEOUT, ALL_LDFLAGS and the generic $(OBJDIR)/%.o rule.
# GPL-3.0-or-later; C11; libc (libm), lib/hg and the generated msg/ types.
#
#   make nav-all          builddir/hangar-nav
#   make check-nav        the suite (part of check): the filter alone, then a
#                         manager, nav, and the suite as the vehicle's sensors

NAV_MSG_SRC := msg/depth.c msg/attitude.c msg/imu.c msg/nav_state.c msg/nav_params.c msg/empty.c
NAV_CPPFLAGS := -D_FILE_OFFSET_BITS=64 -Ilib/hg -Ilib/common -Imsg -Imodules/common -Imodules/nav -Ihangard/tests
NAV_BIN := $(BUILDDIR)/hangar-nav
NAV_TEST := $(TESTDIR)/test_nav

$(OBJDIR)/modules/nav/%.o: EE_CFLAGS := $(HG_CFLAGS)
$(OBJDIR)/modules/nav/%.o: EE_CPPFLAGS := $(NAV_CPPFLAGS)

$(NAV_BIN): $(OBJDIR)/modules/nav/nav.o $(OBJDIR)/modules/nav/nav_filter.o $(call obj,$(HG_SRC)) \
		$(call obj,$(NAV_MSG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) -lm $(LDLIBS)

.PHONY: nav-all
nav-all: $(NAV_BIN)
all: $(NAV_BIN)

$(NAV_TEST): $(OBJDIR)/modules/nav/tests/test_nav.o $(OBJDIR)/modules/nav/nav_filter.o $(call obj,$(HG_SRC)) \
		$(call obj,$(NAV_MSG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) -lm $(LDLIBS)

.PHONY: check-nav
check-nav: $(NAV_TEST) $(NAV_BIN) $(BUILDDIR)/hangard $(BUILDDIR)/hangar
	@if HANGARD_BIN=$(abspath $(BUILDDIR)/hangard) HANGAR_BIN=$(abspath $(BUILDDIR)/hangar) \
	    HANGARD_TOYS=$(abspath $(HANGARD_TOYDIR)) HANGAR_MSG_SRC=$(abspath msg) \
	    NAV_BIN=$(abspath $(NAV_BIN)) \
	    $(if $(TIMEOUT),$(TIMEOUT) 120) $(abspath $<) > $(TESTDIR)/nav.log 2>&1; then \
	  echo "ok    nav"; \
	else \
	  echo "FAIL  nav"; cat $(TESTDIR)/nav.log; exit 1; \
	fi
