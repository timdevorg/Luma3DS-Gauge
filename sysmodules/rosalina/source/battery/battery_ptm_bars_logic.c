/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: ptm battery bars patch, pure part (decision + verification). No services; compiled
*   unchanged into the host test (tests/battery/bars_host_test.c). Design: docs/battery-manager.md, section 15.
*/

#include <stddef.h>
#include "battery/battery_led_logic.h"
#include "battery/battery_ptm_bars.h"

// ptm 0004013020002202, VA 0x106f54 (Thumb), copy A (shared-page byte 0x1FF81085):
//   02 40        ands r2, r0
//   68 46        mov  r0, sp
//   22 70        strb r2, [r4]
//   00 78        ldrb r0, [r0]
//   3C 28        cmp  r0, #60         <- byte 8: 3C stock, 00 fed
//   01 D9        bls  0x106f64
//   05 21        movs r1, #5          <- byte 12: 05 stock, 01..05 fed
//   16 E0        b    0x106f92
static const u8 g_barsWindowA[BATTERY_BARS_WINDOW_LEN] = {
    0x02, 0x40, 0x68, 0x46, 0x22, 0x70, 0x00, 0x78,
    BATTERY_BARS_CMP_STOCK, 0x28, 0x01, 0xD9, BATTERY_BARS_MOVS_STOCK, 0x21, 0x16, 0xE0,
};

// ptm 0004013020002202, VA 0x104cd0 (Thumb), copy B (GetBatteryLevel):
//   01 F0 F2 FB  bl   0x1064b8
//   69 46        mov  r1, sp
//   0A 78        ldrb r2, [r1]
//   3C 2A        cmp  r2, #60         <- byte 8
//   01 D9        bls  0x104ce0
//   05 21        movs r1, #5          <- byte 12
//   04 E0        b    0x104cea
static const u8 g_barsWindowB[BATTERY_BARS_WINDOW_LEN] = {
    0x01, 0xF0, 0xF2, 0xFB, 0x69, 0x46, 0x0A, 0x78,
    BATTERY_BARS_CMP_STOCK, 0x2A, 0x01, 0xD9, BATTERY_BARS_MOVS_STOCK, 0x21, 0x04, 0xE0,
};

// ---- Decision ----

// 3..5 from the corrected % (blue LED level); extra > 0 makes it more pessimistic (hysteresis for stepping up)
static u8 batteryBarsUpper(const BatteryLedConfig *cfg, float corrected, float extra)
{
    if (corrected - extra >= (float)cfg->bars4Pct)
        return 5;
    if (corrected - extra >= (float)cfg->bars3Pct)
        return 4;
    return 3;
}

u8 BatteryBars_Decide(const BatteryLedConfig *cfg, const BatteryBarsInputs *in, u8 prev, u8 *outReason)
{
    u8 reason, level = BATTERY_BARS_STOCK;

    if (!cfg->bars)
        reason = BATTERY_BARS_REASON_OFF;
    else if (!in->haveData)
        reason = BATTERY_BARS_REASON_NO_DATA;
    else if (in->mcuError)
        reason = BATTERY_BARS_REASON_MCU_ERROR;
    else if (in->corrected < 0.0f)
        reason = BATTERY_BARS_REASON_NO_CURVE;
    else if (in->onCharger)
        reason = BATTERY_BARS_REASON_CHARGING; // the curve is for discharge; 3dbrew: level only reliable on battery
    else
    {
        reason = BATTERY_BARS_REASON_FED;

        // Red / blink steps: the LED's level state (same thresholds, "only gets worse" within a discharge, 0.5
        // hysteresis after settings changes), so the red bar and the red LED switch at the same poll. Without a
        // level yet (no decision with a curve so far): the LED's rule on the corrected % as it is.
        u8 led = in->ledLevel;
        if (!in->ledLevelValid)
            led = in->corrected <= (float)cfg->blinkPct ? BATTERY_LED_LEVEL_BLINK :
                  in->corrected <= (float)cfg->redPct ? BATTERY_LED_LEVEL_RED : BATTERY_LED_LEVEL_BLUE;

        if (led == BATTERY_LED_LEVEL_BLINK)
            level = 1;
        else if (led == BATTERY_LED_LEVEL_RED)
            level = 2;
        else
        {
            u8 fresh = batteryBarsUpper(cfg, in->corrected, 0.0f);
            level = fresh;
            if (prev >= 3 && prev <= BATTERY_BARS_LEVEL_MAX && fresh >= prev)
            {
                // Down at once; up only with BATTERY_BARS_HYSTERESIS points above the threshold
                u8 up = batteryBarsUpper(cfg, in->corrected, BATTERY_BARS_HYSTERESIS);
                level = up > prev ? up : prev;
            }
        }
    }

    if (outReason != NULL)
        *outReason = reason;
    return level;
}

bool BatteryBars_BytesFor(u8 level, u8 *cmp, u8 *movs)
{
    *cmp = BATTERY_BARS_CMP_STOCK;
    *movs = BATTERY_BARS_MOVS_STOCK;
    if (level == BATTERY_BARS_STOCK)
        return true;
    if (level < BATTERY_BARS_LEVEL_MIN || level > BATTERY_BARS_LEVEL_MAX)
        return false;
    *cmp = BATTERY_BARS_CMP_FED;
    *movs = level;
    return true;
}

// ---- Verification ----

bool BatteryBars_CmpAllowed(u8 b)
{
    return b == BATTERY_BARS_CMP_STOCK || b == BATTERY_BARS_CMP_FED;
}

bool BatteryBars_MovsAllowed(u8 b)
{
    return b >= BATTERY_BARS_LEVEL_MIN && b <= BATTERY_BARS_LEVEL_MAX;
}

static bool batteryBarsVerify(const u8 *bytes, const u8 *window)
{
    for (u32 i = 0; i < BATTERY_BARS_WINDOW_LEN; i++)
    {
        if (i == BATTERY_BARS_CMP_INDEX)
        {
            if (!BatteryBars_CmpAllowed(bytes[i]))
                return false;
        }
        else if (i == BATTERY_BARS_MOVS_INDEX)
        {
            if (!BatteryBars_MovsAllowed(bytes[i]))
                return false;
        }
        else if (bytes[i] != window[i])
            return false;
    }
    return true;
}

bool BatteryBars_VerifyA(const u8 *bytes)
{
    return batteryBarsVerify(bytes, g_barsWindowA);
}

bool BatteryBars_VerifyB(const u8 *bytes)
{
    return batteryBarsVerify(bytes, g_barsWindowB);
}

const u8 *BatteryBars_WindowA(void)
{
    return g_barsWindowA;
}

const u8 *BatteryBars_WindowB(void)
{
    return g_barsWindowB;
}

const char *BatteryBars_ReasonName(u8 reason)
{
    switch (reason)
    {
        case BATTERY_BARS_REASON_FED:       return "fed";
        case BATTERY_BARS_REASON_OFF:       return "off";
        case BATTERY_BARS_REASON_NO_DATA:   return "no data";
        case BATTERY_BARS_REASON_MCU_ERROR: return "MCU err";
        case BATTERY_BARS_REASON_NO_CURVE:  return "no curve";
        case BATTERY_BARS_REASON_CHARGING:  return "charging";
        default:                            return "?";
    }
}

const char *BatteryBars_StateName(u8 state)
{
    switch (state)
    {
        case BATTERY_BARS_STATE_UNCHECKED:      return "not checked yet";
        case BATTERY_BARS_STATE_OK:             return "ok";
        case BATTERY_BARS_STATE_NO_PTM:         return "off: no ptm";
        case BATTERY_BARS_STATE_MAP_ERROR:      return "map error";
        case BATTERY_BARS_STATE_WRITE_ERROR:    return "write error";
        case BATTERY_BARS_STATE_OFF_TITLE:      return "off: title id";
        case BATTERY_BARS_STATE_OFF_LAYOUT:     return "off: layout";
        case BATTERY_BARS_STATE_OFF_MISMATCH:   return "off: mismatch";
        default:                                return "?";
    }
}
