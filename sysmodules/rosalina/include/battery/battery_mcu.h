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
    u8 info[BATTERY_MCU_INFO_TABLE_SIZE]; // 0x7F: [2] battery vendor, [3..4] MGIC version, [5] RCOMP, [0x0A] red LED
} BatteryMcuSample;

/// Must be called once before any other function (from main(), before threads start).
void BatteryMcu_Init(void);

/// Reads 0x08..0x0F (one read) and the 0x7F info table (one read) using a short-lived session.
Result BatteryMcu_ReadSample(BatteryMcuSample *out);

/// Generic read. Refuses ranges touching 0x05-0x07 (unlock sequence) or 0x10-0x13 (interrupt bitmask).
Result BatteryMcu_ReadRegister(u8 reg, void *data, u32 size);

/// Generic write, whitelisted: refuses anything but a 1-byte write to 0x29 (power LED mode).
/// NOT called anywhere yet (milestone 2 performs no MCU writes).
Result BatteryMcu_WriteRegister(u8 reg, const void *data, u32 size);

/// Converts the raw voltage byte to millivolts (3dbrew scale, 5/256 V per unit).
static inline u32 BatteryMcu_VoltageMv(u8 raw)
{
    return ((u32)raw * 5000u + 128u) / 256u;
}

/// Percentage in tenths of a percent (rounded).
static inline u32 BatteryMcu_PercentX10(u8 pctInt, u8 pctFrac)
{
    return (u32)pctInt * 10u + ((u32)pctFrac * 10u + 128u) / 256u;
}
