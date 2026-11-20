#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bench/ros2/linkbench.py: what the processes doing the autopilot-link job cost,
measured from outside while they run: CPU (percent of one core), PSS, context
switches per second and threads, with the machine's thermal state before and
after. The second-level comparison of bench/ros2/README.md: MAVROS against
hangar's mavlink module, each on the same serial device, one after the other.

    python3 -I bench/ros2/linkbench.py --seconds 120 --out link-mavros.json mavros_node
    python3 -I bench/ros2/linkbench.py --seconds 120 --out link-hangar.json hangar-mavlink
    python3 -I bench/ros2/linkbench.py --compare link-hangar.json link-mavros.json

It starts nothing, stops nothing and talks to nothing: it reads /proc and /sys.
A NAME matches a process by its comm or anywhere in its command line. The PSS of
another user's process, or of a hangar module (not dumpable), needs root; without
it the memory reads as null and the rest stands.
"""
import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True  # nothing is written into the checkout
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import matrix  # noqa: E402  (state, flags: the governor, the temperature, throttling)
import procbench  # noqa: E402  (pss_kb)


def find(names):
    out = {}
    for p in os.listdir('/proc'):
        if not p.isdigit() or int(p) == os.getpid():
            continue
        try:
            st = open(f'/proc/{p}/stat').read()
            comm = st[st.index('(') + 1:st.rindex(')')]
            cmd = open(f'/proc/{p}/cmdline', 'rb').read().replace(b'\0', b' ').decode(errors='replace')
        except OSError:
            continue
        if 'linkbench' in cmd:
            continue  # this script's own shell, sudo and the like name the processes too
        if any(n == comm or n in cmd for n in names):
            out[int(p)] = comm
    return out


def sample(pid):
    """(CPU ticks, context switches of every thread, threads) or None if it is gone."""
    ticks = matrix.cpu_ticks(pid)
    if ticks is None:
        return None
    sw = n = 0
    try:
        for t in os.listdir(f'/proc/{pid}/task'):
            for line in open(f'/proc/{pid}/task/{t}/status'):
                if 'ctxt_switches' in line:
                    sw += int(line.split()[1])
            n += 1
    except OSError:
        pass
    return ticks, sw, n


def compare(a, b):
    ta, tb = a['total'], b['total']
    print(f'{"":26s} {"/".join(a["names"]):>18s} {"/".join(b["names"]):>18s}   factor')
    for key, label in (('cpu_pct', 'CPU, % of one core'), ('pss_kb', 'PSS, kB'), ('ctxsw_s', 'context switches / s'),
                       ('threads', 'threads')):
        x, y = ta.get(key), tb.get(key)
        f = f'×{y / x:.1f}' if x and y is not None else '-'
        print(f'{label:26s} {x if x is not None else "-":>18} {y if y is not None else "-":>18}   {f}')
    for d in (a, b):
        if d['flags']:
            print(f'NOTE {"/".join(d["names"])}: flagged {", ".join(d["flags"])}: measure it again on a cool machine')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--seconds', type=int, default=120)
    ap.add_argument('--out', help='write the result here as JSON')
    ap.add_argument('--hot', type=float, default=80.0)
    ap.add_argument('--compare', nargs=2, metavar='JSON', help='print two earlier results side by side, with the factor')
    ap.add_argument('names', nargs='*', help='process names (comm) or command-line fragments')
    args = ap.parse_args()
    if args.compare:
        compare(*(json.load(open(p)) for p in args.compare))
        return 0
    if not args.names:
        ap.error('which processes? (mavros_node, hangar-mavlink, ...)')
    pids = find(args.names)
    if not pids:
        print(f'linkbench: no process matches {" ".join(args.names)}', file=sys.stderr)
        return 1
    cpus = sorted(os.sched_getaffinity(0))
    before, t0 = matrix.state(cpus), time.monotonic()
    s0 = {p: sample(p) for p in pids}
    pss = {}
    for _ in range(args.seconds):
        time.sleep(1)
        for p in pids:
            k = procbench.pss_kb(p)
            if k is not None:
                pss.setdefault(p, []).append(k)
    s1, dt = {p: sample(p) for p in pids}, time.monotonic() - t0
    after = matrix.state(cpus)
    hz = os.sysconf('SC_CLK_TCK')
    procs = []
    for p, comm in pids.items():
        if s0[p] is None or s1[p] is None:
            print(f'linkbench: {comm} (pid {p}) ended during the run: not counted', file=sys.stderr)
            continue
        m = pss.get(p)
        procs.append({'pid': p, 'comm': comm, 'cpu_pct': round(100.0 * (s1[p][0] - s0[p][0]) / hz / dt, 3),
                      'pss_kb': round(sum(m) / len(m)) if m else None,
                      'ctxsw_s': round((s1[p][1] - s0[p][1]) / dt, 1), 'threads': s1[p][2]})
    known = [x['pss_kb'] for x in procs if x['pss_kb'] is not None]
    doc = {'linkbench': 1, 'names': args.names, 'seconds': round(dt, 1), 'machine': procbench.model(),
           'before': before, 'after': after, 'flags': matrix.flags(before, after, args.hot), 'processes': procs,
           'total': {'cpu_pct': round(sum(x['cpu_pct'] for x in procs), 3),
                     'pss_kb': sum(known) if len(known) == len(procs) and procs else None,
                     'ctxsw_s': round(sum(x['ctxsw_s'] for x in procs), 1),
                     'threads': sum(x['threads'] for x in procs)}}
    for x in procs:
        mem = f'{x["pss_kb"]:8d} kB' if x['pss_kb'] is not None else '       - kB'
        print(f'{x["comm"]:18s} {x["cpu_pct"]:7.2f} % CPU  {mem} PSS  {x["ctxsw_s"]:9.1f} ctxsw/s  {x["threads"]:3d} threads  (pid {x["pid"]})')
    print(f'{dt:.0f} s, {matrix.state_line(before)} -> {matrix.state_line(after)}'
          + (f'  FLAGGED {", ".join(doc["flags"])}' if doc['flags'] else ''))
    if args.out:
        with open(args.out, 'w') as f:
            json.dump(doc, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
