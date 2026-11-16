/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: pure helpers of the menu (home screen summary, "Next:" hint, Diagnostics page titles and voltage
*   lines, footer voltage / temperature). No services; compiled unchanged into the host test
*   (tests/battery/menu_host_test.c). Design: docs/battery-manager.md, 16.11 - 16.13, 17 (Time left row).
*/

#pragma once

#include <3ds/types.h>
#include "battery/battery_led_logic.h"
#include "battery/battery_eta_logic.h"

#define BATTERY_MENU_LINE_CHARS     49 // (320 - 2 * 10) / 6 - 1: body width at x = 10
#define BATTERY_MENU_BODY_LINES     17 // y = 30 .. 206; the hint row is at SCREEN_BOT_HEIGHT - 20

// ---- Home screen ----

typedef enum BatteryMenuTone
{
    BATTERY_TONE_NORMAL = 0,    // white
    BATTERY_TONE_INACTIVE,      // teal: a setting that has no effect right now, a hint
    BATTERY_TONE_ERROR,         // red
} BatteryMenuTone;

typedef enum BatteryHomePatch
{
    BATTERY_HOME_PATCH_UNCHECKED = 0,   // not looked at yet this boot
    BATTERY_HOME_PATCH_OK,
    BATTERY_HOME_PATCH_UNAVAILABLE,     // off for this boot or failing (details in Diagnostics)
} BatteryHomePatch;

typedef struct BatteryHomeInputs
{
    bool haveSample;        // MCU read succeeded
    u32 pctX10;             // stock gauge (MCU) %, tenths
    bool adapter;
    bool charging;
    float corrected;        // corrected %, < 0 without curve / data
    u8 etaState;            // BatteryEtaState (time remaining, section 17)
    u16 etaMinutes;         // valid in BATTERY_ETA_OK
    BatteryLedConfig cfg;
    u8 ledReason;           // BatteryLedReason
    u8 ledDesired;          // BATTERY_LED_MODE_*
    u8 ledError;            // BatteryLedError (battery_led.h): 0 = none
    u8 barsLevel;           // decided level 1..5, 0 = stock
    u8 barsReason;          // BatteryBarsReason
    u8 barsPatch;           // BatteryHomePatch
    u8 sleepPatch;          // BatteryHomePatch (ptm sleep / wake LED bytes)
    bool haveCurve;
    u32 curveSessions;
    bool recording;
    u16 recNumber;
    u32 recElapsedMs;
    bool recSleepGap;
    bool recErrors;         // SD read / write errors in the current session
} BatteryHomeInputs;

#define BATTERY_HOME_SUMMARY_LINES  8 // Battery, Time left, Charger, Power LED, HOME bars, Sleep LED, Curve, Recording
#define BATTERY_HOME_LABEL_CHARS    13
#define BATTERY_HOME_VALUE_CHARS    12

typedef struct BatteryHomeLine
{
    u8 tone; // BatteryMenuTone
    char text[BATTERY_MENU_LINE_CHARS + 1];
} BatteryHomeLine;

/// The summary block: " <label 13><value 12> <note>", each line at most 49 chars.
void BatteryHome_Summary(const BatteryHomeInputs *in, BatteryHomeLine out[BATTERY_HOME_SUMMARY_LINES]);
/// Number of summary lines that were longer than 49 chars and got cut (host test: always 0).
u32 BatteryHome_Overflows(void);

typedef enum BatteryNextStep
{
    BATTERY_NEXT_NONE = 0,      // everything set up: no hint
    BATTERY_NEXT_ERROR,         // the LED control gave up (write failures / MCU keeps overriding)
    BATTERY_NEXT_RECORDING,     // a recording is running: don't sleep or charge
    BATTERY_NEXT_CHARGE,        // no curve, stock gauge < 95 % or on the charger: charge, record, build
    BATTERY_NEXT_RECORD,        // no curve, >= 95 % on the battery: start a recording now
    BATTERY_NEXT_POWER_LED,     // curve, Power LED stock: turn it on in Settings
} BatteryNextStep;

/// Which hint the home screen shows (first match in the order above). Uses only cheap state: the home screen doesn't
/// scan the session files, so "no curve" always says record then build (the Setup guide scans and ticks the steps).
u8 BatteryHome_NextStep(const BatteryHomeInputs *in);
/// The hint's two lines (line2 may be ""), each at most 49 chars. Both "" for BATTERY_NEXT_NONE.
void BatteryHome_NextText(u8 step, const char **line1, const char **line2);
/// The config.ini note in the home screen's last body line (teal), e.g. " config.ini: 2 values replaced (see
/// Diagnostics)" or " config.ini: 1 unknown key (see Diagnostics)". False (out = "") when there is nothing to say, the
/// report was seen on Diagnostics 4/4, or config.ini was saved since.
bool BatteryHome_ConfigNote(char *out, const BatteryConfigReport *r);

// ---- Diagnostics ----

#define BATTERY_DIAG_PAGES          4
#define BATTERY_DIAG_PAGE_ETA_CFG   3 // "Time left and config.ini"

/// "Diagnostics 1/4 - Gauge and LED" etc. (page 0..3), at most 49 chars.
void BatteryDiag_Title(char *out, u32 page);
/// The raw voltage byte (MCU 0x0D) in both scales, labelled (docs 16.13, open question):
///   line1 "Voltage:        raw 185 = 3.613V (x5/256)"
///   line2 "                        = 3.70V (x0.02, footer)"
void BatteryDiag_FormatVoltage(char *line1, char *line2, u8 voltRaw);
/// Time left details (Diagnostics 4/4), three lines of at most 49 chars:
///   "Time left:  ~3h 10m? (provisional)"
///   "Drain %/h:  30.0 smoothed, 29.4 window"        or "30.0 kept, 2h old, 31.2 window" (provisional)
///   "Reset:      7m ago (wake), 8 samples/7m"
void BatteryDiag_FormatEta(const BatteryEtaDiag *d, char *line1, char *line2, char *line3);
/// "config.ini: all values valid at boot", "config.ini: 2 values replaced at boot", "... (from config.tmp)",
/// "config.ini: not found, defaults used", "config.ini: not loaded yet". At most 49 chars.
void BatteryDiag_ConfigHeader(char *out, const BatteryConfigReport *r);
/// "  red_pct=0: out of range, used 1", "  led_override=x: not a number, used 0". At most 49 chars.
void BatteryDiag_ConfigIssue(char *out, const BatteryConfigIssue *is);
/// "  unknown key foo ignored", "  unknown key foo ignored (+2 more)"; "" without unknown keys. At most 49 chars.
void BatteryDiag_ConfigUnknown(char *out, const BatteryConfigReport *r);

// ---- Shared formatting ----

/// Rosalina's temperature: degrees C, or F rounded to nearest ((9C + 2) / 5 + 32, like the menu footer).
u32 BatteryMenu_Temperature(u8 tempC, bool fahrenheit);
/// "3.70V 25\xF8C" / "4.20V 100\xF8F": the voltage in the footer's scale (raw x 0.02, BatteryMcu_VoltageCv), at most
/// 11 chars. Used on the home screen so it shows what the footer shows.
void BatteryMenu_FormatVoltTemp(char *out, u8 voltRaw, u8 tempC, bool fahrenheit);
/// "h:mm:ss"
void BatteryMenu_FormatDuration(char *out, u32 ms);
