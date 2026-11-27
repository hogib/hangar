#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bench/pairs/driver.py: every system's node pair through the same cells, on equal terms
(bench/pairs/README.md is the protocol; read it before reading a number).

    driver.py --plan plans/full.json --out DIR --var bin=~/hangar-root/bin [--systems FILE]...
    driver.py --report DIR [DIR ...] [> report.md]       several sessions pooled

A run is one system in one cell: its helper processes, its subscribers, then its
publisher, each pinned as the plan says, for a duration sized from the rate. The
standard library alone; nothing is installed, nothing outside --out and /dev/shm is
written, no root is needed (a real-time condition is run only where the user may have
it, and reported as not run where not).

What makes the terms equal is in the plan and in the node pair's shared half
(pair.h), not here: this file starts processes, keeps the machine's state beside every
run, and counts."""
import argparse
import datetime
import glob
import hashlib
import json
import math
import os
import random
import re
import shutil
import signal
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
LIVE = []          # every process started and not yet reaped
CLK = os.sysconf('SC_CLK_TCK')
PAGE_KB = os.sysconf('SC_PAGE_SIZE') // 1024
# A percentile is printed for a run only when the run has the samples for it: at
# least 100 beyond the percentile (README "Statistics").
NEED = {'p50': 200, 'p99': 10_000, 'p999': 100_000}
Q = {'p50': 0.5, 'p99': 0.99, 'p999': 0.999}
REPEAT_ON = ('throttled', 'throttled-during')


def say(*a):
    print('pairs:', *a, flush=True)


def read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


# ---- the machine ---------------------------------------------------------------------

def temperature():
    v = read('/sys/class/thermal/thermal_zone0/temp')
    try:
        return round(int(v) / 1000.0, 1)
    except (TypeError, ValueError):
        return None


def throttled():
    """The Pi firmware's word (bits 0-3: now; 16-19: since boot), or None elsewhere."""
    if shutil.which('vcgencmd'):
        try:
            out = subprocess.run(['vcgencmd', 'get_throttled'], capture_output=True, text=True, timeout=5).stdout
            return int(out.strip().split('=')[1], 16)
        except (OSError, subprocess.SubprocessError, IndexError, ValueError):
            pass
    return None


def machine_state(cores):
    govs = sorted({g for g in (read(p) for p in glob.glob('/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor')) if g})
    freq = {}
    for c in cores:
        d = f'/sys/devices/system/cpu/cpu{c}/cpufreq/'
        v = [read(d + n) for n in ('scaling_cur_freq', 'scaling_max_freq', 'cpuinfo_max_freq')]
        if all(v):
            freq[str(c)] = [int(x) for x in v]
    busy = total = 0
    for line in (read('/proc/stat') or '').splitlines():
        f = line.split()
        if f and f[0][:3] == 'cpu' and f[0][3:].isdigit() and int(f[0][3:]) not in cores:
            v = [int(x) for x in f[1:9]]
            total += sum(v)
            busy += sum(v) - v[3] - v[4]
    return {'time': datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
            'temp_c': temperature(), 'throttled': throttled(), 'governor': govs or ['none'], 'freq_khz': freq,
            'loadavg1': float((read('/proc/loadavg') or '0').split()[0]), 'other_cpus_jiffies': [busy, total]}


def other_load(before, after):
    (b0, t0), (b1, t1) = before['other_cpus_jiffies'], after['other_cpus_jiffies']
    return round(100.0 * (b1 - b0) / (t1 - t0), 1) if t1 > t0 else 0.0


def run_flags(before, after, hot, busy, spinners):
    f = []
    if not spinners and other_load(before, after) >= busy:
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
    return f


def environment(args, plan, cores):
    model = read('/proc/device-tree/model')
    if not model:
        for line in (read('/proc/cpuinfo') or '').splitlines():
            if line.startswith('model name'):
                model = line.split(':', 1)[1].strip()
                break
    boot = read('/proc/sys/kernel/random/boot_id') or 'unknown'
    today = datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%d')
    pol = {0: 'SCHED_OTHER', 1: 'SCHED_FIFO', 2: 'SCHED_RR'}.get(os.sched_getscheduler(0), str(os.sched_getscheduler(0)))
    return {
        'session': f'{today}-{boot[:8]}', 'date': today, 'boot_id': boot, 'host': os.uname().nodename,
        'model': (model or os.uname().machine).replace('\x00', ''), 'arch': os.uname().machine, 'kernel': os.uname().release,
        'cmdline': read('/proc/cmdline'), 'cpus': os.cpu_count(), 'allowed_cpus': sorted(os.sched_getaffinity(0)),
        'cores': cores, 'python': sys.version.split()[0], 'driver_commit': args.commit,
        'priority_class': f'{pol}, nice {os.nice(0)}: every process of every system inherits it from the driver, '
                          'except under a condition that says realtime',
        'uptime_s': float((read('/proc/uptime') or '0').split()[0]),
    }


# ---- processes -----------------------------------------------------------------------

def spawn(cmd, env, cpus, out, err, rt=None):
    def pre():
        os.sched_setaffinity(0, cpus)
        if rt:
            os.sched_setscheduler(0, os.SCHED_FIFO if rt['policy'] == 'fifo' else os.SCHED_RR, os.sched_param(rt['priority']))
    p = subprocess.Popen(cmd, env=env, stdout=out, stderr=err, stdin=subprocess.DEVNULL, preexec_fn=pre)
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


def realtime_allowed(rt):
    """Whether this user may give a process the policy and priority: tried, in a child."""
    try:
        p = subprocess.Popen([sys.executable, '-c', 'pass'], preexec_fn=lambda: os.sched_setscheduler(
            0, os.SCHED_FIFO if rt['policy'] == 'fifo' else os.SCHED_RR, os.sched_param(rt['priority'])))
        return p.wait() == 0
    except (OSError, subprocess.SubprocessError):
        return False


def cpu_ticks(pid):
    st = read(f'/proc/{pid}/stat')
    if not st:
        return None
    f = st[st.rindex(')') + 2:].split()
    return int(f[11]) + int(f[12])


def memory_kb(pid):
    """(kB, 'pss') from smaps_rollup where this user may read it, else (kB, 'rss')
    from statm, which anyone may; (None, None) for a process that is gone."""
    roll = read(f'/proc/{pid}/smaps_rollup')
    if roll:
        m = re.search(r'^Pss:\s+(\d+)', roll, re.M)
        if m:
            return int(m.group(1)), 'pss'
    statm = read(f'/proc/{pid}/statm')
    if statm:
        return int(statm.split()[1]) * PAGE_KB, 'rss'
    return None, None


def last_json(path):
    for line in reversed((read(path) or '').splitlines()):
        if line.startswith('{'):
            try:
                return json.loads(line)
            except ValueError:
                pass
    return None


def wait_for_line(path, word, procs, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if word in (read(path) or ''):
            return True
        if any(p.poll() is not None for p in procs):
            return False
        time.sleep(0.02)
    return False


def fill(template, v):
    out = []
    for t in template:
        for k, val in v.items():
            t = t.replace('{' + k + '}', str(val))
        out.append(os.path.expanduser(t))
    return out


class Run:
    """One system in one cell, once: what is started, in which order, and what is read."""

    def __init__(self, ctx, system, cell, tag):
        self.ctx, self.sys, self.cell, self.tag = ctx, system, cell, tag
        self.cond = ctx['plan']['conditions'][cell['condition']]
        self.rt = self.cond.get('realtime')
        self.rundir = f'/dev/shm/pairs-{os.getpid()}-{tag}'
        self.vars = dict(ctx['vars'], rundir=self.rundir, topic=f'r{tag}'.replace('-', ''))
        self.env = dict(os.environ)
        for k, val in system.get('env', {}).items():
            self.env[k] = fill([val], self.vars)[0]
        self.logs = os.path.join(ctx['out'], 'logs')
        self.procs = {}     # name -> Popen, everything of the system
        self.extra = []     # spinners
        self.errors = []

    def path(self, name, ext):
        return os.path.join(self.logs, f'{self.tag}-{name}.{ext}')

    def start(self, name, cmd, cpus):
        out, err = open(self.path(name, 'out'), 'w'), open(self.path(name, 'err'), 'w')
        try:
            p = spawn(cmd, self.env, cpus, out, err, self.rt)
        finally:
            out.close()
            err.close()
        self.procs[name] = p
        return p

    def node(self, role, seconds, more=()):
        c = self.cell
        base = self.sys.get(role) or self.sys['node']
        cmd = fill(base, self.vars) + ['--role', role, '--size', str(c['size']), '--rate', str(c['rate']), '--seconds', str(seconds),
                                       '--warmup', str(self.ctx['plan']['warmup_s']), '--topic', self.vars['topic']] + list(more)
        args = fill(self.sys.get('args', []), self.vars)
        return cmd + (['--'] + args if args else [])

    def begin(self):
        os.makedirs(self.rundir, exist_ok=True)
        cores = self.ctx['cores']
        for i in range(self.cond.get('spinners', 0)):
            # The load is left to the scheduler over every allowed CPU, the pair's included.
            p = spawn([sys.executable, '-c', 'while True: pass'], os.environ, self.ctx['allowed'], subprocess.DEVNULL, subprocess.DEVNULL)
            self.extra.append(p)
        for h in self.sys.get('helpers', []):
            p = self.start('helper-' + h['name'], fill(h['cmd'], self.vars), set(cores['other']))
            time.sleep(h.get('ready_s', 1.0))
            if p.poll() is not None:
                raise RuntimeError(f'helper {h["name"]} exited at once (status {p.returncode}): logs/{self.tag}-helper-{h["name"]}.err')

    def end(self):
        for name, p in list(self.procs.items()):
            stop(p, signal.SIGINT if name.startswith('helper-') else signal.SIGKILL)
        for p in self.extra:
            stop(p, signal.SIGKILL)
        shutil.rmtree(self.rundir, ignore_errors=True)

    def sample(self):
        s = {}
        for name, p in self.procs.items():
            kb, kind = memory_kb(p.pid)
            s[name] = (cpu_ticks(p.pid), kb, kind, time.monotonic())
        return s

    def cost(self, a, b):
        """CPU of every process of the system between two samples, and its memory at
        the second."""
        cpu, mem, kind = {}, {}, {}
        for name in a:
            if name in b and a[name][0] is not None and b[name][0] is not None and b[name][3] > a[name][3]:
                cpu[name] = round(100.0 * (b[name][0] - a[name][0]) / CLK / (b[name][3] - a[name][3]), 3)
            if name in b and b[name][1] is not None:
                mem[name], kind[name] = b[name][1], b[name][2]
        t = [b[n][3] - a[n][3] for n in a if n in b]
        return {'interval_s': round(max(t), 2) if t else 0, 'cpu_pct': cpu, 'cpu_pct_total': round(sum(cpu.values()), 3),
                'mem_kb': mem, 'mem_kind': kind, 'mem_kb_total': sum(mem.values()),
                'mem_all_pss': bool(kind) and all(k == 'pss' for k in kind.values())}

    def latency(self, seconds):
        cores, warm = self.ctx['cores'], self.ctx['plan']['warmup_s']
        if self.sys.get('kind') == 'single':
            self.start('both', self.node('both', seconds), {cores['pub']})
            pub, subs = None, ['both']
        else:
            subs = []
            for k in range(self.cell['subscribers']):
                name = f'sub{k}'
                more = ['--id', str(k)] + (['--check-all'] if self.ctx['plan'].get('check_all') else [])
                p = self.start(name, self.node('sub', seconds, more), {cores['sub'][k % len(cores['sub'])]})
                subs.append(name)
                if not wait_for_line(self.path(name, 'err'), 'pair: ready', [p], 60):
                    raise RuntimeError(f'subscriber {k} did not come up: logs/{self.tag}-{name}.err')
            pub = self.start('pub', self.node('pub', seconds), {cores['pub']})
        t0 = time.monotonic()
        # The cost's interval: from half a second into the measured part to half a
        # second before its end, by this script's clock.
        lead = min(0.5, seconds / 4.0)
        time.sleep(warm + lead)
        a = self.sample()
        time.sleep(max(0.1, seconds - 2 * lead))
        b = self.sample()
        res = {'cost': self.cost(a, b)}
        limit = t0 + warm + seconds + 30
        for name in (['pub'] if pub else []) + subs:
            p = self.procs[name]
            try:
                p.wait(max(0.1, limit - time.monotonic()))
            except subprocess.TimeoutExpired:
                self.errors.append(f'{name} did not end: killed')
                stop(p, signal.SIGKILL)
        res['pub'] = last_json(self.path('pub', 'out')) if pub else None
        res['subs'] = [last_json(self.path(n, 'out')) or {'error': f'no result (exit status {self.procs[n].returncode})'} for n in subs]
        if pub and not res['pub']:
            self.errors.append(f'the publisher gave no result (exit status {pub.returncode})')
        return res

    def startup(self):
        """With the publisher running: a subscriber started, to its first message; then
        a subscriber killed and another started, to its first message."""
        cores = self.ctx['cores']
        if self.sys.get('kind') == 'single':
            raise RuntimeError('a system of one process has no subscriber to start')
        settle = self.ctx['plan'].get('startup_settle_s', 2.0)
        self.start('pub', self.node('pub', 600), {cores['pub']})
        time.sleep(settle)
        res = {}
        for what in ('startup', 'rejoin'):
            if what == 'rejoin':
                # One that has been receiving for a while, killed outright.
                v = self.start('victim', self.node('sub', 600), {cores['sub'][0]})
                wait_for_line(self.path('victim', 'err'), 'pair: ready', [v], 60)
                time.sleep(settle)
                v.kill()
                v.wait()
            name = 'sub-' + what
            t = time.monotonic_ns()   # CLOCK_MONOTONIC, the clock the node stamps with
            p = self.start(name, self.node('sub', 600, ['--first-only']), {cores['sub'][0]})
            try:
                p.wait(90)
            except subprocess.TimeoutExpired:
                stop(p, signal.SIGKILL)
            j = last_json(self.path(name, 'out')) or {}
            if j.get('first_msg_ns'):
                res[what + '_ms'] = round((j['first_msg_ns'] - t) / 1e6, 3)
                res[what + '_open_ms'] = round((j['open_ns'] - t) / 1e6, 3)
            else:
                self.errors.append(f'{what}: no first message in 90 s')
        return res


# ---- the plan ------------------------------------------------------------------------

def plan_cells(plan):
    cells, seen = [], set()
    for sw in plan['sweeps']:
        for size in sw.get('size', [256]):
            for rate in sw.get('rate', [1000]):
                for subs in sw.get('subscribers', [1]):
                    for cond in sw.get('condition', ['quiet']):
                        if cond not in plan['conditions']:
                            raise SystemExit(f'plan: sweep {sw["name"]} names a condition {cond} the plan does not define')
                        c = {'type': sw.get('cell', 'latency'), 'size': size, 'rate': rate, 'subscribers': subs, 'condition': cond}
                        k = cell_key(c)
                        if k not in seen:
                            seen.add(k)
                            cells.append(dict(c, sweeps=[sw['name']]))
                        else:
                            next(x for x in cells if cell_key(x) == k)['sweeps'].append(sw['name'])
    return cells


def cell_key(c):
    return f'{c["type"]}/{c["size"]}B/{c["rate"]}Hz/{c["subscribers"]}sub/{c["condition"]}'


def run_seconds(plan, rate):
    """A run's measured length, sized from the rate so that it has the samples for the
    highest percentile the plan wants, within the plan's bounds."""
    want = math.ceil(plan.get('target_samples', NEED['p999']) / rate)
    return int(max(plan['seconds'], min(want, plan.get('max_seconds', plan['seconds']))))


def system_fits(system, cell):
    """None, or why this system does not run this cell."""
    if cell['type'] not in system.get('cells', ['latency', 'startup']):
        return f'the system has no {cell["type"]} cell'
    if system.get('kind') == 'single' and (cell['subscribers'] != 1 or cell['type'] != 'latency'):
        return 'one process: one subscriber, and nothing to start or rejoin'
    if cell['size'] > system.get('max_size', 1 << 40):
        return 'messages above %d B are not carried (%s)' % (system['max_size'], system.get('max_size_why', "the system's limit"))
    if cell['subscribers'] > system.get('max_subscribers', 1 << 30):
        return f'more than {system["max_subscribers"]} subscribers are not supported'
    return None


# ---- numbers -------------------------------------------------------------------------

def bin_lower(i):
    if i < 32:
        return i
    e, s = 5 + (i - 32) // 32, (i - 32) % 32
    return (32 + s) << (e - 5)


def hist_merge(hs):
    out = {'n': 0, 'min_ns': None, 'max_ns': 0, 'sum_ns': 0.0, 'bins': {}}
    for h in hs:
        if not h or not h.get('n'):
            continue
        out['n'] += h['n']
        out['min_ns'] = h['min_ns'] if out['min_ns'] is None else min(out['min_ns'], h['min_ns'])
        out['max_ns'] = max(out['max_ns'], h['max_ns'])
        out['sum_ns'] += h['sum_ns']
        for i, c in (h['bins'].items() if isinstance(h['bins'], dict) else h['bins']):
            out['bins'][int(i)] = out['bins'].get(int(i), 0) + c
    return out


def hist_quantile(h, q):
    """The value below which a fraction q of the samples lie, in ns: the upper edge of
    the bin the q-th sample is in (never an underestimate), the exact max for the last."""
    if not h['n']:
        return None
    k, seen = math.ceil(q * h['n']), 0
    for i in sorted(h['bins']):
        seen += h['bins'][i]
        if seen >= k:
            return float(min(bin_lower(i + 1) if i >= 32 else i, h['max_ns']))
    return float(h['max_ns'])


def run_metrics(run):
    """One run's figures. A percentile is there only if the run has the samples."""
    m, r = {}, run['result']
    if run['cell']['type'] == 'startup':
        for k in ('startup_ms', 'rejoin_ms'):
            if k in r:
                m[k] = r[k]
        return m
    subs = [s for s in r.get('subs', []) if s and 'error' not in s]
    if not subs:
        return m
    for which in ('call', 'intended'):
        h = hist_merge(s.get(which) for s in subs)
        m[f'n_{which}'] = h['n']
        for p in ('p50', 'p99', 'p999'):
            if h['n'] >= NEED[p]:
                m[f'{p}_{which}_us'] = hist_quantile(h, Q[p]) / 1000.0
        if h['n']:
            m[f'max_{which}_us'] = h['max_ns'] / 1000.0
            m[f'mean_{which}_us'] = h['sum_ns'] / h['n'] / 1000.0
    exp = sum(s['expected'] for s in subs)
    m['lost_pct'] = 100.0 * sum(s['lost'] for s in subs) / max(1, exp)
    for k in ('duplicated', 'out_of_order', 'corrupt', 'discarded'):
        m[k] = sum(s.get(k, 0) for s in subs)
    pub = r.get('pub')
    if pub and 'error' not in pub:
        m['late_pct'] = 100.0 * pub['late'] / max(1, pub['send']['n'])
        if pub['send']['n'] >= NEED['p50']:
            m['send_p50_ns'] = hist_quantile(hist_merge([pub['send']]), 0.5)
    c = r.get('cost') or {}
    if c.get('cpu_pct'):
        m['cpu_pct'] = c['cpu_pct_total']
        m['mem_kb'] = c['mem_kb_total']
        helpers = [n for n in c['cpu_pct'] if n.startswith('helper-')]
        if helpers:
            m['helpers_cpu_pct'] = sum(c['cpu_pct'][n] for n in helpers)
    return m


def bootstrap_ci(xs, seed, reps=2000):
    """A 95 % interval for the median of xs by resampling the runs with replacement."""
    if len(xs) < 3:
        return None
    rnd = random.Random(seed)
    meds = sorted(statistics.median(rnd.choices(xs, k=len(xs))) for _ in range(reps))
    return meds[int(0.025 * reps)], meds[int(0.975 * reps) - 1]


def num(v):
    if v is None:
        return '–'
    a = abs(v)
    if v == 0:
        return '0'
    if a >= 100:
        return f'{v:,.0f}'.replace(',', ' ')
    if a >= 10:
        return f'{v:.1f}'
    if a >= 1:
        return f'{v:.2f}'
    return f'{v:.3f}'


METRICS = [
    ('p50_call_us', 'Latency, publish call to the reader\'s hands, median (µs)'),
    ('p99_call_us', 'The same, 99th percentile (µs)'),
    ('p999_call_us', 'The same, 99.9th percentile (µs)'),
    ('max_call_us', 'The same, the largest sample of a run (µs)'),
    ('p50_intended_us', 'Latency from the intended publish time, median (µs)'),
    ('p99_intended_us', 'The same, 99th percentile (µs)'),
    ('p999_intended_us', 'The same, 99.9th percentile (µs)'),
    ('lost_pct', 'Messages lost, % of those published in the measured part'),
    ('faults', 'Delivered twice / out of order / with a wrong fill / views dropped: messages, summed over the runs'),
    ('late_pct', 'Publishes more than a period late, %'),
    ('cpu_pct', 'CPU, every process of the system, % of one core'),
    ('mem_kb', 'Memory, every process of the system, kB'),
    ('startup_ms', 'A subscriber started, to its first message (ms)'),
    ('rejoin_ms', 'A subscriber killed and started again, to its first message (ms)'),
]


def pooled_percentile(runs, p, which):
    """A percentile over the pooled samples of several runs, where no run alone has
    enough: (value in us, runs pooled, samples) or None."""
    h = hist_merge(s.get(which) for r in runs for s in r['result'].get('subs', []) if s and 'error' not in s)
    if h['n'] >= NEED[p]:
        return hist_quantile(h, Q[p]) / 1000.0, len(runs), h['n']
    return None


def report(docs):
    """The Markdown report of one results folder or of several pooled."""
    first = docs[0]
    plan = first['plan']
    out = [f'# {plan.get("title", "bench/pairs")}', '']
    if any(d.get('smoke') for d in docs):
        out += ['**A smoke run of the harness, not a measurement: too short and too few repetitions for any figure below to be quoted.**', '']
    out += [f'{len(docs)} session(s), {sum(len([r for r in d["runs"] if not r.get("superseded")]) for d in docs)} runs kept. '
            'A cell is the median over the runs, with a 95 % bootstrap interval of that median in brackets where there are three runs '
            'or more. † marks a figure with a flagged run in it; ‡ a percentile no single run had the samples for, taken over the '
            'pooled samples of all runs; "n/a" one for which even those are too few. bench/pairs/README.md is the protocol.', '']
    out += ['## Sessions', '', '| session | machine | kernel | governor | plan | seed | runs | repeated | not clean | preflight |', '|---|---|---|---|---|---|---|---|---|---|']
    for d in docs:
        e = d['environment']
        kept = [r for r in d['runs'] if not r.get('superseded')]
        pf = sorted({rd['preflight']['verdict'] for rd in d['rounds']})
        out.append(f'| {e["session"]} | {e["model"]} | {e["kernel"]} | {"/".join(d["state_start"]["governor"])} | {d["plan_sha"][:8]} | {d["seed"]} | '
                   f'{len(kept)} | {sum(1 for r in d["runs"] if r.get("superseded"))} | {sum(1 for r in kept if r.get("never_clean"))} | {", ".join(pf)} |')
    out += ['', 'Pinned: ' + '; '.join(f'{d["environment"]["session"]}: publisher CPU {d["environment"]["cores"]["pub"]}, subscribers '
                                       f'{d["environment"]["cores"]["sub"]}, helpers {d["environment"]["cores"]["other"]}' for d in docs) + '.',
            'Priority class: ' + first['environment']['priority_class'] + '.', '']
    systems = []
    for d in docs:
        for n in d['systems']:
            if n not in systems:
                systems.append(n)
    out += ['## Systems', '', '| system | what | version | built with | not like for like |', '|---|---|---|---|---|']
    for n in systems:
        s = next(d['systems'][n] for d in docs if n in d['systems'])
        v = s.get('version') or {}
        out.append(f'| `{n}` | {s["title"]} | {v.get("version", "?")} | {v.get("compiler", "?")}; {v.get("flags", "?")} | {s.get("notes", "")} |')
    skipped = [(d['environment']['session'], s) for d in docs for s in d.get('skipped', [])]
    if skipped:
        out += ['', 'Not run: ' + '; '.join(f'`{s["system"]}` ({ses}): {s["reason"]}' for ses, s in skipped) + '.']
    out.append('')

    runs = [dict(r, session=d['environment']['session']) for d in docs for r in d['runs'] if not r.get('superseded')]
    cells = []
    for d in docs:
        for c in d['cells']:
            if cell_key(c) not in [cell_key(x) for x in cells]:
                cells.append(c)
    by = {}
    for r in runs:
        by.setdefault((cell_key(r['cell']), r['system']), []).append(r)
    notes = []
    for key, title in METRICS:
        rows = []
        for c in cells:
            line, any_value = [], False
            for n in systems:
                rs = by.get((cell_key(c), n), [])
                skipped_why = next((r['result'].get('not_run') for r in rs if r['result'].get('not_run')), None)
                vals = [(run_metrics(r).get(key), bool(r['flags'])) for r in rs]
                xs = [v for v, _ in vals if v is not None]
                if key == 'faults':
                    ms = [m for m in (run_metrics(r) for r in rs) if 'lost_pct' in m]
                    cell = '/'.join(str(sum(m[k] for m in ms)) for k in ('duplicated', 'out_of_order', 'corrupt', 'discarded')) if ms else \
                        ('not run' if skipped_why else '–')
                    any_value = any_value or bool(ms)
                elif xs:
                    any_value = True
                    cell = num(statistics.median(xs))
                    ci = bootstrap_ci(xs, first['seed'])
                    if ci:
                        cell += f' [{num(ci[0])}–{num(ci[1])}]'
                    cell += f' (n={len(xs)})' if len(xs) != len(rs) or len(xs) < 3 else ''
                    if any(f for v, f in vals if v is not None):
                        cell += ' †'
                elif key[:4] in ('p50_', 'p99_', 'p999') and rs and not skipped_why:
                    p, which = key.split('_')[0], key.split('_')[1]
                    pooled = pooled_percentile(rs, p, which)
                    if pooled:
                        any_value = True
                        cell = f'{num(pooled[0])} ‡'
                    else:
                        have = max((run_metrics(r).get(f'n_{which}', 0) for r in rs), default=0)
                        cell = 'n/a'
                        notes.append(f'{title}, {cell_key(c)}, `{n}`: a run has {have} samples and all {len(rs)} together '
                                     f'{sum(run_metrics(r).get(f"n_{which}", 0) for r in rs)}; {p} needs {NEED[p]}')
                else:
                    cell = 'not run' if skipped_why else '–'
                line.append(cell)
            if any_value:
                rows.append(f'| {c["size"]} B | {c["rate"]} Hz | {c["subscribers"]} | {c["condition"]} | ' + ' | '.join(line) + ' |')
        if rows and key == 'mem_kb':
            kinds = sorted({k for r in runs for k in (r['result'].get('cost') or {}).get('mem_kind', {}).values()})
            rss = sorted({f'{r["system"]}: {n}' for r in runs for n, k in (r['result'].get('cost') or {}).get('mem_kind', {}).items() if k == 'rss'})
            rows.append('')
            rows.append('PSS (/proc/<pid>/smaps_rollup) for every process.' if kinds == ['pss'] else
                        'PSS where this user may read it; RSS (/proc/<pid>/statm, which counts shared pages in full) for: ' + ', '.join(rss) + '.')
        if rows:
            out += [f'## {title}', '', '| size | rate | subscribers | condition | ' + ' | '.join(f'`{n}`' for n in systems) + ' |',
                    '|---|---|---|---|' + '---|' * len(systems)] + rows + ['']
    if notes:
        out += ['## Percentiles not printed', ''] + [f'- {n}' for n in sorted(set(notes))] + ['']

    # Spread within a session against spread between sessions, on the headline figure.
    sessions = sorted({r['session'] for r in runs})
    if len(sessions) > 1:
        out += ['## Within a session and between sessions (median latency, publish call to the reader\'s hands)', '',
                'Within: the half-range of a session\'s runs as a share of its median, the median of that over the sessions. '
                'Between: the half-range of the sessions\' medians as a share of the median over all runs.', '',
                '| size | rate | subscribers | condition | system | sessions | within | between | session medians (µs) |', '|---|---|---|---|---|---|---|---|---|']
        for c in cells:
            for n in systems:
                per = {}
                for r in by.get((cell_key(c), n), []):
                    v = run_metrics(r).get('p50_call_us')
                    if v is not None:
                        per.setdefault(r['session'], []).append(v)
                if len(per) < 2:
                    continue
                meds = {s: statistics.median(v) for s, v in per.items()}
                within = statistics.median((max(v) - min(v)) / 2 / meds[s] for s, v in per.items() if meds[s] > 0)
                allm = statistics.median([x for v in per.values() for x in v])
                between = (max(meds.values()) - min(meds.values())) / 2 / allm if allm > 0 else 0
                out.append(f'| {c["size"]} B | {c["rate"]} Hz | {c["subscribers"]} | {c["condition"]} | `{n}` | {len(per)} | ±{100 * within:.1f} % | '
                           f'±{100 * between:.1f} % | ' + ', '.join(num(meds[s]) for s in sorted(meds)) + ' |')
        out.append('')

    bad = [dict(r, session=d['environment']['session']) for d in docs for r in d['runs']
           if r.get('superseded') or r.get('never_clean') or r['flags'] or r['result'].get('errors') or r['result'].get('not_run')]
    out += ['## Runs repeated, flagged, failed or not run', '']
    if bad:
        out += ['| session | round | cell | system | attempt | what | before → after |', '|---|---|---|---|---|---|---|']
        for r in bad:
            what = list(r['flags'])
            if r.get('superseded'):
                what.append('REPEATED (this attempt is not in the tables)')
            if r.get('never_clean'):
                what.append('NEVER CLEAN (the last attempt, kept and marked)')
            what += ['FAILED: ' + e for e in r['result'].get('errors', [])]
            if r['result'].get('not_run'):
                what.append('NOT RUN: ' + r['result']['not_run'])
            out.append(f'| {r["session"]} | {r["round"]} | {cell_key(r["cell"])} | `{r["system"]}` | {r["attempt"]} | {"; ".join(what)} | '
                       f'{state_line(r.get("before"))} → {state_line(r.get("after"))} |')
    else:
        out.append('None: every run ran once, with no flag.')
    out.append('')
    return '\n'.join(out)


def state_line(s):
    if not s:
        return '–'
    t = 'no sensor' if s['temp_c'] is None else f'{s["temp_c"]} °C'
    th = '' if s['throttled'] is None else f', throttled {s["throttled"]:#x}'
    return f'{t}, {"/".join(s["governor"])}{th}'


# ---- the preflight -------------------------------------------------------------------

def preflight(args, out, rnd):
    """tools/rov/preflight.py's measurement check on this machine, before a round:
    (verdict, the report's file name)."""
    name = f'preflight-round-{rnd}.txt'
    if args.no_preflight:
        return 'not run (--no-preflight)', None
    pf = args.preflight or next((p for p in (os.path.join(HERE, '..', '..', 'tools', 'rov', 'preflight.py'),
                                             os.path.join(HERE, 'preflight.py')) if os.path.isfile(p)), None)
    if not pf:
        return 'not run (no preflight.py beside the driver: --preflight PATH)', None
    facts = os.path.join(out, 'logs', f'preflight-round-{rnd}.json')
    with open(facts, 'w') as f:
        subprocess.run([sys.executable, '-I', pf, 'probe', '--frozen-file', args.frozen_file, '--services', args.services,
                        '--root', args.install_root], stdout=f, stderr=subprocess.DEVNULL, timeout=60)
    r = subprocess.run([sys.executable, '-I', pf, 'judge', '--mode', 'measure', '--pi', facts], capture_output=True, text=True, timeout=60)
    with open(os.path.join(out, name), 'w') as f:
        f.write(r.stdout + r.stderr)
    last = (r.stdout.strip().splitlines() or ['no report'])[-1]
    return ('FAIL: ' if r.returncode else 'clean: ') + last, name


# ---- main ----------------------------------------------------------------------------

def load_systems(paths, variables):
    systems = []
    for p in paths:
        with open(p) as f:
            for s in json.load(f)['systems']:
                if s['name'] in [x['name'] for x in systems]:
                    raise SystemExit(f'{p}: a second system called {s["name"]}')
                s['file'] = os.path.abspath(p)
                systems.append(s)
    return systems


def system_ready(s, variables, out):
    """None and the system's record, or why it cannot run here. Nothing is installed."""
    v = dict(variables, rundir='/dev/shm', topic='t')
    need = [fill(s.get(r) or s['node'], v)[0] for r in ('pub', 'sub')] + [fill(h['cmd'], v)[0] for h in s.get('helpers', [])]
    for exe in need:
        if not (os.path.isfile(exe) and os.access(exe, os.X_OK)) and not shutil.which(exe):
            return f'{exe} is not there' + (f' ({s["install"]})' if s.get('install') else ''), None
    rec = {'title': s['title'], 'notes': s.get('notes', ''), 'entry': {k: s[k] for k in s if k != 'file'}, 'version': None, 'config': []}
    try:
        r = subprocess.run(fill(s.get('version') or (s['node'] + ['--version']), v), capture_output=True, text=True, timeout=20)
        line = (r.stdout.strip().splitlines() or [''])[-1]
        rec['version'] = json.loads(line) if line.startswith('{') else {'version': line or 'not said'}
    except (OSError, ValueError, subprocess.SubprocessError) as e:
        rec['version'] = {'version': f'not read ({e})'}
    for h in s.get('helpers', []):
        if h.get('version'):
            try:
                r = subprocess.run(fill(h['version'], v), capture_output=True, text=True, timeout=20)
                rec['version'][f'helper {h["name"]}'] = (r.stdout + r.stderr).strip().splitlines()[0]
            except (OSError, IndexError, subprocess.SubprocessError):
                pass
    for c in s.get('config_files', []):
        src = fill([c], dict(v, here=os.path.dirname(s['file'])))[0]
        dst = os.path.join(out, 'config', s['name'])
        os.makedirs(dst, exist_ok=True)
        if os.path.isfile(src):
            shutil.copy(src, dst)
            rec['config'].append(os.path.basename(src))
        else:
            return f'its configuration file {src} is not there', None
    return None, rec


def pick_cores(plan, allowed):
    """The plan's cores: numbers, or counted from the top (-1 the highest allowed)."""
    order = sorted(allowed)

    def one(c):
        return order[c] if c < 0 else c
    spec = plan.get('cores', {})
    pub = one(spec.get('pub', -1))
    sub = [one(c) for c in spec.get('sub', [-2])]
    if len(order) < 2:
        raise SystemExit('two CPUs are needed, one for the publisher and one for a subscriber')
    for c in [pub] + sub:
        if c not in allowed:
            raise SystemExit(f'plan: CPU {c} is not among the allowed {order}')
    other = sorted(allowed - {pub} - set(sub)) or order
    return {'pub': pub, 'sub': sub, 'other': other}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--report', nargs='+', metavar='DIR', help='print the report of one results folder, or of several pooled, and exit')
    ap.add_argument('--plan', help='the plan file (plans/full.json; plans/smoke.json proves the harness)')
    ap.add_argument('--systems', action='append', default=[], help='a registry file; repeatable (default: systems.json beside this file)')
    ap.add_argument('--only', help='comma-separated systems to run, of those in the registry')
    ap.add_argument('--var', action='append', default=[], metavar='NAME=VALUE', help='a value for {NAME} in the registry\'s commands; repeatable')
    ap.add_argument('--out', help='the results folder: results.json, report.md, logs/, config/')
    ap.add_argument('--reps', type=int, help='repetitions (rounds), in place of the plan\'s')
    ap.add_argument('--seed', type=int, help='the seed of the rounds\' orders (default: drawn, and recorded)')
    ap.add_argument('--max-temp', type=float, help='no run starts at or above this (°C), in place of the plan\'s')
    ap.add_argument('--no-preflight', action='store_true', help='do not run the measurement preflight before each round')
    ap.add_argument('--ignore-preflight', action='store_true', help='go on after a preflight with a FAIL (the verdict stays in the results)')
    ap.add_argument('--preflight', help='tools/rov/preflight.py, if it is not where the checkout has it')
    ap.add_argument('--frozen-file', default='/run/hangar-frozen')
    ap.add_argument('--services', default='eeye hellion')
    ap.add_argument('--install-root', default='hangar-root')
    ap.add_argument('--commit', default='unknown')
    args = ap.parse_args()

    if args.report:
        docs = []
        for d in args.report:
            with open(os.path.join(d, 'results.json') if os.path.isdir(d) else d) as f:
                docs.append(json.load(f))
        sys.stdout.write(report(docs))
        return 0
    if not args.plan or not args.out:
        ap.error('--plan and --out are needed')
    with open(args.plan) as f:
        plan_text = f.read()
    plan = json.loads(plan_text)
    plan.setdefault('conditions', {'quiet': {}})
    plan.setdefault('warmup_s', 2)
    reps = args.reps or plan.get('repetitions', 10)
    max_temp = args.max_temp if args.max_temp is not None else plan.get('max_temp_c', 60.0)
    seed = args.seed if args.seed is not None else random.SystemRandom().randrange(1 << 31)
    out = os.path.abspath(args.out)
    os.makedirs(os.path.join(out, 'logs'), exist_ok=True)
    with open(os.path.join(out, 'plan.json'), 'w') as f:
        f.write(plan_text)
    variables = dict(v.split('=', 1) for v in args.var)
    variables.setdefault('here', HERE)

    allowed = os.sched_getaffinity(0)
    cores = pick_cores(plan, allowed)
    os.sched_setaffinity(0, set(cores['other']))   # this script stays off the pair's CPUs
    cells = plan_cells(plan)

    reg = load_systems(args.systems or [os.path.join(HERE, 'systems.json')], variables)
    only = args.only.split(',') if args.only else plan.get('systems')
    systems, records, skipped = [], {}, []
    for s in reg:
        if only and s['name'] not in only:
            continue
        why, rec = system_ready(s, variables, out)
        if why:
            skipped.append({'system': s['name'], 'reason': why})
            say(f'SKIP {s["name"]}: {why}')
        else:
            systems.append(s)
            records[s['name']] = rec
            say(f'ok   {s["name"]}: {s["title"]} ({rec["version"].get("version")})')
    if not systems:
        say('no system can run here')
        return 1
    shutil.copy(args.systems[0] if args.systems else os.path.join(HERE, 'systems.json'), os.path.join(out, 'systems.json'))

    rt_ok = {}
    for name, cond in plan['conditions'].items():
        if cond.get('realtime'):
            rt_ok[name] = realtime_allowed(cond['realtime'])
            if not rt_ok[name]:
                say(f'NOTE condition {name}: this user may not have {cond["realtime"]}: its cells are recorded as not run '
                    '(docs/13, "Real-time scheduling without root")')

    env = environment(args, plan, cores)
    s0 = machine_state([cores['pub']] + cores['sub'])
    doc = {'bench_pairs': 1, 'smoke': bool(plan.get('smoke')), 'environment': env, 'plan': plan,
           'plan_sha': hashlib.sha256(plan_text.encode()).hexdigest(), 'seed': seed, 'repetitions': reps, 'max_temp_c': max_temp,
           'systems': records, 'skipped': skipped, 'cells': cells, 'state_start': s0, 'rounds': [], 'runs': []}
    ctx = {'plan': plan, 'cores': cores, 'allowed': allowed, 'out': out, 'vars': variables}

    per_round = sum(run_seconds(plan, c['rate']) + plan['warmup_s'] + 4 if c['type'] == 'latency' else 12 for c in cells) * len(systems)
    say(f'session {env["session"]}: {len(cells)} cells x {len(systems)} systems x {reps} rounds, about {reps * per_round // 60 + 1} min '
        f'if nothing has to cool down or be repeated; seed {seed}')

    def save():
        doc['state_end'] = machine_state([cores['pub']] + cores['sub'])
        with open(os.path.join(out, 'results.json'), 'w') as f:
            json.dump(doc, f)
        with open(os.path.join(out, 'report.md'), 'w') as f:
            f.write(report([doc]))

    def gate():
        """Waits for the machine to be below max_temp, up to the plan's bound: the
        seconds waited, and whether it got there."""
        t0, bound = time.monotonic(), plan.get('cool_wait_s', 600)
        while True:
            t = temperature()
            if t is None or t < max_temp:
                return round(time.monotonic() - t0, 1), True
            if time.monotonic() - t0 >= bound:
                return round(time.monotonic() - t0, 1), False
            time.sleep(5)

    def attempt(rnd, cell, system, n, tag):
        rec = {'round': rnd, 'cell': {k: cell[k] for k in ('type', 'size', 'rate', 'subscribers', 'condition')}, 'system': system['name'],
               'attempt': n, 'flags': [], 'result': {}}
        why = system_fits(system, cell)
        if not why and cell['condition'] in rt_ok and not rt_ok[cell['condition']]:
            why = 'real time is not permitted to this user here'
        if why:
            rec['result'] = {'not_run': why}
            return rec
        waited, cool = gate()
        rec['cooled_s'] = waited
        if not cool:
            rec['result'] = {'errors': [f'not started: still {temperature()} °C after waiting {waited:.0f} s for {max_temp} °C']}
            rec['flags'] = ['too-hot-to-start']
            return rec
        seconds = run_seconds(plan, cell['rate'])
        rec['seconds'] = seconds
        run = Run(ctx, system, cell, tag)
        before = machine_state([cores['pub']] + cores['sub'])
        try:
            run.begin()
            res = run.latency(seconds) if cell['type'] == 'latency' else run.startup()
        except (OSError, RuntimeError) as e:
            res = {}
            run.errors.append(str(e))
        finally:
            run.end()
        after = machine_state([cores['pub']] + cores['sub'])
        for s in res.get('subs', []) if isinstance(res.get('subs'), list) else []:
            if s.get('error'):
                run.errors.append('a subscriber: ' + s['error'])
            elif s.get('timed_out'):
                run.errors.append('a subscriber waited out the run without its last message')
        if run.errors:
            res['errors'] = run.errors
        rec.update(before=before, after=after, other_load_pct=other_load(before, after), result=res,
                   flags=run_flags(before, after, plan.get('hot_c', 80.0), plan.get('busy_pct', 25.0), run.cond.get('spinners', 0)))
        return rec

    stopped = None
    try:
        for rnd in range(reps):
            verdict, fname = preflight(args, out, rnd)
            # The order of this round: the cells shuffled, and within a cell the systems.
            rng = random.Random(seed * 1000003 + rnd)
            order_cells = list(cells)
            rng.shuffle(order_cells)
            order = []
            for c in order_cells:
                ss = list(systems)
                rng.shuffle(ss)
                order.append((c, ss))
            doc['rounds'].append({'round': rnd, 'preflight': {'verdict': verdict, 'file': fname},
                                  'order': [[cell_key(c), [s['name'] for s in ss]] for c, ss in order]})
            say(f'round {rnd}: preflight {verdict}')
            if verdict.startswith('FAIL') and not args.ignore_preflight:
                stopped = f'round {rnd} was not run: the preflight has a FAIL ({fname}); --ignore-preflight goes on all the same'
                say(stopped)
                break
            for ci, (cell, ss) in enumerate(order):
                for system in ss:
                    for n in range(plan.get('retries', 2) + 1):
                        rec = attempt(rnd, cell, system, n, f'{rnd}-{ci}-{system["name"]}-{n}')
                        doc['runs'].append(rec)
                        again = any(f in REPEAT_ON or f == 'too-hot-to-start' for f in rec['flags'])
                        res = rec['result']
                        say(f'  round {rnd} {cell_key(cell):40s} {system["name"]:16s}'
                            + (f' not run: {res["not_run"]}' if res.get('not_run') else f' {(rec.get("after") or {}).get("temp_c")} °C')
                            + (f'  waited {rec["cooled_s"]:.0f} s to cool' if rec.get('cooled_s', 0) >= 5 else '')
                            + (f'  FLAGGED {",".join(rec["flags"])}' if rec['flags'] else '')
                            + (f'  FAILED: {"; ".join(res["errors"])}' if res.get('errors') else ''))
                        if not again:
                            break
                        if n < plan.get('retries', 2):
                            rec['superseded'] = True
                            say('    repeated: the machine throttled or was too hot')
                        else:
                            rec['never_clean'] = True
                            say('    NEVER CLEAN in every attempt: kept and marked')
                save()
    except KeyboardInterrupt:
        stopped = 'interrupted: what there is was written'
        say(stopped)
    finally:
        for p in list(LIVE):
            stop(p, signal.SIGKILL)
        for d in glob.glob(f'/dev/shm/pairs-{os.getpid()}-*'):
            shutil.rmtree(d, ignore_errors=True)
        doc['stopped'] = stopped
        save()
    say(f'results: {out}/results.json and report.md')
    return 1 if stopped else 0


if __name__ == '__main__':
    sys.exit(main())
