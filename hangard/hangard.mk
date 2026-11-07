# hangard (the manager), hangar (the command) and their suites. Included by
# the root Makefile; relies on its OBJDIR, TESTDIR, BUILDDIR, HG_SRC,
# HG_CFLAGS, HG_LDLIBS, MSGGEN, TIMEOUT, ALL_LDFLAGS and the generic
# $(OBJDIR)/%.o rule. GPL-3.0-or-later; C11 like lib/hg; nothing beyond libc
# and the kernel, so everything here builds static for musl.
#
#   make hangard-all        builddir/hangard, builddir/hangar, the toy modules
#   make check-hangard      the four suites (part of check)
#   make check-tsan-hangard / check-asan-hangard
#                           the library-side suite (manifests, wiring) under a sanitizer
#   make msg                also regenerates msg/hangar_{system,modules}.{h,c}
#                           from hangard/msg/*.msg

HANGARD_CORE_SRC := hangard/manifest.c hangard/wiring.c
HANGARD_SRC := hangard/hangard.c hangard/conf.c hangard/registry.c hangard/topics.c \
	hangard/spawn.c hangard/health.c hangard/control.c $(HANGARD_CORE_SRC) \
	msg/hangar_system.c msg/hangar_modules.c
HANGAR_CLI_SRC := hangar/hangar.c $(HANGARD_CORE_SRC)
HANGARD_MSG_SRC := hangard/msg/hangar_system.msg hangard/msg/hangar_modules.msg
HANGARD_CPPFLAGS := -D_FILE_OFFSET_BITS=64 -Ilib/hg -Ihangard -Imsg -Ilib/common
HANGARD_TOYS := talker listener crasher neverready eventer actuator neutral
HANGARD_TOYDIR := $(TESTDIR)/hangard-modules
HANGARD_TESTS := hangard_manifest hangard_run hangard_health hangard_adopt
HANGARD_LIB_TESTS := hangard_manifest

# Everything under hangard/, hangar/ and msg/ takes lib/hg's flags.
$(OBJDIR)/hangard/%.o: EE_CFLAGS := $(HG_CFLAGS)
$(OBJDIR)/hangard/%.o: EE_CPPFLAGS := $(HANGARD_CPPFLAGS)
$(OBJDIR)/hangar/%.o: EE_CFLAGS := $(HG_CFLAGS)
$(OBJDIR)/hangar/%.o: EE_CPPFLAGS := $(HANGARD_CPPFLAGS)
$(OBJDIR)/msg/%.o: EE_CFLAGS := $(HG_CFLAGS)
$(OBJDIR)/msg/%.o: EE_CPPFLAGS := -Ilib/hg

$(BUILDDIR)/hangard: $(call obj,$(HANGARD_SRC)) $(call obj,$(HG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) $(LDLIBS)

$(BUILDDIR)/hangar: $(call obj,$(HANGAR_CLI_SRC)) $(call obj,$(HG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) $(LDLIBS)

$(HANGARD_TOYDIR)/%: $(OBJDIR)/hangard/tests/modules/%.o $(call obj,$(HG_SRC)) $(OBJDIR)/msg/depth.o
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) $(LDLIBS)

.SECONDARY: $(addsuffix .o,$(addprefix $(OBJDIR)/hangard/tests/modules/,$(HANGARD_TOYS)))

.PHONY: hangard-all
hangard-all: $(BUILDDIR)/hangard $(BUILDDIR)/hangar $(addprefix $(HANGARD_TOYDIR)/,$(HANGARD_TOYS))
all: $(BUILDDIR)/hangard $(BUILDDIR)/hangar

# The suites: the manifest suite links the parser; the others drive the
# built hangard, hangar and toy modules, whose paths they take from the
# environment, under a $HANGAR_ROOT of their own on tmpfs.
define hangard_test_rule
$(TESTDIR)/test_$(1): $(OBJDIR)/hangard/tests/test_$(1).o $(call obj,$(HANGARD_CORE_SRC)) $(call obj,$(HG_SRC)) $(OBJDIR)/msg/depth.o
	@mkdir -p $$(dir $$@)
	$$(CC) $$(HG_CFLAGS) $$(CFLAGS) $$(ALL_LDFLAGS) -o $$@ $$^ $$(HG_LDLIBS) $$(LDLIBS)
endef
$(foreach t,$(HANGARD_TESTS),$(eval $(call hangard_test_rule,$(t))))

HANGARD_CHECKS := $(addprefix check-,$(HANGARD_TESTS))
.PHONY: $(HANGARD_CHECKS) check-hangard check-hangard-msg check-tsan-hangard check-asan-hangard
$(HANGARD_CHECKS): check-%: $(TESTDIR)/test_% $(BUILDDIR)/hangard $(BUILDDIR)/hangar \
		$(addprefix $(HANGARD_TOYDIR)/,$(HANGARD_TOYS))
	@if HANGARD_BIN=$(abspath $(BUILDDIR)/hangard) HANGAR_BIN=$(abspath $(BUILDDIR)/hangar) \
	    HANGARD_TOYS=$(abspath $(HANGARD_TOYDIR)) HANGAR_MSG_SRC=$(abspath msg) \
	    $(if $(TIMEOUT),$(TIMEOUT) 120) $(abspath $<) > $(TESTDIR)/$*.log 2>&1; then \
	  echo "ok    $*"; \
	else \
	  echo "FAIL  $*"; cat $(TESTDIR)/$*.log; exit 1; \
	fi

check-hangard-msg: $(MSGGEN)
	@if $(MSGGEN) --check --out msg $(HANGARD_MSG_SRC); then \
	  echo "ok    hangard msg --check  $(words $(HANGARD_MSG_SRC)) types from hangard/msg/"; \
	else echo "FAIL  hangard msg --check: run make msg"; exit 1; fi

check-hangard: $(HANGARD_CHECKS) check-hangard-msg

msg: msg-hangard
.PHONY: msg-hangard
msg-hangard: $(MSGGEN)
	$(MSGGEN) --out msg $(HANGARD_MSG_SRC)

# The library-side suite under the sanitizers, as check-tsan/check-asan run
# lib/hg's (same probe, same flags, same build directories).
check-tsan-hangard:
	@mkdir -p $(BUILDDIR); \
	if echo 'int main(void){return 0;}' | $(CC) -fsanitize=thread -x c - -o $(BUILDDIR)/.san-probe-tsan >/dev/null 2>&1; then \
	  $(MAKE) --no-print-directory BUILDDIR=$(BUILDDIR)/tsan \
	    CFLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=thread" LDFLAGS="-fsanitize=thread" \
	    $(addprefix check-,$(HANGARD_LIB_TESTS)); \
	else echo "skip  tsan hangard: $(CC) cannot build with -fsanitize=thread"; fi
check-asan-hangard:
	@mkdir -p $(BUILDDIR); \
	if echo 'int main(void){return 0;}' | $(CC) -fsanitize=address,undefined -x c - -o $(BUILDDIR)/.san-probe-asan >/dev/null 2>&1; then \
	  $(MAKE) --no-print-directory BUILDDIR=$(BUILDDIR)/asan \
	    CFLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined" LDFLAGS="-fsanitize=address,undefined" \
	    $(addprefix check-,$(HANGARD_LIB_TESTS)); \
	else echo "skip  asan hangard: $(CC) cannot build with -fsanitize=address,undefined"; fi
check-tsan: check-tsan-hangard
check-asan: check-asan-hangard
