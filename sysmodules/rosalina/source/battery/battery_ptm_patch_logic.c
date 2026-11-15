/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: ptm sleep / wake LED patch, pure part (decision + verification). No services; compiled
*   unchanged into the host test (tests/battery/ptm_host_test.c). Design: docs/battery-manager.md, section 14.
*/

#include <stddef.h>
#include "battery/battery_led_logic.h"
#include "battery/battery_ptm_patch.h"

// ptm 0004013020002202, VA 0x101a1e (Thumb), sleep LED callback:
//   32 21        movs r1, #0x32       brightness 50 (stock, never changed)
//   10 B5        push {r4, lr}
//   00 F0 9B FD  bl   0x10255c        mcu::RTC SetLedBrightness
//   04 48        ldr  r0, =0x10e290
//   02 21        movs r1, #2          <- byte 10: sleep power LED mode
//   00 F0 E1 FC  bl   0x1023f0        mcu::RTC SetPowerLedState
static const u8 g_ptmSleepPattern[BATTERY_PTM_PATTERN_LEN] = {
    0x32, 0x21, 0x10, 0xB5, 0x00, 0xF0, 0x9B, 0xFD,
    0x04, 0x48, BATTERY_PTM_SLEEP_STOCK, 0x21, 0x00, 0xF0, 0xE1, 0xFC,
};

// ptm 0004013020002202, VA 0x101994 (Thumb), wake LED callback:
//   FF 21        movs r1, #0xff       brightness 255 (stock, never changed)
//   86 B0        sub  sp, #24
//   00 F0 E0 FD  bl   0x10255c        mcu::RTC SetLedBrightness
//   1C 48        ldr  r0, =0x10e290
//   00 21        movs r1, #0          <- byte 10: wake power LED mode
//   00 F0 26 FD  bl   0x1023f0        mcu::RTC SetPowerLedState
static const u8 g_ptmWakePattern[BATTERY_PTM_PATTERN_LEN] = {
    0xFF, 0x21, 0x86, 0xB0, 0x00, 0xF0, 0xE0, 0xFD,
    0x1C, 0x48, BATTERY_PTM_WAKE_STOCK, 0x21, 0x00, 0xF0, 0x26, 0xFD,
};

bool BatteryPtmPatch_SleepByteAllowed(u8 b)
{
    return b == BATTERY_LED_MODE_BLUE_FORCED || b == BATTERY_PTM_SLEEP_STOCK || b == BATTERY_LED_MODE_RED ||
           b == BATTERY_LED_MODE_BLUE;
}

bool BatteryPtmPatch_WakeByteAllowed(u8 b)
{
    return b == BATTERY_PTM_WAKE_STOCK || b == BATTERY_LED_MODE_BLUE_FORCED || b == BATTERY_LED_MODE_RED ||
           b == BATTERY_LED_MODE_BLUE;
}

void BatteryPtmPatch_DesiredBytes(const BatteryLedConfig *cfg, u8 sleepDesired, u8 sleepReason, u8 *outSleep, u8 *outWake)
{
    u8 s = BATTERY_PTM_SLEEP_STOCK, w = BATTERY_PTM_WAKE_STOCK;

    // Override off, no curve, error, shutdown, no data, LED test (reason not "lid closed"), or Sleep LED off:
    // ptm stays stock in both callbacks
    bool sleepBlue = cfg->sleepMode == BATTERY_LED_MODE_BLUE || cfg->sleepMode == BATTERY_LED_MODE_BLUE_FORCED;
    if (sleepReason == BATTERY_LED_REASON_SHELL && sleepBlue)
    {
        if (sleepDesired == BATTERY_LED_MODE_BLUE || sleepDesired == BATTERY_LED_MODE_BLUE_FORCED)
        {
            // Blue level, or charging at sleep entry: the Sleep LED setting while asleep, the awake blue mode on wake
            s = sleepDesired;
            w = cfg->blueMode == BATTERY_LED_MODE_BLUE_FORCED ? BATTERY_LED_MODE_BLUE_FORCED : BATTERY_LED_MODE_BLUE;
        }
        else if (sleepDesired == BATTERY_LED_MODE_RED)
        {
            // Red level: solid red without the MCU's own battery check (no red blinking at sleep entry)
            s = BATTERY_LED_MODE_RED;
            w = BATTERY_LED_MODE_RED;
        }
        // Blinking (6): stock, the MCU's genuine low warning; KEEP (nothing written this boot): stock
    }

    *outSleep = s;
    *outWake = w;
}

static bool batteryPtmVerify(const u8 *bytes, const u8 *pattern, bool (*allowed)(u8))
{
    for (u32 i = 0; i < BATTERY_PTM_PATTERN_LEN; i++)
    {
        if (i == BATTERY_PTM_MODE_INDEX)
        {
            if (!allowed(bytes[i]))
                return false;
        }
        else if (bytes[i] != pattern[i])
            return false;
    }
    return true;
}

bool BatteryPtmPatch_VerifySleep(const u8 *bytes)
{
    return batteryPtmVerify(bytes, g_ptmSleepPattern, BatteryPtmPatch_SleepByteAllowed);
}

bool BatteryPtmPatch_VerifyWake(const u8 *bytes)
{
    return batteryPtmVerify(bytes, g_ptmWakePattern, BatteryPtmPatch_WakeByteAllowed);
}

const u8 *BatteryPtmPatch_SleepPattern(void)
{
    return g_ptmSleepPattern;
}

const u8 *BatteryPtmPatch_WakePattern(void)
{
    return g_ptmWakePattern;
}

const char *BatteryPtmPatch_StateName(u8 state, u8 sleepCur, u8 wakeCur)
{
    switch (state)
    {
        case BATTERY_PTM_STATE_UNCHECKED:       return "not checked yet";
        case BATTERY_PTM_STATE_OK:
            return sleepCur == BATTERY_PTM_SLEEP_STOCK && wakeCur == BATTERY_PTM_WAKE_STOCK ? "stock" : "active";
        case BATTERY_PTM_STATE_NO_PTM:          return "off: no ptm";
        case BATTERY_PTM_STATE_MAP_ERROR:       return "map error";
        case BATTERY_PTM_STATE_WRITE_ERROR:     return "write error";
        case BATTERY_PTM_STATE_OFF_TITLE:       return "off: title id";
        case BATTERY_PTM_STATE_OFF_LAYOUT:      return "off: layout";
        case BATTERY_PTM_STATE_OFF_MISMATCH:    return "off: mismatch";
        default:                                return "?";
    }
}
