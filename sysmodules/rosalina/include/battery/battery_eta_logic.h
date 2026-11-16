/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: time-remaining estimate (pure, no services; compiled unchanged into the host test
*   tests/battery/eta_host_test.c). The LED thread feeds it the corrected % of every poll (battery_led.c); it keeps one
*   sample per minute while discharging, fits a least-squares line over the last 30 minutes and divides the corrected %
*   by the drain rate. The curve's real% is remaining runtime as a fraction of the session, so under a steady load the
*   corrected % falls linearly in time. Read-only: no hardware access. Design: docs/battery-manager.md, section 17.
*/

#pragma once

#include <3ds/types.h>

#define BATTERY_ETA_SLOTS           32                  // newest 32 samples x 8 bytes (window: up to 31)
#define BATTERY_ETA_SAMPLE_MS       60000u              // one sample per minute
#define BATTERY_ETA_WINDOW_MS       (30u * 60000u)      // fit over the newest 30 minutes
#define BATTERY_ETA_MIN_SPAN_MS     (10u * 60000u)      // "calculating" until the samples span 10 minutes
#define BATTERY_ETA_MIN_SAMPLES     6                   //   and there are at least this many
#define BATTERY_ETA_MAX_GAP_MS      (3u * 60000u)       // polls further apart than this (sleep, thread halted): reset
#define BATTERY_ETA_MAX_MINUTES     (24u * 60u)         // longer estimates are not shown
#define BATTERY_ETA_ROUND_MINUTES   5u                  // shown rounded to the nearest 5 minutes
#define BATTERY_ETA_RATE_ALPHA      0.25f               // smoothing of the drain rate per new sample (EMA)

typedef enum BatteryEtaState
{
    BATTERY_ETA_NO_DATA = 0,    // no poll yet
    BATTERY_ETA_NO_CURVE,       // no curve: no corrected %
    BATTERY_ETA_CHARGING,       // on the charger (debounced unplug): nothing collected
    BATTERY_ETA_CALCULATING,    // less than 10 minutes of samples
    BATTERY_ETA_UNKNOWN,        // not draining (slope >= 0) or more than 24 h
    BATTERY_ETA_OK,             // minutes is valid
} BatteryEtaState;

typedef struct BatteryEtaSample
{
    u32 tMs;        // ms (wraps after 49 days; only differences are used)
    u16 pctX100;    // corrected % x 100
} BatteryEtaSample;

typedef struct BatteryEta
{
    BatteryEtaSample s[BATTERY_ETA_SLOTS]; // time order, s[count - 1] newest (shifted when full)
    u8 count;
    bool havePoll;
    u32 lastPollMs;
    bool haveRate;
    float rate;     // smoothed drain, % per minute (> 0)
    u8 state;       // BatteryEtaState
    u16 minutes;    // rounded, valid in BATTERY_ETA_OK
} BatteryEta;

/// Empties the buffer and the smoothed rate; the state becomes BATTERY_ETA_CALCULATING (or NO_DATA if never polled).
void BatteryEta_Reset(BatteryEta *e);
/// One MCU poll. haveCurve: a curve is loaded and corrected >= 0. charging: debounced charger state (on the charger
/// until the LED logic's unplug debounce clears it). Resets on charging, without a curve, or after a gap between polls
/// longer than BATTERY_ETA_MAX_GAP_MS; adds a sample once a minute and recomputes the estimate.
void BatteryEta_Update(BatteryEta *e, u32 nowMs, bool haveCurve, bool charging, float corrected);

/// Least-squares slope of pct over time, % per minute (negative while draining). samples in time order. False if
/// fewer than 2 samples or all at the same time.
bool BatteryEta_Slope(const BatteryEtaSample *samples, u32 count, float *outPctPerMin);
/// Minutes until 0 % at the given drain (% per minute), rounded to BATTERY_ETA_ROUND_MINUTES. False if the drain is
/// <= 0 or the result is more than BATTERY_ETA_MAX_MINUTES.
bool BatteryEta_Remaining(float pct, float drainPerMin, u16 *outMinutes);
/// "~2h 15m", "~2h 05m", "~45m", "~24h 00m", "<5m" (at most 8 chars).
void BatteryEta_Format(char *out, u16 minutes);
