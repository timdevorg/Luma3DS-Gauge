/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: power LED control thread (milestone 4).
*
*   Polls the MCU, decides the LED mode (battery_led_logic.c) from the corrected % and writes
*   reg 0x29 through BatteryMcu_SetPowerLedMode(), the only MCU write in the Battery Manager.
*   Settings are kept in /luma/battery/config.ini. Design: docs/battery-manager.md, section 12.
*/

#pragma once

#include <3ds/types.h>
#include "MyThread.h"
#include "battery/battery_led_logic.h"

#define BATTERY_CONFIG_PATH         "/luma/battery/config.ini"
#define BATTERY_CONFIG_TMP_PATH     "/luma/battery/config.tmp"

typedef enum BatteryLedError
{
    BATTERY_LED_ERR_NONE = 0,
    BATTERY_LED_ERR_WRITE_FAILED,   // several writes in a row failed
    BATTERY_LED_ERR_MCU_OVERRIDES,  // too many re-asserts in a short time
} BatteryLedError;

typedef struct BatteryLedStatus
{
    BatteryLedConfig cfg;
    bool haveData;          // at least one MCU poll succeeded
    float pct;              // MCU %, last poll
    float voltSmoothed;     // median voltage (raw units)
    float corrected;        // from voltSmoothed, < 0 if no curve
    u8 flags;               // 0x0F, last poll
    u8 desired;             // BATTERY_LED_MODE_*
    u8 reason;              // BatteryLedReason
    u8 level;               // BatteryLedLevel of this discharge
    u8 lastWritten;         // BATTERY_LED_MODE_*, UNKNOWN if nothing written this boot
    u8 readback;            // BatteryLedReadback
    BatteryLedObserved obs; // last poll
    u32 reasserts;          // re-applies after a mismatch, this boot
    u32 writes;             // successful writes, this boot
    u32 writeFailures;      // this boot
    Result lastWriteRes;
    bool haveMismatch;
    float lastMismatchPct;  // MCU % at the last mismatch
    u8 error;               // BatteryLedError
    bool fastPolling;
} BatteryLedStatus;

/// Creates the LED thread (call once from the recorder's creation, before the service manager runs).
MyThread *BatteryLed_CreateThread(void);

/// Rosalina notification hook (main.c): shell open/close (0x213/0x214), sleep (0x101/0x102/0x106),
/// battery low (0x211/0x212), shutdown (0x108) and pre-termination (0x2000). Runs in the
/// notification thread; writes "automatic" synchronously before sleep / shutdown when needed.
void BatteryLed_HandleNotification(u32 notificationId);

void BatteryLed_GetStatus(BatteryLedStatus *out);
/// Loads config.ini if that hasn't happened yet (menu thread).
void BatteryLed_EnsureConfigLoaded(void);
void BatteryLed_GetConfig(BatteryLedConfig *out);
/// Applies (sanitized) settings, saves config.ini and wakes the thread. Clears a control error. Menu thread.
Result BatteryLed_SetConfig(const BatteryLedConfig *cfg);

/// LED test screen: the thread stops controlling the LED until BatteryLed_TestEnd().
void BatteryLed_TestBegin(void);
/// Writes a whitelisted mode (0/1/4/5/6) while the test is active.
Result BatteryLed_TestSetMode(u8 mode);
/// Gives control back; the thread re-applies the normal mode (or automatic) right away.
void BatteryLed_TestEnd(void);
