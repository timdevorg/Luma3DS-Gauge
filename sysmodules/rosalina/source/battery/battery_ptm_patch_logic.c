/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: ptm sleep LED patch, pure part (decision + verification). No services; compiled
*   unchanged into the host test (tests/battery/ptm_host_test.c). Design: docs/battery-manager.md, section 14.
*/

#include <stddef.h>
#include "battery/battery_led_logic.h"
#include "battery/battery_ptm_patch.h"

// ptm 0004013020002202, VA 0x101a1e (Thumb):
//   32 21        movs r1, #0x32       brightness 50 (stock, never changed)
//   10 B5        push {r4, lr}
//   00 F0 9B FD  bl   0x10255c        mcu::RTC SetLedBrightness
//   04 48        ldr  r0, =0x10e290
//   02 21        movs r1, #2          <- byte 10: power LED mode
//   00 F0 E1 FC  bl   0x1023f0        mcu::RTC SetPowerLedState
static const u8 g_ptmPattern[BATTERY_PTM_PATTERN_LEN] = {
    0x32, 0x21, 0x10, 0xB5, 0x00, 0xF0, 0x9B, 0xFD,
    0x04, 0x48, BATTERY_PTM_BYTE_STOCK, 0x21, 0x00, 0xF0, 0xE1, 0xFC,
};

bool BatteryPtmPatch_ByteAllowed(u8 b)
{
    return b == BATTERY_PTM_BYTE_STOCK || b == BATTERY_PTM_BYTE_BLUE_FORCED || b == BATTERY_PTM_BYTE_BLUE;
}

u8 BatteryPtmPatch_DesiredByte(u8 sleepDesired, u8 sleepReason)
{
    if (sleepReason != BATTERY_LED_REASON_SHELL)
        return BATTERY_PTM_BYTE_STOCK; // override off, no curve, error, shutdown, no data, LED test
    if (sleepDesired == BATTERY_LED_MODE_BLUE)
        return BATTERY_PTM_BYTE_BLUE;
    if (sleepDesired == BATTERY_LED_MODE_BLUE_FORCED)
        return BATTERY_PTM_BYTE_BLUE_FORCED;
    return BATTERY_PTM_BYTE_STOCK;     // red / blinking (MCU's own low warning), anim(2), system, nothing written yet
}

bool BatteryPtmPatch_Verify(const u8 *bytes)
{
    for (u32 i = 0; i < BATTERY_PTM_PATTERN_LEN; i++)
    {
        if (i == BATTERY_PTM_MODE_INDEX)
        {
            if (!BatteryPtmPatch_ByteAllowed(bytes[i]))
                return false;
        }
        else if (bytes[i] != g_ptmPattern[i])
            return false;
    }
    return true;
}

const u8 *BatteryPtmPatch_Pattern(void)
{
    return g_ptmPattern;
}

const char *BatteryPtmPatch_StateName(u8 state, u8 current)
{
    switch (state)
    {
        case BATTERY_PTM_STATE_UNCHECKED:       return "not checked yet";
        case BATTERY_PTM_STATE_OK:              return current == BATTERY_PTM_BYTE_STOCK ? "stock" : "active";
        case BATTERY_PTM_STATE_NO_PTM:          return "off: no ptm";
        case BATTERY_PTM_STATE_MAP_ERROR:       return "map error";
        case BATTERY_PTM_STATE_WRITE_ERROR:     return "write error";
        case BATTERY_PTM_STATE_OFF_TITLE:       return "off: title id";
        case BATTERY_PTM_STATE_OFF_LAYOUT:      return "off: layout";
        case BATTERY_PTM_STATE_OFF_MISMATCH:    return "off: mismatch";
        default:                                return "?";
    }
}
