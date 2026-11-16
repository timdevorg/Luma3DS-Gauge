/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: time-remaining estimate (pure, no services; compiled unchanged into the host test
*   tests/battery/eta_host_test.c). The LED thread feeds it the corrected % of every poll (battery_led.c); it keeps one
*   sample per minute while discharging, fits a least-squares line over the last 30 minutes and divides the corrected %
*   by the drain rate. The curve's real% is remaining runtime as a fraction of the session, so under a steady load the
*   corrected % falls linearly in time. After a wake / poll gap the last smoothed rate is kept as a provisional rate
*   until the new window has enough data (17.7). Read-only: no hardware access. Design: docs/battery-manager.md,
*   section 17.
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
#define BATTERY_ETA_SEED_ALPHA      0.5f                // first new window after a wake / gap: half old rate, half new
#define BATTERY_ETA_PROV_MAX_AGE_MS (12ull * 3600000ull)  // wall clock: an older kept rate isn't used (provisional)

typedef enum BatteryEtaState
{
    BATTERY_ETA_NO_DATA = 0,    // no poll yet
    BATTERY_ETA_NO_CURVE,       // no curve: no corrected %
    BATTERY_ETA_CHARGING,       // on the charger (debounced unplug): nothing collected
    BATTERY_ETA_CALCULATING,    // less than 10 minutes of samples
    BATTERY_ETA_UNKNOWN,        // not draining (slope >= 0) or more than 24 h
    BATTERY_ETA_OK,             // minutes is valid
    BATTERY_ETA_PROVISIONAL,    // after a wake / gap: minutes from the current % and the rate kept from before (17.7)
} BatteryEtaState;

/// Why the estimate last started over (Diagnostics).
typedef enum BatteryEtaReset
{
    BATTERY_ETA_RESET_BOOT = 0, // no poll yet when it started (boot)
    BATTERY_ETA_RESET_CHARGER,  // on the charger (rate discarded)
    BATTERY_ETA_RESET_WAKE,     // woke from sleep (rate kept as provisional)
    BATTERY_ETA_RESET_CURVE,    // curve reloaded (rate discarded)
    BATTERY_ETA_RESET_GAP,      // polls more than 3 min apart (rate kept as provisional)
    BATTERY_ETA_RESET_NO_CURVE, // no corrected % (rate discarded)
} BatteryEtaReset;

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
    u64 lastWallMs; // wall clock (ms) of the last poll
    bool haveRate;
    float rate;     // smoothed drain, % per minute (> 0)
    u64 rateWallMs; // wall clock when rate last came from a window fit
    bool haveProv;  // rate kept over a wake / gap, used until the new window has enough data
    float provRate; //   % per minute
    u64 provWallMs; //   wall clock when it last came from a window fit (age cap BATTERY_ETA_PROV_MAX_AGE_MS)
    bool haveSlope; // the newest window had >= 2 samples at different times
    float windowDrain; // -slope of that window, % per minute (diagnostics; can be <= 0)
    bool haveReset; // resetReason / resetMs valid
    u8 resetReason; // BatteryEtaReset
    u32 resetMs;    // tick ms of the last start over
    u8 state;       // BatteryEtaState
    u16 minutes;    // rounded, valid in BATTERY_ETA_OK and BATTERY_ETA_PROVISIONAL
} BatteryEta;

/// What Diagnostics shows (BatteryEta_GetDiag).
typedef struct BatteryEtaDiag
{
    u8 state;           // BatteryEtaState
    u16 minutes;
    bool haveRate;
    float rate;         // smoothed drain, % per minute
    bool haveProv;
    float provRate;     // kept rate, % per minute
    u32 provAgeMin;     // its age (wall clock) at the last poll
    bool haveSlope;
    float windowDrain;  // -slope of the newest window, % per minute
    u8 samples;         // samples in the newest window
    u32 spanMs;         // newest - oldest sample of the window
    bool haveReset;
    u8 resetReason;     // BatteryEtaReset
    u32 resetAgeMs;     // ms since the last start over
} BatteryEtaDiag;

/// Empties everything (buffer, smoothed and kept rate, reset info); the state becomes BATTERY_ETA_CALCULATING (or
/// NO_DATA if never polled). havePoll / lastPollMs are kept.
void BatteryEta_Reset(BatteryEta *e);
/// Starts over for a reason (BatteryEtaReset): the buffer is emptied. WAKE and GAP keep the smoothed rate (or an older
/// kept one) as the provisional rate; every other reason discards it. Before the first poll the reason is BOOT.
void BatteryEta_Restart(BatteryEta *e, u8 reason, u32 nowMs);
/// One MCU poll. nowMs: tick ms; wallMs: wall clock ms (osGetTime), only for the age of the kept rate. haveCurve: a
/// curve is loaded and corrected >= 0. charging: debounced charger state (on the charger until the LED logic's unplug
/// debounce clears it). Starts over on charging, without a curve (rate discarded), or after a gap between polls longer
/// than BATTERY_ETA_MAX_GAP_MS (rate kept); adds a sample once a minute and recomputes the estimate.
void BatteryEta_Update(BatteryEta *e, u32 nowMs, u64 wallMs, bool haveCurve, bool charging, float corrected);
/// Diagnostics snapshot; nowMs: tick ms now (for the age of the last start over).
void BatteryEta_GetDiag(const BatteryEta *e, u32 nowMs, BatteryEtaDiag *out);
/// "boot", "charger", "wake", "curve", "gap", "no curve".
const char *BatteryEta_ResetName(u8 reason);

/// Least-squares slope of pct over time, % per minute (negative while draining). samples in time order. False if
/// fewer than 2 samples or all at the same time.
bool BatteryEta_Slope(const BatteryEtaSample *samples, u32 count, float *outPctPerMin);
/// Minutes until 0 % at the given drain (% per minute), rounded to BATTERY_ETA_ROUND_MINUTES. False if the drain is
/// <= 0 or the result is more than BATTERY_ETA_MAX_MINUTES.
bool BatteryEta_Remaining(float pct, float drainPerMin, u16 *outMinutes);
/// "~2h 15m", "~2h 05m", "~45m", "~24h 00m", "<5m" (at most 8 chars; the home row adds "?" when provisional).
void BatteryEta_Format(char *out, u16 minutes);
