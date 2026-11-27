#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bench/pairs/tests/test_driver.py BUILDDIR OUTDIR: the driver proved over hangar's node
pair and the null system (`make check-pairs`). It checks that the harness does what
the protocol says (rounds, orders, counts, records, the report), never how fast
anything was: no latency is compared with anything."""
import copy
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(HERE))
import driver  # noqa: E402

BIN, OUT = sys.argv[1], sys.argv[2]
DRIVER = os.path.join(os.path.dirname(HERE), 'driver.py')
fails = n = 0


def check(what, cond, detail=''):
    global fails, n
    n += 1
    if cond:
        print(f'  ok    {what}')
    else:
        fails += 1
        print(f'  FAIL  {what}' + (f': {detail}' if detail else ''))


def run(out, *more, plan='plan-check.json'):
    cmd = [sys.executable, '-I', DRIVER, '--plan', os.path.join(HERE, plan), '--systems', os.path.join(HERE, 'systems-check.json'),
           '--var', f'bin={BIN}', '--out', out, '--seed', '11'] + list(more)
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    return r.returncode, r.stdout + r.stderr


print('== the histogram, against pair.h\'s two lines')
for ns in (0, 1, 31, 32, 33, 63, 64, 1000, 123456, 10**9, 2**40 + 5):
    i = ns if ns < 32 else 32 + (ns.bit_length() - 1 - 5) * 32 + ((ns >> (ns.bit_length() - 1 - 5)) & 31)
    check(f'{ns} ns falls in the bin that starts at or below it and ends above it',
          driver.bin_lower(i) <= ns < driver.bin_lower(i + 1) or (ns < 32 and driver.bin_lower(i) == ns))
h = driver.hist_merge([{'n': 1000, 'min_ns': 10, 'max_ns': 5000, 'sum_ns': 1e6, 'bins': [[10, 990], [250, 10]]}])
check('a quantile is the upper edge of its bin, never above the largest sample',
      driver.hist_quantile(h, 0.5) == 10 and driver.hist_quantile(h, 0.999) <= 5000)
check('the bootstrap interval holds the median, and needs three runs',
      driver.bootstrap_ci([1.0, 2.0], 1) is None and (lambda c: c[0] <= 5 <= c[1])(driver.bootstrap_ci([3, 4, 5, 6, 7.0], 1)))
check('a run is as long as its rate needs for the samples, within the plan\'s bounds',
      driver.run_seconds({'seconds': 20, 'max_seconds': 120, 'target_samples': 100000}, 10000) == 20 and
      driver.run_seconds({'seconds': 20, 'max_seconds': 120, 'target_samples': 100000}, 2000) == 50 and
      driver.run_seconds({'seconds': 20, 'max_seconds': 120, 'target_samples': 100000}, 100) == 120)

print('== a session of two rounds')
a = os.path.join(OUT, 'a')
rc, text = run(a, '--no-preflight')
check('the driver ends with status 0', rc == 0, text[-2000:])
with open(os.path.join(a, 'results.json')) as f:
    doc = json.load(f)
runs = doc['runs']
systems = ['null', 'hangar', 'with-helper']
check('the systems that can run are run, the one that is not installed is named and skipped',
      list(doc['systems']) == systems and [s['system'] for s in doc['skipped']] == ['absent'], str(doc['skipped']))
check('four cells from three sweeps', len(doc['cells']) == 4, str([driver.cell_key(c) for c in doc['cells']]))
check('two rounds, each with every cell and every system once, in an order that is written down',
      len(doc['rounds']) == 2 and all(sorted(k for k, _ in r['order']) == sorted(driver.cell_key(c) for c in doc['cells']) and
                                      all(sorted(ss) == sorted(systems) for _, ss in r['order']) for r in doc['rounds']))
check('the two rounds\' orders differ, and the seed is recorded', doc['rounds'][0]['order'] != doc['rounds'][1]['order'] and doc['seed'] == 11)
check('every cell x system x round has exactly one run that counts',
      sorted((r['round'], driver.cell_key(r['cell']), r['system']) for r in runs if not r.get('superseded')) ==
      sorted((rd, driver.cell_key(c), s) for rd in (0, 1) for c in doc['cells'] for s in systems))

lat = [r for r in runs if r['cell']['type'] == 'latency' and not r['result'].get('not_run')]
errs = [(driver.cell_key(r['cell']), r['system'], r['result']['errors']) for r in runs if r['result'].get('errors')]
check('no run failed', not errs, str(errs))
ok_counts = all(s['unique'] + s['lost'] == s['expected'] == 200 and s['call']['n'] == s['unique'] == s['intended']['n']
                and s['received'] == s['unique'] + s['duplicated'] for r in lat for s in r['result']['subs'])
check('every subscriber accounts for every message: unique + lost = expected, a sample a unique message', ok_counts)
check('nothing corrupt, nothing out of order', all(s['corrupt'] == 0 and s['out_of_order'] == 0 for r in lat for s in r['result']['subs']))
check('the null system ran in one process and lost nothing',
      all(len(r['result']['subs']) == 1 and r['result']['subs'][0]['lost'] == 0 and r['result']['pub'] is None for r in lat if r['system'] == 'null'))
two = [r for r in lat if r['cell']['subscribers'] == 2]
check('two subscribers: two results, each its own', two and all([s['id'] for s in r['result']['subs']] == [0, 1] for r in two))
check('a cell above a system\'s limit, and more subscribers than one process has, are recorded as not run, with the reason',
      all(r['result'].get('not_run') for r in runs if r['cell']['size'] == 4096 and r['system'] in ('null', 'with-helper'))
      and any('the check\'s own limit' in r['result'].get('not_run', '') for r in runs))
rt = [r for r in runs if r['cell']['condition'] == 'rt']
check('the real-time cell ran, or is recorded as not permitted here: ' +
      ('ran' if all(not r['result'].get('not_run') for r in rt) else 'not permitted'),
      all(not r['result'].get('not_run') for r in rt) or all('not permitted' in r['result'].get('not_run', '') for r in rt))
helper = [r for r in lat if r['system'] == 'with-helper']
check('a helper process is started and its CPU and memory counted with the system',
      helper and all('helper-sleeper' in r['result']['cost']['cpu_pct'] and r['result']['cost']['mem_kb'].get('helper-sleeper', 0) > 0 for r in helper))
check('the cost names its interval and the kind of memory', all(r['result']['cost']['interval_s'] > 0 and
                                                                 set(r['result']['cost']['mem_kind'].values()) <= {'pss', 'rss'} for r in lat))
st = [r for r in runs if r['cell']['type'] == 'startup' and not r['result'].get('not_run')]
check('start-up and rejoin: a subscriber started beside a running publisher has its first message, twice',
      len(st) == 4 and all(r['result'].get('startup_ms', 0) > 0 and r['result'].get('rejoin_ms', 0) > 0 for r in st), str([r['result'] for r in st]))
env = doc['environment']
check('the environment: session, boot, kernel, cores, priority class', all(env.get(k) for k in ('session', 'boot_id', 'kernel', 'cores', 'priority_class'))
      and env['session'].endswith(env['boot_id'][:8]))
check('every run has the machine\'s state before and after', all('temp_c' in r['before'] and 'governor' in r['after'] for r in lat))
v = doc['systems']['hangar']['version']
check('each system\'s version, compiler and flags', v.get('version', '').startswith('lib/hg') and v.get('compiler') and '-O' in v.get('flags', ''), str(v))
check('a helper\'s version beside it', 'helper sleeper' in doc['systems']['with-helper']['version'])
check('the plan, the registry and a system\'s configuration file are copied into the results',
      all(os.path.isfile(os.path.join(a, p)) for p in ('plan.json', 'systems.json', 'config/hangar/plan-check.json', 'report.md')))
check('nothing left in /dev/shm', not [d for d in os.listdir('/dev/shm') if d.startswith('pairs-')])

print('== the report')
with open(os.path.join(a, 'report.md')) as f:
    rep = f.read()
check('it says it is a smoke run', 'not a measurement' in rep)
check('a percentile a run has no samples for is not printed, and the reason is', '## The same, 99.9th percentile' not in rep and
      'p999 needs 100000' in rep)
check('lost, the other faults, the cost and the start-up each have a table',
      all(t in rep for t in ('## Messages lost', '## Delivered twice', '## CPU, every process', '## Memory, every process', '## A subscriber started')))
check('what was not run is listed with its reason', 'NOT RUN: ' in rep and 'absent' in rep)
# A second session: the same runs under another boot's name, every latency a tenth more.
b = os.path.join(OUT, 'b')
os.makedirs(b, exist_ok=True)
doc2 = copy.deepcopy(doc)
doc2['environment']['session'] = '2026-01-01-00000000'
for r in doc2['runs']:
    for s in r['result'].get('subs') or []:
        s['call']['bins'] = [[i + 4, c] for i, c in s['call']['bins']]
with open(os.path.join(b, 'results.json'), 'w') as f:
    json.dump(doc2, f)
r = subprocess.run([sys.executable, '-I', DRIVER, '--report', a, b], capture_output=True, text=True, timeout=120)
check('two sessions pooled: both listed, and the spread within against between', r.returncode == 0 and '2026-01-01-00000000' in r.stdout and
      '## Within a session and between sessions' in r.stdout and '2 session(s)' in r.stdout, r.stderr[-800:])

print('== the preflight before a round')
pf = os.path.join(OUT, 'preflight-fail.py')
with open(pf, 'w') as f:
    f.write('import sys\nif sys.argv[1] == "probe":\n    print("{}")\nelse:\n    print("  FAIL  pi      frozen: not frozen")\n'
            '    print("preflight (measure): 0 ok, 0 warn, 1 FAIL")\n    sys.exit(1)\n')
c = os.path.join(OUT, 'c')
rc, text = run(c, '--preflight', pf)
with open(os.path.join(c, 'results.json')) as f:
    d3 = json.load(f)
check('a FAIL stops the round before anything runs, and the verdict is in the results folder',
      rc == 1 and not d3['runs'] and d3['rounds'][0]['preflight']['verdict'].startswith('FAIL') and
      os.path.isfile(os.path.join(c, 'preflight-round-0.txt')) and 'not run' in (d3['stopped'] or ''), text[-800:])

print('== the temperature gate')
if driver.temperature() is None:
    print('  skip  no temperature to read on this machine: the gate lets every run start')
else:
    g = os.path.join(OUT, 'g')
    rc, text = run(g, '--no-preflight', '--max-temp', '-50', '--reps', '1', '--only', 'null')
    with open(os.path.join(g, 'results.json')) as f:
        d4 = json.load(f)
    hot = [r for r in d4['runs'] if not r['result'].get('not_run')]
    check('above the threshold nothing starts: each attempt is recorded, the first repeated, the last marked never clean',
          hot and all(r['flags'] == ['too-hot-to-start'] for r in hot) and any(r.get('superseded') for r in hot) and
          any(r.get('never_clean') for r in hot) and 'not started: still' in text, text[-800:])
    with open(os.path.join(g, 'report.md')) as f:
        check('and the report lists them', 'NEVER CLEAN' in f.read())

print(f'pairs: {n} checks ok' if not fails else f'pairs: {fails} of {n} checks FAILED')
sys.exit(1 if fails else 0)
