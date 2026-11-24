"""Split the live hitreg CSV by experiment phase and compare the human shooter's PROBE rows.

Usage: python3 phases.py <bf2server dir> [name-substring]
Reads exp_phases.log lines "label|wall time|csv file|line count at switch".
"""
import csv
import os
import sys

root = sys.argv[1]
who = sys.argv[2] if len(sys.argv) > 2 else 'Mujka'

marks = []
for line in open(os.path.join(root, 'exp_phases.log')):
    parts = line.strip().split('|')
    if len(parts) == 4:
        marks.append((parts[0], parts[1], parts[2], int(parts[3])))


def rows_between(fname, start, end):
    """Data rows with 1-based file line numbers in (start, end]; end None = to EOF."""
    out = []
    with open(os.path.join(root, fname), newline='') as f:
        rd = csv.reader(f)
        header = next(rd)
        for n, rec in enumerate(rd, start=2):
            if n <= start:
                continue
            if end is not None and n > end:
                break
            if len(rec) == len(header):
                out.append(dict(zip(header, rec)))
    return out


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def pct(v, q):
    if not v:
        return float('nan')
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * q))]


segments = {}   # label -> list of rows from every round of that phase
order = []
lost = 0
for i, (label, wall, fname, start) in enumerate(marks):
    if label.startswith('END'):
        continue
    nxt = marks[i + 1] if i + 1 < len(marks) else None
    end = nxt[3] if (nxt and nxt[2] == fname) else None
    if nxt and nxt[2] != fname:
        lost += 1
    if label not in segments:
        segments[label] = []
        order.append(label)
    segments[label].extend(rows_between(fname, start, end))

hdr = ('phase', 'rounds', 'probes', 'act%', 'van%', 'act-only', 'van-only',
       'actMiss50', 'vanMiss50', 'actRew50', 'vanRew50', 'ping50', 'run act/van', 'dmg', 'kills')
print(' | '.join(hdr))
for label in order:
    rows = segments[label]
    wall = sum(1 for m in marks if m[0] == label)
    mine = [r for r in rows if who in r.get('shooter', '')]
    pr = [r for r in mine if r['event'] == 'PROBE']
    dm = [r for r in mine if r['event'] == 'DAMAGE']
    a = sum(1 for r in pr if r['act_hit'] == '1')
    v = sum(1 for r in pr if r['van_hit'] == '1')
    ao = sum(1 for r in pr if r['act_hit'] == '1' and r['van_hit'] != '1')
    vo = sum(1 for r in pr if r['van_hit'] == '1' and r['act_hit'] != '1')
    run = [r for r in pr if (num(r['tgt_speed']) or 0) >= 3]
    ra = sum(1 for r in run if r['act_hit'] == '1')
    rv = sum(1 for r in run if r['van_hit'] == '1')
    am = [x for x in (num(r['act_miss_m']) for r in pr) if x is not None and 0 < x < 900]
    vm = [x for x in (num(r['van_miss_m']) for r in pr) if x is not None and 0 < x < 900]
    n = len(pr) or 1
    print(' | '.join(str(c) for c in (
        label, wall, len(pr), '%.1f' % (100.0 * a / n), '%.1f' % (100.0 * v / n), ao, vo,
        '%.2f' % pct(am, .5), '%.2f' % pct(vm, .5),
        '%.0f' % pct([num(r['act_rewind_ms']) for r in pr if num(r['act_rewind_ms']) is not None], .5),
        '%.0f' % pct([num(r['van_rewind_ms']) for r in pr if num(r['van_rewind_ms']) is not None], .5),
        '%.0f' % pct([num(r['ping_ms']) for r in pr if num(r['ping_ms']) is not None], .5),
        '%d/%d of %d' % (ra, rv, len(run)), len(dm),
        sum(1 for r in dm if r.get('killed') == '1'))))
if lost:
    print('note: CSV rotated during %d phase(s); rows written after the rotation were not counted' % lost)
