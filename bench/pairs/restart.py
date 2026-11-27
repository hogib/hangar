#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bench/pairs/restart.py: a process killed, and how long until it runs again and
until its messages flow again, under hangard and under what a conventional stack
restarts a process with (bench/pairs/README.md, "Restarting a process").

  restart.py --bin DIR --out DIR [--kills N] [--spacing S] [--rate HZ] [--only a,b]
             [--hangard PATH] [--msg DIR] [--smoke]
  restart.py --report DIR...

One program, restart_node (in DIR), as a publisher and as a subscriber of one lib/hg
topic, under each supervisor in turn. The subscriber is killed with SIGKILL N times,
then the publisher N times, SPACING seconds apart, and two times are taken from each
kill, on the clock the node logs with:

  running   from the kill to the first statement of the new process's main
  messages  from the kill to the first message in a subscriber's hands that involves
            the new process (its own first, or the first from the new publisher)

The supervisors:

  hangard                hangar's manager, a manifest's defaults: Backoff=0.1s..10s
  hangard-fast           the same with Backoff=1ms..10s
  systemd                systemd --user, a transient unit, Restart=always and the
                         defaults for the rest (RestartSec=100ms)
  systemd-fast           the same with RestartSec=0 and no start limit
  ros2-launch            ros2 launch, ExecuteProcess(respawn=True), respawn_delay 0,
                         which is its default and its fastest (needs ROS 2 sourced)

THESE RESTART A PROCESS. That is all this script compares. None of them, hangard
included, is asked here for what hangar's safety module does when a module dies under
an armed vehicle (thrusters to neutral, ArduSub disarmed): tools/rov/failtest.py
measures that, on the vehicle, and systemd and ros2 launch have nothing that
corresponds to it.

SPACING matters: every supervisor here slows down or gives up on a process that keeps
dying (hangard doubles its delay and gives up after 5 failures in 60 s unless the
module was healthy for ResetAfter=30s in between; systemd stops after 5 starts in
10 s). The default spacing of 35 s puts each kill in the state of a first failure for
all of them. --smoke (2 kills, 3 s apart, ResetAfter=1s for hangard, no start limit
for systemd) proves the script and measures nothing.

Standard library only. Nothing is installed; a supervisor that is not there is named
and skipped. systemd --user needs a user manager: there is one while the user has a
session (an SSH login is one). Without a session it needs lingering
(`loginctl enable-linger`), which on Ubuntu asks for authentication.
"""
import argparse
import json
import os
import shutil
import signal
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOPIC = 'restart.depth'


def say(*a):
    print('restart:', *a, flush=True)


def read_log(path):
    starts, msgs = [], []
    try:
        with open(path) as f:
            for line in f:
                p = line.split()
                if len(p) == 4 and p[0] == 'S':
                    starts.append((p[1], int(p[2]), int(p[3])))
                elif len(p) == 4 and p[0] == 'M':
                    msgs.append((int(p[1]), int(p[2]), int(p[3])))
    except (OSError, ValueError):
        pass
    return starts, msgs


def wait_for(cond, seconds, step=0.02):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        v = cond()
        if v:
            return v
        time.sleep(step)
    return None


class Supervisor:
    name = title = ''
    what = ''

    def __init__(self, args, work):
        self.args, self.work = args, work
        self.log = os.path.join(work, 'node.log')
        self.topics = os.path.join(work, 'topics')
        self.node = os.path.join(args.bin, 'restart_node')

    def cmd(self, role):
        return [self.node, '--role', role, '--topic', TOPIC, '--rate', str(self.args.rate), '--log', self.log]

    def available(self):
        return None

    def start(self):
        raise NotImplementedError

    def stop(self):
        raise NotImplementedError

    def settings(self):
        return {}


class Hangard(Supervisor):
    name, title = 'hangard', "hangard, a manifest's defaults"
    backoff = None
    what = ("hangar's manager: it restarts the module after its back-off (0.1 s the first time, doubling with each failure "
            "up to 10 s, starting over once the module has been healthy for ResetAfter=30s) and gives up after 5 failures in "
            "60 s. It also watches each module's health (a rated topic gone stale restarts it too), publishes every module's "
            "state, and, with a safety module in the system, a dead module is what puts an armed vehicle's thrusters to "
            "neutral: none of that is timed here.")

    def available(self):
        self.hangard = self.args.hangard or os.path.join(self.args.bin, 'hangard')
        if not os.access(self.hangard, os.X_OK):
            return f'{self.hangard} is not there (--hangard PATH)'
        self.msg = self.args.msg or next((d for d in (os.path.join(HERE, '..', '..', 'msg'),
                                                      os.path.join(self.args.bin, '..', 'share', 'hangar', 'msg'))
                                          if os.path.isfile(os.path.join(d, 'depth.msg'))), None)
        if not self.msg:
            return 'no directory of message types with depth.msg (--msg DIR)'
        return None

    def restart_section(self):
        lines = ['Burst=1000/60s'] if self.args.smoke else []
        if self.args.smoke:
            lines.append('ResetAfter=1s')
        if self.backoff:
            lines.append(f'Backoff={self.backoff}')
        return ('[Restart]\n' + '\n'.join(lines) + '\n') if lines else ''

    def settings(self):
        return {'manifest [Restart]': self.restart_section().replace('\n', ' ').strip() or 'defaults (Backoff=0.1s..10s, ResetAfter=30s, Burst=5/60s)'}

    def start(self):
        root = os.path.join(self.work, 'root')
        mods = os.path.join(root, 'etc', 'hangar', 'modules')
        os.makedirs(mods)
        with open(os.path.join(root, 'etc', 'hangar', 'hangard.conf'), 'w') as f:
            f.write(f'Role=vehicle\nMsgDir={os.path.abspath(self.msg)}\n')
        r = self.restart_section()
        with open(os.path.join(mods, 'restartpub.module'), 'w') as f:
            f.write(f'[Module]\nExec={" ".join(self.cmd("pub"))}\n[Publish {TOPIC}]\nType=depth\nRate={self.args.rate}\n{r}')
        with open(os.path.join(mods, 'restartsub.module'), 'w') as f:
            f.write(f'[Module]\nExec={" ".join(self.cmd("sub"))}\n[Subscribe {TOPIC}]\n{r}')
        env = dict(os.environ, HANGAR_ROOT=root, XDG_RUNTIME_DIR=root)
        self.out = open(os.path.join(self.work, 'hangard.log'), 'w')
        self.p = subprocess.Popen([self.hangard], env=env, stdin=subprocess.DEVNULL, stdout=self.out, stderr=self.out)

    def stop(self):
        self.p.send_signal(signal.SIGINT)
        try:
            self.p.wait(15)
        except subprocess.TimeoutExpired:
            self.p.kill()
        self.out.close()


class HangardFast(Hangard):
    name, title = 'hangard-fast', 'hangard, Backoff=1ms..10s'
    backoff = '1ms..10s'


class Systemd(Supervisor):
    name, title = 'systemd', 'systemd --user, Restart=always, defaults'
    props = ['Restart=always']
    what = ("systemd's user manager, a transient service a process: Restart=always, and its defaults for the rest "
            "(RestartSec=100ms; after 5 starts in 10 s the unit is left failed). It restarts the process. It knows nothing of "
            "the process's messages, of whether they are stale, or of a vehicle.")

    def available(self):
        if not shutil.which('systemd-run') or not shutil.which('systemctl'):
            return 'no systemd-run here'
        r = subprocess.run(['systemctl', '--user', 'is-system-running'], capture_output=True, text=True)
        if r.stdout.strip() not in ('running', 'degraded'):
            return (f'no user manager to talk to (systemctl --user: {(r.stdout + r.stderr).strip()[:80] or "no answer"}): '
                    'a login session, or lingering (loginctl enable-linger, which may ask for authentication)')
        return None

    def unit(self, role):
        return f'hangar-restart-{role}-{os.getpid()}'

    def all_props(self):
        return self.props + (['StartLimitIntervalSec=0'] if self.args.smoke and 'StartLimitIntervalSec=0' not in self.props else [])

    def settings(self):
        r = subprocess.run(['systemctl', '--user', 'show', '-p', 'DefaultRestartUSec', '-p', 'DefaultStartLimitIntervalUSec',
                            '-p', 'DefaultStartLimitBurst', '-p', 'Version'], capture_output=True, text=True)
        return {'properties': ' '.join(self.all_props()), 'manager': ' '.join(r.stdout.split())}

    def start(self):
        os.makedirs(self.topics, exist_ok=True)
        for role in ('sub', 'pub'):
            cmd = ['systemd-run', '--user', '--quiet', '--collect', f'--unit={self.unit(role)}', f'--setenv=HANGAR_TOPIC_DIR={self.topics}']
            for p in self.all_props():
                cmd += ['-p', p]
            subprocess.run(cmd + self.cmd(role), check=True, capture_output=True)

    def stop(self):
        for role in ('sub', 'pub'):
            subprocess.run(['systemctl', '--user', 'stop', self.unit(role)], capture_output=True)
            subprocess.run(['systemctl', '--user', 'reset-failed', self.unit(role)], capture_output=True)


class SystemdFast(Systemd):
    name, title = 'systemd-fast', 'systemd --user, Restart=always, RestartSec=0, no start limit'
    props = ['Restart=always', 'RestartSec=0', 'StartLimitIntervalSec=0']


class Ros2Launch(Supervisor):
    name, title = 'ros2-launch', 'ros2 launch, ExecuteProcess(respawn=True)'
    what = ("ROS 2's launch, the usual way a ROS 2 system is started: ExecuteProcess with respawn=True, and respawn_delay at "
            "its default of 0. A Python process that restarts a child when it exits. It knows nothing of the child's "
            "messages or of a vehicle, and nothing restarts launch itself. The process here is the same lib/hg program as in "
            "every other row, not an rclcpp node: what is compared is the restarting.")

    def available(self):
        if not shutil.which('ros2'):
            return 'no ros2 here (source ROS 2 first)'
        return None

    def settings(self):
        return {'launch': 'ExecuteProcess(respawn=True, respawn_delay=0.0)', 'ROS_DISTRO': os.environ.get('ROS_DISTRO', '?')}

    def start(self):
        os.makedirs(self.topics, exist_ok=True)
        path = os.path.join(self.work, 'restart.launch.py')
        with open(path, 'w') as f:
            f.write('from launch import LaunchDescription\nfrom launch.actions import ExecuteProcess\n\n\n'
                    'def generate_launch_description():\n    return LaunchDescription([\n'
                    + ''.join(f'        ExecuteProcess(cmd={self.cmd(role)!r}, respawn=True, respawn_delay=0.0, '
                              f'additional_env={{"HANGAR_TOPIC_DIR": {self.topics!r}}}),\n' for role in ('sub', 'pub'))
                    + '    ])\n')
        self.out = open(os.path.join(self.work, 'launch.log'), 'w')
        self.p = subprocess.Popen(['ros2', 'launch', path], stdin=subprocess.DEVNULL, stdout=self.out, stderr=self.out,
                                  start_new_session=True)

    def stop(self):
        try:
            os.killpg(self.p.pid, signal.SIGINT)
            self.p.wait(15)
        except (ProcessLookupError, subprocess.TimeoutExpired):
            try:
                os.killpg(self.p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        self.out.close()


SUPERVISORS = [Hangard, HangardFast, Systemd, SystemdFast, Ros2Launch]


def run_one(sup, args):
    """The subscriber killed N times, then the publisher: one record a kill."""
    kills = []
    sup.start()
    try:
        def flowing():
            s, m = read_log(sup.log)
            return m and s
        if not wait_for(flowing, 30):
            return kills, 'no message within 30 s of starting'
        time.sleep(args.settle)
        for role in ('sub', 'pub'):
            for k in range(args.kills):
                starts, msgs = read_log(sup.log)
                pid = [p for r, p, _ in starts if r == role][-1]
                n_before = len(msgs)
                t_kill = time.monotonic_ns()
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    kills.append({'role': role, 'k': k, 'error': f'process {pid} was already gone'})
                    time.sleep(args.spacing)
                    continue

                def again():
                    s, m = read_log(sup.log)
                    new = [(p, t) for r, p, t in s if r == role and t > t_kill and p != pid]
                    if not new:
                        return None
                    npid = new[0][0]
                    first = [t for rd, wr, t in m if t > t_kill and (rd if role == 'sub' else wr) == npid]
                    return (new[0][1], first[0]) if first else None
                got = wait_for(again, args.timeout, 0.01)
                rec = {'role': role, 'k': k, 't_kill_ns': t_kill, 'messages_before': n_before}
                if got:
                    rec.update(running_ms=round((got[0] - t_kill) / 1e6, 3), messages_ms=round((got[1] - t_kill) / 1e6, 3))
                else:
                    s, _ = read_log(sup.log)
                    came = [t for r, p, t in s if r == role and t > t_kill and p != pid]
                    rec['error'] = (f'restarted after {(came[0] - t_kill) / 1e6:.1f} ms, but no message within {args.timeout} s' if came
                                    else f'not restarted within {args.timeout} s')
                kills.append(rec)
                say(f'  {sup.name:13s} {role} {k}: ' + (f'running {rec["running_ms"]} ms, messages {rec["messages_ms"]} ms'
                                                         if got else rec['error']))
                time.sleep(args.spacing)
    finally:
        sup.stop()
    return kills, None


def report(docs):
    out = ['# bench/pairs/restart.py: a process killed, to running again and to messages again', '']
    if any(d.get('smoke') for d in docs):
        out += ['**A smoke run: the script proved, nothing measured (2 kills a few seconds apart, settings that are not the defaults).**', '']
    out += ['These rows restart a process. None of them is what hangar does about an armed vehicle when a module dies '
            '(thrusters to neutral, ArduSub disarmed: tools/rov/failtest.py, on the vehicle); systemd and ros2 launch have '
            'nothing that corresponds to that. One program, `restart_node`, a publisher and a subscriber of one lib/hg topic, '
            'under each supervisor; killed with SIGKILL; times in ms from the kill, on the clock the node logs with.', '']
    for d in docs:
        e = d['environment']
        out += [f'Session {e["session"]}: {e["model"]}, kernel {e["kernel"]}; {d["kills"]} kills a role, {d["spacing_s"]} s apart, '
                f'the topic at {d["rate"]} Hz (a message every {1000 / d["rate"]:.0f} ms: no "messages" figure can be finer).', '']
    names = []
    for d in docs:
        for n in d['supervisors']:
            if n not in names:
                names.append(n)

    def cell(xs):
        if not xs:
            return '–'
        if len(xs) < 3:
            return ', '.join(f'{x:.1f}' for x in xs) + f' (n={len(xs)})'
        return f'{statistics.median(xs):.1f} [{min(xs):.1f}–{max(xs):.1f}] (n={len(xs)})'

    out += ['| supervisor | killed | running again, median [range] | messages again, median [range] | not restarted |', '|---|---|---|---|---|']
    for n in names:
        for role, word in (('sub', 'the subscriber'), ('pub', 'the publisher')):
            ks = [k for d in docs for k in d['supervisors'].get(n, {}).get('kills', []) if k['role'] == role]
            if not ks:
                continue
            ok = [k for k in ks if 'running_ms' in k]
            out.append(f'| `{n}` | {word} | {cell([k["running_ms"] for k in ok])} | {cell([k["messages_ms"] for k in ok])} | '
                       f'{len(ks) - len(ok)} of {len(ks)} |')
    out.append('')
    out += ['## The supervisors', '', '| supervisor | what | settings | what it is and is not |', '|---|---|---|---|']
    for n in names:
        s = next(d['supervisors'][n] for d in docs if n in d['supervisors'])
        sets = '; '.join(f'{k}: {v}' for k, v in s.get('settings', {}).items())
        out.append(f'| `{n}` | {s["title"]} | {sets} | {s["what"]} |')
    skipped = [(d['environment']['session'], n, w) for d in docs for n, w in d.get('skipped', {}).items()]
    if skipped:
        out += ['', 'Not run: ' + '; '.join(f'`{n}` ({ses}): {w}' for ses, n, w in skipped) + '.']
    errs = [(n, k) for d in docs for n, s in d['supervisors'].items() for k in s.get('kills', []) if k.get('error')]
    errs += [(n, {'role': '-', 'k': '-', 'error': s['error']}) for d in docs for n, s in d['supervisors'].items() if s.get('error')]
    if errs:
        out += ['', '## Kills that did not end in a restart with messages', ''] + [f'- `{n}`, {k["role"]} {k["k"]}: {k["error"]}' for n, k in errs]
    out.append('')
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--report', nargs='+', metavar='DIR')
    ap.add_argument('--bin', help='where restart_node is (and hangard, unless --hangard)')
    ap.add_argument('--hangard')
    ap.add_argument('--msg', help='the directory of message types (msg/, or a release\'s share/hangar/msg)')
    ap.add_argument('--out')
    ap.add_argument('--kills', type=int, default=10)
    ap.add_argument('--spacing', type=float, default=35.0)
    ap.add_argument('--settle', type=float, default=None, help='seconds of running before the first kill (default: the spacing)')
    ap.add_argument('--timeout', type=float, default=30.0)
    ap.add_argument('--rate', type=int, default=100)
    ap.add_argument('--only')
    ap.add_argument('--smoke', action='store_true')
    args = ap.parse_args()
    if args.report:
        docs = []
        for d in args.report:
            with open(os.path.join(d, 'restart.json') if os.path.isdir(d) else d) as f:
                docs.append(json.load(f))
        print(report(docs))
        return 0
    if not args.bin or not args.out:
        ap.error('--bin and --out')
    if args.smoke:
        args.kills, args.spacing = 2, 3.0
    if args.settle is None:
        args.settle = args.spacing
    args.bin = os.path.abspath(args.bin)
    if not os.access(os.path.join(args.bin, 'restart_node'), os.X_OK):
        say(f'{args.bin}/restart_node is not there (make pairs)')
        return 1
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    sys.path.insert(0, HERE)
    import driver
    env = driver.environment(argparse.Namespace(commit='unknown'), {}, {})
    doc = {'bench_pairs_restart': 1, 'smoke': args.smoke, 'environment': env, 'kills': args.kills, 'spacing_s': args.spacing,
           'rate': args.rate, 'supervisors': {}, 'skipped': {}}
    only = args.only.split(',') if args.only else None
    base = f'/dev/shm/pairs-restart-{os.getpid()}'
    try:
        for cls in SUPERVISORS:
            if only and cls.name not in only:
                continue
            work = os.path.join(base, cls.name)
            os.makedirs(work)
            sup = cls(args, work)
            why = sup.available()
            if why:
                doc['skipped'][cls.name] = why
                say(f'SKIP {cls.name}: {why}')
                continue
            say(f'{cls.name}: {cls.title}')
            rec = {'title': cls.title, 'what': cls.what, 'settings': sup.settings()}
            try:
                rec['kills'], err = run_one(sup, args)
                if err:
                    rec['error'] = err
                    say(f'  {cls.name}: {err}')
            except (OSError, subprocess.SubprocessError, IndexError) as e:
                rec['kills'], rec['error'] = [], str(e)
                say(f'  {cls.name}: {e}')
            doc['supervisors'][cls.name] = rec
            keep = os.path.join(out, 'logs', cls.name)
            os.makedirs(keep, exist_ok=True)
            for f in ('node.log', 'hangard.log', 'launch.log', 'restart.launch.py'):
                if os.path.isfile(os.path.join(work, f)):
                    shutil.copy(os.path.join(work, f), keep)
            with open(os.path.join(out, 'restart.json'), 'w') as f:
                json.dump(doc, f)
    finally:
        shutil.rmtree(base, ignore_errors=True)
    with open(os.path.join(out, 'restart.md'), 'w') as f:
        f.write(report([doc]))
    say(f'results: {out}/restart.json and restart.md')
    bad = [n for n, s in doc['supervisors'].items() if s.get('error') or any(k.get('error') for k in s['kills'])]
    return 1 if bad or not doc['supervisors'] else 0


if __name__ == '__main__':
    sys.exit(main())
