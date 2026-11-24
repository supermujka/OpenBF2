"""Summarise SV-PERF / SV-CPU server lines and loadbot telemetry per network-preset phase."""
import os, re, sys

srv = open(os.path.expanduser('~/bf2server/screen_bf2.log'), errors='replace').read().splitlines()
bots = open(os.path.expanduser('~/bots.log'), errors='replace').read().splitlines()
marks = [l.strip().split('|') for l in open(os.path.expanduser('~/bf2server/net_phases.log')) if l.count('|') == 3]


def avg(v):
    return sum(v) / len(v) if v else float('nan')


phase_cols = ('total_us', 'state_masks', 'ghost_scope', 'ghost_pack', 'net_send', 'sim_physics_coll', 'sim_players')
print('%-18s %5s %6s %5s %6s %7s %6s | %s | %6s %6s %6s | %6s %6s %6s %6s' % (
    'phase', 'conns', 'ovr33', 'drop', 'gh/pk', 'B/pk', 'pkts',
    ' '.join('%-17s' % (c + ' p50/p99') for c in phase_cols),
    'cpu%', 'main%', 'rssMB', 'snap50', 'snap95', 'gap95', 'gap99'))
for i, m in enumerate(marks):
    if m[0].startswith('END'):
        continue
    s0, b0 = int(m[2]), int(m[3])
    s1 = int(marks[i + 1][2]) if i + 1 < len(marks) else len(srv)
    b1 = int(marks[i + 1][3]) if i + 1 < len(marks) else len(bots)
    seg = srv[s0:s1]
    seg = seg[2:] if len(seg) > 4 else seg  # drop the window that straddles the switch
    hdr = [l for l in seg if l.startswith('[SV-PERF] ticks=')]
    us = [l for l in seg if l.startswith('[SV-PERF] us(')]
    cpu = [l for l in seg if l.startswith('[SV-CPU]')]
    g = lambda lines, k: [float(re.search(k + r'=([-\d.]+)', l).group(1)) for l in lines if re.search(k + r'=([-\d.]+)', l)]
    pp = {}
    for c in phase_cols:
        p50 = [float(re.search(c + r'=(\d+)/(\d+)/(\d+)/(\d+)', l).group(1)) for l in us if re.search(c + r'=(\d+)/', l)]
        p99 = [float(re.search(c + r'=(\d+)/(\d+)/(\d+)/(\d+)', l).group(3)) for l in us if re.search(c + r'=(\d+)/', l)]
        pp[c] = '%7.0f/%-9.0f' % (avg(p50), max(p99) if p99 else float('nan'))
    bseg = bots[b0:b1]
    snap = [re.search(r'SnapHz : win p50/p95/p99=([\d.]+)/([\d.]+)/([\d.]+) Hz \(gap p50/p95/p99/max=([\d.]+)/([\d.]+)/([\d.]+)', l)
            for l in bseg]
    snap = [x for x in snap if x]
    print('%-18s %5.1f %6.0f %5.0f %6.1f %7.1f %6.0f | %s | %6.1f %6.1f %6.0f | %6.1f %6.1f %6.0f %6.0f' % (
        m[0], avg(g(hdr, 'avg_conns')), sum(g(hdr, 'overruns_33ms')), sum(g(hdr, 'dropped_steps')),
        avg(g(hdr, 'avg_ghosts_per_pkt')), avg(g(hdr, 'avg_bytes_per_pkt')), avg(g(hdr, 'pkts')),
        ' '.join(pp[c] for c in phase_cols),
        avg(g(cpu, 'proc_cpu_pct')), avg(g(cpu, 'main_thread_cpu_pct')), max(g(cpu, 'max_rss_mb') or [float('nan')]),
        avg([float(x.group(1)) for x in snap]), avg([float(x.group(2)) for x in snap]),
        avg([float(x.group(5)) for x in snap]), avg([float(x.group(6)) for x in snap])))
mism = [l for l in srv if 'mismatch' in l.lower()]
print('\nmismatch lines in server log: %d' % len(mism))
for l in mism[:5]:
    print('  ' + l[:200])
