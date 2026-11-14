#!/usr/bin/env bash
# Host test for the Battery Manager curve code.
#
# Usage: tests/battery/run_host_test.sh session.csv [more.csv ...]
#
# Compiles sysmodules/rosalina/source/battery/battery_curve.c natively (clang or cc),
# builds a curve from the given CSVs with it and with scripts/build_curve.py, and checks:
#   - both curve.bin files are byte-identical
#   - the printed summary / report values agree (within 0.01, C uses float for lookups)
#   - the C self-checks pass (monotonic, continuous, checksum)
#
# GRAPH_OUT=<dir>: also renders the on-console graph (battery_graph.c) into
# <dir>/graph_full.ppm, graph_zoom.ppm and graph_nocurve_full.ppm / _zoom.ppm.
#
# AI disclosure: written with the help of AI (Claude Code), reviewed by the maintainer.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/battery-host-test.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

[ $# -ge 1 ] || { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

CC="${CC:-$(command -v clang || command -v cc)}"
"$CC" -std=gnu11 -O2 -ffp-contract=off -Wall -Wextra -Werror \
    -I "$HERE/host_include" -I "$REPO/sysmodules/rosalina/include" \
    "$REPO/sysmodules/rosalina/source/battery/battery_curve.c" "$HERE/curve_host_test.c" \
    -lm -o "$OUT/curve_host_test"

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
