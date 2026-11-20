#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bench/procbench.py: what hangar, electric-eye and the MAVLink relay cost on a
running vehicle, measured from outside: CPU per process (percent of one core,
from /proc/<pid>/stat) and memory (PSS, from /proc/<pid>/smaps_rollup), sampled
once a second, with the machine's total load and its temperature.

    sudo python3 -I bench/procbench.py [seconds]      # default 30

It reads /proc and /sys only and touches nothing it measures. Root is needed
for the PSS of processes run as other users (electric-eye's eeye user); without
it their memory reads as "-". First used on the lab's BlueROV2 on 2026-11-19,
where it showed safety, mux and the recorder spinning at a full core each
(tools/rov/README.md, "Results, 2026-11-19").
"""
import os
import sys
import time

GROUPS = [
    ('hangar', ('hangard', 'hangar-safety', 'hangar-mux', 'hangar-mavlink', 'hangar-encoder', 'dawn-patrold', 'recorder', 'pairer')),
    ('electric-eye', ('eeye', 'hellion', 'ffmpeg', 'rpicam-vid')),
    ('MAVROS relay', ('mavros_node',)),
    # bench/ros2's pair and what a middleware runs beside it (iceoryx's RouDi, Zenoh's router)
    ('ROS 2 benchmark', ('hgb_talker', 'hgb_listener', 'hgb_composed', 'iox-roudi', 'rmw_zenohd')),
]


def group_of(comm, cmd):
    for g, names in GROUPS:
        if comm in names or (g == 'MAVROS relay' and 'mavros_node' in cmd):
            return g
    return None


def procs():
    """pid -> (group, comm, cpu ticks, command line), for the processes measured."""
    out = {}
    for p in os.listdir('/proc'):
        if not p.isdigit():
            continue
        try:
            st = open(f'/proc/{p}/stat').read()
            comm = st[st.index('(') + 1:st.rindex(')')]
            f = st[st.rindex(')') + 2:].split()
            ticks = int(f[11]) + int(f[12])  # utime + stime
            cmd = open(f'/proc/{p}/cmdline', 'rb').read().replace(b'\0', b' ').decode(errors='replace')
        except OSError:
            continue
        g = group_of(comm, cmd)
        if g:
            out[int(p)] = (g, comm, ticks, cmd)
    return out


def pss_kb(pid):
    try:
        for line in open(f'/proc/{pid}/smaps_rollup'):
            if line.startswith('Pss:'):
                return int(line.split()[1])
    except OSError:
        pass
    return None


def hangar_root(pid):
    """The HANGAR_ROOT a hangar process was started with ('' if unreadable: a
    module marks itself undumpable, and only root reads its environment)."""
    try:
        env = open(f'/proc/{pid}/environ', 'rb').read().split(b'\0')
        return next((e[12:].decode() for e in env if e.startswith(b'HANGAR_ROOT=')), '')
    except OSError:
        return ''


def label(pid, comm, cmd):
    if comm in ('hangard', 'dawn-patrold'):
        r = hangar_root(pid)
        return f'{comm} ({"vehicle" if "vehicle" in r else "topside" if "topside" in r else "?"})'
    if comm == 'hangar-mavlink':
        return 'hangar-mavlink (topside)' if '--topside' in cmd else 'hangar-mavlink (vehicle)'
    return comm


def temperature():
    try:
        return int(open('/sys/class/thermal/thermal_zone0/temp').read()) / 1000
    except OSError:
        return float('nan')


def cpu_total():
    v = list(map(int, open('/proc/stat').readline().split()[1:]))
    return sum(v), v[3] + v[4]  # all, idle + iowait


def model():
    try:
        return open('/proc/device-tree/model').read().strip('\0 \n')
    except OSError:
        return os.uname().machine


def main():
    secs = int(sys.argv[1]) if len(sys.argv) > 1 else 30
    hz = os.sysconf('SC_CLK_TCK')
    before, c0, t0 = procs(), cpu_total(), time.monotonic()
    temps, pss = [], {}
    for _ in range(secs):
        time.sleep(1)
        temps.append(temperature())
        for pid in procs():
            k = pss_kb(pid)
            if k is not None:
                pss.setdefault(pid, []).append(k)
    after, c1, dt = procs(), cpu_total(), time.monotonic() - t0
    busy = 100.0 * (1 - (c1[1] - c0[1]) / max(1, c1[0] - c0[0]))
    print(f'{secs} s on {model()}, {os.cpu_count()} cores, the whole machine {busy:.1f} % busy '
          f'(of all cores), {min(temps):.1f}-{max(temps):.1f} C')
    rows = []
    for pid, (g, comm, ticks, cmd) in after.items():
        if pid not in before:
            continue  # started during the run: no whole window to measure
        cpu = 100.0 * (ticks - before[pid][2]) / hz / dt
        m = pss.get(pid)
        rows.append((g, label(pid, comm, cmd), cpu, sum(m) / len(m) / 1024 if m else None, pid))
    for g, _ in GROUPS:
        rs = sorted((r for r in rows if r[0] == g), key=lambda r: -r[2])
        if not rs:
            continue
        print(f'\n{g}')
        for _, name, cpu, mb, pid in rs:
            mem = f'{mb:7.2f} MB' if mb is not None else '      - MB'
            print(f'  {name:28s} {cpu:6.2f} % CPU  {mem} PSS   (pid {pid})')
        known = [r[3] for r in rs if r[3] is not None]
        print(f'  {"total":28s} {sum(r[2] for r in rs):6.2f} % CPU  {sum(known):7.2f} MB PSS   ({len(rs)} processes)')


if __name__ == '__main__':
    main()
