/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: the ONLY place battery code talks to mcu::HWC.
*/

#pragma once

#include <3ds/types.h>

// MCU registers (I2C device 3), see https://www.3dbrew.org/wiki/I2C_Registers
#define BATTERY_MCU_REG_SLIDER3D        0x08
#define BATTERY_MCU_REG_FLAGS           0x0F
#define BATTERY_MCU_REG_POWER_LED       0x29 // the only register battery code may ever write
#define BATTERY_MCU_REG_INFO_TABLE      0x7F

#define BATTERY_MCU_FLAG_SHELL_OPEN     (1u << 1)
#define BATTERY_MCU_FLAG_ADAPTER        (1u << 3)
#define BATTERY_MCU_FLAG_CHARGING       (1u << 4)

#define BATTERY_MCU_INFO_TABLE_SIZE     0x13

// Power LED modes (reg 0x29, mode byte). Only these may ever be written (BatteryMcu_SetPowerLedMode).
#define BATTERY_MCU_LED_AUTO            0 // "anything else = automatic mode" (3dbrew)
#define BATTERY_MCU_LED_BLUE_FORCED     1 // forced default blue
#define BATTERY_MCU_LED_SLEEP           2 // sleep mode animation
#define BATTERY_MCU_LED_POWER_OFF       3 // "power off" mode
#define BATTERY_MCU_LED_RED             4 // blue off, red on
#define BATTERY_MCU_LED_BLUE            5 // red off, blue on
#define BATTERY_MCU_LED_BLINK_RED       6 // animate blue off, flash red

/// Whitelist: the modes 3dbrew documents for reg 0x29 (0..6). Anything else is refused.
static inline bool BatteryMcu_LedModeAllowed(u32 mode)
{
    return mode <= BATTERY_MCU_LED_BLINK_RED;
}

typedef struct BatteryMcuSample
{
    u8 slider3d;     // 0x08
    u8 volume;       // 0x09
    u8 temperature;  // 0x0A, degrees Celsius
    u8 pctInt;       // 0x0B
    u8 pctFrac;      // 0x0C, percentage = pctInt + pctFrac / 256
    u8 voltageRaw;   // 0x0D, 3dbrew: raw * 5 / 256 V (Rosalina: raw * 0.02 V)
    u8 flags;        // 0x0F
    bool infoValid;  // info[] could be read
    u8 info[BATTERY_MCU_INFO_TABLE_SIZE]; // 0x7F: [2] battery vendor, [3..4] MGIC version, [5] RCOMP, [0x0A] red LED, [0x0B] blue LED
    bool ledModeValid; // ledMode could be read
    u8 ledMode;      // 0x29, first (mode) byte. A "dynamic" register: may not read back what was written
} BatteryMcuSample;

/// Must be called once before any other function (from main(), before threads start).
void BatteryMcu_Init(void);

/// Reads 0x08..0x0F (one read), the 0x29 mode byte and the 0x7F info table using one short-lived session.
Result BatteryMcu_ReadSample(BatteryMcuSample *out);

/// Generic read. Refuses ranges touching 0x05-0x07 (unlock sequence) or 0x10-0x13 (interrupt bitmask).
Result BatteryMcu_ReadRegister(u8 reg, void *data, u32 size);

/// The ONLY MCU write in the Battery Manager: writes the 1-byte mode of reg 0x29 (power LED).
/// Refuses any mode not in BatteryMcu_LedModeAllowed(). The 4 red-pattern bytes are never written.
/// Callers: battery_led.c only.
Result BatteryMcu_SetPowerLedMode(u8 mode);

/// Converts the raw voltage byte to millivolts (3dbrew scale, 5/256 V per unit). Used by the CSV (`volt`) and
/// Diagnostics. Which scale is right is an open question (docs/battery-manager.md, 16.13).
static inline u32 BatteryMcu_VoltageMv(u8 raw)
{
    return ((u32)raw * 5000u + 128u) / 256u;
}

/// Converts the raw voltage byte to centivolts in Rosalina's scale (0.02 V per unit, upstream menu footer): the one
/// scale on user screens (footer, Battery Manager home). Exact in integers (raw x 2), no float rounding.
static inline u32 BatteryMcu_VoltageCv(u8 raw)
{
    return (u32)raw * 2u;
}

/// Percentage in tenths of a percent (rounded).
static inline u32 BatteryMcu_PercentX10(u8 pctInt, u8 pctFrac)
{
    return (u32)pctInt * 10u + ((u32)pctFrac * 10u + 128u) / 256u;
}
