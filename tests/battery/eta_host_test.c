/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Host test for the time-remaining estimate (sysmodules/rosalina/source/battery/battery_eta_logic.c, section 17):
*   slope / remaining / format helpers, and BatteryEta_Update driven like the LED thread (1 s and 10 s polls):
*   linear drain, noisy drain, too little data, flat / rising (slope >= 0), the 24 h clamp, the resets (charging with
*   the debounced unplug, no curve, gap between polls, BatteryEta_Reset), the 31-sample buffer and u32 tick wrap; the
*   provisional estimate after a wake / gap (kept rate, seed, discarded on charger / curve / no curve, 12 h age cap).
*
*   Usage: eta_host_test   (run through tests/battery/run_host_test.sh)
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "fmt.h"
#include "battery/battery_eta_logic.h"

#undef sprintf

static int g_failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

// Rosalina's long is 32 bits: drop a single 'l' before d/u/x so u32 arguments work on 64-bit hosts (as menu_host_test.c)
int host_sprintf(char *buf, const char *fmt, ...)
{
    char f[512];
    u32 n = 0;
    for (const char *p = fmt; *p != 0 && n < sizeof(f) - 2; p++)
    {
        f[n++] = *p;
        if (*p != '%')
            continue;
        while (p[1] != 0 && strchr("-+ #0123456789.*", p[1]) != NULL && n < sizeof(f) - 2)
            f[n++] = *++p;
        if (p[1] == 'l' && p[2] != 'l')
            p++;
    }
    f[n] = 0;

    va_list args;
    va_start(args, fmt);
    int r = vsnprintf(buf, 1024, f, args);
    va_end(args);
    return r;
}

// Wall clock of a poll at tick t: the tick plus a skew that tests move forward to model a sleep during which the tick
// counter may stand still (osGetTime keeps going)
static u64 g_wallSkew = 1000000000ull;
#define WALL(t) ((u64)(t) + g_wallSkew)

static u32 g_rnd = 0x2468ace1u;
static float noise(float amp) // uniform in [-amp, amp]
{
    g_rnd ^= g_rnd << 13;
    g_rnd ^= g_rnd >> 17;
    g_rnd ^= g_rnd << 5;
    return ((g_rnd % 20001u) / 10000.0f - 1.0f) * amp;
}

static BatteryEta fresh(void)
{
    BatteryEta e;
    memset(&e, 0x5A, sizeof(e)); // garbage, as before BatteryEta_Reset in a real struct
    e.havePoll = false;
    BatteryEta_Reset(&e);
    return e;
}

// Polls every stepMs from t0 for durMs at a drain of ratePerMin starting at pct0; returns the time after the last poll
static u32 drain(BatteryEta *e, u32 t0, u32 durMs, u32 stepMs, float pct0, float ratePerMin, float noiseAmp)
{
    u32 t = t0;
    for (u32 d = 0; d <= durMs; d += stepMs, t += stepMs)
    {
        float p = pct0 - ratePerMin * (d / 60000.0f) + noiseAmp * noise(1.0f);
        BatteryEta_Update(e, t, WALL(t), true, false, p < 0.0f ? 0.0f : p);
    }
    return t;
}

static void testHelpers(void)
{
    // Slope of an exact line
    BatteryEtaSample s[20];
    for (u32 i = 0; i < 20; i++)
    {
        s[i].tMs = 1000u + i * 60000u;
        s[i].pctX100 = (u16)(8000 - i * 50); // -0.5 %/min
    }
    float slope = 0.0f;
    CHECK(BatteryEta_Slope(s, 20, &slope) && fabsf(slope + 0.5f) < 1e-4f);
    CHECK(!BatteryEta_Slope(s, 1, &slope));
    BatteryEtaSample same[3] = { { 5, 100 }, { 5, 200 }, { 5, 300 } };
    CHECK(!BatteryEta_Slope(same, 3, &slope)); // all at one time

    // Remaining, rounding to 5 minutes, clamps
    u16 m = 0;
    CHECK(BatteryEta_Remaining(50.0f, 0.5f, &m) && m == 100);
    CHECK(BatteryEta_Remaining(50.0f, 0.5f * 100.0f / 102.0f, &m) && m == 100); // 102 -> 100
    CHECK(BatteryEta_Remaining(50.0f, 0.5f * 100.0f / 103.0f, &m) && m == 105); // 103 -> 105
    CHECK(BatteryEta_Remaining(1.0f, 1.0f, &m) && m == 0);                     // 1 min -> 0 ("<5m")
    CHECK(BatteryEta_Remaining(0.0f, 1.0f, &m) && m == 0);
    CHECK(!BatteryEta_Remaining(50.0f, 0.0f, &m));
    CHECK(!BatteryEta_Remaining(50.0f, -0.1f, &m));
    CHECK(!BatteryEta_Remaining(50.0f, NAN, &m));
    CHECK(BatteryEta_Remaining(100.0f, 100.0f / 1440.0f, &m) && m == 1440);     // exactly 24 h: shown
    CHECK(!BatteryEta_Remaining(100.0f, 100.0f / 1450.0f, &m));                 // over 24 h: not shown
    CHECK(BatteryEta_Remaining(100.0f, 100.0f / 1439.0f, &m) && m == 1440);

    // Format
    static const struct { u16 m; const char *s; } f[] = {
        { 0, "<5m" }, { 4, "<5m" }, { 5, "~5m" }, { 45, "~45m" }, { 55, "~55m" }, { 60, "~1h 00m" },
        { 65, "~1h 05m" }, { 135, "~2h 15m" }, { 1435, "~23h 55m" }, { 1440, "~24h 00m" },
    };
    for (u32 i = 0; i < sizeof(f) / sizeof(f[0]); i++)
    {
        char buf[16];
        BatteryEta_Format(buf, f[i].m);
        CHECK(strcmp(buf, f[i].s) == 0);
        CHECK(strlen(buf) <= 8);
    }
    printf("eta helpers: slope, remaining (rounding, <= 0, 24 h clamp), format OK\n");
}

static void testLinear(void)
{
    // 0.5 %/min from 80 %: after 10 min 75 % left -> 150 min, after 30 min 65 % -> 130 min; 1 s polls
    BatteryEta e = fresh();
    CHECK(e.state == BATTERY_ETA_NO_DATA);
    u32 t = drain(&e, 1000, 9u * 60000u, 1000, 80.0f, 0.5f, 0.0f);
    CHECK(e.state == BATTERY_ETA_CALCULATING); // 9 minutes: not yet
    t = drain(&e, t, 61000, 1000, 75.5f, 0.5f, 0.0f);
    CHECK(e.state == BATTERY_ETA_OK && e.minutes == 150);
    t = drain(&e, t, 20u * 60000u, 1000, e.s[e.count - 1].pctX100 / 100.0f - 0.5f / 60.0f, 0.5f, 0.0f);
    CHECK(e.state == BATTERY_ETA_OK);
    float left = e.s[e.count - 1].pctX100 / 100.0f / 0.5f;
    CHECK(abs((int)e.minutes - (int)left) <= 5);
    CHECK(e.minutes % BATTERY_ETA_ROUND_MINUTES == 0);
    // The buffer never holds more than 32 samples; the fit uses at most the newest 30 minutes
    t = drain(&e, t, 60u * 60000u, 10000, e.s[e.count - 1].pctX100 / 100.0f, 0.5f, 0.0f);
    CHECK(e.count == BATTERY_ETA_SLOTS);
    for (u32 i = 1; i < e.count; i++)
        CHECK(e.s[i].tMs - e.s[i - 1].tMs >= BATTERY_ETA_SAMPLE_MS);
    left = e.s[e.count - 1].pctX100 / 100.0f / 0.5f;
    CHECK(e.state == BATTERY_ETA_OK && abs((int)e.minutes - (int)left) <= 5);
    printf("eta linear drain: calculating for 10 min, then %u min (expected ~%.0f), buffer %u samples\n",
           (unsigned)e.minutes, left, (unsigned)e.count);

    // Faster drain (load changed): the 30 min window and the smoothing follow it within the window
    BatteryEta f = fresh();
    t = drain(&f, 5000, 30u * 60000u, 1000, 90.0f, 0.2f, 0.0f);
    u16 slow = f.minutes;
    t = drain(&f, t, 40u * 60000u, 1000, 90.0f - 0.2f * 30.0f, 1.0f, 0.0f);
    left = f.s[f.count - 1].pctX100 / 100.0f / 1.0f;
    CHECK(f.state == BATTERY_ETA_OK && abs((int)f.minutes - (int)left) <= 5);
    printf("eta load change: %u min at 0.2 %%/min, then %u min at 1 %%/min (expected ~%.0f)\n", (unsigned)slow,
           (unsigned)f.minutes, left);
}

static void testNoisy(void)
{
    // 0.3 %/min with +-1.5 % noise on every poll: within 15 % of the truth after 10 min, 10 % after 30 min,
    // and the shown value changes by at most 15 min between minutes once settled
    BatteryEta e = fresh();
    u32 t = drain(&e, 777, 10u * 60000u + 1000, 1000, 70.0f, 0.3f, 1.5f);
    CHECK(e.state == BATTERY_ETA_OK);
    float truth = (70.0f - 0.3f * 10.0f) / 0.3f;
    CHECK(fabsf(e.minutes - truth) <= 0.15f * truth + 5.0f);
    u16 prev = e.minutes;
    u32 maxStep = 0;
    for (u32 k = 0; k < 20; k++)
    {
        float p0 = 70.0f - 0.3f * (10.0f + 1.0f / 60.0f + k);
        t = drain(&e, t, 59000, 1000, p0, 0.3f, 1.5f);
        CHECK(e.state == BATTERY_ETA_OK);
        u32 step = (u32)abs((int)e.minutes - (int)prev);
        if (step > maxStep)
            maxStep = step;
        prev = e.minutes;
    }
    truth = (70.0f - 0.3f * 30.0f) / 0.3f;
    CHECK(fabsf(e.minutes - truth) <= 0.10f * truth + 5.0f);
    CHECK(maxStep <= 15);
    printf("eta noisy drain: %u min (truth ~%.0f), largest change between minutes %u min\n", (unsigned)e.minutes, truth,
           (unsigned)maxStep);
}

static void testTooLittleAndFlat(void)
{
    // Too little data: 5 samples over 10 minutes (sparse polls 2.5 min apart, below the gap limit) -> calculating
    BatteryEta e = fresh();
    for (u32 i = 0; i < 5; i++)
        BatteryEta_Update(&e, 1000 + i * 150000u, WALL(1000 + i * 150000u), true, false, 80.0f - i);
    CHECK(e.count == 5 && e.state == BATTERY_ETA_CALCULATING);
    // Many samples but under 10 minutes
    e = fresh();
    drain(&e, 1000, 9u * 60000u + 59000u, 1000, 80.0f, 1.0f, 0.0f);
    CHECK(e.state == BATTERY_ETA_CALCULATING);

    // Flat: slope 0 -> unknown (nothing shown)
    e = fresh();
    drain(&e, 1000, 20u * 60000u, 1000, 50.0f, 0.0f, 0.0f);
    CHECK(e.state == BATTERY_ETA_UNKNOWN);
    // Rising (corrected % recovering): unknown
    e = fresh();
    drain(&e, 1000, 20u * 60000u, 1000, 40.0f, -0.1f, 0.0f);
    CHECK(e.state == BATTERY_ETA_UNKNOWN);
    // Very slow drain: over 24 h -> unknown
    e = fresh();
    drain(&e, 1000, 20u * 60000u, 1000, 90.0f, 0.05f, 0.0f); // ~1780 min
    CHECK(e.state == BATTERY_ETA_UNKNOWN);
    // Just under 24 h: shown, clamped to at most 1440
    e = fresh();
    drain(&e, 1000, 20u * 60000u, 1000, 90.0f, 0.07f, 0.0f); // ~1265 min
    CHECK(e.state == BATTERY_ETA_OK && e.minutes <= BATTERY_ETA_MAX_MINUTES);
    // Near empty: "<5m"
    e = fresh();
    drain(&e, 1000, 12u * 60000u, 1000, 13.0f, 1.0f, 0.0f);
    CHECK(e.state == BATTERY_ETA_OK && e.minutes <= 5);
    printf("eta too little data / flat / rising / > 24 h / near empty OK\n");
}

static void testResets(void)
{
    BatteryEta e = fresh();
    u32 t = drain(&e, 1000, 15u * 60000u, 1000, 80.0f, 0.5f, 0.0f);
    CHECK(e.state == BATTERY_ETA_OK && e.count > 10);

    // Charging (the caller passes the debounced charger state): emptied, "charging", nothing collected
    for (u32 i = 0; i < 120; i++, t += 1000)
        BatteryEta_Update(&e, t, WALL(t), true, true, 85.0f);
    CHECK(e.state == BATTERY_ETA_CHARGING && e.count == 0 && !e.haveRate);
    // After the debounced unplug: starts fresh, calculating for 10 minutes again
    t = drain(&e, t, 5u * 60000u, 1000, 85.0f, 0.5f, 0.0f);
    CHECK(e.state == BATTERY_ETA_CALCULATING && e.count >= 5);
    t = drain(&e, t, 6u * 60000u, 1000, 85.0f - 2.5f, 0.5f, 0.0f);
    CHECK(e.state == BATTERY_ETA_OK);

    // No curve: emptied, "no curve"; with a curve again: calculating
    BatteryEta_Update(&e, t, WALL(t), false, false, -1.0f);
    CHECK(e.state == BATTERY_ETA_NO_CURVE && e.count == 0);
    t += 1000;
    BatteryEta_Update(&e, t, WALL(t), true, false, -1.0f); // curve flag but no corrected % yet
    CHECK(e.state == BATTERY_ETA_NO_CURVE && e.count == 0);
    t += 1000;
    BatteryEta_Update(&e, t, WALL(t), true, false, 60.0f);
    CHECK(e.state == BATTERY_ETA_CALCULATING && e.count == 1);

    // Gap between polls (sleep: the thread didn't run): emptied at the next poll
    t = drain(&e, t, 15u * 60000u, 1000, 60.0f, 0.5f, 0.0f);
    CHECK(e.state == BATTERY_ETA_OK);
    t += BATTERY_ETA_MAX_GAP_MS + 1;
    BatteryEta_Update(&e, t, WALL(t), true, false, 50.0f);
    CHECK(e.state == BATTERY_ETA_PROVISIONAL && e.count == 1 && e.haveProv && !e.haveRate); // rate kept (17.7)
    CHECK(e.resetReason == BATTERY_ETA_RESET_GAP);
    // 10 s polls (Power LED stock) are no gap
    t = drain(&e, t, 15u * 60000u, 10000, 50.0f, 0.5f, 0.0f);
    CHECK(e.state == BATTERY_ETA_OK);

    // Explicit reset (wake from sleep, curve reload): calculating, empty
    BatteryEta_Reset(&e);
    CHECK(e.state == BATTERY_ETA_CALCULATING && e.count == 0 && !e.haveRate && !e.haveProv);
    printf("eta resets: charging + unplug, no curve, gap between polls, explicit reset OK\n");
}

// Polls a 30 min drain at 0.5 %/min from 80 % (1 s polls); returns the time after it
static u32 settle(BatteryEta *e, u32 t0)
{
    u32 t = drain(e, t0, 30u * 60000u, 1000, 80.0f, 0.5f, 0.0f);
    CHECK(e->state == BATTERY_ETA_OK && e->haveRate && fabsf(e->rate - 0.5f) < 0.01f);
    return t;
}

static void testProvisional(void)
{
    // Wake: until the new window has 10 min / 6 samples, the current % at the kept rate, marked provisional
    BatteryEta e = fresh();
    u32 t = settle(&e, 1000);
    BatteryEta_Restart(&e, BATTERY_ETA_RESET_WAKE, t);
    CHECK(e.state == BATTERY_ETA_CALCULATING && e.count == 0 && e.haveProv && !e.haveRate);
    g_wallSkew += 3600000ull; // slept 1 h (tick may not have moved)
    t += 1000;
    BatteryEta_Update(&e, t, WALL(t), true, false, 50.0f); // no gap: the restart counts as the last poll
    CHECK(e.state == BATTERY_ETA_PROVISIONAL && e.minutes == 100 && e.resetReason == BATTERY_ETA_RESET_WAKE);
    u16 first = e.minutes;
    t = drain(&e, t, 9u * 60000u, 1000, 50.0f, 0.5f, 0.0f);
    CHECK(e.state == BATTERY_ETA_PROVISIONAL && e.minutes < first && abs((int)e.minutes - 91) <= 5);
    BatteryEtaDiag d;
    BatteryEta_GetDiag(&e, t, &d);
    CHECK(d.haveProv && !d.haveRate && d.provAgeMin >= 60 && d.provAgeMin <= 71 && d.samples == 10 && d.haveSlope &&
          d.spanMs == 9u * 60000u && d.resetReason == BATTERY_ETA_RESET_WAKE && d.resetAgeMs >= 9u * 60000u);
    // Enough new data: normal, the kept rate seeds the smoothing (same load: still 0.5)
    t = drain(&e, t, 61000, 1000, 50.0f - 0.5f * 9.0f - 0.5f / 60.0f, 0.5f, 0.0f);
    CHECK(e.state == BATTERY_ETA_OK && e.haveRate && !e.haveProv && fabsf(e.rate - 0.5f) < 0.01f);

    // Seed: a new load of 1 %/min after the wake -> the first rate is halfway (0.75), then it follows the new load
    e = fresh();
    t = settle(&e, 1000);
    BatteryEta_Restart(&e, BATTERY_ETA_RESET_WAKE, t);
    t = drain(&e, t + 1000, 10u * 60000u, 1000, 60.0f, 1.0f, 0.0f);
    CHECK(e.state == BATTERY_ETA_OK && fabsf(e.rate - 0.75f) < 0.02f);
    t = drain(&e, t, 30u * 60000u, 1000, 50.0f - 1.0f / 60.0f, 1.0f, 0.0f);
    CHECK(e.state == BATTERY_ETA_OK && fabsf(e.rate - 1.0f) < 0.05f);

    // Gap between polls: the same, reason "gap"
    e = fresh();
    t = settle(&e, 1000);
    t += BATTERY_ETA_MAX_GAP_MS + 1;
    BatteryEta_Update(&e, t, WALL(t), true, false, 40.0f);
    CHECK(e.state == BATTERY_ETA_PROVISIONAL && e.minutes == 80 && e.resetReason == BATTERY_ETA_RESET_GAP);
    // Two wakes in a row before the new window is ready: the kept rate stays
    t = drain(&e, t, 3u * 60000u, 1000, 40.0f, 0.5f, 0.0f);
    BatteryEta_Restart(&e, BATTERY_ETA_RESET_WAKE, t);
    t += 1000;
    BatteryEta_Update(&e, t, WALL(t), true, false, 30.0f);
    CHECK(e.state == BATTERY_ETA_PROVISIONAL && e.minutes == 60);

    // Charging, curve change, no curve: nothing kept, "calculating" afterwards
    e = fresh();
    t = settle(&e, 1000);
    for (u32 i = 0; i < 10; i++, t += 1000)
        BatteryEta_Update(&e, t, WALL(t), true, true, 70.0f);
    CHECK(e.state == BATTERY_ETA_CHARGING && !e.haveProv && e.resetReason == BATTERY_ETA_RESET_CHARGER);
    BatteryEta_Restart(&e, BATTERY_ETA_RESET_WAKE, t); // a wake after charging has nothing to keep
    t = drain(&e, t + 1000, 5u * 60000u, 1000, 70.0f, 0.5f, 0.0f);
    CHECK(e.state == BATTERY_ETA_CALCULATING && !e.haveProv);
    e = fresh();
    t = settle(&e, 1000);
    BatteryEta_Restart(&e, BATTERY_ETA_RESET_CURVE, t);
    t = drain(&e, t + 1000, 5u * 60000u, 1000, 50.0f, 0.5f, 0.0f);
    CHECK(e.state == BATTERY_ETA_CALCULATING && !e.haveProv && e.resetReason == BATTERY_ETA_RESET_CURVE);
    e = fresh();
    t = settle(&e, 1000);
    BatteryEta_Update(&e, t, WALL(t), false, false, -1.0f);
    CHECK(e.state == BATTERY_ETA_NO_CURVE && !e.haveProv && e.resetReason == BATTERY_ETA_RESET_NO_CURVE);
    t = drain(&e, t + 1000, 5u * 60000u, 1000, 50.0f, 0.5f, 0.0f);
    CHECK(e.state == BATTERY_ETA_CALCULATING && !e.haveProv);
    // ... and a charger seen during the provisional phase drops the kept rate too
    e = fresh();
    t = settle(&e, 1000);
    BatteryEta_Restart(&e, BATTERY_ETA_RESET_WAKE, t);
    t = drain(&e, t + 1000, 2u * 60000u, 1000, 50.0f, 0.5f, 0.0f);
    CHECK(e.state == BATTERY_ETA_PROVISIONAL);
    BatteryEta_Update(&e, t, WALL(t), true, true, 50.0f);
    t = drain(&e, t + 1000, 2u * 60000u, 1000, 50.0f, 0.5f, 0.0f);
    CHECK(e.state == BATTERY_ETA_CALCULATING && !e.haveProv);

    // Age cap (wall clock): 11 h asleep -> provisional; 13 h -> dropped; repeated wakes add up
    e = fresh();
    t = settle(&e, 1000);
    BatteryEta_Restart(&e, BATTERY_ETA_RESET_WAKE, t);
    g_wallSkew += 11ull * 3600000ull;
    t += 1000;
    BatteryEta_Update(&e, t, WALL(t), true, false, 50.0f);
    CHECK(e.state == BATTERY_ETA_PROVISIONAL);
    BatteryEta_Restart(&e, BATTERY_ETA_RESET_WAKE, t);
    g_wallSkew += 2ull * 3600000ull;
    t += 1000;
    BatteryEta_Update(&e, t, WALL(t), true, false, 45.0f);
    CHECK(e.state == BATTERY_ETA_CALCULATING && !e.haveProv);
    t = drain(&e, t, 10u * 60000u + 1000, 1000, 45.0f, 0.5f, 0.0f); // then normal, without a seed
    CHECK(e.state == BATTERY_ETA_OK && fabsf(e.rate - 0.5f) < 0.01f);
    e = fresh();
    t = settle(&e, 1000);
    BatteryEta_Restart(&e, BATTERY_ETA_RESET_WAKE, t);
    g_wallSkew += 13ull * 3600000ull;
    t += 1000;
    BatteryEta_Update(&e, t, WALL(t), true, false, 50.0f);
    CHECK(e.state == BATTERY_ETA_CALCULATING && !e.haveProv);
    // A wall clock set back: not used
    e = fresh();
    t = settle(&e, 1000);
    BatteryEta_Restart(&e, BATTERY_ETA_RESET_WAKE, t);
    g_wallSkew -= 3600000ull;
    t += 1000;
    BatteryEta_Update(&e, t, WALL(t), true, false, 50.0f);
    CHECK(e.state == BATTERY_ETA_CALCULATING && !e.haveProv);
    // Provisional over 24 h at the kept rate: calculating, not a value
    e = fresh();
    drain(&e, 1000, 20u * 60000u, 1000, 80.0f, 0.065f, 0.0f); // ~1030 min
    CHECK(e.state == BATTERY_ETA_OK);
    t = 1000 + 21u * 60000u;
    BatteryEta_Restart(&e, BATTERY_ETA_RESET_WAKE, t);
    BatteryEta_Update(&e, t + 1000, WALL(t + 1000), true, false, 100.0f);
    CHECK(e.state == BATTERY_ETA_CALCULATING && e.haveProv);

    // Boot: the first restart before any poll is "boot", the first poll stamps it
    e = fresh();
    BatteryEta_Restart(&e, BATTERY_ETA_RESET_CURVE, 500);
    CHECK(e.resetReason == BATTERY_ETA_RESET_BOOT && e.state == BATTERY_ETA_NO_DATA);
    e = fresh();
    BatteryEta_Update(&e, 7000, WALL(7000), true, false, 50.0f);
    BatteryEta_GetDiag(&e, 67000, &d);
    CHECK(d.haveReset && d.resetReason == BATTERY_ETA_RESET_BOOT && d.resetAgeMs == 60000 && d.samples == 1 && !d.haveSlope);
    CHECK(strcmp(BatteryEta_ResetName(BATTERY_ETA_RESET_NO_CURVE), "no curve") == 0 &&
          strcmp(BatteryEta_ResetName(BATTERY_ETA_RESET_WAKE), "wake") == 0 && strcmp(BatteryEta_ResetName(99), "boot") == 0);
    printf("eta provisional: wake / gap keep the rate (10 min provisional, seed), charger / curve / no curve drop it, "
           "12 h age cap OK\n");
}

static void testWrap(void)
{
    // The u32 ms counter wraps (49.7 days): differences stay right
    BatteryEta e = fresh();
    drain(&e, 0xFFFFFFFFu - 5u * 60000u, 20u * 60000u, 1000, 80.0f, 0.5f, 0.0f);
    float left = e.s[e.count - 1].pctX100 / 100.0f / 0.5f;
    CHECK(e.state == BATTERY_ETA_OK && abs((int)e.minutes - (int)left) <= 5);
    printf("eta u32 tick wrap OK\n");
}

int main(void)
{
    testHelpers();
    testLinear();
    testNoisy();
    testTooLittleAndFlat();
    testResets();
    testProvisional();
    testWrap();
    printf(g_failures == 0 ? "eta host test: OK\n" : "eta host test: %d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
