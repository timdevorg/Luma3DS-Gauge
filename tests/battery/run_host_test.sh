#!/usr/bin/env bash
# Host test for the Battery Manager curve, SD and LED code.
#
# Usage: tests/battery/run_host_test.sh [session.csv ...]
#
# Without arguments it uses sessions/0001.csv and sessions/0002.csv (repo root, gitignored) if both
# exist. Otherwise only the unit tests run (LED logic, safe write) and the parts that need
# recorded sessions (curve comparison, session parsing, LED replay) are SKIPPED, with a message.
# Get the CSVs from the console's /luma/battery/sessions/ into sessions/ first for the full run.
#
# Compiles sysmodules/rosalina/source/battery/battery_curve.c natively (clang or cc),
# builds a curve from the given CSVs with it and with scripts/build_curve.py, and checks:
#   - both curve.bin files are byte-identical (sessions in ascending order, whatever the order given)
#   - the printed summary / report values agree (within 0.01, C uses float for lookups)
#   - the C self-checks pass (monotonic, continuous, checksum)
# Then:
#   - converts the CSVs to recorder v2 (extra LED columns) and checks that build_curve.py and
#     the console's session parser / curve builder (battery_sessions.c, battery_curve_sd.c on
#     host stubs, sd_host_test.c) treat v1 and v2 the same, and that BatteryCurve_Build with
#     the sessions newest first gives the same curve.bin as build_curve.py
#   - LED logic unit tests + replay of the sessions with that curve (led_host_test.c)
# Always (with or without sessions):
#   - ptm sleep / wake LED patch: decision matrix + verification unit tests (ptm_host_test.c). With the
#     New 3DS ptm dump (GodMode9 "Extract .code" of 0004013020002202) also checks both patterns in the
#     real code (in place, stock, unique).
#     PTM_CODE=<path> selects the dump; default sessions/ptm/0004013020002202.dec.code; SKIPPED if missing.
#
# GRAPH_OUT=<dir>: also renders the on-console graph (battery_graph.c) into
# <dir>/graph_full.ppm, graph_zoom.ppm and graph_nocurve_full.ppm / _zoom.ppm (needs sessions).
#
# AI disclosure: written with the help of AI (Claude Code), reviewed by the maintainer.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/battery-host-test.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

case "${1:-}" in -h|--help) sed -n '2,31p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;; esac
if [ $# -eq 0 ] && [ -f "$REPO/sessions/0001.csv" ] && [ -f "$REPO/sessions/0002.csv" ]; then
    set -- "$REPO/sessions/0001.csv" "$REPO/sessions/0002.csv"
    echo "using $REPO/sessions/0001.csv and 0002.csv"
fi

CC="${CC:-$(command -v clang || command -v cc)}"
"$CC" -std=gnu11 -O2 -ffp-contract=off -Wall -Wextra -Werror \
    -I "$HERE/host_include" -I "$REPO/sysmodules/rosalina/include" \
    "$REPO/sysmodules/rosalina/source/battery/battery_curve.c" "$HERE/curve_host_test.c" \
    -lm -o "$OUT/curve_host_test"
"$CC" -std=gnu11 -O2 -ffp-contract=off -Wall -Wextra -Werror \
    -I "$HERE/host_include" -I "$REPO/sysmodules/rosalina/include" \
    "$REPO/sysmodules/rosalina/source/battery/battery_curve.c" \
    "$REPO/sysmodules/rosalina/source/battery/battery_led_logic.c" \
    "$REPO/sysmodules/rosalina/source/battery/battery_ptm_patch_logic.c" "$HERE/led_host_test.c" \
    -o "$OUT/led_host_test"
"$CC" -std=gnu11 -O2 -ffp-contract=off -Wall -Wextra -Werror -Wno-format \
    -I "$HERE/host_sd" -I "$HERE/host_include" -I "$REPO/sysmodules/rosalina/include" \
    "$REPO/sysmodules/rosalina/source/battery/battery_curve.c" \
    "$REPO/sysmodules/rosalina/source/battery/battery_curve_sd.c" \
    "$REPO/sysmodules/rosalina/source/battery/battery_sessions.c" "$HERE/sd_host_test.c" \
    -o "$OUT/sd_host_test"
"$CC" -std=gnu11 -O2 -ffp-contract=off -Wall -Wextra -Werror \
    -I "$HERE/host_include" -I "$REPO/sysmodules/rosalina/include" \
    "$REPO/sysmodules/rosalina/source/battery/battery_led_logic.c" \
    "$REPO/sysmodules/rosalina/source/battery/battery_ptm_patch_logic.c" "$HERE/ptm_host_test.c" \
    -o "$OUT/ptm_host_test"
PTM_CODE="${PTM_CODE:-$REPO/sessions/ptm/0004013020002202.dec.code}"

if [ $# -eq 0 ]; then
    echo "No session CSVs (sessions/0001.csv + 0002.csv not found, none given):"
    echo "curve comparison, session parsing and LED replay are SKIPPED; running the unit tests only."
    echo
    status=0
    "$OUT/led_host_test" || status=1
    echo
    "$OUT/sd_host_test" "$OUT/sd_unit" || status=1
    echo
    "$OUT/ptm_host_test" "$PTM_CODE" || status=1
    echo
    [ $status -eq 0 ] && echo "PASS (unit tests only, replay skipped)" || echo "FAIL"
    exit $status
fi

"$OUT/curve_host_test" "$OUT/c.bin" "$@" | tee "$OUT/c.txt"
python3 "$REPO/scripts/build_curve.py" --quiet --include-high-end --include-incomplete \
    --bin "$OUT/py.bin" "$@" > "$OUT/py.txt"

status=0
if cmp -s "$OUT/c.bin" "$OUT/py.bin"; then
    echo "curve.bin: C and build_curve.py are byte-identical ($(wc -c < "$OUT/c.bin" | tr -d ' ') bytes)"
else
    echo "curve.bin: MISMATCH between C and build_curve.py"
    cmp -l "$OUT/c.bin" "$OUT/py.bin" | head
    status=1
fi

python3 - "$OUT/c.txt" "$OUT/py.txt" <<'PY' || status=1
import re, sys
def rows(path):
    out = []
    for line in open(path):
        if line.startswith("self-check") or line.startswith("host test"):
            break
        if line.strip():
            out.append(line.split())
    return out
c, p = rows(sys.argv[1]), rows(sys.argv[2])
num = re.compile(r"^-?\d+(\.\d+)?$")
bad = 0
if len(c) != len(p):
    print("report: different line counts (%d vs %d)" % (len(c), len(p))); sys.exit(1)
for a, b in zip(c, p):
    if len(a) != len(b):
        bad += 1; print("report differs:", a, b); continue
    for x, y in zip(a, b):
        if num.match(x) and num.match(y):
            if abs(float(x) - float(y)) > 0.0101:
                bad += 1; print("report differs:", " ".join(a), "|", " ".join(b)); break
        elif x != y:
            bad += 1; print("report differs:", " ".join(a), "|", " ".join(b)); break
print("report: C and build_curve.py agree" if bad == 0 else "report: %d differing lines" % bad)
sys.exit(1 if bad else 0)
PY

# Recorder v2 copies + fake SD cards (v1 and v2) for the console's SD code
numbers=()
v2files=()
for csv in "$@"; do
    num=$(sed -n 's/^# session=\([0-9]*\).*/\1/p' "$csv" | head -1)
    numbers+=("$num")
    mkdir -p "$OUT/sd1/luma/battery/sessions" "$OUT/sd2/luma/battery/sessions"
    cp "$csv" "$OUT/sd1/luma/battery/sessions/$num.csv"
    python3 - "$csv" "$OUT/sd2/luma/battery/sessions/$num.csv" <<'PY'
import sys
out = open(sys.argv[2], "w")
for line in open(sys.argv[1]):
    line = line.rstrip("\r\n")
    if line.startswith("# luma3ds-gauge recorder v1"):
        line = "# luma3ds-gauge recorder v2"
    elif line.startswith("t_ms,"):
        line += ",corrected,led_desired,led_actual,reasserts"
    elif line[:1].isdigit():
        line += ",12.34,5,5,0"
    out.write(line + "\n")
PY
    v2files+=("$OUT/sd2/luma/battery/sessions/$num.csv")
done

python3 "$REPO/scripts/build_curve.py" --quiet --include-high-end --include-incomplete \
    --bin "$OUT/py_v2.bin" "${v2files[@]}" > /dev/null
if cmp -s "$OUT/py.bin" "$OUT/py_v2.bin"; then
    echo "build_curve.py: CSV v1 and v2 give the same curve.bin"
else
    echo "build_curve.py: CSV v1 and v2 give DIFFERENT curve.bin"
    status=1
fi

echo
"$OUT/sd_host_test" "$OUT/sd1" "$OUT/sd2" "$OUT/py.bin" "${numbers[@]}" || status=1
echo
"$OUT/led_host_test" "$OUT/py.bin" "$@" || status=1
echo
"$OUT/ptm_host_test" "$PTM_CODE" || status=1
echo

if [ -n "${GRAPH_OUT:-}" ]; then
    mkdir -p "$GRAPH_OUT"
    "$CC" -std=gnu11 -O2 -ffp-contract=off -Wall -Wextra -Werror -Wno-format \
        -I "$HERE/host_graph" -I "$HERE/host_include" -I "$REPO/sysmodules/rosalina/include" \
        "$REPO/sysmodules/rosalina/source/battery/battery_graph.c" \
        "$REPO/sysmodules/rosalina/source/battery/battery_curve.c" "$HERE/graph_host_render.c" \
        -o "$OUT/graph_host_render"
    "$OUT/graph_host_render" "$OUT/c.bin" "$GRAPH_OUT/graph"
    "$OUT/graph_host_render" - "$GRAPH_OUT/graph_nocurve"
    echo "graph rendered to $GRAPH_OUT"
fi

grep -q "^host test: OK" "$OUT/c.txt" || status=1
[ $status -eq 0 ] && echo "PASS" || echo "FAIL"
exit $status
