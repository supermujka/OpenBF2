#!/usr/bin/env python3
"""Analyze OpenBF2 hit-registration telemetry CSVs (openbf2_hitreg_*.csv).

The CSV is written by Code/BF2/Physics/HitregTelemetry.cpp (event types
CONFIG / FIRE / PROBE / DELIVER / DAMAGE, linked by shot_id).  This script
only uses the Python 3 standard library.

Examples:
    python analyze_hitreg_csv.py openbf2_hitreg_20261124_004012_Strike_at_Karkand_m4.csv
    python analyze_hitreg_csv.py C:\\logs\\bf2_csv                 # every openbf2_hitreg_*.csv in dir
    python analyze_hitreg_csv.py runA.csv runB.csv --compare       # side-by-side key metrics
    python analyze_hitreg_csv.py dirA dirB --compare --out report.md --min-shots 50

Semantics (verified against HitregTelemetry.cpp, see notes printed in report):
  * PROBE rows are written for soldier hits AND near-misses <= 2 m, so
    "hit rate" below is hits / probes, not hits / shots.
  * act_* = live (Active) hit-reg decision, van_* = vanilla BF2 1.50 shadow probe.
  * tick_us_p50/p99 are percentiles over the last 60 server ticks (rolling
    window), snapshotted on each PROBE row - they are not independent samples.
"""

import argparse
import csv
import math
import re
import statistics
import sys
from collections import Counter, OrderedDict, defaultdict
from pathlib import Path

# ----------------------------------------------------------------------------
# Thresholds / constants
# ----------------------------------------------------------------------------
EXPECTED_COLS = 102                   # header of this branch (82 + 9 deviation/input-idx + 11 deviation-component columns)
KNOWN_COL_VARIANTS = (82, 91, 102, 127, 143)
TICK_BUDGET_US = 33333                # 30 Hz
# tick_us measures the (paced) tick period, so ~33333 is normal; only flag overruns.
TICK_OVER_TOL_US = 1000
TICK_OVER_WARN_PCT = 1.0              # % of samples over budget+tol
LOSS_WARN_PCT = 2.0                   # pkt_loss_dn_pct p95
PING_WARN_MS = 150.0                  # ping_ms p95
JITTER_WARN_MS = 30.0                 # rtt_jitter_ms p95
DISAGREE_WARN_PCT = 5.0               # act-vs-van disagreement share of probes
LOST_HIT_WARN_PCT = 1.0               # act-hit shots without DAMAGE
PHANTOM_WARN_PCT = 1.0                # DAMAGE shots without act-hit probe
OVERRIDE_WARN_PCT = 10.0              # OVERRIDE_* share of DELIVER rows
RING_UF_WARN_PCT = 2.0                # ring_uf share (handoff: >2% = act on it)
Q_DEPTH_WARN = 4                      # q_depth p95
MISS_SENTINEL_M = 999.0               # *_miss_m cap == "no data"
HEAD_BONE = 47                        # act_bone/van_bone of the head (per handoff)
FILE_GLOB = "openbf2_hitreg_*.csv"
FILE_RE = re.compile(r"openbf2_hitreg_(\d{8})_(\d{6})_(.+?)_m(\d+)\.csv$", re.I)

KEY_COLUMNS = {  # section -> columns it needs (for missing-column warnings)
    "meta": ["event", "tick", "time_s", "shot_id"],
    "health": ["tick_us_p50", "tick_us_p99"],
    "hitreg": ["act_hit", "van_hit", "act_miss_m", "van_miss_m", "act_rewind_ms",
               "van_rewind_ms", "shot_dist_m", "ping_ms", "tgt_act_pose", "tgt_speed"],
    "deliver": ["outcome", "blocker"],
    "damage": ["final_dmg", "killed", "hitmarker"],
}

BAD_NUM = Counter()  # column -> unparsable values (per file, reset in analyze)


# ----------------------------------------------------------------------------
# Small helpers
# ----------------------------------------------------------------------------
def num(r, c):
    s = r.get(c)
    if s is None:
        return None
    try:
        v = float(s)
    except ValueError:
        BAD_NUM[c] += 1
        return None
    if not math.isfinite(v):
        BAD_NUM[c] += 1
        return None
    return v


def vals(rows, c):
    out = []
    for r in rows:
        v = num(r, c)
        if v is not None:
            out.append(v)
    return out


def pctile(sorted_v, p):
    if not sorted_v:
        return None
    k = (len(sorted_v) - 1) * p / 100.0
    lo = int(math.floor(k))
    hi = min(lo + 1, len(sorted_v) - 1)
    return sorted_v[lo] + (sorted_v[hi] - sorted_v[lo]) * (k - lo)


def describe(v):
    if not v:
        return None
    s = sorted(v)
    return {"n": len(s), "mean": sum(s) / len(s), "min": s[0], "p50": pctile(s, 50),
            "p95": pctile(s, 95), "p99": pctile(s, 99), "max": s[-1]}


def f(v, nd=2):
    if v is None:
        return "n/a"
    if isinstance(v, int):
        return str(v)
    return "%.*f" % (nd, v)


def pc(a, b):
    return 100.0 * a / b if b else None


def fp(a, b, nd=1):
    p = pc(a, b)
    return "n/a" if p is None else "%.*f%%" % (nd, p)


def shot_key(r):
    return r.get("shot_id")


def as_int(s):
    try:
        return int(float(s))
    except (TypeError, ValueError):
        return None


# ----------------------------------------------------------------------------
# Output document (renders to plain text or Markdown)
# ----------------------------------------------------------------------------
class Doc:
    def __init__(self, md):
        self.md = md
        self.lines = []

    def h(self, level, text):
        if self.md:
            self.lines += ["", "#" * level + " " + text, ""]
        elif level == 1:
            self.lines += ["", "=" * 78, text, "=" * 78]
        else:
            self.lines += ["", "--- " + text + " ---"]

    def p(self, text):
        self.lines.append(text)
        if self.md:
            self.lines.append("")

    def bullets(self, items):
        for it in items:
            self.lines.append(("- " if self.md else "  * ") + it)
        if self.md:
            self.lines.append("")

    def table(self, headers, rows):
        rows = [[str(c) for c in r] for r in rows]
        if self.md:
            esc = lambda s: s.replace("|", "\\|")
            self.lines.append("| " + " | ".join(esc(h) for h in headers) + " |")
            self.lines.append("|" + "|".join(["---"] + ["---:"] * (len(headers) - 1)) + "|")
            for r in rows:
                self.lines.append("| " + " | ".join(esc(c) for c in r) + " |")
            self.lines.append("")
            return
        w = [len(h) for h in headers]
        for r in rows:
            for i, c in enumerate(r):
                w[i] = max(w[i], len(c))
        fmt = lambda r: "  ".join(
            (c.ljust(w[i]) if i == 0 else c.rjust(w[i])) for i, c in enumerate(r))
        self.lines.append(fmt(headers))
        self.lines.append("  ".join("-" * x for x in w))
        for r in rows:
            self.lines.append(fmt(r))
        self.lines.append("")

    def render(self):
        return "\n".join(self.lines) + "\n"


# ----------------------------------------------------------------------------
# Loading
# ----------------------------------------------------------------------------
class Data:
    def __init__(self, path):
        self.path = path
        self.header = []
        self.events = defaultdict(list)   # EVENT -> [compact row dict]
        self.nrows = 0
        self.malformed = 0                # rows whose field count != header count
        self.errors = []                  # fatal / parse problems
        self.ticks = []                   # (tick, time_s) for tick-rate estimate


def load_csv(path):
    d = Data(path)
    try:
        fh = open(path, newline="", encoding="utf-8-sig", errors="replace")
    except OSError as e:
        d.errors.append("cannot open: %s" % e)
        return d
    with fh:
        rd = csv.reader(fh)
        header = None
        try:
            for row in rd:
                if not row or all(not c.strip() for c in row):
                    continue
                if header is None:
                    header = [h.strip() for h in row]
                    if header[0].lower() != "event":
                        d.errors.append("first row is not a header starting with 'event'")
                        return d
                    d.header = header
                    continue
                d.nrows += 1
                if len(row) != len(header):
                    d.malformed += 1
                r = {}
                for h, v in zip(header, row):
                    v = v.strip()
                    if v != "":
                        r[h] = v
                ev = row[0].strip().upper()
                if not ev:
                    continue
                d.events[ev].append(r)
                t, ts = num(r, "tick"), num(r, "time_s")
                if t is not None and ts is not None:
                    d.ticks.append((t, ts))
        except csv.Error as e:
            d.errors.append("CSV parse error after %d rows: %s" % (d.nrows, e))
    if header is None:
        d.errors.append("empty file (no header)")
    return d


def parse_config(row):
    for v in row.values():
        if "=" in v and ";" in v:
            out = OrderedDict()
            for part in v.split(";"):
                if "=" in part:
                    k, _, x = part.partition("=")
                    out[k.strip()] = x.strip()
            return out
    return OrderedDict()


def file_label(path):
    m = FILE_RE.search(path.name)
    if m:
        return "%s_m%s" % (m.group(2), m.group(4))
    return path.stem[-14:]


# ----------------------------------------------------------------------------
# Analysis
# ----------------------------------------------------------------------------
class Report:
    def __init__(self, path):
        self.path = path
        self.label = file_label(path)
        self.M = OrderedDict()       # compare metrics (preformatted strings)
        self.config = OrderedDict()  # first CONFIG key/values
        self.flags = []              # (severity, text)
        self.doc = None

    def flag(self, sev, text):
        self.flags.append((sev, text))


def analyze(path, min_shots, md):
    BAD_NUM.clear()
    rep = Report(path)
    doc = rep.doc = Doc(md)
    M = rep.M
    d = load_csv(path)
    doc.h(1, "File: %s" % path.name)
    if d.errors:
        for e in d.errors:
            rep.flag("WARN", e)
    if not d.header:
        doc.p("Could not analyze: " + "; ".join(d.errors))
        M["rows"] = "0"
        M["anomalies (WARN)"] = str(sum(1 for s, _ in rep.flags if s == "WARN"))
        return rep

    ev = d.events
    fire, probes, deliv, dmg = ev.get("FIRE", []), ev.get("PROBE", []), ev.get("DELIVER", []), ev.get("DAMAGE", [])
    hset = set(d.header)

    section_meta(rep, doc, d, hset)
    section_health(rep, doc, d, probes)
    section_network(rep, doc, fire, probes)
    section_hitreg(rep, doc, probes)
    section_deliver(rep, doc, deliv)
    section_damage(rep, doc, dmg, probes)
    section_funnel(rep, doc, fire, probes, deliv, dmg)
    section_deviation_idx(rep, doc, fire, probes, hset)
    section_deviation_components(rep, doc, fire, probes, hset)
    section_tables(rep, doc, fire, probes, dmg, min_shots)
    if BAD_NUM:
        rep.flag("INFO", "unparsable numeric values ignored: " +
                 ", ".join("%s=%d" % kv for kv in BAD_NUM.most_common(6)))
    section_flags(rep, doc)
    return rep


# --- 1. Meta ---------------------------------------------------------------
def section_meta(rep, doc, d, hset):
    M = rep.M
    m = FILE_RE.search(rep.path.name)
    doc.h(2, "1. Meta")
    if m:
        ts = m.group(1) + " " + m.group(2)
        doc.p("Recorded %s-%s-%s %s:%s:%s, map `%s`, match #%s" % (
            ts[0:4], ts[4:6], ts[6:8], ts[9:11], ts[11:13], ts[13:15], m.group(3), m.group(4)))
    else:
        doc.p("Filename does not match openbf2_hitreg_<date>_<time>_<map>_m<N>.csv; map/match unknown.")

    times = [t for _, t in d.ticks]
    ticks = [t for t, _ in d.ticks]
    dur = (max(times) - min(times)) if times else None
    rate = None
    if dur and dur > 0:
        rate = (max(ticks) - min(ticks)) / dur
    doc.p("Duration (time_s span): %s s (%s min); effective tick rate from tick/time_s: %s Hz" % (
        f(dur, 1), f(dur / 60.0 if dur else None, 1), f(rate, 2)))
    M["duration_s"] = f(dur, 0)
    M["tick rate Hz (tick/time_s)"] = f(rate, 2)

    counts = Counter({k: len(v) for k, v in d.events.items()})
    order = [e for e in ("CONFIG", "FIRE", "PROBE", "DELIVER", "DAMAGE") if e in counts]
    order += sorted(e for e in counts if e not in order)
    doc.table(["event", "rows"], [[e, counts[e]] for e in order] + [["total data rows", d.nrows]])
    for e in ("FIRE", "PROBE", "DELIVER", "DAMAGE"):
        M["rows " + e] = str(counts.get(e, 0))
    unknown = [e for e in counts if e not in ("CONFIG", "FIRE", "PROBE", "DELIVER", "DAMAGE")]
    if unknown:
        rep.flag("INFO", "unknown event types present: " + ", ".join(sorted(unknown)))

    cfgs = [(num(r, "time_s"), parse_config(r)) for r in d.events.get("CONFIG", [])]
    cfgs = [(t, c) for t, c in cfgs if c]
    if cfgs:
        rep.config = cfgs[0][1]
        doc.p("CONFIG (%d row%s):" % (len(cfgs), "" if len(cfgs) == 1 else "s"))
        doc.p("; ".join("%s=%s" % kv for kv in cfgs[0][1].items()) if not doc.md
              else "`" + "; ".join("%s=%s" % kv for kv in cfgs[0][1].items()) + "`")
        for t, c in cfgs[1:]:
            ch = ["%s: %s -> %s" % (k, cfgs[0][1].get(k, "?"), v)
                  for k, v in c.items() if cfgs[0][1].get(k) != v]
            doc.p("  config change at t=%s s: %s" % (f(t, 1), ", ".join(ch) or "(no key differences)"))
            rep.flag("INFO", "CONFIG changed mid-file at t=%s s (%s)" % (f(t, 1), ", ".join(ch)))
    else:
        doc.p("No CONFIG row found (settings unknown).")

    ncols = len(d.header)
    M["columns"] = str(ncols)
    doc.p("Columns: %d (this branch expects %d)" % (ncols, EXPECTED_COLS))
    if ncols != EXPECTED_COLS:
        note = "known variant from another branch" if ncols in KNOWN_COL_VARIANTS else "unknown layout"
        rep.flag("WARN", "column count %d != %d (%s); analyzing the columns that exist, looked up by name"
                 % (ncols, EXPECTED_COLS, note))
    missing = sorted({c for cols in KEY_COLUMNS.values() for c in cols if c not in hset})
    if missing:
        rep.flag("WARN", "columns missing from header (related metrics show n/a): " + ", ".join(missing))
    if d.malformed:
        rep.flag("WARN", "%d row(s) have a field count different from the header (truncated last line?)"
                 % d.malformed)


# --- 2. Server health ------------------------------------------------------
def section_health(rep, doc, d, probes):
    doc.h(2, "2. Server health (tick_us)")
    M = rep.M
    rows = [r for rs in d.events.values() for r in rs]
    p50, p99 = vals(rows, "tick_us_p50"), vals(rows, "tick_us_p99")
    if not p50 and not p99:
        doc.p("No tick_us samples (column missing or telemetry level < 2).")
        M["tick_us_p50 median"] = M["tick_us_p99 p95"] = M["tick over budget %"] = "n/a"
        return
    doc.p("Samples are rolling 60-tick (2 s) percentiles written on each PROBE row (not independent). "
          "tick_us seems to be the paced tick period, so ~%d us is normal; budget = %d us (30 Hz), "
          "'over' = more than budget + %d us." % (TICK_BUDGET_US, TICK_BUDGET_US, TICK_OVER_TOL_US))
    limit = TICK_BUDGET_US + TICK_OVER_TOL_US
    trs = []
    for name, v in (("tick_us_p50", p50), ("tick_us_p99", p99)):
        s = describe(v)
        over = sum(1 for x in v if x > limit)
        trs.append([name, s["n"] if s else 0, f(s and s["p50"], 0), f(s and s["p95"], 0),
                    f(s and s["p99"], 0), f(s and s["max"], 0), fp(over, len(v), 2)])
        if v:
            if name == "tick_us_p50":
                M["tick_us_p50 median"] = f(s["p50"], 0)
            else:
                M["tick_us_p99 p95"] = f(s["p95"], 0)
                M["tick_us_p99 max"] = f(s["max"], 0)
                M["tick over budget %"] = fp(over, len(v), 2)
                if pc(over, len(v)) > TICK_OVER_WARN_PCT:
                    rep.flag("WARN", "tick_us_p99 over budget (> %d us) in %s of samples, max %d us"
                             % (limit, fp(over, len(v), 1), s["max"]))
                elif s["max"] > limit:
                    rep.flag("INFO", "tick_us_p99 peaked at %d us (> %d) in %d sample(s)"
                             % (s["max"], limit, over))
    doc.table(["metric", "n", "p50", "p95", "p99", "max", "% over"], trs)


# --- 3. Network ------------------------------------------------------------
NET_COLS = ["ping_ms", "raw_ping_ms", "rtt_jitter_ms", "pkt_loss_dn_pct", "interp_ms", "raw_interp_ms",
            "q_depth", "q_delay_ms", "q_target", "q_drain_drops", "replay_cnt", "replays_30t"]


def section_network(rep, doc, fire, probes):
    doc.h(2, "3. Network / queue")
    M = rep.M
    doc.p("Per-shooter state at shot time. Source = FIRE rows when the column is present there, "
          "else PROBE rows. pkt_loss is a rolling ~64-packet window; q_drain_drops is the per-player "
          "queue-drain drop counter.")
    rows = []
    stats = {}
    for c in NET_COLS:
        src, v = "-", []
        for name, rs in (("FIRE", fire), ("PROBE", probes)):
            v = vals(rs, c)
            if v:
                src = name
                break
        s = describe(v)
        stats[c] = s
        rows.append([c, src, s["n"] if s else 0, f(s and s["mean"]), f(s and s["p50"]),
                     f(s and s["p95"]), f(s and s["p99"]), f(s and s["max"])])
    doc.table(["metric", "src", "n", "mean", "p50", "p95", "p99", "max"], rows)
    g = lambda c, k: stats[c][k] if stats.get(c) else None
    M["ping_ms p50"] = f(g("ping_ms", "p50"), 0)
    M["ping_ms p95"] = f(g("ping_ms", "p95"), 0)
    M["rtt_jitter p95"] = f(g("rtt_jitter_ms", "p95"))
    M["pkt_loss % p95"] = f(g("pkt_loss_dn_pct", "p95"))
    M["q_depth p95"] = f(g("q_depth", "p95"), 1)
    M["q_delay_ms p95"] = f(g("q_delay_ms", "p95"), 1)

    if (g("ping_ms", "p95") or 0) > PING_WARN_MS:
        rep.flag("WARN", "ping_ms p95 = %s ms (> %g)" % (f(g("ping_ms", "p95"), 0), PING_WARN_MS))
    if (g("rtt_jitter_ms", "p95") or 0) > JITTER_WARN_MS:
        rep.flag("WARN", "rtt_jitter_ms p95 = %s (> %g)" % (f(g("rtt_jitter_ms", "p95")), JITTER_WARN_MS))
    if (g("pkt_loss_dn_pct", "p95") or 0) > LOSS_WARN_PCT:
        rep.flag("WARN", "pkt_loss_dn_pct p95 = %s%% (> %g%%)" % (f(g("pkt_loss_dn_pct", "p95")), LOSS_WARN_PCT))
    if (g("q_depth", "p95") or 0) > Q_DEPTH_WARN:
        rep.flag("WARN", "q_depth p95 = %s (> %d)" % (f(g("q_depth", "p95"), 1), Q_DEPTH_WARN))

    # q_drain_drops: cumulative per shooter?
    per = defaultdict(list)
    for r in probes:
        v = num(r, "q_drain_drops")
        if v is not None:
            per[shooter_name(r)].append(v)
    if per:
        cumulative = all(all(b >= a for a, b in zip(s, s[1:])) for s in per.values())
        tot = sum(max(s) for s in per.values())
        affected = sum(1 for s in per.values() if max(s) > 0)
        doc.p("q_drain_drops: %s; shooters with drops > 0: %d/%d; %s %d" % (
            "monotonic per shooter (cumulative counter)" if cumulative else "NOT monotonic (per-window or reset)",
            affected, len(per), "sum of per-shooter max =" if cumulative else "sum of per-shooter max (indicative) =", tot))
        M["q_drain_drops (sum of max)"] = str(int(tot))
        if tot > 0:
            rep.flag("WARN", "q_drain_drops > 0: %d total across %d/%d shooters" % (tot, affected, len(per)))
    else:
        M["q_drain_drops (sum of max)"] = "n/a"


def shooter_name(r):
    n = r.get("shooter")
    return n if n else "id%s" % r.get("shooter_id", "?")


# --- 4. Hit registration ---------------------------------------------------
def b(r, c):
    v = num(r, c)
    return None if v is None else int(v != 0)


def bucketize(v, edges, labels):
    if v is None:
        return "n/a"
    for e, l in zip(edges, labels):
        if v < e:
            return l
    return labels[-1]


DIST_B = ([25, 50, 100], ["0-25 m", "25-50 m", "50-100 m", "100+ m"])
PING_B = ([50, 100, 150], ["<50 ms", "50-100 ms", "100-150 ms", "150+ ms"])
SPEED_B = ([0.5, 3, 6], ["still (<0.5)", "walk (0.5-3)", "run (3-6)", "fast (6+)"])


def section_hitreg(rep, doc, probes):
    doc.h(2, "4. Hit registration (PROBE: Active vs Vanilla)")
    M = rep.M
    if not probes:
        doc.p("No PROBE rows.")
        for k in ("act hit % (probes)", "van hit % (probes)", "act-only", "van-only", "disagree %",
                  "act_miss_m p50 (misses)"):
            M[k] = "n/a"
        return
    doc.p("PROBE = soldier hit or near-miss <= 2.0 m; hit rate = hits / probes (not per shot). "
          "act_hit = live hit-reg decision (rewound pose/bone history), van_hit = vanilla 1.50 shadow probe. "
          "*_miss_m = min surface distance (0 for hits, %g = no data, excluded). act_dt_err_ms is clamped to +-500 "
          "and only computed for projectile speed >= 2 m/s; its sign convention was not verified." % MISS_SENTINEL_M)

    both = act_only = van_only = neither = 0
    for r in probes:
        a, v = b(r, "act_hit"), b(r, "van_hit")
        if a is None or v is None:
            continue
        if a and v:
            both += 1
        elif a:
            act_only += 1
        elif v:
            van_only += 1
        else:
            neither += 1
    n = both + act_only + van_only + neither
    doc.table(["", "Vanilla hit", "Vanilla miss"],
              [["Active hit", both, act_only], ["Active miss", van_only, neither]])
    ah, vh = both + act_only, both + van_only
    dis = act_only + van_only
    doc.p("Probes with both act_hit/van_hit: %d of %d.  Active hit rate %s, Vanilla hit rate %s, disagreement %s "
          "(act-only %d, van-only %d)." % (n, len(probes), fp(ah, n), fp(vh, n), fp(dis, n), act_only, van_only))
    M["probes"] = str(len(probes))
    M["act hit % (probes)"] = fp(ah, n)
    M["van hit % (probes)"] = fp(vh, n)
    M["act-only"] = str(act_only)
    M["van-only"] = str(van_only)
    M["disagree %"] = fp(dis, n)
    if n and pc(dis, n) > DISAGREE_WARN_PCT:
        rep.flag("WARN", "Active vs Vanilla disagree on %s of probes (act-only %d, van-only %d)"
                 % (fp(dis, n), act_only, van_only))
    if van_only > act_only and n:
        rep.flag("INFO", "Vanilla registers more hits than Active (van-only %d > act-only %d)" % (van_only, act_only))

    fb = [r for r in probes if b(r, "flyby") == 0]
    if len(fb) != len(probes):
        a = [b(r, "act_hit") for r in fb if b(r, "act_hit") is not None]
        v = [b(r, "van_hit") for r in fb if b(r, "van_hit") is not None]
        doc.p("flyby==0 subset: %d probes, Active %s, Vanilla %s (flyby rows: %d)." % (
            len(fb), fp(sum(a), len(a)), fp(sum(v), len(v)), len(probes) - len(fb)))

    # head share
    def head(r, hit, bone, name=None):
        if b(r, hit) != 1:
            return None
        if num(r, bone) == HEAD_BONE:
            return True
        return bool(name and "head" in r.get(name, "").lower())
    for lab, hit, bone, name in (("Active", "act_hit", "act_bone", "act_bone_name"), ("Vanilla", "van_hit", "van_bone", None)):
        hs = [head(r, hit, bone, name) for r in probes]
        hs = [x for x in hs if x is not None]
        if hs:
            doc.p("%s hits on head (bone %d%s): %d of %d (%s)" % (
                lab, HEAD_BONE, " or name~head" if name else "", sum(hs), len(hs), fp(sum(hs), len(hs))))

    # miss distances / rewind / dt err
    act_miss = [x for x in vals([r for r in probes if b(r, "act_hit") == 0], "act_miss_m") if x < MISS_SENTINEL_M]
    van_miss = [x for x in vals([r for r in probes if b(r, "van_hit") == 0], "van_miss_m") if x < MISS_SENTINEL_M]
    dt_all = vals(probes, "act_dt_err_ms")
    dt_miss = vals([r for r in probes if b(r, "act_hit") == 0], "act_dt_err_ms")
    rows = []
    for lab, v in (("act_miss_m (act misses)", act_miss), ("van_miss_m (van misses)", van_miss),
                   ("act_rewind_ms", vals(probes, "act_rewind_ms")), ("van_rewind_ms", vals(probes, "van_rewind_ms")),
                   ("act_dt_err_ms (all)", dt_all), ("act_dt_err_ms (act misses)", dt_miss),
                   ("|act_dt_err_ms| (act misses)", [abs(x) for x in dt_miss])):
        s = describe(v)
        rows.append([lab, s["n"] if s else 0, f(s and s["mean"], 3), f(s and s["p50"], 3), f(s and s["p95"], 3),
                     f(s and s["p99"], 3), f(s and s["max"], 3)])
    doc.table(["metric", "n", "mean", "p50", "p95", "p99", "max"], rows)
    am = describe(act_miss)
    M["act_miss_m p50 (misses)"] = f(am and am["p50"], 3)
    rw = [(num(r, "act_rewind_ms"), num(r, "van_rewind_ms")) for r in probes]
    rw = [(a_, v_) for a_, v_ in rw if a_ is not None and v_ is not None]
    if rw:
        delta = sum(a_ - v_ for a_, v_ in rw) / len(rw)
        doc.p("Mean (act_rewind_ms - van_rewind_ms) = %s ms over %d probes." % (f(delta), len(rw)))
        M["mean act-van rewind ms"] = f(delta)

    ruf = vals(probes, "van_ring_unfilled")
    ruf2 = vals(probes, "ring_uf")
    bh = vals(probes, "act_bone_hist")
    items = []
    if ruf:
        items.append("van_ring_unfilled=1: %d of %d (%s)" % (sum(1 for x in ruf if x), len(ruf), fp(sum(1 for x in ruf if x), len(ruf))))
    if ruf2:
        k = sum(1 for x in ruf2 if x)
        items.append("ring_uf=1 (estimated underflow): %d of %d (%s)" % (k, len(ruf2), fp(k, len(ruf2))))
        M["ring_uf %"] = fp(k, len(ruf2))
        if pc(k, len(ruf2)) > RING_UF_WARN_PCT:
            rep.flag("WARN", "ring_uf=1 on %s of probes (> %g%%)" % (fp(k, len(ruf2)), RING_UF_WARN_PCT))
    if bh:
        k = sum(1 for x in bh if x)
        items.append("act_bone_hist=1 (bone history used): %d of %d (%s)" % (k, len(bh), fp(k, len(bh))))
    if items:
        doc.bullets(items)

    # breakdowns
    def breakdown(title, keyfn, order=None):
        groups = defaultdict(list)
        for r in probes:
            groups[keyfn(r)].append(r)
        keys = order if order else sorted(groups, key=lambda k: (k == "n/a", k))
        rows = []
        for k in keys:
            rs = groups.get(k)
            if not rs:
                continue
            a = [b(r, "act_hit") for r in rs]
            v = [b(r, "van_hit") for r in rs]
            pairs = [(x, y) for x, y in zip(a, v) if x is not None and y is not None]
            ao = sum(1 for x, y in pairs if x and not y)
            vo = sum(1 for x, y in pairs if y and not x)
            am_ = [x for x in vals([r for r in rs if b(r, "act_hit") == 0], "act_miss_m") if x < MISS_SENTINEL_M]
            rows.append([k, len(rs), fp(sum(1 for x, _ in pairs if x), len(pairs)),
                         fp(sum(1 for _, y in pairs if y), len(pairs)), ao, vo,
                         f(sum(am_) / len(am_), 3) if am_ else "n/a"])
        if rows:
            doc.p("By " + title + ":")
            doc.table([title, "probes", "act hit", "van hit", "act-only", "van-only", "avg act miss m"], rows)

    def with_na(labels):
        return labels + ["n/a"]
    breakdown("distance", lambda r: bucketize(num(r, "shot_dist_m"), *DIST_B), with_na(DIST_B[1]))
    breakdown("ping", lambda r: bucketize(num(r, "ping_ms"), *PING_B), with_na(PING_B[1]))
    breakdown("target pose (tgt_act_pose)",
              lambda r: "pose %d" % as_int(r["tgt_act_pose"]) if as_int(r.get("tgt_act_pose")) is not None else "n/a")
    breakdown("target speed", lambda r: bucketize(num(r, "tgt_speed"), *SPEED_B), with_na(SPEED_B[1]))
    mism = sum(1 for r in probes
               if as_int(r.get("tgt_act_pose")) is not None and as_int(r.get("tgt_live_pose")) is not None
               and as_int(r["tgt_act_pose"]) != as_int(r["tgt_live_pose"]))
    if any("tgt_live_pose" in r for r in probes):
        doc.p("Pose enum is logged numerically (probably 0=stand, 1=crouch, 2=prone; not verified). "
              "tgt_act_pose != tgt_live_pose on %d probes (%s) - pose differs between rewound and live state."
              % (mism, fp(mism, len(probes))))


# --- 5. Delivery -----------------------------------------------------------
def section_deliver(rep, doc, deliv):
    doc.h(2, "5. Delivery (DELIVER)")
    M = rep.M
    if not deliv:
        doc.p("No DELIVER rows.")
        M["DELIVERED %"] = M["OVERRIDE %"] = "n/a"
        return
    oc = Counter(r.get("outcome", "?") for r in deliv)
    doc.table(["outcome", "count", "%"], [[k, v, fp(v, len(deliv))] for k, v in oc.most_common()])
    ov = sum(v for k, v in oc.items() if k.startswith("OVERRIDE"))
    M["DELIVERED %"] = fp(oc.get("DELIVERED", 0), len(deliv))
    M["OVERRIDE %"] = fp(ov, len(deliv))
    if pc(ov, len(deliv)) > OVERRIDE_WARN_PCT:
        rep.flag("WARN", "OVERRIDE_* is %s of DELIVER rows (%d of %d): hits that the active probe saw were taken by "
                 "terrain/wall/another soldier" % (fp(ov, len(deliv)), ov, len(deliv)))
    ovr = [r for r in deliv if r.get("outcome", "").startswith("OVERRIDE")]
    if ovr:
        bl = Counter("%s [%s]" % (r.get("blocker", "?"), r.get("obj_class", "?")) for r in ovr)
        doc.p("Top blockers on OVERRIDE rows (blocker [obj_class]):")
        doc.table(["blocker", "count"], [[k, v] for k, v in bl.most_common(8)])
        mt = Counter(r.get("blocker_mat", "?") for r in ovr)
        doc.p("Top blocker_mat on OVERRIDE rows (material id): " +
              ", ".join("%s x%d" % kv for kv in mt.most_common(6)))
        mg = describe(vals(ovr, "override_margin_m"))
        if mg:
            doc.p("override_margin_m (winner dist - soldier dist): p50 %s, p95 %s, max %s m" % (
                f(mg["p50"], 3), f(mg["p95"], 3), f(mg["max"], 3)))
        ec = Counter(r.get("eye_clear", "?") for r in ovr)
        doc.p("eye_clear on OVERRIDE rows (1 = shooter-eye line clear, 0 = blocked, -1 = n/a): " +
              ", ".join("%s: %d" % kv for kv in sorted(ec.items())))
    mats = Counter(r.get("blocker_mat", "?") for r in deliv)
    doc.p("Contact material ids (blocker_mat column, all DELIVER rows): " +
          ", ".join("%s x%d" % kv for kv in mats.most_common(6)))


# --- 6. Damage -------------------------------------------------------------
def section_damage(rep, doc, dmg, probes):
    doc.h(2, "6. Damage (DAMAGE)")
    M = rep.M
    if not dmg:
        doc.p("No DAMAGE rows.")
        M["damage rows"] = "0"
        M["kills"] = M["avg final_dmg"] = "n/a"
        return
    fin = vals(dmg, "final_dmg")
    pos = [x for x in fin if x > 0]
    kills = sum(1 for r in dmg if b(r, "killed") == 1)
    hm = sum(1 for r in dmg if b(r, "hitmarker") == 1)
    non_sold = sum(1 for r in dmg if as_int(r.get("tgt_id")) is None or as_int(r.get("tgt_id")) < 0)
    doc.p("DAMAGE rows: %d; kills: %d; hitmarkers: %d; rows with final_dmg > 0: %d; "
          "rows on non-soldier / unknown target (tgt_id < 0): %d" % (len(dmg), kills, hm, len(pos), non_sold))
    rows = []
    for c in ("base_dmg", "mat_mod", "angle_mod", "final_dmg", "hp_before", "hp_after"):
        v = vals(dmg, c)
        if c == "final_dmg":
            v = pos
        s = describe(v)
        rows.append([c + (" (>0)" if c == "final_dmg" else ""), s["n"] if s else 0, f(s and s["mean"], 3),
                     f(s and s["p50"], 3), f(s and s["p95"], 3), f(s and s["max"], 3)])
    doc.table(["field", "n", "mean", "median", "p95", "max"], rows)
    M["damage rows"] = str(len(dmg))
    M["kills"] = str(kills)
    M["avg final_dmg"] = f(sum(pos) / len(pos) if pos else None, 2)

    # bone parts of damaging hits (join DAMAGE -> PROBE act_hit=1 by shot_id)
    hit_probe = {}
    for r in probes:
        if b(r, "act_hit") == 1 and shot_key(r):
            hit_probe.setdefault(shot_key(r), r)
    bones = Counter()
    heads = 0
    joined = 0
    for r in dmg:
        p = hit_probe.get(shot_key(r))
        if not p:
            continue
        joined += 1
        name = p.get("act_bone_name") or ("bone%s" % p.get("act_bone", "?"))
        bones[name] += 1
        if num(p, "act_bone") == HEAD_BONE or "head" in name.lower():
            heads += 1
    if joined:
        doc.p("Bone hit by damaging shots (joined to act_hit probe by shot_id, %d of %d DAMAGE rows): head %s" % (
            joined, len(dmg), fp(heads, joined)))
        doc.table(["act_bone_name", "damage rows", "%"], [[k, v, fp(v, joined)] for k, v in bones.most_common(10)])
        M["head share of damage"] = fp(heads, joined)
    else:
        doc.p("No DAMAGE row could be joined to an act_hit=1 probe (no bone info).")


# --- 7. Funnel -------------------------------------------------------------
def section_funnel(rep, doc, fire, probes, deliv, dmg):
    doc.h(2, "7. Shot funnel (by shot_id)")
    M = rep.M
    ids = lambda rs: {shot_key(r) for r in rs if shot_key(r)}
    fire_ids, probe_ids = ids(fire), ids(probes)
    act_ids = ids([r for r in probes if b(r, "act_hit") == 1])
    deliv_ids = ids(deliv)
    delivered_ids = ids([r for r in deliv if r.get("outcome") == "DELIVERED"])
    dmg_ids = ids(dmg)
    dmg_sold = {shot_key(r) for r in dmg if shot_key(r) and (as_int(r.get("tgt_id")) or -1) >= 0}
    base = fire_ids or (probe_ids | deliv_ids | dmg_ids)
    if not fire_ids:
        doc.p("No FIRE rows (telemetry level < 2?): funnel base is the union of ids seen in other events.")
    steps = [("FIRE (shots)", base), ("any PROBE", probe_ids & base if fire_ids else probe_ids),
             ("PROBE act_hit=1", act_ids), ("DELIVER (any)", deliv_ids),
             ("DELIVER = DELIVERED", delivered_ids), ("DAMAGE (any)", dmg_ids),
             ("DAMAGE on soldier", dmg_sold)]
    n0 = len(base)
    doc.table(["stage", "shots", "% of FIRE"], [[k, len(v), fp(len(v), n0)] for k, v in steps])
    orphans = [(n, len(s - fire_ids)) for n, s in (("PROBE", probe_ids), ("DELIVER", deliv_ids), ("DAMAGE", dmg_ids))]
    if fire_ids and any(o for _, o in orphans):
        doc.p("shot_ids not present in FIRE: " + ", ".join("%s %d" % x for x in orphans if x[1]) +
              " (e.g. FIRE row missing/other ids)")
    M["shots (FIRE)"] = str(len(fire_ids) if fire_ids else "n/a")

    # lost hits
    lost = sorted(act_ids - dmg_ids, key=lambda s: as_int(s) or 0)
    probe_hit_first = {}
    for r in probes:
        if b(r, "act_hit") == 1 and shot_key(r) and shot_key(r) not in probe_hit_first:
            probe_hit_first[shot_key(r)] = r
    deliv_by = defaultdict(list)
    for r in deliv:
        deliv_by[shot_key(r)].append(r.get("outcome", "?"))
    cats = Counter()
    cat_of = {}
    for s in lost:
        p = probe_hit_first[s]
        outs = deliv_by.get(s, [])
        if b(p, "tgt_alive") == 0 or b(p, "tgt_mandown") == 1:
            c = "target dead/mandown at probe"
        elif any(o.startswith("OVERRIDE") for o in outs) and "DELIVERED" not in outs:
            c = "overridden (" + "/".join(sorted({o for o in outs if o.startswith("OVERRIDE")})) + ")"
        elif "DELIVERED" in outs:
            c = "DELIVERED but no DAMAGE row (0 dmg / friendly?)"
        else:
            c = "no DELIVER row (projectile not delivered)"
        cats[c] += 1
        cat_of[s] = c
    doc.p("Lost hits (act_hit=1 probe, no DAMAGE row): %d of %d act-hit shots (%s)" % (
        len(lost), len(act_ids), fp(len(lost), len(act_ids))))
    M["lost hits (act-hit w/o DAMAGE)"] = "%d (%s)" % (len(lost), fp(len(lost), len(act_ids)))
    if lost:
        doc.table(["lost-hit reason", "shots"], [[k, v] for k, v in cats.most_common()])
        ex = []
        for s in lost[:5]:
            p = probe_hit_first[s]
            ex.append("shot_id %s tick %s %s: %s -> %s, bone=%s, ping=%s ms [%s]" % (
                s, p.get("tick", "?"), p.get("weapon", "?"), shooter_name(p), p.get("tgt", "?"),
                p.get("act_bone_name", "?"), p.get("ping_ms", "?"), cat_of[s]))
        doc.p("Examples:")
        doc.bullets(ex)
        if pc(len(lost), len(act_ids)) > LOST_HIT_WARN_PCT:
            rep.flag("WARN", "lost hits: %d act-hit shots (%s) never produced DAMAGE; e.g. shot_ids %s" % (
                len(lost), fp(len(lost), len(act_ids)), ", ".join(lost[:5])))

    # phantom damage
    probes_by = defaultdict(list)
    for r in probes:
        probes_by[shot_key(r)].append(r)
    phantom = sorted(dmg_sold - act_ids, key=lambda s: as_int(s) or 0)
    dmg_first = {}
    for r in dmg:
        if shot_key(r) and shot_key(r) not in dmg_first:
            dmg_first[shot_key(r)] = r
    no_probe = [s for s in phantom if s not in probes_by]
    doc.p("Phantom damage (DAMAGE on soldier without an act_hit=1 probe): %d of %d damaging shots (%s); "
          "of these %d have no PROBE row at all, %d have only act_hit=0 probes." % (
              len(phantom), len(dmg_sold), fp(len(phantom), len(dmg_sold)), len(no_probe),
              len(phantom) - len(no_probe)))
    M["phantom damage"] = "%d (%s)" % (len(phantom), fp(len(phantom), len(dmg_sold)))
    if phantom:
        ex = []
        for s in phantom[:5]:
            r = dmg_first[s]
            ex.append("shot_id %s tick %s %s: %s -> %s, final_dmg=%s, killed=%s, probes=%d" % (
                s, r.get("tick", "?"), r.get("weapon", "?"), shooter_name(r), r.get("tgt", "?"),
                r.get("final_dmg", "?"), r.get("killed", "?"), len(probes_by.get(s, []))))
        doc.p("Examples:")
        doc.bullets(ex)
        if pc(len(phantom), len(dmg_sold)) > PHANTOM_WARN_PCT:
            rep.flag("WARN", "phantom damage: %d damaging shots (%s) had no act_hit=1 probe; e.g. shot_ids %s" % (
                len(phantom), fp(len(phantom), len(dmg_sold)), ", ".join(phantom[:5])))


# --- 8. Per weapon / per shooter ------------------------------------------
def section_deviation_idx(rep, doc, fire, probes, hset):
    """Server fire_input_idx vs client action act_input_idx (needs the 9 appended FIRE columns)."""
    doc.h(2, "7b. Deviation input-index match (fire_input_idx vs act_input_idx)")
    need = ("fire_input_idx", "act_input_idx", "act_src")
    if not all(c in hset for c in need):
        doc.p("Columns %s not present in this CSV (older format); section skipped." % ", ".join(need))
        return
    # shot_id -> (any aim_hit==1, any act_hit==1) over PROBE rows
    aim, act = {}, {}
    for r in probes:
        k = shot_key(r)
        if not k:
            continue
        if b(r, "aim_hit") == 1:
            aim[k] = True
        if b(r, "act_hit") == 1:
            act[k] = True
    per = defaultdict(lambda: {"match": [0, 0, 0, 0], "mismatch": [0, 0, 0, 0], "replay": [0, 0, 0, 0], "unknown": 0})
    for r in fire:
        fi, ai, src = num(r, "fire_input_idx"), num(r, "act_input_idx"), num(r, "act_src")
        who = shooter_name(r)
        if fi is None:
            per[who]["unknown"] += 1
            continue
        if src == 2:
            grp = "replay"
        elif ai is None:
            per[who]["unknown"] += 1
            continue
        else:
            grp = "match" if int(fi) == int(ai) else "mismatch"
        k = shot_key(r)
        c = per[who][grp]
        c[0] += 1                                   # shots
        c[1] += 1 if k in act else 0                # act hit
        c[2] += 1 if k in aim else 0                # aim hit
        c[3] += 1 if (k in aim and k not in act) else 0   # aim hit, no act hit
    rows = []
    tot = {"match": [0, 0, 0, 0], "mismatch": [0, 0, 0, 0], "replay": [0, 0, 0, 0]}
    for who in sorted(per):
        d = per[who]
        for g in tot:
            for i in range(4):
                tot[g][i] += d[g][i]
        n = d["match"][0] + d["mismatch"][0] + d["replay"][0]
        rows.append(_dev_row(who, d, n))
    if not rows:
        doc.p("No FIRE rows with fire_input_idx.")
        return
    tot["unknown"] = sum(per[w]["unknown"] for w in per)
    rows.append(_dev_row("ALL", tot, sum(tot[g][0] for g in ("match", "mismatch", "replay"))))
    doc.p("mismatch = act_src==1 (fresh queued client action) and fire_input_idx != act_input_idx; "
          "replay = server re-ran the previous action (no client index, act_src==2).  "
          "hit = shot has an act_hit=1 PROBE; aim-but-miss = shot has aim_hit=1 PROBE but no act_hit=1 PROBE.")
    doc.table(["shooter", "shots", "mismatch", "mismatch%", "replay", "unknown",
               "hit% match", "hit% mismatch", "aimMiss% match", "aimMiss% mismatch"], rows)
    m = tot["mismatch"][0]
    allfresh = tot["match"][0] + m
    if allfresh and m * 100.0 / allfresh >= 5.0:
        rep.flag("WARN", "deviation input index differs from the client action index on %d of %d fresh-action shots (%s)"
                 % (m, allfresh, fp(m, allfresh)))


# --- 7c. Deviation component breakdown --------------------------------------
DEV_COMP_COLS = ("dev_mag", "dev_min", "dev_speed", "dev_turn", "dev_misc", "dev_fire",
                 "dev_pose_mod", "dev_zoom_mod", "sh_pose", "sh_zoomed", "sh_speed_ms",
                 "sh_turn_rate")


def section_deviation_components(rep, doc, fire, probes, hset):
    """Median deviation terms per shooter x PROBE category (FIRE<->PROBE joined on shot_id)."""
    doc.h(2, "7c. Deviation components at fire time (median per shooter x probe category)")
    if not all(c in hset for c in DEV_COMP_COLS):
        doc.p("Columns dev_min..sh_turn_rate not present in this CSV (older format); section skipped.")
        return
    rank = {"act_hit": 3, "aim_hit&!act_hit": 2, "aim_miss": 1}
    cat = {}   # shot_id -> best category: act_hit > aim_hit&!act_hit > aim_miss
    for r in probes:
        k = shot_key(r)
        if not k:
            continue
        if b(r, "act_hit") == 1:
            c = "act_hit"
        elif b(r, "aim_hit") == 1:
            c = "aim_hit&!act_hit"
        elif b(r, "aim_hit") == 0:
            c = "aim_miss"
        else:
            continue
        if k not in cat or rank[c] > rank[cat[k]]:
            cat[k] = c
    groups = defaultdict(list)   # (shooter, cat) -> FIRE rows
    for r in fire:
        k = shot_key(r)
        if k in cat and num(r, "dev_min") is not None:
            groups[(shooter_name(r), cat[k])].append(r)
    if not groups:
        doc.p("No FIRE rows with deviation components joined to PROBE rows.")
        return

    def med(rows, col):
        v = sorted(vals(rows, col))
        return f(pctile(v, 50), 3) if v else "n/a"
    out = []
    for (who, c) in sorted(groups, key=lambda x: (x[0], -rank[x[1]])):
        rows = groups[(who, c)]
        out.append([who, c, len(rows)] + [med(rows, col) for col in DEV_COMP_COLS])
    doc.p("dev_mag = deviation used by postFire; dev_min/speed/turn/misc/fire = SoldierDeviationComp terms "
          "(fire = accumulated before this shot's add); pose_mod/zoom_mod = multipliers; "
          "dev_mag ~= (min+speed+turn+misc+fire)*pose_mod*zoom_mod.  sh_pose 0=stand 1=crouch 2=prone; "
          "sh_speed_ms = server soldier speed; sh_turn_rate = rad/s.  Per-shot category = best PROBE "
          "(act_hit > aim_hit without act_hit > aim miss).")
    doc.table(["shooter", "probe", "shots"] + list(DEV_COMP_COLS), out)


def _dev_row(who, d, n):
    def hit(g):
        return fp(d[g][1], d[g][0]) if d[g][0] else "-"

    def aimmiss(g):
        return fp(d[g][3], d[g][2]) if d[g][2] else "-"
    mm = d["mismatch"][0]
    fresh = d["match"][0] + mm
    return [who, n, mm, fp(mm, fresh) if fresh else "-", d["replay"][0], d.get("unknown", 0),
            hit("match"), hit("mismatch"), aimmiss("match"), aimmiss("mismatch")]


def section_tables(rep, doc, fire, probes, dmg, min_shots):
    doc.h(2, "8. Per weapon / per shooter (>= %d shots)" % min_shots)

    def build(keyfn):
        acc = defaultdict(lambda: {"ids": set(), "fire": 0, "probes": 0, "ah": 0, "an": 0, "vh": 0, "vn": 0,
                                   "ping": [], "pingsrc": None, "dmg": 0, "kills": 0})
        for r in fire:
            a = acc[keyfn(r)]
            a["fire"] += 1
            v = num(r, "ping_ms")
            if v is not None:
                a["ping"].append(v)
        for r in probes:
            a = acc[keyfn(r)]
            a["probes"] += 1
            ah, vh = b(r, "act_hit"), b(r, "van_hit")
            if ah is not None:
                a["an"] += 1
                a["ah"] += ah
            if vh is not None:
                a["vn"] += 1
                a["vh"] += vh
            a["ids"].add(shot_key(r))
        for r in dmg:
            a = acc[keyfn(r)]
            a["dmg"] += 1
            a["kills"] += 1 if b(r, "killed") == 1 else 0
        return acc

    def show(title, acc):
        # without FIRE rows, "shots" = distinct shot_ids that produced a probe
        rows = []
        for k, a in acc.items():
            shots = a["fire"] if a["fire"] else len(a["ids"])
            if shots < min_shots:
                continue
            ping = sum(a["ping"]) / len(a["ping"]) if a["ping"] else None
            rows.append((shots, [k, shots, a["probes"], fp(a["ah"], a["an"]), fp(a["vh"], a["vn"]),
                                 f(ping, 0), a["dmg"], a["kills"]]))
        rows.sort(key=lambda x: -x[0])
        doc.p("%s: %d with >= %d shots (showing up to 40)" % (title, len(rows), min_shots))
        if rows:
            doc.table(["key", "shots", "probes", "act hit %", "van hit %",
                       "avg ping", "dmg rows", "kills"], [r for _, r in rows[:40]])

    show("Per weapon", build(lambda r: r.get("weapon", "?")))
    show("Per shooter", build(shooter_name))
    doc.p("(avg ping = mean ping_ms on FIRE rows; hit % are over PROBE rows incl. near-misses.)")


# --- 9. Flags --------------------------------------------------------------
def section_flags(rep, doc):
    doc.h(2, "9. Anomalies / flags")
    if not rep.flags:
        doc.p("None above thresholds.")
    else:
        doc.bullets(["[%s] %s" % fl for fl in rep.flags])
    doc.p("Thresholds: tick over budget > %d us for > %g%% of samples; loss p95 > %g%%; ping p95 > %g ms; jitter p95 > %g ms; "
          "disagreement > %g%%; lost hits > %g%%; phantom > %g%%; OVERRIDE > %g%%; ring_uf > %g%%; q_depth p95 > %d." % (
              TICK_BUDGET_US + TICK_OVER_TOL_US, TICK_OVER_WARN_PCT, LOSS_WARN_PCT, PING_WARN_MS, JITTER_WARN_MS,
              DISAGREE_WARN_PCT, LOST_HIT_WARN_PCT, PHANTOM_WARN_PCT, OVERRIDE_WARN_PCT, RING_UF_WARN_PCT, Q_DEPTH_WARN))
    rep.M["anomalies (WARN)"] = str(sum(1 for s, _ in rep.flags if s == "WARN"))


# ----------------------------------------------------------------------------
# Compare + CLI
# ----------------------------------------------------------------------------
def compare_doc(reps, md):
    doc = Doc(md)
    doc.h(1, "Comparison (%d files)" % len(reps))
    labels, seen = [], Counter()
    for r in reps:
        l = r.label
        seen[l] += 1
        labels.append(l if seen[l] == 1 else "%s#%d" % (l, seen[l]))
    keys = []
    for r in reps:
        for k in r.M:
            if k not in keys:
                keys.append(k)
    rows = [[k] + [r.M.get(k, "n/a") for r in reps] for k in keys]
    cfg_keys = []
    for r in reps:
        for k in r.config:
            if k not in cfg_keys:
                cfg_keys.append(k)
    diff = [k for k in cfg_keys if len({r.config.get(k, "?") for r in reps}) > 1]
    rows += [["cfg:" + k] + [r.config.get(k, "?") for r in reps] for k in diff]
    doc.table(["metric"] + labels, rows)
    if not diff:
        doc.p("CONFIG identical across files (or no CONFIG rows).")
    doc.p("Files: " + "; ".join("%s = %s" % (l, r.path.name) for l, r in zip(labels, reps)))
    return doc


def collect(paths):
    files, bad = [], False
    for s in paths:
        p = Path(s)
        if p.is_dir():
            found = sorted(p.glob(FILE_GLOB))
            if not found:
                print("warning: no %s in %s" % (FILE_GLOB, p), file=sys.stderr)
            files += found
        elif p.is_file():
            files.append(p)
        else:
            print("error: path not found: %s" % p, file=sys.stderr)
            bad = True
    return files, bad


def main(argv=None):
    ap = argparse.ArgumentParser(description="Analyze OpenBF2 hitreg telemetry CSV files.")
    ap.add_argument("paths", nargs="+", help="CSV file(s) or directories containing %s" % FILE_GLOB)
    ap.add_argument("--out", help="write a Markdown report to this file (default: text to stdout)")
    ap.add_argument("--markdown", action="store_true", help="print Markdown to stdout instead of text")
    ap.add_argument("--compare", action="store_true", help="side-by-side key metrics when several files")
    ap.add_argument("--min-shots", type=int, default=20,
                    help="min shots for per-weapon / per-shooter rows (default 20)")
    args = ap.parse_args(argv)

    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(errors="replace")
        except Exception:
            pass

    files, bad = collect(args.paths)
    if not files:
        print("error: no input files", file=sys.stderr)
        return 2
    md = bool(args.out) or args.markdown
    reps = []
    for p in files:
        try:
            reps.append(analyze(p, args.min_shots, md))
        except Exception as e:  # keep going with other files
            print("error: failed to analyze %s: %s: %s" % (p, type(e).__name__, e), file=sys.stderr)
            bad = True
    parts = []
    if args.compare:
        if len(reps) > 1:
            parts.append(compare_doc(reps, md).render())
        else:
            print("note: --compare needs at least 2 files; ignored", file=sys.stderr)
    parts += [r.doc.render() for r in reps]
    text = "\n".join(parts)
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
        print("wrote %s (%d file(s) analyzed)" % (args.out, len(reps)))
    else:
        sys.stdout.write(text)
    return 1 if bad and not reps else 0


if __name__ == "__main__":
    sys.exit(main())
