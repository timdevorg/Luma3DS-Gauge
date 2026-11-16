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
#include "battery/battery_ptm_bars.h"

// ---- Settings ----

void BatteryLedConfig_Defaults(BatteryLedConfig *cfg)
{
    cfg->override = false;
    cfg->redPct = BATTERY_LED_DEFAULT_RED_PCT;
    cfg->blinkPct = BATTERY_LED_DEFAULT_BLINK_PCT;
    cfg->blueMode = BATTERY_LED_DEFAULT_BLUE_MODE;
    cfg->sleepMode = BATTERY_LED_DEFAULT_SLEEP_MODE;
    cfg->bars = BATTERY_BARS_DEFAULT_ON;
    cfg->bars4Pct = BATTERY_BARS_DEFAULT_4;
    cfg->bars3Pct = BATTERY_BARS_DEFAULT_3;
}

static bool batteryLedSleepSettingValid(u8 m)
{
    return m == BATTERY_LED_MODE_BLUE || m == BATTERY_LED_MODE_BLUE_FORCED || m == BATTERY_LED_SLEEP_SYSTEM;
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
    if (cfg->sleepMode == BATTERY_LED_MODE_SLEEP)
        cfg->sleepMode = BATTERY_LED_SLEEP_SYSTEM; // legacy anim(2): same stock ptm behaviour as off
    else if (!batteryLedSleepSettingValid(cfg->sleepMode))
        cfg->sleepMode = BATTERY_LED_DEFAULT_SLEEP_MODE;

    // Bars thresholds (section 15): bars_4 > bars_3 > red_pct, bars_4 <= 100; else the defaults. A red_pct of 40 or
    // more (max 50) makes the default bars_3 invalid too: then bars_3 just above red_pct
    if (!(cfg->bars4Pct <= BATTERY_BARS_MAX_PCT && cfg->bars4Pct > cfg->bars3Pct && cfg->bars3Pct > cfg->redPct))
    {
        cfg->bars4Pct = BATTERY_BARS_DEFAULT_4;
        cfg->bars3Pct = BATTERY_BARS_DEFAULT_3;
        if (cfg->bars3Pct <= cfg->redPct)
            cfg->bars3Pct = cfg->redPct + 1;
        if (cfg->bars4Pct <= cfg->bars3Pct)
            cfg->bars4Pct = cfg->bars3Pct + 1;
    }
}

bool BatteryLedConfig_Parse(const char *buf, BatteryLedConfig *cfg)
{
    bool any = false;
    for (const char *p = buf; *p != 0;)
    {
        const char *eq = p;
        while (*eq != 0 && *eq != '=' && *eq != '\n')
            eq++;
        u32 wordLen = 0;
        if (*eq == '=' && (u32)(eq - p) == 10 && strncmp(p, "sleep_mode", 10) == 0)
        {
            if (strncmp(eq + 1, "system", 6) == 0)
                wordLen = 6;
            else if (strncmp(eq + 1, "anim", 4) == 0)
                wordLen = 4;
            char t = wordLen != 0 ? eq[1 + wordLen] : 'x';
            if (!(t == 0 || t == '\n' || t == '\r' || t == ' ' || t == '#'))
                wordLen = 0;
        }
        if (wordLen != 0)
        {
            cfg->sleepMode = BATTERY_LED_SLEEP_SYSTEM; // legacy words "system" / "anim": off (stock ptm)
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
            else if (keyLen == 4 && strncmp(p, "bars", 4) == 0)
            {
                cfg->bars = v != 0;
                any = true;
            }
            else if (keyLen == 6 && strncmp(p, "bars_4", 6) == 0)
            {
                cfg->bars4Pct = v8; // checked together with bars_3 / red_pct in BatteryLedConfig_Sanitize
                any = true;
            }
            else if (keyLen == 6 && strncmp(p, "bars_3", 6) == 0)
            {
                cfg->bars3Pct = v8;
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
    l->chargerOffCount = 0;
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
    // Blue: the Sleep LED setting. Default dim (1): ptm's sleep fade dims it; bright (5): full blue.
    // off (0): stock ptm, nothing is written.
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

u8 BatteryLedLogic_SleepLevel(const BatteryLedConfig *cfg, u8 level, bool charger)
{
    // Charging at sleep entry: blue while asleep with Sleep LED dim / bright, whatever the level (the ptm
    // sleep byte follows the same rule, section 14). off: the level as it is.
    if (charger && (cfg->sleepMode == BATTERY_LED_MODE_BLUE || cfg->sleepMode == BATTERY_LED_MODE_BLUE_FORCED))
        return BATTERY_LED_LEVEL_BLUE;
    return level;
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
        case BATTERY_LED_MODE_BLUE:         return "bright blue";
        case BATTERY_LED_MODE_BLUE_FORCED:  return "dim blue";
        case BATTERY_LED_SLEEP_SYSTEM:      return "off (stock)";
        default:                            return "other";
    }
}

u8 BatteryLedLogic_NextSleepSetting(u8 sleepMode)
{
    switch (sleepMode)
    {
        case BATTERY_LED_MODE_BLUE_FORCED:  return BATTERY_LED_MODE_BLUE;      // dim -> bright
        case BATTERY_LED_MODE_BLUE:         return BATTERY_LED_SLEEP_SYSTEM;   // bright -> off
        default:                            return BATTERY_LED_MODE_BLUE_FORCED; // off -> dim
    }
}

bool BatteryLedConfig_Step(BatteryLedConfig *cfg, u8 setting, s32 dir)
{
    bool inc = dir > 0;
    if (dir == 0)
        return false;

    switch (setting)
    {
        case BATTERY_SETTING_POWER_LED:
            cfg->override = !cfg->override;
            return true;
        case BATTERY_SETTING_SLEEP_LED:
            // Three values: two steps forward are one step back
            cfg->sleepMode = BatteryLedLogic_NextSleepSetting(cfg->sleepMode);
            if (!inc)
                cfg->sleepMode = BatteryLedLogic_NextSleepSetting(cfg->sleepMode);
            return true;
        case BATTERY_SETTING_BARS:
            cfg->bars = !cfg->bars;
            return true;
        case BATTERY_SETTING_BLUE_MODE:
            cfg->blueMode = cfg->blueMode == BATTERY_LED_MODE_BLUE ? BATTERY_LED_MODE_BLUE_FORCED : BATTERY_LED_MODE_BLUE;
            return true;
        case BATTERY_SETTING_RED:
            if (inc && cfg->redPct < BATTERY_LED_MAX_RED_PCT && cfg->redPct + 1 < cfg->bars3Pct)
                cfg->redPct++;
            else if (!inc && cfg->redPct > cfg->blinkPct + 1)
                cfg->redPct--;
            else
                return false;
            return true;
        case BATTERY_SETTING_BLINK:
            if (inc && cfg->blinkPct + 1 < cfg->redPct)
                cfg->blinkPct++;
            else if (!inc && cfg->blinkPct > 0)
                cfg->blinkPct--;
            else
                return false;
            return true;
        case BATTERY_SETTING_BARS4:
            if (inc && cfg->bars4Pct < BATTERY_BARS_MAX_PCT)
                cfg->bars4Pct++;
            else if (!inc && cfg->bars4Pct > cfg->bars3Pct + 1)
                cfg->bars4Pct--;
            else
                return false;
            return true;
        case BATTERY_SETTING_BARS3:
            if (inc && cfg->bars3Pct + 1 < cfg->bars4Pct)
                cfg->bars3Pct++;
            else if (!inc && cfg->bars3Pct > cfg->redPct + 1)
                cfg->bars3Pct--;
            else
                return false;
            return true;
        default:
            return false;
    }
}

bool BatteryLedLogic_OwnsLed(u8 lastWritten)
{
    return lastWritten != BATTERY_LED_MODE_AUTO && lastWritten != BATTERY_LED_MODE_UNKNOWN &&
           lastWritten != BATTERY_LED_MODE_POWER_OFF;
}

bool BatteryLedLogic_ReadFailHandBack(u8 readFailStreak, u8 lastWritten, bool sleepPending, bool handedBack)
{
    return readFailStreak >= BATTERY_LED_READ_FAIL_HANDBACK && BatteryLedLogic_OwnsLed(lastWritten) && !sleepPending &&
           !handedBack;
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

// On the adapter: the level only gets better (red -> blue), with hysteresis, never worse. Awake on the charger
// the LED is blue whatever this level is; it is only used for the lid-closed mode with Sleep LED off (red kept)
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
    // Debounced charger state: plugged in counts at once; unplugged only after BATTERY_LED_UNPLUG_DECISIONS
    // decisions in a row with the adapter and the charging flag both clear, so a flickering charging flag can't
    // toggle the LED blue <-> red. On the debounced unplug a new discharge starts from the corrected % at that
    // moment (the "only gets worse" ratchet restarts there, in either direction).
    // Only decisions on a new MCU sample count (C2): a stale one (failed read, sleep hook) would repeat old flags
    // and could end the debounce without a real second "unplugged" sample.
    if (!in->stale)
    {
        if (in->adapter || in->charging)
        {
            l->onCharger = true;
            l->chargerOffCount = 0;
        }
        else if (l->onCharger && ++l->chargerOffCount >= BATTERY_LED_UNPLUG_DECISIONS)
        {
            l->onCharger = false;
            l->chargerOffCount = 0;
            l->levelValid = false;
        }
    }
    bool charger = l->onCharger;

    // Level state: kept up to date whenever there is a curve (also with the override off or in the error state),
    // because the ptm bars patch (section 15) takes its red / blink steps from it. The LED decision below is
    // unchanged; switching the override on re-evaluates the level with hysteresis as before.
    if (!in->shuttingDown && in->corrected >= 0.0f)
    {
        if (charger)
            batteryLedChargeLevel(l, cfg, in);
        else
            batteryLedDischargeLevel(l, cfg, in->corrected);
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
        if (in->shellClosed)
        {
            reason = BATTERY_LED_REASON_SHELL;
            // Never the first write of a boot, and never "automatic" (that would show the MCU's own
            // low-battery blinking instead of a sleep indication)
            if (in->lastWritten == BATTERY_LED_MODE_UNKNOWN || in->lastWritten == BATTERY_LED_MODE_AUTO)
                mode = BATTERY_LED_MODE_KEEP;
            else
                mode = BatteryLedLogic_SleepModeForLevel(cfg, BatteryLedLogic_SleepLevel(cfg, l->level, charger));
        }
        else
        {
            // Charging: always blue_mode, at any level (also matches the ptm wake byte after a sleep on the charger)
            reason = charger ? BATTERY_LED_REASON_CHARGER : BATTERY_LED_REASON_ACTIVE;
            mode = charger ? cfg->blueMode : BatteryLedLogic_ModeForLevel(cfg, l->level);
        }
    }

    if (outReason != NULL)
        *outReason = reason;
    return mode;
}

u8 BatteryLedLogic_ThreadAction(u8 desired, u8 lastWritten, u8 reason, bool sleepPending)
{
    if (desired == BATTERY_LED_MODE_KEEP || desired == BATTERY_LED_MODE_AUTO)
        return BATTERY_LED_ACTION_NONE; // KEEP: nothing; automatic has its own path in battery_led.c
    // Sleep requested (0x101 until wake / denied): the sleep hook has written the sleep mode and ptm's sleep LED
    // callback sends its (patched) byte right before sleep. The thread writes nothing at all then.
    if (sleepPending)
        return BATTERY_LED_ACTION_NONE;
    if (desired != lastWritten)
        return BATTERY_LED_ACTION_WRITE;
    // Lid closed: no mismatch re-assert. ptm's byte (02 at the blink level) or the MCU's own sleep indication
    // may legitimately differ from our last write until wake; re-asserting would fight it (section 14).
    if (reason == BATTERY_LED_REASON_SHELL)
        return BATTERY_LED_ACTION_NONE;
    return BATTERY_LED_ACTION_CHECK;
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
