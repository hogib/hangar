#!/usr/bin/env python3
"""tools/rov/failtest.py: failure tests on the bench, repeated and timed. PROPS OFF.

Each repetition arms the vehicle through hangar, removes one thing it depends on, records
what the system did and when (on the vehicle's own clock), puts everything back (clear,
disarm) and goes on. It runs on the laptop and reaches the vehicle over SSH, which stays
up when a bridge is suspended. hangar must be up first (tools/rov/lab.sh up --bridge).

    failtest.py --props-off TEST [--times N] [--load CORES] [--out DIR]

    safety-kill       the safety module killed while armed
    mavlink-kill      the vehicle's mavlink module killed while armed
    mux-kill          the mux killed while armed
    bridge-hang       the vehicle's bridge suspended for 6 s while armed
    silence SECONDS   the topside's bridge (on this laptop) suspended for SECONDS while
                      armed: 1 and 2.5 should fire nothing, 5 the tether trigger
    arm-in-silence    the topside's bridge suspended, an arm requested 2 s into it
    unplug            armed, then YOU unplug the tether when it says so (40 s watched)
    all               every test above but unplug, with the repetitions of the plan

--times N      repetitions (5)
--load CORES   that many busy loops on the vehicle's computer for the test's duration;
               they end by themselves
--out DIR      where the results go (~/hangar-results/<date>-failtests)

The script arms a vehicle by itself, so it refuses to start without --props-off, which is
your statement that the propellers are off. It leaves the vehicle disarmed, also when it
is interrupted. Rehearsal: LAB_HOST=local, against tools/rov/rehearse.sh's kind of setup.
The vehicle's half is failtest_vehicle.py, beside this file."""
import datetime, json, os, signal, statistics, subprocess, sys, threading, time

here = os.path.dirname(os.path.realpath(__file__))
repo = os.path.realpath(os.path.join(here, "..", ".."))
conf = subprocess.run(["bash", "-c", '. "$1"; echo "$LAB_HOST $LAB_USER $ROV_ROOT"', "_", os.path.join(here, "lab.conf")],
                      capture_output=True, text=True, check=True).stdout.split()
HOST, USER, ROV_ROOT = conf
LOCAL = HOST == "local"
if LOCAL and "ROV_ROOT" not in os.environ:
    ROV_ROOT = "hangar-labtest"
TARGET = "%s@%s" % (USER, HOST)
SSH = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", TARGET]
RV, RT = "/dev/shm/hangar-vehicle", "/dev/shm/hangar-topside"
TOPSIDE_HANGAR = os.path.join(repo, "builddir", "hangar")
VEHICLE_SCRIPT = os.path.join(here, "failtest_vehicle.py")


def on_vehicle(shell, **kw):
    """A shell line on the vehicle's computer, with hangar's paths set."""
    pre = 'R=$HOME/%s; export HANGAR_ROOT=%s FAILTEST_HANGAR=$R/bin/hangar; ' % (ROV_ROOT, RV)
    if LOCAL:
        return subprocess.run(["bash", "-c", pre + shell], text=True, **kw)
    return subprocess.run(SSH + [pre + shell], text=True, **kw)


def vehicle_test(args, on_ready=None, wait=120):
    """failtest_vehicle.py on the vehicle with args. on_ready runs when it has armed."""
    pre = 'R=$HOME/%s; export HANGAR_ROOT=%s FAILTEST_HANGAR=$R/bin/hangar; ' % (ROV_ROOT, RV)
    line = pre + "exec python3 -I - " + " ".join(str(a) for a in args)
    cmd = ["bash", "-c", line] if LOCAL else SSH + [line]
    with open(VEHICLE_SCRIPT) as f:
        p = subprocess.Popen(cmd, stdin=f, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    killer = threading.Timer(wait, p.kill)
    killer.start()
    result = None
    try:
        for out in p.stdout:
            out = out.strip()
            if out == "READY" and on_ready:
                threading.Thread(target=on_ready, daemon=True).start()
            elif out.startswith("RESULT "):
                result = json.loads(out[7:])
        p.wait()
    finally:
        killer.cancel()
    if result is None:
        result = {"ok": False, "error": "no result from the vehicle: " + p.stderr.read()[-300:].strip()}
    return result


def topside_bridge_pid():
    out = subprocess.run([TOPSIDE_HANGAR, "status"], env=dict(os.environ, HANGAR_ROOT=RT),
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 3 and f[0] == "bridge" and f[2].isdigit():
            pid = int(f[2])
            try:
                if open("/proc/%d/comm" % pid).read().strip() == "hangar-bridge":
                    return pid
            except OSError:
                pass
    return None


def suspend_topside(seconds, note):
    def go():
        pid = topside_bridge_pid()
        if pid is None:
            note["topside_error"] = "no topside bridge on this laptop (is it up, with --bridge?)"
            return
        os.kill(pid, signal.SIGSTOP)
        time.sleep(seconds)
        try:
            os.kill(pid, signal.SIGCONT)
        except ProcessLookupError:
            pass
        time.sleep(1.5)
        note["topside_bridge_restarted"] = topside_bridge_pid() != pid
    return go


def reset_topside_bridge():
    """The topside's manager restarts a bridge it finds hung, counts that as a failure,
    lengthens its back-off each time and gives up after its Burst (5 in 60 s): the sixth
    silence would be a bridge left down, and the fourth already longer than asked for.
    A restart by hand starts the count over, so every silence meets the first failure."""
    env = dict(os.environ, HANGAR_ROOT=RT)
    subprocess.run([TOPSIDE_HANGAR, "restart", "bridge"], env=env, capture_output=True)
    end = time.monotonic() + 20
    while time.monotonic() < end:
        out = subprocess.run([TOPSIDE_HANGAR, "status"], env=env, capture_output=True, text=True).stdout
        if any(l.split()[:2] == ["bridge", "ready"] and " up " in l for l in out.splitlines()):
            break
        time.sleep(0.3)
    time.sleep(1.0)


def say_unplug():
    print("\n  >>> ARMED. UNPLUG THE TETHER NOW. Plug it back in when the thrusters are neutral "
          "(about 3 s), or after 10 s.\n", flush=True)


def thermal():
    if LOCAL:
        return None
    r = on_vehicle("vcgencmd measure_temp; vcgencmd get_throttled", capture_output=True)
    return " ".join(r.stdout.split())


def preflight(outdir, name):
    """lab.sh preflight --measure before a block: what else the two machines are doing.
    The report is kept beside the block's results and its verdict goes into every row,
    so a block taken beside a package upgrade or on a throttled Pi is marked as such."""
    try:
        r = subprocess.run(["bash", os.path.join(here, "lab.sh"), "preflight", "--measure"],
                           capture_output=True, text=True, timeout=90)
        out, rc = r.stdout + r.stderr, r.returncode
    except (OSError, subprocess.TimeoutExpired) as e:
        out, rc = "preflight did not run: %s\n" % e, None
    with open(os.path.join(outdir, name + ".preflight.txt"), "a") as f:
        f.write("# %s\n%s\n" % (datetime.datetime.now().isoformat(timespec="seconds"), out))
    fails = [l.strip() for l in out.splitlines() if l.startswith("  FAIL")]
    warns = sum(1 for l in out.splitlines() if l.startswith("  warn"))
    if rc is None:
        print("  preflight: did not run (%s.preflight.txt)" % name)
        return "not run"
    if fails:
        print("  preflight: NOT CLEAN, the block runs all the same and is marked:")
        for l in fails:
            print("    " + l)
        return "NOT CLEAN: " + "; ".join(fails)
    print("  preflight: no FAIL, %d warn (%s.preflight.txt)" % (warns, name))
    return "clean, %d warn" % warns


def start_load(cores, seconds):
    if not cores:
        return
    on_vehicle("for i in $(seq 1 %d); do nohup timeout %d sh -c 'while :; do :; done' > /dev/null 2>&1 & done"
               % (cores, int(seconds)))
    print("  load: %d busy loops on the vehicle's computer, ending by themselves within %d s" % (cores, seconds))
    time.sleep(5)


def stop_load(cores):
    if cores:
        on_vehicle("pkill -u \"$(id -u)\" -f \"^sh -c while :; do :; done\" || true")


def make_safe():
    on_vehicle('"$FAILTEST_HANGAR" call safety.disarm > /dev/null 2>&1; '
               '"$FAILTEST_HANGAR" status 2>/dev/null | grep -E "^(safety|mavlink) " | cut -c1-120')


# name: (vehicle arguments, seconds one repetition takes, what the laptop does at READY)
def spec(test, arg):
    if test == "safety-kill":
        return ["kill", "safety", 4], 25, None
    if test == "mavlink-kill":
        return ["kill", "mavlink", 8], 45, None
    if test == "mux-kill":
        return ["kill", "mux", 5], 30, None
    if test == "bridge-hang":
        return ["hang", "bridge", 6, 9], 45, None
    if test == "silence":
        return ["watch", arg + 5], arg + 40, ("suspend", arg)
    if test == "arm-in-silence":
        return ["arm-in-silence", 2.0, 6], 45, ("suspend", 7.0)
    if test == "unplug":
        return ["watch", 40], 100, ("unplug", 0)
    sys.exit("unknown test %s" % test)


KEYS = ["failsafe_ms", "failsafe_after_last_packet_ms", "outputs_closed_ms", "ardusub_disarming_ms",
        "ardusub_disarmed_ms", "longest_silence_ms"]


def run(test, arg, times, load, outdir):
    name = test + ("-%g" % arg if test == "silence" else "") + ("-load%d" % load if load else "")
    vargs, per, action = spec(test, arg)
    path = os.path.join(outdir, name + ".jsonl")
    print("\n== %s, %d repetitions -> %s" % (name, times, path))
    verdict = preflight(outdir, name)
    start_load(load, times * per + 30)
    rows = []
    try:
        with open(path, "a") as f:
            for i in range(times):
                note, before = {}, thermal()
                ready = None
                if action and action[0] == "suspend":
                    ready = suspend_topside(action[1], note)
                elif action:
                    ready = say_unplug
                r = vehicle_test(vargs, ready, wait=per + 60)
                if action and action[0] == "suspend":
                    reset_topside_bridge()
                r.update(note)
                r.update({"name": name, "rep": i + 1, "at": datetime.datetime.now().isoformat(timespec="seconds"),
                          "load_cores": load, "thermal_before": before, "thermal_after": thermal(),
                          "preflight": verdict})
                f.write(json.dumps(r) + "\n")
                f.flush()
                rows.append(r)
                if not r.get("ok"):
                    print("  %2d  NOT RUN: %s" % (i + 1, r.get("error")))
                    if not r.get("put_back", True) or "not disarmed" in str(r.get("error")):
                        print("      stopping this test: the vehicle could not be put back by the script")
                        break
                    continue
                bits = ["failsafe by %s after %s ms" % (r.get("failsafe_by"), r.get("failsafe_ms")) if r.get("failsafe")
                        else "no failsafe"]
                if r.get("failsafe_after_last_packet_ms") is not None:
                    bits.append("%s ms after the last packet" % r["failsafe_after_last_packet_ms"])
                if r.get("ardusub_disarmed_ms") is not None:
                    bits.append("ArduSub disarmed at %s ms" % r["ardusub_disarmed_ms"])
                if r.get("still_armed_at_end"):
                    bits.append("still armed")
                if "arm_refused" in r:
                    bits.append("arm %s (%s)" % ("refused" if r["arm_refused"] else "ACCEPTED", r.get("arm_reply", "")[:70]))
                if r.get("restarted"):
                    bits.append("restarted: " + ",".join(r["restarted"]))
                if not r.get("put_back"):
                    bits.append("NOT PUT BACK")
                for k in ("topside_error", "topside_bridge_restarted"):
                    if r.get(k):
                        bits.append("%s: %s" % (k, r[k]))
                print("  %2d  %s%s" % (i + 1, "; ".join(bits), "   [%s]" % r["thermal_after"] if r["thermal_after"] else ""))
                if not r.get("put_back"):
                    print("      stopping this test: the vehicle was not put back")
                    break
    finally:
        stop_load(load)
    good = [r for r in rows if r.get("ok")]
    for k in KEYS:
        v = [r[k] for r in good if r.get(k) is not None]
        if v:
            print("  %-30s n=%-2d min %8.1f  median %8.1f  max %8.1f" % (k, len(v), min(v), statistics.median(v), max(v)))
    fired = sum(1 for r in good if r.get("failsafe"))
    print("  failsafe in %d of %d; not run %d" % (fired, len(good), len(rows) - len(good)))
    return rows


PLAN = [("safety-kill", 0, 10), ("mavlink-kill", 0, 5), ("mux-kill", 0, 5), ("bridge-hang", 0, 10),
        ("silence", 5.0, 10), ("silence", 1.0, 10), ("silence", 2.5, 10), ("arm-in-silence", 0, 5)]


def main():
    a = sys.argv[1:]
    if "--props-off" not in a:
        sys.exit(__doc__.split("\n\n")[0] + "\n\nThis script arms the vehicle by itself. Run it with --props-off "
                 "once the propellers are off.")
    a.remove("--props-off")

    def opt(name, default, kind):
        if name in a:
            i = a.index(name)
            v = kind(a[i + 1])
            del a[i:i + 2]
            return v
        return default
    times = opt("--times", None, int)
    load = opt("--load", 0, int)
    outdir = opt("--out", os.path.expanduser("~/hangar-results/%s-failtests" % datetime.date.today().isoformat()), str)
    if not a:
        sys.exit(__doc__)
    test = a[0]
    arg = float(a[1]) if test == "silence" else 0
    os.makedirs(outdir, exist_ok=True)
    r = on_vehicle('"$FAILTEST_HANGAR" status > /dev/null 2>&1', capture_output=True)
    if r.returncode != 0:
        sys.exit("failtest: hangar does not answer on the vehicle's computer (%s): lab.sh up first"
                 % ("this machine" if LOCAL else TARGET))
    try:
        if test == "all":
            for t, x, n in PLAN:
                run(t, x, times or n, load, outdir)
        else:
            run(test, arg, times or 5, load, outdir)
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        print("\nleaving the vehicle disarmed:")
        make_safe()
        print("results in " + outdir)


if __name__ == "__main__":
    main()
