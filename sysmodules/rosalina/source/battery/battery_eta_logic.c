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
    e->haveRate = false;
    e->rate = 0.0f;
    e->minutes = 0;
    e->state = e->havePoll ? BATTERY_ETA_CALCULATING : BATTERY_ETA_NO_DATA;
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

// Fit over the samples of the newest 30 minutes; "calculating" until they span 10 minutes
static void batteryEtaRecompute(BatteryEta *e, float pct)
{
    // s[] is in time order (no ring: shifted once a minute when full), so the window is a tail of it: no copy on the
    // LED thread's small stack
    u32 tNew = e->s[e->count - 1].tMs;
    u32 first = 0;
    while (first < e->count && tNew - e->s[first].tMs > BATTERY_ETA_WINDOW_MS)
        first++;
    const BatteryEtaSample *win = &e->s[first];
    u32 n = e->count - first;

    float slope;
    if (n < BATTERY_ETA_MIN_SAMPLES || tNew - win[0].tMs < BATTERY_ETA_MIN_SPAN_MS || !BatteryEta_Slope(win, n, &slope))
    {
        e->state = BATTERY_ETA_CALCULATING;
        return;
    }

    float drain = -slope;
    if (!(drain > 0.0f))
    {
        e->state = BATTERY_ETA_UNKNOWN; // flat or rising: the smoothed rate is kept for when it drains again
        return;
    }
    e->rate = e->haveRate ? e->rate + BATTERY_ETA_RATE_ALPHA * (drain - e->rate) : drain;
    e->haveRate = true;

    u16 minutes;
    if (BatteryEta_Remaining(pct, e->rate, &minutes))
    {
        e->minutes = minutes;
        e->state = BATTERY_ETA_OK;
    }
    else
        e->state = BATTERY_ETA_UNKNOWN;
}

void BatteryEta_Update(BatteryEta *e, u32 nowMs, bool haveCurve, bool charging, float corrected)
{
    bool gap = e->havePoll && nowMs - e->lastPollMs > BATTERY_ETA_MAX_GAP_MS;
    e->havePoll = true;
    e->lastPollMs = nowMs;
    if (gap)
        BatteryEta_Reset(e); // the thread didn't run (sleep): the drain before doesn't describe the load now

    if (!haveCurve || corrected < 0.0f)
    {
        BatteryEta_Reset(e);
        e->state = BATTERY_ETA_NO_CURVE;
        return;
    }
    if (charging)
    {
        BatteryEta_Reset(e);
        e->state = BATTERY_ETA_CHARGING;
        return;
    }
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
