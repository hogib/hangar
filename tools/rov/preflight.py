#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/rov/preflight.py: what a lab session's machines are doing besides hangar, read
before the session and before every measurement (tools/rov/README.md, "Preflight").
tools/rov/lab.sh runs it; it changes nothing on either machine.

  preflight.py probe [--ports udp:5610,tcp:8080 ...] [--services "eeye hellion"]
                     [--root INSTALL] [--hangar-root ROOT[,ROOT]] [--own-pidfiles A,B]
                     [--frozen-file PATH]
        One machine's facts as a JSON object on standard output. The standard library
        alone: lab.sh sends this file to the Pi's python3 on standard input.

  preflight.py judge --mode up|measure --pi FACTS [--laptop FACTS] [--pi-read-at EPOCH]
                     [--own-pids 1,2] [--expect-services "eeye hellion"]
        One line a check, `ok`, `warn` or `FAIL`, with the command that would put it
        right; exit status 1 if any line is FAIL. --mode up: what would keep a session
        from coming up as meant. --mode measure: what would spoil a number.

The checks and their thresholds are the table CHECKS documents (README "Preflight")."""
import glob, json, os, re, resource, socket, subprocess, sys, time

OWN = re.compile(r"^(hangar|hangard|dawn-patrold|hg_bench|failtest)")
# Processes of the package system. unattended-upgrade-shutdown is Ubuntu's idle
# waiter, there from boot to shutdown: not a run. packagekitd and aptd are
# daemons that sit idle for hours on a stock Ubuntu (CI's machines have one):
# not a run either; one that works shows under "other load".
APT_COMM = {"unattended-upgr", "apt", "apt-get", "apt.systemd.dai", "dpkg", "apt-check"}


def read(path, default=None):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return default


def run(cmd, timeout=6):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return None, ""


def processes():
    out = {}
    for d in glob.glob("/proc/[0-9]*"):
        stat = read(d + "/stat")
        if not stat:
            continue
        close = stat.rfind(")")
        f = stat[close + 2:].split()
        try:
            out[int(d[6:])] = (stat[stat.find("(") + 1:close], int(f[11]) + int(f[12]))
        except (ValueError, IndexError):
            pass
    return out


def port_holder(proto, port):
    """Who listens on the port here: None if nobody, else (comm, pid), ("?", 0) when it
    is another user's and the kernel does not say."""
    rc, out = run(["ss", "-H", "-l", "-n", "-p", "-u" if proto == "udp" else "-t", "sport = :%d" % port])
    if rc is None:
        return ("ss not found", -1)
    if not out:
        return None
    m = re.search(r'users:\(\("([^"]*)",pid=(\d+)', out)
    return (m.group(1), int(m.group(2))) if m else ("?", 0)


def mount_options(path):
    best, opts = "", ""
    for line in (read("/proc/self/mountinfo", "") or "").splitlines():
        f = line.split()
        if len(f) > 5 and (path == f[4] or path.startswith(f[4].rstrip("/") + "/")) and len(f[4]) >= len(best):
            best, opts = f[4], f[5]
    return opts


def probe(argv):
    opt = {"--ports": "", "--services": "", "--root": "", "--hangar-root": "", "--own-pidfiles": "", "--frozen-file": ""}
    for i in range(0, len(argv) - 1, 2):
        if argv[i] in opt:
            opt[argv[i]] = argv[i + 1]
    facts = {"host": socket.gethostname(), "user": os.environ.get("USER", "")}
    up = read("/proc/uptime")
    facts["uptime_s"] = float(up.split()[0]) if up else None

    # A process is the session's own if a pid file of the session names it or its
    # process group (the relay is started as a group; its file names the leader).
    own = set()
    for pf in filter(None, opt["--own-pidfiles"].split(",")):
        txt = read(pf if pf.startswith("/") else os.path.join(os.path.expanduser("~"), pf), "") or ""
        if txt.strip().isdigit():
            own.add(int(txt))

    def mine(pid):
        try:
            return pid in own or os.getpgid(pid) in own
        except OSError:
            return False

    # The busiest processes over one second, and the package system's among all.
    a, t0 = processes(), time.monotonic()
    time.sleep(1.0)
    b, dt = processes(), time.monotonic() - t0
    hz = os.sysconf("SC_CLK_TCK")
    busy = sorted(((100.0 * (b[p][1] - a[p][1]) / hz / dt, b[p][0], p) for p in b if p in a and p != os.getpid()),
                  reverse=True)[:5]
    facts["busy"] = [{"pct": round(c, 1), "comm": n, "pid": p, "own": mine(p)} for c, n, p in busy if c >= 1.0]
    apt = []
    for pid, (comm, _) in b.items():
        if comm in APT_COMM:
            cmdline = (read("/proc/%d/cmdline" % pid, "") or "").replace("\0", " ").strip()
            if "unattended-upgrade-shutdown" not in cmdline:
                apt.append({"comm": comm, "pid": pid, "cmdline": cmdline[:120]})
    facts["apt"] = apt
    facts["timers"] = {}
    for t in ("apt-daily.timer", "apt-daily-upgrade.timer"):
        rc, out = run(["systemctl", "is-active", t])
        facts["timers"][t] = out or "unknown"

    temp = read("/sys/class/thermal/thermal_zone0/temp")
    facts["temp_c"] = round(int(temp) / 1000.0, 1) if temp and temp.strip().lstrip("-").isdigit() else None
    rc, out = run(["vcgencmd", "get_throttled"])
    m = re.search(r"0x([0-9a-fA-F]+)", out)
    facts["throttled"] = int(m.group(1), 16) if rc == 0 and m else None
    govs = sorted({(read(g, "") or "").strip() for g in glob.glob("/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor")} - {""})
    facts["governor"] = govs

    mem = re.search(r"MemAvailable:\s+(\d+)", read("/proc/meminfo", "") or "")
    facts["mem_avail_mb"] = int(mem.group(1)) // 1024 if mem else None
    for key, path in (("shm_avail_mb", "/dev/shm"), ("home_avail_mb", os.path.expanduser("~"))):
        try:
            v = os.statvfs(path)
            facts[key] = v.f_bavail * v.f_frsize // (1 << 20)
        except OSError:
            facts[key] = None

    facts["ports"] = []
    for spec in filter(None, opt["--ports"].split(",")):
        proto, port = spec.split(":")
        h = port_holder(proto, int(port))
        facts["ports"].append({"proto": proto, "port": int(port), "held": h is not None, "comm": h[0] if h else "",
                               "pid": h[1] if h else 0, "own": bool(h and h[1] > 0 and mine(h[1]))})
    facts["services"] = {}
    for s in opt["--services"].split():
        rc, out = run(["systemctl", "is-active", s])
        facts["services"][s] = out or "unknown"

    root = opt["--root"]
    if root and not root.startswith("/"):
        root = os.path.join(os.path.expanduser("~"), root)
    man = read(os.path.join(root, "MANIFEST"), "") if root else ""
    facts["release"] = {k: (re.search(r"^%s (\S+)" % k, man or "", re.M) or [None, None])[1] for k in ("version", "commit")}

    # Whether a manager started here could give a module real-time scheduling and
    # locked memory (docs/13 "Real-time scheduling without root").
    hangard = os.path.join(root, "bin", "hangard") if root else ""
    rt = {"rtprio": resource.getrlimit(resource.RLIMIT_RTPRIO)[0], "memlock": resource.getrlimit(resource.RLIMIT_MEMLOCK)[0],
          "root": os.geteuid() == 0, "hangard": os.path.exists(hangard), "caps": None, "nosuid": None}
    if rt["hangard"]:
        rc, out = run(["getcap", hangard])
        rt["caps"] = out.split(None, 1)[1] if rc == 0 and " " in out else ("" if rc == 0 else None)
        rt["nosuid"] = "nosuid" in mount_options(os.path.realpath(hangard)).split(",")
    facts["rt"] = rt

    # What the running manager says is not in force, if one runs.
    # A manager of this session's running here: hangar's modules make themselves
    # non-dumpable, so the kernel does not show whose their sockets are, and a port
    # they hold reads as held by nobody one can name.
    facts["not_in_force"] = None
    roots = [h for h in opt["--hangar-root"].split(",") if h]
    running = []
    for h in roots:
        pid = (read(os.path.join(h, "hangard.pid"), "") or "").strip()
        if pid.isdigit() and (read("/proc/%s/comm" % pid, "") or "").strip() == "hangard":
            running.append(h)
    facts["hangar_running"] = running
    hroot = roots[0] if roots else ""
    cli = os.path.join(root, "bin", "hangar") if root else ""
    if hroot in running and os.path.exists(cli):
        os.environ["HANGAR_ROOT"] = hroot
        rc, out = run([cli, "status"], timeout=5)
        if rc == 0:
            facts["not_in_force"] = [l[len("not in force: "):] for l in out.splitlines() if l.startswith("not in force: ")]
    # lab.sh freeze's record (tools/rov/freeze.sh): there while the machine is frozen.
    facts["frozen"] = None
    if opt["--frozen-file"]:
        rec = read(opt["--frozen-file"])
        if rec:
            m = re.search(r"^frozen_at (\S+)", rec, re.M)
            facts["frozen"] = {"since": m.group(1) if m else "?",
                               "masked": re.findall(r"^masked (\S+)", rec, re.M),
                               "stopped": re.findall(r"^stopped (\S+)", rec, re.M)}
    facts["now"] = time.time()  # last: the caller holds it against its own clock
    json.dump(facts, sys.stdout)
    print()
    return 0


# ---- judging -------------------------------------------------------------------------

# Thresholds, each with its reason (README "Preflight" repeats them).
CLOCK_MAX_S = 2.0        # set_clock's own tolerance; recordings and logs are stamped by it
TEMP_WARN_C = 70.0       # 2026-11-25, with the heat sink: 50 to 62 in every real state
TEMP_FAIL_C = 80.0       # the Pi 5's soft limit: the clock is lowered from here
MEM_WARN_MB, MEM_FAIL_MB = 500, 200
SHM_WARN_MB, SHM_FAIL_MB = 512, 128     # the roots and every topic live in /dev/shm
DISK_WARN_MB, DISK_FAIL_MB = 1000, 100  # recorder sessions and results
BUSY_WARN_PCT = 20.0     # a process that is not hangar's taking a fifth of a core
THROTTLE_NOW = {0x1: "under-voltage", 0x2: "frequency capped", 0x4: "throttled", 0x8: "soft temperature limit"}
THROTTLE_PAST = 0xF0000
# Ports whose loss leaves the session without what it is for; the others are consoles.
ESSENTIAL = lambda p: p["proto"] == "udp"


class Report:
    def __init__(self):
        self.n = {"ok": 0, "warn": 0, "FAIL": 0}
        self.lines = []

    def add(self, level, machine, check, text, fix=None):
        self.n[level] += 1
        self.lines.append("  %-4s  %-7s %s: %s" % (level, machine, check, text))
        if fix and level != "ok":
            self.lines.append("                  -> %s" % fix)


def judge_machine(r, name, f, mode, own_pids, expect_services):
    measure = mode == "measure"
    hard = "FAIL" if measure else "warn"

    # Frozen (lab.sh freeze): the Pi alone; the laptop is its user's own machine.
    if name == "pi":
        fr = f.get("frozen")
        if fr:
            r.add("ok", name, "frozen", "since %s: %d unit(s) masked until the next boot, %s stopped"
                  % (fr["since"], len(fr["masked"]), ", ".join(fr["stopped"]) or "no service"))
        elif measure:
            r.add("FAIL", name, "frozen", "not frozen: the package system can start by itself, and the governor and the lab's "
                  "services are as they happen to be. No two measurements are then taken on the same terms",
                  "lab.sh freeze   (one sudo on the Pi; lab.sh thaw puts it back)")
        else:
            r.add("ok", name, "frozen", "not frozen (lab.sh freeze before measurements)")

    # The package system.
    if f["apt"]:
        who = ", ".join("%s (pid %d)" % (p["comm"], p["pid"]) for p in f["apt"])
        r.add(hard, name, "package system", "running now: %s. It holds a core for minutes and heats the machine" % who,
              "wait for it, or end it: sudo kill %s (systemctl stop does not end the process)"
              % " ".join(str(p["pid"]) for p in f["apt"]))
    else:
        r.add("ok", name, "package system", "nothing of apt or unattended-upgrade runs")
    active = [t for t, s in f["timers"].items() if s == "active"]
    if active:
        up = f.get("uptime_s") or 0
        r.add("warn", name, "apt timers", "%s active%s: unattended-upgrade can start by itself in the session"
              % (" and ".join(active), ", %d min after boot (it has started some 25 min after)" % (up // 60) if up < 3600 else ""),
              "sudo systemctl stop %s   (start them again when leaving)" % " ".join(active))
    else:
        r.add("ok", name, "apt timers", "not active")

    # Heat. The limits are the Pi 5's; another machine's temperature is said, and
    # judged only by what its own throttling reports.
    t, th = f.get("temp_c"), f.get("throttled")
    pi = name == "pi" and th is not None
    if t is None:
        r.add("warn" if measure else "ok", name, "temperature", "cannot be read here")
    elif pi and t >= TEMP_FAIL_C:
        r.add(hard, name, "temperature", "%.1f °C: at the soft limit (%.0f), the clock is being lowered" % (t, TEMP_FAIL_C),
              "let it cool; find what is busy (other load, below)")
    elif pi and t >= TEMP_WARN_C:
        r.add("warn", name, "temperature", "%.1f °C: above every state measured with the heat sink (50 to 62)" % t)
    else:
        r.add("ok", name, "temperature", "%.1f °C" % t)
    if th is not None:
        now = [w for bit, w in THROTTLE_NOW.items() if th & bit]
        if now:
            r.add(hard, name, "throttling", "now: %s (0x%x)" % (", ".join(now), th),
                  "under-voltage: the supply or its cable; the rest: let it cool")
        elif th & THROTTLE_PAST:
            r.add("warn" if measure else "ok", name, "throttling", "not now, but since boot (0x%x): earlier numbers of this boot may be marked by it" % th)
        else:
            r.add("ok", name, "throttling", "none since boot")
    govs = f.get("governor") or []
    if not govs:
        r.add("ok", name, "governor", "none to read")
    elif measure and govs != ["performance"]:
        r.add("warn", name, "governor", "%s: latencies read higher and wider than under performance; say which in the results" % ", ".join(govs),
              "echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor   (put it back when leaving)")
    else:
        r.add("ok", name, "governor", ", ".join(govs))

    # Room.
    for key, what, warn, fail, fix in (
            ("mem_avail_mb", "memory", MEM_WARN_MB, MEM_FAIL_MB, None),
            ("shm_avail_mb", "/dev/shm", SHM_WARN_MB, SHM_FAIL_MB, "old roots and topic files under /dev/shm; lab.sh down removes hangar's"),
            ("home_avail_mb", "disk (home)", DISK_WARN_MB, DISK_FAIL_MB, "old recorder sessions (~/hangar-sessions) and staged releases (~/hangar-visit)")):
        v = f.get(key)
        if v is None:
            r.add("warn", name, what, "cannot be read")
        elif v < fail:
            r.add("FAIL", name, what, "%d MB free (under %d)" % (v, fail), fix)
        elif v < warn:
            r.add("warn", name, what, "%d MB free (under %d)" % (v, warn), fix)
        else:
            r.add("ok", name, what, "%d MB free" % v)

    # What else is busy.
    foreign = [p for p in f.get("busy", []) if p["pct"] >= BUSY_WARN_PCT and not OWN.match(p["comm"])
               and not p.get("own") and p["pid"] not in own_pids]
    if foreign:
        r.add("warn", name, "other load",
              ", ".join("%s %.0f %% (pid %d)" % (p["comm"], p["pct"], p["pid"]) for p in foreign) + " of a core, not this session's",
              "its load is in every number taken now: say so in the results, or stop it")
    else:
        r.add("ok", name, "other load", "no process of another's above %.0f %% of a core" % BUSY_WARN_PCT)

    # Ports (before a session comes up).
    for p in f.get("ports", []):
        label = "%s %d" % (p["proto"].upper(), p["port"])
        if not p["held"]:
            r.add("ok", name, label, "free")
        elif p.get("own") or p["pid"] in own_pids or OWN.match(p["comm"]):
            r.add("ok", name, label, "held by this session's own %s (pid %d)" % (p["comm"], p["pid"]))
        elif p["pid"] < 0:
            r.add("warn", name, label, "cannot be checked: %s" % p["comm"])
        elif p["pid"] == 0 and f.get("hangar_running"):
            r.add("ok", name, label, "held, by a process that does not show its sockets: hangar runs here (%s), and its modules do not"
                  % ", ".join(f["hangar_running"]))
        else:
            who = "%s (pid %d)" % (p["comm"], p["pid"]) if p["pid"] else "a process of another user"
            fix = "sudo systemctl stop %s" % p["comm"] if p["comm"] in ("hellion", "eeye") else "stop it, or move the port in lab.conf"
            r.add("FAIL" if ESSENTIAL(p) and not measure else "warn", name, label, "taken by %s" % who, fix)

    # The lab's own services.
    for s, st in f.get("services", {}).items():
        if st == "active" and measure:
            r.add("warn", name, "service %s" % s, "active: the lab's own, and its load is in the numbers (up --video stops it)")
        elif st == "active":
            r.add("ok", name, "service %s" % s, "active (the lab's own; up --video stops it, down starts it again)")
        elif s in expect_services:
            r.add("warn", name, "service %s" % s, "%s, and it ran when the session came up: down starts it again" % st)
        else:
            r.add("ok", name, "service %s" % s, st)

    # Real-time scheduling and locked memory.
    rt = f.get("rt") or {}
    nif = f.get("not_in_force")
    if nif:
        for line in nif:
            r.add(hard, name, "schedule", "not in force: " + line, "docs/13, \"Real-time scheduling without root\"")
    if rt.get("hangard"):
        caps = rt.get("caps") or ""
        by_cap = "cap_sys_nice" in caps
        by_lim = rt.get("rtprio", 0) > 0
        if rt.get("root") or by_cap or by_lim:
            how = "root" if rt.get("root") else "file capabilities (%s)" % caps if by_cap else "RLIMIT_RTPRIO %d" % rt["rtprio"]
            if by_cap and rt.get("nosuid"):
                r.add("warn", name, "real time", "hangard has %s, but its filesystem is mounted nosuid: the kernel ignores them" % caps,
                      "install under a filesystem without nosuid, or grant by limits (docs/13)")
            else:
                r.add("ok", name, "real time", "reachable by %s; a manifest's [Schedule] decides who gets it" % how)
        else:
            r.add("warn" if measure else "ok", name, "real time",
                  "not reachable: no capabilities on hangard, RLIMIT_RTPRIO 0. Every module runs at the ordinary priority, unlocked",
                  "lab.sh deploy --realtime (sudo setcap, once per install), or limits: docs/13")


def judge(argv):
    opt = {"--mode": "up", "--pi": "", "--laptop": "", "--pi-read-at": "", "--own-pids": "", "--expect-services": ""}
    for i in range(0, len(argv) - 1, 2):
        if argv[i] in opt:
            opt[argv[i]] = argv[i + 1]
    mode = opt["--mode"]
    own = {int(p) for p in opt["--own-pids"].split(",") if p.strip().isdigit()}
    expect = set(opt["--expect-services"].split())
    r = Report()
    facts = {}
    for name, path in (("pi", opt["--pi"]), ("laptop", opt["--laptop"])):
        if not path:
            continue
        try:
            with open(path) as fh:
                facts[name] = json.load(fh)
        except (OSError, ValueError) as e:
            r.add("FAIL", name, "probe", "no facts from this machine (%s)" % e, "is python3 there? does SSH reach it?")
    same = "pi" in facts and "laptop" in facts and facts["pi"].get("host") == facts["laptop"].get("host")
    if "pi" in facts:
        f = facts["pi"]
        if opt["--pi-read-at"] and not same:
            off = float(opt["--pi-read-at"]) - f["now"]
            if abs(off) > CLOCK_MAX_S:
                r.add("FAIL" if mode == "measure" else "warn", "pi", "clock",
                      "%.0f s %s this laptop's: recordings and logs are stamped by it" % (abs(off), "behind" if off > 0 else "ahead of"),
                      "lab.sh up sets it (it asks for the Pi's sudo password)")
            else:
                r.add("ok", "pi", "clock", "%.1f s off this laptop's" % abs(off))
        judge_machine(r, "pi", f, mode, own, expect)
    if "laptop" in facts and not same:
        judge_machine(r, "laptop", facts["laptop"], mode, own, set())
    if "pi" in facts and "laptop" in facts:
        a, b = facts["pi"]["release"], facts["laptop"]["release"]
        if a.get("commit") and b.get("commit") and a["commit"] != b["commit"]:
            r.add("warn", "both", "release", "the Pi has %s (%s), this laptop %s (%s): with --bridge the two refuse each other"
                  % (a.get("version"), a["commit"][:7], b.get("version"), b["commit"][:7]), "lab.sh deploy")
        elif a.get("commit"):
            r.add("ok", "both", "release", "%s (%s)%s" % (a.get("version"), a["commit"][:7], "" if b.get("commit") else " on the Pi; this laptop runs its build tree"))
        else:
            r.add("warn", "pi", "release", "no install found", "lab.sh deploy")
    elif "pi" in facts:
        a = facts["pi"]["release"]
        if a.get("commit"):
            r.add("ok", "pi", "release", "%s (%s)" % (a.get("version"), a["commit"][:7]))
        else:
            r.add("warn", "pi", "release", "no install found", "lab.sh deploy")
    print("\n".join(r.lines))
    print("preflight (%s): %d ok, %d warn, %d FAIL" % (mode, r.n["ok"], r.n["warn"], r.n["FAIL"]))
    return 1 if r.n["FAIL"] else 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "probe":
        sys.exit(probe(sys.argv[2:]))
    if len(sys.argv) > 1 and sys.argv[1] == "judge":
        sys.exit(judge(sys.argv[2:]))
    sys.exit(__doc__)
