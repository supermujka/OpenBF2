"""Break down the human's 'aim on target but server missed' shots in the newest CSV."""
import csv, glob, os, sys
from collections import Counter

f = max(glob.glob(os.path.join(sys.argv[1], 'openbf2_hitreg_*.csv')), key=os.path.getmtime)
rows = [r for r in csv.DictReader(open(f, newline='')) if 'Mujka' in r.get('shooter', '')]
fire = {r['shot_id']: r for r in rows if r['event'] == 'FIRE'}


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


miss = [r for r in rows if r['event'] == 'PROBE' and r.get('aim_hit') == '1' and r['act_hit'] != '1']
hits = [r for r in rows if r['event'] == 'PROBE' and r['act_hit'] == '1']
print('aim-on-target server misses:', len(miss), ' hits:', len(hits))
print('  vanilla shadow would have hit:', sum(r['van_hit'] == '1' for r in miss))
print('  act_miss distance buckets:', Counter('<5cm' if num(r['act_miss_m']) < .05 else '5-15cm' if num(r['act_miss_m']) < .15
                                               else '15-30cm' if num(r['act_miss_m']) < .3 else '>30cm' for r in miss))
print('  aimed bone (act_near_bone) top:', Counter(r.get('act_near_bone') for r in miss).most_common(5))
print('  shooter pose at fire:', Counter(fire.get(r['shot_id'], {}).get('sh_pose') for r in miss))
print('  burst (dev_fire>0):', sum((num(fire.get(r['shot_id'], {}).get('dev_fire')) or 0) > 0 for r in miss))
print('  zoomed:', sum(fire.get(r['shot_id'], {}).get('sh_zoomed') == '1' for r in miss))
print('  target speed >=3 m/s:', sum((num(r['tgt_speed']) or 0) >= 3 for r in miss))
print('  distance p50: %.1f m' % sorted(num(r['shot_dist_m']) for r in miss)[len(miss) // 2])
dd = sorted(num(fire[r['shot_id']]['dev_deg']) for r in miss if r['shot_id'] in fire and num(fire[r['shot_id']]['dev_deg']) is not None)
dh = sorted(num(fire[r['shot_id']]['dev_deg']) for r in hits if r['shot_id'] in fire and num(fire[r['shot_id']]['dev_deg']) is not None)
print('  bullet angle off aim (dev_deg) p50 misses %.3f vs hits %.3f deg' % (dd[len(dd) // 2], dh[len(dh) // 2]))
print('\nexpected miss from spread alone: dev_deg * distance')
exp = [num(fire[r['shot_id']]['dev_deg']) * 0.01745 * num(r['shot_dist_m']) for r in miss if r['shot_id'] in fire]
exp.sort()
print('  p50 lateral offset at target from spread: %.2f m' % exp[len(exp) // 2])
