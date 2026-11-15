/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: power LED decision logic (pure code, no services / MCU / SD).
*
*   Compiled into Rosalina and, unchanged, into the host test (tests/battery/led_host_test.c).
*   Design: docs/battery-manager.md, section 12.
*/

#include <stddef.h>
#include <string.h>
#include "battery/battery_led_logic.h"

// ---- Settings ----

void BatteryLedConfig_Defaults(BatteryLedConfig *cfg)
{
    cfg->override = false;
    cfg->redPct = BATTERY_LED_DEFAULT_RED_PCT;
    cfg->blinkPct = BATTERY_LED_DEFAULT_BLINK_PCT;
    cfg->blueMode = BATTERY_LED_DEFAULT_BLUE_MODE;
    cfg->sleepMode = BATTERY_LED_DEFAULT_SLEEP_MODE;
}

static bool batteryLedSleepSettingValid(u8 m)
{
    return m == BATTERY_LED_MODE_BLUE || m == BATTERY_LED_MODE_BLUE_FORCED || m == BATTERY_LED_MODE_SLEEP ||
           m == BATTERY_LED_SLEEP_SYSTEM;
}

void BatteryLedConfig_Sanitize(BatteryLedConfig *cfg)
{
    if (cfg->redPct < 1)
        cfg->redPct = 1;
    if (cfg->redPct > BATTERY_LED_MAX_RED_PCT)
        cfg->redPct = BATTERY_LED_MAX_RED_PCT;
    if (cfg->blinkPct >= cfg->redPct)
        cfg->blinkPct = cfg->redPct - 1;
    if (cfg->blueMode != BATTERY_LED_MODE_BLUE && cfg->blueMode != BATTERY_LED_MODE_BLUE_FORCED)
        cfg->blueMode = BATTERY_LED_DEFAULT_BLUE_MODE;
    if (!batteryLedSleepSettingValid(cfg->sleepMode))
        cfg->sleepMode = BATTERY_LED_DEFAULT_SLEEP_MODE;
}

bool BatteryLedConfig_Parse(const char *buf, BatteryLedConfig *cfg)
{
    bool any = false;
    for (const char *p = buf; *p != 0;)
    {
        const char *eq = p;
        while (*eq != 0 && *eq != '=' && *eq != '\n')
            eq++;
        if (*eq == '=' && (u32)(eq - p) == 10 && strncmp(p, "sleep_mode", 10) == 0 && strncmp(eq + 1, "system", 6) == 0 &&
            (eq[7] == 0 || eq[7] == '\n' || eq[7] == '\r' || eq[7] == ' ' || eq[7] == '#'))
        {
            cfg->sleepMode = BATTERY_LED_SLEEP_SYSTEM; // the word form of sleep_mode=0
            any = true;
        }
        else if (*eq == '=' && *p != '#' && eq[1] >= '0' && eq[1] <= '9')
        {
            u32 v = 0;
            for (const char *d = eq + 1; *d >= '0' && *d <= '9' && v < 1000; d++)
                v = 10 * v + (u32)(*d - '0');
            u8 v8 = (u8)(v > 255 ? 255 : v);
            u32 keyLen = (u32)(eq - p);

            if (keyLen == 12 && strncmp(p, "led_override", 12) == 0)
            {
                cfg->override = v != 0;
                any = true;
            }
            else if (keyLen == 7 && strncmp(p, "red_pct", 7) == 0)
            {
                cfg->redPct = v8;
                any = true;
            }
            else if (keyLen == 9 && strncmp(p, "blink_pct", 9) == 0)
            {
                cfg->blinkPct = v8;
                any = true;
            }
            else if (keyLen == 9 && strncmp(p, "blue_mode", 9) == 0)
            {
                cfg->blueMode = v8;
                any = true;
            }
            else if (keyLen == 10 && strncmp(p, "sleep_mode", 10) == 0)
            {
                cfg->sleepMode = v8; // invalid values become the default in BatteryLedConfig_Sanitize
                any = true;
            }
        }

        while (*p != 0 && *p != '\n')
            p++;
        if (*p == '\n')
            p++;
    }
    return any;
}

// ---- Decision ----

void BatteryLedLogic_Init(BatteryLedLogic *l)
{
    l->level = BATTERY_LED_LEVEL_BLUE;
    l->levelValid = false;
    l->reevaluate = false;
    l->onCharger = false;
}

void BatteryLedLogic_Reevaluate(BatteryLedLogic *l)
{
    l->reevaluate = true;
}

// Level for a corrected %; extra > 0 makes it more pessimistic (used to decide whether the LED may get better)
static u8 batteryLedLevelFor(const BatteryLedConfig *cfg, float corrected, float extra)
{
    if (corrected <= (float)cfg->blinkPct + extra)
        return BATTERY_LED_LEVEL_BLINK;
    if (corrected <= (float)cfg->redPct + extra)
        return BATTERY_LED_LEVEL_RED;
    return BATTERY_LED_LEVEL_BLUE;
}

u8 BatteryLedLogic_ModeForLevel(const BatteryLedConfig *cfg, u8 level)
{
    switch (level)
    {
        case BATTERY_LED_LEVEL_BLINK:   return BATTERY_LED_MODE_BLINK;
        case BATTERY_LED_LEVEL_RED:     return BATTERY_LED_MODE_RED;
        default:                        return cfg->blueMode;
    }
}

u8 BatteryLedLogic_SleepModeForLevel(const BatteryLedConfig *cfg, u8 level)
{
    // Blue: the Sleep LED setting. Default 5 (blue on, red off): the MCU's sleep animation (2) picks the red
    // LED when the MCU itself thinks the battery is low (section 13.9). "system" writes nothing.
    // Red / blinking red are kept: the stock LED also shows low battery instead of the sleep animation,
    // blinking uses less LED power than solid red, and switching blink -> solid red on every lid close would
    // only be confusing.
    switch (level)
    {
        case BATTERY_LED_LEVEL_BLINK:   return BATTERY_LED_MODE_BLINK;
        case BATTERY_LED_LEVEL_RED:     return BATTERY_LED_MODE_RED;
        default:
            if (cfg->sleepMode == BATTERY_LED_SLEEP_SYSTEM || !batteryLedSleepSettingValid(cfg->sleepMode))
                return BATTERY_LED_MODE_KEEP;
            return cfg->sleepMode;
    }
}

u8 BatteryLedLogic_SleepWrite(u8 desired, u8 reason, u8 level, u8 lastWritten, bool entering)
{
    if (desired == BATTERY_LED_MODE_KEEP)
        return BATTERY_LED_MODE_KEEP;
    // Blue level, lid just closed: write the configured mode even if the LED should already show it, so the
    // MCU has been told exactly this right before sleep (diagnostics compare it with the wake read-back)
    if (entering && reason == BATTERY_LED_REASON_SHELL && level == BATTERY_LED_LEVEL_BLUE)
        return desired;
    return BatteryLedLogic_NeedsWrite(desired, lastWritten) ? desired : BATTERY_LED_MODE_KEEP;
}

const char *BatteryLedLogic_SleepSettingName(u8 sleepMode)
{
    switch (sleepMode)
    {
        case BATTERY_LED_MODE_BLUE:         return "blue(5)";
        case BATTERY_LED_MODE_BLUE_FORCED:  return "blue(1)";
        case BATTERY_LED_MODE_SLEEP:        return "anim(2)";
        case BATTERY_LED_SLEEP_SYSTEM:      return "system";
        default:                            return "other";
    }
}

u8 BatteryLedLogic_NextSleepSetting(u8 sleepMode)
{
    switch (sleepMode)
    {
        case BATTERY_LED_MODE_BLUE:         return BATTERY_LED_MODE_BLUE_FORCED;
        case BATTERY_LED_MODE_BLUE_FORCED:  return BATTERY_LED_MODE_SLEEP;
        case BATTERY_LED_MODE_SLEEP:        return BATTERY_LED_SLEEP_SYSTEM;
        default:                            return BATTERY_LED_MODE_BLUE;
    }
}

bool BatteryLedLogic_OwnsLed(u8 lastWritten)
{
    return lastWritten != BATTERY_LED_MODE_AUTO && lastWritten != BATTERY_LED_MODE_UNKNOWN &&
           lastWritten != BATTERY_LED_MODE_POWER_OFF;
}

bool BatteryLedLogic_NeedsWrite(u8 desired, u8 lastWritten)
{
    if (desired == BATTERY_LED_MODE_KEEP)
        return false;
    if (desired == BATTERY_LED_MODE_AUTO)
        return lastWritten != BATTERY_LED_MODE_AUTO && lastWritten != BATTERY_LED_MODE_UNKNOWN;
    return desired != lastWritten;
}

// Discharging: the level only gets worse (voltage recovers when the load drops)
static void batteryLedDischargeLevel(BatteryLedLogic *l, const BatteryLedConfig *cfg, float corrected)
{
    u8 fresh = batteryLedLevelFor(cfg, corrected, 0.0f);
    if (!l->levelValid)
    {
        // Boot, or the adapter was just unplugged: start from the corrected % as it is
        l->level = fresh;
        l->levelValid = true;
        l->reevaluate = false;
        return;
    }

    if (l->reevaluate)
    {
        // After a settings change the LED may get better, with hysteresis
        u8 hyst = batteryLedLevelFor(cfg, corrected, BATTERY_LED_HYSTERESIS);
        if (hyst < l->level)
            l->level = hyst;
        l->reevaluate = false;
    }

    if (fresh > l->level)
        l->level = fresh;
}

// On the adapter: the level only gets better (red -> blue), with hysteresis, never worse
static void batteryLedChargeLevel(BatteryLedLogic *l, const BatteryLedConfig *cfg, const BatteryLedInputs *in)
{
    if (!l->levelValid || l->reevaluate)
    {
        // Boot on the charger, or a settings change while charging: start from the corrected % as it is
        l->level = batteryLedLevelFor(cfg, in->corrected, 0.0f);
        l->levelValid = true;
        l->reevaluate = false;
    }
    else
    {
        u8 hyst = batteryLedLevelFor(cfg, in->corrected, BATTERY_LED_HYSTERESIS);
        if (hyst < l->level)
            l->level = hyst;
    }

    if (in->adapter && !in->charging && in->pct >= BATTERY_LED_FULL_MCU_PCT)
        l->level = BATTERY_LED_LEVEL_BLUE; // fully charged
}

u8 BatteryLedLogic_Decide(BatteryLedLogic *l, const BatteryLedConfig *cfg, const BatteryLedInputs *in, u8 *outReason)
{
    u8 reason;
    u8 mode = BATTERY_LED_MODE_AUTO;
    bool charger = in->adapter || in->charging;

    // Plugged in: the charge starts from the current level. Unplugged: a new discharge starts from the
    // corrected % at that moment (the "only gets worse" ratchet restarts there).
    if (charger != l->onCharger)
    {
        l->onCharger = charger;
        if (!charger)
            l->levelValid = false;
    }

    if (in->shuttingDown)
    {
        reason = BATTERY_LED_REASON_SHUTDOWN;
        mode = BATTERY_LED_MODE_KEEP; // the shutdown latch (BatteryLedGate) decides the one last write
    }
    else if (in->error)
        reason = BATTERY_LED_REASON_ERROR;
    else if (!in->override)
        reason = BATTERY_LED_REASON_OFF;
    else if (in->corrected < 0.0f)
        reason = BATTERY_LED_REASON_NO_CURVE;
    else
    {
        if (charger)
            batteryLedChargeLevel(l, cfg, in);
        else
            batteryLedDischargeLevel(l, cfg, in->corrected);

        if (in->shellClosed)
        {
            reason = BATTERY_LED_REASON_SHELL;
            // Never the first write of a boot, and never "automatic" (that would show the MCU's own
            // low-battery blinking instead of a sleep indication)
            if (in->lastWritten == BATTERY_LED_MODE_UNKNOWN || in->lastWritten == BATTERY_LED_MODE_AUTO)
                mode = BATTERY_LED_MODE_KEEP;
            else
                mode = BatteryLedLogic_SleepModeForLevel(cfg, l->level);
        }
        else
        {
            reason = charger ? BATTERY_LED_REASON_CHARGER : BATTERY_LED_REASON_ACTIVE;
            mode = BatteryLedLogic_ModeForLevel(cfg, l->level);
        }
    }

    if (outReason != NULL)
        *outReason = reason;
    return mode;
}

// ---- Shutdown latch ----

void BatteryLedGate_Init(BatteryLedGate *g)
{
    g->state = BATTERY_LED_GATE_OPEN;
}

u8 BatteryLedGate_Shutdown(BatteryLedGate *g, u8 lastWritten, bool readValid, u8 readMode)
{
    if (g->state != BATTERY_LED_GATE_OPEN)
        return BATTERY_LED_MODE_KEEP;

    // Only if we own the LED; if the system has already set "power off", leave it alone
    if (BatteryLedLogic_OwnsLed(lastWritten) && !(readValid && readMode == BATTERY_LED_MODE_POWER_OFF))
    {
        g->state = BATTERY_LED_GATE_LAST_WRITE;
        return BATTERY_LED_MODE_POWER_OFF;
    }

    g->state = BATTERY_LED_GATE_CLOSED;
    return BATTERY_LED_MODE_KEEP;
}

bool BatteryLedGate_Allow(BatteryLedGate *g, u8 mode)
{
    switch (g->state)
    {
        case BATTERY_LED_GATE_OPEN:
            return true;
        case BATTERY_LED_GATE_LAST_WRITE:
            if (mode != BATTERY_LED_MODE_POWER_OFF)
                return false;
            g->state = BATTERY_LED_GATE_CLOSED; // used up, whether the write succeeds or not
            return true;
        default:
            return false;
    }
}

bool BatteryLedGate_IsOpen(const BatteryLedGate *g)
{
    return g->state == BATTERY_LED_GATE_OPEN;
}

// ---- Mismatch detection ----

void BatteryLedWatch_Init(BatteryLedWatch *w)
{
    memset(w, 0, sizeof(*w));
    w->lastWritten = BATTERY_LED_MODE_UNKNOWN;
    w->readback = BATTERY_LED_READBACK_UNKNOWN;
}

void BatteryLedWatch_Written(BatteryLedWatch *w, u8 mode, u64 nowMs)
{
    w->lastWritten = mode;
    w->lastWriteMs = nowMs;
    w->redOffStreak = 0;
}

bool BatteryLedWatch_Mismatch(BatteryLedWatch *w, u8 desired, const BatteryLedObserved *o, u64 nowMs)
{
    // Automatic, the sleep animation and "power off" belong to the system / fade by design: not watched
    if (desired == BATTERY_LED_MODE_AUTO || desired == BATTERY_LED_MODE_SLEEP || desired == BATTERY_LED_MODE_POWER_OFF ||
        desired == BATTERY_LED_MODE_KEEP || desired == BATTERY_LED_MODE_UNKNOWN || desired != w->lastWritten)
    {
        w->redOffStreak = 0;
        return false;
    }

    u64 grace = desired == BATTERY_LED_MODE_RED || desired == BATTERY_LED_MODE_BLINK ? BATTERY_LED_GRACE_RED_MS : BATTERY_LED_GRACE_MS;
    if (nowMs - w->lastWriteMs < grace)
    {
        w->redOffStreak = 0;
        return false;
    }

    bool bad = false;
    if (o->infoValid)
    {
        switch (desired)
        {
            case BATTERY_LED_MODE_BLUE:
            case BATTERY_LED_MODE_BLUE_FORCED:
                bad = o->red != 0 || o->blue == 0;
                break;
            case BATTERY_LED_MODE_RED:
                if (o->red == 0)
                    bad = ++w->redOffStreak >= BATTERY_LED_RED_OFF_POLLS;
                else
                    w->redOffStreak = 0;
                if (o->blue != 0)
                    bad = true;
                break;
            case BATTERY_LED_MODE_BLINK:
                bad = o->blue != 0; // red toggles while blinking
                break;
            default:
                break;
        }
    }

    // Reg 0x29 is a "dynamic" register: only use it once it has been seen to read back what we wrote
    if (o->modeValid)
    {
        if (w->readback == BATTERY_LED_READBACK_UNKNOWN)
        {
            if (o->mode == desired)
                w->readback = BATTERY_LED_READBACK_TRUSTED;
            else if (o->infoValid && !bad)
                w->readback = BATTERY_LED_READBACK_UNTRUSTED;
        }
        if (w->readback == BATTERY_LED_READBACK_TRUSTED && o->mode != desired)
            bad = true;
    }

    if (bad)
        w->redOffStreak = 0;
    return bad;
}

// ---- Voltage smoothing ----

void BatteryLedSmoother_Init(BatteryLedSmoother *s)
{
    memset(s, 0, sizeof(*s));
}

void BatteryLedSmoother_Push(BatteryLedSmoother *s, u8 voltRaw)
{
    s->win[s->count % BATTERY_LED_VOLT_WINDOW] = voltRaw;
    s->count++;
}

float BatteryLedSmoother_Median(const BatteryLedSmoother *s)
{
    u8 v[BATTERY_LED_VOLT_WINDOW];
    u32 n = s->count < BATTERY_LED_VOLT_WINDOW ? s->count : BATTERY_LED_VOLT_WINDOW;
    if (n == 0)
        return 0.0f;

    memcpy(v, s->win, n); // the first n slots are the filled ones until the window wraps
    for (u32 i = 1; i < n; i++) // insertion sort, <= 6 elements
    {
        u8 x = v[i];
        u32 j = i;
        for (; j > 0 && v[j - 1] > x; j--)
            v[j] = v[j - 1];
        v[j] = x;
    }
    return (n & 1) ? (float)v[n / 2] : ((float)v[n / 2 - 1] + (float)v[n / 2]) / 2.0f;
}

// ---- Misc ----

bool BatteryLedLogic_NearMcuEvent(float pct)
{
    static const float events[] = { 10.0f, 5.0f, 0.0f };
    for (u32 i = 0; i < sizeof(events) / sizeof(events[0]); i++)
    {
        float d = pct - events[i];
        if (d <= 0.6f && d >= -0.6f)
            return true;
    }
    return false;
}

const char *BatteryLedLogic_ModeName(u8 mode)
{
    switch (mode)
    {
        case BATTERY_LED_MODE_AUTO:         return "auto";
        case BATTERY_LED_MODE_BLUE_FORCED:  return "blue(1)";
        case BATTERY_LED_MODE_SLEEP:        return "sleep";
        case BATTERY_LED_MODE_POWER_OFF:    return "power off";
        case BATTERY_LED_MODE_RED:          return "red";
        case BATTERY_LED_MODE_BLUE:         return "blue(5)";
        case BATTERY_LED_MODE_BLINK:        return "blink red";
        case BATTERY_LED_MODE_KEEP:         return "keep";
        case BATTERY_LED_MODE_UNKNOWN:      return "none yet";
        default:                            return "other";
    }
}

const char *BatteryLedLogic_ReasonName(u8 reason)
{
    switch (reason)
    {
        case BATTERY_LED_REASON_ACTIVE:     return "active";
        case BATTERY_LED_REASON_OFF:        return "override off";
        case BATTERY_LED_REASON_NO_CURVE:   return "no curve";
        case BATTERY_LED_REASON_CHARGER:    return "charging";
        case BATTERY_LED_REASON_SHELL:      return "lid closed";
        case BATTERY_LED_REASON_SHUTDOWN:   return "shutdown";
        case BATTERY_LED_REASON_ERROR:      return "error";
        case BATTERY_LED_REASON_TEST:       return "LED test";
        default:                            return "no data";
    }
}
