#!/usr/bin/env python3
"""Battery Manager curve builder (host-side reference implementation).

Implements the same algorithm as sysmodules/rosalina/source/battery/battery_curve.c
(docs/battery-manager.md, section 11) on recorded session CSVs, prints the tables and
can write a curve.bin that is byte-identical to the one the console builds.

Usage:
  scripts/build_curve.py sessions/0001.csv [more.csv ...]
        [--bin curve.bin] [--csv table.csv] [--svg plot.svg]
        [--include-high-end] [--include-incomplete] [--quiet]

Accepts recorder CSV v1 and v2 (v2 appends corrected,led_desired,led_actual,reasserts; the
columns used here are the first six, which are the same in both).

Python 3, standard library only.

AI disclosure: written with the help of AI (Claude Code), reviewed by the maintainer.
"""

import argparse
import struct
import sys
import zlib

MAGIC = 0x56524342  # "BCRV"
VERSION = 1
MAX_SESSIONS = 16
MEDIAN_WINDOW = 7
MIN_SAMPLES = 30
MIN_DURATION_MS = 600000
OUTLIER_X100 = 500
HIGH_END_PCT = 5.0
BLEND_HI = 3.0
BLEND_LO = 2.0
FLAG_VOLT_VALID = 1


def make_grids():
    mcu = list(range(0, 51, 1)) + list(range(55, 201, 5)) + list(range(210, 1001, 10))
    volt = [1405 + 10 * i for i in range(81)]
    return mcu, volt


MCU_GRID, VOLT_GRID = make_grids()
assert len(MCU_GRID) == 161 and len(VOLT_GRID) == 81


class Session:
    """One pass over a session CSV (mirrors BatteryCurveSession)."""

    def __init__(self, path):
        self.path = path
        self.number = 0
        self.samples = 0
        self.sleep_gap = False
        self.end_reason = None
        self.first_pct = self.last_pct = self.last_t = 0.0
        self.rows = []  # (t, pct, volt_raw) for the report
        self.mcu = [0.0] * len(MCU_GRID)
        self.volt = [0.0] * len(VOLT_GRID)
        self._mcu = [len(MCU_GRID) - 1, False, 0.0, 0.0]  # next, has_prev, prev_x, prev_t
        self._volt = [len(VOLT_GRID) - 1, False, 0.0, 0.0]
        self._win = []  # (volt, t)
        with open(path, "r", encoding="ascii", errors="replace") as f:
            for line in f:
                self.feed(line.rstrip("\r\n"))

    @staticmethod
    def _cross(grid, times, st, x, t):
        while st[0] >= 0 and grid[st[0]] / 10.0 >= x:
            g = grid[st[0]] / 10.0
            tc = t
            if st[1] and st[2] > x:
                tc = st[3] + (t - st[3]) * (st[2] - g) / (st[2] - x)
            times[st[0]] = tc
            st[0] -= 1
        st[1], st[2], st[3] = True, x, t

    def feed(self, line):
        if line.startswith("#"):
            if line.startswith("# sleep "):
                self.sleep_gap = True
            elif line.startswith("# end reason="):
                self.end_reason = line[len("# end reason="):].split()[0] if len(line) > 13 else ""
            elif line.startswith("# session="):
                try:
                    self.number = int(line[len("# session="):])
                except ValueError:
                    pass
            return
        f = line.split(",")
        if len(f) < 7 or not all(s.isdigit() for s in (f[0], f[2], f[3], f[5])):
            return
        t_ms, pct_int, pct_frac, volt_raw = int(f[0]), int(f[2]), int(f[3]), int(f[5])
        if pct_int > 100 or pct_frac > 255 or volt_raw > 255:
            return
        t = float(t_ms)
        pct = pct_int + pct_frac / 256.0
        if self.samples == 0:
            self.first_pct = pct
        self.samples += 1
        self.last_pct, self.last_t = pct, t
        self.rows.append((t, pct, volt_raw))

        self._cross(MCU_GRID, self.mcu, self._mcu, pct, t)

        self._win.append((volt_raw, t))
        if len(self._win) > MEDIAN_WINDOW:
            self._win.pop(0)
        if len(self._win) == MEDIAN_WINDOW:
            v = float(sorted(w[0] for w in self._win)[MEDIAN_WINDOW // 2])
            self._cross(VOLT_GRID, self.volt, self._volt, v, self._win[MEDIAN_WINDOW // 2][1])

    def end(self):
        """Returns None on success, or a rejection reason."""
        if self.sleep_gap:
            return "sleep gap"
        if self.samples < MIN_SAMPLES or self.last_t < MIN_DURATION_MS:
            return "too short"
        for times, st in ((self.mcu, self._mcu), (self.volt, self._volt)):
            for i in range(st[0] + 1):
                times[i] = self.last_t
            for i in range(len(times)):
                times[i] = 100.0 * (self.last_t - times[i]) / self.last_t
        return None

    def is_complete(self):
        return (not self.sleep_gap and self.samples > 0 and pct_x10(self.first_pct) >= 950
                and self.end_reason in (None, "shutdown", "cutoff"))

    def end_pct_x10(self):
        return pct_x10(self.last_pct)


def pct_x10(pct):
    """Same rounding as BatteryMcu_PercentX10 (used by the Sessions list)."""
    pi = int(pct)
    pf = round((pct - pi) * 256)
    return pi * 10 + (pf * 10 + 128) // 256


def to_x100(real):
    if real <= 0.0:
        return 0
    if real >= 100.0:
        return 10000
    return int(real * 100.0 + 0.5)


def monotonic(v):
    for i in range(1, len(v)):
        if v[i] < v[i - 1]:
            v[i] = v[i - 1]


def build(sessions):
    n = len(sessions)
    tables = {}
    for name, size, attr in (("mcu", len(MCU_GRID), "mcu"), ("volt", len(VOLT_GRID), "volt")):
        s_ = [0.0] * size
        mn = [0] * size
        mx = [0] * size
        for k, s in enumerate(sessions):
            vals = getattr(s, attr)
            for i in range(size):
                x = to_x100(vals[i])
                s_[i] += vals[i]
                if k == 0 or x < mn[i]:
                    mn[i] = x
                if k == 0 or x > mx[i]:
                    mx[i] = x
        mean = [to_x100(v / n) for v in s_]
        for t in (mean, mn, mx):
            monotonic(t)
        tables[name] = (s_, mean, mn, mx)

    devs = []
    for s in sessions:
        if n < 2:
            devs.append(0)
            continue
        total = 0.0
        sums = tables["mcu"][0]
        for i in range(len(MCU_GRID)):
            others = (sums[i] - s.mcu[i]) / (n - 1)
            total += abs(s.mcu[i] - others)
        devs.append(to_x100(total / len(MCU_GRID)))

    _, mmean, mmin, mmax = tables["mcu"]
    _, vmean, vmin, vmax = tables["volt"]
    spread = max(mmax[i] - mmin[i] for i in range(len(MCU_GRID)))
    return {
        "n": n, "devs": devs, "spread": spread,
        "outliers": sum(1 for d in devs if d > OUTLIER_X100),
        "mcu": (mmean, mmin, mmax), "volt": (vmean, vmin, vmax),
        "numbers": [s.number for s in sessions],
    }


def pack(curve):
    def pad(lst):
        return lst + [0] * (MAX_SESSIONS - len(lst))

    u16 = []
    u16 += [VERSION, curve["n"], FLAG_VOLT_VALID, len(MCU_GRID), len(VOLT_GRID),
            curve["spread"], curve["outliers"]]
    u16 += pad(curve["numbers"]) + pad(curve["devs"])
    u16 += MCU_GRID + list(curve["mcu"][0]) + list(curve["mcu"][1]) + list(curve["mcu"][2])
    u16 += VOLT_GRID + list(curve["volt"][0]) + list(curve["volt"][1]) + list(curve["volt"][2])
    u16 += [0]  # reserved
    body = struct.pack("<I", MAGIC) + struct.pack("<%dH" % len(u16), *u16)
    data = body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)
    assert len(data) == 2024
    return data


def interp(grid, values, x_x10):
    if x_x10 <= grid[0]:
        return values[0] / 100.0
    if x_x10 >= grid[-1]:
        return values[-1] / 100.0
    lo, hi = 0, len(grid) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if grid[mid] <= x_x10:
            lo = mid
        else:
            hi = mid
    f = (x_x10 - grid[lo]) / (grid[hi] - grid[lo])
    return (values[lo] + f * (values[hi] - values[lo])) / 100.0


def corrected(curve, pct, volt_raw):
    m = interp(MCU_GRID, curve["mcu"][0], pct * 10.0)
    w = 0.0
    if volt_raw > 0:
        if pct <= BLEND_LO:
            w = 1.0
        elif pct < BLEND_HI:
            w = (BLEND_HI - pct) / (BLEND_HI - BLEND_LO)
    if w == 0.0:
        return m
    v = min(interp(VOLT_GRID, curve["volt"][0], volt_raw * 10.0), m)
    return (1.0 - w) * m + w * v


def report_rows(session):
    """(label, t, pct, volt_raw) rows: first samples at MCU <= 50/25/10/5/3/2/1, and the last minutes."""
    out = []
    rows = session.rows
    for target in (50, 25, 10, 5, 3, 2, 1):
        for t, pct, v in rows:
            if pct <= target:
                out.append(("mcu<=%d" % target, t, pct, v))
                break
    t_end = rows[-1][0]
    for mins in (15, 10, 5, 2, 0):
        for t, pct, v in rows:
            if t >= t_end - mins * 60000:
                out.append(("end-%dmin" % mins, t, pct, v))
                break
    return out


def fmt(x100):
    return "%d.%02d" % (x100 // 100, x100 % 100)


def svg_plot(curve, path, zoom=False):
    w, h, l, r, t, b = 640, 480, 50, 20, 20, 40
    xmax, ymax = (10.0, 50.0) if zoom else (100.0, 100.0)
    pw, ph = w - l - r, h - t - b

    def px(x, y):
        return l + pw * x / xmax, t + ph * (1 - y / ymax)

    def poly(vals, color, width):
        pts = []
        for k in range(301):
            x = xmax * k / 300
            xx, yy = px(x, min(interp(MCU_GRID, vals, x * 10), ymax))
            pts.append("%.1f,%.1f" % (xx, yy))
        return '<polyline fill="none" stroke="%s" stroke-width="%s" points="%s"/>' % (color, width, " ".join(pts))

    parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d">' % (w, h),
             '<rect width="100%" height="100%" fill="white"/>']
    for k in range(6):
        x = xmax * k / 5
        xx, yy = px(x, 0)
        parts.append('<text x="%.0f" y="%.0f" font-size="12" text-anchor="middle">%g</text>' % (xx, yy + 16, x))
        y = ymax * k / 5
        xx, yy = px(0, y)
        parts.append('<text x="%.0f" y="%.0f" font-size="12" text-anchor="end">%g</text>' % (xx - 4, yy + 4, y))
        parts.append('<line x1="%d" y1="%.0f" x2="%d" y2="%.0f" stroke="#eee"/>' % (l, yy, w - r, yy))
    band_hi = [px(xmax * k / 300, min(interp(MCU_GRID, curve["mcu"][2], xmax * k / 30), ymax)) for k in range(301)]
    band_lo = [px(xmax * k / 300, min(interp(MCU_GRID, curve["mcu"][1], xmax * k / 30), ymax)) for k in range(301)]
    parts.append('<polygon fill="#bfe3c0" points="%s"/>' % " ".join("%.1f,%.1f" % p for p in band_hi + band_lo[::-1]))
    x0, y0 = px(0, 0)
    x1, y1 = px(xmax, min(xmax, ymax))
    parts.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#888" stroke-dasharray="4"/>' % (x0, y0, x1, y1))
    parts.append(poly(curve["mcu"][0], "#1a7f37", 2))
    parts.append('<text x="%d" y="%d" font-size="13">X: MCU %%, Y: real %% (n=%d)</text>' % (l + 10, t + 14, curve["n"]))
    parts.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(parts) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--bin", help="write curve.bin")
    ap.add_argument("--csv", dest="out_csv", help="write the tables as CSV")
    ap.add_argument("--svg", help="write an SVG plot (full range; a *_zoom.svg is written too)")
    ap.add_argument("--include-high-end", action="store_true", help="use complete sessions ending above 5 %%")
    ap.add_argument("--include-incomplete", action="store_true", help="use sessions that aren't 'complete'")
    ap.add_argument("--quiet", action="store_true", help="only print the summary and the report")
    args = ap.parse_args()

    used = []
    for path in args.csv:
        s = Session(path)
        label = "#%04d (%s)" % (s.number, path)
        if not s.is_complete() and not args.include_incomplete:
            print("skip %s: not complete" % label)
            continue
        if s.end_pct_x10() > HIGH_END_PCT * 10 and not args.include_high_end:
            print("skip %s: ends at %.1f %% (manual power-off?)" % (label, s.last_pct))
            continue
        why = s.end()
        if why:
            print("skip %s: %s" % (label, why))
            continue
        if len(used) == MAX_SESSIONS:
            print("skip %s: more than %d sessions" % (label, MAX_SESSIONS))
            continue
        used.append(s)

    if not used:
        sys.exit("no usable session")

    # Ascending session numbers, like the console (BatteryCurve_Build): curve.bin doesn't depend on the order
    used.sort(key=lambda s: s.number)

    curve = build(used)
    print("sessions used: %d, max spread: %s points, outliers: %d"
          % (curve["n"], fmt(curve["spread"]), curve["outliers"]))
    for s, d in zip(used, curve["devs"]):
        print("  #%04d  %.2f h  %.1f -> %.1f %%  deviation %s%s"
              % (s.number, s.last_t / 3600000, s.first_pct, s.last_pct, fmt(d),
                 "  OUTLIER" if d > OUTLIER_X100 else ""))

    if not args.quiet:
        print("\nMCU table (MCU %, real % mean / min / max)")
        for i, g in enumerate(MCU_GRID):
            print("  %5.1f  %6s  %6s  %6s" % (g / 10, fmt(curve["mcu"][0][i]), fmt(curve["mcu"][1][i]), fmt(curve["mcu"][2][i])))
        print("\nVoltage table (volt_raw, V at 5/256, real % mean / min / max)")
        for i, g in enumerate(VOLT_GRID):
            print("  %5.1f  %5.3f  %6s  %6s  %6s" % (g / 10, g / 10 * 5 / 256, fmt(curve["volt"][0][i]),
                                                fmt(curve["volt"][1][i]), fmt(curve["volt"][2][i])))

    print("\nReport (session #%04d): label, t_min, mcu_pct, volt_raw, real_time_based, mcu_table, corrected" % used[0].number)
    t_end = used[0].rows[-1][0]
    for label, t, pct, v in report_rows(used[0]):
        print("  %-10s %7.1f %7.3f %3d %6.2f %6.2f %6.2f" % (
            label, t / 60000, pct, v, 100 * (t_end - t) / t_end,
            interp(MCU_GRID, curve["mcu"][0], pct * 10), corrected(curve, pct, v)))

    if args.bin:
        with open(args.bin, "wb") as f:
            f.write(pack(curve))
    if args.out_csv:
        with open(args.out_csv, "w") as f:
            f.write("table,x,mean,min,max\n")
            for i, g in enumerate(MCU_GRID):
                f.write("mcu,%.1f,%s,%s,%s\n" % (g / 10, fmt(curve["mcu"][0][i]), fmt(curve["mcu"][1][i]), fmt(curve["mcu"][2][i])))
            for i, g in enumerate(VOLT_GRID):
                f.write("volt,%.1f,%s,%s,%s\n" % (g / 10, fmt(curve["volt"][0][i]), fmt(curve["volt"][1][i]), fmt(curve["volt"][2][i])))
    if args.svg:
        svg_plot(curve, args.svg)
        svg_plot(curve, args.svg[:-4] + "_zoom.svg" if args.svg.endswith(".svg") else args.svg + "_zoom", zoom=True)


if __name__ == "__main__":
    main()
