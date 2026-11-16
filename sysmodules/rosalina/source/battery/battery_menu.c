/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: Rosalina screens (home screen with a live summary; settings, learning: recording / sessions / curve /
*   graph, advanced: diagnostics in 3 pages, LED test, blue LED type, restore previous curve, limits; setup guide).
*   Settings only go through BatteryLed_SetConfig; nothing here writes to the MCU or ptm.
*/

#include <3ds.h>
#include <stdarg.h>
#include "menu.h"
#include "draw.h"
#include "fmt.h"
#include "memory.h"
#include "luma_config.h"
#include "battery/battery_menu.h"
#include "battery/battery_menu_logic.h"
#include "battery/battery_mcu.h"
#include "battery/battery_recorder.h"
#include "battery/battery_sessions.h"
#include "battery/battery_curve.h"
#include "battery/battery_graph.h"
#include "battery/battery_led.h"
#include "battery/battery_ptm_patch.h"
#include "battery/battery_ptm_bars.h"

#define BATTERY_MENU_LIST_ROWS      15
#define BATTERY_SESSION_ROWS        13 // Sessions list: header, 13 rows, blank, legend / feedback
#define BATTERY_MENU_BODY_END       (30 + BATTERY_MENU_BODY_LINES * SPACING_Y) // y of the first row below the body

_Static_assert(BATTERY_LED_ERR_NONE == 0, "battery_menu_logic.c treats a led error of 0 as none");

// Menu tree (docs/battery-manager.md, section 4.2). The Rosalina entry opens BatteryMenu_Home, a custom screen (summary +
// four entries) that opens Learning... and Advanced... with menuShow, so those two stay ordinary Rosalina menus. Settings
// are edited on one screen and saved when it is left, so no item title depends on config.ini. The only dynamic title is
// Start / Stop recording, refreshed by its visibility callback (menuDraw calls it on every redraw, about once a second)
// into a static buffer padded to a fixed length, because the menu redraws without clearing.

static void batteryMenuSettings(void);
static void batteryMenuRecording(void);
static void batteryMenuSessions(void);
static void batteryMenuBuildCurve(void);
static void batteryMenuGraph(void);
static void batteryMenuDiagnostics(void);
static void batteryMenuLedTest(void);
static void batteryMenuBlueLedType(void);
static void batteryMenuRestoreCurve(void);
static void batteryMenuAbout(void);
static void batteryMenuSetupGuide(void);
static bool batteryRecordingTitleRefresh(void);

#define BATTERY_RECORDING_TITLE_CHARS   34 // "Stop recording  #9999  1193:02:47" (u32 ms) fits

static char g_recordingTitle[BATTERY_RECORDING_TITLE_CHARS + 1] = "Start recording";

static Menu batteryLearningMenu = {
    "Battery Manager - Learning",
    {
        { g_recordingTitle, METHOD, .method = &batteryMenuRecording, .visibility = &batteryRecordingTitleRefresh },
        { "Sessions", METHOD, .method = &batteryMenuSessions },
        { "Build curve", METHOD, .method = &batteryMenuBuildCurve },
        { "Curve graph", METHOD, .method = &batteryMenuGraph },
        {},
    }
};

static Menu batteryAdvancedMenu = {
    "Battery Manager - Advanced",
    {
        { "Diagnostics", METHOD, .method = &batteryMenuDiagnostics },
        { "LED test", METHOD, .method = &batteryMenuLedTest },
        { "Blue LED type", METHOD, .method = &batteryMenuBlueLedType },
        { "Restore previous curve", METHOD, .method = &batteryMenuRestoreCurve },
        { "Limits and about", METHOD, .method = &batteryMenuAbout },
        {},
    }
};

static void batteryOpenLearning(void)
{
    menuShow(&batteryLearningMenu);
}

static void batteryOpenAdvanced(void)
{
    menuShow(&batteryAdvancedMenu);
}

// The home screen's entries (BatteryMenu_Home), in the order of the former main menu
static const struct
{
    const char *title;
    void (*open)(void);
} g_homeEntries[] = {
    { "Settings", batteryMenuSettings },
    { "Learning...", batteryOpenLearning },
    { "Setup guide", batteryMenuSetupGuide },
    { "Advanced...", batteryOpenAdvanced },
};

#define BATTERY_HOME_ENTRIES (sizeof(g_homeEntries) / sizeof(g_homeEntries[0]))

// "5", "none", "?" or "-" for the Sleep diagnostics line
static void batteryFormatSleepMode(char *out, u8 mode, bool pending, bool isWrite)
{
    if (pending)
        memcpy(out, "-", 2);
    else if (mode == BATTERY_LED_MODE_UNKNOWN)
        memcpy(out, isWrite ? "none" : "?", isWrite ? 5 : 2);
    else
        sprintf(out, "%lu", (u32)mode);
}

static void batteryMenuClear(void)
{
    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();
}

// Draws one padded line (so shorter values overwrite longer old ones). Draw_Lock must be held.
static u32 batteryMenuLine(u32 y, u32 color, const char *fmt, ...)
{
    char buf[96];
    va_list args;
    va_start(args, fmt);
    vsprintf(buf, fmt, args);
    va_end(args);
    buf[BATTERY_MENU_LINE_CHARS] = 0;

    Draw_DrawFormattedString(10, y, color, "%-*s", BATTERY_MENU_LINE_CHARS, buf);
    return y + SPACING_Y;
}

// Blank lines from y to the end of the body, so a screen whose line count varies leaves no residue. Draw_Lock must be held.
static void batteryMenuPadBody(u32 y)
{
    while (y < BATTERY_MENU_BODY_END)
        y = batteryMenuLine(y, COLOR_WHITE, "");
}

// ---- Text screens (messages, confirmations, setup guide) ----

typedef struct BatteryText
{
    u32 count;
    u32 color[BATTERY_MENU_BODY_LINES];
    char line[BATTERY_MENU_BODY_LINES][BATTERY_MENU_LINE_CHARS + 1];
} BatteryText;

static BatteryText g_text; // menu thread only

static void batteryTextBegin(void)
{
    g_text.count = 0;
}

// Adds one body line, cut at 49 chars like batteryMenuLine; lines beyond 17 are dropped (every screen is sized to fit)
static void batteryTextAdd(u32 color, const char *fmt, ...)
{
    char buf[96];
    va_list args;
    va_start(args, fmt);
    vsprintf(buf, fmt, args);
    va_end(args);
    buf[BATTERY_MENU_LINE_CHARS] = 0;

    if (g_text.count < BATTERY_MENU_BODY_LINES)
    {
        memcpy(g_text.line[g_text.count], buf, sizeof(g_text.line[0]));
        g_text.color[g_text.count++] = color;
    }
}

// Shows the collected lines under title with hint until one of keys is pressed. Returns that key, 0 if the menu closes.
static u32 batteryTextShow(const char *title, const char *hint, u32 keys)
{
    u32 pressed = 0;
    batteryMenuClear();
    do
    {
        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, title);
        u32 y = 30;
        for (u32 i = 0; i < g_text.count; i++)
            y = batteryMenuLine(y, g_text.color[i], "%s", g_text.line[i]);
        Draw_DrawString(10, SCREEN_BOT_HEIGHT - 20, COLOR_TITLE, hint);
        Draw_FlushFramebuffer();
        Draw_Unlock();
        pressed = waitInput();
    }
    while (!(pressed & keys) && !menuShouldExit);
    batteryMenuClear();
    return menuShouldExit ? 0 : (pressed & keys);
}

// A / B question; true if A was pressed
static bool batteryTextConfirm(const char *title, const char *hint)
{
    return batteryTextShow(title, hint, KEY_A | KEY_B) == KEY_A;
}

// Simple message screen, B to go back
static void batteryMenuMessage(const char *title, const char *line1, const char *line2)
{
    batteryTextBegin();
    if (line1 != NULL)
        batteryTextAdd(COLOR_WHITE, "%s", line1);
    if (line2 != NULL)
        batteryTextAdd(COLOR_WHITE, "%s", line2);
    batteryTextShow(title, "Press B to go back.", KEY_B);
}

// One line of feedback in the hint row for 0.7 s (e.g. "Saved."), before the screen is left. Not while the menu closes.
static void batteryFlashHint(u32 color, const char *text)
{
    if (menuShouldExit)
        return;
    Draw_Lock();
    Draw_DrawFormattedString(10, SCREEN_BOT_HEIGHT - 20, color, "%-*s", BATTERY_MENU_LINE_CHARS, text);
    Draw_FlushFramebuffer();
    Draw_Unlock();
    svcSleepThread(700 * 1000 * 1000LL);
}

static void batteryFormatX10(char *out, float v)
{
    u32 x10 = (u32)(v * 10.0f + 0.5f);
    sprintf(out, "%lu.%lu", x10 / 10, x10 % 10);
}

// ---- Advanced > Diagnostics: raw gauge, LED, ptm patch and recorder values in 3 pages, refreshed every second ----

typedef struct BatteryDiagData
{
    Result res;                 // BatteryMcu_ReadSample
    BatteryMcuSample s;
    BatteryRecorderStatus st;
    BatteryLedStatus led;
    BatteryPtmPatchStatus ptm;
    BatteryBarsStatus bars;
    bool haveBak;               // curve.bak, read once when Diagnostics opens
    u16 bakCount;
} BatteryDiagData;

static BatteryDiagData g_diag; // menu thread only

static void batteryFormatBootS(char *out, u64 ms)
{
    if (ms == 0)
        memcpy(out, "-", 2);
    else
        batteryFormatX10(out, ms / 1000.0f);
}

// Page 1: gauge and LED. Draw_Lock must be held.
static u32 batteryDiagGaugeLed(u32 y, const BatteryDiagData *d)
{
    const BatteryMcuSample *s = &d->s;
    const BatteryLedStatus *led = &d->led;
    char num[16], num2[16], line1[64], line2[64];
    bool ok = R_SUCCEEDED(d->res);

    if (ok)
    {
        u32 p = BatteryMcu_PercentX10(s->pctInt, s->pctFrac);
        bool adapter = (s->flags & BATTERY_MCU_FLAG_ADAPTER) != 0;
        bool charging = (s->flags & BATTERY_MCU_FLAG_CHARGING) != 0;

        y = batteryMenuLine(y, COLOR_WHITE, "MCU charge:     %lu.%lu%%  (stock gauge)", p / 10, p % 10);
        // Same value the LED uses (median voltage); instantaneous voltage before the LED thread's first poll
        float c = led->haveData ? led->corrected : BatteryCurve_Corrected(s->pctInt + s->pctFrac / 256.0f, (float)s->voltageRaw);
        if (c >= 0.0f)
        {
            batteryFormatX10(num, c);
            batteryFormatX10(num2, led->haveData ? led->voltSmoothed : (float)s->voltageRaw);
            y = batteryMenuLine(y, COLOR_WHITE, "Corrected:      %s%%  (volt median %s raw)", num, num2);
        }
        else
            y = batteryMenuLine(y, COLOR_WHITE, "Corrected:      no curve yet");
        // Both scales of the raw byte, labelled: which one is right is open (docs 16.13)
        BatteryDiag_FormatVoltage(line1, line2, s->voltageRaw);
        y = batteryMenuLine(y, COLOR_WHITE, "%s", line1);
        y = batteryMenuLine(y, COLOR_WHITE, "%s", line2);
        // Same unit and rounding as the Rosalina footer (menu.c)
        bool fahrenheit = (lumaMcuConfig.flags & MCUCFG_FLAG_TEMPERATURE_IN_FAHRENHEIT) != 0;
        y = batteryMenuLine(y, COLOR_WHITE, "Temp: %lu\xF8%c  Adapter: %s  Charging: %s", BatteryMenu_Temperature(s->temperature, fahrenheit),
                            fahrenheit ? 'F' : 'C', adapter ? "yes" : "no", charging ? "yes" : "no");
        if (s->infoValid)
            y = batteryMenuLine(y, COLOR_WHITE, "Vendor: %lu  RCOMP: 0x%02lx  MGIC: %lu.%lu",
                                (u32)s->info[2], (u32)s->info[5], (u32)s->info[3], (u32)s->info[4]);
        else
            y = batteryMenuLine(y, COLOR_WHITE, "Info table (0x7F) unavailable");
    }
    else
    {
        y = batteryMenuLine(y, COLOR_RED, "MCU read failed (0x%08lx).", (u32)d->res);
        for (u32 i = 0; i < 5; i++)
            y = batteryMenuLine(y, COLOR_WHITE, "");
    }
    y = batteryMenuLine(y, COLOR_WHITE, "");

    // LED control
    if (led->reason == BATTERY_LED_REASON_OFF)
        y = batteryMenuLine(y, COLOR_WHITE, "LED control:    OFF%s", led->fastPolling ? "  (fast poll)" : "");
    else
        y = batteryMenuLine(y, led->cfg.override ? COLOR_GREEN : COLOR_WHITE, "LED control:    %s  (%s%s)",
                            led->cfg.override ? "ON" : "OFF", BatteryLedLogic_ReasonName(led->reason),
                            led->fastPolling ? ", fast poll" : "");
    if (ok && s->ledModeValid)
        y = batteryMenuLine(y, COLOR_WHITE, "LED desired:    %-9s actual(0x29): %lu", BatteryLedLogic_ModeName(led->desired), (u32)s->ledMode);
    else
        y = batteryMenuLine(y, COLOR_WHITE, "LED desired:    %-9s actual(0x29): ?", BatteryLedLogic_ModeName(led->desired));
    if (ok && s->infoValid)
        y = batteryMenuLine(y, COLOR_WHITE, "Red LED: %-3s  Blue: %-3lu  Written: %s",
                            s->info[0x0A] ? "on" : "off", (u32)s->info[0x0B], BatteryLedLogic_ModeName(led->lastWritten));
    else
        y = batteryMenuLine(y, COLOR_WHITE, "Red LED: ?    Blue: ?    Written: %s", BatteryLedLogic_ModeName(led->lastWritten));
    u32 sessionReasserts = d->st.recording ? led->reasserts - d->st.reassertBase : 0;
    if (led->haveMismatch)
    {
        batteryFormatX10(num, led->lastMismatchPct);
        y = batteryMenuLine(y, COLOR_WHITE, "Reasserts:      %lu boot/%lu session (at %s%%)", led->reasserts, sessionReasserts, num);
    }
    else
        y = batteryMenuLine(y, COLOR_WHITE, "Reasserts:      %lu boot/%lu session", led->reasserts, sessionReasserts);
    y = batteryMenuLine(y, led->writeFailures != 0 ? COLOR_RED : COLOR_WHITE, "LED writes:     %lu ok, %lu failed", led->writes,
                        led->writeFailures);
    // The error and the boot timings have their own lines (they shared one before)
    if (led->error == BATTERY_LED_ERR_WRITE_FAILED)
        y = batteryMenuLine(y, COLOR_RED, "LED error:      writes failed (0x%08lx), auto", (u32)led->lastWriteRes);
    else if (led->error == BATTERY_LED_ERR_MCU_OVERRIDES)
        y = batteryMenuLine(y, COLOR_RED, "LED error:      MCU keeps overriding, auto");
    else if (led->writeFailures != 0)
        y = batteryMenuLine(y, COLOR_RED, "LED error:      none (last fail 0x%08lx)", (u32)led->lastWriteRes);
    else
        y = batteryMenuLine(y, COLOR_WHITE, "LED error:      none");
    // Boot timing (s since the tick counter started, ~power-on): Rosalina, SD usable, first MCU read, first LED write
    char t0[16], t1[16];
    batteryFormatBootS(t0, led->startMs);
    batteryFormatBootS(t1, led->sdReadyMs);
    y = batteryMenuLine(y, COLOR_WHITE, "Boot s:         Rosalina %s, SD %s", t0, t1);
    batteryFormatBootS(t0, led->firstDataMs);
    batteryFormatBootS(t1, led->firstWriteMs);
    return batteryMenuLine(y, COLOR_WHITE, "First s:        MCU read %s, LED %s", t0, t1);
}

// Page 2: Sleep LED and the two ptm patches (sections 14, 15). Draw_Lock must be held.
static u32 batteryDiagPtm(u32 y, const BatteryDiagData *d)
{
    const BatteryLedStatus *led = &d->led;
    const BatteryPtmPatchStatus *ptm = &d->ptm;
    const BatteryBarsStatus *bars = &d->bars;

    y = batteryMenuLine(y, COLOR_WHITE, "Sleep setting:  %s", BatteryLedLogic_SleepSettingName(led->cfg.sleepMode));
    if (led->sleepCount != 0)
    {
        // What the sleep hook wrote, and 0x29 read on wake before anything was written
        char w[8], r[8];
        batteryFormatSleepMode(w, led->sleepWrote, false, true);
        batteryFormatSleepMode(r, led->wakeRead, led->wakePending, false);
        y = batteryMenuLine(y, COLOR_WHITE, "Last sleep:     wrote %s / wake read %s, n=%lu", w, r, led->sleepCount);
    }
    else
        y = batteryMenuLine(y, COLOR_WHITE, "Last sleep:     none this boot");
    y = batteryMenuLine(y, COLOR_WHITE, "");

    // ptm's sleep / wake LED bytes (section 14): "sleep 05 wake 05 (active)", "sleep 02 wake 00 (stock)", "off: ..."
    y = batteryMenuLine(y, COLOR_WHITE, "Sleep / wake LED patch (ptm):");
    const char *ptmName = BatteryPtmPatch_StateName(ptm->state, ptm->sleepCur, ptm->wakeCur);
    if (ptm->state == BATTERY_PTM_STATE_OK)
        y = batteryMenuLine(y, COLOR_WHITE, "  sleep %02lx wake %02lx (%s), writes %lu", (u32)ptm->sleepCur, (u32)ptm->wakeCur,
                            ptmName, ptm->writes);
    else if (ptm->state == BATTERY_PTM_STATE_NO_PTM || ptm->state == BATTERY_PTM_STATE_MAP_ERROR ||
             ptm->state == BATTERY_PTM_STATE_OFF_UNMAP)
        y = batteryMenuLine(y, COLOR_RED, "  %s 0x%08lx", ptmName, (u32)ptm->lastRes);
    else if (ptm->state == BATTERY_PTM_STATE_OFF_TITLE)
        y = batteryMenuLine(y, COLOR_RED, "  off: title id %08lx%08lx", (u32)(ptm->titleId >> 32), (u32)ptm->titleId);
    else if (ptm->state == BATTERY_PTM_STATE_OFF_MISMATCH)
        y = batteryMenuLine(y, COLOR_RED, "  off: mismatch (%s), both off",
                            ptm->mismatch == (BATTERY_PTM_MISMATCH_SLEEP | BATTERY_PTM_MISMATCH_WAKE) ? "sleep+wake" :
                            ptm->mismatch == BATTERY_PTM_MISMATCH_WAKE ? "wake" : "sleep");
    else if (ptm->state == BATTERY_PTM_STATE_WRITE_ERROR)
        y = batteryMenuLine(y, COLOR_RED, "  write error (want %02lx/%02lx), writes %lu", (u32)ptm->sleepWant, (u32)ptm->wakeWant,
                            ptm->writes);
    else
        y = batteryMenuLine(y, ptm->state == BATTERY_PTM_STATE_UNCHECKED ? COLOR_WHITE : COLOR_RED, "  %s, writes %lu", ptmName,
                            ptm->writes);
    y = batteryMenuLine(y, COLOR_WHITE, "  checks %lu", ptm->checks);
    y = batteryMenuLine(y, COLOR_WHITE, "");

    // ptm battery bars (section 15): "fed L=3 (shared 3)", "fed L=3 (held), shared 3" (charging), "stock (off), shared 5",
    // "off: ...". "shared" is ptm's level in the shared page (0x1FF81085 bits 2..4, read only), updated at ptm's own
    // timer tick
    y = batteryMenuLine(y, COLOR_WHITE, "HOME Menu bars patch (ptm):");
    u32 shared = (OS_SharedConfig->led_battery >> 2) & 7;
    if (bars->state == BATTERY_BARS_STATE_OK && bars->cmpCur == BATTERY_BARS_CMP_FED && led->barsReason == BATTERY_BARS_REASON_CHARGING)
        y = batteryMenuLine(y, COLOR_WHITE, "  fed L=%lu (held), shared %lu, writes %lu", (u32)bars->movsCur, shared, bars->writes);
    else if (bars->state == BATTERY_BARS_STATE_OK && bars->cmpCur == BATTERY_BARS_CMP_FED)
        y = batteryMenuLine(y, COLOR_WHITE, "  fed L=%lu (shared %lu), writes %lu", (u32)bars->movsCur, shared, bars->writes);
    else if (bars->state == BATTERY_BARS_STATE_OK)
        y = batteryMenuLine(y, COLOR_WHITE, "  stock (%s), shared %lu, writes %lu", BatteryBars_ReasonName(led->barsReason), shared,
                            bars->writes);
    else if (bars->state == BATTERY_BARS_STATE_UNCHECKED)
        y = batteryMenuLine(y, COLOR_WHITE, "  not checked yet, shared %lu", shared);
    else if (bars->state == BATTERY_BARS_STATE_NO_PTM || bars->state == BATTERY_BARS_STATE_MAP_ERROR ||
             bars->state == BATTERY_BARS_STATE_OFF_UNMAP)
        y = batteryMenuLine(y, COLOR_RED, "  %s 0x%08lx", BatteryBars_StateName(bars->state), (u32)bars->lastRes);
    else if (bars->state == BATTERY_BARS_STATE_OFF_TITLE)
        y = batteryMenuLine(y, COLOR_RED, "  off: title id %08lx%08lx", (u32)(bars->titleId >> 32), (u32)bars->titleId);
    else if (bars->state == BATTERY_BARS_STATE_OFF_MISMATCH)
        y = batteryMenuLine(y, COLOR_RED, "  off: mismatch (%s)",
                            bars->mismatch == (BATTERY_BARS_MISMATCH_A | BATTERY_BARS_MISMATCH_B) ? "A+B" :
                            bars->mismatch == BATTERY_BARS_MISMATCH_B ? "B" : "A");
    else if (bars->state == BATTERY_BARS_STATE_WRITE_ERROR)
        y = batteryMenuLine(y, COLOR_RED, "  write error (want L=%lu), writes %lu", (u32)bars->levelWant, bars->writes);
    else
        y = batteryMenuLine(y, COLOR_RED, "  %s", BatteryBars_StateName(bars->state));
    if (led->barsLevel != BATTERY_BARS_STOCK)
        y = batteryMenuLine(y, COLOR_WHITE, "  decided L=%lu (%s), checks %lu", (u32)led->barsLevel, BatteryBars_ReasonName(led->barsReason),
                            bars->checks);
    else
        y = batteryMenuLine(y, COLOR_WHITE, "  decided stock (%s), checks %lu", BatteryBars_ReasonName(led->barsReason), bars->checks);
    y = batteryMenuLine(y, COLOR_WHITE, "");
    // Title ID of the "ptm" process, read by either patch's check (0 until the first one); 0004013020002202 = New 3DS
    u64 tid = ptm->titleId != 0 ? ptm->titleId : bars->titleId;
    if (tid != 0)
        y = batteryMenuLine(y, COLOR_WHITE, "ptm title ID:   %08lx%08lx", (u32)(tid >> 32), (u32)tid);
    else
        y = batteryMenuLine(y, COLOR_WHITE, "ptm title ID:   not read yet");
    return y;
}

// Page 3: recorder and curve. Draw_Lock must be held; curve from BatteryCurve_Snapshot (taken before the lock).
static u32 batteryDiagRecorderCurve(u32 y, const BatteryDiagData *d, const BatteryCurveFile *curve)
{
    const BatteryRecorderStatus *st = &d->st;
    char dur[16];

    BatteryMenu_FormatDuration(dur, st->elapsedMs);
    if (st->recording)
    {
        y = batteryMenuLine(y, COLOR_WHITE, "Recorder:       recording #%04lu", (u32)st->number);
        y = batteryMenuLine(y, COLOR_WHITE, "Elapsed:        %s", dur);
        y = batteryMenuLine(y, COLOR_WHITE, "Samples:        %lu (%lu not on SD yet)", st->samples, st->bufferedLines);
    }
    else
    {
        y = batteryMenuLine(y, COLOR_WHITE, "Recorder:       idle");
        if (st->number != 0 && st->recoveredNumber == st->number)
            y = batteryMenuLine(y, COLOR_WHITE, "Last:           #%04lu (cut off, closed at boot)", (u32)st->number);
        else if (st->number != 0)
            y = batteryMenuLine(y, COLOR_WHITE, "Last:           #%04lu, %s, %lu samples", (u32)st->number, dur, st->samples);
        else
            y = batteryMenuLine(y, COLOR_WHITE, "Last:           none this boot");
        if (st->endPending) // its end line couldn't be written yet (the recorder retries)
            y = batteryMenuLine(y, COLOR_RED, "End line:       not on SD yet, retrying");
        else
            y = batteryMenuLine(y, COLOR_WHITE, "");
    }
    // Sleep gap and errors have their own lines (they shared one before)
    if (st->sleepGap)
        y = batteryMenuLine(y, COLOR_RED, "Sleep gap:      yes, session invalid for curves");
    else
        y = batteryMenuLine(y, COLOR_WHITE, "Sleep gap:      no");
    y = batteryMenuLine(y, st->readErrors != 0 || st->writeErrors != 0 ? COLOR_RED : COLOR_WHITE,
                        "Errors:         %lu read, %lu write (0x%08lx)", st->readErrors, st->writeErrors, (u32)st->lastWriteRes);
    y = batteryMenuLine(y, COLOR_WHITE, "");

    if (curve != NULL)
    {
        y = batteryMenuLine(y, COLOR_WHITE, "Curve:          %lu session%s, max spread %lu.%02lu", (u32)curve->numSessions,
                            curve->numSessions == 1 ? "" : "s", (u32)curve->maxSpreadX100 / 100, (u32)curve->maxSpreadX100 % 100);
        // Session numbers, 5 per line (16 at most: 4 lines)
        for (u32 k = 0; k < curve->numSessions && k < BATTERY_CURVE_MAX_SESSIONS; k += 5)
        {
            char line[64];
            int n = sprintf(line, k == 0 ? "Sessions:      " : "               ");
            for (u32 j = k; j < k + 5 && j < curve->numSessions; j++)
                n += sprintf(line + n, " #%04lu", (u32)curve->sessions[j]);
            y = batteryMenuLine(y, COLOR_WHITE, "%s", line);
        }
        y = batteryMenuLine(y, curve->numOutliers != 0 ? COLOR_RED : COLOR_WHITE, "Outliers:       %lu (deviation > 5 points)",
                            (u32)curve->numOutliers);
    }
    else
        y = batteryMenuLine(y, COLOR_WHITE, "Curve:          none loaded");
    if (d->haveBak)
        y = batteryMenuLine(y, COLOR_WHITE, "Backup:         curve.bak, %lu session%s", (u32)d->bakCount, d->bakCount == 1 ? "" : "s");
    else
        y = batteryMenuLine(y, COLOR_WHITE, "Backup:         none (no valid curve.bak)");
    return y;
}

static void batteryMenuDiagnostics(void)
{
    BatteryDiagData *d = &g_diag;
    u32 page = 0;

    // curve.bak is only read here (an SD read), not on every refresh
    d->haveBak = BatteryCurve_BackupInfo(&d->bakCount, NULL);
    batteryMenuClear();

    while (!menuShouldExit)
    {
        char title[64];
        d->res = BatteryMcu_ReadSample(&d->s);
        BatteryRecorder_GetStatus(&d->st);
        BatteryLed_GetStatus(&d->led);
        BatteryPtmPatch_GetStatus(&d->ptm);
        BatteryBars_GetStatus(&d->bars);
        const BatteryCurveFile *curve = page == 2 ? BatteryCurve_Snapshot() : NULL;
        BatteryDiag_Title(title, page);

        Draw_Lock();
        Draw_DrawFormattedString(10, 10, COLOR_TITLE, "%-*s", BATTERY_MENU_LINE_CHARS, title);
        u32 y = 30;
        if (page == 0)
            y = batteryDiagGaugeLed(y, d);
        else if (page == 1)
            y = batteryDiagPtm(y, d);
        else
            y = batteryDiagRecorderCurve(y, d, curve);
        batteryMenuPadBody(y);
        Draw_DrawString(10, SCREEN_BOT_HEIGHT - 20, COLOR_TITLE, "Left/Right or L/R: page  B: back");
        Draw_FlushFramebuffer();
        Draw_Unlock();

        u32 pressed = waitInputWithTimeout(1000);
        if (pressed & KEY_B)
            break;
        else if (pressed & (KEY_RIGHT | KEY_R))
            page = (page + 1) % BATTERY_DIAG_PAGES;
        else if (pressed & (KEY_LEFT | KEY_L))
            page = (page + BATTERY_DIAG_PAGES - 1) % BATTERY_DIAG_PAGES;
    }
    batteryMenuClear();
}

// ---- Recording ----

static bool batteryRecordingTitleRefresh(void)
{
    BatteryRecorderStatus st;
    char title[48];
    BatteryRecorder_GetStatus(&st);
    if (st.recording)
    {
        char dur[16];
        BatteryMenu_FormatDuration(dur, st.elapsedMs);
        sprintf(title, "Stop recording  #%04lu  %s", (u32)st.number, dur);
    }
    else
        memcpy(title, "Start recording", 16);
    title[BATTERY_RECORDING_TITLE_CHARS] = 0;
    u32 len = strlen(title);
    memset(title + len, ' ', BATTERY_RECORDING_TITLE_CHARS - len); // fixed length: no residue from a longer title
    memcpy(g_recordingTitle, title, sizeof(g_recordingTitle));
    return true; // always visible
}

static void batteryStartRecording(void)
{
    static const char *const title = "Battery Manager - Start recording";
    char line[64];
    u16 number = 0;
    Result res = 0;
    BatteryMcuSample s;

    // Check first (a failed MCU read is reported by BatteryRecorder_Start below)
    if (R_SUCCEEDED(BatteryMcu_ReadSample(&s)))
    {
        u32 p = BatteryMcu_PercentX10(s.pctInt, s.pctFrac);
        u8 check = BatterySessions_StartCheck(p, (s.flags & BATTERY_MCU_FLAG_ADAPTER) != 0);
        if (check == BATTERY_START_CHARGER)
        {
            // The recorder refuses to start on the charger (a session ends as soon as the charger is plugged in)
            batteryTextBegin();
            batteryTextAdd(COLOR_WHITE, "The charger is plugged in.");
            batteryTextAdd(COLOR_WHITE, "");
            batteryTextAdd(COLOR_WHITE, "A recording has to run on the battery until");
            batteryTextAdd(COLOR_WHITE, "the console switches itself off.");
            batteryTextAdd(COLOR_WHITE, "Best: charge to 100 %%, unplug, then start.");
            batteryTextShow(title, "Press B to go back.", KEY_B);
            return;
        }
        if (check == BATTERY_START_LOW)
        {
            batteryTextBegin();
            batteryTextAdd(COLOR_RED, "The battery is at %lu.%lu %% (stock gauge).", p / 10, p % 10);
            batteryTextAdd(COLOR_WHITE, "");
            batteryTextAdd(COLOR_WHITE, "Only a session that starts at 95 %% or more");
            batteryTextAdd(COLOR_WHITE, "and runs until the console switches itself");
            batteryTextAdd(COLOR_WHITE, "off can be used for the curve. This one will");
            batteryTextAdd(COLOR_WHITE, "be partial (still useful as a record).");
            batteryTextAdd(COLOR_WHITE, "");
            batteryTextAdd(COLOR_WHITE, "Best: charge to 100 %%, unplug, then start.");
            if (!batteryTextConfirm(title, "A: start anyway  B: cancel"))
                return;
        }
    }

    BatteryRecorderError err = BatteryRecorder_Start(&number, &res);
    if (err == BATTERY_REC_OK)
    {
        batteryTextBegin();
        batteryTextAdd(COLOR_GREEN, "Recording session #%04lu.", (u32)number);
        batteryTextAdd(COLOR_WHITE, "");
        batteryTextAdd(COLOR_WHITE, "Use the console normally until it switches");
        batteryTextAdd(COLOR_WHITE, "itself off. Closing the lid (sleep) or");
        batteryTextAdd(COLOR_WHITE, "plugging in the charger makes the session");
        batteryTextAdd(COLOR_WHITE, "unusable for the curve.");
        batteryTextAdd(COLOR_WHITE, "To end early: Learning > Stop recording.");
        batteryTextShow(title, "Press B to go back.", KEY_B);
    }
    else
    {
        if (err == BATTERY_REC_ALREADY_RECORDING)
            sprintf(line, "Session #%04lu is being recorded.", (u32)number);
        else if (R_FAILED(res))
            sprintf(line, "Error 0x%08lx.", (u32)res);
        else
            line[0] = 0;
        batteryMenuMessage(title, BatteryRecorder_ErrorString(err), line);
    }
}

static void batteryStopRecording(const BatteryRecorderStatus *st)
{
    static const char *const title = "Battery Manager - Stop recording";
    char line[64], dur[16];
    u16 number = 0;

    BatteryMenu_FormatDuration(dur, st->elapsedMs);
    batteryTextBegin();
    batteryTextAdd(COLOR_WHITE, "Stop recording session #%04lu (%s)?", (u32)st->number, dur);
    batteryTextAdd(COLOR_WHITE, "");
    batteryTextAdd(COLOR_WHITE, "A stopped session can't be used for the curve:");
    batteryTextAdd(COLOR_WHITE, "only one that runs until the console switches");
    batteryTextAdd(COLOR_WHITE, "itself off can. The data stays on the SD card.");
    if (!batteryTextConfirm(title, "A: stop  B: cancel"))
        return;

    BatteryRecorderError err = BatteryRecorder_Stop(&number);
    if (err == BATTERY_REC_OK)
    {
        sprintf(line, "Session #%04lu stopped.", (u32)number);
        batteryMenuMessage(title, line, NULL);
    }
    else
        batteryMenuMessage(title, BatteryRecorder_ErrorString(err), NULL);
}

// Learning > Start recording / Stop recording #NNNN (one entry, the title shows which)
static void batteryMenuRecording(void)
{
    BatteryRecorderStatus st;
    BatteryRecorder_GetStatus(&st);
    if (st.recording)
        batteryStopRecording(&st);
    else
        batteryStartRecording();
}

// ---- Sessions ----

static BatterySessionInfo g_sessions[BATTERY_MAX_LISTED_SESSIONS];
static bool g_inCurve[BATTERY_MAX_LISTED_SESSIONS]; // same index as g_sessions: used by the loaded curve
static u32 g_numSessions;

// Reads and classifies all sessions (a few seconds for many long ones) and marks the ones in the loaded curve
static void batterySessionsRescan(const char *title)
{
    batteryMenuClear();
    Draw_Lock();
    Draw_DrawString(10, 10, COLOR_TITLE, title);
    Draw_DrawString(10, 30, COLOR_WHITE, "Reading sessions...");
    Draw_FlushFramebuffer();
    Draw_Unlock();

    if (R_FAILED(BatterySessions_Scan(g_sessions, BATTERY_MAX_LISTED_SESSIONS, &g_numSessions)))
        g_numSessions = 0;

    const BatteryCurveFile *curve = BatteryCurve_Snapshot();
    for (u32 i = 0; i < g_numSessions; i++)
    {
        g_inCurve[i] = false;
        for (u32 k = 0; curve != NULL && k < curve->numSessions; k++)
            if (curve->sessions[k] == g_sessions[i].number)
                g_inCurve[i] = true;
    }
    batteryMenuClear();
}

// User-facing class names (the CSV, the docs and BatterySessions_ClassName keep complete / incomplete / invalid)
static const char *batterySessionLabel(const BatterySessionInfo *info, const BatteryRecorderStatus *st)
{
    if (st->recording && st->number == info->number)
        return "recording";
    switch (info->cls)
    {
        case BATTERY_SESSION_COMPLETE:  return "usable";
        case BATTERY_SESSION_INVALID:   return "sleep gap";
        default:                        return "partial";
    }
}

static const char *batteryEndReasonText(u8 reason)
{
    switch (reason)
    {
        case BATTERY_END_STOP:      return "stopped in the menu";
        case BATTERY_END_CHARGER:   return "charger plugged in";
        case BATTERY_END_SHUTDOWN:  return "console switched off";
        case BATTERY_END_CUTOFF:    return "power lost (found at boot)";
        case BATTERY_END_OTHER:     return "other";
        default:                    return "none (not ended)";
    }
}

static void batterySessionDetails(const BatterySessionInfo *info, bool inCurve, const BatteryRecorderStatus *st)
{
    char dur[16];
    char title[48];
    BatteryMenu_FormatDuration(dur, info->durationMs);
    sprintf(title, "Battery Manager - Session #%04lu", (u32)info->number);

    batteryTextBegin();
    batteryTextAdd(COLOR_WHITE, "Result:      %s%s", batterySessionLabel(info, st), inCurve ? " (in the current curve)" : "");
    batteryTextAdd(COLOR_WHITE, "End reason:  %s", batteryEndReasonText(info->endReason));
    batteryTextAdd(COLOR_WHITE, "Sleep gap:   %s", info->hasSleepGap ? "yes" : "no");
    batteryTextAdd(COLOR_WHITE, "Duration:    %s", dur);
    batteryTextAdd(COLOR_WHITE, "Samples:     %lu", info->samples);
    batteryTextAdd(COLOR_WHITE, "Start:       %3lu.%lu %% (stock gauge)", (u32)info->startPctX10 / 10, (u32)info->startPctX10 % 10);
    batteryTextAdd(COLOR_WHITE, "End:         %3lu.%lu %% (stock gauge)", (u32)info->endPctX10 / 10, (u32)info->endPctX10 % 10);
    batteryTextAdd(COLOR_WHITE, "File size:   %lu bytes", info->sizeBytes);
    batteryTextAdd(COLOR_WHITE, "");
    batteryTextAdd(COLOR_WHITE, "usable = starts at 95 %% or more, runs until the");
    batteryTextAdd(COLOR_WHITE, "console switches itself off, no sleep.");
    batteryTextAdd(COLOR_WHITE, "partial = stopped, charger plugged in, or");
    batteryTextAdd(COLOR_WHITE, "started below 95 %%. sleep gap = the console");
    batteryTextAdd(COLOR_WHITE, "slept during it: never usable.");
    batteryTextShow(title, "Press B to go back.", KEY_B);
}

// Returns true if the session was deleted
static bool batterySessionDelete(const BatterySessionInfo *info, bool inCurve, const BatteryRecorderStatus *st)
{
    static const char *const title = "Battery Manager - Delete";
    if (st->recording && st->number == info->number)
    {
        batteryMenuMessage(title, "This session is being recorded.", "Stop recording first.");
        return false;
    }

    u32 m = info->durationMs / 60000;
    batteryTextBegin();
    batteryTextAdd(COLOR_WHITE, "Delete session #%04lu (%lu:%02lu, %s)?", (u32)info->number, m / 60, m % 60,
                   batterySessionLabel(info, st));
    batteryTextAdd(COLOR_WHITE, "");
    if (inCurve)
    {
        batteryTextAdd(COLOR_RED, "It is part of the current curve. The curve");
        batteryTextAdd(COLOR_RED, "stays as it is until you build a new one.");
    }
    batteryTextAdd(COLOR_WHITE, "The file is removed from the SD card.");
    if (!batteryTextConfirm(title, "A: delete  B: cancel"))
        return false;

    Result res = BatterySessions_Delete(info->number);
    if (R_FAILED(res))
    {
        char line[32];
        sprintf(line, "Error 0x%08lx.", (u32)res);
        batteryMenuMessage(title, "Delete failed.", line);
        return false;
    }
    // Its curve_select.ini entry goes too. The number is never reused (last_session), so a failure here only leaves
    // a harmless stale line
    BatteryCurve_SelectionForget(info->number);
    return true;
}

// Learning > Sessions: newest first, Up/Down select, A details, Y delete (asks first), B back
static void batteryMenuSessions(void)
{
    static const char *const title = "Battery Manager - Sessions";
    u32 selected = 0; // index in display order (newest first)
    u32 scroll = 0;
    u32 deletedNumber = 0; // feedback line until the next key

    batterySessionsRescan(title);

    do
    {
        BatteryRecorderStatus st;
        BatteryRecorder_GetStatus(&st);

        if (g_numSessions != 0 && selected >= g_numSessions)
            selected = g_numSessions - 1;
        if (selected < scroll)
            scroll = selected;
        else if (selected >= scroll + BATTERY_SESSION_ROWS)
            scroll = selected - BATTERY_SESSION_ROWS + 1;

        bool anyInCurve = false;
        for (u32 i = 0; i < g_numSessions; i++)
            anyInCurve = anyInCurve || g_inCurve[i];

        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, title);
        u32 y = 30;
        if (g_numSessions == 0)
            y = batteryMenuLine(y, COLOR_WHITE, "No sessions in " BATTERY_SESSIONS_DIR);
        else
            y = batteryMenuLine(y, COLOR_TITLE, "  #     Result      Length  Battery (stock)");

        for (u32 row = 0; row < BATTERY_SESSION_ROWS; row++)
        {
            u32 i = scroll + row;
            if (i >= g_numSessions)
            {
                y = batteryMenuLine(y, COLOR_WHITE, "");
                continue;
            }

            u32 k = g_numSessions - 1 - i;
            const BatterySessionInfo *info = &g_sessions[k];
            u32 m = info->durationMs / 60000;
            y = batteryMenuLine(y, COLOR_WHITE, "%c %04lu  %-9s %c %3lu:%02lu  %3lu.%lu -> %3lu.%lu %%",
                i == selected ? '>' : ' ', (u32)info->number, batterySessionLabel(info, &st), g_inCurve[k] ? '*' : ' ',
                m / 60, m % 60,
                (u32)info->startPctX10 / 10, (u32)info->startPctX10 % 10,
                (u32)info->endPctX10 / 10, (u32)info->endPctX10 % 10);
        }

        y += SPACING_Y;
        if (deletedNumber != 0)
            batteryMenuLine(y, COLOR_GREEN, "Session #%04lu deleted.", deletedNumber);
        else
            batteryMenuLine(y, COLOR_TITLE, anyInCurve ? "* in the current curve" : "");
        Draw_DrawString(10, SCREEN_BOT_HEIGHT - 20, COLOR_TITLE, "A: details  Y: delete  B: back");
        Draw_FlushFramebuffer();
        Draw_Unlock();

        u32 pressed = waitInput();
        deletedNumber = 0;
        if (pressed & KEY_B)
            break;
        else if (g_numSessions == 0)
            continue;
        else if (pressed & KEY_DOWN)
            selected = (selected + 1) % g_numSessions;
        else if (pressed & KEY_UP)
            selected = (selected + g_numSessions - 1) % g_numSessions;
        else if (pressed & KEY_A)
        {
            u32 k = g_numSessions - 1 - selected;
            batterySessionDetails(&g_sessions[k], g_inCurve[k], &st);
        }
        else if (pressed & KEY_Y) // only Y: X builds on the Build curve screen
        {
            u32 k = g_numSessions - 1 - selected;
            u16 number = g_sessions[k].number;
            if (batterySessionDelete(&g_sessions[k], g_inCurve[k], &st))
            {
                batterySessionsRescan(title);
                deletedNumber = number;
            }
        }
    }
    while (!menuShouldExit);
}

// ---- Build curve ----

typedef struct BatteryCandidate
{
    u16 number;
    bool selected;
    bool highEnd;    // ends above 5 %: probably powered off by hand
    u16 devX100;     // from the loaded curve, 0xFFFF if the session isn't in it
} BatteryCandidate;

static BatteryCandidate g_candidates[BATTERY_MAX_LISTED_SESSIONS];
static u32 g_numCandidates;

static void batteryCandidatesRefresh(void)
{
    BatteryRecorderStatus st;
    BatteryRecorder_GetStatus(&st);
    const BatteryCurveFile *curve = BatteryCurve_Snapshot();

    batterySessionsRescan("Battery Manager - Build curve"); // shows "Reading sessions..."
    BatteryCurve_SelectionLoad();

    g_numCandidates = 0;
    for (u32 i = g_numSessions; i > 0; i--) // newest first
    {
        const BatterySessionInfo *info = &g_sessions[i - 1];
        if (info->cls != BATTERY_SESSION_COMPLETE || (st.recording && st.number == info->number))
            continue;

        BatteryCandidate *c = &g_candidates[g_numCandidates++];
        c->number = info->number;
        c->highEnd = info->endPctX10 > BATTERY_CURVE_HIGH_END_X10;
        s32 sel = BatteryCurve_SelectionGet(info->number);
        c->selected = sel < 0 ? !c->highEnd : sel == 1;
        c->devX100 = 0xFFFF;
        for (u32 k = 0; curve != NULL && k < curve->numSessions; k++)
            if (curve->sessions[k] == info->number)
                c->devX100 = curve->sessionDevX100[k];
    }
}

static const BatterySessionInfo *batteryFindSession(u16 number)
{
    for (u32 i = 0; i < g_numSessions; i++)
        if (g_sessions[i].number == number)
            return &g_sessions[i];
    return NULL;
}

static void batteryBuildProgress(u32 pass, u32 index, u32 count, u32 number)
{
    Draw_Lock();
    Draw_DrawString(10, 10, COLOR_TITLE, "Battery Manager - Build curve");
    u32 y = batteryMenuLine(30, COLOR_WHITE, "Building...");
    y = batteryMenuLine(y, COLOR_WHITE, "Pass %lu/2: session #%04lu (%lu of %lu)", pass, number, index + 1, count);
    batteryMenuLine(y + SPACING_Y, COLOR_WHITE, "Reading the CSVs takes a few seconds.");
    Draw_FlushFramebuffer();
    Draw_Unlock();
}

static void batteryBuildResult(bool ok, const BatteryCurveBuildResult *r, bool hadCurve)
{
    const BatteryCurveFile *f = ok ? BatteryCurve_Snapshot() : NULL;

    batteryMenuClear();
    do
    {
        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "Battery Manager - Build curve");
        u32 y = 30;

        if (f != NULL)
        {
            y = batteryMenuLine(y, COLOR_GREEN, "Curve saved to " BATTERY_CURVE_PATH);
            if (hadCurve)
                y = batteryMenuLine(y, COLOR_WHITE, "The previous curve is kept as curve.bak.");
            y = batteryMenuLine(y, COLOR_WHITE, "Sessions used: %lu   Max spread: %lu.%02lu points",
                                (u32)f->numSessions, (u32)f->maxSpreadX100 / 100, (u32)f->maxSpreadX100 % 100);
            if (f->numOutliers != 0)
                y = batteryMenuLine(y, COLOR_RED, "Outliers: %lu (deviation > 5 points)", (u32)f->numOutliers);
            for (u32 k = 0; k < f->numSessions && k < 6; k++)
            {
                u16 d = f->sessionDevX100[k];
                if (f->numSessions >= 2)
                    y = batteryMenuLine(y, d > BATTERY_CURVE_OUTLIER_X100 ? COLOR_RED : COLOR_WHITE,
                                        "  #%04lu  deviation %lu.%02lu%s", (u32)f->sessions[k], (u32)d / 100, (u32)d % 100,
                                        d > BATTERY_CURVE_OUTLIER_X100 ? "  OUTLIER" : "");
                else
                    y = batteryMenuLine(y, COLOR_WHITE, "  #%04lu", (u32)f->sessions[k]);
            }
            if (f->numSessions > 6)
                y = batteryMenuLine(y, COLOR_WHITE, "  ...");

            // Stock gauge (MCU) % -> real % at a few points
            static const u8 points[] = { 50, 25, 10, 5, 2, 1 };
            u32 r10[sizeof(points)];
            for (u32 k = 0; k < sizeof(points); k++)
                r10[k] = (u32)(BatteryCurve_Interp(f->mcuGridX10, f->mcuMean, BATTERY_CURVE_MCU_POINTS, points[k] * 10.0f) * 10.0f + 0.5f);
            y += SPACING_Y;
            y = batteryMenuLine(y, COLOR_WHITE, "Stock 50/25/10 %% = real %lu.%lu / %lu.%lu / %lu.%lu %%",
                                r10[0] / 10, r10[0] % 10, r10[1] / 10, r10[1] % 10, r10[2] / 10, r10[2] % 10);
            y = batteryMenuLine(y, COLOR_WHITE, "Stock  5/ 2/ 1 %% = real %lu.%lu / %lu.%lu / %lu.%lu %%",
                                r10[3] / 10, r10[3] % 10, r10[4] / 10, r10[4] % 10, r10[5] / 10, r10[5] % 10);
        }
        else if (r->used == 0 && R_SUCCEEDED(r->res))
            y = batteryMenuLine(y, COLOR_RED, "No usable session: nothing written.");
        else if (r->res == BATTERY_FS_RES_OLD_NOT_KEPT) // curve.bak can't be deleted: curve.bin is left as it was
            y = batteryMenuLine(y, COLOR_RED, "curve.bak can't be replaced: nothing written.");
        else if (R_FAILED(r->res))
            y = batteryMenuLine(y, COLOR_RED, "SD error 0x%08lx: curve not written.", (u32)r->res);
        else
            y = batteryMenuLine(y, COLOR_RED, "Written, but curve.bin doesn't load back.");

        if (r->rejected != 0)
        {
            char line[64];
            int n = sprintf(line, "Skipped (too short / sleep):");
            u32 k = 0;
            for (; k < r->rejected && n + 6 <= BATTERY_MENU_LINE_CHARS - 4; k++) // leave room for " ..."
                n += sprintf(line + n, " #%04lu", (u32)r->rejectedNumbers[k]);
            if (k < r->rejected)
                sprintf(line + n, " ...");
            batteryMenuLine(y + SPACING_Y, COLOR_RED, "%s", line);
        }

        Draw_DrawString(10, SCREEN_BOT_HEIGHT - 20, COLOR_TITLE, "Press B to go back.");
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while (!(waitInput() & KEY_B) && !menuShouldExit);
    batteryMenuClear();
}

// After the first curve: offer to switch the power LED to corrected (only a config change, through BatteryLed_SetConfig
// like the Settings screen)
static void batteryOfferPowerLed(void)
{
    static const char *const title = "Battery Manager - Power LED";
    BatteryLedConfig cfg;
    BatteryLed_EnsureConfigLoaded();
    BatteryLed_GetConfig(&cfg);
    if (cfg.override)
        return;

    batteryTextBegin();
    batteryTextAdd(COLOR_GREEN, cfg.bars ? "The curve is ready; HOME Menu bars use it now." : "The curve is ready.");
    batteryTextAdd(COLOR_WHITE, "");
    batteryTextAdd(COLOR_WHITE, "Use it for the power LED too?");
    batteryTextAdd(COLOR_WHITE, "Power LED: corrected = blue above %lu %%, solid", (u32)cfg.redPct);
    batteryTextAdd(COLOR_WHITE, "red at %lu %% and below, blinking at %lu %% and", (u32)cfg.redPct, (u32)cfg.blinkPct);
    batteryTextAdd(COLOR_WHITE, "below, always blue while charging.");
    batteryTextAdd(COLOR_WHITE, "");
    batteryTextAdd(COLOR_WHITE, "You can change this later in Settings.");
    if (!batteryTextConfirm(title, "A: yes, turn on  B: later"))
        return;

    cfg.override = true;
    Result res = BatteryLed_SetConfig(&cfg);
    if (R_FAILED(res))
    {
        char line[48];
        sprintf(line, "Could not save config.ini (0x%08lx).", (u32)res);
        batteryMenuMessage(title, line, "Power LED: corrected until reboot.");
        return;
    }
    batteryTextBegin();
    batteryTextAdd(COLOR_GREEN, "Power LED: corrected.");
    batteryTextShow(title, "Press B to go back.", KEY_B);
}

// Returns true if a build ran (the candidates need a refresh then)
static bool batteryBuildRun(void)
{
    u16 numbers[BATTERY_CURVE_MAX_SESSIONS];
    u32 count = 0;
    BatteryCurveBuildResult r;

    for (u32 i = 0; i < g_numCandidates; i++)
    {
        if (!g_candidates[i].selected)
            continue;
        if (count == BATTERY_CURVE_MAX_SESSIONS)
        {
            batteryMenuMessage("Battery Manager - Build curve", "Too many sessions selected.", "Select at most 16.");
            return false;
        }
        numbers[count++] = g_candidates[i].number;
    }

    if (count == 0)
    {
        batteryMenuMessage("Battery Manager - Build curve", "No session selected.", NULL);
        return false;
    }

    // Confirmation: the new curve replaces the current one at once; the current one is kept as curve.bak
    const BatteryCurveFile *cur = BatteryCurve_Snapshot();
    bool hadCurve = cur != NULL;
    u32 curSessions = hadCurve ? cur->numSessions : 0;
    batteryTextBegin();
    if (hadCurve)
    {
        batteryTextAdd(COLOR_WHITE, "Build a new curve from %lu session%s?", count, count == 1 ? "" : "s");
        batteryTextAdd(COLOR_WHITE, "It replaces the current one (%lu session%s) and", curSessions, curSessions == 1 ? "" : "s");
        batteryTextAdd(COLOR_WHITE, "is used for the LED and bars right away.");
        batteryTextAdd(COLOR_WHITE, "The current curve is kept as curve.bak");
        batteryTextAdd(COLOR_WHITE, "(Advanced > Restore previous curve).");
    }
    else
    {
        batteryTextAdd(COLOR_WHITE, "Build a curve from %lu session%s?", count, count == 1 ? "" : "s");
        batteryTextAdd(COLOR_WHITE, "It is used for the LED and bars right away.");
    }
    if (!batteryTextConfirm("Battery Manager - Build curve", "A: build  B: cancel"))
        return false;

    batteryMenuClear();
    bool ok = BatteryCurve_Build(numbers, count, batteryBuildProgress, &r);
    batteryBuildResult(ok, &r, hadCurve);
    if (ok && !hadCurve)
        batteryOfferPowerLed();
    return true;
}

// Learning > Build curve: checklist of usable sessions, X builds (asks first)
static void batteryMenuBuildCurve(void)
{
    u32 selected = 0, scroll = 0;
    bool dirty = false;

    batteryCandidatesRefresh();

    do
    {
        if (g_numCandidates != 0 && selected >= g_numCandidates)
            selected = g_numCandidates - 1;
        if (selected < scroll)
            scroll = selected;
        else if (selected >= scroll + BATTERY_MENU_LIST_ROWS - 2)
            scroll = selected - (BATTERY_MENU_LIST_ROWS - 2) + 1;

        u32 numSelected = 0;
        for (u32 i = 0; i < g_numCandidates; i++)
            numSelected += g_candidates[i].selected ? 1 : 0;

        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "Battery Manager - Build curve");
        u32 y = 30;
        if (g_numCandidates == 0)
        {
            y = batteryMenuLine(y, COLOR_WHITE, "No usable sessions yet.");
            y = batteryMenuLine(y, COLOR_WHITE, "usable = starts at 95 %% or more, runs until the");
            y = batteryMenuLine(y, COLOR_WHITE, "console switches itself off, no sleep.");
        }
        else
        {
            y = batteryMenuLine(y, COLOR_WHITE, "Usable sessions: %lu, selected: %lu (max 16)", g_numCandidates, numSelected);
            y = batteryMenuLine(y, COLOR_TITLE, "ends = stock gauge %% when it switched off");
        }

        for (u32 row = 0; row < BATTERY_MENU_LIST_ROWS - 2; row++)
        {
            u32 i = scroll + row;
            if (i >= g_numCandidates)
            {
                if (g_numCandidates != 0)
                    y = batteryMenuLine(y, COLOR_WHITE, "");
                continue;
            }

            const BatteryCandidate *c = &g_candidates[i];
            const BatterySessionInfo *info = batteryFindSession(c->number);
            u32 m = info != NULL ? info->durationMs / 60000 : 0;
            u32 e = info != NULL ? info->endPctX10 : 0;
            const char *flag = c->highEnd ? "high end %" :
                               c->devX100 != 0xFFFF && c->devX100 > BATTERY_CURVE_OUTLIER_X100 ? "outlier" :
                               c->devX100 != 0xFFFF ? "in curve" : "";
            y = batteryMenuLine(y, c->highEnd || (c->devX100 != 0xFFFF && c->devX100 > BATTERY_CURVE_OUTLIER_X100) ? COLOR_RED : COLOR_WHITE,
                "%c[%c] %04lu %3lu:%02lu ends %3lu.%lu %% %s",
                i == selected ? '>' : ' ', c->selected ? 'x' : ' ', (u32)c->number, m / 60, m % 60, e / 10, e % 10, flag);
        }

        Draw_DrawString(10, SCREEN_BOT_HEIGHT - 20, COLOR_TITLE, "A: select  X: build  B: back");
        Draw_FlushFramebuffer();
        Draw_Unlock();

        u32 pressed = waitInput();
        if (pressed & KEY_B)
            break;
        else if (g_numCandidates == 0)
            continue;
        else if (pressed & KEY_DOWN)
            selected = (selected + 1) % g_numCandidates;
        else if (pressed & KEY_UP)
            selected = (selected + g_numCandidates - 1) % g_numCandidates;
        else if (pressed & KEY_A)
        {
            BatteryCandidate *c = &g_candidates[selected];
            c->selected = !c->selected;
            BatteryCurve_SelectionSet(c->number, c->selected);
            dirty = true;
        }
        else if (pressed & (KEY_X | KEY_START))
        {
            if (dirty)
                BatteryCurve_SelectionSave();
            dirty = false;
            if (batteryBuildRun())
                batteryCandidatesRefresh(); // outlier flags of the new curve
        }
    }
    while (!menuShouldExit);

    if (dirty)
        BatteryCurve_SelectionSave();
}

// ---- Curve graph ----

static void batteryMenuGraph(void)
{
    bool zoom = false;
    const BatteryCurveFile *curve = BatteryCurve_Snapshot();

    do
    {
        Draw_Lock();
        Draw_ClearFramebuffer();
        if (curve != NULL)
            Draw_DrawFormattedString(10, 10, COLOR_TITLE, "Battery curve, %lu session(s)%s",
                                     (u32)curve->numSessions, zoom ? ", stock 0-10%" : "");
        else
            Draw_DrawString(10, 10, COLOR_TITLE, "Battery curve");
        BatteryGraph_Draw(curve, zoom);
        Draw_DrawString(10, SCREEN_BOT_HEIGHT - 20, COLOR_TITLE, zoom ? "Y: full view  B: back" : "Y: zoom 0-10%  B: back");
        Draw_DrawString(SCREEN_BOT_WIDTH - 4 - 19 * SPACING_X, SCREEN_BOT_HEIGHT - 20, COLOR_WHITE, "x: stock, y: real %");
        Draw_FlushFramebuffer();
        Draw_Unlock();

        u32 pressed;
        do
            pressed = waitInput();
        while (!(pressed & (KEY_B | KEY_Y)) && !menuShouldExit);

        if (pressed & KEY_B)
            break;
        zoom = !zoom;
    }
    while (!menuShouldExit);

    batteryMenuClear();
}

// ---- Settings ----

#define BATTERY_SETTING_LABEL_CHARS 18
#define BATTERY_SETTING_HELP_LINES  6

static const char *batterySettingLabel(u8 setting)
{
    switch (setting)
    {
        case BATTERY_SETTING_POWER_LED: return "Power LED";
        case BATTERY_SETTING_SLEEP_LED: return "Sleep LED";
        case BATTERY_SETTING_BARS:      return "HOME Menu bars";
        case BATTERY_SETTING_RED:       return "Red LED at";
        case BATTERY_SETTING_BLINK:     return "Blinking red at";
        case BATTERY_SETTING_BARS4:     return "4 bars from";
        case BATTERY_SETTING_BARS3:     return "3 bars from";
        case BATTERY_SETTING_BLUE_MODE: return "Blue LED type";
        default:                        return "?";
    }
}

// Value column of a row (at most 22 chars)
static void batterySettingValue(char *out, u8 setting, const BatteryLedConfig *cfg)
{
    switch (setting)
    {
        case BATTERY_SETTING_POWER_LED:
            memcpy(out, cfg->override ? "corrected" : "stock", cfg->override ? 10 : 6);
            break;
        case BATTERY_SETTING_SLEEP_LED:
            sprintf(out, "%s%s", cfg->sleepMode == BATTERY_LED_MODE_BLUE ? "bright blue" :
                                 cfg->sleepMode == BATTERY_LED_SLEEP_SYSTEM ? "stock" : "dim blue",
                    cfg->override ? "" : " (inactive)");
            break;
        case BATTERY_SETTING_BARS:
            memcpy(out, cfg->bars ? "corrected" : "stock", cfg->bars ? 10 : 6);
            break;
        case BATTERY_SETTING_RED:   sprintf(out, "%3lu %%", (u32)cfg->redPct); break;
        case BATTERY_SETTING_BLINK: sprintf(out, "%3lu %%", (u32)cfg->blinkPct); break;
        case BATTERY_SETTING_BARS4: sprintf(out, "%3lu %%", (u32)cfg->bars4Pct); break;
        case BATTERY_SETTING_BARS3: sprintf(out, "%3lu %%", (u32)cfg->bars3Pct); break;
        case BATTERY_SETTING_BLUE_MODE:
            sprintf(out, "%s", cfg->blueMode == BATTERY_LED_MODE_BLUE_FORCED ? "forced blue (1)" : "red off, blue on (5)");
            break;
        default:
            out[0] = 0;
            break;
    }
}

// Help text of the selected row: always BATTERY_SETTING_HELP_LINES lines (blank-padded, so a shorter text clears a longer
// one). Each line fits 49 chars with the largest values (red 50, blink 49, bars 100 / 99). Draw_Lock must be held.
static u32 batterySettingHelp(u32 y, u8 setting, const BatteryLedConfig *cfg)
{
    u32 red = cfg->redPct, blink = cfg->blinkPct, n = 0;

#define HELP(color, ...) do { y = batteryMenuLine(y, color, __VA_ARGS__); n++; } while (0)
    switch (setting)
    {
        case BATTERY_SETTING_POWER_LED:
            HELP(COLOR_WHITE, "corrected: the power LED follows the corrected");
            HELP(COLOR_WHITE, "battery %%: blue above %lu %%, solid red at %lu %%", red, red);
            HELP(COLOR_WHITE, "and below, blinking red at %lu %% and below.", blink);
            HELP(COLOR_WHITE, "Always blue while charging.");
            HELP(COLOR_WHITE, "stock: the console's own LED (it turns red far");
            HELP(COLOR_WHITE, "too early with this battery).");
            break;
        case BATTERY_SETTING_SLEEP_LED:
            HELP(COLOR_WHITE, "Lid closed (sleep): dim or bright blue while the");
            HELP(COLOR_WHITE, "battery is above the red level or charging,");
            HELP(COLOR_WHITE, "steady red at the red level, the console's own");
            HELP(COLOR_WHITE, "warning at the blinking level.");
            HELP(COLOR_WHITE, "stock: the console's own sleep light.");
            HELP(cfg->override ? COLOR_WHITE : COLOR_RED, "Needs Power LED: corrected.");
            break;
        case BATTERY_SETTING_BARS:
            HELP(COLOR_WHITE, "HOME Menu battery icon from the corrected %%:");
            HELP(COLOR_WHITE, "4 bars from %lu %%, 3 bars from %lu %%, 2 bars", (u32)cfg->bars4Pct, (u32)cfg->bars3Pct);
            HELP(COLOR_WHITE, "above %lu %%, 1 red bar at %lu %%, blinking at %lu %%.", red, red, blink);
            HELP(COLOR_WHITE, "Held while charging. Changes show within about");
            HELP(COLOR_WHITE, "8 s. Works without Power LED: corrected, but");
            HELP(COLOR_WHITE, "needs a curve. stock: the console's own bars.");
            break;
        case BATTERY_SETTING_RED:
        case BATTERY_SETTING_BLINK:
            if (setting == BATTERY_SETTING_RED)
            {
                HELP(COLOR_WHITE, "Corrected %% at which the power LED turns solid");
                HELP(COLOR_WHITE, "red and HOME Menu shows 1 red bar.");
            }
            else
            {
                HELP(COLOR_WHITE, "Corrected %% at which the power LED and the");
                HELP(COLOR_WHITE, "HOME Menu bar start blinking red.");
            }
            HELP(COLOR_WHITE, "Order: blinking < red < 3 bars < 4 bars.");
            HELP(COLOR_WHITE, "Within one discharge the LED only gets worse;");
            HELP(COLOR_WHITE, "after a change it gets better again only if");
            HELP(COLOR_WHITE, "the battery is over 0.5 points above it.");
            break;
        case BATTERY_SETTING_BARS4:
        case BATTERY_SETTING_BARS3:
            HELP(COLOR_WHITE, "HOME Menu shows %s bars from this corrected %%", setting == BATTERY_SETTING_BARS4 ? "4" : "3");
            HELP(COLOR_WHITE, "upwards (2 bars from just above the red level).");
            HELP(COLOR_WHITE, "Order: blinking < red < 3 bars < 4 bars.");
            HELP(COLOR_WHITE, "Only used with HOME Menu bars: corrected.");
            break;
        case BATTERY_SETTING_BLUE_MODE:
            HELP(COLOR_WHITE, "How blue is shown on the power LED (corrected):");
            HELP(COLOR_WHITE, "red off, blue on (5): the default.");
            HELP(COLOR_WHITE, "forced blue (1): the console's default blue.");
            HELP(COLOR_WHITE, "Only try 1 if blue looks wrong with 5. Used at");
            HELP(COLOR_WHITE, "the blue level and while charging; the Sleep");
            HELP(COLOR_WHITE, "LED setting is separate.");
            break;
        default:
            break;
    }
#undef HELP
    for (; n < BATTERY_SETTING_HELP_LINES; n++)
        y = batteryMenuLine(y, COLOR_WHITE, "");
    return y;
}

static bool batteryConfigEqual(const BatteryLedConfig *a, const BatteryLedConfig *b)
{
    return a->override == b->override && a->redPct == b->redPct && a->blinkPct == b->blinkPct && a->blueMode == b->blueMode &&
           a->sleepMode == b->sleepMode && a->bars == b->bars && a->bars4Pct == b->bars4Pct && a->bars3Pct == b->bars3Pct;
}

// One editor for all settings: Up/Down select a row, Left/Right change it (BatteryLedConfig_Step keeps the limits),
// SELECT undoes every change made here, B saves (only if something changed) and goes back. Same config.ini writes and
// LED re-evaluation as the old per-entry toggles / Thresholds screen: one BatteryLed_SetConfig on the way out.
static void batterySettingsEditor(const char *title, const u8 *rows, u32 numRows, bool curveNote)
{
    BatteryLedConfig cfg, orig;
    u32 row = 0;

    BatteryLed_EnsureConfigLoaded();
    BatteryLed_GetConfig(&cfg);
    orig = cfg;
    bool haveCurve = BatteryCurve_IsLoaded();

    batteryMenuClear();
    do
    {
        bool dirty = !batteryConfigEqual(&cfg, &orig);

        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, title);
        u32 y = 30;
        for (u32 i = 0; i < numRows; i++)
        {
            char value[32];
            batterySettingValue(value, rows[i], &cfg);
            bool inactive = rows[i] == BATTERY_SETTING_SLEEP_LED && !cfg.override;
            y = batteryMenuLine(y, inactive ? COLOR_TITLE : COLOR_WHITE, "%c %-*s %s", i == row ? '>' : ' ',
                                BATTERY_SETTING_LABEL_CHARS, batterySettingLabel(rows[i]), value);
        }
        y += SPACING_Y;
        y = batterySettingHelp(y, rows[row], &cfg);
        y += SPACING_Y;
        if (curveNote && !haveCurve)
            y = batteryMenuLine(y, COLOR_RED, "No curve yet: Power LED and bars stay stock.");
        else
            y = batteryMenuLine(y, COLOR_WHITE, "");
        batteryMenuLine(y, COLOR_TITLE, dirty ? "Not saved yet." : "");
        Draw_DrawString(10, SCREEN_BOT_HEIGHT - 20, COLOR_TITLE, "Left/Right: change  SELECT: undo  B: save, back");
        Draw_FlushFramebuffer();
        Draw_Unlock();

        u32 pressed = waitInput();
        if (pressed & KEY_B)
            break;
        else if (pressed & KEY_DOWN)
            row = (row + 1) % numRows;
        else if (pressed & KEY_UP)
            row = (row + numRows - 1) % numRows;
        else if (pressed & (KEY_LEFT | KEY_RIGHT))
            BatteryLedConfig_Step(&cfg, rows[row], (pressed & KEY_RIGHT) ? 1 : -1);
        else if (pressed & KEY_SELECT)
            cfg = orig;
    }
    while (!menuShouldExit);

    if (!batteryConfigEqual(&cfg, &orig))
    {
        // On a forced close (lid / sleep: menuShouldExit) BatteryLed_SetConfig saves now and applies after wake
        Result res = BatteryLed_SetConfig(&cfg);
        if (R_FAILED(res))
        {
            // F9: the settings are already active, only the file failed
            char line[48];
            sprintf(line, "Could not save config.ini (0x%08lx).", (u32)res);
            batteryMenuMessage(title, line, "The new settings are active until reboot.");
        }
        else
            batteryFlashHint(COLOR_GREEN, "Saved.");
    }
    batteryMenuClear();
}

static void batteryMenuSettings(void)
{
    static const u8 rows[] = {
        BATTERY_SETTING_POWER_LED, BATTERY_SETTING_SLEEP_LED, BATTERY_SETTING_BARS, BATTERY_SETTING_RED,
        BATTERY_SETTING_BLINK, BATTERY_SETTING_BARS4, BATTERY_SETTING_BARS3,
    };
    batterySettingsEditor("Battery Manager - Settings", rows, sizeof(rows), true);
}

// Advanced > Blue LED type (config.ini blue_mode)
static void batteryMenuBlueLedType(void)
{
    static const u8 rows[] = { BATTERY_SETTING_BLUE_MODE };
    batterySettingsEditor("Battery Manager - Blue LED type", rows, sizeof(rows), false);
}

// ---- Advanced ----

// Advanced > Restore previous curve: curve.bak (kept by the last build or restore) becomes the curve again; the current
// curve becomes curve.bak, so a second restore switches back
static void batteryMenuRestoreCurve(void)
{
    static const char *const title = "Battery Manager - Restore previous curve";
    u16 bakCount = 0, bakSessions[BATTERY_CURVE_MAX_SESSIONS];
    char line[64];

    const BatteryCurveFile *cur = BatteryCurve_Snapshot();
    u32 curCount = cur != NULL ? cur->numSessions : 0;
    bool haveCurve = cur != NULL;
    if (!BatteryCurve_BackupInfo(&bakCount, bakSessions)) // overwrites the snapshot buffer: cur isn't used below
    {
        batteryMenuMessage(title, "There is no previous curve (curve.bak) yet.",
                           "Building a new curve keeps the old one.");
        return;
    }

    int n = sprintf(line, " ");
    u32 k = 0;
    for (; k < bakCount && n + 6 <= BATTERY_MENU_LINE_CHARS - 4; k++)
        n += sprintf(line + n, " #%04lu", (u32)bakSessions[k]);
    if (k < bakCount)
        sprintf(line + n, " ...");

    batteryTextBegin();
    batteryTextAdd(COLOR_WHITE, "Go back to the previous curve?");
    batteryTextAdd(COLOR_WHITE, "");
    batteryTextAdd(COLOR_WHITE, "Previous: %lu session%s", (u32)bakCount, bakCount == 1 ? "" : "s");
    batteryTextAdd(COLOR_WHITE, "%s", line);
    if (haveCurve)
        batteryTextAdd(COLOR_WHITE, "Current:  %lu session%s", curCount, curCount == 1 ? "" : "s");
    else
        batteryTextAdd(COLOR_WHITE, "Current:  none");
    batteryTextAdd(COLOR_WHITE, "");
    batteryTextAdd(COLOR_WHITE, "It is used for the LED and bars right away.");
    if (haveCurve)
    {
        batteryTextAdd(COLOR_WHITE, "The current curve becomes the previous one,");
        batteryTextAdd(COLOR_WHITE, "so you can switch back the same way.");
    }
    if (!batteryTextConfirm(title, "A: restore  B: cancel"))
        return;

    Result res = 0;
    u8 r = BatteryCurve_RestoreBackup(&res);
    if (r == BATTERY_CURVE_RESTORE_OK)
    {
        batteryTextBegin();
        batteryTextAdd(COLOR_GREEN, "Previous curve restored (%lu session%s).", (u32)bakCount, bakCount == 1 ? "" : "s");
        batteryTextShow(title, "Press B to go back.", KEY_B);
    }
    else if (r == BATTERY_CURVE_RESTORE_NO_BACKUP)
        batteryMenuMessage(title, "curve.bak is missing or invalid.", "Nothing was changed.");
    else if (r == BATTERY_CURVE_RESTORE_SD_ERROR && res == BATTERY_FS_RES_OLD_NOT_KEPT)
        batteryMenuMessage(title, "curve.bak can't be replaced.", "Nothing was changed.");
    else if (r == BATTERY_CURVE_RESTORE_SD_ERROR)
    {
        sprintf(line, "SD error 0x%08lx.", (u32)res);
        batteryMenuMessage(title, "Could not restore the curve.", line);
    }
    else
        batteryMenuMessage(title, "Written, but curve.bin doesn't load back.", NULL);
}

// ---- Setup guide ----

// Static steps, ticked from the current state. A loaded curve means steps 1-4 are done (it can only be built from usable
// sessions); otherwise the sessions are scanned for a usable one (same "Reading sessions..." as the Sessions screen).
static void batteryMenuSetupGuide(void)
{
    static const char *const title = "Battery Manager - Setup guide";
    BatteryRecorderStatus st;
    BatteryLedConfig cfg;
    BatteryMcuSample s;

    BatteryRecorder_GetStatus(&st);
    BatteryLed_EnsureConfigLoaded();
    BatteryLed_GetConfig(&cfg);
    bool haveCurve = BatteryCurve_IsLoaded();
    bool usable = haveCurve;
    if (!haveCurve)
    {
        batterySessionsRescan(title);
        for (u32 i = 0; i < g_numSessions && !usable; i++)
            usable = g_sessions[i].cls == BATTERY_SESSION_COMPLETE && !(st.recording && st.number == g_sessions[i].number);
    }
    bool full = R_SUCCEEDED(BatteryMcu_ReadSample(&s)) && !(s.flags & BATTERY_MCU_FLAG_ADAPTER) &&
                BatterySessions_StartCheck(BatteryMcu_PercentX10(s.pctInt, s.pctFrac), false) == BATTERY_START_OK;

    bool done[5] = {
        usable || st.recording || full,
        usable || st.recording,
        usable,
        haveCurve,
        cfg.override,
    };

#define STEP(i, text) batteryTextAdd(done[i] ? COLOR_GREEN : COLOR_WHITE, "[%c] %lu %s", done[i] ? 'x' : ' ', (u32)(i) + 1, text)
    batteryTextBegin();
    batteryTextAdd(COLOR_WHITE, "Your battery's real charge differs from what");
    batteryTextAdd(COLOR_WHITE, "the console shows. This learns the real curve.");
    batteryTextAdd(COLOR_WHITE, "");
    STEP(0, "Charge to 100 %, then unplug.");
    STEP(1, "Learning > Start recording.");
    STEP(2, "Play until it switches itself off");
    batteryTextAdd(done[2] ? COLOR_GREEN : COLOR_WHITE, "      (no sleep, no charger; several hours).");
    STEP(3, "Switch on, Learning > Build curve.");
    STEP(4, "Settings > Power LED: corrected.");
    batteryTextAdd(COLOR_WHITE, "");
    batteryTextAdd(COLOR_WHITE, "HOME Menu bars switch over by themselves");
    batteryTextAdd(COLOR_WHITE, "once a curve exists. More discharges make");
    batteryTextAdd(COLOR_WHITE, "the curve more accurate.");
    if (st.recording)
    {
        char dur[16];
        BatteryMenu_FormatDuration(dur, st.elapsedMs);
        batteryTextAdd(COLOR_WHITE, "");
        batteryTextAdd(COLOR_TITLE, "Recording session #%04lu (%s).", (u32)st.number, dur);
    }
#undef STEP
    batteryTextShow(title, "Press B to go back.", KEY_B);
}

// Advanced > Limits and about: static text (docs 14.10, 15.10)
static void batteryMenuAbout(void)
{
    static const char *const lines[] = {
        "- The first ~5 s after power-on the LED is the",
        "  console's own (Rosalina isn't running yet).",
        "- During sleep the LED can't follow the battery:",
        "  a change shows on wake. A charger plugged in",
        "  while asleep shows when the lid opens.",
        "- HOME Menu bars change within about 8 s.",
        "- Sleep LED and HOME Menu bars need the New 3DS",
        "  system module they were made for; otherwise",
        "  they stay stock (see Diagnostics).",
        "- Apps that read the gauge directly still get",
        "  the stock %.",
        "- Nothing is written to NAND. Files: SD card,",
        "  /luma/battery/. To undo everything, use the",
        "  official Luma3DS boot.firm.",
        "",
        "Luma3DS-Gauge, an unofficial Luma3DS fork.",
    };

    batteryMenuClear();
    do
    {
        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "Battery Manager - Limits and about");
        u32 y = 30;
        for (u32 i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
            y = batteryMenuLine(y, COLOR_WHITE, "%s", lines[i]);
        Draw_DrawString(10, SCREEN_BOT_HEIGHT - 20, COLOR_TITLE, "Press B to go back.");
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while (!(waitInput() & KEY_B) && !menuShouldExit);
}

// Advanced > LED test: writes whitelisted modes by hand (BatteryLed_TestSetMode); normal control is paused meanwhile
static void batteryMenuLedTest(void)
{
    static const u8 modes[] = {
        BATTERY_LED_MODE_AUTO, BATTERY_LED_MODE_BLUE_FORCED, BATTERY_LED_MODE_SLEEP, BATTERY_LED_MODE_POWER_OFF,
        BATTERY_LED_MODE_RED, BATTERY_LED_MODE_BLUE, BATTERY_LED_MODE_BLINK,
    };
    static const char *const descriptions[] = {
        "automatic (MCU decides)", "forced default blue", "sleep animation", "\"power off\" mode",
        "blue off, red on", "red off, blue on", "blue off, flash red",
    };
    u32 row = 0;
    s32 written = -1;
    Result lastRes = 0;

    BatteryLed_TestBegin();
    batteryMenuClear();
    do
    {
        BatteryMcuSample s;
        Result res = BatteryMcu_ReadSample(&s);

        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "Battery Manager - LED test");
        u32 y = 30;
        for (u32 i = 0; i < sizeof(modes); i++)
            y = batteryMenuLine(y, (s32)i == written ? COLOR_GREEN : COLOR_WHITE, "%c %lu  %s", row == i ? '>' : ' ',
                                (u32)modes[i], descriptions[i]);
        y += SPACING_Y;
        if (written >= 0)
            y = batteryMenuLine(y, R_FAILED(lastRes) ? COLOR_RED : COLOR_WHITE, "Written: %lu  result 0x%08lx", (u32)modes[written], (u32)lastRes);
        else
            y = batteryMenuLine(y, COLOR_WHITE, "Written: nothing yet");
        if (R_SUCCEEDED(res))
        {
            if (s.ledModeValid)
                y = batteryMenuLine(y, COLOR_WHITE, "Read back 0x29: %lu", (u32)s.ledMode);
            else
                y = batteryMenuLine(y, COLOR_WHITE, "Read back 0x29: ?");
            if (s.infoValid)
                y = batteryMenuLine(y, COLOR_WHITE, "Red LED: %-3s  Blue intensity: %lu", s.info[0x0A] ? "on" : "off", (u32)s.info[0x0B]);
            else
                y = batteryMenuLine(y, COLOR_WHITE, "Red LED: ?    Blue intensity: ?");
        }
        else
        {
            y = batteryMenuLine(y, COLOR_RED, "MCU read failed (0x%08lx).", (u32)res);
            y = batteryMenuLine(y, COLOR_WHITE, "");
        }
        y += SPACING_Y;
        y = batteryMenuLine(y, COLOR_WHITE, "Normal LED control is paused here and");
        batteryMenuLine(y, COLOR_WHITE, "comes back when you leave this screen.");
        Draw_DrawString(10, SCREEN_BOT_HEIGHT - 20, COLOR_TITLE, "Up/Down: select  A: write  B: back");
        Draw_FlushFramebuffer();
        Draw_Unlock();

        u32 pressed = waitInputWithTimeout(250); // refresh the read-back
        if (pressed & KEY_B)
            break;
        else if (pressed & KEY_DOWN)
            row = (row + 1) % sizeof(modes);
        else if (pressed & KEY_UP)
            row = (row + sizeof(modes) - 1) % sizeof(modes);
        else if (pressed & KEY_A)
        {
            lastRes = BatteryLed_TestSetMode(modes[row]);
            written = (s32)row;
        }
    }
    while (!menuShouldExit);

    BatteryLed_TestEnd();
    batteryMenuClear();
}

// ---- Home screen ----

typedef struct BatteryHomeData
{
    BatteryHomeInputs in;
    BatteryHomeLine lines[BATTERY_HOME_SUMMARY_LINES];
    BatteryMcuSample s;
    BatteryRecorderStatus st;
    BatteryLedStatus led;
    BatteryPtmPatchStatus ptm;
    BatteryBarsStatus bars;
} BatteryHomeData;

static BatteryHomeData g_home; // menu thread only (static: keeps the stack small under the nested menuShow)

static u8 batteryHomePatchState(u8 state)
{
    // BATTERY_PTM_STATE_* and BATTERY_BARS_STATE_* share UNCHECKED = 0 and OK = 1
    return state == BATTERY_PTM_STATE_OK ? BATTERY_HOME_PATCH_OK :
           state == BATTERY_PTM_STATE_UNCHECKED ? BATTERY_HOME_PATCH_UNCHECKED : BATTERY_HOME_PATCH_UNAVAILABLE;
}

_Static_assert(BATTERY_PTM_STATE_UNCHECKED == 0 && BATTERY_BARS_STATE_UNCHECKED == 0 && BATTERY_PTM_STATE_OK == 1 &&
               BATTERY_BARS_STATE_OK == 1, "batteryHomePatchState maps both patch states the same way");

// Reads the live state (MCU read, status getters, curve snapshot): the same sources as Diagnostics, nothing written
static void batteryHomeGather(BatteryHomeData *h, bool *haveVoltTemp)
{
    BatteryHomeInputs *in = &h->in;
    memset(in, 0, sizeof(*in));

    Result res = BatteryMcu_ReadSample(&h->s);
    BatteryRecorder_GetStatus(&h->st);
    BatteryLed_GetStatus(&h->led);
    BatteryLed_GetConfig(&in->cfg);
    BatteryPtmPatch_GetStatus(&h->ptm);
    BatteryBars_GetStatus(&h->bars);
    const BatteryCurveFile *curve = BatteryCurve_Snapshot();

    in->haveSample = R_SUCCEEDED(res);
    *haveVoltTemp = in->haveSample;
    if (in->haveSample)
    {
        in->pctX10 = BatteryMcu_PercentX10(h->s.pctInt, h->s.pctFrac);
        in->adapter = (h->s.flags & BATTERY_MCU_FLAG_ADAPTER) != 0;
        in->charging = (h->s.flags & BATTERY_MCU_FLAG_CHARGING) != 0;
    }
    // Same corrected % as Diagnostics and the footer: the LED's (median voltage), instantaneous before its first poll
    if (h->led.haveData)
        in->corrected = h->led.corrected;
    else
        in->corrected = in->haveSample ? BatteryCurve_Corrected(h->s.pctInt + h->s.pctFrac / 256.0f, (float)h->s.voltageRaw) : -1.0f;
    in->etaState = h->led.etaState; // computed by the LED thread (section 17); "calculating" before its first poll
    in->etaMinutes = h->led.etaMinutes;
    in->ledReason = h->led.reason;
    in->ledDesired = h->led.desired;
    in->ledError = h->led.error;
    in->barsLevel = h->led.barsLevel;
    in->barsReason = h->led.barsReason;
    in->barsPatch = batteryHomePatchState(h->bars.state);
    in->sleepPatch = batteryHomePatchState(h->ptm.state);
    in->haveCurve = curve != NULL;
    in->curveSessions = curve != NULL ? curve->numSessions : 0;
    in->recording = h->st.recording;
    in->recNumber = h->st.number;
    in->recElapsedMs = h->st.elapsedMs;
    in->recSleepGap = h->st.sleepGap;
    in->recErrors = h->st.readErrors != 0 || h->st.writeErrors != 0;
}

static u32 batteryToneColor(u8 tone)
{
    return tone == BATTERY_TONE_ERROR ? COLOR_RED : tone == BATTERY_TONE_INACTIVE ? COLOR_TITLE : COLOR_WHITE;
}

// Rosalina > Battery Manager...: live summary (refreshed every second), a "Next:" hint and the four entries. Up/Down
// select, A opens, B goes back, like a Rosalina menu. Settings and Setup guide are called directly; Learning... and
// Advanced... are opened with menuShow, so they behave exactly like before (same items, dynamic recording title, footer).
// Layout (fixed rows, every one padded, so the 1 s redraw leaves no residue): 8 summary rows, blank, 2 hint rows,
// blank, 4 entries; 16 of the 17 body lines.
void BatteryMenu_Home(void)
{
    BatteryHomeData *h = &g_home;
    u32 selected = 0;

    BatteryLed_EnsureConfigLoaded(); // read only: config.ini once, like Settings
    batteryMenuClear();

    while (!menuShouldExit)
    {
        bool haveVoltTemp;
        char voltTemp[16];
        const char *next1, *next2;

        batteryHomeGather(h, &haveVoltTemp);
        BatteryHome_Summary(&h->in, h->lines);
        u8 step = BatteryHome_NextStep(&h->in);
        BatteryHome_NextText(step, &next1, &next2);
        bool fahrenheit = (lumaMcuConfig.flags & MCUCFG_FLAG_TEMPERATURE_IN_FAHRENHEIT) != 0;
        if (haveVoltTemp)
            BatteryMenu_FormatVoltTemp(voltTemp, h->s.voltageRaw, h->s.temperature, fahrenheit);
        else
            voltTemp[0] = 0;

        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "Battery Manager");
        u32 y = 30;
        for (u32 i = 0; i < BATTERY_HOME_SUMMARY_LINES; i++)
            y = batteryMenuLine(y, batteryToneColor(h->lines[i].tone), "%s", h->lines[i].text);
        y = batteryMenuLine(y, COLOR_WHITE, "");
        u32 nextColor = step == BATTERY_NEXT_ERROR ? COLOR_RED : COLOR_TITLE;
        y = batteryMenuLine(y, nextColor, "%s", next1);
        y = batteryMenuLine(y, nextColor, "%s", next2);
        y = batteryMenuLine(y, COLOR_WHITE, "");
        // Entries drawn like menuDraw: cursor at x = 10 in the title colour, title at x = 30 (46 chars)
        for (u32 i = 0; i < BATTERY_HOME_ENTRIES; i++)
        {
            Draw_DrawCharacter(10, y, COLOR_TITLE, i == selected ? '>' : ' ');
            Draw_DrawFormattedString(30, y, COLOR_WHITE, "%-*s", BATTERY_MENU_LINE_CHARS - 3, g_homeEntries[i].title);
            y += SPACING_Y;
        }
        batteryMenuPadBody(y);
        Draw_DrawString(10, SCREEN_BOT_HEIGHT - 20, COLOR_TITLE, "A: open  B: back");
        // Voltage (footer scale, raw x 0.02) and temperature, right-aligned in a fixed 11-char field like the footer
        Draw_DrawFormattedString(SCREEN_BOT_WIDTH - 10 - SPACING_X * 11, SCREEN_BOT_HEIGHT - 20, COLOR_WHITE, "%11s", voltTemp);
        Draw_FlushFramebuffer();
        Draw_Unlock();

        u32 pressed = waitInputWithTimeout(1000);
        if (pressed & KEY_B)
            break;
        else if (pressed & KEY_DOWN)
            selected = (selected + 1) % BATTERY_HOME_ENTRIES;
        else if (pressed & KEY_UP)
            selected = (selected + BATTERY_HOME_ENTRIES - 1) % BATTERY_HOME_ENTRIES;
        else if (pressed & KEY_A)
        {
            batteryMenuClear();
            g_homeEntries[selected].open();
            batteryMenuClear();
        }
    }
}
