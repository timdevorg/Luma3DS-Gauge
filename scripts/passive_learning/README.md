# Passive learning prototype

Host-side prototype for learning the Battery Manager curve from ordinary partial play
(stints on battery with sleep, power-off and charging in between) instead of complete
full-charge-to-shutdown discharges. Algorithm, results and the console recommendation:
[`docs/passive-learning.md`](../../docs/passive-learning.md).

Python 3, **standard library only** (no numpy). Both scripts import `scripts/build_curve.py`
for the grid, the reference curve build and the `curve.bin` packing.

| File | What |
|---|---|
| `passive.py` | Library + CLI: CSV to stints, the random-effects fit, coverage gate, confidence band, candidate `curve.bin` |
| `validate.py` | Validation harness: cuts full sessions into simulated play stints and compares the rebuilt curve with `build_curve.py`'s |

## Build a candidate from recorder CSVs

```sh
cd scripts/passive_learning
# with the existing curve as prior (normal case)
./passive.py --prior ../../sessions/curve.bin ../../sessions/0001.csv ../../sessions/0002.csv
# no curve yet: stock gauge as prior
./passive.py --prior-identity ../../sessions/*.csv
# write the candidate (same curve.bin v1 layout, flags bit1 = passive, min/max = 2-sigma band)
./passive.py --prior ../../sessions/curve.bin ../../sessions/*.csv --bin /tmp/candidate.bin
```

Each CSV is split into stints at `# sleep` lines, charger samples and time gaps over 60 s; the
last stint counts as "to shutdown" when the file ends with `# end reason=shutdown|cutoff`.
Options: `--trim` (minutes dropped at each stint start, default 5), `--sigma-k` (expected load
spread between stints, log scale; default 1.0 with `--prior`, 0.3 with `--prior-identity`).

The output lists every stint (load factor `k` in minutes per real %, leave-one-out misfit, used
or dropped), the gate result, prior vs candidate at a few MCU %, and the per-block coverage.

## Run the validation

```sh
cd scripts/passive_learning
./validate.py                 # all experiments, 200 seeds (about 40 min)
./validate.py --seeds 100     # what docs/passive-learning.md shows (about 20 min)
./validate.py --quick         # 40 seeds
./validate.py --only A        # one experiment: A (incl. A2), B (incl. B2), B3, C, D, E, F, G, H, I, J, K
./validate.py --sessions /path/to/sessions
```

Needs `0001.csv` and `0002.csv` (full discharges) in the sessions folder. Prints Markdown
tables: median / 90th percentile over the seeds of the absolute error in real % points,
per MCU region and at the MCU % where the reference curve crosses real 70 / 40 / 10 / 3 %,
plus how often the coverage gate would have allowed the candidate.

AI disclosure: written with the help of AI (Claude Code).
