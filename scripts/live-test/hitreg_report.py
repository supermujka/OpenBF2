#!/usr/bin/env python3
"""Per-match, per-player hit-registration report for a folder of openbf2_hitreg_*.csv files.

[CHOSEN, not recovered] Test tooling. Works with the 143-column feat/hitreg telemetry (v1_*)
and the 82/91/102-column telemetry (act_*). Each finished CSV is summarised once and cached
in <folder>/.hitreg_report_cache.json (keyed by name, size and mtime), so re-running only
reads new files. Files still being written (*.filepart, or modified in the last --settle
seconds) are skipped.

Usage:
  python3 hitreg_report.py <folder> [--players "zzz Mujka[BIH],SYNKRONIZED"] [--settle 60]
                           [--out report.md] [--no-cache]
"""
import argparse
import csv
import glob
import json
import os
import sys
import time
from collections import defaultdict

CACHE_NAME = '.hitreg_report_cache.json'
CACHE_VERSION = 1
BOT_PREFIX = 'LoadBot'


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def pct(values, p):
    v = sorted(x for x in values if x is not None)
    return v[min(len(v) - 1, int(len(v) * p))] if v else None


def summarise_file(path):
    """Stream one CSV and return a JSON-serialisable summary per shooter."""
    with open(path, newline='', errors='replace') as fh:
        rd = csv.reader(fh)
        hdr = next(rd)
        col = {name: i for i, name in enumerate(hdr)}
        live = 'v1' if 'v1_hit' in col else 'act'
        need = ['event', 'time_s', 'shooter', 'tgt', 'ping_ms', 'raw_interp_ms', 'q_depth',
                live + '_hit', 'van_hit', live + '_rewind_ms', 'van_rewind_ms', 'aim_hit',
                'dev_deg', 'killed', 'q_drain_drops', 'tick_us_p99', 'outcome', 'weapon']
        idx = {k: col.get(k) for k in need}

        def g(rec, k):
            i = idx[k]
            return rec[i] if i is not None and i < len(rec) else ''

        per = defaultdict(lambda: {
            'fire': 0, 'probe': 0, 'hit': 0, 'lv_only': 0, 'van_only': 0, 'aim_on': 0,
            'aim_hit': 0, 'kills': 0, 'deaths': 0, 'deliver': 0, 'wall': 0, 'drain_max': 0.0,
            'ping': [], 'interp': [], 'qdep': [], 'dev': [], 'rew_live': [], 'rew_van': [],
            'weapons': defaultdict(int)})
        t_min, t_max, ticks, config = None, None, [], ''
        for rec in rd:
            if len(rec) < len(hdr):
                continue
            ev = g(rec, 'event')
            t = num(g(rec, 'time_s'))
            if t is not None:
                t_min = t if t_min is None else min(t_min, t)
                t_max = t if t_max is None else max(t_max, t)
            if ev == 'CONFIG':
                config = next((c for c in rec if 'improved=' in c), '')
                continue
            who = g(rec, 'shooter').strip()
            if not who:
                continue
            s = per[who]
            if ev == 'FIRE':
                s['fire'] += 1
                s['weapons'][g(rec, 'weapon')] += 1
                for key, field in (('ping', 'ping_ms'), ('interp', 'raw_interp_ms'),
                                   ('qdep', 'q_depth'), ('dev', 'dev_deg')):
                    v = num(g(rec, field))
                    if v is not None:
                        s[key].append(v)
            elif ev == 'PROBE':
                s['probe'] += 1
                lh, vh = g(rec, live + '_hit') == '1', g(rec, 'van_hit') == '1'
                s['hit'] += lh
                s['lv_only'] += lh and not vh
                s['van_only'] += vh and not lh
                if g(rec, 'aim_hit') == '1':
                    s['aim_on'] += 1
                    s['aim_hit'] += lh
                for key, field in (('rew_live', live + '_rewind_ms'), ('rew_van', 'van_rewind_ms')):
                    v = num(g(rec, field))
                    if v is not None:
                        s[key].append(v)
                d = num(g(rec, 'q_drain_drops'))
                if d is not None and d > s['drain_max']:
                    s['drain_max'] = d
                tk = num(g(rec, 'tick_us_p99'))
                if tk is not None:
                    ticks.append(tk)
            elif ev == 'DELIVER':
                s['deliver'] += 1
                s['wall'] += g(rec, 'outcome').startswith('OVERRIDE')
            elif ev == 'DAMAGE' and g(rec, 'killed') == '1':
                s['kills'] += 1
                victim = g(rec, 'tgt').strip()
                if victim:
                    per[victim]['deaths'] += 1

    out = {'minutes': ((t_max or 0) - (t_min or 0)) / 60.0, 'config': config, 'format': live,
           'tick_p99_p50': pct(ticks, .5), 'tick_p99_p95': pct(ticks, .95), 'players': {}}
    for who, s in per.items():
        out['players'][who] = {
            'fire': s['fire'], 'probe': s['probe'], 'hit': s['hit'], 'lv_only': s['lv_only'],
            'van_only': s['van_only'], 'aim_on': s['aim_on'], 'aim_hit': s['aim_hit'],
            'kills': s['kills'], 'deaths': s['deaths'], 'deliver': s['deliver'], 'wall': s['wall'],
            'drain_max': s['drain_max'],
            'ping50': pct(s['ping'], .5), 'interp50': pct(s['interp'], .5),
            'interp95': pct(s['interp'], .95), 'qdep50': pct(s['qdep'], .5),
            'dev50': pct(s['dev'], .5), 'rew_live50': pct(s['rew_live'], .5),
            'rew_van50': pct(s['rew_van'], .5),
            'weapon': max(s['weapons'], key=s['weapons'].get) if s['weapons'] else ''}
    return out


def fmt(v, spec='%.0f'):
    return '-' if v is None else spec % v


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('folder')
    ap.add_argument('--players', default='', help='comma-separated names; default: every non-bot shooter')
    ap.add_argument('--settle', type=float, default=60.0, help='skip files modified in the last N seconds')
    ap.add_argument('--out', help='also write the report as Markdown to this file')
    ap.add_argument('--no-cache', action='store_true')
    a = ap.parse_args()

    cache_path = os.path.join(a.folder, CACHE_NAME)
    cache = {}
    if not a.no_cache and os.path.exists(cache_path):
        try:
            cache = json.load(open(cache_path))
            if cache.get('_v') != CACHE_VERSION:
                cache = {}
        except (OSError, ValueError):
            cache = {}
    now = time.time()
    files = sorted(glob.glob(os.path.join(a.folder, 'openbf2_hitreg_*.csv')))
    results, skipped, fresh = [], [], 0
    for p in files:
        st = os.stat(p)
        if now - st.st_mtime < a.settle:
            skipped.append(os.path.basename(p))
            continue
        key = '%s|%d|%d' % (os.path.basename(p), st.st_size, int(st.st_mtime))
        if key not in cache:
            sys.stderr.write('reading %s ...\n' % os.path.basename(p))
            cache[key] = summarise_file(p)
            fresh += 1
        results.append((os.path.basename(p), cache[key]))
    skipped += [os.path.basename(p) for p in glob.glob(os.path.join(a.folder, '*.filepart'))]
    if not a.no_cache:
        cache['_v'] = CACHE_VERSION
        live_keys = {'%s|%d|%d' % (os.path.basename(p), os.stat(p).st_size, int(os.stat(p).st_mtime)) for p in files}
        cache = {k: v for k, v in cache.items() if k == '_v' or k in live_keys}
        json.dump(cache, open(cache_path, 'w'))

    want = [w.strip() for w in a.players.split(',') if w.strip()]
    lines = []
    w = lines.append
    w('# Hitreg report: %s' % a.folder)
    w('')
    w('%d file(s), %d newly read, %d skipped (still downloading or just written).' % (len(results), fresh, len(skipped)))
    w('')
    w('| match | min | igrac | oruzje | hitaca | pogoci | nisan->pogodak | live/van | interp p50/p95 | ping | red | drain | rasip. | K/D | tick p99 |')
    w('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    totals = defaultdict(lambda: defaultdict(float))
    for name, r in results:
        humans = [p for p in r['players'] if not p.startswith(BOT_PREFIX)]
        for who in (want or sorted(humans)):
            s = r['players'].get(who)
            if not s or s['fire'] == 0:
                continue
            n = s['probe'] or 1
            w('| %s | %.0f | %s | %s | %d | %.1f%% | %.0f%% | %d/%d | %s/%s | %s | %s | %d | %s | %d/%d | %s |' % (
                name[15:30], r['minutes'], who, s['weapon'], s['fire'], 100.0 * s['hit'] / n,
                100.0 * s['aim_hit'] / (s['aim_on'] or 1), s['lv_only'], s['van_only'],
                fmt(s['interp50']), fmt(s['interp95']), fmt(s['ping50']), fmt(s['qdep50']),
                s['drain_max'], fmt(s['dev50'], '%.2f'), s['kills'], s['deaths'],
                fmt(r['tick_p99_p95'] and r['tick_p99_p95'] / 1000.0, '%.0f ms')))
            t = totals[who]
            for k in ('fire', 'probe', 'hit', 'aim_on', 'aim_hit', 'lv_only', 'van_only', 'kills', 'deaths'):
                t[k] += s[k]
            t['minutes'] += r['minutes']
    w('')
    w('## Ukupno po igracu')
    w('')
    w('| igrac | min | hitaca | pogoci | nisan->pogodak | live/van (na 100 proba) | K/D | kill/min |')
    w('|---|---|---|---|---|---|---|---|')
    for who, t in sorted(totals.items(), key=lambda kv: -kv[1]['fire']):
        n = t['probe'] or 1
        w('| %s | %.0f | %d | %.1f%% | %.0f%% | %d/%d (%+.2f) | %d/%d | %.1f |' % (
            who, t['minutes'], t['fire'], 100.0 * t['hit'] / n, 100.0 * t['aim_hit'] / (t['aim_on'] or 1),
            t['lv_only'], t['van_only'], 100.0 * (t['lv_only'] - t['van_only']) / n,
            t['kills'], t['deaths'], t['kills'] / (t['minutes'] or 1)))
    w('')
    w('live/van = pogoci samo live hitrega / samo vanilla rewinda na istim hicima; drain = max odbacenih inputa; '
      'rasip. = odstupanje metka od nisana p50 (stepeni); tick p99 = p95 kliznog p99 servera (33 ms = budzet).')
    if skipped:
        w('')
        w('Preskoceno: ' + ', '.join(skipped))
    text = '\n'.join(lines)
    print(text)
    if a.out:
        open(a.out, 'w').write(text + '\n')


if __name__ == '__main__':
    main()
