/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: Rosalina submenu (settings, learning: recording / sessions / curve / graph, advanced: diagnostics,
*   LED test, blue LED type, limits). Settings only go through BatteryLed_SetConfig; nothing here writes to the MCU or ptm.
*/

#include <3ds.h>
#include <stdarg.h>
#include "menu.h"
#include "draw.h"
#include "fmt.h"
#include "memory.h"
#include "luma_config.h"
#include "battery/battery_menu.h"
#include "battery/battery_mcu.h"
#include "battery/battery_recorder.h"
#include "battery/battery_sessions.h"
#include "battery/battery_curve.h"
#include "battery/battery_graph.h"
#include "battery/battery_led.h"
#include "battery/battery_ptm_patch.h"
#include "battery/battery_ptm_bars.h"

#define BATTERY_MENU_LINE_CHARS     49 // (320 - 2 * 10) / 6 - 1
#define BATTERY_MENU_LIST_ROWS      15

// Menu tree (docs/battery-manager.md, section 4.2). Settings are edited on one screen and saved when it is left, so no
// item title depends on config.ini. The only dynamic title is Start / Stop recording, refreshed by its visibility callback
// (menuDraw calls it on every redraw, about once a second) into a static buffer padded to a fixed length, because the
// menu redraws without clearing.

static void batteryMenuSettings(void);
static void batteryMenuRecording(void);
static void batteryMenuSessions(void);
static void batteryMenuBuildCurve(void);
static void batteryMenuGraph(void);
static void batteryMenuDiagnostics(void);
static void batteryMenuLedTest(void);
static void batteryMenuBlueLedType(void);
static void batteryMenuAbout(void);
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
        { "Limits and about", METHOD, .method = &batteryMenuAbout },
        {},
    }
};

Menu batteryMenu = {
    "Battery Manager",
    {
        { "Settings", METHOD, .method = &batteryMenuSettings },
        { "Learning...", MENU, .menu = &batteryLearningMenu },
        { "Advanced...", MENU, .menu = &batteryAdvancedMenu },
        {},
    }
};

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

static void batteryFormatDuration(char *out, u32 ms)
{
    u32 s = ms / 1000;
    sprintf(out, "%lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
}

// Simple message screen, B to go back
static void batteryMenuMessage(const char *title, const char *line1, const char *line2)
{
    batteryMenuClear();
    do
    {
        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, title);
        u32 y = 30;
        if (line1 != NULL)
            y = batteryMenuLine(y, COLOR_WHITE, "%s", line1);
        if (line2 != NULL)
            batteryMenuLine(y, COLOR_WHITE, "%s", line2);
        Draw_DrawString(10, SCREEN_BOT_HEIGHT - 20, COLOR_TITLE, "Press B to go back.");
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while (!(waitInput() & KEY_B) && !menuShouldExit);
}

static void batteryFormatX10(char *out, float v)
{
    u32 x10 = (u32)(v * 10.0f + 0.5f);
    sprintf(out, "%lu.%lu", x10 / 10, x10 % 10);
}

// Advanced > Diagnostics: raw gauge, LED, ptm patch and recorder values, refreshed every second
static void batteryMenuDiagnostics(void)
{
    batteryMenuClear();

    do
    {
        BatteryMcuSample s;
        BatteryRecorderStatus st;
        BatteryLedStatus led;
        BatteryPtmPatchStatus ptm;
        BatteryBarsStatus bars;
        char dur[16], num[16], num2[16];

        Result res = BatteryMcu_ReadSample(&s);
        BatteryRecorder_GetStatus(&st);
        BatteryLed_GetStatus(&led);
        BatteryPtmPatch_GetStatus(&ptm);
        BatteryBars_GetStatus(&bars);

        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "Battery Manager - Diagnostics");
        u32 y = 30;

        if (R_SUCCEEDED(res))
        {
            u32 p = BatteryMcu_PercentX10(s.pctInt, s.pctFrac);
            u32 mv = BatteryMcu_VoltageMv(s.voltageRaw);
            bool adapter = (s.flags & BATTERY_MCU_FLAG_ADAPTER) != 0;
            bool charging = (s.flags & BATTERY_MCU_FLAG_CHARGING) != 0;

            y = batteryMenuLine(y, COLOR_WHITE, "MCU charge:     %lu.%lu%%", p / 10, p % 10);
            // Same value the LED uses (median voltage); instantaneous voltage before the LED thread's first poll
            float c = led.haveData ? led.corrected : BatteryCurve_Corrected(s.pctInt + s.pctFrac / 256.0f, (float)s.voltageRaw);
            if (c >= 0.0f)
            {
                batteryFormatX10(num, c);
                batteryFormatX10(num2, led.haveData ? led.voltSmoothed : (float)s.voltageRaw);
                y = batteryMenuLine(y, COLOR_WHITE, "Corrected:      %s%%  (volt median %s)", num, num2);
            }
            else
                y = batteryMenuLine(y, COLOR_WHITE, "Corrected:      no curve yet");
            y = batteryMenuLine(y, COLOR_WHITE, "Voltage:        %lu.%03luV (raw %lu)", mv / 1000, mv % 1000, (u32)s.voltageRaw);
            // Same unit and rounding as the Rosalina footer (menu.c)
            bool fahrenheit = (lumaMcuConfig.flags & MCUCFG_FLAG_TEMPERATURE_IN_FAHRENHEIT) != 0;
            u32 temp = fahrenheit ? (9 * (u32)s.temperature + 2) / 5 + 32 : (u32)s.temperature;
            y = batteryMenuLine(y, COLOR_WHITE, "Temp: %lu\xF8%c  Adapter: %s  Charging: %s", temp, fahrenheit ? 'F' : 'C',
                                adapter ? "yes" : "no", charging ? "yes" : "no");
            if (s.infoValid)
                y = batteryMenuLine(y, COLOR_WHITE, "Vendor: %lu  RCOMP: 0x%02lx  MGIC: %lu.%lu",
                                    (u32)s.info[2], (u32)s.info[5], (u32)s.info[3], (u32)s.info[4]);
            else
                y = batteryMenuLine(y, COLOR_WHITE, "Info table (0x7F) unavailable");
        }
        else
        {
            y = batteryMenuLine(y, COLOR_RED, "MCU read failed (0x%08lx).", (u32)res);
            for (u32 i = 0; i < 4; i++)
                y = batteryMenuLine(y, COLOR_WHITE, "");
        }

        // LED control
        if (led.reason == BATTERY_LED_REASON_OFF)
            y = batteryMenuLine(y, COLOR_WHITE, "LED control:    OFF%s", led.fastPolling ? "  (fast poll)" : "");
        else
            y = batteryMenuLine(y, led.cfg.override ? COLOR_GREEN : COLOR_WHITE, "LED control:    %s  (%s%s)",
                                led.cfg.override ? "ON" : "OFF", BatteryLedLogic_ReasonName(led.reason),
                                led.fastPolling ? ", fast poll" : "");
        if (R_SUCCEEDED(res) && s.ledModeValid)
            y = batteryMenuLine(y, COLOR_WHITE, "LED desired:    %-9s actual(0x29): %lu",
                                BatteryLedLogic_ModeName(led.desired), (u32)s.ledMode);
        else
            y = batteryMenuLine(y, COLOR_WHITE, "LED desired:    %-9s actual(0x29): ?", BatteryLedLogic_ModeName(led.desired));
        if (R_SUCCEEDED(res) && s.infoValid)
            y = batteryMenuLine(y, COLOR_WHITE, "Red LED: %-3s  Blue: %-3lu  Written: %s",
                                s.info[0x0A] ? "on" : "off", (u32)s.info[0x0B], BatteryLedLogic_ModeName(led.lastWritten));
        else
            y = batteryMenuLine(y, COLOR_WHITE, "Red LED: ?    Blue: ?    Written: %s", BatteryLedLogic_ModeName(led.lastWritten));
        u32 sessionReasserts = st.recording ? led.reasserts - st.reassertBase : 0;
        if (led.haveMismatch)
        {
            batteryFormatX10(num, led.lastMismatchPct);
            y = batteryMenuLine(y, COLOR_WHITE, "Reasserts:      %lu boot/%lu session (at %s%%)", led.reasserts, sessionReasserts, num);
        }
        else
            y = batteryMenuLine(y, COLOR_WHITE, "Reasserts:      %lu boot/%lu session", led.reasserts, sessionReasserts);
        if (led.sleepCount != 0)
        {
            // What the sleep hook wrote, and 0x29 read on wake before anything was written
            char w[8], r[8];
            batteryFormatSleepMode(w, led.sleepWrote, false, true);
            batteryFormatSleepMode(r, led.wakeRead, led.wakePending, false);
            y = batteryMenuLine(y, COLOR_WHITE, "Sleep: wrote %s / wake read %s (n=%lu)", w, r, led.sleepCount);
        }
        else
            y = batteryMenuLine(y, COLOR_WHITE, "Sleep: setting %s, no sleep yet", BatteryLedLogic_SleepSettingName(led.cfg.sleepMode));
        // ptm's sleep / wake LED bytes (section 14): "sleep 05 wake 05 (active)", "sleep 02 wake 00 (stock)", "off: ..."
        const char *ptmName = BatteryPtmPatch_StateName(ptm.state, ptm.sleepCur, ptm.wakeCur);
        if (ptm.state == BATTERY_PTM_STATE_OK)
            y = batteryMenuLine(y, COLOR_WHITE, "ptm patch: sleep %02lx wake %02lx (%s), writes %lu", (u32)ptm.sleepCur,
                                (u32)ptm.wakeCur, ptmName, ptm.writes);
        else if (ptm.state == BATTERY_PTM_STATE_NO_PTM || ptm.state == BATTERY_PTM_STATE_MAP_ERROR)
            y = batteryMenuLine(y, COLOR_RED, "ptm patch: %s 0x%08lx", ptmName, (u32)ptm.lastRes);
        else if (ptm.state == BATTERY_PTM_STATE_OFF_TITLE)
            y = batteryMenuLine(y, COLOR_RED, "ptm patch: off: title id %08lx%08lx", (u32)(ptm.titleId >> 32), (u32)ptm.titleId);
        else if (ptm.state == BATTERY_PTM_STATE_OFF_MISMATCH)
            y = batteryMenuLine(y, COLOR_RED, "ptm patch: off: mismatch (%s), both off",
                                ptm.mismatch == (BATTERY_PTM_MISMATCH_SLEEP | BATTERY_PTM_MISMATCH_WAKE) ? "sleep+wake" :
                                ptm.mismatch == BATTERY_PTM_MISMATCH_WAKE ? "wake" : "sleep");
        else if (ptm.state == BATTERY_PTM_STATE_WRITE_ERROR)
            y = batteryMenuLine(y, COLOR_RED, "ptm patch: write error (want %02lx/%02lx), writes %lu", (u32)ptm.sleepWant,
                                (u32)ptm.wakeWant, ptm.writes);
        else
            y = batteryMenuLine(y, ptm.state == BATTERY_PTM_STATE_UNCHECKED ? COLOR_WHITE : COLOR_RED, "ptm patch: %s, writes %lu",
                                ptmName, ptm.writes);
        // ptm battery bars (section 15): "fed L=3 (shared 3)", "fed L=3 (held), shared 3" (charging), "stock (off), shared 5, w 6",
        // "off: ...", each at most 49 chars (BATTERY_MENU_LINE_CHARS) with up to 4-digit write counts. "shared" is
        // ptm's level in the shared page (0x1FF81085 bits 2..4, read only), updated at ptm's own timer tick
        u32 shared = (OS_SharedConfig->led_battery >> 2) & 7;
        if (bars.state == BATTERY_BARS_STATE_OK && bars.cmpCur == BATTERY_BARS_CMP_FED && led.barsReason == BATTERY_BARS_REASON_CHARGING)
            y = batteryMenuLine(y, COLOR_WHITE, "bars patch: fed L=%lu (held), shared %lu, writes %lu", (u32)bars.movsCur, shared, bars.writes);
        else if (bars.state == BATTERY_BARS_STATE_OK && bars.cmpCur == BATTERY_BARS_CMP_FED)
            y = batteryMenuLine(y, COLOR_WHITE, "bars patch: fed L=%lu (shared %lu), writes %lu", (u32)bars.movsCur, shared, bars.writes);
        else if (bars.state == BATTERY_BARS_STATE_OK)
            y = batteryMenuLine(y, COLOR_WHITE, "bars patch: stock (%s), shared %lu, w %lu",
                                BatteryBars_ReasonName(led.barsReason), shared, bars.writes);
        else if (bars.state == BATTERY_BARS_STATE_UNCHECKED)
            y = batteryMenuLine(y, COLOR_WHITE, "bars patch: not checked yet, shared %lu", shared);
        else if (bars.state == BATTERY_BARS_STATE_NO_PTM || bars.state == BATTERY_BARS_STATE_MAP_ERROR)
            y = batteryMenuLine(y, COLOR_RED, "bars patch: %s 0x%08lx", BatteryBars_StateName(bars.state), (u32)bars.lastRes);
        else if (bars.state == BATTERY_BARS_STATE_OFF_TITLE)
            y = batteryMenuLine(y, COLOR_RED, "bars patch: off: title id %08lx%08lx", (u32)(bars.titleId >> 32), (u32)bars.titleId);
        else if (bars.state == BATTERY_BARS_STATE_OFF_MISMATCH)
            y = batteryMenuLine(y, COLOR_RED, "bars patch: off: mismatch (%s)",
                                bars.mismatch == (BATTERY_BARS_MISMATCH_A | BATTERY_BARS_MISMATCH_B) ? "A+B" :
                                bars.mismatch == BATTERY_BARS_MISMATCH_B ? "B" : "A");
        else if (bars.state == BATTERY_BARS_STATE_WRITE_ERROR)
            y = batteryMenuLine(y, COLOR_RED, "bars patch: write error (want L=%lu), writes %lu", (u32)bars.levelWant, bars.writes);
        else
            y = batteryMenuLine(y, COLOR_RED, "bars patch: %s", BatteryBars_StateName(bars.state));
        if (led.error == BATTERY_LED_ERR_WRITE_FAILED)
            y = batteryMenuLine(y, COLOR_RED, "LED: writes failed (0x%08lx), automatic", (u32)led.lastWriteRes);
        else if (led.error == BATTERY_LED_ERR_MCU_OVERRIDES)
            y = batteryMenuLine(y, COLOR_RED, "LED: MCU keeps overriding, automatic");
        else if (led.writeFailures != 0)
            y = batteryMenuLine(y, COLOR_RED, "LED write failures: %lu (0x%08lx)", led.writeFailures, (u32)led.lastWriteRes);
        else if (led.firstWriteMs != 0 || led.firstDataMs != 0)
        {
            // Boot timing (s since the tick counter started, ~power-on): Rosalina, SD usable, first write
            char t0[16], t1[16], t2[16];
            batteryFormatX10(t0, led.startMs / 1000.0f);
            batteryFormatX10(t1, led.sdReadyMs / 1000.0f);
            if (led.firstWriteMs != 0)
                batteryFormatX10(t2, led.firstWriteMs / 1000.0f);
            else
                memcpy(t2, "-", 2);
            y = batteryMenuLine(y, COLOR_WHITE, "Boot s: Rosalina %s SD %s 1st LED %s", t0, t1, t2);
        }
        else
            y = batteryMenuLine(y, COLOR_WHITE, "");

        batteryFormatDuration(dur, st.elapsedMs);
        if (st.recording)
        {
            y = batteryMenuLine(y, COLOR_WHITE, "Recorder:       recording #%04lu", (u32)st.number);
            y = batteryMenuLine(y, COLOR_WHITE, "Elapsed:        %s", dur);
            y = batteryMenuLine(y, COLOR_WHITE, "Samples:        %lu (%lu not on SD yet)", st.samples, st.bufferedLines);
        }
        else
        {
            y = batteryMenuLine(y, COLOR_WHITE, "Recorder:       idle");
            if (st.number != 0 && st.recoveredNumber == st.number)
                y = batteryMenuLine(y, COLOR_WHITE, "Last:           #%04lu (cut off, closed at boot)", (u32)st.number);
            else if (st.number != 0)
                y = batteryMenuLine(y, COLOR_WHITE, "Last:           #%04lu, %s, %lu samples", (u32)st.number, dur, st.samples);
            else
                y = batteryMenuLine(y, COLOR_WHITE, "");
            y = batteryMenuLine(y, COLOR_WHITE, "");
        }

        if (st.sleepGap)
            y = batteryMenuLine(y, COLOR_RED, "Sleep gap seen: session invalid for curves");
        else if (st.readErrors != 0 || st.writeErrors != 0)
            y = batteryMenuLine(y, COLOR_RED, "Errors: %lu read, %lu write (0x%08lx)", st.readErrors, st.writeErrors, (u32)st.lastWriteRes);
        else
            y = batteryMenuLine(y, COLOR_WHITE, "");

        Draw_DrawString(10, SCREEN_BOT_HEIGHT - 20, COLOR_TITLE, "Press B to go back.");
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while (!(waitInputWithTimeout(1000) & KEY_B) && !menuShouldExit);
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
        batteryFormatDuration(dur, st.elapsedMs);
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
    char line[64];
    u16 number = 0;
    Result res = 0;

    BatteryRecorderError err = BatteryRecorder_Start(&number, &res);
    if (err == BATTERY_REC_OK)
    {
        sprintf(line, "Recording session #%04lu.", (u32)number);
        batteryMenuMessage("Battery Manager - Start recording", line,
                           "Don't sleep or charge until it powers off.");
    }
    else
    {
        if (err == BATTERY_REC_ALREADY_RECORDING)
            sprintf(line, "Session #%04lu is being recorded.", (u32)number);
        else if (R_FAILED(res))
            sprintf(line, "Error 0x%08lx.", (u32)res);
        else
            line[0] = 0;
        batteryMenuMessage("Battery Manager - Start recording", BatteryRecorder_ErrorString(err), line);
    }
}

static void batteryStopRecording(void)
{
    char line[64];
    u16 number = 0;

    BatteryRecorderError err = BatteryRecorder_Stop(&number);
    if (err == BATTERY_REC_OK)
    {
        sprintf(line, "Session #%04lu stopped.", (u32)number);
        batteryMenuMessage("Battery Manager - Stop recording", line, NULL);
    }
    else
        batteryMenuMessage("Battery Manager - Stop recording", BatteryRecorder_ErrorString(err), NULL);
}

// Learning > Start recording / Stop recording #NNNN (one entry, the title shows which)
static void batteryMenuRecording(void)
{
    BatteryRecorderStatus st;
    BatteryRecorder_GetStatus(&st);
    if (st.recording)
        batteryStopRecording();
    else
        batteryStartRecording();
}

// ---- Sessions ----

static BatterySessionInfo g_sessions[BATTERY_MAX_LISTED_SESSIONS];
static u32 g_numSessions;

static void batterySessionsRescan(void)
{
    batteryMenuClear();
    Draw_Lock();
    Draw_DrawString(10, 10, COLOR_TITLE, "Battery Manager - Sessions");
    Draw_DrawString(10, 30, COLOR_WHITE, "Reading sessions...");
    Draw_FlushFramebuffer();
    Draw_Unlock();

    if (R_FAILED(BatterySessions_Scan(g_sessions, BATTERY_MAX_LISTED_SESSIONS, &g_numSessions)))
        g_numSessions = 0;
    batteryMenuClear();
}

static const char *batterySessionLabel(const BatterySessionInfo *info, const BatteryRecorderStatus *st)
{
    if (st->recording && st->number == info->number)
        return "recording";
    return BatterySessions_ClassName(info->cls);
}

static void batterySessionDetails(const BatterySessionInfo *info, const BatteryRecorderStatus *st)
{
    char dur[16];
    batteryFormatDuration(dur, info->durationMs);

    batteryMenuClear();
    do
    {
        Draw_Lock();
        Draw_DrawFormattedString(10, 10, COLOR_TITLE, "Battery Manager - Session #%04lu", (u32)info->number);
        u32 y = 30;
        y = batteryMenuLine(y, COLOR_WHITE, "Class:       %s", batterySessionLabel(info, st));
        y = batteryMenuLine(y, COLOR_WHITE, "End reason:  %s", BatterySessions_EndReasonName(info->endReason));
        y = batteryMenuLine(y, COLOR_WHITE, "Sleep gap:   %s", info->hasSleepGap ? "yes" : "no");
        y = batteryMenuLine(y, COLOR_WHITE, "Duration:    %s", dur);
        y = batteryMenuLine(y, COLOR_WHITE, "Samples:     %lu", info->samples);
        y = batteryMenuLine(y, COLOR_WHITE, "Start:       %lu.%lu%%", (u32)info->startPctX10 / 10, (u32)info->startPctX10 % 10);
        y = batteryMenuLine(y, COLOR_WHITE, "End:         %lu.%lu%%", (u32)info->endPctX10 / 10, (u32)info->endPctX10 % 10);
        y = batteryMenuLine(y, COLOR_WHITE, "File size:   %lu bytes", info->sizeBytes);
        y += SPACING_Y;
        y = batteryMenuLine(y, COLOR_WHITE, "complete = starts >= 95%%, ends by power-off,");
        batteryMenuLine(y, COLOR_WHITE, "no sleep gap.");
        Draw_DrawString(10, SCREEN_BOT_HEIGHT - 20, COLOR_TITLE, "Press B to go back.");
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while (!(waitInput() & KEY_B) && !menuShouldExit);
    batteryMenuClear();
}

// Returns true if the session was deleted
static bool batterySessionDelete(const BatterySessionInfo *info)
{
    BatteryRecorderStatus st;
    BatteryRecorder_GetStatus(&st);
    if (st.recording && st.number == info->number)
    {
        batteryMenuMessage("Battery Manager - Delete", "This session is being recorded.", "Stop recording first.");
        batteryMenuClear();
        return false;
    }

    batteryMenuClear();
    u32 pressed = 0;
    do
    {
        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "Battery Manager - Delete");
        u32 y = batteryMenuLine(30, COLOR_WHITE, "Delete session #%04lu?", (u32)info->number);
        batteryMenuLine(y + SPACING_Y, COLOR_WHITE, "A: delete    B: cancel");
        Draw_FlushFramebuffer();
        Draw_Unlock();
        pressed = waitInput();
    }
    while (!(pressed & (KEY_A | KEY_B)) && !menuShouldExit);

    if (!(pressed & KEY_A))
    {
        batteryMenuClear();
        return false;
    }

    Result res = BatterySessions_Delete(info->number);
    if (R_FAILED(res))
    {
        char line[32];
        sprintf(line, "Error 0x%08lx.", (u32)res);
        batteryMenuMessage("Battery Manager - Delete", "Delete failed.", line);
        batteryMenuClear();
        return false;
    }
    return true;
}

static void batteryMenuSessions(void)
{
    u32 selected = 0; // index in display order (newest first)
    u32 scroll = 0;

    batterySessionsRescan();

    do
    {
        BatteryRecorderStatus st;
        BatteryRecorder_GetStatus(&st);

        if (g_numSessions != 0 && selected >= g_numSessions)
            selected = g_numSessions - 1;
        if (selected < scroll)
            scroll = selected;
        else if (selected >= scroll + BATTERY_MENU_LIST_ROWS)
            scroll = selected - BATTERY_MENU_LIST_ROWS + 1;

        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "Battery Manager - Sessions");
        u32 y = 30;
        if (g_numSessions == 0)
            y = batteryMenuLine(y, COLOR_WHITE, "No sessions in " BATTERY_SESSIONS_DIR);

        for (u32 row = 0; row < BATTERY_MENU_LIST_ROWS; row++)
        {
            u32 i = scroll + row;
            if (i >= g_numSessions)
            {
                if (g_numSessions != 0)
                    y = batteryMenuLine(y, COLOR_WHITE, "");
                continue;
            }

            const BatterySessionInfo *info = &g_sessions[g_numSessions - 1 - i];
            u32 m = info->durationMs / 60000;
            y = batteryMenuLine(y, COLOR_WHITE, "%c %04lu %-10s %3lu:%02lu %3lu.%lu->%3lu.%lu%%",
                i == selected ? '>' : ' ', (u32)info->number, batterySessionLabel(info, &st),
                m / 60, m % 60,
                (u32)info->startPctX10 / 10, (u32)info->startPctX10 % 10,
                (u32)info->endPctX10 / 10, (u32)info->endPctX10 % 10);
        }

        Draw_DrawString(10, SCREEN_BOT_HEIGHT - 20, COLOR_TITLE, "A: details  Y: delete  B: back");
        Draw_FlushFramebuffer();
        Draw_Unlock();

        u32 pressed = waitInput();
        if (pressed & KEY_B)
            break;
        else if (g_numSessions == 0)
            continue;
        else if (pressed & KEY_DOWN)
            selected = (selected + 1) % g_numSessions;
        else if (pressed & KEY_UP)
            selected = (selected + g_numSessions - 1) % g_numSessions;
        else if (pressed & KEY_A)
            batterySessionDetails(&g_sessions[g_numSessions - 1 - selected], &st);
        else if (pressed & (KEY_X | KEY_Y))
        {
            if (batterySessionDelete(&g_sessions[g_numSessions - 1 - selected]))
                batterySessionsRescan();
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

    batterySessionsRescan(); // shows "Reading sessions..."
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

static void batteryBuildResult(bool ok, const BatteryCurveBuildResult *r)
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

            // MCU % -> real % at a few points
            static const u8 points[] = { 50, 25, 10, 5, 2, 1 };
            u32 r10[sizeof(points)];
            for (u32 k = 0; k < sizeof(points); k++)
                r10[k] = (u32)(BatteryCurve_Interp(f->mcuGridX10, f->mcuMean, BATTERY_CURVE_MCU_POINTS, points[k] * 10.0f) * 10.0f + 0.5f);
            y += SPACING_Y;
            y = batteryMenuLine(y, COLOR_WHITE, "Real %% at MCU 50/25/10%%: %lu.%lu / %lu.%lu / %lu.%lu",
                                r10[0] / 10, r10[0] % 10, r10[1] / 10, r10[1] % 10, r10[2] / 10, r10[2] % 10);
            y = batteryMenuLine(y, COLOR_WHITE, "Real %% at MCU 5/2/1%%:    %lu.%lu / %lu.%lu / %lu.%lu",
                                r10[3] / 10, r10[3] % 10, r10[4] / 10, r10[4] % 10, r10[5] / 10, r10[5] % 10);
        }
        else if (r->used == 0 && R_SUCCEEDED(r->res))
            y = batteryMenuLine(y, COLOR_RED, "No usable session: nothing written.");
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

static void batteryBuildRun(void)
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
            batteryMenuClear();
            return;
        }
        numbers[count++] = g_candidates[i].number;
    }

    if (count == 0)
    {
        batteryMenuMessage("Battery Manager - Build curve", "No session selected.", NULL);
        batteryMenuClear();
        return;
    }

    batteryMenuClear();
    bool ok = BatteryCurve_Build(numbers, count, batteryBuildProgress, &r);
    batteryBuildResult(ok, &r);
}

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
            y = batteryMenuLine(y, COLOR_WHITE, "No complete sessions yet.");
            y = batteryMenuLine(y, COLOR_WHITE, "complete = starts >= 95%%, ends by power-off,");
            y = batteryMenuLine(y, COLOR_WHITE, "no sleep gap.");
        }
        else
        {
            y = batteryMenuLine(y, COLOR_WHITE, "Complete sessions: %lu, selected: %lu (max 16)", g_numCandidates, numSelected);
            y = batteryMenuLine(y, COLOR_WHITE, "");
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
                "%c[%c] %04lu %3lu:%02lu end %3lu.%lu%% %s",
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
            batteryBuildRun();
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
        Result res = BatteryLed_SetConfig(&cfg);
        if (R_FAILED(res))
        {
            char line[48];
            sprintf(line, "Error 0x%08lx.", (u32)res);
            batteryMenuMessage(title, "Could not save config.ini.", line);
        }
        else if (!menuShouldExit)
        {
            Draw_Lock();
            Draw_DrawFormattedString(10, SCREEN_BOT_HEIGHT - 20, COLOR_GREEN, "%-*s", BATTERY_MENU_LINE_CHARS, "Saved.");
            Draw_FlushFramebuffer();
            Draw_Unlock();
            svcSleepThread(700 * 1000 * 1000LL);
        }
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
