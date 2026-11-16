#!/usr/bin/env python3
"""Validation harness for passive learning (docs/passive-learning.md, section 4).

Cuts full discharge sessions into realistic play stints, rebuilds the curve with passive.py
and compares it with build_curve.py's curve from the full sessions.

Usage:
  validate.py [--sessions ../../sessions] [--seeds 200] [--quick] [--only NAME]

Prints Markdown tables (the ones in docs/passive-learning.md). Python 3, standard library only.

AI disclosure: written with the help of AI (Claude Code).
"""

import argparse
import math
import os
import random
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import passive as P  # noqa: E402
bc = P.bc
GRID = P.GRID


# ---------------------------------------------------------------- data

class Full:
    """A full session: rows (t_ms, pct) and its reference real % at any time."""

    def __init__(self, path):
        s = bc.Session(path)
        self.number = s.number
        self.rows = [(t, p) for t, p, _v in s.rows]
        self.T = self.rows[-1][0]
        s.end()
        self.session = s

    def index_at(self, t):
        lo, hi = 0, len(self.rows) - 1
        while lo < hi:
            mid = (lo + hi) // 2
            if self.rows[mid][0] < t:
                lo = mid + 1
            else:
                hi = mid
        return lo


def reference(fulls):
    return bc.build([f.session for f in fulls])["mcu"][0]


# ---------------------------------------------------------------- piece generator

class Scenario:
    def __init__(self, name, **kw):
        self.name = name
        self.seg_min = kw.get("seg_min", (30, 90))      # wall minutes per stint
        self.gap_min = kw.get("gap_min", (2, 30))       # session minutes lost per sleep / off gap
        self.p_flat = kw.get("p_flat", 0.2)             # cycle runs until shutdown
        self.p_led = kw.get("p_led", 0.4)               # cycle stops at real 10 % (red LED)
        self.stop_mcu = kw.get("stop_mcu", (5, 40))     # otherwise: charge at this MCU %
        self.p_topup = kw.get("p_topup", 0.3)           # next cycle starts after a partial charge
        self.topup_mcu = kw.get("topup_mcu", (50, 90))
        self.loads = kw.get("loads", (1.0,))            # time stretch per stint (1.3 = lighter load)
        self.p_change = kw.get("p_change", 0.0)         # load changes half way through the stint
        self.relax = kw.get("relax", 0.0)               # MCU % reads this much high at stint start ...
        self.relax_tau = kw.get("relax_tau", 5.0)       # ... decaying with this time constant (min)
        self.never_low = kw.get("never_low", False)     # never below MCU stop_mcu (no flat, no LED stop)
        self.load_by_mcu = kw.get("load_by_mcu", None)  # f(MCU % at stint start) -> load (correlated load)


def make_pieces(fulls, hours, sc, rng, trim):
    """Segments totalling about `hours` of recorded play."""
    segs = []
    total = 0.0
    while total < hours * 60.0:
        f = rng.choice(fulls)
        # where does this battery cycle start?
        if segs and rng.random() < sc.p_topup:
            target = rng.uniform(*sc.topup_mcu)
            pos = f.rows[next(i for i, r in enumerate(f.rows) if r[1] <= target)][0]
        else:
            pos = 0.0
        # where does it end?
        u = rng.random()
        if sc.never_low:
            stop_t = first_time_below(f, rng.uniform(*sc.stop_mcu))
        elif u < sc.p_flat:
            stop_t = f.T
        elif u < sc.p_flat + sc.p_led:
            stop_t = 0.9 * f.T                 # real 10 % by the session's own time scale
        else:
            stop_t = first_time_below(f, rng.uniform(*sc.stop_mcu))
        while pos < stop_t - 60000 and total < hours * 60.0:
            k = rng.choice(sc.loads)
            if sc.load_by_mcu:
                k = sc.load_by_mcu(f.rows[f.index_at(pos)][1])
            wall = rng.uniform(*sc.seg_min)
            span = wall / k * 60000.0          # session time this stint consumes
            end = min(stop_t, pos + span)
            k2 = rng.choice(sc.loads) if rng.random() < sc.p_change else k
            rows = cut(f, pos, end, k, k2, sc)
            shutdown = end >= f.T
            s = P.segment_from_rows(rows, shutdown=shutdown, trim_min=trim, tag="%04d" % f.number)
            if s:
                segs.append(s)
            total += (rows[-1][0] - rows[0][0]) / 60000.0 if rows else 0.0
            pos = end + rng.uniform(*sc.gap_min) * 60000.0
    return segs


def first_time_below(f, mcu):
    for t, p in f.rows:
        if p <= mcu:
            return t
    return f.T


def cut(f, a, b, k, k2, sc):
    """Rows of f between session times a and b, with time stretched by k (k2 after half way)
    and an optional relaxation offset on the MCU % at the start."""
    i, j = f.index_at(a), f.index_at(b)
    src = f.rows[i:j + 1]
    if len(src) < 2:
        return []
    t0 = src[0][0]
    mid = t0 + (src[-1][0] - t0) / 2.0
    out = []
    for t, p in src:
        if t <= mid:
            tw = (t - t0) * k
        else:
            tw = (mid - t0) * k + (t - mid) * k2
        if sc.relax:
            p = min(100.0, p + sc.relax * math.exp(-tw / 60000.0 / sc.relax_tau))
        out.append((tw, p))
    # the recorder only sees the MCU % move down after a jump (first-crossing rule handles it)
    return out


# ---------------------------------------------------------------- metrics

REGIONS = [("1-5", 10, 50), ("5-20", 50, 200), ("20-50", 200, 500), ("50-98", 500, 980)]
THRESH = [70, 40, 10, 3]


IDENT = P.prior_identity()


def thresholds_mcu(ref):
    """MCU % (x10, float) where the reference curve crosses each real threshold."""
    out = {}
    for th in THRESH:
        x = th * 100
        for i in range(1, len(GRID)):
            if ref[i - 1] <= x <= ref[i] and ref[i] > ref[i - 1]:
                out[th] = GRID[i - 1] + (GRID[i] - GRID[i - 1]) * (x - ref[i - 1]) / (ref[i] - ref[i - 1])
                break
    return out


def errors(est, ref, th_mcu):
    e = {}
    for name, lo, hi in REGIONS:
        diffs = [abs(est[i] - ref[i]) / 100.0 for i, g in enumerate(GRID) if lo <= g <= hi]
        e["max " + name] = max(diffs)
        e["mean " + name] = sum(diffs) / len(diffs)
    e["max all"] = max(e["max " + n] for n, _, _ in REGIONS)
    for th, x in th_mcu.items():
        e["@%d" % th] = abs(bc.interp(GRID, est, x) - bc.interp(GRID, ref, x))
    return e


def q(vals, p):
    s = sorted(vals)
    return s[min(len(s) - 1, int(p * len(s)))]


def summarise(rows, keys):
    out = {}
    for k in keys:
        v = [r[k] for r in rows]
        out[k] = (statistics.median(v), q(v, 0.9))
    return out


# ---------------------------------------------------------------- experiments

def run(fulls_pieces, prior_d0, ref, sc, hours, seeds, fitkw, trim=P.DEFAULTS["trim_min"], seed0=0, extra=None,
        fitfn=None, curve_prior=None):
    th = thresholds_mcu(ref)
    rows, stats = [], {"segs": [], "dropped": [], "flat": []}
    for k in range(seeds):
        rng = random.Random(seed0 + k * 7919 + int(hours * 1000))
        segs = make_pieces(fulls_pieces, hours, sc, rng, trim)
        if extra:
            segs += extra(rng)
        res = (fitfn or P.fit)(segs, prior_d0, **fitkw)
        est = P.curve_x100(res["d"])
        e = errors(est, ref, th)
        if "nb" in res:
            e["gate"] = 1.0 if P.gate(res, prior_is_curve=curve_prior if curve_prior is not None
                                      else prior_d0 is not IDENT)[0] else 0.0
        rows.append(e)
        stats["segs"].append(len(segs))
        stats["dropped"].append(len(segs) - len(res["segs"]))
        stats["flat"].append(sum(1 for s in res["segs"] if s.shutdown))
    return rows, stats


KEYS = ["max all", "mean 1-5", "max 1-5", "mean 5-20", "mean 20-50", "mean 50-98", "@70", "@40", "@10", "@3"]


def table(title, results, keys=KEYS, note="", gate_col=True):
    print("\n### %s\n" % title)
    if note:
        print(note + "\n")
    print("| case | " + " | ".join(keys) + (" | gate ok |" if gate_col else " |"))
    print("|---|" + "---|" * (len(keys) + (1 if gate_col else 0)))
    for label, rows in results:
        if not rows:
            print("| %s | " % label + " | ".join("-" for _ in keys) + (" | - |" if gate_col else " |"))
            continue
        s = summarise(rows, keys)
        g = ""
        if gate_col:
            gv = [r["gate"] for r in rows if "gate" in r]
            g = " | %d %%" % round(100 * sum(gv) / len(gv)) if gv else " | -"
        print("| %s | " % label + " | ".join("%.2f / %.2f" % s[k] for k in keys) + g + " |")


def warp(d0, below_x10=50, factor=0.5):
    """A deliberately wrong prior: the runtime below MCU below_x10 / 10 % scaled by factor."""
    return [x * factor if GRID[j + 1] <= below_x10 else x for j, x in enumerate(d0)]


def prior_err(d0, ref):
    est = P.curve_x100(d0)
    return errors(est, ref, thresholds_mcu(ref))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--sessions", default=os.path.join(here, "..", "..", "sessions"))
    ap.add_argument("--seeds", type=int, default=200)
    ap.add_argument("--quick", action="store_true", help="40 seeds")
    ap.add_argument("--only", help="run only the experiment with this name (A, A2 is part of A, B, B3, C .. K)")
    args = ap.parse_args()
    seeds = 40 if args.quick else args.seeds

    f1 = Full(os.path.join(args.sessions, "0001.csv"))
    f2 = Full(os.path.join(args.sessions, "0002.csv"))
    ref12 = reference([f1, f2])
    ref1 = reference([f1])
    ref2 = reference([f2])
    ident = IDENT
    d_ref1 = P.prior_from_x100(ref1)
    d_ref12 = P.prior_from_x100(ref12)
    base = Scenario("base")
    loads = Scenario("loads", loads=(0.7, 1.0, 1.3))
    hours_list = (4, 8, 16, 32, 64)

    def want(name):
        return not args.only or args.only == name

    print("Cells: median / 90th percentile over %d seeds of the absolute error in real %% points "
          "(MCU grid; `@N` = at the MCU %% where the reference crosses real N %%)." % seeds)

    def fk(**kw):          # curve prior
        d = dict(sigma_0=0.3)
        d.update(kw)
        return d

    def fki(**kw):         # stock (identity) prior: weak
        d = dict(sigma_0=2.0)
        d.update(kw)
        return d

    if want("A"):
        res = [("stock prior alone", [prior_err(ident, ref12)])]
        for lname, lv, sk in (("same load", (1.0,), 0.1), ("load x0.85/1/1.15", (0.85, 1.0, 1.15), 0.2),
                              ("load x0.7/1/1.3", (0.7, 1.0, 1.3), 0.3)):
            for h in (8, 16, 32, 64):
                rows, st = run([f1, f2], ident, ref12, Scenario(lname, loads=lv), h, seeds, fki(sigma_k=sk))
                res.append(("%s, %d h (%.0f stints, %.1f to shutdown)" % (lname, h, statistics.mean(st["segs"]),
                                                                       statistics.mean(st["flat"])), rows))
        table("A. From scratch: stock gauge as prior (sigma_0 2), pieces of 0001+0002, vs curve(0001+0002)", res,
              note="sigma_k is set to match the simulated load spread (0.1 / 0.2 / 0.3).")
        res = []
        for h in (16, 32, 64):
            rows, _ = run([f1, f2], ident, ref12, loads, h, seeds, fki())
            res.append(("%d h, gate passed" % h, [r for r in rows if r["gate"]]))
            res.append(("%d h, gate failed" % h, [r for r in rows if not r["gate"]]))
        table("A2. The coverage gate (every block crossed by >= 2 stints, >= 1 stint to shutdown), load x0.7/1/1.3",
              res, note="Same runs as the x0.7/1/1.3 rows above, split by whether the gate would allow the candidate.")

    if want("B"):
        res = [("prior curve(0001) alone", [prior_err(d_ref1, ref2)])]
        for h in hours_list:
            rows, _ = run([f2], d_ref1, ref2, loads, h, seeds, fk())
            res.append(("%d h" % h, rows))
        table("B. Leave-one-out: curve(0001) as prior, pieces of 0002 only, vs curve(0002)", res,
              note="0001 and 0002 differ by at most 1.6 points, so the prior alone is already close; this checks "
                   "that partial data does not make a good curve worse.")
        res = [("prior curve(0002) alone", [prior_err(P.prior_from_x100(ref2), ref1)])]
        for h in hours_list:
            rows, _ = run([f1], P.prior_from_x100(ref2), ref1, loads, h, seeds, fk())
            res.append(("%d h" % h, rows))
        table("B2. The other way round: curve(0002) as prior, pieces of 0001, vs curve(0001)", res)

    if want("B3"):
        res = [("warped prior alone", [prior_err(warp(d_ref1), ref2)])]
        for h in hours_list:
            rows, _ = run([f2], warp(d_ref1), ref2, loads, h, seeds, fk())
            res.append(("%d h" % h, rows))
        table("B3. Wrong prior: curve(0001) with its MCU < 5 % runtime halved (a different cell), pieces of 0002, "
              "vs curve(0002)", res)

    if want("C"):
        nl = Scenario("never_low", loads=(0.7, 1.0, 1.3), never_low=True, stop_mcu=(8, 30))
        res = [("stock prior alone", [prior_err(ident, ref12)])]
        for h in (4, 8, 16, 32):
            rows, _ = run([f1, f2], ident, ref12, nl, h, seeds, fki())
            res.append(("stock prior, %d h" % h, rows))
        for h in (8, 32):
            rows, _ = run([f2], d_ref1, ref2, nl, h, seeds, fk())
            res.append(("LOO prior 0001, %d h" % h, rows))
        for h in (8, 32):
            rows, _ = run([f2], warp(d_ref1), ref2, nl, h, seeds, fk())
            res.append(("warped prior, %d h" % h, rows))
        table("C. Never below MCU 8-30 % (always charges early), loads x0.7/1/1.3", res)

    if want("D"):
        res = []
        for sk in (0.03, 0.1, 0.3, 1.0, 3.0):
            rows, _ = run([f1, f2], ident, ref12, loads, 32, seeds, fki(sigma_k=sk))
            res.append(("stock prior, sigma_k %g, 32 h" % sk, rows))
        for s0 in (0.1, 0.3, 1.0):
            rows, _ = run([f2], warp(d_ref1), ref2, loads, 32, seeds, fk(sigma_0=s0))
            res.append(("warped prior, sigma_0 %g, 32 h" % s0, rows))
        for o in (1, 3):
            rows, _ = run([f1, f2], ident, ref12, loads, 32, seeds, fki(outer=o))
            res.append(("stock prior, %d outer iteration(s), 32 h" % o, rows))
        for it in (20, 300):
            rows, _ = run([f1, f2], ident, ref12, loads, 32, seeds, dict(lam=0.05, iters=it), fitfn=P.fit_als)
            res.append(("first attempt: plain ALS (no load prior), %d iterations, 32 h" % it, rows))
        table("D. Load spread sigma_k, prior sigma_0, outer iterations, and the plain ALS (loads x0.7/1/1.3)", res)

    if want("E"):
        res = []
        for relax, trim in ((0.0, 0.0), (0.0, 5.0), (0.5, 0.0), (0.5, 5.0), (0.5, 10.0), (1.0, 5.0), (1.0, 10.0)):
            sc = Scenario("relax", loads=(0.7, 1.0, 1.3), relax=relax)
            rows, _ = run([f1, f2], ident, ref12, sc, 32, seeds, fki(), trim=trim)
            res.append(("relax +%.1f %% (tau 5 min), trim %g min, 32 h" % (relax, trim), rows))
        table("E. Synthetic relaxation after wake / unplug (MCU % reads high at the stint start), stock prior", res,
              note="Synthetic: the recorded sessions have no sleep or charging, so the offset and its decay are "
                   "assumptions, not measurements.")

    if want("F"):
        res = []
        for pc in (0.0, 0.3, 1.0):
            for out in (0.0, 0.10):
                sc = Scenario("change", loads=(0.5, 1.0, 1.5), p_change=pc)
                rows, st = run([f1, f2], ident, ref12, sc, 32, seeds, fki(outlier=out))
                res.append(("loads x0.5/1/1.5, change p=%.1f, outlier %s, 32 h (%.1f dropped)" % (
                    pc, "%.2f" % out if out else "off", statistics.mean(st["dropped"])), rows))
        table("F. Load changing half way through a stint (violates the model), with and without the outlier check", res)

    if want("G"):
        # gross outliers: a few stints from a "different battery" (time x2 below MCU 20 %)
        def bad(rng):
            f = f2
            out = []
            for _ in range(2):
                a = first_time_below(f, 30)
                rows = cut(f, a, f.T * 0.92, 1.0, 1.0, base)
                fixed, tw = [rows[0]], rows[0][0]
                for (ta, _pa), (tb, pb) in zip(rows, rows[1:]):
                    tw += (tb - ta) * (2.0 if pb < 20 else 1.0)   # below MCU 20 % time runs twice as long
                    fixed.append((tw, pb))
                s = P.segment_from_rows(fixed, trim_min=5)
                if s:
                    out.append(s)
            return out
        res = []
        for out in (0.0, 0.10):
            rows, st = run([f1, f2], ident, ref12, loads, 32, seeds, fki(outlier=out), extra=bad)
            res.append(("32 h + 2 bad stints, outlier %s (%.1f dropped)" % ("%.2f" % out if out else "off",
                                                                       statistics.mean(st["dropped"])), rows))
        table("G. Two corrupted stints (a different load or cell below MCU 20 %) mixed into 32 h", res)

    if want("H"):
        short = Scenario("short", loads=(0.7, 1.0, 1.3), seg_min=(15, 30))
        longs = Scenario("long", loads=(0.7, 1.0, 1.3), seg_min=(90, 180))
        res = []
        for sc in (short, loads, longs):
            for h in (16, 32):
                rows, st = run([f1, f2], ident, ref12, sc, h, seeds, fki())
                res.append(("stints %d-%d min, %d h (%.0f segs)" % (sc.seg_min[0], sc.seg_min[1], h,
                                                                 statistics.mean(st["segs"])), rows))
        table("H. Stint length (stock prior, loads x0.7/1/1.3)", res)

    if want("I"):
        res = []
        corr = Scenario("corr", load_by_mcu=lambda p: 0.7 if p > 60 else (1.3 if p < 30 else 1.0))
        for sk in (0.1, 0.3, 1.0):
            for h in (32, 64):
                rows, _ = run([f1, f2], ident, ref12, corr, h, seeds, fki(sigma_k=sk))
                res.append(("stock prior, sigma_k %g, %d h" % (sk, h), rows))
        for sk in (0.3, 1.0):
            rows, _ = run([f2], d_ref1, ref2, corr, 32, seeds, fk(sigma_k=sk))
            res.append(("LOO prior 0001, sigma_k %g, 32 h" % sk, rows))
        table("I. Load correlated with the charge level (heavy x0.7 above MCU 60 %, light x1.3 below 30 %)", res)

    if want("J"):
        res = []
        for s0 in (0.3, 0.5, 1.0):
            for name, prior in (("correct prior curve(0001)", d_ref1), ("warped prior", warp(d_ref1))):
                for h in (16, 64):
                    rows, _ = run([f2], prior, ref2, loads, h, seeds, fk(sigma_0=s0))
                    res.append(("%s, sigma_0 %g, %d h" % (name, s0, h), rows))
        table("J. Prior strength sigma_0: correct vs wrong prior (pieces of 0002, loads x0.7/1/1.3)", res)

    if want("K"):
        res = []
        corr = Scenario("corr", load_by_mcu=lambda p: 0.7 if p > 60 else (1.3 if p < 30 else 1.0))
        for sk in (0.3, 1.0):
            for sc in (loads, corr):
                for name, prior in (("correct prior", d_ref1), ("warped prior", warp(d_ref1))):
                    rows, _ = run([f2], prior, ref2, sc, 32, seeds, fk(sigma_0=0.5, sigma_k=sk))
                    res.append(("%s, %s load, sigma_k %g, 32 h" % (
                        name, "uncorrelated" if sc is loads else "correlated", sk), rows))
        table("K. Load spread sigma_k with a curve prior (sigma_0 0.5): uncorrelated vs correlated load, "
              "pieces of 0002", res)


if __name__ == "__main__":
    main()
