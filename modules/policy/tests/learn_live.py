#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""hangar.learn and hangar.topic against the running toy policy, run by
test_policy.c while it drives the anchor (docs/09 "Tests"): the live
reader on policy.toy.wrench (the C writer's file), runtime_info, deploy of
a file (until the bridge: the incoming directory and load_weights), watch,
revert, a deploy rejected by its probes. Running good_a on entry and on exit.

    learn_live.py <models dir> <dataset hash, hex> <good_a hash, hex>
Exit 0, or 1 with the failures printed."""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.environ.get("POLICY_PYTHON") or os.path.join(HERE, "..", "..", "..", "python"))

from hangar import learn, topic  # noqa: E402

fails = []


def check(cond, what):
    if not cond:
        fails.append(what)
        print("    FAIL", what)


def main():
    models, ds_hash, hash_a = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)

    # The live reader: about 50 Hz, heave = 0.5 a + 0.25 t + 0.1 with a = 1, t = 2.
    t = topic.open_topic("policy.toy.wrench", timeout=2.0)
    t0 = time.monotonic()
    got = []
    while time.monotonic() - t0 < 0.5:
        got += t.read()
        time.sleep(0.01)
    check(15 <= len(got) <= 35, "policy.toy.wrench: %d messages in 0.5 s" % len(got))
    if got:
        w = t.decode(got[-1])
        check(abs(w["heave"] - 1.1) < 1e-4, "heave %r, want 1.1" % w["heave"])
        check(not got[-1].flags & 1, "the output is flagged stale")
        active_id = topic.open_topic("policy.toy.active", start="oldest").topic_id
        check(any(tid == active_id for tid, _ in got[-1].refs), "no reference to policy.toy.active")
        check(sum(m.gap for m in got[1:]) == 0, "gaps in a 0.5 s read")
    print("    hangar.topic: %d messages in 0.5 s, heave %.3f" % (len(got), w["heave"] if got else float("nan")))

    rt = learn.runtime_info("toy")
    check(rt["dataset"] == "toy" and rt["dataset_hash"] == ds_hash, "runtime_info: %r" % rt)
    check(("dense", 1) in rt["ops"], "runtime_info ops: %r" % rt["ops"])

    # deploy a file; watch sees it swapped in.
    w_iter = learn.watch("toy", timeout=3.0)
    t1 = time.monotonic()
    r = learn.deploy(os.path.join(models, "good_b.hgw"), "toy")
    dt = time.monotonic() - t1
    check(r["state"] == "done", "deploy good_b: %r" % r)
    seen = None
    for kind, d in w_iter:
        if kind == "active" and d["hash"] == r["hash"]:
            seen = d
            break
    check(seen is not None and seen["cause"] == 1, "watch: no active message for good_b")
    print("    deploy (file to swap, through hangar call): %.0f ms, version %d" % (dt * 1000, r["version"]))

    # revert: good_a back.
    rv = learn.revert("toy")
    check(int(rv["hash"]) == hash_a, "revert: %r, want hash %016x" % (rv, hash_a))

    # A probe that does not reproduce: rejected by check 6, good_a still running.
    r = learn.deploy(os.path.join(models, "bad_probe.hgw"), "toy")
    check(r["state"] == "rejected" and r["check"] == 6, "deploy bad_probe: %r" % r)
    try:
        learn.call("policy_toy.no_such_method")
        check(False, "a call to an unknown method succeeded")
    except learn.LearnError:
        pass
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
