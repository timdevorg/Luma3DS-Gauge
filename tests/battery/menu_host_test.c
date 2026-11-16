/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Host test for the menu's pure helpers (sysmodules/rosalina/source/battery/battery_menu_logic.c):
*   1. Home summary: 7 lines, every line at most 49 chars and never cut, over every LED reason / mode / error, bars
*      level / reason / patch state, Sleep LED setting and a random walk over the numbers (500000 inputs); fixed
*      columns (value at 14, note at 27); expected texts for the documented states.
*   2. "Next:" hint: the decision table (error > recording > no curve (charge / record) > Power LED stock > none) and
*      the texts (at most 49 chars, none for "all set").
*   3. Diagnostics page titles and the two labelled voltage lines (all raw 0..255 fit, "=" lined up); the footer
*      scale in integers (raw x 2 cV) against the upstream float formula; voltage / temperature string length.
*
*   Usage: menu_host_test   (run through tests/battery/run_host_test.sh)
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fmt.h"
#include "battery/battery_menu_logic.h"
#include "battery/battery_mcu.h"
#include "battery/battery_ptm_bars.h"
#include "battery/battery_curve.h"

#undef sprintf

static int g_failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

// Rosalina's long is 32 bits: drop a single 'l' before d/u/x so u32 arguments work on 64-bit hosts (as sd_host_test.c)
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

static u32 rnd(void)
{
    static u32 x = 0x12345678u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static BatteryHomeInputs baseInputs(void)
{
    BatteryHomeInputs in;
    memset(&in, 0, sizeof(in));
    in.haveSample = true;
    in.pctX10 = 36;
    in.corrected = 29.94f;
    BatteryLedConfig_Defaults(&in.cfg);
    in.cfg.override = true;
    in.ledReason = BATTERY_LED_REASON_ACTIVE;
    in.ledDesired = BATTERY_LED_MODE_BLUE;
    in.barsLevel = 3;
    in.barsReason = BATTERY_BARS_REASON_FED;
    in.barsPatch = BATTERY_HOME_PATCH_OK;
    in.sleepPatch = BATTERY_HOME_PATCH_OK;
    in.haveCurve = true;
    in.curveSessions = 3;
    return in;
}

static void checkSummary(const BatteryHomeInputs *in)
{
    BatteryHomeLine out[BATTERY_HOME_SUMMARY_LINES];
    BatteryHome_Summary(in, out);
    for (u32 i = 0; i < BATTERY_HOME_SUMMARY_LINES; i++)
    {
        size_t len = strlen(out[i].text);
        CHECK(len <= BATTERY_MENU_LINE_CHARS);
        CHECK(len > 1 + BATTERY_HOME_LABEL_CHARS); // label and value present
        CHECK(out[i].text[0] == ' ' && out[i].text[1] != ' ');
        CHECK(out[i].tone <= BATTERY_TONE_ERROR);
        // value column: starts at 14, no label runs into it
        CHECK(out[i].text[BATTERY_HOME_LABEL_CHARS] == ' ' && out[i].text[1 + BATTERY_HOME_LABEL_CHARS] != ' ');
        // note column: one space after the 12-char value, so value and note never touch
        if (len > 1 + BATTERY_HOME_LABEL_CHARS + BATTERY_HOME_VALUE_CHARS)
            CHECK(out[i].text[1 + BATTERY_HOME_LABEL_CHARS + BATTERY_HOME_VALUE_CHARS] == ' ');
    }

    u8 step = BatteryHome_NextStep(in);
    const char *l1, *l2;
    BatteryHome_NextText(step, &l1, &l2);
    CHECK(strlen(l1) <= BATTERY_MENU_LINE_CHARS && strlen(l2) <= BATTERY_MENU_LINE_CHARS);
    CHECK((step == BATTERY_NEXT_NONE) == (l1[0] == 0));
}

static void testSummaryWidths(void)
{
    static const float corrected[] = { -1.0f, 0.0f, 0.04f, 29.94f, 99.96f, 100.0f };
    static const u32 pcts[] = { 0, 36, 949, 950, 1000 };
    static const u8 modes[] = { 0, 1, 2, 3, 4, 5, 6, BATTERY_LED_MODE_KEEP, BATTERY_LED_MODE_UNKNOWN };
    static const u8 sleepModes[] = { BATTERY_LED_SLEEP_SYSTEM, BATTERY_LED_MODE_BLUE_FORCED, BATTERY_LED_MODE_BLUE };

    // Every LED reason x mode x error x override, every bars level x reason x patch x setting, every sleep setting x
    // patch, all with the widest numbers
    u32 n = 0;
    for (u8 reason = 0; reason <= BATTERY_LED_REASON_TEST + 1; reason++)
        for (u32 m = 0; m < sizeof(modes); m++)
            for (u8 err = 0; err <= 2; err++)
                for (u8 ov = 0; ov <= 1; ov++)
                {
                    BatteryHomeInputs in = baseInputs();
                    in.ledReason = reason;
                    in.ledDesired = modes[m];
                    in.ledError = err;
                    in.cfg.override = ov;
                    in.pctX10 = 1000;
                    in.corrected = 100.0f;
                    in.recording = true;
                    in.recNumber = 9999;
                    in.recElapsedMs = 0xFFFFFFFFu;
                    in.recSleepGap = true;
                    in.curveSessions = BATTERY_CURVE_MAX_SESSIONS;
                    checkSummary(&in);
                    n++;
                }
    for (u8 level = 0; level <= 6; level++)
        for (u8 reason = 0; reason <= BATTERY_BARS_REASON_CHARGING + 1; reason++)
            for (u8 patch = 0; patch <= BATTERY_HOME_PATCH_UNAVAILABLE; patch++)
                for (u8 on = 0; on <= 1; on++)
                    for (u32 s = 0; s < sizeof(sleepModes); s++)
                    {
                        BatteryHomeInputs in = baseInputs();
                        in.barsLevel = level;
                        in.barsReason = reason;
                        in.barsPatch = patch;
                        in.sleepPatch = patch;
                        in.cfg.bars = on;
                        in.cfg.override = on;
                        in.cfg.sleepMode = sleepModes[s];
                        checkSummary(&in);
                        n++;
                    }

    // Random walk over everything
    for (u32 i = 0; i < 500000; i++)
    {
        BatteryHomeInputs in = baseInputs();
        in.haveSample = rnd() & 1;
        in.pctX10 = pcts[rnd() % (sizeof(pcts) / sizeof(pcts[0]))];
        in.adapter = rnd() & 1;
        in.charging = rnd() & 1;
        in.corrected = corrected[rnd() % (sizeof(corrected) / sizeof(corrected[0]))];
        in.cfg.override = rnd() & 1;
        in.cfg.bars = rnd() & 1;
        in.cfg.sleepMode = sleepModes[rnd() % sizeof(sleepModes)];
        in.ledReason = rnd() % (BATTERY_LED_REASON_TEST + 2);
        in.ledDesired = modes[rnd() % sizeof(modes)];
        in.ledError = rnd() % 3;
        in.barsLevel = rnd() % 7;
        in.barsReason = rnd() % (BATTERY_BARS_REASON_CHARGING + 2);
        in.barsPatch = rnd() % 3;
        in.sleepPatch = rnd() % 3;
        in.haveCurve = rnd() & 1;
        in.curveSessions = 1 + rnd() % 16;
        in.recording = rnd() & 1;
        in.recNumber = rnd() % 10000;
        in.recElapsedMs = (rnd() & 1) ? rnd() : 0xFFFFFFFFu;
        in.recSleepGap = rnd() & 1;
        in.recErrors = rnd() & 1;
        checkSummary(&in);
        n++;
    }
    CHECK(BatteryHome_Overflows() == 0);
    printf("home summary: %u inputs, every line <= %d chars, none cut (%u cut)\n", n, BATTERY_MENU_LINE_CHARS,
           BatteryHome_Overflows());
}

static void testSummaryTexts(void)
{
    BatteryHomeLine out[BATTERY_HOME_SUMMARY_LINES];
    BatteryHomeInputs in = baseInputs();
    BatteryHome_Summary(&in, out);
    CHECK(strcmp(out[0].text, " Battery      29.9 %       (stock gauge 3.6 %)") == 0);
    CHECK(strcmp(out[1].text, " Charger      no") == 0);
    CHECK(strcmp(out[2].text, " Power LED    blue         corrected") == 0);
    CHECK(strcmp(out[3].text, " HOME bars    2 of 4       corrected") == 0);
    CHECK(strcmp(out[4].text, " Sleep LED    dim blue") == 0);
    CHECK(strcmp(out[5].text, " Curve        3 sessions") == 0);
    CHECK(strcmp(out[6].text, " Recording    off") == 0);
    for (u32 i = 0; i < BATTERY_HOME_SUMMARY_LINES; i++)
        CHECK(out[i].tone == BATTERY_TONE_NORMAL);

    // First run: no curve, Power LED stock (the defaults)
    in = baseInputs();
    BatteryLedConfig_Defaults(&in.cfg);
    in.haveCurve = false;
    in.corrected = -1.0f;
    in.pctX10 = 724;
    in.ledReason = BATTERY_LED_REASON_OFF;
    in.ledDesired = BATTERY_LED_MODE_KEEP;
    in.barsLevel = 0;
    in.barsReason = BATTERY_BARS_REASON_NO_CURVE;
    BatteryHome_Summary(&in, out);
    CHECK(strcmp(out[0].text, " Battery      72.4 %       (stock, not corrected)") == 0);
    CHECK(strcmp(out[2].text, " Power LED    stock        (console's own)") == 0);
    CHECK(strcmp(out[3].text, " HOME bars    stock        (needs a curve)") == 0 && out[3].tone == BATTERY_TONE_INACTIVE);
    CHECK(strcmp(out[4].text, " Sleep LED    dim blue     (needs Power LED)") == 0 && out[4].tone == BATTERY_TONE_INACTIVE);
    CHECK(strcmp(out[5].text, " Curve        none yet") == 0);

    // Charging, held bars, recording, error, patch unavailable
    in = baseInputs();
    in.adapter = in.charging = true;
    in.ledReason = BATTERY_LED_REASON_CHARGER;
    in.barsReason = BATTERY_BARS_REASON_CHARGING;
    in.recording = true;
    in.recNumber = 5;
    in.recElapsedMs = (1 * 3600 + 23 * 60 + 5) * 1000u;
    BatteryHome_Summary(&in, out);
    CHECK(strcmp(out[1].text, " Charger      charging") == 0);
    CHECK(strcmp(out[2].text, " Power LED    blue         corrected, charging") == 0);
    CHECK(strcmp(out[3].text, " HOME bars    2 of 4       corrected, held") == 0);
    CHECK(strcmp(out[6].text, " Recording    #0005        1:23:05") == 0 && out[6].tone == BATTERY_TONE_NORMAL);
    in.recSleepGap = true;
    BatteryHome_Summary(&in, out);
    CHECK(strcmp(out[6].text, " Recording    #0005        1:23:05 sleep gap") == 0 && out[6].tone == BATTERY_TONE_ERROR);

    in = baseInputs();
    in.ledReason = BATTERY_LED_REASON_ERROR;
    in.barsPatch = in.sleepPatch = BATTERY_HOME_PATCH_UNAVAILABLE;
    in.haveSample = false;
    BatteryHome_Summary(&in, out);
    CHECK(strcmp(out[0].text, " Battery      ?            (gauge read failed)") == 0 && out[0].tone == BATTERY_TONE_ERROR);
    CHECK(strcmp(out[2].text, " Power LED    error        (see Diagnostics)") == 0 && out[2].tone == BATTERY_TONE_ERROR);
    CHECK(strcmp(out[3].text, " HOME bars    stock        (see Diagnostics)") == 0 && out[3].tone == BATTERY_TONE_INACTIVE);
    CHECK(strcmp(out[4].text, " Sleep LED    dim blue     (see Diagnostics)") == 0);

    // Bar levels 1..5
    static const char *const levels[] = { "1 (blinking)", "1 (red)", "2 of 4", "3 of 4", "4 of 4" };
    for (u8 l = 1; l <= 5; l++)
    {
        char want[64];
        in = baseInputs();
        in.barsLevel = l;
        BatteryHome_Summary(&in, out);
        snprintf(want, sizeof(want), " HOME bars    %-12s corrected", levels[l - 1]);
        CHECK(strcmp(out[3].text, want) == 0);
    }
    in = baseInputs();
    in.ledDesired = BATTERY_LED_MODE_BLINK;
    in.cfg.sleepMode = BATTERY_LED_MODE_BLUE;
    in.curveSessions = 1;
    BatteryHome_Summary(&in, out);
    CHECK(strcmp(out[2].text, " Power LED    blinking red corrected") == 0);
    CHECK(strcmp(out[4].text, " Sleep LED    bright blue") == 0);
    CHECK(strcmp(out[5].text, " Curve        1 session") == 0);
    printf("home summary: expected texts OK\n");
}

static void testNextStep(void)
{
    BatteryHomeInputs in = baseInputs();
    CHECK(BatteryHome_NextStep(&in) == BATTERY_NEXT_NONE);
    in.cfg.override = false;
    CHECK(BatteryHome_NextStep(&in) == BATTERY_NEXT_POWER_LED);
    in.haveCurve = false;
    in.pctX10 = 949;
    CHECK(BatteryHome_NextStep(&in) == BATTERY_NEXT_CHARGE);
    in.pctX10 = 950;
    CHECK(BatteryHome_NextStep(&in) == BATTERY_NEXT_RECORD);
    in.adapter = true; // on the charger the recorder refuses to start
    CHECK(BatteryHome_NextStep(&in) == BATTERY_NEXT_CHARGE);
    in.adapter = false;
    in.haveSample = false;
    CHECK(BatteryHome_NextStep(&in) == BATTERY_NEXT_CHARGE);
    in.haveSample = true;
    in.recording = true;
    CHECK(BatteryHome_NextStep(&in) == BATTERY_NEXT_RECORDING);
    in.haveCurve = true;
    in.cfg.override = true;
    CHECK(BatteryHome_NextStep(&in) == BATTERY_NEXT_RECORDING); // recording a further discharge
    in.ledError = 1;
    CHECK(BatteryHome_NextStep(&in) == BATTERY_NEXT_ERROR);
    in.ledError = 0;
    in.ledReason = BATTERY_LED_REASON_ERROR;
    CHECK(BatteryHome_NextStep(&in) == BATTERY_NEXT_ERROR);

    for (u8 step = BATTERY_NEXT_NONE; step <= BATTERY_NEXT_POWER_LED + 1; step++)
    {
        const char *l1, *l2;
        BatteryHome_NextText(step, &l1, &l2);
        CHECK(l1 != NULL && l2 != NULL);
        CHECK(strlen(l1) <= BATTERY_MENU_LINE_CHARS && strlen(l2) <= BATTERY_MENU_LINE_CHARS);
        bool none = step == BATTERY_NEXT_NONE || step > BATTERY_NEXT_POWER_LED;
        CHECK(none == (l1[0] == 0));
    }
    printf("next step: decision table and texts OK\n");
}

static void testDiagnostics(void)
{
    char t[96], l1[96], l2[96];
    for (u32 p = 0; p < BATTERY_DIAG_PAGES + 2; p++)
    {
        BatteryDiag_Title(t, p);
        CHECK(strlen(t) <= BATTERY_MENU_LINE_CHARS);
        CHECK(strncmp(t, "Diagnostics ", 12) == 0);
        CHECK(t[12] == (char)('1' + (p < BATTERY_DIAG_PAGES ? p : 0)) && strncmp(t + 13, "/3 - ", 5) == 0);
    }
    BatteryDiag_Title(t, 0);
    CHECK(strcmp(t, "Diagnostics 1/3 - Gauge and LED") == 0);

    BatteryDiag_FormatVoltage(l1, l2, 185);
    CHECK(strcmp(l1, "Voltage:        raw 185 = 3.613V (x5/256)") == 0);
    CHECK(strcmp(l2, "                        = 3.70V (x0.02, footer)") == 0);
    for (u32 raw = 0; raw < 256; raw++)
    {
        BatteryDiag_FormatVoltage(l1, l2, (u8)raw);
        CHECK(strlen(l1) <= BATTERY_MENU_LINE_CHARS && strlen(l2) <= BATTERY_MENU_LINE_CHARS);
        CHECK(strchr(l1, '=') - l1 == strchr(l2, '=') - l2);
    }

    // Footer scale: raw x 2 cV exactly. Count the raw values the upstream float code (menu.c before 16.13) shows
    // differently: 0.02f * raw, rounded to 0.01 V, then int part and (u32)(v * 100) % 100
    u32 floatDiffers = 0;
    for (u32 raw = 0; raw < 256; raw++)
    {
        float v = 0.02f * raw;
        v = (u32)((v + 0.005f) * 100.0f) / 100.0f;
        u32 shown = (u32)v * 100u + (u32)(v * 100.0f) % 100u;
        floatDiffers += shown != BatteryMcu_VoltageCv((u8)raw) ? 1 : 0;
        CHECK(BatteryMcu_VoltageCv((u8)raw) == 2 * raw);
    }
    printf("voltage: footer scale raw x 2 cV; the old float formatting differed for %u of 256 raw values\n", floatDiffers);

    // Voltage / temperature string: at most 11 chars in both units
    u32 maxLen = 0;
    for (u32 raw = 0; raw < 256; raw += 5)
        for (u32 temp = 0; temp < 256; temp++)
            for (u32 f = 0; f <= 1; f++)
            {
                BatteryMenu_FormatVoltTemp(t, (u8)raw, (u8)temp, f);
                maxLen = strlen(t) > maxLen ? (u32)strlen(t) : maxLen;
            }
    CHECK(maxLen <= 11);
    BatteryMenu_FormatVoltTemp(t, 185, 25, false);
    CHECK(strcmp(t, "3.70V 25\xF8""C") == 0);
    BatteryMenu_FormatVoltTemp(t, 210, 25, true);
    CHECK(strcmp(t, "4.20V 77\xF8""F") == 0);
    CHECK(BatteryMenu_Temperature(0, true) == 32 && BatteryMenu_Temperature(37, true) == 99 && BatteryMenu_Temperature(37, false) == 37);

    BatteryMenu_FormatDuration(t, 0);
    CHECK(strcmp(t, "0:00:00") == 0);
    BatteryMenu_FormatDuration(t, 3723000);
    CHECK(strcmp(t, "1:02:03") == 0);
    BatteryMenu_FormatDuration(t, 0xFFFFFFFFu);
    CHECK(strcmp(t, "1193:02:47") == 0);
    printf("diagnostics: titles, voltage lines, volt/temp (max %u chars), durations OK\n", maxLen);
}

int main(void)
{
    testSummaryWidths();
    testSummaryTexts();
    testNextStep();
    testDiagnostics();
    printf(g_failures == 0 ? "menu host test: OK\n" : "menu host test: %d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
