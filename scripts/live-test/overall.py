"""Per-player totals per dataset with kills per minute."""
import csv, sys


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def q(v, p=.5):
    v = sorted(x for x in v if x is not None)
    return v[min(len(v) - 1, int(len(v) * p))] if v else float('nan')


print('%-22s %-15s %5s %6s %6s %7s %9s %8s %6s %7s %6s' % ('skup', 'igrac', 'min', 'PROBE', 'hit%', 'aim->hit', 'live/van', 'interp50', 'qDep', 'kill/min', 'dev50'))
for lab, path in zip(sys.argv[1::2], sys.argv[2::2]):
    rows = list(csv.DictReader(open(path, newline='')))
    for who in ('zzz Mujka[BIH]', 'SYNKRONIZED'):
        R = [r for r in rows if r.get('shooter', '').strip() == who]
        if not R:
            continue
        ts = [num(r['time_s']) for r in R if num(r['time_s']) is not None]
        fire = [r for r in R if r['event'] == 'FIRE']
        pr = [r for r in R if r['event'] == 'PROBE']
        dm = [r for r in R if r['event'] == 'DAMAGE']
        aim = [r for r in pr if r.get('aim_hit') == '1']
        mins = 0.0
        # active minutes = number of distinct minutes with any row (robust across merged files)
        mins = len({(r.get('tick', '')[:0], int((num(r['time_s']) or 0) // 60), r.get('_f', '')) for r in R}) or 1
        n = len(pr) or 1
        print('%-22s %-15s %5d %6d %5.1f%% %7.0f%% %4d/%-4d %8.0f %6.0f %7.1f %6.2f' % (
            lab, who[:15], mins, len(pr), 100.0 * sum(r['v1_hit'] == '1' for r in pr) / n,
            100.0 * sum(r['v1_hit'] == '1' for r in aim) / (len(aim) or 1),
            sum(r['v1_hit'] == '1' and r['van_hit'] != '1' for r in pr), sum(r['van_hit'] == '1' and r['v1_hit'] != '1' for r in pr),
            q(num(r['raw_interp_ms']) for r in fire), q(num(r['q_depth']) for r in fire),
            sum(r.get('killed') == '1' for r in dm) / mins, q(num(r['dev_deg']) for r in fire)))
