#!/usr/bin/env python3
"""failtest_vehicle.py: one failure test, on the vehicle's computer. Started by
tools/rov/failtest.py (over SSH, or on this machine when rehearsing); not run by hand.

    failtest_vehicle.py kill MODULE WINDOW        SIGKILL the module while armed
    failtest_vehicle.py hang MODULE SECONDS WINDOW   SIGSTOP it, SIGCONT after SECONDS
    failtest_vehicle.py watch WINDOW              armed, and nothing done here: the
                                                  topside does it (a silence, an unplug)
    failtest_vehicle.py arm-in-silence AGE WINDOW disarmed: once the last packet from the
                                                  topside is AGE seconds old, ask to arm

It arms through safety.arm (as an operator of this machine), prints READY, does its one
thing, and watches safety.state, ardusub.state and the bridge's link topic for WINDOW
seconds. Every time it reports is one the system stamped itself, on this machine's
monotonic clock: when safety entered a state (since_ns), when a message was published
(publish_ns), when the last packet from the topside came (last_rx_ns). The time of a
kill is this script's, taken immediately before the signal. Then it puts things back
(clear, disarm) and prints RESULT and one JSON object.

HANGAR_ROOT is the vehicle's root; FAILTEST_HANGAR the hangar command."""
import json, os, signal, subprocess, sys, threading, time

ROOT = os.environ.get("HANGAR_ROOT", "/dev/shm/hangar-vehicle")
HANGAR = os.environ.get("FAILTEST_HANGAR") or os.path.expanduser("~/hangar-root/bin/hangar")
ENV = dict(os.environ, HANGAR_ROOT=ROOT)
now = time.monotonic_ns


class Stream:
    """Every message of a topic from now on, as `hangar echo --json` prints them."""

    def __init__(self, topic):
        self.topic, self.msgs, self.lock = topic, [], threading.Lock()
        self.p = subprocess.Popen([HANGAR, "echo", topic, "--json"], env=ENV, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True)
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            try:
                m = json.loads(line)
            except ValueError:
                continue
            with self.lock:
                self.msgs.append(m)

    def last(self):
        with self.lock:
            return self.msgs[-1] if self.msgs else None

    def since(self, t):
        with self.lock:
            return [m for m in self.msgs if m["publish_ns"] >= t]

    def all(self):
        with self.lock:
            return list(self.msgs)

    def stop(self):
        self.p.terminate()


def call(method):
    r = subprocess.run([HANGAR, "call", method], env=ENV, capture_output=True, text=True, timeout=10)
    return r.returncode, " | ".join((r.stdout + r.stderr).split("\n")[:3]).strip()


def modules():
    out = subprocess.run([HANGAR, "status"], env=ENV, capture_output=True, text=True, timeout=10).stdout
    mods = {}
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 4 and f[2].isdigit():
            mods[f[0]] = {"state": f[1], "pid": int(f[2]), "fails": f[3], "line": " ".join(f)[:160]}
    return mods


def wait(pred, seconds, step=0.02):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(step)
    return pred()


def ms(a, b):
    return None if a is None or b is None else round((a - b) / 1e6, 1)


def main():
    test, args = sys.argv[1], sys.argv[2:]
    window = float(args[-1])
    res = {"test": test, "args": args[:-1], "ok": False}
    S, A = Stream("safety.state"), Stream("ardusub.state")
    L = Stream("bridge.vehicle.link")
    if not wait(lambda: S.last() and A.last(), 5):
        res["error"] = "no safety.state or no ardusub.state within 5 s: is hangar up, with an autopilot?"
        return res
    time.sleep(0.3)
    have_link = L.last() is not None
    st = lambda: S.last()["state"]
    armed = lambda: A.last()["armed"]
    link_up = lambda: not have_link or L.last()["state"] == 3

    def put_back():
        """Link up, failsafe cleared, disarmed. True if it got there."""
        wait(link_up, 40, 0.1)
        end = time.monotonic() + 25
        while time.monotonic() < end and st() == 2:
            call("safety.clear")
            wait(lambda: st() != 2, 1.5)
        if st() in (1, 3):
            call("safety.disarm")
        return wait(lambda: st() == 0 and armed() == 0, 10)

    try:
        if st() != 0 or armed() != 0:
            if not put_back():
                res["error"] = "not disarmed at the start (safety state %s, ArduSub armed %s) and it could not be put back" % (st(), armed())
                return res
        if not link_up():
            res["error"] = "the link is not up at the start (state %s)" % L.last()["state"]
            return res
        res["modules_before"] = {k: v["pid"] for k, v in modules().items()}

        if test != "arm-in-silence":
            rc, text = call("safety.arm")
            if not wait(lambda: st() == 1 and armed() == 1, 6):
                res["error"] = "did not arm: %s; safety.state reason %r" % (text, S.last().get("reason"))
                return res
            time.sleep(1.0)
        print("READY", flush=True)

        t0 = now()
        if test == "kill":
            pid = modules()[args[0]]["pid"]
            t0 = now()
            os.kill(pid, signal.SIGKILL)
        elif test == "hang":
            pid = modules()[args[0]]["pid"]
            t0 = now()
            os.kill(pid, signal.SIGSTOP)

            def resume():
                try:
                    os.kill(pid, signal.SIGCONT)
                except ProcessLookupError:
                    pass  # the manager had already killed it and started another
            threading.Timer(float(args[1]), resume).start()
        elif test == "arm-in-silence":
            age = float(args[0])
            if not have_link:
                res["error"] = "no bridge.vehicle.link here: this test needs the bridge"
                return res
            if not wait(lambda: now() - L.last()["last_rx_ns"] >= age * 1e9, 30, 0.01):
                res["error"] = "the topside never fell silent for %.1f s" % age
                return res
            t0 = now()
            res["last_packet_age_at_arm_ms"] = ms(t0, L.last()["last_rx_ns"])
            rc, text = call("safety.arm")
            res["arm_refused"] = rc != 0
            res["arm_reply"] = text[:200]
            wait(lambda: st() in (1, 3), 1.0)
            res["armed_after_request"] = st() in (1, 3) or armed() == 1

        time.sleep(window)

        s_after, a_after, l_all = S.since(t0), A.since(t0), L.all()
        fs = next((m for m in s_after if m["state"] == 2), None)
        dis = next((m for m in a_after if m["armed"] == 0), None)
        dising = next((m for m in a_after if m["armed"] == 2), None)
        closed = next((m for m in s_after if not m["outputs_enabled"]), None)
        res["failsafe"] = fs is not None
        if fs:
            entered = fs.get("since_ns") or fs["publish_ns"]
            res["failsafe_ms"] = ms(entered, t0)
            res["failsafe_by"], res["failsafe_reason"] = fs.get("by"), fs.get("reason")
            res["failsafe_triggers"], res["failsafe_action"] = fs.get("triggers"), fs.get("action")
            rx = [m["last_rx_ns"] for m in l_all if m["publish_ns"] <= entered and m["last_rx_ns"]]
            if rx:
                res["failsafe_after_last_packet_ms"] = ms(entered, max(rx))
        res["outputs_closed_ms"] = ms(closed["publish_ns"], t0) if closed else None
        if test != "arm-in-silence":  # there it was disarmed all along
            res["ardusub_disarming_ms"] = ms(dising["publish_ns"], t0) if dising else None
            res["ardusub_disarmed_ms"] = ms(dis["publish_ns"], t0) if dis else None
        res["still_armed_at_end"] = st() == 1 and armed() == 1
        if have_link:
            gaps = [m["publish_ns"] - m["last_rx_ns"] for m in l_all if m["publish_ns"] >= t0 and m["last_rx_ns"]]
            res["longest_silence_ms"] = round(max(gaps) / 1e6, 1) if gaps else None
            res["link_states"] = sorted({m["state"] for m in l_all if m["publish_ns"] >= t0})
        res["states"] = [[ms(m["publish_ns"], t0), m["state"], m.get("triggers"), m.get("reason")]
                         for i, m in enumerate(s_after)
                         if i == 0 or (m["state"], m.get("triggers")) != (s_after[i - 1]["state"], s_after[i - 1].get("triggers"))][:12]
        res["ok"] = True
        return res
    finally:
        try:
            res["put_back"] = put_back()
            res["end"] = {"safety_state": st(), "ardusub_armed": armed()}
            after = modules()
            before = res.get("modules_before", {})
            res["restarted"] = sorted(k for k, v in after.items() if k in before and v["pid"] != before[k])
            res["not_ready"] = sorted(k for k, v in after.items() if v["state"] != "ready")
            res.pop("modules_before", None)
        except Exception as e:  # the report matters more than a tidy end
            res["put_back_error"] = repr(e)
        for x in (S, A, L):
            x.stop()


if __name__ == "__main__":
    try:
        r = main()
    except Exception as e:
        r = {"ok": False, "error": repr(e)}
    print("RESULT " + json.dumps(r), flush=True)
