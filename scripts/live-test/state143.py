"""State summary for a 143-column (feat/hitreg) hitreg CSV: humans vs bots, live (v1) vs vanilla."""
import csv, sys
from collections import Counter, defaultdict

rows = list(csv.DictReader(open(sys.argv[1], newline='')))


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def q(v, p=.5):
    v = sorted(x for x in v if x is not None)
    return v[min(len(v) - 1, int(len(v) * p))] if v else float('nan')


t0, t1 = num(rows[1]['time_s']), num(rows[-1]['time_s'])
print('span %.0f s (%.1f min)' % (t1 - t0, (t1 - t0) / 60))
cfg = [r for r in rows if r['event'] == 'CONFIG']
if cfg:
    print('CONFIG', [v for v in cfg[0].values() if v and '=' in v][:1])

by = defaultdict(list)
for r in rows:
    s = r.get('shooter', '').strip()
    if s:
        by['BOTS' if s.startswith('LoadBot') else s].append(r)

print('\n%-16s %5s %5s %6s %5s %5s %6s %6s %7s %7s %6s %6s %6s %6s %5s' % (
    'shooter', 'FIRE', 'PROBE', 'v1hit%', 'v1on', 'vanon', 'aimOn', 'aimMis', 'miss50', 'dev50', 'ping', 'interp', 'qDep', 'rewV1', 'kills'))
for who, R in sorted(by.items(), key=lambda kv: -len(kv[1])):
    fire = [r for r in R if r['event'] == 'FIRE']
    pr = [r for r in R if r['event'] == 'PROBE']
    dm = [r for r in R if r['event'] == 'DAMAGE']
    n = len(pr) or 1
    print('%-16s %5d %5d %5.1f%% %5d %5d %6d %6d %7.2f %7.2f %6.0f %6.0f %6.0f %6.0f %5d' % (
        who[:16], len(fire), len(pr), 100.0 * sum(r['v1_hit'] == '1' for r in pr) / n,
        sum(r['v1_hit'] == '1' and r['van_hit'] != '1' for r in pr),
        sum(r['van_hit'] == '1' and r['v1_hit'] != '1' for r in pr),
        sum(r.get('aim_hit') == '1' for r in pr),
        sum(r.get('aim_hit') == '1' and r['v1_hit'] != '1' for r in pr),
        q(x for x in (num(r['v1_miss_m']) for r in pr) if x is not None and x < 900),
        q(num(r.get('dev_deg')) for r in fire),
        q(num(r['ping_ms']) for r in fire), q(num(r['interp_ms']) for r in fire),
        q(num(r['q_depth']) for r in fire), q(num(r['v1_rewind_ms']) for r in pr),
        sum(r.get('killed') == '1' for r in dm)))

print('\nDeaths of humans (killer -> victim):')
deaths = Counter()
for r in rows:
    if r['event'] == 'DAMAGE' and r.get('killed') == '1':
        v = r.get('tgt', '').strip()
        if not v.startswith('LoadBot'):
            k = r.get('shooter', '').strip()
            deaths[('BOT' if k.startswith('LoadBot') else k, v)] += 1
for (k, v), c in deaths.most_common(10):
    print('  %-14s -> %-16s %d' % (k, v, c))

print('\nDELIVER outcomes:', Counter(r['outcome'] for r in rows if r['event'] == 'DELIVER').most_common())
tp = [num(r['tick_us_p99']) for r in rows if r['event'] == 'PROBE']
print('server tick_us_p99 over PROBE rows: p50 %.0f  p95 %.0f  max %.0f  (33333 = on budget)' % (q(tp), q(tp, .95), max(x for x in tp if x is not None)))
# timeline: tick p99 per minute
mins = defaultdict(list)
for r in rows:
    if r['event'] == 'PROBE' and num(r['tick_us_p99']) is not None:
        mins[int((num(r['time_s']) - t0) // 60)].append(num(r['tick_us_p99']))
print('tick_us_p99 by minute:', ' '.join('%d:%.0f' % (m, q(v, .95)) for m, v in sorted(mins.items())))
