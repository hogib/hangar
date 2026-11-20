# The tether bridge (docs/08-bridge.md, modules/bridge/README.md), binary
# hangar-bridge, and its suite. Included by the root Makefile after
# hangard/hangard.mk, which builds bridge_conf.c and blake2s.c into the manager
# too and sets the flags of $(OBJDIR)/modules/bridge/. GPL-3.0-or-later; C11;
# libc, lib/hg and the generated msg/ types only, so it builds static for musl.
#
#   make bridge-all      builddir/hangar-bridge
#   make check-bridge    the suite (part of check): two managers, two topic
#                        directories, two bridges on loopback

BRIDGE_LIB_SRC := msg/bridge_link.c lib/hg/hg_ini.c modules/bridge/bridge_conf.c modules/bridge/blake2s.c
BRIDGE_BIN := $(BUILDDIR)/hangar-bridge
BRIDGE_TEST := $(TESTDIR)/test_bridge

$(BRIDGE_BIN): $(OBJDIR)/modules/bridge/bridge.o $(call obj,$(HG_SRC)) $(call obj,$(BRIDGE_LIB_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) $(LDLIBS)

.PHONY: bridge-all
bridge-all: $(BRIDGE_BIN)
all: $(BRIDGE_BIN)

# The suite: two managers in two roots on loopback, with the toy modules of
# hangard/tests/modules on either side, and a UDP forwarder in the test
# process between the two bridges (loss, replay, a changed byte, a forged
# hello, a recorded session sent again). About 45 s; one run alone with
# test_bridge --only <run> (modules/bridge/README.md "Tests").
$(BRIDGE_TEST): $(OBJDIR)/modules/bridge/tests/test_bridge.o $(OBJDIR)/modules/bridge/blake2s.o \
		$(OBJDIR)/msg/bridge_link.o $(OBJDIR)/msg/depth.o $(call obj,$(HG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) $(LDLIBS)

.PHONY: check-bridge
check-bridge: $(BRIDGE_TEST) $(BRIDGE_BIN) $(BUILDDIR)/hangard $(BUILDDIR)/hangar \
		$(addprefix $(HANGARD_TOYDIR)/,$(HANGARD_TOYS))
	@if HANGARD_BIN=$(abspath $(BUILDDIR)/hangard) HANGAR_BIN=$(abspath $(BUILDDIR)/hangar) \
	    HANGARD_TOYS=$(abspath $(HANGARD_TOYDIR)) HANGAR_MSG_SRC=$(abspath msg) BRIDGE_BIN=$(abspath $(BRIDGE_BIN)) \
	    $(if $(TIMEOUT),$(TIMEOUT) 240) $(abspath $<) > $(TESTDIR)/bridge.log 2>&1; then \
	  echo "ok    bridge"; \
	else \
	  echo "FAIL  bridge"; cat $(TESTDIR)/bridge.log; exit 1; \
	fi
