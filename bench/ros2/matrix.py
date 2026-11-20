#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bench/ros2/matrix.py: hangar against a ROS 2 talker/listener pair, on this machine.

run.sh builds the ROS 2 package and calls this; it is not meant to be started by
hand except to print a report again:

    python3 bench/ros2/matrix.py --report results.json > results.md

It runs, `--reps` times over, hg_bench and then the same measurements through
every ROS 2 middleware configuration that is installed (CONFIGS below), each on
the same two CPUs, and records the governor, the temperature and the throttling
state before and after every single run. A run during which the machine was
hot, throttled or frequency-capped is kept and flagged, never dropped: the
table marks the cell.

Output: <out>/results.json (every run, the summary, the factors), <out>/results.md
(the table), <out>/logs/ (what the processes wrote to stderr).
"""
import argparse
import datetime
import glob
import json
import os
import shutil
import signal
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True  # nothing is written into the checkout
sys.path.insert(0, os.path.dirname(HERE))
import procbench  # noqa: E402  (bench/procbench.py: pss_kb, temperature, model)

PKG = 'hangar_ros2_bench'
SIZES = (16, 256, 4096, 1048576)
RATES = (100, 1000, 10000)
QOS = ('latest', 'queue')
FRAME = (1048576, 30)  # a camera's frames: 1 MiB at 30 Hz
ROS_DISTRO = os.environ.get('ROS_DISTRO', 'jazzy')

# name -> what it is. `need` is checked before anything runs; a configuration
# whose need is not met is skipped with one line that says how one would get it.
# Nothing is ever installed from here.
CONFIGS = [
    {
        'name': 'fastdds',
        'title': 'Fast DDS, as installed (UDP and its shared-memory transport)',
        'rmw': 'rmw_fastrtps_cpp',
        'env': {},
    },
    {
        'name': 'fastdds-datasharing',
        'title': 'Fast DDS, data-sharing delivery and loaned messages (config/fastdds-datasharing.xml)',
        'rmw': 'rmw_fastrtps_cpp',
        'env': {
            'FASTRTPS_DEFAULT_PROFILES_FILE': os.path.join(HERE, 'config', 'fastdds-datasharing.xml'),
            'FASTDDS_DEFAULT_PROFILES_FILE': os.path.join(HERE, 'config', 'fastdds-datasharing.xml'),
            'RMW_FASTRTPS_USE_QOS_FROM_XML': '1',
            'ROS_DISABLE_LOANED_MESSAGES': '0',
        },
        'loan': True,
    },
    {
        'name': 'cyclonedds',
        'title': 'Cyclone DDS, as installed (UDP over loopback)',
        'rmw': 'rmw_cyclonedds_cpp',
        'env': {},
    },
    {
        'name': 'cyclonedds-iceoryx',
        'title': 'Cyclone DDS over iceoryx shared memory, loaned messages (config/cyclonedds-iceoryx.xml, the RouDi daemon running)',
        'rmw': 'rmw_cyclonedds_cpp',
        'env': {
            'CYCLONEDDS_URI': 'file://' + os.path.join(HERE, 'config', 'cyclonedds-iceoryx.xml'),
            'ROS_DISABLE_LOANED_MESSAGES': '0',
        },
        'loan': True,
        'daemon': 'iox-roudi',
    },
    {
        'name': 'zenoh',
        'title': 'Zenoh (rmw_zenoh_cpp), as installed, its router running',
        'rmw': 'rmw_zenoh_cpp',
        'env': {},
        'daemon': 'rmw_zenohd',
    },
    {
        'name': 'zenoh-shm',
        'title': 'Zenoh with its shared-memory transport enabled (ZENOH_CONFIG_OVERRIDE), its router running',
        'rmw': 'rmw_zenoh_cpp',
        'env': {'ZENOH_CONFIG_OVERRIDE': 'transport/shared_memory/enabled=true'},
        'daemon': 'rmw_zenohd',
    },
]
INSTALL_HINT = {
    'rmw_fastrtps_cpp': f'sudo apt install ros-{ROS_DISTRO}-rmw-fastrtps-cpp',
    'rmw_cyclonedds_cpp': f'sudo apt install ros-{ROS_DISTRO}-rmw-cyclonedds-cpp',
    'rmw_zenoh_cpp': f'sudo apt install ros-{ROS_DISTRO}-rmw-zenoh-cpp',
    'iox-roudi': f'sudo apt install ros-{ROS_DISTRO}-iceoryx-posh (it comes with ros-{ROS_DISTRO}-cyclonedds)',
}

LIVE = []  # every process started and not yet reaped


def say(*a):
    print('bench-ros2:', *a, flush=True)


# ---- the machine ---------------------------------------------------------------------

def read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


def throttled():
    """The Pi firmware's throttling word (bits 0-3: now; 16-19: since boot), or None."""
    if shutil.which('vcgencmd'):
        try:
            out = subprocess.run(['vcgencmd', 'get_throttled'], capture_output=True, text=True, timeout=5).stdout
            return int(out.strip().split('=')[1], 16)
        except (OSError, subprocess.SubprocessError, IndexError, ValueError):
            pass
    v = read('/sys/devices/platform/soc/soc:firmware/get_throttled')
    try:
        return int(v, 16) if v else None
    except ValueError:
        return None


def state(cpus):
    govs = sorted({g for g in (read(p) for p in glob.glob('/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor')) if g})
    freq = {}
    for c in cpus:
        d = f'/sys/devices/system/cpu/cpu{c}/cpufreq/'
        v = [read(d + n) for n in ('scaling_cur_freq', 'scaling_max_freq', 'cpuinfo_max_freq')]
        if all(v):
            freq[str(c)] = [int(x) for x in v]
    counts = [read(p) for p in glob.glob('/sys/devices/system/cpu/cpu[0-9]*/thermal_throttle/*_throttle_count')]
    temp = procbench.temperature()
    # Jiffies so far, busy and all, of the CPUs the pair is NOT pinned to: what
    # they do during a run is somebody else's load (an apt run, another job).
    busy = total = 0
    for line in (read('/proc/stat') or '').splitlines():
        f = line.split()
        if f and f[0][:3] == 'cpu' and f[0][3:].isdigit() and int(f[0][3:]) not in cpus:
            v = [int(x) for x in f[1:9]]
            total += sum(v)
            busy += sum(v) - v[3] - v[4]
    return {
        'other_cpus_jiffies': [busy, total],
        'time': datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
        'temp_c': None if temp != temp else round(temp, 1),
        'governor': govs or ['none'],
        'throttled': throttled(),
        'freq_khz': freq,  # per pinned CPU: now, the limit in force, the hardware's
        'x86_throttle_count': sum(int(c) for c in counts if c) if counts else None,
        'loadavg1': float(read('/proc/loadavg').split()[0]),
    }


def other_load(before, after):
    """How busy the CPUs beside the pair's were between two states, in percent."""
    (b0, t0), (b1, t1) = before['other_cpus_jiffies'], after['other_cpus_jiffies']
    return round(100.0 * (b1 - b0) / (t1 - t0), 1) if t1 > t0 else 0.0


def flags(before, after, hot, busy=25.0):
    f = []
    if other_load(before, after) >= busy:
        f.append('other-load')
    tb, ta = before['throttled'], after['throttled']
    if (tb or 0) & 0xF or (ta or 0) & 0xF:
        f.append('throttled')
    elif ta is not None and tb is not None and (ta >> 16) & ~(tb >> 16) & 0xF:
        f.append('throttled-during')
    temps = [s['temp_c'] for s in (before, after) if s['temp_c'] is not None]
    if temps and max(temps) >= hot:
        f.append('hot')
    if any(v[1] < v[2] for s in (before, after) for v in s['freq_khz'].values()):
        f.append('frequency-capped')
    if before['governor'] != after['governor']:
        f.append('governor-changed')
    if before['x86_throttle_count'] is not None and after['x86_throttle_count'] != before['x86_throttle_count']:
        f.append('throttled-during')
    return f


def cool_down(args):
    """Waits, up to --cool-wait seconds, for the temperature to fall below --max-temp."""
    t0 = time.monotonic()
    while True:
        t = procbench.temperature()
        if t != t or t < args.max_temp or time.monotonic() - t0 >= args.cool_wait:
            return round(time.monotonic() - t0, 1)
        time.sleep(2)


def machine_name():
    """The board's name on a Pi (procbench.model), the processor's elsewhere."""
    m = procbench.model()
    if m != os.uname().machine:
        return m
    for line in (read('/proc/cpuinfo') or '').splitlines():
        if line.startswith('model name'):
            return line.split(':', 1)[1].strip()
    return m


# ---- processes -----------------------------------------------------------------------

def spawn(cmd, env, cpus, log, out=subprocess.PIPE):
    """stdout is the result line, read at the end; a daemon's goes to the log
    (nobody would read its pipe, and a full pipe stops the writer)."""
    p = subprocess.Popen(cmd, env=env, stdout=out, stderr=log, text=True,
                         preexec_fn=lambda: os.sched_setaffinity(0, cpus))
    LIVE.append(p)
    return p


def stop(p, sig=signal.SIGINT, wait=5):
    if p.poll() is None:
        try:
            p.send_signal(sig)
            p.wait(wait)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
    if p in LIVE:
        LIVE.remove(p)
    if p.stdout:
        p.stdout.close()


def cpu_ticks(pid):
    st = read(f'/proc/{pid}/stat')
    if not st:
        return None
    f = st[st.rindex(')') + 2:].split()
    return int(f[11]) + int(f[12])


def last_json(text):
    for line in reversed(text.splitlines()):
        if line.startswith('{'):
            try:
                return json.loads(line)
            except ValueError:
                pass
    return None


def wait_all(procs, timeout, watch=()):
    """Waits for `procs` (name -> Popen), sampling the PSS of them and of `watch`
    (name -> pid) twice a second once they are a second old. Returns
    (outputs by name, median PSS in kB by name, timed out)."""
    t0 = time.monotonic()
    pss = {}
    pids = {n: p.pid for n, p in procs.items()}
    pids.update(watch)
    timed_out = False
    while any(p.poll() is None for p in procs.values()):
        time.sleep(0.5)
        el = time.monotonic() - t0
        if el > 1.0:
            for n, pid in pids.items():
                k = procbench.pss_kb(pid)
                if k is not None:
                    pss.setdefault(n, []).append(k)
        if el > timeout:
            timed_out = True
            break
    out = {}
    for n, p in procs.items():
        if p.poll() is None:
            stop(p, signal.SIGKILL)
        try:
            text = p.stdout.read()
        except (OSError, ValueError):
            text = ''
        stop(p)
        out[n] = last_json(text) or {'error': f'no result (exit status {p.returncode})'}
    return out, {n: statistics.median(v) for n, v in pss.items()}, timed_out


class Ros:
    """One middleware configuration: its environment, its daemon, its runs."""

    def __init__(self, cfg, args, ctx):
        self.cfg, self.args, self.ctx = cfg, args, ctx
        self.bin = os.path.join(args.prefix, 'lib', PKG)
        self.env = dict(os.environ)
        self.env.update({
            'RMW_IMPLEMENTATION': cfg['rmw'],
            'ROS_DOMAIN_ID': str(args.domain),
            'ROS_AUTOMATIC_DISCOVERY_RANGE': args.discovery_range,
            'ROS_LOG_DIR': os.path.join(args.out, 'logs', 'ros'),
            'RCUTILS_COLORIZED_OUTPUT': '0',
        })
        self.env.update(cfg['env'])
        self.log = open(os.path.join(args.out, 'logs', cfg['name'] + '.log'), 'a')
        self.daemon = None

    def start_daemon(self):
        d = self.cfg.get('daemon')
        if not d:
            return
        self.daemon = spawn([self.ctx['daemons'][d]], self.env, self.ctx['housekeeping'], self.log, out=self.log)
        time.sleep(3)
        if self.daemon.poll() is not None:
            raise RuntimeError(f'{d} exited at once (status {self.daemon.returncode}); see logs/{self.cfg["name"]}.log')

    def stop_daemon(self):
        if self.daemon:
            stop(self.daemon)
            self.daemon = None

    def close(self):
        self.stop_daemon()
        self.log.close()

    def cmd(self, exe, *opts, pinned=None):
        c = [os.path.join(self.bin, exe)] + [str(o) for o in opts]
        if pinned is not None:
            c += ['--cpu', str(pinned)]
        if self.cfg.get('loan'):
            c.append('--loan')
        return c

    def one(self, *opts, timeout=600):
        """A talker alone (cost, take), on the writer's CPU."""
        p = spawn(self.cmd('hgb_talker', *opts, pinned=self.ctx['writer']), self.env, self.ctx['allowed'], self.log)
        out, _, _ = wait_all({'talker': p}, timeout)
        return out

    def pair(self, *opts, pin=True, seconds=0, startup=False):
        """A talker and a listener, two processes. Returns their two results,
        the PSS of each (and of the daemon) and the daemon's CPU over the run."""
        w, r = (self.ctx['writer'], self.ctx['reader']) if pin else (None, None)
        watch = {'daemon': self.daemon.pid} if self.daemon else {}
        d0, t0 = (cpu_ticks(self.daemon.pid), time.monotonic()) if self.daemon else (None, 0)
        start_ns = time.monotonic_ns()  # CLOCK_MONOTONIC, the clock the listener stamps with
        if startup:
            talker = spawn(self.cmd('hgb_talker', *opts, pinned=w), self.env, self.ctx['allowed'], self.log)
            listener = spawn(self.cmd('hgb_listener', *opts, pinned=r), self.env, self.ctx['allowed'], self.log)
            out, pss, timed_out = wait_all({'listener': listener}, 90)
            stop(talker)
            out['start_ns'] = start_ns
        else:
            listener = spawn(self.cmd('hgb_listener', *opts, pinned=r), self.env, self.ctx['allowed'], self.log)
            talker = spawn(self.cmd('hgb_talker', *opts, pinned=w), self.env, self.ctx['allowed'], self.log)
            out, pss, timed_out = wait_all({'talker': talker, 'listener': listener}, seconds + 120, watch)
        out['pss_kb'] = pss
        if timed_out:
            out['error'] = 'timed out'
        if self.daemon:
            d1 = cpu_ticks(self.daemon.pid)
            if d0 is not None and d1 is not None:
                out['daemon_cpu_pct'] = round(100.0 * (d1 - d0) / os.sysconf('SC_CLK_TCK') / (time.monotonic() - t0), 3)
        return out

    def composed(self, *opts, seconds=0):
        p = spawn(self.cmd('hgb_composed', *opts, pinned=self.ctx['reader']), self.env, self.ctx['allowed'], self.log)
        out, pss, _ = wait_all({'composed': p}, seconds + 120)
        out['pss_kb'] = pss
        return out


def ros_cases(args):
    """(case name, how to run it) for one ROS 2 configuration, in order."""
    s, w, cnt = args.seconds, args.warmup, args.ros_count
    cases = []
    for size in SIZES:
        n = max(1, cnt // 256) if size > 65536 else cnt
        cases.append((f'cost_{size}', lambda r, size=size, n=n: r.one(
            '--mode', 'cost', '--size', size, '--count', n, '--runs', args.cost_runs)))
    for size in SIZES:
        n = max(1, cnt // 256) if size > 65536 else cnt
        cases.append((f'take_{size}', lambda r, size=size, n=n: r.one(
            '--mode', 'take', '--size', size, '--count', n, '--runs', args.cost_runs)))
    for rate in RATES:
        for qos in QOS:
            cases.append((f'rate_256_{rate}_{qos}', lambda r, rate=rate, qos=qos: r.pair(
                '--mode', 'rate', '--size', 256, '--rate', rate, '--qos', qos, '--seconds', s,
                '--warmup', w, seconds=s + w)))
    cases.append(('nopin_256_1000_latest', lambda r: r.pair(
        '--mode', 'rate', '--size', 256, '--rate', 1000, '--qos', 'latest', '--seconds', s,
        '--warmup', w, pin=False, seconds=s + w)))
    cases.append((f'frame_{FRAME[0]}_{FRAME[1]}_latest', lambda r: r.pair(
        '--mode', 'rate', '--size', FRAME[0], '--rate', FRAME[1], '--qos', 'latest', '--seconds', s,
        '--warmup', w, seconds=s + w)))
    cases.append(('idle', lambda r: r.pair(
        '--mode', 'idle', '--idle-seconds', args.idle_seconds, seconds=args.idle_seconds)))
    cases.append(('startup', lambda r: r.pair(
        '--mode', 'startup', '--size', 256, '--rate', 1000, '--seconds', 60, startup=True)))
    cases.append(('composed_256_1000_latest', lambda r: r.composed(
        '--size', 256, '--rate', 1000, '--qos', 'latest', '--seconds', s, '--warmup', w, seconds=s + w)))
    return cases


def hangar_cases(args, ctx):
    """(case name, CPUs, hg_bench arguments). An hg_bench that does not know
    --wake-bytes (before bench-ros2) loses the frame case, and says so."""
    w, r = ctx['writer'], ctx['reader']
    s = ['--seconds', str(args.seconds)]
    cases = [
        ('hg_cost', {w}, ['publish', 'latest', '--runs', str(args.cost_runs), '--count', str(args.hg_count)]),
        ('hg_pair', {w, r}, ['wake', 'idle', 'rss', '--idle-seconds', str(args.idle_seconds)] + s),
        ('hg_nopin', ctx['allowed'], ['wake_nopin'] + s),
    ]
    if ctx['hg_frame']:
        cases.append(('hg_frame', {w, r}, ['wake', '--wake-bytes', str(FRAME[0]), '--wake-rate', str(FRAME[1])] + s))
    return cases


def run_hg(args, ctx, cpus, hg_args, tag):
    path = os.path.join(args.out, 'logs', f'hg_bench-{tag}.json')
    with open(os.path.join(args.out, 'logs', 'hangar.log'), 'a') as log:
        p = subprocess.Popen([args.hg_bench] + hg_args + ['--json', path], stdout=log, stderr=log,
                             preexec_fn=lambda: os.sched_setaffinity(0, cpus))
        LIVE.append(p)
        try:
            p.wait(600 + 20 * args.seconds)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
        LIVE.remove(p)
    try:
        with open(path) as f:
            out = json.load(f)
    except (OSError, ValueError):
        out = {'error': f'hg_bench {" ".join(hg_args)}: no report (exit status {p.returncode})'}
    if p.returncode != 0 and 'error' not in out:
        out['error'] = f'hg_bench exit status {p.returncode}'
    return out


# ---- from runs to numbers ------------------------------------------------------------

def ok(d):
    return isinstance(d, dict) and 'error' not in d


def ros_metrics(case, res):
    m = {}
    t, l = res.get('talker'), res.get('listener')
    kind = case.split('_')[0]
    if kind == 'cost' and ok(t):
        m[f'publish_ns_{case.split("_")[1]}'] = t['ns']
    elif kind == 'take' and ok(t) and not t.get('gave_up'):
        m[f'latest_ns_{case.split("_")[1]}'] = t['ns']
    elif kind in ('rate', 'nopin', 'frame') and ok(t) and ok(l) and l.get('samples'):
        if kind == 'rate':
            sfx = '_' + '_'.join(case.split('_')[2:])
        else:
            sfx = '_' + kind
        for k in ('p50', 'p99', 'p999', 'max'):
            m[f'lat_{k}{sfx}'] = l[f'{k}_us']
        m['cpu_pct' + sfx] = t['cpu_pct'] + l['cpu_pct']
        win = max(t['window_s'], 1e-9), max(l['window_s'], 1e-9)
        m['ctxsw_s' + sfx] = sum(t['ctxsw']) / win[0] + sum(l['ctxsw']) / win[1]
        m['lost_pct' + sfx] = 100.0 * l['lost'] / max(1, l['expected'])
        if case == 'rate_256_1000_latest':
            pss = res.get('pss_kb', {})
            if 'talker' in pss and 'listener' in pss:
                m['pss_kb'] = pss['talker'] + pss['listener']
            if 'daemon' in pss:
                m['daemon_pss_kb'] = pss['daemon']
            if 'daemon_cpu_pct' in res:
                m['daemon_cpu_pct'] = res['daemon_cpu_pct']
            m['threads'] = t.get('threads', 0) + l.get('threads', 0)
    elif kind == 'idle' and ok(t) and ok(l):
        m['idle_listener_ctxsw_s'] = sum(l['ctxsw']) / max(l['window_s'], 1e-9)
        m['idle_listener_cpu_pct'] = l['cpu_pct']
        m['idle_talker_ctxsw_s'] = sum(t['ctxsw']) / max(t['window_s'], 1e-9)
        m['idle_talker_cpu_pct'] = t['cpu_pct']
    elif kind == 'startup' and ok(l) and l.get('first_msg_ns'):
        m['startup_ms'] = (l['first_msg_ns'] - res['start_ns']) / 1e6
    elif kind == 'composed':
        c = res.get('composed')
        if ok(c) and c.get('samples'):
            for k in ('p50', 'p99', 'p999', 'max'):
                m[f'composed_lat_{k}'] = c[f'{k}_us']
            m['composed_cpu_pct'] = c['cpu_pct']
            m['composed_ctxsw_s'] = sum(c['ctxsw']) / max(c['window_s'], 1e-9)
            if 'composed' in res.get('pss_kb', {}):
                m['composed_pss_kb'] = res['pss_kb']['composed']
    return m


def hg_wake(w, sfx):
    """A wake object of hg_bench's report. The latency is to the message copied
    out of the ring where the report has it (hg_bench since bench-ros2), else
    to the wake-up, a `latest` (0.2 us at 256 B) earlier."""
    m = {}
    pre = 'copied_' if 'copied_p50_us' in w else ''
    for k in ('p50', 'p99', 'p999', 'max'):
        if f'{pre}{k}_us' in w:
            m[f'lat_{k}{sfx}'] = w[f'{pre}{k}_us']
    m['cpu_pct' + sfx] = w['writer_cpu_pct'] + w['reader_cpu_pct']
    m['ctxsw_s' + sfx] = (sum(w['writer_ctxsw']) + sum(w['reader_ctxsw'])) / max(1, w['seconds'])
    m['lost_pct' + sfx] = 100.0 * w['gaps'] / max(1, w['expected'])
    return m


def hangar_metrics(case, rep):
    m = {}
    if not isinstance(rep, dict):
        return m
    for s in rep.get('publish', {}).get('sizes', []):
        m[f'publish_ns_{s["bytes"]}'] = s['ns']
    for s in rep.get('latest', {}).get('sizes', []):
        m[f'latest_ns_{s["bytes"]}'] = s['ns']
    for w in rep.get('wake', []):
        if case == 'hg_frame':
            m.update(hg_wake(w, '_frame'))
        else:
            # one hangar run stands beside both QoS profiles: hg_next over a
            # 16-slot ring is the queue, and no slower than hg_latest
            for q in QOS:
                m.update(hg_wake(w, f'_{w["rate_hz"]}_{q}'))
    if 'wake_nopin' in rep:
        m.update(hg_wake(rep['wake_nopin'], '_nopin'))
    if 'rss' in rep and rep['rss'].get('writer', {}).get('pss_kb', -1) >= 0:
        m['pss_kb'] = rep['rss']['writer']['pss_kb'] + rep['rss']['reader']['pss_kb']
        m['threads'] = 2
    if 'idle' in rep:
        i = rep['idle']
        m['idle_listener_ctxsw_s'] = (i['vol_delta'] + i['nonvol_delta']) / max(1, i['seconds'])
        m['idle_listener_cpu_pct'] = 100.0 * i['cpu_ticks_delta'] / os.sysconf('SC_CLK_TCK') / max(1, i['seconds'])
    return m


def summarize(doc):
    """summary[metric][config] = median, min, max, n, flagged; factors[metric][config]."""
    vals = {}
    for run in doc['runs']:
        fn = hangar_metrics if run['config'] == 'hangar' else ros_metrics
        for k, v in fn(run['case'], run['result']).items():
            vals.setdefault(k, {}).setdefault(run['config'], []).append((v, bool(run['flags'])))
    summary, factors = {}, {}
    for k, by in vals.items():
        summary[k] = {}
        for c, vs in by.items():
            xs = [v for v, _ in vs]
            summary[k][c] = {'median': statistics.median(xs), 'min': min(xs), 'max': max(xs), 'n': len(xs),
                             'flagged': sum(1 for _, f in vs if f)}
        h = summary[k].get('hangar')
        if h and h['median'] > 0 and not k.startswith('lost_pct'):
            factors[k] = {c: s['median'] / h['median'] for c, s in summary[k].items() if c != 'hangar'}
    doc['summary'], doc['factors'] = summary, factors


# ---- the table -----------------------------------------------------------------------

def num(v):
    if v == 0:
        return '0'
    a = abs(v)
    if a >= 100:
        return f'{v:,.0f}'.replace(',', ' ')
    if a >= 10:
        return f'{v:.1f}'
    if a >= 1:
        return f'{v:.2f}'
    return f'{v:.3f}'


def factor(f):
    return f'×{f:,.0f}'.replace(',', ' ') if f >= 100 else f'×{f:.1f}' if f >= 10 else f'×{f:.2f}'


def rows():
    """(heading) or (metric key, label)."""
    r = [('Publish, ns per message, nobody subscribed (hg_publish; publish())',)]
    names = {16: '16 B', 256: '256 B', 4096: '4 KB', 1048576: '1 MB'}
    r += [(f'publish_ns_{s}', names[s]) for s in SIZES]
    r.append(('Read the newest message, ns (hg_latest; take() of a message just published in the same process)',))
    r += [(f'latest_ns_{s}', names[s]) for s in SIZES]
    for rate in RATES:
        for q in QOS:
            what = 'best-effort keep-last 1' if q == 'latest' else 'reliable keep-last 16'
            hz = f'{rate} Hz' if rate < 1000 else f'{rate // 1000} kHz'
            r.append((f'256 B at {hz}, two processes on two CPUs; ROS 2 QoS: {what}',))
            sfx = f'_{rate}_{q}'
            r += [('lat_p50' + sfx, 'publish → message in the reader\'s hands, p50, µs'),
                  ('lat_p99' + sfx, 'p99, µs'), ('lat_p999' + sfx, 'p99.9, µs'), ('lat_max' + sfx, 'max, µs'),
                  ('cpu_pct' + sfx, 'CPU, both processes, % of one core'),
                  ('ctxsw_s' + sfx, 'context switches per second, both processes'),
                  ('lost_pct' + sfx, 'messages lost, %')]
    r.append(('256 B at 1 kHz, the pair left to the scheduler (not pinned); best-effort keep-last 1',))
    r += [('lat_p50_nopin', 'p50, µs'), ('lat_p99_nopin', 'p99, µs'), ('lat_max_nopin', 'max, µs'),
          ('cpu_pct_nopin', 'CPU, both processes, % of one core')]
    r.append(('1 MB at 30 Hz (a camera\'s frames), two processes on two CPUs; best-effort keep-last 1',))
    r += [('lat_p50_frame', 'p50, µs'), ('lat_p99_frame', 'p99, µs'), ('lat_max_frame', 'max, µs'),
          ('cpu_pct_frame', 'CPU, both processes, % of one core'), ('lost_pct_frame', 'messages lost, %')]
    r.append(('Idle: matched, nothing published',))
    r += [('idle_listener_ctxsw_s', 'subscriber: context switches per second'),
          ('idle_listener_cpu_pct', 'subscriber: CPU, % of one core'),
          ('idle_talker_ctxsw_s', 'publisher: context switches per second (hangar: none to count, it has no thread asleep)'),
          ('idle_talker_cpu_pct', 'publisher: CPU, % of one core')]
    r.append(('Memory and start, the 1 kHz pair',))
    r += [('pss_kb', 'PSS, both processes, kB'), ('threads', 'threads, both processes'),
          ('daemon_pss_kb', 'the middleware\'s daemon: PSS, kB'), ('daemon_cpu_pct', 'the middleware\'s daemon: CPU, %'),
          ('startup_ms', 'start of both processes → first message received, ms (hg_bench does not measure it)')]
    r.append(('NOT the comparison: both nodes composed in ONE process, rclcpp intra-process delivery, 256 B at 1 kHz',))
    r += [('composed_lat_p50', 'p50, µs'), ('composed_lat_p99', 'p99, µs'), ('composed_lat_max', 'max, µs'),
          ('composed_cpu_pct', 'CPU, the one process, %'), ('composed_ctxsw_s', 'context switches per second'),
          ('composed_pss_kb', 'PSS, the one process, kB')]
    return r


def report(doc):
    env, a = doc['environment'], doc['args']
    cfgs = [c for c in doc['configs'] if c != 'hangar']
    out = [f'# hangar against ROS 2: {doc["label"]}', '']
    if doc['smoke_test']:
        out += ['**A smoke test of the benchmark, not a measurement' + (': ' + doc['smoke_reason'] if doc.get('smoke_reason') else '')
                + '. These numbers do not go into docs/00.**', '']
    out += [f'{doc["date"]}, {env["model"]}, {env["arch"]}, Linux {env["kernel"]}, {env["cpus"]} CPUs, '
            f'governor {"/".join(env["governor"])}' + (', **inside a container**' if env['container'] else '') + '.',
            f'ROS 2 {env["ros_distro"]}; hangar lib/hg {env.get("hg_commit", "?")} ({env.get("hg_compiler", "?")}), '
            f'bench/ros2 at {env["commit"]}.',
            f'{a["reps"]} repetition(s); each rate run {a["seconds"]} s after {a["warmup"]} s of warm-up (ROS 2), idle {a["idle_seconds"]} s; '
            f'writer on CPU {env["writer_cpu"]}, reader on CPU {env["reader_cpu"]}, the same two for both systems.',
            f'Load average at the start {env.get("loadavg1", "?")}; running beside the benchmark: '
            + (', '.join(env.get('running_beside') or []) or 'nothing procbench.py knows') + '.',
            '',
            'A cell is the median over the repetitions, (min–max) when there are several, and **×N**: how many '
            'times hangar\'s figure the ROS 2 one is. † marks a figure with a run in it during which the machine '
            'was hot, throttled, frequency-capped or busy with something else (listed below).', '']
    out.append('| | hangar | ' + ' | '.join(f'`{c}`' for c in cfgs) + ' |')
    out.append('|---|---|' + '---|' * len(cfgs))
    S, F = doc['summary'], doc['factors']
    for row in rows():
        if len(row) == 1:
            out.append(f'| **{row[0]}** | |' + ' |' * len(cfgs))
            continue
        key, label = row
        if key not in S:
            continue
        cells = []
        for c in ['hangar'] + cfgs:
            s = S[key].get(c)
            if not s:
                cells.append('–')
                continue
            cell = f'{s["median"]:.0f}' if key == 'threads' else num(s['median'])
            if s['n'] > 1 and key != 'threads':
                cell += f' ({num(s["min"])}–{num(s["max"])})'
            if s['flagged']:
                cell += ' †'
            if c != 'hangar' and key in F and c in F[key]:
                cell += f' **{factor(F[key][c])}**'
            cells.append(cell)
        out.append(f'| {label} | ' + ' | '.join(cells) + ' |')
    out += ['', '## The columns', '', '| | |', '|---|---|',
            f'| hangar | `hg_bench` ({env.get("hg_bench", "?")}): shared-memory ring of 16 slots, one futex wake |']
    for c in cfgs:
        d = doc['configs'][c]
        loan = ''
        if 'loaned' in d:
            loan = '; loaned messages: ' + ('granted' if d['loaned'] else '**asked for and not granted by the middleware**')
        out.append(f'| `{c}` | {d["title"]}{loan} |')
    out += ['', 'The ROS 2 processes: `ROS_DOMAIN_ID=' + str(a['domain']) + '`, `ROS_AUTOMATIC_DISCOVERY_RANGE=' + a['discovery_range']
            + '`, a SingleThreadedExecutor, every thread of a process pinned to its one CPU.', '']
    if doc['skipped']:
        out += ['## Not run', ''] + [f'- `{s["config"]}`: {s["reason"]}' for s in doc['skipped']] + ['']
    bad = [r for r in doc['runs'] if r['flags'] or 'error' in r['result'] or any(
        isinstance(v, dict) and 'error' in v for v in r['result'].values())]
    out += ['## Thermal state', '',
            f'At the start: {state_line(doc["state_start"])}. At the end: {state_line(doc["state_end"])}.', '']
    if bad:
        out += ['Runs flagged or failed:', '', '| rep | column | run | flags | before → after | other CPUs busy |',
                '|---|---|---|---|---|---|']
        for r in bad:
            errs = [str(v['error']) for v in [r['result']] + list(r['result'].values()) if isinstance(v, dict) and 'error' in v]
            out.append(f'| {r["rep"]} | `{r["config"]}` | {r["case"]} | {", ".join(r["flags"] + ["FAILED: " + e for e in errs])} | '
                       f'{state_line(r["before"])} → {state_line(r["after"])} | {r.get("other_load_pct", "?")} % |')
    else:
        out.append('No run was flagged: never hot, throttled, frequency-capped or with other load beside it '
                   'between a run\'s start and its end.')
    return '\n'.join(out) + '\n'


def state_line(s):
    t = 'no sensor' if s['temp_c'] is None else f'{s["temp_c"]} °C'
    th = '' if s['throttled'] is None else f', get_throttled {s["throttled"]:#x}'
    return f'{t}, {"/".join(s["governor"])}{th}'


# ---- main ----------------------------------------------------------------------------

def find_daemons():
    d = {}
    if shutil.which('iox-roudi'):
        d['iox-roudi'] = shutil.which('iox-roudi')
    for p in os.environ.get('AMENT_PREFIX_PATH', '').split(':'):
        z = os.path.join(p, 'lib', 'rmw_zenoh_cpp', 'rmw_zenohd')
        if p and os.access(z, os.X_OK):
            d['rmw_zenohd'] = z
    return d


def installed(pkg):
    return any(p and os.path.isfile(os.path.join(p, 'share', pkg, 'package.xml'))
               for p in os.environ.get('AMENT_PREFIX_PATH', '').split(':'))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--report', metavar='JSON', help='print the Markdown table of an earlier results.json and exit')
    ap.add_argument('--prefix', help='where the ROS 2 package is installed (run.sh gives it)')
    ap.add_argument('--hg-bench', help='the hg_bench to measure hangar with; without it only ROS 2 runs')
    ap.add_argument('--out', help='directory for results.json, results.md and logs/')
    ap.add_argument('--label', default='', help='a line for the report\'s title (the machine, the occasion)')
    ap.add_argument('--reps', type=int, default=3)
    ap.add_argument('--seconds', type=int, default=20, help='length of every rate run (hg_bench\'s default)')
    ap.add_argument('--warmup', type=int, default=2, help='seconds of a ROS 2 rate run not measured')
    ap.add_argument('--idle-seconds', type=int, default=30)
    ap.add_argument('--cost-runs', type=int, default=5)
    ap.add_argument('--hg-count', type=int, default=1000000, help='publishes per run of hg_bench publish/latest')
    ap.add_argument('--ros-count', type=int, default=200000, help='publishes per run of the ROS 2 cost and take loops')
    ap.add_argument('--quick', action='store_true', help='1 repetition of 3 s runs: a smoke test, not numbers')
    ap.add_argument('--cpus', help='W,R: the writer\'s and the reader\'s CPU (default: the two highest allowed)')
    ap.add_argument('--configs', help='comma-separated subset of: hangar,' + ','.join(c['name'] for c in CONFIGS))
    ap.add_argument('--domain', type=int, default=77, help='ROS_DOMAIN_ID, away from a ROS 2 graph already on the machine')
    ap.add_argument('--discovery-range', default='LOCALHOST',
                    help='ROS_AUTOMATIC_DISCOVERY_RANGE: LOCALHOST keeps discovery off the network (SUBNET is ROS 2\'s default)')
    ap.add_argument('--max-temp', type=float, default=75.0, help='wait before a run while the machine is hotter (°C)')
    ap.add_argument('--cool-wait', type=float, default=180.0, help='... but no longer than this (s)')
    ap.add_argument('--hot', type=float, default=80.0, help='flag a run that began or ended this hot (°C)')
    ap.add_argument('--busy', type=float, default=25.0,
                    help='flag a run during which the CPUs beside the pair\'s were this busy (%%)')
    ap.add_argument('--commit', default='unknown')
    ap.add_argument('--smoke-reason', default='')
    args = ap.parse_args()

    if args.report:
        with open(args.report) as f:
            doc = json.load(f)
        summarize(doc)
        sys.stdout.write(report(doc))
        return 0
    if not args.prefix or not args.out:
        ap.error('--prefix and --out are needed (bench/ros2/run.sh gives both)')
    if args.quick:
        args.reps, args.seconds, args.warmup, args.idle_seconds = 1, 3, 1, 3
        args.cost_runs, args.hg_count, args.ros_count = 3, 200000, 20000
    os.makedirs(os.path.join(args.out, 'logs', 'ros'), exist_ok=True)

    allowed = os.sched_getaffinity(0)
    if args.cpus:
        writer, reader = sorted(int(c) for c in args.cpus.split(','))
        if not {writer, reader} <= allowed or writer == reader:
            ap.error(f'--cpus {args.cpus}: two different CPUs out of {sorted(allowed)}')
    elif len(allowed) >= 2:
        writer, reader = sorted(allowed)[-2:]  # hg_bench's own choice
    else:
        ap.error('two CPUs are needed, one for the writer and one for the reader')
    housekeeping = (allowed - {writer, reader}) or allowed
    os.sched_setaffinity(0, housekeeping)  # this script stays off the pair's CPUs
    ctx = {'allowed': allowed, 'writer': writer, 'reader': reader, 'housekeeping': housekeeping,
           'daemons': find_daemons(), 'hg_frame': False}

    want = args.configs.split(',') if args.configs else None
    skipped, todo = [], []
    for cfg in CONFIGS:
        if want and cfg['name'] not in want:
            continue
        missing = None
        if not installed(cfg['rmw']):
            missing = cfg['rmw']
        elif cfg.get('daemon') and cfg['daemon'] not in ctx['daemons']:
            missing = cfg['daemon']
        if missing:
            reason = f'{missing} is not installed; to add it: {INSTALL_HINT.get(missing, "install " + missing)}'
            skipped.append({'config': cfg['name'], 'reason': reason})
            say(f'SKIP {cfg["name"]}: {reason}')
        else:
            todo.append(cfg)
    with_hangar = bool(args.hg_bench) and (not want or 'hangar' in want)
    if not args.hg_bench:
        say('no hg_bench: the ROS 2 side alone, no factors')

    env = {
        'model': machine_name(), 'arch': os.uname().machine, 'kernel': os.uname().release,
        'cpus': os.cpu_count(), 'allowed_cpus': sorted(allowed), 'writer_cpu': writer, 'reader_cpu': reader,
        'container': os.path.exists('/.dockerenv') or os.path.exists('/run/.containerenv'),
        'ros_distro': ROS_DISTRO, 'commit': args.commit, 'hg_bench': args.hg_bench,
    }
    s0 = state([writer, reader])
    env['governor'] = s0['governor']
    # What procbench.py knows to look for on the vehicle's Pi: hangar, electric-eye, MAVROS.
    others = sorted({f'{comm} ({g})' for g, comm, _, _ in procbench.procs().values()})
    env['running_beside'] = others
    env['loadavg1'] = s0['loadavg1']
    if others:
        say('NOTE running beside the benchmark: ' + ', '.join(others)
            + '. Stop them first (README, "On the Pi") or the numbers carry their load')
    smoke_reason = args.smoke_reason or ('--quick' if args.quick else '')
    doc = {
        'bench_ros2': 1,
        'label': args.label or env['model'],
        'date': s0['time'],
        'smoke_test': bool(smoke_reason),
        'smoke_reason': smoke_reason,
        'args': {k: v for k, v in vars(args).items() if k not in ('report',)},
        'environment': env,
        'configs': {},
        'skipped': skipped,
        'state_start': s0,
        'runs': [],
    }
    if s0['throttled'] is not None and s0['throttled'] >> 16:
        say(f'NOTE get_throttled is {s0["throttled"]:#x}: this Pi has throttled since it booted; '
            'only throttling from now on can be told apart')
    if s0['temp_c'] is not None and s0['temp_c'] >= args.max_temp:
        say(f'NOTE {s0["temp_c"]} °C at the start: every run will wait for {args.max_temp} °C first')

    # Each ROS 2 configuration proves itself with a one-second pair before it gets a column.
    ready = []
    for cfg in todo:
        r = Ros(cfg, args, ctx)
        try:
            r.start_daemon()
            res = r.pair('--mode', 'rate', '--size', 256, '--rate', 100, '--qos', 'latest', '--seconds', 1,
                         '--warmup', 1, seconds=2)
            t, l = res.get('talker', {}), res.get('listener', {})
            if not ok(t) or not ok(l) or not l.get('samples'):
                raise RuntimeError('a one-second talker/listener pair exchanged nothing: '
                                   + str(t.get('error') or l.get('error') or 'no message arrived')
                                   + f' (logs/{cfg["name"]}.log)')
            entry = {'title': cfg['title'], 'rmw': cfg['rmw'], 'env': cfg['env']}
            if cfg.get('loan'):
                entry['loaned'] = bool(t.get('loaned'))
                entry['subscription_can_loan'] = bool(l.get('can_loan'))
                if not entry['loaned']:
                    say(f'NOTE {cfg["name"]}: the middleware does not grant loaned messages; the column is the '
                        'configuration without them')
            doc['configs'][cfg['name']] = entry
            ready.append(cfg)
            say(f'ok   {cfg["name"]}: {cfg["title"]}')
        except (RuntimeError, OSError) as e:
            skipped.append({'config': cfg['name'], 'reason': str(e)})
            say(f'SKIP {cfg["name"]}: {e}')
        finally:
            r.close()
    if with_hangar:
        probe = run_hg(args, ctx, {writer, reader}, ['wake', '--wake-bytes', '256', '--wake-rate', '100', '--seconds', '1'], 'probe')
        ctx['hg_frame'] = 'error' not in probe
        if not ctx['hg_frame']:
            probe = run_hg(args, ctx, {writer, reader}, ['publish', '--runs', '1', '--count', '1000'], 'probe')
            if 'error' in probe:
                say(f'hg_bench does not run here: {probe["error"]} (logs/hangar.log)')
                return 1
            say('NOTE this hg_bench is older than bench-ros2: no 1 MB frame run, no p99.9, and its latency is '
                'to the wake-up, not to the message copied out')
        env['hg_commit'] = probe['hg_bench']['hg_commit']
        env['hg_compiler'] = probe['hg_bench']['compiler']
        doc['configs'] = {'hangar': {'title': 'hg_bench'}, **doc['configs']}

    per_cfg = (len(SIZES) * 2 * 2 + (len(RATES) * len(QOS) + 3) * (args.seconds + args.warmup + 3) + args.idle_seconds + 12)
    per_hg = 5 * args.seconds + args.idle_seconds + 25 if with_hangar else 0
    total = args.reps * (len(ready) * per_cfg + per_hg)
    say(f'{args.reps} repetition(s) of {"hangar and " if with_hangar else ""}{len(ready)} ROS 2 configuration(s): '
        f'about {total // 60 + 1} min, more if the machine has to cool down between runs')

    def record(rep, config, case, fn):
        waited = cool_down(args)
        before = state([writer, reader])
        try:
            result = fn()
        except (OSError, RuntimeError) as e:
            result = {'error': str(e)}
        after = state([writer, reader])
        f = flags(before, after, args.hot, args.busy)
        doc['runs'].append({'rep': rep, 'config': config, 'case': case, 'cooled_s': waited, 'before': before,
                            'after': after, 'other_load_pct': other_load(before, after), 'flags': f,
                            'result': result})
        failed = 'error' in result or any(isinstance(v, dict) and 'error' in v for v in result.values())
        say(f'  rep {rep} {config:20s} {case:28s} {after["temp_c"]} °C'
            + (f'  (waited {waited:.0f} s for {args.max_temp} °C)' if waited >= 2 else '')
            + (f'  FLAGGED {",".join(f)}' if f else '') + ('  FAILED' if failed else ''))

    def save():
        doc['state_end'] = state([writer, reader])
        summarize(doc)
        with open(os.path.join(args.out, 'results.json'), 'w') as f:
            json.dump(doc, f, indent=1, default=sorted)
        with open(os.path.join(args.out, 'results.md'), 'w') as f:
            f.write(report(doc))

    try:
        for rep in range(args.reps):
            # hangar and the configurations take turns at going first, so that a
            # machine warming up over the session does not always favour one
            order = ([None] if with_hangar else []) + ready
            order = order[rep % len(order):] + order[:rep % len(order)] if order else []
            for cfg in order:
                if cfg is None:
                    for case, cpus, hg_args in hangar_cases(args, ctx):
                        record(rep, 'hangar', case, lambda: run_hg(args, ctx, cpus, hg_args, f'{case}-{rep}'))
                else:
                    r = Ros(cfg, args, ctx)
                    try:
                        r.start_daemon()
                        for case, fn in ros_cases(args):
                            record(rep, cfg['name'], case, lambda: fn(r))
                    except RuntimeError as e:
                        say(f'  rep {rep} {cfg["name"]}: {e}')
                    finally:
                        r.close()
                save()
    except KeyboardInterrupt:
        say('interrupted: writing what there is')
    finally:
        for p in list(LIVE):
            stop(p, signal.SIGKILL)
        save()
    sys.stdout.write('\n' + report(doc))
    say(f'results: {os.path.join(args.out, "results.json")} and results.md')
    return 0


if __name__ == '__main__':
    sys.exit(main())
