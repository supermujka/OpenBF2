"""Deviation components for the human's shots in the newest (102-column) CSV, by outcome and pose."""
import csv, glob, os, sys
from collections import Counter, defaultdict

d = sys.argv[1]
who = sys.argv[2] if len(sys.argv) > 2 else 'Mujka'
f = max(glob.glob(os.path.join(d, 'openbf2_hitreg_*.csv')), key=os.path.getmtime)
rows = [r for r in csv.DictReader(open(f, newline='')) if who in r.get('shooter', '')]


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def q(v, p=.5):
    v = sorted(x for x in v if x is not None)
    return v[min(len(v) - 1, int(len(v) * p))] if v else float('nan')


fire = {r['shot_id']: r for r in rows if r['event'] == 'FIRE'}
cat = {}
for r in rows:
    if r['event'] != 'PROBE':
        continue
    c = 'HIT' if r['act_hit'] == '1' else ('AIM_ON_SRV_MISS' if r.get('aim_hit') == '1' else 'AIM_OFF')
    rank = {'HIT': 0, 'AIM_ON_SRV_MISS': 1, 'AIM_OFF': 2}
    if r['shot_id'] not in cat or rank[c] < rank[cat[r['shot_id']]]:
        cat[r['shot_id']] = c
print(os.path.basename(f), 'FIRE', len(fire), 'weapons', Counter(r['weapon'] for r in fire.values()).most_common(3))
cols = ['dev_mag', 'dev_min', 'dev_speed', 'dev_turn', 'dev_misc', 'dev_fire', 'dev_pose_mod', 'dev_zoom_mod',
        'sh_speed_ms', 'sh_turn_rate', 'dev_deg']
for weapon in sorted({r['weapon'] for r in fire.values()}, key=lambda w: -sum(r['weapon'] == w for r in fire.values())):
    W = [r for r in fire.values() if r['weapon'] == weapon]
    if len(W) < 10:
        continue
    print('\n== %s (%d shots)  pose counts %s  zoomed %s' % (weapon, len(W), dict(Counter(r.get('sh_pose') for r in W)),
                                                            dict(Counter(r.get('sh_zoomed') for r in W))))
    print('%-16s %4s ' % ('category', 'n') + ' '.join('%9s' % c.replace('dev_', '').replace('sh_', '')[:9] for c in cols))
    groups = defaultdict(list)
    for r in W:
        groups[cat.get(r['shot_id'], 'NO_PROBE')].append(r)
    for g in ('HIT', 'AIM_ON_SRV_MISS', 'AIM_OFF', 'NO_PROBE'):
        L = groups.get(g)
        if not L:
            continue
        print('%-16s %4d ' % (g, len(L)) + ' '.join('%9.3f' % q([num(r.get(c)) for r in L]) for c in cols))
    print('share of shots with dev_speed>0: %.0f%%, dev_turn>0: %.0f%%, dev_fire>0: %.0f%%, sh_speed>0.5 m/s: %.0f%%' % (
        100.0 * sum((num(r.get('dev_speed')) or 0) > 1e-3 for r in W) / len(W),
        100.0 * sum((num(r.get('dev_turn')) or 0) > 1e-3 for r in W) / len(W),
        100.0 * sum((num(r.get('dev_fire')) or 0) > 1e-3 for r in W) / len(W),
        100.0 * sum((num(r.get('sh_speed_ms')) or 0) > 0.5 for r in W) / len(W)))
    still = [r for r in W if (num(r.get('sh_speed_ms')) or 0) < 0.3]
    if still:
        print('physically still (<0.3 m/s): %d shots, of which dev_speed>0: %d, dev_mag p50 %.3f vs min %.3f' % (
            len(still), sum((num(r.get('dev_speed')) or 0) > 1e-3 for r in still),
            q([num(r['dev_mag']) for r in still]), q([num(r['dev_min']) for r in still])))
