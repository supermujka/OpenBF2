"""Deep dive for one human in a 143-column CSV, compared with another."""
import csv, sys
from collections import Counter

rows = list(csv.DictReader(open(sys.argv[1], newline='')))


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def q(v, p=.5):
    v = sorted(x for x in v if x is not None)
    return v[min(len(v) - 1, int(len(v) * p))] if v else float('nan')


for who in sys.argv[2:]:
    R = [r for r in rows if r.get('shooter', '').strip() == who]
    fire = [r for r in R if r['event'] == 'FIRE']
    pr = [r for r in R if r['event'] == 'PROBE']
    dl = [r for r in R if r['event'] == 'DELIVER']
    print('\n=====', who)
    print('weapons:', Counter(r['weapon'] for r in fire).most_common(4))
    print('net: ping p50/p95 %.0f/%.0f  jitter p95 %.2f  loss p95 %.1f%%  interp p50 %.0f raw_interp p50/p95 %.0f/%.0f' % (
        q(num(r['ping_ms']) for r in fire), q((num(r['ping_ms']) for r in fire), .95),
        q((num(r['rtt_jitter_ms']) for r in fire), .95), q((num(r['pkt_loss_dn_pct']) for r in fire), .95),
        q(num(r['interp_ms']) for r in fire), q(num(r['raw_interp_ms']) for r in fire),
        q((num(r['raw_interp_ms']) for r in fire), .95)))
    print('queue: depth p50/p95 %.0f/%.0f  delay p50/p95 %.0f/%.0f ms  replays30 mean %.2f  drain drops %s  cap drops %s' % (
        q(num(r['q_depth']) for r in fire), q((num(r['q_depth']) for r in fire), .95),
        q(num(r['q_delay_ms']) for r in fire), q((num(r['q_delay_ms']) for r in fire), .95),
        sum(num(r['replays_30t']) or 0 for r in fire) / (len(fire) or 1),
        max([num(r['q_drain_drops']) or 0 for r in pr] or [0]), max([num(r['q_cap_drops']) or 0 for r in pr] or [0])))
    print('input losses: fdrop_cum max %s  early_np_cum max %s  (fire presses dropped / not processed)' % (
        max([num(r['sh_fdrop_cum']) or 0 for r in fire] or [0]), max([num(r['sh_early_np_cum']) or 0 for r in fire] or [0])))
    print('deviation deg p50/p90 %.2f/%.2f  cone p50 %.2f' % (q(num(r['dev_deg']) for r in fire), q((num(r['dev_deg']) for r in fire), .9),
                                                           q(num(r['dev_cone_deg']) for r in fire)))
    aim_on = [r for r in pr if r.get('aim_hit') == '1']
    miss = [r for r in aim_on if r['v1_hit'] != '1']
    print('aim on target %d -> server hit %d, missed %d (%.0f%%); of misses vanilla would hit %d' % (
        len(aim_on), len(aim_on) - len(miss), len(miss), 100.0 * len(miss) / (len(aim_on) or 1), sum(r['van_hit'] == '1' for r in miss)))
    print('  miss distance buckets:', Counter('<5cm' if num(r['v1_miss_m']) < .05 else '5-15' if num(r['v1_miss_m']) < .15
                                              else '15-30' if num(r['v1_miss_m']) < .3 else '>30cm' for r in miss if num(r['v1_miss_m']) is not None))
    print('  shot distance p50 %.1f m, target speed p50 %.2f m/s, target pose diff (v1 vs live) share %.0f%%' % (
        q(num(r['shot_dist_m']) for r in pr), q(num(r['tgt_speed']) for r in pr),
        100.0 * sum(r.get('tgt_pose_diff') == '1' for r in pr) / (len(pr) or 1)))
    print('deliver:', Counter(r['outcome'] for r in dl).most_common(), ' blockers:', Counter(r['blocker'] for r in dl if r['outcome'] != 'DELIVERED').most_common(3))
