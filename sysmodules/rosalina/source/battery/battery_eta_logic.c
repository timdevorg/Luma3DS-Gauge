/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: time-remaining estimate (pure). Design: docs/battery-manager.md, section 17.
*/

#include "fmt.h"
#include "memory.h"
#include "battery/battery_eta_logic.h"

void BatteryEta_Reset(BatteryEta *e)
{
    e->count = 0;
    e->lastWallMs = 0;
    e->haveRate = false;
    e->rate = 0.0f;
    e->rateWallMs = 0;
    e->haveProv = false;
    e->provRate = 0.0f;
    e->provWallMs = 0;
    e->haveSlope = false;
    e->windowDrain = 0.0f;
    e->haveReset = false;
    e->resetReason = BATTERY_ETA_RESET_BOOT;
    e->resetMs = 0;
    e->minutes = 0;
    e->state = e->havePoll ? BATTERY_ETA_CALCULATING : BATTERY_ETA_NO_DATA;
}

void BatteryEta_Restart(BatteryEta *e, u8 reason, u32 nowMs)
{
    if (!e->havePoll)
        reason = BATTERY_ETA_RESET_BOOT;
    // Wake / gap: the console was idle (asleep or the thread halted), the load after it is probably like the one
    // before. Keep the smoothed rate, or a kept one that the new window hasn't replaced yet (two wakes in a row).
    // Charger, curve and no curve: the old rate says nothing about what comes now
    bool keep = reason == BATTERY_ETA_RESET_WAKE || reason == BATTERY_ETA_RESET_GAP;
    bool haveProv = false;
    float provRate = 0.0f;
    u64 provWallMs = 0;
    if (keep && e->haveRate)
    {
        haveProv = true;
        provRate = e->rate;
        provWallMs = e->rateWallMs;
    }
    else if (keep && e->haveProv)
    {
        haveProv = true;
        provRate = e->provRate;
        provWallMs = e->provWallMs;
    }

    u64 lastWallMs = e->lastWallMs;
    BatteryEta_Reset(e);
    e->lastWallMs = lastWallMs;
    if (e->havePoll)
        e->lastPollMs = nowMs; // the gap check of the next poll measures from here (a wake isn't also a "gap")
    e->haveProv = haveProv;
    e->provRate = provRate;
    e->provWallMs = provWallMs;
    e->haveReset = true;
    e->resetReason = reason;
    e->resetMs = nowMs;
}

const char *BatteryEta_ResetName(u8 reason)
{
    switch (reason)
    {
        case BATTERY_ETA_RESET_CHARGER:     return "charger";
        case BATTERY_ETA_RESET_WAKE:        return "wake";
        case BATTERY_ETA_RESET_CURVE:       return "curve";
        case BATTERY_ETA_RESET_GAP:         return "gap";
        case BATTERY_ETA_RESET_NO_CURVE:    return "no curve";
        default:                            return "boot";
    }
}

bool BatteryEta_Slope(const BatteryEtaSample *samples, u32 count, float *outPctPerMin)
{
    if (count < 2)
        return false;

    // x: minutes relative to the newest sample (u32 differences: wrap-safe), centred on the means
    u32 tNew = samples[count - 1].tMs;
    float mx = 0.0f, my = 0.0f;
    for (u32 i = 0; i < count; i++)
    {
        mx += -(float)(tNew - samples[i].tMs) / 60000.0f;
        my += samples[i].pctX100 / 100.0f;
    }
    mx /= (float)count;
    my /= (float)count;

    float sxy = 0.0f, sxx = 0.0f;
    for (u32 i = 0; i < count; i++)
    {
        float dx = -(float)(tNew - samples[i].tMs) / 60000.0f - mx;
        float dy = samples[i].pctX100 / 100.0f - my;
        sxy += dx * dy;
        sxx += dx * dx;
    }
    if (sxx <= 0.0f)
        return false;
    *outPctPerMin = sxy / sxx;
    return true;
}

bool BatteryEta_Remaining(float pct, float drainPerMin, u16 *outMinutes)
{
    if (!(drainPerMin > 0.0f) || pct < 0.0f)
        return false;
    float m = pct / drainPerMin;
    if (!(m <= (float)BATTERY_ETA_MAX_MINUTES))
        return false;
    u32 r = (u32)(m / BATTERY_ETA_ROUND_MINUTES + 0.5f) * BATTERY_ETA_ROUND_MINUTES;
    if (r > BATTERY_ETA_MAX_MINUTES)
        r = BATTERY_ETA_MAX_MINUTES;
    *outMinutes = (u16)r;
    return true;
}

void BatteryEta_Format(char *out, u16 minutes)
{
    u32 m = minutes;
    if (m < BATTERY_ETA_ROUND_MINUTES)
        sprintf(out, "<%lum", (u32)BATTERY_ETA_ROUND_MINUTES);
    else if (m < 60)
        sprintf(out, "~%lum", m);
    else
        sprintf(out, "~%luh %02lum", m / 60, m % 60);
}

// The kept rate is used only while it is at most BATTERY_ETA_PROV_MAX_AGE_MS old by the wall clock (a clock set back:
// not used either)
static bool batteryEtaProvUsable(const BatteryEta *e)
{
    return e->haveProv && e->lastWallMs >= e->provWallMs && e->lastWallMs - e->provWallMs <= BATTERY_ETA_PROV_MAX_AGE_MS;
}

// First index of the newest 30 minutes in s[] (count > 0)
static u32 batteryEtaWindowFirst(const BatteryEta *e)
{
    u32 tNew = e->s[e->count - 1].tMs;
    u32 first = 0;
    while (first < e->count && tNew - e->s[first].tMs > BATTERY_ETA_WINDOW_MS)
        first++;
    return first;
}

// Fit over the samples of the newest 30 minutes; "calculating" (or provisional) until they span 10 minutes
static void batteryEtaRecompute(BatteryEta *e, float pct)
{
    // s[] is in time order (no ring: shifted once a minute when full), so the window is a tail of it: no copy on the
    // LED thread's small stack
    u32 tNew = e->s[e->count - 1].tMs;
    u32 first = batteryEtaWindowFirst(e);
    const BatteryEtaSample *win = &e->s[first];
    u32 n = e->count - first;

    float slope = 0.0f;
    e->haveSlope = BatteryEta_Slope(win, n, &slope);
    if (e->haveSlope)
        e->windowDrain = -slope;

    bool provOk = batteryEtaProvUsable(e);
    if (!provOk)
        e->haveProv = false; // too old: dropped for good

    if (n < BATTERY_ETA_MIN_SAMPLES || tNew - win[0].tMs < BATTERY_ETA_MIN_SPAN_MS || !e->haveSlope)
    {
        // Not enough new data: after a wake / gap, the current % at the rate kept from before
        u16 minutes;
        if (provOk && BatteryEta_Remaining(pct, e->provRate, &minutes))
        {
            e->minutes = minutes;
            e->state = BATTERY_ETA_PROVISIONAL;
        }
        else
            e->state = BATTERY_ETA_CALCULATING;
        return;
    }

    // Enough new data. The first fit after a wake / gap is averaged with the kept rate (seed), then normal smoothing
    float alpha = BATTERY_ETA_RATE_ALPHA;
    if (provOk)
    {
        e->rate = e->provRate;
        e->rateWallMs = e->provWallMs;
        e->haveRate = true;
        alpha = BATTERY_ETA_SEED_ALPHA;
    }
    e->haveProv = false;

    float drain = -slope;
    if (!(drain > 0.0f))
    {
        e->state = BATTERY_ETA_UNKNOWN; // flat or rising: the smoothed rate is kept for when it drains again
        return;
    }
    e->rate = e->haveRate ? e->rate + alpha * (drain - e->rate) : drain;
    e->haveRate = true;
    e->rateWallMs = e->lastWallMs;

    u16 minutes;
    if (BatteryEta_Remaining(pct, e->rate, &minutes))
    {
        e->minutes = minutes;
        e->state = BATTERY_ETA_OK;
    }
    else
        e->state = BATTERY_ETA_UNKNOWN;
}

void BatteryEta_Update(BatteryEta *e, u32 nowMs, u64 wallMs, bool haveCurve, bool charging, float corrected)
{
    bool first = !e->havePoll;
    bool gap = e->havePoll && nowMs - e->lastPollMs > BATTERY_ETA_MAX_GAP_MS;
    e->havePoll = true;
    e->lastPollMs = nowMs;
    e->lastWallMs = wallMs;
    if (first && !e->haveReset)
    {
        e->haveReset = true; // boot: the window starts at the first poll
        e->resetReason = BATTERY_ETA_RESET_BOOT;
        e->resetMs = nowMs;
    }

    if (!haveCurve || corrected < 0.0f)
    {
        BatteryEta_Restart(e, BATTERY_ETA_RESET_NO_CURVE, nowMs);
        e->state = BATTERY_ETA_NO_CURVE;
        return;
    }
    if (charging)
    {
        BatteryEta_Restart(e, BATTERY_ETA_RESET_CHARGER, nowMs); // after the charger the load is unknown: nothing kept
        e->state = BATTERY_ETA_CHARGING;
        return;
    }
    if (gap)
        BatteryEta_Restart(e, BATTERY_ETA_RESET_GAP, nowMs); // the thread didn't run (sleep): rate kept as provisional
    if (e->state == BATTERY_ETA_NO_CURVE || e->state == BATTERY_ETA_CHARGING || e->state == BATTERY_ETA_NO_DATA)
        e->state = BATTERY_ETA_CALCULATING; // buffer already empty (reset when that state was entered)

    if (e->count > 0 && nowMs - e->s[e->count - 1].tMs < BATTERY_ETA_SAMPLE_MS)
        return;

    float p = corrected > 100.0f ? 100.0f : corrected;
    if (e->count == BATTERY_ETA_SLOTS)
    {
        memmove(&e->s[0], &e->s[1], (BATTERY_ETA_SLOTS - 1) * sizeof(e->s[0])); // drop the oldest
        e->count--;
    }
    e->s[e->count].tMs = nowMs;
    e->s[e->count].pctX100 = (u16)(p * 100.0f + 0.5f);
    e->count++;
    batteryEtaRecompute(e, p);
}

void BatteryEta_GetDiag(const BatteryEta *e, u32 nowMs, BatteryEtaDiag *out)
{
    out->state = e->state;
    out->minutes = e->minutes;
    out->haveRate = e->haveRate;
    out->rate = e->rate;
    out->haveProv = e->haveProv;
    out->provRate = e->provRate;
    u64 age = e->haveProv && e->lastWallMs >= e->provWallMs ? e->lastWallMs - e->provWallMs : 0;
    out->provAgeMin = age / 60000u > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)(age / 60000u);
    out->haveSlope = e->haveSlope && e->count > 0;
    out->windowDrain = e->windowDrain;
    if (e->count > 0)
    {
        u32 first = batteryEtaWindowFirst(e);
        out->samples = (u8)(e->count - first);
        out->spanMs = e->s[e->count - 1].tMs - e->s[first].tMs;
    }
    else
    {
        out->samples = 0;
        out->spanMs = 0;
    }
    out->haveReset = e->haveReset;
    out->resetReason = e->resetReason;
    out->resetAgeMs = e->haveReset ? nowMs - e->resetMs : 0;
}
