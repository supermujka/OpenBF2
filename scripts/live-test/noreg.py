"""Human 'aimed on target but server missed' forensics using the 91-column CSV."""
import csv, glob, os, sys
from collections import Counter, defaultdict

d = sys.argv[1]
who = sys.argv[2] if len(sys.argv) > 2 else 'Mujka'
f = max(glob.glob(os.path.join(d, 'openbf2_hitreg_*.csv')), key=os.path.getmtime)
rows = list(csv.DictReader(open(f, newline='')))


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def q(v, p):
    v = sorted(x for x in v if x is not None)
    return v[min(len(v) - 1, int(len(v) * p))] if v else float('nan')


mine = [r for r in rows if who in r.get('shooter', '')]
fire = {r['shot_id']: r for r in mine if r['event'] == 'FIRE'}
probe = {}
for r in mine:
    if r['event'] == 'PROBE':
        probe.setdefault(r['shot_id'], r)
dmg = {r['shot_id'] for r in mine if r['event'] == 'DAMAGE'}
span = num(rows[-1]['time_s']) - num(rows[1]['time_s'])
print('%s  span %.0fs  FIRE %d  PROBE %d  DAMAGE %d' % (os.path.basename(f), span, len(fire), len(probe), len(dmg)))

src = Counter(r.get('act_src', '') for r in fire.values())
mism = [r for r in fire.values() if r.get('act_src') == '1' and r['fire_input_idx'] != r['act_input_idx']]
print('act_src counts (1=fresh,2=replay,blank=none):', dict(src))
print('fresh shots with fire_input_idx != act_input_idx:', len(mism))

cats = defaultdict(list)
for sid, p in probe.items():
    fr = fire.get(sid)
    if not fr:
        continue
    aim = p.get('aim_hit') == '1'
    act = p['act_hit'] == '1'
    key = ('AIM_ON_TARGET_SERVER_MISS' if aim and not act else
           'HIT' if act else 'AIM_OFF')
    cats[key].append((p, fr))
print('\ncategory | n | act_src fresh/replay | idx mismatch | dev_mag p50 | dev_deg p50 | dist p50 | ping p50 | q_depth p50 | aim_miss p50')
for k in ('HIT', 'AIM_ON_TARGET_SERVER_MISS', 'AIM_OFF'):
    L = cats[k]
    if not L:
        continue
    fr = [x[1] for x in L]
    pr = [x[0] for x in L]
    print('%s | %d | %d/%d | %d | %.2f | %.2f | %.1f | %.0f | %.0f | %.2f' % (
        k, len(L),
        sum(1 for r in fr if r.get('act_src') == '1'), sum(1 for r in fr if r.get('act_src') == '2'),
        sum(1 for r in fr if r.get('act_src') == '1' and r['fire_input_idx'] != r['act_input_idx']),
        q([num(r['dev_mag']) for r in fr], .5), q([num(r['dev_deg']) for r in fr], .5),
        q([num(r['shot_dist_m']) for r in pr], .5), q([num(r['ping_ms']) for r in fr], .5),
        q([num(r['q_depth']) for r in fr], .5), q([num(r['aim_miss_m']) for r in pr], .5)))
print('\nweapons:', Counter(r['weapon'] for r in fire.values()).most_common(4))
miss = cats['AIM_ON_TARGET_SERVER_MISS']
if miss:
    print('\nAIM_ON_TARGET_SERVER_MISS by dev_mag bucket (server spread %):')
    b = Counter()
    for p, fr in miss:
        m = num(fr['dev_mag']) or 0
        b['<0.5' if m < .5 else '0.5-1' if m < 1 else '1-2' if m < 2 else '>=2'] += 1
    print(dict(b))
    hits = cats['HIT']
    b2 = Counter()
    for p, fr in hits:
        m = num(fr['dev_mag']) or 0
        b2['<0.5' if m < .5 else '0.5-1' if m < 1 else '1-2' if m < 2 else '>=2'] += 1
    print('HIT by dev_mag bucket:', dict(b2))
