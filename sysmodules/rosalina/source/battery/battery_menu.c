/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: Rosalina submenu (milestone 2: Status, Start/Stop recording, Sessions).
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

#define BATTERY_MENU_LINE_CHARS     49 // (320 - 2 * 10) / 6 - 1
#define BATTERY_MENU_LIST_ROWS      15

Menu batteryMenu = {
    "Battery Manager",
    {
        { "Status", METHOD, .method = &BatteryMenu_Status },
        { "Start recording", METHOD, .method = &BatteryMenu_StartRecording },
        { "Stop recording", METHOD, .method = &BatteryMenu_StopRecording },
        { "Sessions", METHOD, .method = &BatteryMenu_Sessions },
        {},
    }
};

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

void BatteryMenu_Status(void)
{
    batteryMenuClear();

    do
    {
        BatteryMcuSample s;
        BatteryRecorderStatus st;
        char dur[16];

        Result res = BatteryMcu_ReadSample(&s);
        BatteryRecorder_GetStatus(&st);

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
            y = batteryMenuLine(y, COLOR_WHITE, "Voltage:        %lu.%03luV (raw %lu)", mv / 1000, mv % 1000, (u32)s.voltageRaw);
            y = batteryMenuLine(y, COLOR_WHITE, "Temperature:    %lu\xF8""C", (u32)s.temperature);
            y = batteryMenuLine(y, COLOR_WHITE, "Adapter:        %-4s Charging: %s", adapter ? "yes" : "no", charging ? "yes" : "no");
            if (s.infoValid)
            {
                y = batteryMenuLine(y, COLOR_WHITE, "Red LED:        %s", s.info[0x0A] ? "on" : "off");
                y = batteryMenuLine(y, COLOR_WHITE, "Battery vendor: %-4lu RCOMP: 0x%02lx", (u32)s.info[2], (u32)s.info[5]);
                y = batteryMenuLine(y, COLOR_WHITE, "MGIC version:   %lu.%lu", (u32)s.info[3], (u32)s.info[4]);
            }
            else
            {
                y = batteryMenuLine(y, COLOR_WHITE, "Red LED:        ?");
                y = batteryMenuLine(y, COLOR_WHITE, "Info table (0x7F) unavailable");
                y = batteryMenuLine(y, COLOR_WHITE, "");
            }
        }
        else
        {
            y = batteryMenuLine(y, COLOR_RED, "MCU read failed (0x%08lx).", (u32)res);
            for (u32 i = 0; i < 6; i++)
                y = batteryMenuLine(y, COLOR_WHITE, "");
        }

        y += SPACING_Y;
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
        else
            y = batteryMenuLine(y, COLOR_WHITE, "");

        if (st.readErrors != 0 || st.writeErrors != 0)
            batteryMenuLine(y, COLOR_RED, "Errors: %lu read, %lu write (0x%08lx)", st.readErrors, st.writeErrors, (u32)st.lastWriteRes);
        else
            batteryMenuLine(y, COLOR_WHITE, "");

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
