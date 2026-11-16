#!/usr/bin/env python3
"""Passive learning prototype for the Battery Manager curve (docs/passive-learning.md).

Learns the MCU % -> real % table from ordinary partial play (stints on battery separated by
sleep, power-off or charging) instead of complete full-charge -> shutdown discharges.

Model (section 2 of docs/passive-learning.md):
  * The MCU grid of build_curve.py (161 points) splits MCU % into 160 bins.
  * Every unbroken stretch on battery is a *segment*. A bin that the segment crosses
    completely gives an observation  dt[s, j] = time between the two grid crossings.
  * dt[s, j] ~= k[s] * d[j]: d[j] is the bin's share of the runtime at a reference load
    (in real % points), k[s] the segment's minutes per real % point (its load).
  * k and d are fitted by alternating least squares, with the existing curve as a prior
    on d (weight `lam` pseudo-observations per bin). Bins nobody crossed keep the prior.
  * real(g) = 100 * R(g) / R(98 %), R(g) = sum of d over the bins below g.

As a library: segments_from_csv(), segment_from_rows(), fit(), curve_x100().
As a CLI:     passive.py [--prior curve.bin | --prior-identity] session.csv ... [--bin out.bin]

Python 3, standard library only. Imports scripts/build_curve.py (grid, curve.bin packing).

AI disclosure: written with the help of AI (Claude Code).
"""

import argparse
import math
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import build_curve as bc  # noqa: E402

GRID = bc.MCU_GRID            # ascending, % x10
NG = len(GRID)                # 161 points
NB = NG - 1                   # 160 bins; bin j spans GRID[j] .. GRID[j+1]
FULL_X10 = 980                # real 100 % is defined at MCU 98.0 % (where a full charge starts)
FULL_IDX = GRID.index(FULL_X10)

FLAG_PASSIVE = 2              # proposed curve.bin flag bit1: built by passive learning

# Defaults (the values the validation in docs/passive-learning.md recommends)
DEFAULTS = dict(
    lam=1.0,            # fit_als only: prior weight per bin, in segment observations
    iters=20,           # fit_als only: ALS iterations
    trim_min=5.0,       # minutes dropped at each segment start (relaxation after wake / unplug)
    min_seg_min=10.0,   # shorter segments are ignored
    min_bins=3,         # segments with fewer complete bins are ignored
    outlier=0.10,       # max cumulative misfit (fraction of the stint's time, leave-one-out) before a stint is dropped
)


# ---------------------------------------------------------------- segments

class Segment:
    """One unbroken stretch on battery, reduced to grid crossings.

    obs: {bin j: dt in minutes} for every bin crossed completely.
    """

    def __init__(self, obs, minutes, first_pct, last_pct, shutdown, tag=""):
        self.obs = obs
        self.minutes = minutes
        self.first_pct = first_pct
        self.last_pct = last_pct
        self.shutdown = shutdown
        self.tag = tag
        self.k = None
        self.misfit = 0.0
        self.used = True

    def bins(self):
        return sorted(self.obs)


def segment_from_rows(rows, shutdown=False, trim_min=DEFAULTS["trim_min"], tag=""):
    """rows: [(t_ms, pct)] in time order, one unbroken stretch on battery.

    Same first-crossing rule and interpolation as build_curve.Session._cross. With
    shutdown=True all grid points below the last crossing are crossed at the last sample
    (the reference builder's "never reached -> t = T_end").
    Returns a Segment or None.
    """
    if not rows:
        return None
    t0 = rows[0][0] + trim_min * 60000.0
    rows = [r for r in rows if r[0] >= t0]
    if len(rows) < 2:
        return None
    cross = {}                     # grid index -> t (ms)
    nxt = NG - 1
    prev = None
    first_pct = rows[0][1]
    # grid points at or above the first sample were crossed before the segment started: skip them
    while nxt >= 0 and GRID[nxt] / 10.0 >= first_pct:
        nxt -= 1
    for t, x in rows:
        while nxt >= 0 and GRID[nxt] / 10.0 >= x:
            g = GRID[nxt] / 10.0
            tc = t
            if prev is not None and prev[1] > x:
                tc = prev[0] + (t - prev[0]) * (prev[1] - g) / (prev[1] - x)
            cross[nxt] = tc
            nxt -= 1
        prev = (t, x)
    t_last = rows[-1][0]
    if shutdown:
        while nxt >= 0:
            cross[nxt] = t_last
            nxt -= 1
    obs = {}
    for j in range(NB):
        if j in cross and (j + 1) in cross:
            obs[j] = (cross[j] - cross[j + 1]) / 60000.0
    minutes = (t_last - rows[0][0]) / 60000.0
    return Segment(obs, minutes, first_pct, rows[-1][1], shutdown, tag)


def read_csv_rows(path):
    """[(t_ms, pct, volt_raw, on_charger, comment)] from a recorder CSV (v1 or v2)."""
    out = []
    with open(path, "r", encoding="ascii", errors="replace") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if line.startswith("#"):
                out.append((None, None, None, None, line))
                continue
            p = line.split(",")
            if len(p) < 11 or not p[0].isdigit() or not p[2].isdigit() or not p[3].isdigit():
                continue
            on_chg = (p[9] not in ("0", "")) or (p[10] not in ("0", ""))
            out.append((int(p[0]), int(p[2]) + int(p[3]) / 256.0, int(p[5]), on_chg, None))
    return out


def segments_from_csv(path, trim_min=DEFAULTS["trim_min"], gap_ms=60000):
    """Splits a recorder CSV at sleep gaps, charger and time gaps > gap_ms."""
    segs, cur, last_t = [], [], None
    end_reason = None

    def flush(shutdown=False):
        if cur:
            s = segment_from_rows(cur, shutdown, trim_min, tag=os.path.basename(path))
            if s:
                segs.append(s)
        cur.clear()

    for t, pct, _v, chg, comment in read_csv_rows(path):
        if comment is not None:
            if comment.startswith("# sleep"):
                flush()
            elif comment.startswith("# end reason="):
                end_reason = comment[len("# end reason="):].split()[0] if len(comment) > 13 else ""
            continue
        if chg:
            flush()
            last_t = t
            continue
        if last_t is not None and t - last_t > gap_ms:
            flush()
        cur.append((t, pct))
        last_t = t
    flush(shutdown=end_reason in ("shutdown", "cutoff"))
    return segs


# ---------------------------------------------------------------- prior

def prior_from_x100(real_x100):
    """Bin durations (real % points) from a curve's mean table (x100, on GRID)."""
    return [max(0.0, (real_x100[j + 1] - real_x100[j]) / 100.0) for j in range(NB)]


def prior_identity():
    """No curve yet: the stock gauge (real % = MCU %)."""
    return [(GRID[j + 1] - GRID[j]) / 10.0 if GRID[j + 1] <= FULL_X10 else 0.0 for j in range(NB)]


def read_curve_bin(path):
    data = open(path, "rb").read()
    if len(data) != 2024 or struct.unpack_from("<I", data, 0)[0] != bc.MAGIC:
        raise ValueError("not a curve.bin v1")
    if zlib.crc32(data[:2020]) & 0xFFFFFFFF != struct.unpack_from("<I", data, 2020)[0]:
        raise ValueError("bad CRC")
    mean = list(struct.unpack_from("<161H", data, 82 + 161 * 2))
    volt = [list(struct.unpack_from("<81H", data, 1370 + 81 * 2 * k)) for k in range(1, 4)]
    return mean, volt


# ---------------------------------------------------------------- fit

def fit_als(segs, d0, lam=DEFAULTS["lam"], iters=DEFAULTS["iters"], outlier=DEFAULTS["outlier"],
            min_seg_min=DEFAULTS["min_seg_min"], min_bins=DEFAULTS["min_bins"], lam_bins=None):
    """First attempt (kept for the comparison in docs/passive-learning.md 3.1): plain alternating least squares for dt[s,j] = k[s] * d[j] with a prior d0.

    Returns dict(d, n, se, segs). n[j] = segments observing bin j, se[j] = standard error of d[j].
    Segments whose cumulative misfit exceeds `outlier` are dropped (s.used = False) and the fit
    is repeated once without them.
    """
    use = [s for s in segs if s.minutes >= min_seg_min and len(s.obs) >= min_bins]
    for s in segs:
        s.used = s in use
    lamv = lam_bins if lam_bins is not None else [lam] * NB

    def run(active):
        d = list(d0)
        for _ in range(max(1, iters)):
            # k step: ratio estimator (total time / total reference duration of the bins it crossed)
            for s in active:
                ref = sum(d[j] for j in s.obs)
                tot = sum(s.obs.values())
                s.k = tot / ref if ref > 1e-9 else None
            # d step: prior + mean of dt/k
            num = [lamv[j] * d0[j] for j in range(NB)]
            den = list(lamv)
            for s in active:
                if not s.k:
                    continue
                for j, dt in s.obs.items():
                    num[j] += dt / s.k
                    den[j] += 1.0
            d = [num[j] / den[j] if den[j] > 0 else d0[j] for j in range(NB)]
        return d

    d = run(use)
    if outlier:
        for s in use:
            s.misfit = misfit(s, d)
        keep = [s for s in use if s.misfit <= outlier]
        if len(keep) < len(use):
            for s in use:
                s.used = s in keep
            use = keep
            d = run(use)
            for s in use:
                s.misfit = misfit(s, d)

    # per-bin confidence
    n = [0] * NB
    sq = [0.0] * NB
    for s in use:
        if not s.k:
            continue
        for j, dt in s.obs.items():
            n[j] += 1
            sq[j] += (dt / s.k - d[j]) ** 2
    se = []
    for j in range(NB):
        if n[j] >= 2:
            se.append(math.sqrt(sq[j] / (n[j] - 1)) / math.sqrt(n[j] + lamv[j]))
        else:
            se.append(float("nan"))
    return dict(d=d, n=n, se=se, segs=use)


# Fit blocks (bin ranges, MCU % x10 edges). Blocks are long enough (>= ~1 % of the runtime at the
# low end, several % at the top) that a stint's time inside one is a usable log observation.
BLOCK_EDGES = [0, 10, 15, 20, 25, 30, 40, 50, 70, 100, 150, 200, 300, 400, 500, 600, 700, 800, 900, 980]
BLOCKS = [(GRID.index(BLOCK_EDGES[i]), GRID.index(BLOCK_EDGES[i + 1])) for i in range(len(BLOCK_EDGES) - 1)]
NBLK = len(BLOCKS)            # 19; block b = bins BLOCKS[b][0] .. BLOCKS[b][1]-1


def solve(a, b):
    """Dense Gaussian elimination with partial pivoting (a is n x n, modified)."""
    n = len(b)
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(a[r][c]))
        a[c], a[p] = a[p], a[c]
        b[c], b[p] = b[p], b[c]
        piv = a[c][c]
        for r in range(c + 1, n):
            f = a[r][c] / piv
            if f:
                row_r, row_c = a[r], a[c]
                for k in range(c, n):
                    row_r[k] -= f * row_c[k]
                b[r] -= f * b[c]
    x = [0.0] * n
    for c in range(n - 1, -1, -1):
        x[c] = (b[c] - sum(a[c][k] * x[k] for k in range(c + 1, n))) / a[c][c]
    return x


def fit(segs, d0, sigma_k=0.3, sigma_0=0.3, sigma_m=0.15, e_t=0.5, lam=0.25, outer=3,
        outlier=DEFAULTS["outlier"], min_seg_min=DEFAULTS["min_seg_min"], min_bins=DEFAULTS["min_bins"],
        loo=True, iters=None):
    """Random-effects fit (docs/passive-learning.md, section 2).

    log(time of stint s in block B / covered share of B) = a_s + b_B + noise
      a_s = log k_s ~ N(mu, sigma_k^2)      (stint load, shrunk towards the typical load mu)
      b_B = log D_B ~ N(log D0_B, sigma_0^2) (block duration, prior = existing curve)
      noise variance sigma_m^2 + 2 e_t^2 / dt^2 (load wobble + crossing-time quantisation)
    solved exactly (normal equations), then fine bins: d_j = D_B * share_j, shares from the
    pooled, load-corrected bin times (lam pseudo-observations of the prior share).
    Returns dict(d, n, se, segs, nb, seb) (n/se per bin, nb/seb per block).
    """
    use = [s for s in segs if s.minutes >= min_seg_min and len(s.obs) >= min_bins]
    for s in segs:
        s.used = s in use
    d0 = [x if j < FULL_IDX else 0.0 for j, x in enumerate(d0)]

    def blocks_of(dd):
        return [sum(dd[lo:hi]) for lo, hi in BLOCKS]

    def run(active):
        d = list(d0)
        info = None
        for _ in range(outer):
            D = blocks_of(d)
            obs = []          # (seg index, block, y, w)
            for si, s in enumerate(active):
                for bi, (lo, hi) in enumerate(BLOCKS):
                    if D[bi] <= 0:
                        continue
                    cov = [j for j in range(lo, hi) if j in s.obs]
                    if not cov:
                        continue
                    dt = sum(s.obs[j] for j in cov)
                    phi = sum(d[j] for j in cov) / D[bi]
                    if phi < 0.05 or dt < 0.5:
                        continue
                    w = 1.0 / (sigma_m ** 2 + 2.0 * e_t ** 2 / dt ** 2)
                    obs.append((si, bi, math.log(dt / phi), w))
            S = len(active)
            n = S + NBLK + 1                  # a_0..a_S-1, b_0..b_18, mu
            A = [[0.0] * n for _ in range(n)]
            r = [0.0] * n
            for si, bi, y, w in obs:
                ia, ib = si, S + bi
                A[ia][ia] += w
                A[ib][ib] += w
                A[ia][ib] += w
                A[ib][ia] += w
                r[ia] += w * y
                r[ib] += w * y
            pk = 1.0 / sigma_k ** 2
            imu = n - 1
            for si in range(S):
                A[si][si] += pk
                A[si][imu] -= pk
                A[imu][si] -= pk
                A[imu][imu] += pk
            p0 = 1.0 / sigma_0 ** 2
            for bi in range(NBLK):
                A[S + bi][S + bi] += p0
                r[S + bi] += p0 * math.log(max(D0[bi], 1e-6))
            if S == 0:
                A[imu][imu] += 1.0
            x = solve([row[:] for row in A], r[:])
            for si, s in enumerate(active):
                s.k = math.exp(x[si])
            Dn = [math.exp(x[S + bi]) for bi in range(NBLK)]
            # fine bins: load-corrected pooled times give the shares inside each block
            num = [lam * d[j] for j in range(NB)]
            den = [lam] * NB
            for s in active:
                for j, dt in s.obs.items():
                    if j < FULL_IDX:
                        num[j] += dt / s.k
                        den[j] += 1.0
            fine = [num[j] / den[j] for j in range(NB)]
            nd = [0.0] * NB
            for bi, (lo, hi) in enumerate(BLOCKS):
                tot = sum(fine[lo:hi])
                for j in range(lo, hi):
                    nd[j] = Dn[bi] * fine[j] / tot if tot > 0 else 0.0
            d = nd
            info = (A, S, obs)
        return d, info

    D0 = blocks_of(d0)
    d, info = run(use)
    if outlier and use:
        if loo and len(use) > 2:
            # leave-one-out: judge each stint against the curve fitted from all the others
            cnt = [0] * NB
            for o in use:
                for j in o.obs:
                    cnt[j] += 1
            for s in use:
                # only the bins at least two other stints crossed can be checked
                chk = {j: dt for j, dt in s.obs.items() if cnt[j] - 1 >= 2}
                if len(chk) < min_bins or sum(chk.values()) < 0.5 * sum(s.obs.values()):
                    s.misfit = 0.0
                    continue
                d_others, _ = run([o for o in use if o is not s])
                ref = sum(d_others[j] for j in chk)
                probe = Segment(chk, s.minutes, s.first_pct, s.last_pct, s.shutdown)
                probe.k = sum(chk.values()) / ref if ref > 0 else None
                s.misfit = misfit(probe, d_others)
        else:
            for s in use:
                s.misfit = misfit(s, d)
        keep = [s for s in use if s.misfit <= outlier]
        if len(keep) < len(use):
            for s in use:
                s.used = s in keep
            use = keep
            d, info = run(use)
            for s in use:
                s.misfit = misfit(s, d)

    # confidence: observations per bin / block, posterior sd of log D_B (diagonal of A^-1)
    nbin = [0] * NB
    for s in use:
        for j in s.obs:
            nbin[j] += 1
    A, S, obs = info
    nb = [0] * NBLK
    for _si, bi, _y, _w in obs:
        nb[bi] += 1
    seb = []
    for bi in range(NBLK):
        e = [0.0] * len(A)
        e[S + bi] = 1.0
        seb.append(math.sqrt(max(0.0, solve([row[:] for row in A], e)[S + bi])))
    se = [float("nan")] * NB
    for bi, (lo, hi) in enumerate(BLOCKS):
        for j in range(lo, hi):
            se[j] = seb[bi] * d[j]       # sd of d_j from its block (shares treated as exact)
    return dict(d=d, n=nbin, se=se, segs=use, nb=nb, seb=seb)


def misfit(s, d):
    """Largest gap between the segment's cumulative time and k * cumulative d, as a fraction
    of the segment's observed time. A load change inside the segment bends it."""
    if not s.k:
        return 1.0
    js = sorted(s.obs, reverse=True)       # time order: high bins first
    tot = sum(s.obs.values())
    c_obs = c_pred = worst = 0.0
    for j in js:
        c_obs += s.obs[j]
        c_pred += s.k * d[j]
        worst = max(worst, abs(c_obs - c_pred))
    return worst / tot if tot > 0 else 1.0


def curve_from_d(d):
    """real % at every grid point (floats), real(98 %) = 100, monotonic, clamped."""
    r = [0.0]
    for j in range(NB):
        r.append(r[-1] + d[j])
    full = r[FULL_IDX]
    if full <= 0:
        raise ValueError("empty curve")
    return [min(100.0, 100.0 * x / full) for x in r]


def curve_x100(d):
    v = [bc.to_x100(x) for x in curve_from_d(d)]
    bc.monotonic(v)
    return v


def band_x100(fitres, z=2.0):
    """min / max tables: real % +- z * sd, from the block sds (blocks with no data add nothing)."""
    real = curve_from_d(fitres["d"])
    d = fitres["d"]
    full = sum(d[:FULL_IDX])
    lo, hi = [], []
    for i in range(NG):
        var = 0.0
        for bi, (a, b) in enumerate(BLOCKS):
            if fitres["nb"][bi] == 0 or a >= i:
                continue
            part = sum(d[a:min(b, i)])          # the share of block bi below grid point i
            var += (part * fitres["seb"][bi]) ** 2
        e = z * 100.0 * math.sqrt(var) / full
        lo.append(bc.to_x100(real[i] - e))
        hi.append(bc.to_x100(real[i] + e))
    bc.monotonic(lo)
    bc.monotonic(hi)
    return lo, hi


def gate(fitres, min_block=2, min_shutdown=1, prior_is_curve=True):
    """Coverage rule before a candidate may be offered (docs/passive-learning.md, section 5.4).

    Stock prior (no full discharge yet): every block crossed by >= min_block stints and
    >= min_shutdown stints that ended in a shutdown (the tail below MCU 1 % is only seen there).
    Curve prior: blocks with too little data simply keep the prior, so only >= 1 block must
    qualify. Returns (ok, list of reasons)."""
    why = []
    thin = [bi for bi in range(NBLK) if fitres["nb"][bi] < min_block]
    flat = sum(1 for s in fitres["segs"] if s.shutdown)
    if not prior_is_curve:
        if thin:
            why.append("%d of %d blocks crossed by < %d stints" % (len(thin), NBLK, min_block))
        if flat < min_shutdown:
            why.append("%d stints ended in a shutdown (need %d)" % (flat, min_shutdown))
    elif len(thin) == NBLK:
        why.append("no block crossed by %d stints" % min_block)
    return (not why), why


def pack_candidate(fitres, volt_tables, n_sources):
    """curve.bin v1 bytes for a candidate (same layout; flags bit1 = passive, min/max = 2-sigma band)."""
    mean = curve_x100(fitres["d"])
    lo, hi = band_x100(fitres)
    curve = {"n": max(1, min(bc.MAX_SESSIONS, n_sources)), "devs": [], "spread": max(h - l for h, l in zip(hi, lo)),
             "outliers": 0, "mcu": (mean, lo, hi), "volt": tuple(volt_tables), "numbers": []}
    data = bytearray(bc.pack(curve))
    volt_ok = any(volt_tables[0])
    struct.pack_into("<H", data, 8, (bc.FLAG_VOLT_VALID if volt_ok else 0) | FLAG_PASSIVE)
    struct.pack_into("<I", data, 2020, zlib.crc32(bytes(data[:2020])) & 0xFFFFFFFF)
    return bytes(data)


# ---------------------------------------------------------------- CLI

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="+")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--prior", help="existing curve.bin (prior for bins nobody crossed)")
    g.add_argument("--prior-identity", action="store_true", help="no curve yet: stock gauge as prior")
    ap.add_argument("--sigma-k", type=float, help="expected load spread between stints (log); "
                    "default 1.0 with --prior, 0.3 with --prior-identity (docs/passive-learning.md 5.3)")
    ap.add_argument("--trim", type=float, default=DEFAULTS["trim_min"], help="minutes trimmed per segment start")
    ap.add_argument("--bin", help="write the candidate curve.bin")
    args = ap.parse_args()

    if args.prior:
        mean, volt = read_curve_bin(args.prior)
        d0 = prior_from_x100(mean)
    else:
        mean, volt = None, None
        d0 = prior_identity()
    segs = []
    for p in args.csv:
        segs += segments_from_csv(p, trim_min=args.trim)
    sk = args.sigma_k if args.sigma_k is not None else (1.0 if args.prior else 0.3)
    res = fit(segs, d0, sigma_0=0.5 if args.prior else 2.0, sigma_k=sk)
    ok, why = gate(res, prior_is_curve=bool(args.prior))
    print("stints: %d found, %d used; gate: %s" % (len(segs), len(res["segs"]), "ok" if ok else "; ".join(why)))
    for s in segs:
        print("  %-10s %6.1f min  %5.1f -> %5.1f %%  bins %3d  k %6s  misfit %.3f%s%s" % (
            s.tag, s.minutes, s.first_pct, s.last_pct, len(s.obs),
            "%.2f" % s.k if s.k else "-", s.misfit, "  shutdown" if s.shutdown else "",
            "" if s.used else "  NOT USED"))
    cand = curve_x100(res["d"])
    lo, hi = band_x100(res)
    print("\n MCU %   prior   candidate  (2-sigma band)")
    for x in (980, 900, 700, 500, 400, 250, 100, 50, 30, 20, 15, 10):
        i = GRID.index(x)
        print("  %5.1f  %6s   %6s    %6s .. %6s" % (x / 10, bc.fmt(mean[i]) if mean else "-", bc.fmt(cand[i]),
                                                bc.fmt(lo[i]), bc.fmt(hi[i])))
    print("\n block (MCU %)   stints   sd(log D)")
    for bi in range(NBLK):
        print("  %5.1f-%5.1f   %3d      %s" % (BLOCK_EDGES[bi] / 10, BLOCK_EDGES[bi + 1] / 10, res["nb"][bi],
                                            "%.3f" % res["seb"][bi]))
    if args.bin:
        if volt is None:
            volt = ([0] * 81, [0] * 81, [0] * 81)
        open(args.bin, "wb").write(pack_candidate(res, volt, len(res["segs"])))
        print("wrote", args.bin)


if __name__ == "__main__":
    main()
