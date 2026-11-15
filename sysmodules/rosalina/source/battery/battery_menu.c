/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: Rosalina submenu (Status, recording, sessions, curve, graph, LED control).
*/

#include <3ds.h>
#include <stdarg.h>
#include "menu.h"
#include "draw.h"
#include "fmt.h"
#include "memory.h"
#include "battery/battery_menu.h"
#include "battery/battery_mcu.h"
#include "battery/battery_recorder.h"
#include "battery/battery_sessions.h"
#include "battery/battery_curve.h"
#include "battery/battery_graph.h"
#include "battery/battery_led.h"
#include "battery/battery_ptm_patch.h"

#define BATTERY_MENU_LINE_CHARS     49 // (320 - 2 * 10) / 6 - 1
#define BATTERY_MENU_LIST_ROWS      15

Menu batteryMenu = {
    "Battery Manager",
    {
        { "Status", METHOD, .method = &BatteryMenu_Status },
        { "Start recording", METHOD, .method = &BatteryMenu_StartRecording },
        { "Stop recording", METHOD, .method = &BatteryMenu_StopRecording },
        { "Sessions", METHOD, .method = &BatteryMenu_Sessions },
        { "Build curve", METHOD, .method = &BatteryMenu_BuildCurve },
        { "Show graph", METHOD, .method = &BatteryMenu_ShowGraph },
        { "LED override: OFF", METHOD, .method = &BatteryMenu_LedOverride },
        { "Sleep LED: blue(5)", METHOD, .method = &BatteryMenu_SleepLed },
        { "Thresholds", METHOD, .method = &BatteryMenu_Thresholds },
        { "LED test", METHOD, .method = &BatteryMenu_LedTest },
        {},
    }
};

#define BATTERY_MENU_LED_OVERRIDE_ITEM  6
#define BATTERY_MENU_SLEEP_LED_ITEM     7

void BatteryMenu_UpdateLedTitle(bool on)
{
    batteryMenu.items[BATTERY_MENU_LED_OVERRIDE_ITEM].title = on ? "LED override: ON" : "LED override: OFF";
}

void BatteryMenu_UpdateSleepLedTitle(u8 sleepMode)
{
    const char *title;
    switch (sleepMode)
    {
        case BATTERY_LED_MODE_BLUE_FORCED:  title = "Sleep LED: blue(1)"; break;
        case BATTERY_LED_MODE_SLEEP:        title = "Sleep LED: anim(2)"; break;
        case BATTERY_LED_SLEEP_SYSTEM:      title = "Sleep LED: system"; break;
        default:                            title = "Sleep LED: blue(5)"; break;
    }
    batteryMenu.items[BATTERY_MENU_SLEEP_LED_ITEM].title = title;
}

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

void BatteryMenu_Status(void)
{
    batteryMenuClear();

    do
    {
        BatteryMcuSample s;
        BatteryRecorderStatus st;
        BatteryLedStatus led;
        BatteryPtmPatchStatus ptm;
        char dur[16], num[16], num2[16];

        Result res = BatteryMcu_ReadSample(&s);
        BatteryRecorder_GetStatus(&st);
        BatteryLed_GetStatus(&led);
        BatteryPtmPatch_GetStatus(&ptm);

        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "Battery Manager - Status");
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
            y = batteryMenuLine(y, COLOR_WHITE, "Temp: %lu\xF8""C  Adapter: %s  Charging: %s", (u32)s.temperature,
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
        y = batteryMenuLine(y, led.cfg.override ? COLOR_GREEN : COLOR_WHITE, "LED override:   %s  (%s%s)",
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
        // ptm's sleep LED byte (section 14): "05 (active)", "02 (stock)", "off: mismatch", ...
        if (ptm.state == BATTERY_PTM_STATE_OK)
            y = batteryMenuLine(y, COLOR_WHITE, "ptm sleep patch: %02lx (%s), writes %lu", (u32)ptm.current,
                                BatteryPtmPatch_StateName(ptm.state, ptm.current), ptm.writes);
        else if (ptm.state == BATTERY_PTM_STATE_NO_PTM || ptm.state == BATTERY_PTM_STATE_MAP_ERROR)
            y = batteryMenuLine(y, COLOR_RED, "ptm sleep patch: %s 0x%08lx", BatteryPtmPatch_StateName(ptm.state, ptm.current),
                                (u32)ptm.lastRes);
        else if (ptm.state == BATTERY_PTM_STATE_OFF_TITLE)
            y = batteryMenuLine(y, COLOR_RED, "ptm sleep patch: off: title id %08lx%08lx", (u32)(ptm.titleId >> 32), (u32)ptm.titleId);
        else
            y = batteryMenuLine(y, ptm.state == BATTERY_PTM_STATE_UNCHECKED ? COLOR_WHITE : COLOR_RED, "ptm sleep patch: %s, writes %lu",
                                BatteryPtmPatch_StateName(ptm.state, ptm.current), ptm.writes);
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

        y += SPACING_Y / 2;
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

void BatteryMenu_StartRecording(void)
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

void BatteryMenu_StopRecording(void)
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

void BatteryMenu_Sessions(void)
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
            for (u32 k = 0; k < r->rejected && n < 44; k++)
                n += sprintf(line + n, " #%04lu", (u32)r->rejectedNumbers[k]);
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

void BatteryMenu_BuildCurve(void)
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

        Draw_DrawString(10, SCREEN_BOT_HEIGHT - 20, COLOR_TITLE, "A: toggle  X/START: build  B: back");
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

// ---- Show graph ----

void BatteryMenu_ShowGraph(void)
{
    bool zoom = false;
    const BatteryCurveFile *curve = BatteryCurve_Snapshot();

    do
    {
        Draw_Lock();
        Draw_ClearFramebuffer();
        if (curve != NULL)
            Draw_DrawFormattedString(10, 10, COLOR_TITLE, "Battery curve, %lu session(s)%s",
                                     (u32)curve->numSessions, zoom ? ", MCU 0-10%" : "");
        else
            Draw_DrawString(10, 10, COLOR_TITLE, "Battery curve");
        BatteryGraph_Draw(curve, zoom);
        Draw_DrawString(10, SCREEN_BOT_HEIGHT - 18, COLOR_TITLE, zoom ? "Y: full view  B: back" : "Y: zoom 0-10%  B: back");
        Draw_DrawString(SCREEN_BOT_WIDTH - 4 - 19 * SPACING_X, SCREEN_BOT_HEIGHT - 18, COLOR_WHITE, "x: MCU %, y: real %");
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

// ---- LED control ----

void BatteryMenu_LedOverride(void)
{
    BatteryLedConfig cfg;
    BatteryLed_EnsureConfigLoaded();
    BatteryLed_GetConfig(&cfg);
    cfg.override = !cfg.override;
    Result res = BatteryLed_SetConfig(&cfg);
    BatteryMenu_UpdateLedTitle(cfg.override);
    bool haveCurve = BatteryCurve_IsLoaded();

    batteryMenuClear();
    do
    {
        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "Battery Manager - LED override");
        u32 y = 30;
        if (cfg.override)
        {
            y = batteryMenuLine(y, COLOR_GREEN, "LED override is now ON.");
            y += SPACING_Y;
            y = batteryMenuLine(y, COLOR_WHITE, "Corrected %% above %lu%%: blue (mode %lu)", (u32)cfg.redPct, (u32)cfg.blueMode);
            y = batteryMenuLine(y, COLOR_WHITE, "%lu%% or less: solid red", (u32)cfg.redPct);
            y = batteryMenuLine(y, COLOR_WHITE, "%lu%% or less: blinking red", (u32)cfg.blinkPct);
            y = batteryMenuLine(y, COLOR_WHITE, "Charging: only gets better. Lid closed:");
            y = batteryMenuLine(y, COLOR_WHITE, "Sleep LED %s if blue. Power off: mode 3.", BatteryLedLogic_SleepSettingName(cfg.sleepMode));
            if (!haveCurve)
            {
                y += SPACING_Y;
                y = batteryMenuLine(y, COLOR_RED, "No curve yet: the LED stays automatic");
                y = batteryMenuLine(y, COLOR_RED, "until you build one.");
            }
        }
        else
        {
            y = batteryMenuLine(y, COLOR_WHITE, "LED override is now OFF.");
            y += SPACING_Y;
            y = batteryMenuLine(y, COLOR_WHITE, "The power LED is back to automatic");
            y = batteryMenuLine(y, COLOR_WHITE, "(the MCU decides).");
        }
        if (R_FAILED(res))
        {
            y += SPACING_Y;
            batteryMenuLine(y, COLOR_RED, "Could not save config.ini (0x%08lx).", (u32)res);
        }
        Draw_DrawString(10, SCREEN_BOT_HEIGHT - 20, COLOR_TITLE, "Press B to go back.");
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while (!(waitInput() & KEY_B) && !menuShouldExit);
}

void BatteryMenu_SleepLed(void)
{
    BatteryLedConfig cfg;
    BatteryLed_EnsureConfigLoaded();
    BatteryLed_GetConfig(&cfg);
    cfg.sleepMode = BatteryLedLogic_NextSleepSetting(cfg.sleepMode);
    Result res = BatteryLed_SetConfig(&cfg);
    BatteryMenu_UpdateSleepLedTitle(cfg.sleepMode);

    batteryMenuClear();
    do
    {
        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "Battery Manager - Sleep LED");
        u32 y = 30;
        y = batteryMenuLine(y, COLOR_GREEN, "Sleep LED is now: %s", BatteryLedLogic_SleepSettingName(cfg.sleepMode));
        y += SPACING_Y;
        y = batteryMenuLine(y, COLOR_WHITE, "Written when the lid closes while the LED");
        y = batteryMenuLine(y, COLOR_WHITE, "is blue (override on). Red / blinking red");
        y = batteryMenuLine(y, COLOR_WHITE, "stay as they are.");
        y += SPACING_Y;
        y = batteryMenuLine(y, cfg.sleepMode == BATTERY_LED_MODE_BLUE ? COLOR_GREEN : COLOR_WHITE, "blue(5): blue on, red off (default)");
        y = batteryMenuLine(y, cfg.sleepMode == BATTERY_LED_MODE_BLUE_FORCED ? COLOR_GREEN : COLOR_WHITE, "blue(1): forced default blue");
        y = batteryMenuLine(y, cfg.sleepMode == BATTERY_LED_MODE_SLEEP ? COLOR_GREEN : COLOR_WHITE, "anim(2): MCU sleep animation (may be red)");
        y = batteryMenuLine(y, cfg.sleepMode == BATTERY_LED_SLEEP_SYSTEM ? COLOR_GREEN : COLOR_WHITE, "system:  write nothing, MCU decides");
        y += SPACING_Y;
        y = batteryMenuLine(y, COLOR_WHITE, "Select this entry again for the next one.");
        y = batteryMenuLine(y, COLOR_WHITE, "Status shows the \"Sleep:\" and \"ptm\" lines.");
        if (R_FAILED(res))
        {
            y += SPACING_Y;
            batteryMenuLine(y, COLOR_RED, "Could not save config.ini (0x%08lx).", (u32)res);
        }
        Draw_DrawString(10, SCREEN_BOT_HEIGHT - 20, COLOR_TITLE, "Press B to go back.");
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while (!(waitInput() & KEY_B) && !menuShouldExit);
}

void BatteryMenu_Thresholds(void)
{
    BatteryLedConfig cfg, orig;
    u32 row = 0;
    Result res = 0;

    BatteryLed_EnsureConfigLoaded();
    BatteryLed_GetConfig(&cfg);
    orig = cfg;

    batteryMenuClear();
    do
    {
        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "Battery Manager - Thresholds");
        u32 y = 30;
        y = batteryMenuLine(y, COLOR_WHITE, "%c Solid red at:     %2lu%% corrected", row == 0 ? '>' : ' ', (u32)cfg.redPct);
        y = batteryMenuLine(y, COLOR_WHITE, "%c Blinking red at:  %2lu%% corrected", row == 1 ? '>' : ' ', (u32)cfg.blinkPct);
        y = batteryMenuLine(y, COLOR_WHITE, "%c Blue mode:        %lu (%s)", row == 2 ? '>' : ' ', (u32)cfg.blueMode,
                            cfg.blueMode == BATTERY_LED_MODE_BLUE ? "red off, blue on" : "forced default blue");
        y += SPACING_Y;
        y = batteryMenuLine(y, COLOR_WHITE, "Blinking must be below solid red.");
        y = batteryMenuLine(y, COLOR_WHITE, "Within one discharge the LED only gets");
        y = batteryMenuLine(y, COLOR_WHITE, "worse (blue -> red -> blinking), while");
        y = batteryMenuLine(y, COLOR_WHITE, "charging only better. After a change it");
        y = batteryMenuLine(y, COLOR_WHITE, "may get better if the corrected %% is more");
        y = batteryMenuLine(y, COLOR_WHITE, "than 0.5 points above the new threshold.");
        Draw_DrawString(10, SCREEN_BOT_HEIGHT - 20, COLOR_TITLE, "Up/Down: select  Left/Right: change  B: save");
        Draw_FlushFramebuffer();
        Draw_Unlock();

        u32 pressed = waitInput();
        if (pressed & KEY_B)
            break;
        else if (pressed & KEY_DOWN)
            row = (row + 1) % 3;
        else if (pressed & KEY_UP)
            row = (row + 2) % 3;
        else if (pressed & (KEY_LEFT | KEY_RIGHT))
        {
            bool inc = (pressed & KEY_RIGHT) != 0;
            if (row == 0)
            {
                if (inc && cfg.redPct < BATTERY_LED_MAX_RED_PCT)
                    cfg.redPct++;
                else if (!inc && cfg.redPct > cfg.blinkPct + 1)
                    cfg.redPct--;
            }
            else if (row == 1)
            {
                if (inc && cfg.blinkPct + 1 < cfg.redPct)
                    cfg.blinkPct++;
                else if (!inc && cfg.blinkPct > 0)
                    cfg.blinkPct--;
            }
            else
                cfg.blueMode = cfg.blueMode == BATTERY_LED_MODE_BLUE ? BATTERY_LED_MODE_BLUE_FORCED : BATTERY_LED_MODE_BLUE;
        }
    }
    while (!menuShouldExit);

    if (cfg.redPct != orig.redPct || cfg.blinkPct != orig.blinkPct || cfg.blueMode != orig.blueMode)
    {
        res = BatteryLed_SetConfig(&cfg);
        if (R_FAILED(res))
        {
            char line[48];
            sprintf(line, "Error 0x%08lx.", (u32)res);
            batteryMenuMessage("Battery Manager - Thresholds", "Could not save config.ini.", line);
        }
    }
    batteryMenuClear();
}

void BatteryMenu_LedTest(void)
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
