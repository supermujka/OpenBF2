"""Rewind comparison per player: live (v1) vs vanilla vs unclamped-interp shadow (uncl10)."""
import csv, sys


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def med(v):
    v = sorted(x for x in v if x is not None)
    return v[len(v) // 2] if v else float('nan')


for label, path in zip(sys.argv[1::2], sys.argv[2::2]):
    rows = list(csv.DictReader(open(path, newline='')))
    print('\n==', label)
    for who in ('SYNKRONIZED', 'zzz Mujka[BIH]'):
        pr = [r for r in rows if r['event'] == 'PROBE' and r.get('shooter', '').strip() == who]
        if not pr:
            continue
        n = len(pr)
        v1o = sum(r['v1_hit'] == '1' and r['van_hit'] != '1' for r in pr)
        vno = sum(r['van_hit'] == '1' and r['v1_hit'] != '1' for r in pr)
        uo = sum(r.get('uncl10_hit') == '1' and r['v1_hit'] != '1' for r in pr)
        v1u = sum(r['v1_hit'] == '1' and r.get('uncl10_hit') != '1' for r in pr)
        clamped = sum((num(r['raw_interp_ms']) or 0) > 133 for r in pr)
        print('  %-15s probes %4d | live-only %3d vs vanilla-only %3d (net %+.1f/100) | unclamped-only %3d vs live-only %3d | raw interp >133: %d%% | rewind live/van p50 %.0f/%.0f' % (
            who, n, v1o, vno, 100.0 * (v1o - vno) / n, uo, v1u, 100.0 * clamped / n,
            med(num(r['v1_rewind_ms']) for r in pr), med(num(r['van_rewind_ms']) for r in pr)))
