/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Host test for the menu's pure helpers (sysmodules/rosalina/source/battery/battery_menu_logic.c):
*   1. Home summary: 8 lines, every line at most 49 chars and never cut, over every LED reason / mode / error, bars
*      level / reason / patch state, Sleep LED setting and a random walk over the numbers (500000 inputs); fixed
*      columns (value at 14, note at 27); expected texts for the documented states, including every Time left state.
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
#include <math.h>
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
    in.etaState = BATTERY_ETA_OK;
    in.etaMinutes = 135;
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
                    in.etaMinutes = 23 * 60 + 55; // widest "~23h 55m"
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
        in.etaState = rnd() % (BATTERY_ETA_PROVISIONAL + 2);
        in.etaMinutes = (rnd() & 1) ? BATTERY_ETA_MAX_MINUTES : rnd() % (BATTERY_ETA_MAX_MINUTES + 1);
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
    CHECK(strcmp(out[1].text, " Time left    ~2h 15m      (at the current load)") == 0);
    CHECK(strcmp(out[2].text, " Charger      no") == 0);
    CHECK(strcmp(out[3].text, " Power LED    blue         corrected") == 0);
    CHECK(strcmp(out[4].text, " HOME bars    2 of 4       corrected") == 0);
    CHECK(strcmp(out[5].text, " Sleep LED    dim blue") == 0);
    CHECK(strcmp(out[6].text, " Curve        3 sessions") == 0);
    CHECK(strcmp(out[7].text, " Recording    off") == 0);
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
    CHECK(strcmp(out[3].text, " Power LED    stock        (console's own)") == 0);
    CHECK(strcmp(out[4].text, " HOME bars    stock        (needs a curve)") == 0 && out[4].tone == BATTERY_TONE_INACTIVE);
    CHECK(strcmp(out[5].text, " Sleep LED    dim blue     (needs Power LED)") == 0 && out[5].tone == BATTERY_TONE_INACTIVE);
    CHECK(strcmp(out[6].text, " Curve        none yet") == 0);

    // Charging, held bars, recording, error, patch unavailable
    in = baseInputs();
    in.adapter = in.charging = true;
    in.ledReason = BATTERY_LED_REASON_CHARGER;
    in.barsReason = BATTERY_BARS_REASON_CHARGING;
    in.recording = true;
    in.recNumber = 5;
    in.recElapsedMs = (1 * 3600 + 23 * 60 + 5) * 1000u;
    BatteryHome_Summary(&in, out);
    CHECK(strcmp(out[2].text, " Charger      charging") == 0);
    CHECK(strcmp(out[3].text, " Power LED    blue         corrected, charging") == 0);
    CHECK(strcmp(out[4].text, " HOME bars    2 of 4       corrected, held") == 0);
    CHECK(strcmp(out[7].text, " Recording    #0005        1:23:05") == 0 && out[7].tone == BATTERY_TONE_NORMAL);
    in.recSleepGap = true;
    BatteryHome_Summary(&in, out);
    CHECK(strcmp(out[7].text, " Recording    #0005        1:23:05 sleep gap") == 0 && out[7].tone == BATTERY_TONE_ERROR);

    in = baseInputs();
    in.ledReason = BATTERY_LED_REASON_ERROR;
    in.barsPatch = in.sleepPatch = BATTERY_HOME_PATCH_UNAVAILABLE;
    in.haveSample = false;
    BatteryHome_Summary(&in, out);
    CHECK(strcmp(out[0].text, " Battery      ?            (gauge read failed)") == 0 && out[0].tone == BATTERY_TONE_ERROR);
    CHECK(strcmp(out[3].text, " Power LED    error        (see Diagnostics)") == 0 && out[3].tone == BATTERY_TONE_ERROR);
    CHECK(strcmp(out[4].text, " HOME bars    stock        (see Diagnostics)") == 0 && out[4].tone == BATTERY_TONE_INACTIVE);
    CHECK(strcmp(out[5].text, " Sleep LED    dim blue     (see Diagnostics)") == 0);

    // Bar levels 1..5
    static const char *const levels[] = { "1 (blinking)", "1 (red)", "2 of 4", "3 of 4", "4 of 4" };
    for (u8 l = 1; l <= 5; l++)
    {
        char want[64];
        in = baseInputs();
        in.barsLevel = l;
        BatteryHome_Summary(&in, out);
        snprintf(want, sizeof(want), " HOME bars    %-12s corrected", levels[l - 1]);
        CHECK(strcmp(out[4].text, want) == 0);
    }
    in = baseInputs();
    in.ledDesired = BATTERY_LED_MODE_BLINK;
    in.cfg.sleepMode = BATTERY_LED_MODE_BLUE;
    in.curveSessions = 1;
    BatteryHome_Summary(&in, out);
    CHECK(strcmp(out[3].text, " Power LED    blinking red corrected") == 0);
    CHECK(strcmp(out[5].text, " Sleep LED    bright blue") == 0);
    CHECK(strcmp(out[6].text, " Curve        1 session") == 0);

    // Time left: every state (section 17)
    static const struct { u8 state; u16 minutes; const char *text; u8 tone; } eta[] = {
        { BATTERY_ETA_NO_DATA,      0,    " Time left    calculating  (10 min on battery)", BATTERY_TONE_INACTIVE },
        { BATTERY_ETA_CALCULATING,  0,    " Time left    calculating  (10 min on battery)", BATTERY_TONE_INACTIVE },
        { BATTERY_ETA_CHARGING,     0,    " Time left    charging",                         BATTERY_TONE_NORMAL },
        { BATTERY_ETA_NO_CURVE,     0,    " Time left    --           (needs a curve)",     BATTERY_TONE_INACTIVE },
        { BATTERY_ETA_UNKNOWN,      0,    " Time left    --           (no steady drain)",   BATTERY_TONE_INACTIVE },
        { BATTERY_ETA_OK,           0,    " Time left    <5m          (at the current load)", BATTERY_TONE_NORMAL },
        { BATTERY_ETA_OK,           45,   " Time left    ~45m         (at the current load)", BATTERY_TONE_NORMAL },
        { BATTERY_ETA_OK,           65,   " Time left    ~1h 05m      (at the current load)", BATTERY_TONE_NORMAL },
        { BATTERY_ETA_OK,           1440, " Time left    ~24h 00m     (at the current load)", BATTERY_TONE_NORMAL },
        { BATTERY_ETA_PROVISIONAL,  190,  " Time left    ~3h 10m?     (provisional)",       BATTERY_TONE_INACTIVE },
        { BATTERY_ETA_PROVISIONAL,  1440, " Time left    ~24h 00m?    (provisional)",       BATTERY_TONE_INACTIVE },
        { BATTERY_ETA_PROVISIONAL,  0,    " Time left    <5m?         (provisional)",       BATTERY_TONE_INACTIVE },
    };
    for (u32 i = 0; i < sizeof(eta) / sizeof(eta[0]); i++)
    {
        in = baseInputs();
        in.etaState = eta[i].state;
        in.etaMinutes = eta[i].minutes;
        BatteryHome_Summary(&in, out);
        CHECK(strcmp(out[1].text, eta[i].text) == 0 && out[1].tone == eta[i].tone);
    }
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
        CHECK(t[12] == (char)('1' + (p < BATTERY_DIAG_PAGES ? p : 0)) && strncmp(t + 13, "/4 - ", 5) == 0);
    }
    BatteryDiag_Title(t, 0);
    CHECK(strcmp(t, "Diagnostics 1/4 - Gauge and LED") == 0);
    BatteryDiag_Title(t, BATTERY_DIAG_PAGE_ETA_CFG);
    CHECK(strcmp(t, "Diagnostics 4/4 - Time left and config.ini") == 0);

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

// Diagnostics 4/4: the three time-left lines (every state x rate kind x window x reset reason, widest numbers) and the
// config.ini lines (widest key / value / count), all at most 49 chars; expected texts; the home screen note
static void testEtaConfigPage(void)
{
    static const float rates[] = { 0.0f, 0.0004f, 0.5f, 16.665f, 1000.0f, -0.01f, -1000.0f, NAN };
    static const u32 ages[] = { 0, 59, 60, 59999, 60000, 0xFFFFFFFFu };
    char l1[256], l2[256], l3[256];
    u32 n = 0, maxLen = 0;
    for (u8 state = 0; state <= BATTERY_ETA_PROVISIONAL + 1; state++)
        for (u32 kind = 0; kind < 3; kind++)
            for (u32 r = 0; r < sizeof(rates) / sizeof(rates[0]); r++)
                for (u32 a = 0; a < sizeof(ages) / sizeof(ages[0]); a++)
                    for (u8 reason = 0; reason <= BATTERY_ETA_RESET_NO_CURVE + 1; reason++)
                        for (u32 slope = 0; slope <= 1; slope++)
                        {
                            BatteryEtaDiag d;
                            memset(&d, 0, sizeof(d));
                            d.state = state;
                            d.minutes = (a & 1) ? BATTERY_ETA_MAX_MINUTES : 23 * 60 + 55;
                            d.haveRate = kind == 1;
                            d.haveProv = kind == 2;
                            d.rate = d.provRate = rates[r];
                            d.provAgeMin = ages[a];
                            d.haveSlope = slope;
                            d.windowDrain = rates[(r + 5) % (sizeof(rates) / sizeof(rates[0]))];
                            d.samples = BATTERY_ETA_SLOTS - 1;
                            d.spanMs = BATTERY_ETA_WINDOW_MS;
                            d.haveReset = reason <= BATTERY_ETA_RESET_NO_CURVE;
                            d.resetReason = reason;
                            d.resetAgeMs = ages[a] * 1000u;
                            BatteryDiag_FormatEta(&d, l1, l2, l3);
                            CHECK(strlen(l1) <= BATTERY_MENU_LINE_CHARS && strlen(l2) <= BATTERY_MENU_LINE_CHARS &&
                                  strlen(l3) <= BATTERY_MENU_LINE_CHARS);
                            u32 m = (u32)strlen(l1);
                            m = strlen(l2) > m ? (u32)strlen(l2) : m;
                            m = strlen(l3) > m ? (u32)strlen(l3) : m;
                            maxLen = m > maxLen ? m : maxLen;
                            n++;
                        }

    BatteryEtaDiag d;
    memset(&d, 0, sizeof(d));
    d.state = BATTERY_ETA_PROVISIONAL;
    d.minutes = 190;
    d.haveProv = true;
    d.provRate = 0.5f;
    d.provAgeMin = 125;
    d.haveSlope = true;
    d.windowDrain = 0.49f;
    d.samples = 8;
    d.spanMs = 7u * 60000u + 59000u;
    d.haveReset = true;
    d.resetReason = BATTERY_ETA_RESET_WAKE;
    d.resetAgeMs = 8u * 60000u;
    BatteryDiag_FormatEta(&d, l1, l2, l3);
    CHECK(strcmp(l1, "Time left:  ~3h 10m? (provisional)") == 0);
    CHECK(strcmp(l2, "Drain %/h:  30.0 kept, 2h old, 29.4 window") == 0);
    CHECK(strcmp(l3, "Reset:      8m ago (wake), 8 samples/7m") == 0);
    d.state = BATTERY_ETA_OK;
    d.haveProv = false;
    d.haveRate = true;
    d.rate = 0.25f;
    d.windowDrain = -0.004f;
    d.samples = 1;
    d.resetReason = BATTERY_ETA_RESET_CHARGER;
    BatteryDiag_FormatEta(&d, l1, l2, l3);
    CHECK(strcmp(l1, "Time left:  ~3h 10m") == 0);
    CHECK(strcmp(l2, "Drain %/h:  15.0 smoothed, -0.2 window") == 0);
    CHECK(strcmp(l3, "Reset:      8m ago (charger), 1 sample/7m") == 0);
    d.state = BATTERY_ETA_CALCULATING;
    d.haveRate = false;
    d.haveSlope = false;
    d.haveReset = false;
    d.samples = 0;
    d.spanMs = 0;
    BatteryDiag_FormatEta(&d, l1, l2, l3);
    CHECK(strcmp(l1, "Time left:  calculating") == 0 && strcmp(l2, "Drain %/h:  none yet, no window yet") == 0 &&
          strcmp(l3, "Reset:      --, 0 samples/0m") == 0);

    // config.ini lines
    BatteryConfigReport r;
    memset(&r, 0, sizeof(r));
    char t[256];
    BatteryDiag_ConfigHeader(t, &r);
    CHECK(strcmp(t, "config.ini: not loaded yet") == 0);
    CHECK(!BatteryHome_ConfigNote(t, &r) && t[0] == 0);
    r.loaded = true;
    BatteryDiag_ConfigHeader(t, &r);
    CHECK(strcmp(t, "config.ini: not found, defaults used") == 0);
    r.source = BATTERY_CFG_SOURCE_INI;
    BatteryDiag_ConfigHeader(t, &r);
    CHECK(strcmp(t, "config.ini: all values valid at boot") == 0);
    CHECK(!BatteryHome_ConfigNote(t, &r));
    BatteryDiag_ConfigUnknown(t, &r);
    CHECK(t[0] == 0);
    r.unknownCount = 1;
    memcpy(r.unknown, "foo", 4);
    BatteryDiag_ConfigHeader(t, &r);
    CHECK(strcmp(t, "config.ini: no values replaced at boot") == 0);
    BatteryDiag_ConfigUnknown(t, &r);
    CHECK(strcmp(t, "  unknown key foo ignored") == 0);
    CHECK(BatteryHome_ConfigNote(t, &r) && strcmp(t, " config.ini: 1 unknown key (see Diagnostics)") == 0);
    r.count = 2;
    r.issue[0].key = BATTERY_CFG_KEY_RED;
    r.issue[0].kind = BATTERY_CFG_ISSUE_RANGE;
    r.issue[0].used = 1;
    memcpy(r.issue[0].raw, "0", 2);
    r.issue[1].key = BATTERY_CFG_KEY_OVERRIDE;
    r.issue[1].kind = BATTERY_CFG_ISSUE_INVALID;
    r.issue[1].used = 0;
    memcpy(r.issue[1].raw, "yes", 4);
    BatteryDiag_ConfigHeader(t, &r);
    CHECK(strcmp(t, "config.ini: 2 values replaced at boot") == 0);
    BatteryDiag_ConfigIssue(t, &r.issue[0]);
    CHECK(strcmp(t, "  red_pct=0: out of range, used 1") == 0);
    BatteryDiag_ConfigIssue(t, &r.issue[1]);
    CHECK(strcmp(t, "  led_override=yes: not a number, used 0") == 0);
    CHECK(BatteryHome_ConfigNote(t, &r) && strcmp(t, " config.ini: 2 values replaced (see Diagnostics)") == 0);
    r.seen = true;
    CHECK(!BatteryHome_ConfigNote(t, &r) && t[0] == 0);
    r.seen = false;
    r.saved = true;
    CHECK(!BatteryHome_ConfigNote(t, &r));
    r.saved = false;
    r.count = 1;
    r.source = BATTERY_CFG_SOURCE_TMP;
    BatteryDiag_ConfigHeader(t, &r);
    CHECK(strcmp(t, "config.ini: 1 value replaced (from config.tmp)") == 0);
    CHECK(BatteryHome_ConfigNote(t, &r) && strcmp(t, " config.ini: 1 value replaced (see Diagnostics)") == 0);

    // Widest: every key with a 7-char value, 255 used; 8 issues; 255 unknown keys with a 12-char name
    u32 cfgMax = 0;
    r.count = BATTERY_CFG_KEYS;
    r.unknownCount = 255;
    memcpy(r.unknown, "abcdefghijkl", 13);
    for (u8 src = 0; src <= BATTERY_CFG_SOURCE_TMP; src++)
        for (u32 c = 0; c <= BATTERY_CFG_KEYS; c++)
        {
            r.source = src;
            r.count = (u8)c;
            BatteryDiag_ConfigHeader(t, &r);
            cfgMax = strlen(t) > cfgMax ? (u32)strlen(t) : cfgMax;
            CHECK(strlen(t) <= BATTERY_MENU_LINE_CHARS);
            BatteryHome_ConfigNote(t, &r);
            CHECK(strlen(t) <= BATTERY_MENU_LINE_CHARS);
        }
    BatteryDiag_ConfigUnknown(t, &r);
    CHECK(strcmp(t, "  unknown key abcdefghijkl ignored (+254 more)") == 0 && strlen(t) <= BATTERY_MENU_LINE_CHARS);
    r.count = 0;
    BatteryHome_ConfigNote(t, &r);
    CHECK(strcmp(t, " config.ini: 255 unknown keys (see Diagnostics)") == 0);
    for (u8 k = 0; k <= BATTERY_CFG_KEYS; k++)
        for (u8 kind = 0; kind <= BATTERY_CFG_ISSUE_RANGE; kind++)
        {
            BatteryConfigIssue is;
            is.key = k;
            is.kind = kind;
            is.used = 255;
            memcpy(is.raw, "9999999", 8);
            BatteryDiag_ConfigIssue(t, &is);
            cfgMax = strlen(t) > cfgMax ? (u32)strlen(t) : cfgMax;
            CHECK(strlen(t) <= BATTERY_MENU_LINE_CHARS);
        }
    // Page 4 fits the body: 3 time-left lines, blank, header, 8 issues, unknown, "saved since"
    CHECK(3 + 1 + 1 + BATTERY_CFG_KEYS + 1 + 1 <= BATTERY_MENU_BODY_LINES);
    printf("diagnostics 4/4: %u time-left inputs (max %u chars), config.ini lines (max %u chars), home note OK\n", n, maxLen,
           cfgMax);
}

int main(void)
{
    testSummaryWidths();
    testSummaryTexts();
    testNextStep();
    testDiagnostics();
    testEtaConfigPage();
    printf(g_failures == 0 ? "menu host test: OK\n" : "menu host test: %d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
