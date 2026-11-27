# bench/pairs/pairs.mk -- the node pairs of bench/pairs that need nothing but this
# checkout: hangar's, and the null system (bench/pairs/README.md).
#
# A GNU make fragment, included by the root Makefile after bench/bench.mk, whose
# HG_COMMIT, HG_SRC, HG_CFLAGS, HG_LDLIBS and obj it uses:
#
#   make pairs         $(BUILDDIR)/hangar_pair, null_pair and unix_pair
#   make check-pairs   the driver's smoke plan over them (part of check): the harness
#                      proved, no number judged
#   make check-baselines   the same over every system of the registry, in a container
#                      (not part of check)
#
# The compiler and its flags go into each node (--version), and from there into every
# results file.

PAIR_BINS := $(BUILDDIR)/hangar_pair $(BUILDDIR)/null_pair $(BUILDDIR)/unix_pair \
	$(BUILDDIR)/hangar_graph $(BUILDDIR)/unix_graph
PAIR_DEFS = -DPAIR_CFLAGS='"$(CC) $(HG_CFLAGS) $(CFLAGS)"' -DHG_COMMIT='"$(HG_COMMIT)"'

$(OBJDIR)/bench/pairs/hangar_pair.o: bench/pairs/hangar_pair.c bench/pairs/pair.h
	@mkdir -p $(dir $@)
	$(CC) $(LIB_CPPFLAGS) $(CPPFLAGS) $(PAIR_DEFS) $(HG_CFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/bench/pairs/null_pair.o: bench/pairs/null_pair.c bench/pairs/pair.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(PAIR_DEFS) $(HG_CFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/hangar_pair: $(OBJDIR)/bench/pairs/hangar_pair.o $(call obj,$(HG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) $(LDLIBS)

# The graph's modules (graph.h): hangar's, and the Unix sockets'.
$(OBJDIR)/bench/pairs/hangar_graph.o: bench/pairs/hangar_graph.c bench/pairs/graph.h bench/pairs/pair.h
	@mkdir -p $(dir $@)
	$(CC) $(LIB_CPPFLAGS) $(CPPFLAGS) $(PAIR_DEFS) $(HG_CFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/hangar_graph: $(OBJDIR)/bench/pairs/hangar_graph.o $(call obj,$(HG_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(HG_LDLIBS) $(LDLIBS)

$(BUILDDIR)/unix_graph: bench/pairs/unix_graph.c bench/pairs/graph.h bench/pairs/pair.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(PAIR_DEFS) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $< $(LDLIBS)

# The baselines that need nothing but libc (the others: tools/baselines.sh).
$(BUILDDIR)/unix_pair: bench/pairs/unix_pair.c bench/pairs/pair.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(PAIR_DEFS) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $< $(LDLIBS)

$(BUILDDIR)/null_pair: $(OBJDIR)/bench/pairs/null_pair.o
	@mkdir -p $(dir $@)
	$(CC) $(HG_CFLAGS) $(CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(LDLIBS)

# Every entry of the registry, the baselines' included, through the smoke plan in a
# container that has ROS 2 (tools/baselines.sh ros2-smoke: docker, cargo, the image of
# bench/ros2/Dockerfile). Not part of check: it needs what a stock CI machine lacks, and
# minutes. A proof that each system's node runs, never a measurement.
.PHONY: check-baselines
check-baselines:
	@mkdir -p $(BUILDDIR); if tools/baselines.sh ros2-smoke > $(BUILDDIR)/baselines-smoke.log 2>&1 && \
	    ! grep -q 'SKIP' $(BUILDDIR)/baselines-smoke.log; then echo "ok    baselines"; \
	else echo "FAIL  baselines"; tail -30 $(BUILDDIR)/baselines-smoke.log; exit 1; fi

.PHONY: pairs check-pairs
pairs: $(PAIR_BINS)

check-pairs: $(PAIR_BINS)
	@mkdir -p $(TESTDIR); rm -rf $(TESTDIR)/pairs-smoke; \
	if python3 -I bench/pairs/tests/test_driver.py $(abspath $(BUILDDIR)) $(abspath $(TESTDIR))/pairs-smoke > $(TESTDIR)/pairs.log 2>&1; then \
	  echo "ok    bench pairs"; \
	else echo "FAIL  bench pairs"; cat $(TESTDIR)/pairs.log; exit 1; fi
