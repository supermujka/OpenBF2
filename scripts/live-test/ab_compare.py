"""Compare the two 2-minute phases per human: hits, live vs vanilla, interp, queue, spread."""
import csv, sys


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def q(v, p=.5):
    v = sorted(x for x in v if x is not None)
    return v[min(len(v) - 1, int(len(v) * p))] if v else float('nan')


print('%-10s %-15s %5s %5s %6s %5s %5s %6s %6s %8s %8s %7s %6s %6s %5s' % (
    'faza', 'igrac', 'FIRE', 'PROBE', 'hit%', 'lv', 'van', 'aimOn', 'aimHit', 'interp50', 'interp95', 'rew50', 'qDep', 'dev50', 'kill'))
for path in sys.argv[1:]:
    lab = path.split('ab_')[-1].replace('.csv', '')
    rows = list(csv.DictReader(open(path, newline='')))
    for who in ('zzz Mujka[BIH]', 'SYNKRONIZED'):
        R = [r for r in rows if r.get('shooter', '').strip() == who]
        fire = [r for r in R if r['event'] == 'FIRE']
        pr = [r for r in R if r['event'] == 'PROBE']
        dm = [r for r in R if r['event'] == 'DAMAGE']
        n = len(pr) or 1
        aim = [r for r in pr if r.get('aim_hit') == '1']
        print('%-10s %-15s %5d %5d %5.1f%% %5d %5d %6d %5.0f%% %8.0f %8.0f %7.0f %6.0f %6.2f %5d' % (
            lab, who[:15], len(fire), len(pr), 100.0 * sum(r['v1_hit'] == '1' for r in pr) / n,
            sum(r['v1_hit'] == '1' and r['van_hit'] != '1' for r in pr),
            sum(r['van_hit'] == '1' and r['v1_hit'] != '1' for r in pr),
            len(aim), 100.0 * sum(r['v1_hit'] == '1' for r in aim) / (len(aim) or 1),
            q(num(r['raw_interp_ms']) for r in fire), q((num(r['raw_interp_ms']) for r in fire), .95),
            q(num(r['v1_rewind_ms']) for r in pr), q(num(r['q_depth']) for r in fire),
            q(num(r['dev_deg']) for r in fire), sum(r.get('killed') == '1' for r in dm)))
    tp = [num(r['tick_us_p99']) for r in rows if r['event'] == 'PROBE']
    print('%-10s server tick p99: p50 %.0f us, p95 %.0f us' % (lab, q(tp), q(tp, .95)))
print("lv/van = pogoci samo live / samo vanilla na istim hicima (u VANILLA fazi su jednaki po definiciji)")
