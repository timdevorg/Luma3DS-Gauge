# Passive learning of the battery curve (research + prototype)

Status: **host prototype and validation only, no console code.** Prototype:
[`scripts/passive_learning/`](../scripts/passive_learning/README.md) (Python 3, stdlib only).
Background: [`battery-manager.md`](battery-manager.md), sections 5 (recording guide), 11 (curve
builder), 12 (LED control) and 17 (time remaining).

**Decision for v13.5-gauge.1: not built on the console.** It is not worth it for now. It needs tens of
hours of stints before it beats the prior. The heavy-load session 0004 (battery-manager.md 11.8)
also answers 6.7 step 2: the MCU % → real % curve itself shifts with load, by ~6–7 points below stock
50 %. Stints at mixed loads would therefore mix curves. The console keeps Build curve from full
discharges. This document and the prototype stay as the basis if it is picked up again.

**In short.** A random-effects fit (one load factor per play stint, shrunk towards a common load;
19 MCU blocks; exact least squares; the existing curve as prior) recovers the curve from partial
play, but slowly: from scratch it needs ~32–64 h of play (4–9 battery cycles) plus one run to
shutdown before it is within ~2–4 real % points (90th percentile). With a full-discharge curve as
prior it keeps that curve within ~1–3 points and corrects a wrong low end over tens of hours. Load
that correlates with the charge level is a fundamental blind spot. Recommendation: passive stint
recording plus a **shadow candidate** in Diagnostics, applied only by the user, on top of a
full-discharge base curve (section 6, ~1 200 lines, 4–6 days).

## 1. The question

Today the curve (MCU % → real %) is learned only from **complete** discharges: full charge, no
sleep, no charger, until the console shuts itself off. On this console that is ~7.3 h of
continuous play per session. Nobody does that in normal use. The goal: let the curve improve from
ordinary play, 30–90 minute stints with sleep, power-off and charging top-ups in between, at
varying load.

Real % keeps its meaning from section 11: **the share of the remaining runtime**, i.e. for a
complete session `100 * (T_end - t) / T_end`. Under a constant load real % falls linearly in time.

### 1.1 What the two recorded sessions look like

| | 0001 | 0002 |
|---|---|---|
| runtime | 7.47 h | 7.21 h |
| start → end MCU % | 97.8 → 1.0 | 98.5 → 1.0 |
| MCU step sizes (1/256 %) | mostly 36–47 above ~20 %, 74–112 at 5–20 %, 3–4 below 5 % | same |
| time below MCU 5 % | 2.3 h (31 % of the runtime) | 2.0 h (31 %) |
| plateau at exactly 1.000 % | last 18 min | last 15 min |

Both are mostly idle Animal Crossing at similar load, so they differ by at most 1.6 real % points
(build_curve's "max spread"). That matters for the validation: **the data cannot show how the
curve changes with load**; load differences are simulated by stretching time (section 4).

Quantisation: above ~20 % the MCU % moves every ~60–70 s in steps of ~0.15 %, at 5–20 % every
2–4 min in steps of 0.3–0.44 %, below 5 % in steps of ~0.012–0.016 %. The 161-point MCU grid of
section 11.3 (0.1 below 5 %, 0.5 at 5–20 %, 1 above) matches this; crossing times are
interpolated between samples like build_curve does.

## 2. Algorithm

### 2.1 Starting point and why it had to change

The proposal was a multiplicative model: time a stint *s* spends in MCU bin *j* is
`dt[s,j] = k[s] * d[j]` (*k* = load factor of the stint, *d* = bin duration at reference load),
fitted by alternating least squares (ALS) with the existing curve as prior for uncovered bins.

That is the right model, but fitted naively it **does not learn anything across MCU regions**:

- Each stint only covers a slice of the MCU range (median 12 grid bins, 90th percentile 35).
  Its own *k* is free, so the stint can only say how time is split *inside its slice*. How long
  MCU 2–3 % lasts compared with MCU 60–70 % is only learnable through chains of stints whose
  slices overlap (or through the prior).
- Plain ALS converges along such chains extremely slowly (diffusion through the chain) and the
  prior wins: with 16 h of simulated stints and the stock gauge as prior it stayed at a max error of
  16–22 points (prior alone: 26), even with 300 iterations (table D).
- The same confounding is real, not a numerical artefact: if the load varies freely between
  stints, the relative length of disjoint MCU regions is genuinely unidentified.

### 2.2 The fix: loads as random effects, durations in blocks, solved exactly

1. **Stints.** A stint is an unbroken stretch on battery, cut at charger, sleep / lid close,
   shutdown and gaps between polls (> 60 s in the CSV, > 3 min in the LED thread). The first
   **5 minutes** of each stint are dropped (relaxation after wake / unplug, 2.6). Stints shorter
   than 10 min or with fewer than 3 complete bins are ignored.
2. **Bin times.** For each MCU grid point crossed inside the stint, the crossing time (first time
   the MCU % is ≤ the grid point, interpolated; exactly the build_curve rule). A bin counts only
   if both its edges were crossed inside the stint, so partial bins at the stint edges are dropped
   (at most one bin, ≤ ~5 min, per edge). A stint that ends in a shutdown crosses every grid point
   below its last value at the last sample (build_curve's "never reached → T_end"), which yields
   the **tail**: the time from MCU 1.0 % to the cut-off.
3. **Blocks.** The 160 bins are grouped into 19 blocks (MCU % edges 0, 1, 1.5, 2, 2.5, 3, 4, 5, 7,
   10, 15, 20, 30 … 90, 98). Each block holds ≥ ~1 % of the runtime at the low end and 5–10 % at
   the top, so the time a stint spends in a block is a usable log observation.
4. **Model per (stint, block)**:

   ```
   y[s,B] = log( time of stint s in the covered bins of block B / covered share of B )
          = a[s] + b[B] + noise
   a[s] = log k[s]  ~ N(mu, sigma_k^2)          stint load, shrunk towards the typical load mu
   b[B] = log D[B]  ~ N(log D0[B], sigma_0^2)   block duration, prior = the base curve
   noise variance   = sigma_m^2 + 2 e_t^2 / dt^2   (load wobble 0.15; crossing-time error 0.5 min)
   ```

   The *covered share* of a partly covered block comes from the current fine bin durations.
   This is linear least squares in (a, b, mu): solved **exactly** from the normal equations
   (dense elimination; on the console the stint unknowns are eliminated first, leaving a
   20 × 20 system, 6.5).
   `sigma_k` is the key knob: 0 means "every stint has the same load" (pooled averages, no
   chain needed, but biased if the load correlates with the charge level); ∞ is the
   unidentified model of 2.1. There is **no setting that is both fast and safe** (tables D, I,
   K): 0.3 learns fastest from scratch, but if the load correlates with the charge level (heavy
   play right after charging, idle near empty) it pulls even a correct curve 6–8 points off;
   1.0 is immune to that but moves away from the prior slowly. Recommended: **1.0 with a
   full-discharge curve as prior, 0.3 only from scratch** (6.3).
   `sigma_0` (trust in the prior): 0.5 with a full-discharge curve (table J), 2 for the stock
   gauge (essentially no trust).
5. **Fine bins.** With the stint loads fixed, each bin's load-corrected time is pooled over the
   stints that crossed it, `(lam * d_prior + sum dt/k) / (lam + n)` with lam = 0.25, and only
   used for the **shares inside each block**; the block totals come from step 4. Steps 4–5 are
   repeated 3 times (the shares feed the covered-share term; 1 iteration is clearly worse,
   3 and 10 are the same, table D).
6. **Curve.** `R(g)` = sum of the bin durations below grid point *g*;
   `real(g) = 100 * R(g) / R(98 %)`, clamped to 100, monotonic by construction (durations ≥ 0).
   Real 100 % is defined at MCU 98 %, where a full charge starts (0001 and 0002 start at 97.8 and
   98.5 %); this costs at most ~0.3 points at the top compared with build_curve, which uses each
   session's own start.
7. **Outlier check (leave-one-out).** Each stint is compared with the curve fitted from all the
   *other* stints: the largest gap between its cumulative time and `k * cumulative d` over its
   bins, as a fraction of its time. Only bins at least two other stints crossed are checked (a
   stint that is the only witness of a region cannot be judged and is kept). Above **0.10** the
   stint is dropped and the fit repeated. With clean simulated stints (load ±30 %) the 99th
   percentile of this misfit is 0.065; stints whose load changes half way have a median of 0.10.
8. **Confidence.** Per block: number of stints that crossed it, and the posterior standard
   deviation of log D (from the normal matrix). The 2-sigma band on real % is written into the
   min / max tables of the candidate `curve.bin` (so the existing graph draws it as the band).
9. **Coverage gate** (when a candidate may be offered, 6.4):
   - no full-discharge curve yet (stock prior): every block crossed by ≥ 2 stints **and** ≥ 1
     stint that ended in a shutdown;
   - with a full-discharge curve as prior: anything; blocks with < 2 stints simply keep the prior.

### 2.3 Sanity check on the full sessions

Feeding the two complete sessions to the passive fit as two (very long) stints, with the current
`sessions/curve.bin` as prior, reproduces build_curve within 0.3 points everywhere (MCU 50 %:
52.15 vs 51.98, 10 %: 33.54 vs 33.43, 2 %: 13.29 vs 13.25, 1 %: 3.80 vs 3.79). The leftover is
the 98 % normalisation.

### 2.4 Low end and the tail

Below MCU 5 % lies ~31 % of the runtime, and the last ~3.8 % (MCU 1.000 plateau to the cut-off)
is **only** visible in a stint that ends in a shutdown. Everything else, including real % at the
LED thresholds, is relative to that total, so a wrong tail shifts the whole low end:

- With a full-discharge curve as prior, the tail and the uncovered low-end blocks keep the prior
  (in proportion to the covered blocks around them). This is the safe default.
- With only the stock gauge as prior, the tail is assumed to be 1 point instead of ~3.8, and the
  candidate is several points off at real 10 % until a stint has run flat. That is why the gate
  requires one stint to shutdown in that case (table A2: median error at real 10 % 0.6–1.1 points
  with the gate passed, 1.9–7.5 with it failed).
- If the user never runs below ~10 % MCU (table C), blocks below that point are never observed:
  passive learning cannot fix a wrong low end then. It only keeps what the prior says.

### 2.5 Stints after a partial charge

The fit treats them like any other stint (the simulation jumps back up the session to MCU 50–90 %
for 30 % of the cycles). Whether the MCU % after a top-up behaves like the MCU % during a
discharge from full **cannot be checked with the recorded data** (no charging in either
session). The ModelGauge chip estimates SoC from voltage with an internal model, and right after
charging the terminal voltage is high, so the MCU % may read high and then drop faster. Two
safeguards: the 5-minute trim, and a per-stint flag "started after a partial charge" so the
console can show their misfit separately (6.1). Recording a few of those stints and comparing them
with the full-discharge curve is on the hardware test list (6.7).

### 2.6 Relaxation after wake / unplug

Also not in the recorded data. Simulated as an MCU % that reads +0.5 or +1.0 % high at the stint
start and decays with a 5-minute time constant (table E). Without trimming this costs ~2–4 points;
a 5-minute trim recovers most of it; 10 minutes throws away more useful data than it saves.
The real size and decay must be measured on hardware (6.7).

### 2.7 Load variation inside a stint

Violates the model (one *k* per stint). Simulated as a load jump half way through (loads
×0.5 / 1 / 1.5, table F). The damage is moderate because a stint is short compared with the
curve; the leave-one-out check catches part of it. Splitting stints at a detected change of the
MCU % rate would be the next step, but the MCU % is too coarse (one step per minute at the top)
to see a change inside 30 minutes reliably.

## 3. Validation setup (`scripts/passive_learning/validate.py`)

- **Ground truth**: build_curve's curve from the full sessions (0001+0002, or one of them for the
  leave-one-out tests).
- **Simulated use**: battery cycles drawn from 0001 / 0002. A cycle starts at full charge, or (30 %)
  after a partial charge at MCU 50–90 % (the simulation assumes the MCU % then behaves as during
  the discharge, 2.5). It ends at shutdown (20 % of cycles), at real 10 % (the red LED, 40 %), or
  at a random MCU 5–40 % (40 %). Inside a cycle: stints of 30–90 min wall time, separated by gaps
  that skip 2–30 min of the session (sleep drain or power-off; unobserved). Per stint a load
  factor (time stretched ×0.7 / 1 / 1.3 unless stated); optional load change half way, relaxation
  offset, never-below-X behaviour. Stints are cut from the real 10 s samples, so the MCU %
  quantisation is the real one.
- **Amount of data**: total recorded stint time, 4–64 h. For scale: one full discharge here is
  ~7.3 h, so 32 h of play is about 4–5 battery cycles, 64 h about 9.
- **Metric**: absolute error in real % points over the MCU grid, by region, and at the MCU %
  where the ground truth crosses real **70 / 40** (HOME Menu bars) and **10 / 3** (LED).
  Each cell is **median / 90th percentile over 100 seeds**. 1 real % point ≈ 4.4 min of play at
  this load.
- **Gate ok**: share of seeds where the coverage gate (2.2, step 9) would allow the candidate.

Full tables (all columns, incl. max in 1–5 %): run `validate.py --seeds 100`. Below, the columns
that matter, copied from that run (J and K: 50 seeds).

## 4. Results

### 4.1 From scratch (stock gauge as prior), error vs amount of data — table A

| load spread, data | max (any MCU) | mean 1–5 % | mean 5–20 % | mean 20–50 % | mean 50–98 % | @70 | @40 | @10 | @3 | gate ok |
|---|---|---|---|---|---|---|---|---|---|---|
| stock gauge alone | 26.3 | 18.4 | 21.9 | 8.6 | 2.2 | 3.9 | 14.0 | 8.3 | 2.0 | – |
| same load, 8 h | 13.3 / 26.7 | 10.1 / 18.7 | 11.0 / 23.3 | 7.6 / 18.8 | 2.9 / 8.1 | 3.1 / 9.1 | 9.1 / 20.3 | 7.7 / 8.4 | 1.8 / 2.1 | 0 % |
| same load, 16 h | 7.7 / 12.0 | 6.0 / 9.3 | 5.5 / 8.9 | 4.4 / 7.3 | 1.8 / 3.4 | 1.8 / 4.0 | 5.0 / 8.0 | 7.4 / 7.7 | 1.8 / 1.8 | 0 % |
| same load, 32 h | 2.8 / 8.5 | 2.3 / 6.5 | 2.0 / 6.3 | 1.7 / 5.3 | 0.9 / 2.6 | 1.0 / 3.0 | 1.8 / 5.7 | 2.5 / 7.7 | 1.0 / 1.8 | 13 % |
| same load, 64 h | 0.5 / 3.2 | 0.2 / 2.3 | 0.3 / 2.3 | 0.3 / 2.0 | 0.4 / 1.1 | 0.4 / 1.2 | 0.3 / 2.1 | 0.2 / 3.2 | 0.1 / 1.7 | 45 % |
| ×0.85–1.15, 32 h | 2.8 / 7.7 | 2.1 / 6.3 | 2.3 / 5.7 | 1.7 / 4.9 | 1.0 / 2.6 | 1.1 / 2.9 | 2.0 / 5.2 | 1.8 / 7.6 | 0.4 / 1.8 | 12 % |
| ×0.85–1.15, 64 h | 1.2 / 3.0 | 0.8 / 2.6 | 0.8 / 2.5 | 0.8 / 2.3 | 0.5 / 1.3 | 0.6 / 1.5 | 0.8 / 2.4 | 0.7 / 2.9 | 0.3 / 1.7 | 42 % |
| ×0.7–1.3, 8 h | 14.5 / 26.6 | 10.8 / 18.6 | 12.3 / 22.7 | 8.6 / 16.5 | 3.2 / 8.5 | 3.7 / 9.9 | 9.8 / 17.6 | 7.7 / 8.4 | 1.8 / 2.1 | 0 % |
| ×0.7–1.3, 16 h | 8.7 / 13.0 | 6.6 / 10.3 | 6.7 / 10.9 | 4.7 / 8.9 | 2.2 / 5.0 | 2.5 / 5.7 | 5.2 / 9.3 | 7.5 / 7.8 | 1.7 / 1.9 | 3 % |
| ×0.7–1.3, 32 h | 3.6 / 8.7 | 2.6 / 6.8 | 2.5 / 7.0 | 1.9 / 6.3 | 1.2 / 3.6 | 1.4 / 4.3 | 2.2 / 6.6 | 1.9 / 7.7 | 0.6 / 1.8 | 18 % |
| ×0.7–1.3, 64 h | 1.8 / 4.2 | 1.2 / 3.4 | 1.2 / 3.4 | 1.2 / 3.2 | 0.9 / 2.0 | 1.0 / 2.4 | 1.1 / 3.3 | 0.9 / 4.1 | 0.3 / 1.7 | 47 % |

Split by the gate (load ×0.7–1.3) — table A2:

| data | gate | max (any MCU) | mean 1–5 % | @70 | @40 | @10 | @3 |
|---|---|---|---|---|---|---|---|
| 16 h | passed (3 %) | 3.7 / 4.2 | 1.4 / 2.8 | 1.5 / 2.2 | 3.4 / 4.1 | 0.7 / 1.9 | 0.7 / 1.2 |
| 16 h | failed | 8.8 / 13.0 | 6.7 / 10.3 | 2.6 / 5.7 | 5.5 / 9.3 | 7.5 / 7.8 | 1.7 / 1.9 |
| 32 h | passed (18 %) | 1.8 / 3.8 | 1.0 / 3.2 | 0.7 / 2.2 | 1.0 / 3.1 | 1.1 / 3.4 | 0.4 / 0.7 |
| 32 h | failed | 3.8 / 8.7 | 2.9 / 7.3 | 1.7 / 4.5 | 2.6 / 6.8 | 2.1 / 7.7 | 0.9 / 1.8 |
| 64 h | passed (47 %) | 1.3 / 2.7 | 0.9 / 1.9 | 0.9 / 2.1 | 1.0 / 2.0 | 0.6 / 1.6 | 0.2 / 0.5 |
| 64 h | failed | 2.4 / 4.6 | 1.5 / 3.9 | 1.1 / 2.8 | 1.5 / 4.2 | 1.9 / 4.3 | 0.6 / 1.8 |

Reading: from scratch, partial play needs **~32–64 h** (4–9 battery cycles) and at least one
stint run to shutdown before the curve is within ~2–4 points everywhere (90th percentile); at
8–16 h it is still far off at the low end, mostly because the tail and the low blocks are not
covered yet. The gate separates good from bad candidates well. For comparison, one full discharge
gives the curve to within the 1.6-point session-to-session spread.

### 4.2 With a full-discharge curve as prior — tables B, B2, B3, J

Leave-one-out: curve(0001) as prior, pieces of 0002 only, compared with curve(0002) (B2: the
other way round). The two sessions are nearly the same, so the prior alone is already good;
this tests "does passive learning make a good curve worse?". `sigma_k` 0.3, `sigma_0` 0.3:

| data | max | mean 1–5 % | mean 20–50 % | @70 | @40 | @10 | @3 |
|---|---|---|---|---|---|---|---|
| prior curve(0001) alone | 1.60 | 0.37 | 0.57 | 1.17 | 0.51 | 0.17 | 0.20 |
| 4 h | 1.44 / 2.65 | 0.33 / 0.69 | 0.57 / 1.52 | 0.77 / 2.21 | 0.49 / 1.26 | 0.14 / 0.26 | 0.21 / 0.26 |
| 16 h | 1.40 / 2.73 | 0.59 / 1.38 | 0.88 / 2.18 | 1.02 / 2.19 | 0.86 / 2.20 | 0.21 / 0.75 | 0.22 / 0.29 |
| 64 h | 0.93 / 1.80 | 0.45 / 1.08 | 0.66 / 1.52 | 0.52 / 1.40 | 0.70 / 1.48 | 0.30 / 0.74 | 0.14 / 0.27 |
| B2: prior curve(0002) alone | 2.09 | 0.49 | 0.84 | 1.69 | 0.73 | 0.27 | 0.18 |
| B2: 64 h | 1.00 / 1.43 | 0.49 / 0.96 | 0.51 / 1.28 | 0.54 / 1.31 | 0.55 / 1.30 | 0.35 / 0.75 | 0.18 / 0.26 |

The median gets slightly better, but up to ~32 h the **90th percentile is worse than the prior**
(~2.5–3 vs 1.6 points): with noisy loads, a few hours of stints add noise faster than signal. At
the LED thresholds the errors stay small (≤ 0.9 points) because the prior holds the low end.

Wrong prior (B3): curve(0001) with its runtime below MCU 5 % halved, i.e. a cell with much less
hidden reserve than the prior claims; pieces of 0002, compared with curve(0002). And J: the prior
weight, correct vs wrong prior (50 seeds):

| prior, `sigma_0`, data | max | mean 1–5 % | @40 | @10 |
|---|---|---|---|---|
| warped prior alone | 13.0 | 9.1 | 11.5 | 4.2 |
| warped, 0.3, 16 h / 32 h / 64 h | 7.8 / 5.6 / 3.2 (median) | 5.8 / 4.3 / 2.6 | 7.0 / 5.0 / 2.9 | 3.7 / 2.8 / 1.9 |
| warped, 0.5, 16 h / 64 h | 5.3 / 8.7 → 1.7 / 3.7 | 4.2 / 6.6 → 1.2 / 3.0 | 4.6 / 7.8 → 1.4 / 3.7 | 3.2 / 3.9 → 1.1 / 2.8 |
| warped, 1.0, 16 h / 64 h | 3.8 / 7.6 → 1.4 / 3.1 | 2.8 / 5.6 → 0.8 / 2.5 | 2.7 / 5.9 → 0.9 / 3.1 | 2.9 / 3.8 → 0.7 / 2.7 |
| correct, 0.3, 16 h / 64 h | 1.5 / 2.7 → 0.9 / 1.8 | 0.6 / 1.2 → 0.5 / 1.2 | 0.9 / 2.2 → 0.7 / 1.6 | 0.2 / 0.8 → 0.3 / 0.8 |
| correct, 0.5, 16 h / 64 h | 2.0 / 3.7 → 1.1 / 2.2 | 0.9 / 1.8 → 0.6 / 1.4 | 1.3 / 3.3 → 0.8 / 1.9 | 0.3 / 1.1 → 0.4 / 1.1 |
| correct, 1.0, 16 h / 64 h | 2.6 / 4.5 → 1.1 / 2.3 | 1.2 / 2.5 → 0.7 / 1.5 | 1.7 / 4.3 → 0.8 / 2.2 | 0.3 / 1.6 → 0.5 / 1.2 |

(`a / b → c / d` = median / 90th percentile at the first and second amount of data.)
A wrong low end is corrected, but only as fast as stints actually reach the low end; `sigma_0`
0.5 is the compromise (a correct prior stays within ~2 points at 64 h, a badly wrong one gets
from 13 to ~1.7 median).

### 4.3 Load correlated with the charge level — tables I and K

Heavy play (×0.7) above MCU 60 %, light (×1.3) below 30 %. This is the realistic worst case
(play right after charging, leave it idling near empty), and it is **not identifiable** from
stint data: "the top is short because the load was heavy" and "the top is short" look the same.

| case (32 h unless stated) | max | mean 20–50 % | @40 | @10 |
|---|---|---|---|---|
| stock prior, `sigma_k` 0.3, 64 h | 8.8 / 9.9 | 8.7 / 9.5 | 8.7 / 9.7 | 2.9 / 3.9 |
| stock prior, `sigma_k` 1.0, 64 h | 2.4 / 4.9 | 1.9 / 3.5 | 1.9 / 3.9 | 1.3 / 4.0 |
| correct curve prior, `sigma_k` 0.3 | 8.5 / 9.1 | 8.3 / 8.9 | 8.3 / 9.1 | 2.1 / 2.8 |
| correct curve prior, `sigma_k` 1.0 | 2.1 / 2.6 | 2.1 / 2.6 | 2.1 / 2.6 | 0.5 / 0.9 |
| correct curve prior, uncorrelated, `sigma_k` 0.3 | 1.5 / 3.1 | 1.1 / 2.8 | 1.1 / 3.0 | 0.5 / 1.3 |
| correct curve prior, uncorrelated, `sigma_k` 1.0 | 0.7 / 1.2 | 0.4 / 1.0 | 0.5 / 0.9 | 0.2 / 0.6 |
| warped prior, uncorrelated, `sigma_k` 0.3 / 1.0 | 3.6 / 5.6 vs 5.6 / 10.0 | 3.0 / 5.4 vs 5.4 / 8.6 | 3.1 / 5.5 vs 5.5 / 9.1 | 2.1 / 3.7 vs 3.1 / 3.9 |

(K rows: `sigma_0` 0.5, 50 seeds.) With `sigma_k` 0.3 the correlated load turns a correct curve
(1.6) into one that is 8 points off around MCU 20–50 %. With `sigma_k` 1.0 that damage stays at
~2 points and a correct prior is kept tighter, at the price of correcting a wrong prior more
slowly. This is the main reason the recommendation is "refine a full-discharge curve, in shadow
mode", not "learn from scratch".

### 4.4 Other findings — tables C, D, E, F, G, H

- **Never below MCU 8–30 %** (C): from scratch the gate never passes and the candidate is
  *worse* than the stock gauge in the middle (mean 20–50 % error 15–20 vs 8.6), because the top
  shape is learned but normalised against the stock gauge's wrong low end. With a curve prior
  it stays within ~1–2.5 points; a wrong low end is **not** corrected (12.9, unchanged).
- **Plain ALS** (the starting proposal, D): 9.5 / 20 max error at 32 h even with 300 iterations,
  vs 3.6 / 8.7 for the random-effects fit.
- **Outer iterations** (D): 1 → 4.4 / 9.1, 3 → 3.6 / 8.7 (10 is the same as 3).
- **Relaxation** (E, synthetic): +0.5 % offset, no trim 6.2 / 14.7, 5 min trim 3.7 / 9.7 (no
  offset: 3.6 / 8.7); +1 %: 5 min trim 5.7 / 11.4, 10 min trim 4.0 / 9.4. The gate never
  passed with an offset because the simulated offset was still there when a short final stint hit
  the cut-off, so the tail was lost; on hardware the MCU % at the cut-off is not offset, but this
  shows how sensitive the tail is.
- **Load change inside a stint** (F, loads ×0.5 / 1 / 1.5): none 4.7 / 8.2, 30 % of stints
  5.6 / 9.8, every stint 8.2 / 14.8. The leave-one-out outlier check **does not help** here
  (8.7 / 15.1, and it throws away 17 of ~35 stints, so the gate passes less often). Partly
  bent stints are not outliers, just noisier data.
- **Corrupted stints** (G: two stints whose time below MCU 20 % runs twice as long, into 32 h):
  3.6 / 6.9 with or without the check; the check did not flag them (their own load factor
  absorbs most of the distortion, and there are too few other stints in the same blocks).
  The outlier check is therefore only a guard against gross garbage (wrong clock, a stint
  recorded with the charger flag missing), not a quality filter.
- **Stint length** (H) at 32 h: 15–30 min 8.4 / 13.0, 30–90 min 3.6 / 8.7, 90–180 min 2.5 / 8.7.
  Longer stints carry more cross-region information per hour; very short stints are almost
  worthless because each costs a 5-minute trim and two partial edge bins.

## 5. Failure modes (summary)

1. **Load / region confounding** (2.1, 4.3). Fundamental. Stint data alone cannot tell a heavier
   load from a shorter MCU region. Shrinking loads towards a common value helps only if the load
   does not correlate with the charge level; if it does, the curve is pulled off by up to 8
   points. Mitigation: a full-discharge curve as prior, `sigma_k` 1.0, shadow mode, and showing
   the correlation of stint load vs MCU % in Diagnostics (6.4).
2. **The tail and the low end need deep stints** (2.4). The last ~31 % of the runtime lies below
   MCU 5 %, the last ~3.8 % (the 1.000 plateau) is only seen in a stint that ends in a shutdown.
   A user who always charges early never teaches the low end, and from scratch the result is then
   worse than the stock gauge in the middle.
3. **Noise with little data.** Up to ~32 h, even with a good prior the 90th percentile error is
   ~2.5–3 points vs 1.6 for the prior alone. Passive candidates must not be applied automatically.
4. **Relaxation and top-ups** are untested on hardware; the 5-minute trim handles the synthetic
   version. Unknown real size (6.7).
5. **Load changes inside stints** degrade the fit gradually and are not caught by the outlier
   check.
6. **The voltage table is not learned.** Below MCU 3 % the corrected % blends in the voltage
   table (section 11.3). A passive candidate keeps the base curve's voltage table; from scratch
   there is none, so the plateau at MCU 1.000 is not resolved. Learning it needs stints to
   shutdown as well (6.8, optional part).
7. **The validation data is narrow**: two near-identical idle sessions of one cell at room
   temperature. Load effects are simulated by stretching time, which assumes the MCU % → real %
   relation itself does not change with load. In reality heavier load sags the voltage and the
   ModelGauge chip reads lower, so the curve itself shifts with load; this prototype cannot
   measure that.

## 6. Recommendation for the console

**Do it, but as a refinement of a full-discharge curve, in shadow mode, never applied
automatically.** Passive learning reliably keeps a good curve good and slowly corrects a wrong one
where the user actually plays; it does not replace one complete discharge as the way to get the
low end right. From scratch it needs ~32–64 h of play and one run to shutdown before the gate
passes, and it is fragile when the load correlates with the charge level.

Concretely:

- Keep **Build curve** (full discharges) as the primary way to make a curve; the README's "record
  one full discharge" advice stays.
- Add **passive stint recording** (always on, read-only, no MCU writes) and a **candidate** in
  Diagnostics. The user applies it by hand.
- As an easier alternative to a 7 h session, the docs can suggest a **"deep" session**: from any
  charge level, play down to shutdown (from MCU 10 % that is ~2.5 h here). Every such stint fixes
  the tail and the low end, which is exactly what passive data lacks.

### 6.1 What gets recorded, and when

Hooked into the LED thread's existing poll (1 s with the Power LED corrected, 10 s otherwise;
section 12.4), not the CSV recorder:

- **Start**: on battery, i.e. after the debounced unplug (13.3) or at boot when not charging, and
  after waking from sleep. The first 5 minutes are not used (trim), so the start can be lazy.
- **End**: charger on (debounced); sleep request / lid close (0x213 path); shutdown notification
  (0x108 / pre-termination); a gap of more than 3 minutes between polls; a curve reload is *not*
  a boundary (stints store MCU % crossings, not corrected %).
- **Per poll**: only the first-crossing logic of section 11.2 on the MCU %: next grid index,
  previous (t, pct), and a `u16` bin-time array. No floats needed while recording.
- **Flags per stint**: ended in shutdown (shutdown notification with MCU ≤ 1.5 % and voltage near
  the cut-off, or the existing recorder's `cutoff` detection), started after a partial charge
  (charger seen with MCU < 95 % at unplug), started after wake.
- **Written** once at the end of the stint (one 340-byte slot write). For a stint that ends at the
  cut-off, the shutdown handler writes the slot, like the recorder's `# end reason=shutdown`
  today; to survive a hard cut-off, the stint in progress is also checkpointed to its slot every
  10 minutes (bounded writes: ≤ 6 per hour).
- Stints shorter than 10 minutes after the trim, or with fewer than 3 complete bins, are not
  written.

### 6.2 On-SD format and size

`/luma/battery/stints.bin`: a header (magic `BSTN`, version, slot count 64, next slot, CRC) and
**64 fixed slots of 340 bytes** (≈ 21.8 KB total), used as a ring (oldest overwritten). Fixed
slots mean one in-place write per stint, no file rewrite, and a torn write damages one slot only.

| Offset | Type | Field |
|---|---|---|
| 0 | u16 | magic / slot version |
| 2 | u16 | flags (bit0 shutdown, bit1 after partial charge, bit2 after wake, bit3 in progress) |
| 4 | u32 | start time (minutes since 1900, from `osGetTime`) |
| 8 | u16 | duration (s / 4) |
| 10 | u8 | first complete bin index (0–159) |
| 11 | u8 | number of complete bins (0–160) |
| 12 | u16 | MCU % ×10 at start, after the trim |
| 14 | u16 | MCU % ×10 at end |
| 16 | u32 | CRC-32 of the slot with this field zeroed (as `curve.bin`) |
| 20 | u16[160] | bin times in seconds (complete bins only, from the first index; rest 0) |

Median stint: 12 bins, 90th percentile 35, maximum seen 81.
RAM while recording: the slot itself (340 B) plus ~16 B of crossing state, inside the static LED
state. The 64-stint ring holds roughly 60–90 h of play, which is the amount the validation says is
needed, and it ages out old data naturally (cell wear, seasons).

### 6.3 Rebuild rule

- **Prior / base curve**: the last curve made by **Build curve** (full discharges), never a
  previous passive candidate, otherwise stints would be counted twice. When a passive candidate
  is applied, the base is kept as `curve.base` (and the old `curve.bin` as `curve.bak`, as today).
  With no base: the stock gauge, `sigma_0` 2.
- **Parameters**: base curve → `sigma_k` 1.0, `sigma_0` 0.5; stock gauge → `sigma_k` 0.3,
  `sigma_0` 2. `sigma_m` 0.15, `e_t` 0.5 min, `lam` 0.25, 3 outer iterations, 5-minute trim.
- **When**: on demand, when the Diagnostics page is opened (milliseconds for the fit, under a second with the
  leave-one-out check, see 6.5). Nothing runs in
  the background.

### 6.4 Safeguards

- **Coverage gate**: from scratch, every block crossed by ≥ 2 stints and ≥ 1 stint to shutdown;
  with a base curve, at least one block with ≥ 2 stints (other blocks keep the base). Below that the
  page shows "learning: n stints, x h, missing: ..." and no candidate.
- **Outlier check**: leave-one-out misfit > 0.10 drops the stint (only judged on bins ≥ 2 other
  stints cover). Treat it as a garbage filter (4.4).
- **Plausibility** (suggested limits, not validated here): refuse a candidate that differs from the base by more than 15 points anywhere,
  or whose implied runtime (k × total d) differs from the base's by more than ±50 %.
- **Shadow mode**: Diagnostics shows candidate vs current at MCU 50 / 25 / 10 / 5 / 3 / 2 / 1 % and
  at the real 70 / 40 / 10 / 3 % thresholds, the 2-sigma band, per-block coverage, stint count and
  hours, and the correlation of stint load with MCU % (a strong correlation = failure mode 1, show
  a warning). Optionally the Curve graph draws the candidate as a second line.
- **Apply only on confirmation**, through the existing safe write (`curve.tmp` → `curve.bak` →
  `curve.bin`); Restore previous curve works unchanged.

### 6.5 What the console can compute cheaply

- Recording: integer / fixed-point only, same cost as today's recorder crossing logic.
- Fit: unknowns are one load per stint (≤ 64), 19 block durations and mu. Each stint's load only
  couples to its own blocks, so it is eliminated while streaming the slots (Schur complement):
  one pass builds a **20 × 20** system, solved by Gaussian elimination (~3 k float ops); a second
  pass gets the stint loads and the fine-bin sums (2 × 160 floats). Three outer iterations →
  6 passes over ≤ 21.8 KB. RAM: 20 × 21 floats (1.7 KB) + 160 × 3 floats (1.9 KB) + one slot
  buffer + 64 loads = **~4 KB**, comparable to the curve build's 12 KB static budget.
- `log` / `exp` per (stint, block) observation (≤ ~600 per pass); float is already used by the
  curve code (VFP on the ARM11).
- Leave-one-out check: 64 refits of the 20 × 20 system with the stint's contribution removed
  (keeping the shares of the full fit): ~64 × 3 k ops plus re-reading each slot, fine in < 1 s.
- Confidence: the diagonal of the inverse of the 20 × 20 matrix (20 more solves).

Nothing here needs the "float-heavy iteration" of plain ALS; the exact solve is cheaper than
300 ALS iterations and much better.

### 6.6 curve.bin compatibility

The candidate fits the **existing v1 format unchanged** (2024 bytes, same grids): mean = candidate,
min / max = the 2-sigma band (so the graph's band shows the uncertainty), number of sessions =
number of stints used capped at 16 (the loader requires 1–16), session numbers and deviations 0,
flags bit1 = "built by passive learning" (the loader ignores unknown bits; bit0 is set only if a
voltage table was copied from the base). `passive.py --bin` writes exactly this. Nothing else on
SD changes; `stints.bin` and `curve.base` are new files.

### 6.7 Hardware tests before trusting it

1. Record a few stints with the CSV recorder: after wake, after unplug from a full charge, after a
   partial charge (e.g. 40 → 70 %). Measure the real size and decay of the MCU % jump (sets the
   trim; checks 2.5 / 2.6).
2. A light and a heavy full session (section 5's open question): does the MCU % → real % curve
   itself shift with load? If yes by more than ~2 points, passive learning mixes curves and
   should weight by load or keep separate curves. *Done (battery-manager.md 11.8): yes, ~6–7 points
   below stock 50 % at full 3D vs 3D off.*
3. Run passive.py on those CSVs against the current curve.bin and compare with Build curve.

### 6.8 Effort estimate (console)

| Part | Size |
|---|---|
| Stint tracker in the LED thread (crossings, boundaries, flags, checkpoint) | ~250 lines |
| `stints.bin` ring (slots, CRC, safe write) | ~150 lines |
| Fit (Schur-complement solve, fine bins, gate, LOO check, band) in a pure `battery_passive.c` | ~400 lines |
| Diagnostics page (shadow comparison, apply with confirmation, `curve.base`) | ~250 lines |
| Host test: C vs `passive.py` on the same stints (like `tests/battery/`) | ~200 lines |

About **1 200–1 300 lines, roughly 4–6 working days** with the existing patterns, plus several
weeks of normal use to collect stints on hardware before the first candidate can be judged.
Optional later: learn the voltage table from stints to shutdown (map each smoothed-voltage
crossing to `k × remaining time`), ~150 more lines.

The C fit must stay in sync with `passive.py`, the same way `battery_curve.c` and
`build_curve.py` are kept in sync.
