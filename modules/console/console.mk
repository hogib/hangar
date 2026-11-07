# Dawn Patrol: the console module (modules/console/README.md), binary
# dawn-patrold, and its suite. Included by the root Makefile after
# hangard/hangard.mk; relies on OBJDIR, TESTDIR, BUILDDIR, HG_SRC, HG_CFLAGS,
# HG_LDLIBS, TIMEOUT, ALL_LDFLAGS, the generic $(OBJDIR)/%.o rule, the embed
# tool ($(BUILDDIR)/embed, legacy/hellion/embed.c) and hangard.mk's toy
# modules. GPL-3.0-or-later; C11 like lib/hg; libc, pthreads, lib/hg and
# lib/net only, so it builds static for musl.
#
#   make console-all        builddir/dawn-patrold
#   make check-console      the suite (part of check): a manager with the toy
#                           talker and the console, driven over HTTP

CONSOLE_SRC := modules/console/dawn_patrold.c modules/console/console_api.c
CONSOLE_LINK_SRC := $(CONSOLE_SRC) msg/hangar_system.c msg/hangar_modules.c lib/net/http.c
CONSOLE_CPPFLAGS := -D_FILE_OFFSET_BITS=64 -Ilib/hg -Ilib/net -Ilib/common -Imsg -Imodules/console -Ihangard/tests
CONSOLE_PAGE := $(OBJDIR)/gen/console_page.c
CONSOLE_BIN := $(BUILDDIR)/dawn-patrold
CONSOLE_TEST := $(TESTDIR)/test_console

$(OBJDIR)/modules/console/%.o: EE_CFLAGS := $(HG_CFLAGS)
$(OBJDIR)/modules/console/%.o: EE_CPPFLAGS := $(CONSOLE_CPPFLAGS)

# The page goes inside the binary, as hellion's does: web/index.html becomes
# a C array through the embed tool built for the build machine.
$(CONSOLE_PAGE): modules/console/web/index.html $(BUILDDIR)/embed
	@mkdir -p $(dir $@)
	$(BUILDDIR)/embed console_page_html $< $@

$(OBJDIR)/gen/console_page.o: $(CONSOLE_PAGE)
	$(CC) $(HG_CFLAGS) $(CFLAGS) -c -o $@ $<

$(CONSOLE_BIN): $(call obj,$(CONSOLE_LINK_SRC)) $(call obj,$(HG_SRC)) $(OBJDIR)/gen/console_page.o
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) $(LDLIBS)

.PHONY: console-all
console-all: $(CONSOLE_BIN)
all: $(CONSOLE_BIN)

# The suite: hangard's harness (hangard/tests/hd_test.h), lib/net's HTTP
# client for the requests, the manager, the toy talker and dawn-patrold by
# their paths from the environment, under a $HANGAR_ROOT of its own.
$(CONSOLE_TEST): $(OBJDIR)/modules/console/tests/test_console.o $(call obj,$(HG_SRC)) $(OBJDIR)/lib/net/http.o
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) $(LDLIBS)

.PHONY: check-console
check-console: $(CONSOLE_TEST) $(CONSOLE_BIN) $(BUILDDIR)/hangard $(BUILDDIR)/hangar $(HANGARD_TOYDIR)/talker
	@if HANGARD_BIN=$(abspath $(BUILDDIR)/hangard) HANGAR_BIN=$(abspath $(BUILDDIR)/hangar) \
	    HANGARD_TOYS=$(abspath $(HANGARD_TOYDIR)) HANGAR_MSG_SRC=$(abspath msg) \
	    DAWN_PATROLD_BIN=$(abspath $(CONSOLE_BIN)) \
	    $(if $(TIMEOUT),$(TIMEOUT) 120) $(abspath $<) > $(TESTDIR)/console.log 2>&1; then \
	  echo "ok    console"; \
	else \
	  echo "FAIL  console"; cat $(TESTDIR)/console.log; exit 1; \
	fi
