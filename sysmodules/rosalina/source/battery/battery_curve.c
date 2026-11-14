/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: curve building and lookup (pure code, no SD / no services).
*
*   Compiled into Rosalina and, unchanged, into the host test (tests/battery/).
*   scripts/build_curve.py implements the same algorithm; keep both in sync
*   (the host test checks that both produce a byte-identical curve.bin).
*/

#include <stddef.h>
#include <string.h>
#include "battery/battery_curve.h"

// ---- Grids ----

void BatteryCurve_MakeGrids(u16 *mcuGridX10, u16 *voltGridX10)
{
    u32 n = 0;
    for (u32 x = 0; x <= 50; x += 1)    // 0.0 .. 5.0, step 0.1
        mcuGridX10[n++] = (u16)x;
    for (u32 x = 55; x <= 200; x += 5)  // 5.5 .. 20.0, step 0.5
        mcuGridX10[n++] = (u16)x;
    for (u32 x = 210; x <= 1000; x += 10) // 21 .. 100, step 1
        mcuGridX10[n++] = (u16)x;

    for (u32 i = 0; i < BATTERY_CURVE_VOLT_POINTS; i++)
        voltGridX10[i] = (u16)(BATTERY_CURVE_VOLT_FIRST_X10 + 10 * i);
}

static u16 g_mcuGrid[BATTERY_CURVE_MCU_POINTS];
static u16 g_voltGrid[BATTERY_CURVE_VOLT_POINTS];
static bool g_gridsReady;

static void batteryCurveGrids(void)
{
    if (!g_gridsReady)
    {
        BatteryCurve_MakeGrids(g_mcuGrid, g_voltGrid);
        g_gridsReady = true;
    }
}

// ---- Per-session streaming ----

// "First time the value is <= grid point" for every grid point, walking the grid downwards.
// Linear interpolation between the previous sample (above the grid point) and this one.
static void batteryCurveCross(const u16 *grid, double *times, s32 *next, bool *hasPrev, double *prevX, double *prevT,
                              double x, double t)
{
    while (*next >= 0 && grid[*next] / 10.0 >= x)
    {
        double g = grid[*next] / 10.0;
        double tc = t;
        if (*hasPrev && *prevX > x)
            tc = *prevT + (t - *prevT) * (*prevX - g) / (*prevX - x);
        times[(*next)--] = tc;
    }
    *hasPrev = true;
    *prevX = x;
    *prevT = t;
}

void BatteryCurve_SessionBegin(BatteryCurveSession *s)
{
    batteryCurveGrids();
    memset(s, 0, sizeof(*s));
    s->mcuNext = BATTERY_CURVE_MCU_POINTS - 1;
    s->voltNext = BATTERY_CURVE_VOLT_POINTS - 1;
}

static bool batteryCurveParseU32(const char **p, u32 *out)
{
    const char *s = *p;
    u32 n = 0;
    if (*s < '0' || *s > '9')
        return false;
    while (*s >= '0' && *s <= '9')
        n = 10 * n + (u32)(*s++ - '0');
    *p = s;
    *out = n;
    return true;
}

static bool batteryCurveSkipField(const char **p)
{
    const char *s = *p;
    while (*s != ',' && *s != 0)
        s++;
    if (*s != ',')
        return false;
    *p = s + 1;
    return true;
}

static u8 batteryCurveMedian(const u8 *win)
{
    u8 v[BATTERY_CURVE_MEDIAN_WINDOW];
    memcpy(v, win, sizeof(v));
    for (u32 i = 1; i < BATTERY_CURVE_MEDIAN_WINDOW; i++) // insertion sort, 7 elements
    {
        u8 x = v[i];
        u32 j = i;
        for (; j > 0 && v[j - 1] > x; j--)
            v[j] = v[j - 1];
        v[j] = x;
    }
    return v[BATTERY_CURVE_MEDIAN_WINDOW / 2];
}

void BatteryCurve_SessionFeedLine(BatteryCurveSession *s, const char *line)
{
    if (line[0] == '#')
    {
        if (strncmp(line, "# sleep ", 8) == 0)
            s->sleepGap = true;
        return;
    }

    // t_ms,wall_ms,pct_int,pct_frac,pct,volt_raw,...
    const char *p = line;
    u32 tMs, pctInt, pctFrac, voltRaw;
    if (!batteryCurveParseU32(&p, &tMs) || *p++ != ',')
        return; // column header or garbage
    if (!batteryCurveSkipField(&p)) // wall_ms (may be negative)
        return;
    if (!batteryCurveParseU32(&p, &pctInt) || *p++ != ',')
        return;
    if (!batteryCurveParseU32(&p, &pctFrac) || *p++ != ',')
        return;
    if (!batteryCurveSkipField(&p)) // pct (decimal, we use int + frac/256)
        return;
    if (!batteryCurveParseU32(&p, &voltRaw) || *p != ',')
        return;
    if (pctInt > 100 || pctFrac > 255 || voltRaw > 255)
        return;

    double t = (double)tMs;
    double pct = (double)pctInt + (double)pctFrac / 256.0;

    if (s->samples == 0)
        s->firstPct = pct;
    s->samples++;
    s->lastPct = pct;
    s->lastT = t;

    batteryCurveCross(g_mcuGrid, s->mcu, &s->mcuNext, &s->mcuHasPrev, &s->mcuPrevX, &s->mcuPrevT, pct, t);

    // Centered median over BATTERY_CURVE_MEDIAN_WINDOW samples, attributed to the middle sample's time
    u32 w = s->voltCount % BATTERY_CURVE_MEDIAN_WINDOW;
    s->voltWin[w] = (u8)voltRaw;
    s->voltWinT[w] = t;
    s->voltCount++;
    if (s->voltCount >= BATTERY_CURVE_MEDIAN_WINDOW)
    {
        u32 mid = (s->voltCount - 1 - BATTERY_CURVE_MEDIAN_WINDOW / 2) % BATTERY_CURVE_MEDIAN_WINDOW;
        double v = (double)batteryCurveMedian(s->voltWin);
        batteryCurveCross(g_voltGrid, s->volt, &s->voltNext, &s->voltHasPrev, &s->voltPrevX, &s->voltPrevT,
                          v, s->voltWinT[mid]);
    }
}

static void batteryCurveTimesToReal(double *v, u32 count, s32 next, double tEnd)
{
    for (s32 i = 0; i <= next; i++) // never reached: the console shut down above this grid point
        v[i] = tEnd;
    for (u32 i = 0; i < count; i++)
        v[i] = 100.0 * (tEnd - v[i]) / tEnd;
}

BatteryCurveSessionError BatteryCurve_SessionEnd(BatteryCurveSession *s)
{
    if (s->sleepGap)
        return BATTERY_CURVE_SESSION_SLEEP_GAP;
    if (s->samples < BATTERY_CURVE_MIN_SAMPLES || s->lastT < (double)BATTERY_CURVE_MIN_DURATION_MS)
        return BATTERY_CURVE_SESSION_TOO_SHORT;

    batteryCurveTimesToReal(s->mcu, BATTERY_CURVE_MCU_POINTS, s->mcuNext, s->lastT);
    batteryCurveTimesToReal(s->volt, BATTERY_CURVE_VOLT_POINTS, s->voltNext, s->lastT);
    return BATTERY_CURVE_SESSION_OK;
}

// ---- Combining sessions ----

static u16 batteryCurveToX100(double real)
{
    if (real <= 0.0)
        return 0;
    if (real >= 100.0)
        return 10000;
    return (u16)(u32)(real * 100.0 + 0.5);
}

void BatteryCurve_AccumInit(BatteryCurveAccum *a)
{
    memset(a, 0, sizeof(*a));
}

static void batteryCurveAccumTable(double *sum, u16 *mn, u16 *mx, const double *v, u32 count, bool first)
{
    for (u32 i = 0; i < count; i++)
    {
        u16 x = batteryCurveToX100(v[i]);
        sum[i] += v[i];
        if (first || x < mn[i])
            mn[i] = x;
        if (first || x > mx[i])
            mx[i] = x;
    }
}

void BatteryCurve_AccumAdd(BatteryCurveAccum *a, const BatteryCurveSession *s)
{
    bool first = a->n == 0;
    batteryCurveAccumTable(a->mcuSum, a->mcuMin, a->mcuMax, s->mcu, BATTERY_CURVE_MCU_POINTS, first);
    batteryCurveAccumTable(a->voltSum, a->voltMin, a->voltMax, s->volt, BATTERY_CURVE_VOLT_POINTS, first);
    a->n++;
}

// Non-decreasing along the (ascending) grid. A no-op for tables built here; kept as a guarantee.
static void batteryCurveMonotonic(u16 *v, u32 count)
{
    for (u32 i = 1; i < count; i++)
        if (v[i] < v[i - 1])
            v[i] = v[i - 1];
}

void BatteryCurve_AccumFinish(const BatteryCurveAccum *a, BatteryCurveFile *out)
{
    batteryCurveGrids();
    memset(out, 0, sizeof(*out));
    out->magic = BATTERY_CURVE_MAGIC;
    out->version = BATTERY_CURVE_VERSION;
    out->numSessions = (u16)a->n;
    out->flags = BATTERY_CURVE_FLAG_VOLT_VALID;
    out->mcuPoints = BATTERY_CURVE_MCU_POINTS;
    out->voltPoints = BATTERY_CURVE_VOLT_POINTS;
    memcpy(out->mcuGridX10, g_mcuGrid, sizeof(g_mcuGrid));
    memcpy(out->voltGridX10, g_voltGrid, sizeof(g_voltGrid));

    if (a->n == 0)
        return;

    for (u32 i = 0; i < BATTERY_CURVE_MCU_POINTS; i++)
        out->mcuMean[i] = batteryCurveToX100(a->mcuSum[i] / a->n);
    for (u32 i = 0; i < BATTERY_CURVE_VOLT_POINTS; i++)
        out->voltMean[i] = batteryCurveToX100(a->voltSum[i] / a->n);
    memcpy(out->mcuMin, a->mcuMin, sizeof(out->mcuMin));
    memcpy(out->mcuMax, a->mcuMax, sizeof(out->mcuMax));
    memcpy(out->voltMin, a->voltMin, sizeof(out->voltMin));
    memcpy(out->voltMax, a->voltMax, sizeof(out->voltMax));

    batteryCurveMonotonic(out->mcuMean, BATTERY_CURVE_MCU_POINTS);
    batteryCurveMonotonic(out->mcuMin, BATTERY_CURVE_MCU_POINTS);
    batteryCurveMonotonic(out->mcuMax, BATTERY_CURVE_MCU_POINTS);
    batteryCurveMonotonic(out->voltMean, BATTERY_CURVE_VOLT_POINTS);
    batteryCurveMonotonic(out->voltMin, BATTERY_CURVE_VOLT_POINTS);
    batteryCurveMonotonic(out->voltMax, BATTERY_CURVE_VOLT_POINTS);
}

u16 BatteryCurve_Deviation(const BatteryCurveAccum *a, const BatteryCurveSession *s)
{
    if (a->n < 2)
        return 0;

    double total = 0.0;
    for (u32 i = 0; i < BATTERY_CURVE_MCU_POINTS; i++)
    {
        double others = (a->mcuSum[i] - s->mcu[i]) / (a->n - 1);
        double d = s->mcu[i] - others;
        total += d < 0.0 ? -d : d;
    }
    return batteryCurveToX100(total / BATTERY_CURVE_MCU_POINTS);
}

void BatteryCurve_FileFinalize(BatteryCurveFile *f)
{
    u16 spread = 0;
    for (u32 i = 0; i < BATTERY_CURVE_MCU_POINTS; i++)
        if (f->mcuMax[i] - f->mcuMin[i] > spread)
            spread = (u16)(f->mcuMax[i] - f->mcuMin[i]);
    f->maxSpreadX100 = spread;
    f->checksum = BatteryCurve_Crc32(f, offsetof(BatteryCurveFile, checksum));
}

// ---- Validation ----

u32 BatteryCurve_Crc32(const void *data, u32 len)
{
    const u8 *p = (const u8 *)data;
    u32 crc = 0xFFFFFFFFu;
    for (u32 i = 0; i < len; i++)
    {
        crc ^= p[i];
        for (u32 b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool BatteryCurve_FileIsValid(const BatteryCurveFile *f)
{
    if (f->magic != BATTERY_CURVE_MAGIC || f->version != BATTERY_CURVE_VERSION ||
        f->mcuPoints != BATTERY_CURVE_MCU_POINTS || f->voltPoints != BATTERY_CURVE_VOLT_POINTS ||
        f->numSessions == 0 || f->numSessions > BATTERY_CURVE_MAX_SESSIONS)
        return false;
    if (BatteryCurve_Crc32(f, offsetof(BatteryCurveFile, checksum)) != f->checksum)
        return false;

    for (u32 i = 1; i < BATTERY_CURVE_MCU_POINTS; i++)
        if (f->mcuGridX10[i] <= f->mcuGridX10[i - 1])
            return false;
    for (u32 i = 1; i < BATTERY_CURVE_VOLT_POINTS; i++)
        if (f->voltGridX10[i] <= f->voltGridX10[i - 1])
            return false;
    return true;
}

// ---- Lookup ----

float BatteryCurve_Interp(const u16 *gridX10, const u16 *values, u32 count, float xX10)
{
    if (xX10 <= gridX10[0])
        return values[0] / 100.0f;
    if (xX10 >= gridX10[count - 1])
        return values[count - 1] / 100.0f;

    u32 lo = 0, hi = count - 1; // gridX10[lo] < x < gridX10[hi]
    while (hi - lo > 1)
    {
        u32 mid = (lo + hi) / 2;
        if (gridX10[mid] <= xX10)
            lo = mid;
        else
            hi = mid;
    }

    float f = (xX10 - gridX10[lo]) / (float)(gridX10[hi] - gridX10[lo]);
    return (values[lo] + f * (float)(values[hi] - values[lo])) / 100.0f;
}

float BatteryCurve_CorrectedFrom(const BatteryCurveFile *f, float pct, float voltRaw)
{
    float m = BatteryCurve_Interp(f->mcuGridX10, f->mcuMean, BATTERY_CURVE_MCU_POINTS, pct * 10.0f);

    float w = 0.0f;
    if ((f->flags & BATTERY_CURVE_FLAG_VOLT_VALID) && voltRaw > 0.0f)
    {
        if (pct <= BATTERY_CURVE_BLEND_LO)
            w = 1.0f;
        else if (pct < BATTERY_CURVE_BLEND_HI)
            w = (BATTERY_CURVE_BLEND_HI - pct) / (BATTERY_CURVE_BLEND_HI - BATTERY_CURVE_BLEND_LO);
    }
    if (w == 0.0f)
        return m;

    // The voltage may only pull the estimate down: this keeps the result continuous and
    // monotonic in both pct and voltage (see docs/battery-manager.md, section 11.3).
    float v = BatteryCurve_Interp(f->voltGridX10, f->voltMean, BATTERY_CURVE_VOLT_POINTS, voltRaw * 10.0f);
    if (v > m)
        v = m;
    return (1.0f - w) * m + w * v;
}
