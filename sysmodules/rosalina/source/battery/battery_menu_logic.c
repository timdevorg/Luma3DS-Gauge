/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: pure helpers of the menu (home screen summary, "Next:" hint, Diagnostics titles and voltage lines).
*   No services; compiled unchanged into the host test (tests/battery/menu_host_test.c). Design: docs/battery-manager.md,
*   16.11 - 16.13.
*/

#include "fmt.h"
#include "memory.h"
#include "battery/battery_menu_logic.h"
#include "battery/battery_mcu.h"
#include "battery/battery_ptm_bars.h"

#define BATTERY_LED_ERR_NONE_VALUE  0 // BATTERY_LED_ERR_NONE (battery_led.h isn't host-compilable)

// ---- Shared formatting ----

u32 BatteryMenu_Temperature(u8 tempC, bool fahrenheit)
{
    return fahrenheit ? (9u * tempC + 2u) / 5u + 32u : tempC;
}

void BatteryMenu_FormatVoltTemp(char *out, u8 voltRaw, u8 tempC, bool fahrenheit)
{
    u32 cv = BatteryMcu_VoltageCv(voltRaw);
    sprintf(out, "%lu.%02luV %lu\xF8%c", cv / 100, cv % 100, BatteryMenu_Temperature(tempC, fahrenheit), fahrenheit ? 'F' : 'C');
}

void BatteryMenu_FormatDuration(char *out, u32 ms)
{
    u32 s = ms / 1000;
    sprintf(out, "%lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
}

// ---- Home screen ----

static u32 g_homeOverflows; // lines that had to be cut (host test: must stay 0)

u32 BatteryHome_Overflows(void)
{
    return g_homeOverflows;
}

static void homeLine(BatteryHomeLine *line, u8 tone, const char *label, const char *value, const char *note)
{
    char buf[96];
    int n = sprintf(buf, " %-*s%-*s %s", BATTERY_HOME_LABEL_CHARS, label, BATTERY_HOME_VALUE_CHARS, value, note);
    while (n > 0 && buf[n - 1] == ' ')
        buf[--n] = 0;
    if (n > BATTERY_MENU_LINE_CHARS)
    {
        g_homeOverflows++;
        buf[BATTERY_MENU_LINE_CHARS] = 0; // never more than the body width (every combination fits: host-tested)
    }
    memcpy(line->text, buf, sizeof(line->text));
    line->tone = tone;
}

static void formatPct(char *out, u32 x10)
{
    sprintf(out, "%lu.%lu %%", x10 / 10, x10 % 10);
}

static const char *ledColourName(u8 mode)
{
    switch (mode)
    {
        case BATTERY_LED_MODE_BLUE:
        case BATTERY_LED_MODE_BLUE_FORCED:  return "blue";
        case BATTERY_LED_MODE_RED:          return "red";
        case BATTERY_LED_MODE_BLINK:        return "blinking red";
        case BATTERY_LED_MODE_SLEEP:        return "sleep";
        case BATTERY_LED_MODE_POWER_OFF:    return "power off";
        case BATTERY_LED_MODE_AUTO:         return "stock";
        default:                            return "unchanged";
    }
}

static void homePowerLed(BatteryHomeLine *line, const BatteryHomeInputs *in)
{
    switch (in->ledReason)
    {
        case BATTERY_LED_REASON_ACTIVE:
            homeLine(line, BATTERY_TONE_NORMAL, "Power LED", ledColourName(in->ledDesired), "corrected");
            break;
        case BATTERY_LED_REASON_CHARGER:
            homeLine(line, BATTERY_TONE_NORMAL, "Power LED", ledColourName(in->ledDesired), "corrected, charging");
            break;
        case BATTERY_LED_REASON_OFF:
            homeLine(line, BATTERY_TONE_NORMAL, "Power LED", "stock", "(console's own)");
            break;
        case BATTERY_LED_REASON_NO_CURVE:
            homeLine(line, BATTERY_TONE_INACTIVE, "Power LED", "stock", "(needs a curve)");
            break;
        case BATTERY_LED_REASON_ERROR:
            homeLine(line, BATTERY_TONE_ERROR, "Power LED", "error", "(see Diagnostics)");
            break;
        case BATTERY_LED_REASON_SHELL:
            homeLine(line, BATTERY_TONE_NORMAL, "Power LED", ledColourName(in->ledDesired), "lid closed");
            break;
        case BATTERY_LED_REASON_SHUTDOWN:
            homeLine(line, BATTERY_TONE_NORMAL, "Power LED", "power off", "shutdown");
            break;
        case BATTERY_LED_REASON_TEST:
            homeLine(line, BATTERY_TONE_NORMAL, "Power LED", "LED test", "");
            break;
        default: // no MCU reading yet
            homeLine(line, BATTERY_TONE_INACTIVE, "Power LED", in->cfg.override ? "corrected" : "stock", "(no reading yet)");
            break;
    }
}

static void homeBars(BatteryHomeLine *line, const BatteryHomeInputs *in)
{
    static const char *const levels[] = { "stock", "1 (blinking)", "1 (red)", "2 of 4", "3 of 4", "4 of 4" };

    if (!in->cfg.bars)
        homeLine(line, BATTERY_TONE_NORMAL, "HOME bars", "stock", "");
    else if (in->barsPatch == BATTERY_HOME_PATCH_UNAVAILABLE)
        homeLine(line, BATTERY_TONE_INACTIVE, "HOME bars", "stock", "(see Diagnostics)");
    else if (in->barsLevel >= BATTERY_BARS_LEVEL_MIN && in->barsLevel <= BATTERY_BARS_LEVEL_MAX)
        homeLine(line, BATTERY_TONE_NORMAL, "HOME bars", levels[in->barsLevel],
                 in->barsReason == BATTERY_BARS_REASON_CHARGING ? "corrected, held" : "corrected");
    else if (in->barsReason == BATTERY_BARS_REASON_NO_CURVE)
        homeLine(line, BATTERY_TONE_INACTIVE, "HOME bars", "stock", "(needs a curve)");
    else if (in->barsReason == BATTERY_BARS_REASON_MCU_ERROR)
        homeLine(line, BATTERY_TONE_ERROR, "HOME bars", "stock", "(MCU read error)");
    else
        homeLine(line, BATTERY_TONE_INACTIVE, "HOME bars", "stock", "(no reading yet)");
}

static void homeSleepLed(BatteryHomeLine *line, const BatteryHomeInputs *in)
{
    const char *value = in->cfg.sleepMode == BATTERY_LED_MODE_BLUE ? "bright blue" :
                        in->cfg.sleepMode == BATTERY_LED_SLEEP_SYSTEM ? "stock" : "dim blue";
    if (in->cfg.sleepMode == BATTERY_LED_SLEEP_SYSTEM)
        homeLine(line, BATTERY_TONE_NORMAL, "Sleep LED", value, "");
    else if (!in->cfg.override)
        homeLine(line, BATTERY_TONE_INACTIVE, "Sleep LED", value, "(needs Power LED)");
    else if (in->sleepPatch == BATTERY_HOME_PATCH_UNAVAILABLE)
        homeLine(line, BATTERY_TONE_INACTIVE, "Sleep LED", value, "(see Diagnostics)");
    else
        homeLine(line, BATTERY_TONE_NORMAL, "Sleep LED", value, "");
}

// Time left (section 17): the LED thread's estimate. Value column at most 12 chars ("calculating", "~23h 55m").
static void homeTimeLeft(BatteryHomeLine *line, const BatteryHomeInputs *in)
{
    char value[16];
    switch (in->etaState)
    {
        case BATTERY_ETA_OK:
            BatteryEta_Format(value, in->etaMinutes);
            homeLine(line, BATTERY_TONE_NORMAL, "Time left", value, "(at the current load)");
            break;
        case BATTERY_ETA_CHARGING:
            homeLine(line, BATTERY_TONE_NORMAL, "Time left", "charging", "");
            break;
        case BATTERY_ETA_NO_CURVE:
            homeLine(line, BATTERY_TONE_INACTIVE, "Time left", "--", "(needs a curve)");
            break;
        case BATTERY_ETA_UNKNOWN:
            homeLine(line, BATTERY_TONE_INACTIVE, "Time left", "--", "(no steady drain)");
            break;
        case BATTERY_ETA_PROVISIONAL: // after a wake: the rate from before, until 10 new minutes (17.7)
            BatteryEta_Format(value, in->etaMinutes);
            {
                u32 n = strlen(value);
                value[n] = '?';
                value[n + 1] = 0;
            }
            homeLine(line, BATTERY_TONE_INACTIVE, "Time left", value, "(provisional)");
            break;
        default: // no poll yet, or less than 10 minutes of samples
            homeLine(line, BATTERY_TONE_INACTIVE, "Time left", "calculating", "(10 min on battery)");
            break;
    }
}

void BatteryHome_Summary(const BatteryHomeInputs *in, BatteryHomeLine out[BATTERY_HOME_SUMMARY_LINES])
{
    char value[32], note[32];

    // Battery: the corrected % first, the stock gauge in brackets
    if (!in->haveSample)
        homeLine(&out[0], BATTERY_TONE_ERROR, "Battery", "?", "(gauge read failed)");
    else if (in->corrected >= 0.0f)
    {
        u32 c10 = (u32)(in->corrected * 10.0f + 0.5f);
        char stock[16];
        formatPct(value, c10);
        formatPct(stock, in->pctX10);
        sprintf(note, "(stock gauge %s)", stock);
        homeLine(&out[0], BATTERY_TONE_NORMAL, "Battery", value, note);
    }
    else
    {
        formatPct(value, in->pctX10);
        homeLine(&out[0], BATTERY_TONE_NORMAL, "Battery", value, "(stock, not corrected)");
    }

    homeTimeLeft(&out[1], in);

    if (!in->haveSample)
        homeLine(&out[2], BATTERY_TONE_NORMAL, "Charger", "?", "");
    else if (in->adapter && in->charging)
        homeLine(&out[2], BATTERY_TONE_NORMAL, "Charger", "charging", "");
    else if (in->adapter)
        homeLine(&out[2], BATTERY_TONE_NORMAL, "Charger", "plugged in", "(not charging)");
    else
        homeLine(&out[2], BATTERY_TONE_NORMAL, "Charger", "no", "");

    homePowerLed(&out[3], in);
    homeBars(&out[4], in);
    homeSleepLed(&out[5], in);

    if (in->haveCurve)
    {
        sprintf(value, "%lu session%s", in->curveSessions, in->curveSessions == 1 ? "" : "s");
        homeLine(&out[6], BATTERY_TONE_NORMAL, "Curve", value, "");
    }
    else
        homeLine(&out[6], BATTERY_TONE_NORMAL, "Curve", "none yet", "");

    if (in->recording)
    {
        char dur[16];
        BatteryMenu_FormatDuration(dur, in->recElapsedMs);
        sprintf(value, "#%04lu", (u32)in->recNumber);
        sprintf(note, "%s%s", dur, in->recSleepGap ? " sleep gap" : in->recErrors ? " SD errors" : "");
        homeLine(&out[7], in->recSleepGap || in->recErrors ? BATTERY_TONE_ERROR : BATTERY_TONE_NORMAL, "Recording", value, note);
    }
    else
        homeLine(&out[7], BATTERY_TONE_NORMAL, "Recording", "off", "");
}

u8 BatteryHome_NextStep(const BatteryHomeInputs *in)
{
    if (in->ledReason == BATTERY_LED_REASON_ERROR || in->ledError != BATTERY_LED_ERR_NONE_VALUE)
        return BATTERY_NEXT_ERROR;
    if (in->recording)
        return BATTERY_NEXT_RECORDING;
    if (!in->haveCurve)
        return in->haveSample && !in->adapter && in->pctX10 >= 950 ? BATTERY_NEXT_RECORD : BATTERY_NEXT_CHARGE;
    if (!in->cfg.override)
        return BATTERY_NEXT_POWER_LED;
    return BATTERY_NEXT_NONE;
}

void BatteryHome_NextText(u8 step, const char **line1, const char **line2)
{
    // Indented by one like the summary rows; the second line lines up after "Next: "
    switch (step)
    {
        case BATTERY_NEXT_ERROR:
            *line1 = " Next: the LED control stopped after errors,";
            *line2 = "       see Advanced > Diagnostics.";
            break;
        case BATTERY_NEXT_RECORDING:
            *line1 = " Recording: don't close the lid or charge";
            *line2 = " until the console switches itself off.";
            break;
        case BATTERY_NEXT_CHARGE:
            *line1 = " Next: charge to 100 %, unplug, record a full";
            *line2 = "       discharge, then Learning > Build curve.";
            break;
        case BATTERY_NEXT_RECORD:
            *line1 = " Next: Learning > Start recording, then use";
            *line2 = "       it until it switches itself off.";
            break;
        case BATTERY_NEXT_POWER_LED:
            *line1 = " Next: Settings > Power LED: corrected, so";
            *line2 = "       the power LED follows the curve too.";
            break;
        default:
            *line1 = "";
            *line2 = "";
            break;
    }
}

bool BatteryHome_ConfigNote(char *out, const BatteryConfigReport *r)
{
    out[0] = 0;
    if (!r->loaded || r->seen || r->saved)
        return false;
    if (r->count != 0)
        sprintf(out, " config.ini: %lu value%s replaced (see Diagnostics)", (u32)r->count, r->count == 1 ? "" : "s");
    else if (r->unknownCount != 0)
        sprintf(out, " config.ini: %lu unknown key%s (see Diagnostics)", (u32)r->unknownCount, r->unknownCount == 1 ? "" : "s");
    return out[0] != 0;
}

// ---- Diagnostics ----

void BatteryDiag_Title(char *out, u32 page)
{
    static const char *const names[BATTERY_DIAG_PAGES] = {
        "Gauge and LED", "Sleep LED and ptm patches", "Recorder and curve", "Time left and config.ini",
    };
    if (page >= BATTERY_DIAG_PAGES)
        page = 0;
    sprintf(out, "Diagnostics %lu/%lu - %s", page + 1, (u32)BATTERY_DIAG_PAGES, names[page]);
}

void BatteryDiag_FormatVoltage(char *line1, char *line2, u8 voltRaw)
{
    u32 mv = BatteryMcu_VoltageMv(voltRaw);
    u32 cv = BatteryMcu_VoltageCv(voltRaw);
    int n = sprintf(line1, "Voltage:        raw %lu = ", (u32)voltRaw);
    sprintf(line1 + n, "%lu.%03luV (x5/256)", mv / 1000, mv % 1000);
    // "=" under "=" of line1 (its column depends on the digits of raw)
    memset(line2, ' ', n - 2);
    sprintf(line2 + n - 2, "= %lu.%02luV (x0.02, footer)", cv / 100, cv % 100);
}

// % per minute -> "12.3" % per hour (signed, at most -999.9 .. 999.9)
static void formatRatePerHour(char *out, float perMin)
{
    float h = perMin * 60.0f;
    bool neg = h < 0.0f;
    if (neg)
        h = -h;
    if (!(h <= 999.9f))
        h = 999.9f; // also NaN
    u32 x10 = (u32)(h * 10.0f + 0.5f);
    sprintf(out, "%s%lu.%lu", neg && x10 != 0 ? "-" : "", x10 / 10, x10 % 10);
}

// "25m", "3h", ">999h"
static void formatAgeMin(char *out, u32 minutes)
{
    if (minutes < 60)
        sprintf(out, "%lum", minutes);
    else if (minutes / 60 <= 999)
        sprintf(out, "%luh", minutes / 60);
    else
        memcpy(out, ">999h", 6);
}

void BatteryDiag_FormatEta(const BatteryEtaDiag *d, char *line1, char *line2, char *line3)
{
    char value[16], a[16], b[16];
    switch (d->state)
    {
        case BATTERY_ETA_OK:
            BatteryEta_Format(value, d->minutes);
            sprintf(line1, "Time left:  %s", value);
            break;
        case BATTERY_ETA_PROVISIONAL:
            BatteryEta_Format(value, d->minutes);
            sprintf(line1, "Time left:  %s? (provisional)", value);
            break;
        case BATTERY_ETA_CHARGING:      sprintf(line1, "Time left:  charging"); break;
        case BATTERY_ETA_NO_CURVE:      sprintf(line1, "Time left:  -- (no curve)"); break;
        case BATTERY_ETA_UNKNOWN:       sprintf(line1, "Time left:  -- (no steady drain)"); break;
        case BATTERY_ETA_CALCULATING:   sprintf(line1, "Time left:  calculating"); break;
        default:                        sprintf(line1, "Time left:  no reading yet"); break;
    }

    // Smoothed (or kept) rate, then the raw slope of the newest window, both in % per hour
    int n;
    if (d->haveRate)
    {
        formatRatePerHour(a, d->rate);
        n = sprintf(line2, "Drain %%/h:  %s smoothed, ", a);
    }
    else if (d->haveProv)
    {
        formatRatePerHour(a, d->provRate);
        formatAgeMin(b, d->provAgeMin);
        n = sprintf(line2, "Drain %%/h:  %s kept, %s old, ", a, b);
    }
    else
        n = sprintf(line2, "Drain %%/h:  none yet, ");
    if (d->haveSlope)
    {
        formatRatePerHour(a, d->windowDrain);
        sprintf(line2 + n, "%s window", a);
    }
    else
        sprintf(line2 + n, "no window yet");

    u32 spanMin = d->spanMs / 60000u;
    if (d->haveReset)
    {
        formatAgeMin(a, d->resetAgeMs / 60000u);
        sprintf(line3, "Reset:      %s ago (%s), %lu sample%s/%lum", a, BatteryEta_ResetName(d->resetReason), (u32)d->samples,
                d->samples == 1 ? "" : "s", spanMin);
    }
    else
        sprintf(line3, "Reset:      --, %lu sample%s/%lum", (u32)d->samples, d->samples == 1 ? "" : "s", spanMin);
}

void BatteryDiag_ConfigHeader(char *out, const BatteryConfigReport *r)
{
    bool tmp = r->source == BATTERY_CFG_SOURCE_TMP;
    if (!r->loaded)
        sprintf(out, "config.ini: not loaded yet");
    else if (r->count != 0)
        sprintf(out, "config.ini: %lu value%s replaced %s", (u32)r->count, r->count == 1 ? "" : "s",
                tmp ? "(from config.tmp)" : "at boot");
    else if (r->source == BATTERY_CFG_SOURCE_DEFAULTS && r->unknownCount == 0)
        sprintf(out, "config.ini: not found, defaults used");
    else if (r->unknownCount != 0)
        sprintf(out, "config.ini: no values replaced %s", tmp ? "(from config.tmp)" : "at boot");
    else
        sprintf(out, "config.ini: all values valid %s", tmp ? "(from config.tmp)" : "at boot");
}

void BatteryDiag_ConfigIssue(char *out, const BatteryConfigIssue *is)
{
    sprintf(out, "  %s=%s: %s, used %lu", BatteryLedConfig_KeyName(is->key), is->raw,
            is->kind == BATTERY_CFG_ISSUE_INVALID ? "not a number" : "out of range", (u32)is->used);
}

void BatteryDiag_ConfigUnknown(char *out, const BatteryConfigReport *r)
{
    out[0] = 0;
    if (r->unknownCount == 1)
        sprintf(out, "  unknown key %s ignored", r->unknown);
    else if (r->unknownCount > 1)
        sprintf(out, "  unknown key %s ignored (+%lu more)", r->unknown, (u32)r->unknownCount - 1);
}
