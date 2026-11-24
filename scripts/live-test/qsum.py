"""Per queue-drain-target phase: the human's input queue latency, replays, drops and hit outcome."""
import csv, os, sys
from collections import defaultdict

root = os.path.expanduser('~/bf2server')
who = sys.argv[1] if len(sys.argv) > 1 else 'Mujka'
marks = [l.strip().split('|') for l in open(os.path.join(root, 'exp_phases.log')) if l.count('|') == 3]


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def q(v, p):
    v = sorted(x for x in v if x is not None)
    return v[min(len(v) - 1, int(len(v) * p))] if v else float('nan')


cache = {}


def rows_of(fn):
    if fn not in cache:
        with open(os.path.join(root, fn), newline='') as f:
            rd = csv.reader(f)
            hdr = next(rd)
            cache[fn] = [(n, dict(zip(hdr, rec))) for n, rec in enumerate(rd, start=2) if len(rec) >= len(hdr)]
    return cache[fn]


seg = defaultdict(list)
for i, (lab, wall, fn, start) in enumerate(marks):
    if lab == 'END':
        continue
    nxt = marks[i + 1] if i + 1 < len(marks) else None
    end = int(nxt[3]) if nxt and nxt[2] == fn else None
    for n, r in rows_of(fn):
        if n > int(start) and (end is None or n <= end) and who in r.get('shooter', ''):
            seg[lab].append(r)

print('%-4s %5s %5s | %7s %7s | %8s %8s | %7s %7s | %6s %6s %6s %8s' % (
    'tgt', 'FIRE', 'PROBE', 'qDep50', 'qDep95', 'qDly50', 'qDly95', 'repl30', 'drops', 'act%', 'aOnly', 'vOnly', 'rewind50'))
for lab in ('Q3', 'Q2', 'Q1'):
    R = seg.get(lab, [])
    fire = [r for r in R if r['event'] == 'FIRE']
    pr = [r for r in R if r['event'] == 'PROBE']
    drops = defaultdict(list)
    for r in pr:
        v = num(r.get('q_drain_drops'))
        if v is not None:
            drops[r['shooter']].append(v)
    dropped = sum(max(v) - min(v) for v in drops.values())
    n = len(pr) or 1
    print('%-4s %5d %5d | %7.0f %7.0f | %8.1f %8.1f | %7.2f %7.0f | %5.1f%% %6d %6d %8.0f' % (
        lab, len(fire), len(pr),
        q([num(r['q_depth']) for r in fire], .5), q([num(r['q_depth']) for r in fire], .95),
        q([num(r['q_delay_ms']) for r in fire], .5), q([num(r['q_delay_ms']) for r in fire], .95),
        sum(num(r['replays_30t']) or 0 for r in fire) / (len(fire) or 1), dropped,
        100.0 * sum(r['act_hit'] == '1' for r in pr) / n,
        sum(r['act_hit'] == '1' and r['van_hit'] != '1' for r in pr),
        sum(r['van_hit'] == '1' and r['act_hit'] != '1' for r in pr),
        q([num(r['act_rewind_ms']) for r in pr], .5)))
print('\nqDep/qDly = your queued actions and the wait before the server executes your shot (FIRE rows);')
print('repl30 = mean replays in the last 30 ticks at fire time; drops = inputs discarded by queue drain.')
