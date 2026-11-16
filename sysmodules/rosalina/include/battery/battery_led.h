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
#include "battery/battery_eta_logic.h"

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
    u64 startMs;            // boot timing, ms since the ARM11 tick counter started (~power-on):
    u64 sdReadyMs;          //   LED thread created / SD usable / first MCU read / first write (0 = not yet)
    u64 firstDataMs;
    u64 firstWriteMs;
    u32 sleepCount;         // sleep diagnostics: sleep entries this boot
    u8 sleepWrote;          //   mode written at the last sleep entry, BATTERY_LED_MODE_UNKNOWN = nothing
    u8 wakeRead;            //   0x29 read on wake before any write, BATTERY_LED_MODE_UNKNOWN = read failed
    bool wakePending;       //   still asleep / lid closed: no wake read yet
    u8 barsLevel;           // ptm battery bars (section 15): decided level 1..5, 0 = stock
    u8 barsReason;          //   BatteryBarsReason
    u8 etaState;            // time remaining (section 17): BatteryEtaState
    u16 etaMinutes;         //   rounded minutes, valid in BATTERY_ETA_OK / BATTERY_ETA_PROVISIONAL
    BatteryEtaDiag eta;     //   details for Diagnostics 4/4
} BatteryLedStatus;

/// Creates the LED thread (call once from the recorder's creation, before the service manager runs).
MyThread *BatteryLed_CreateThread(void);

/// Rosalina notification hook (main.c): shell open/close (0x213/0x214), sleep (0x101/0x102/0x106),
/// battery low (0x211/0x212), shutdown (0x108) and pre-termination (0x2000). Runs in the
/// notification thread; synchronously writes the Sleep LED setting before sleep (if the LED is blue and
/// ours; nothing for "system") and "power off" once at the first shutdown signal (if the LED is ours), then nothing more.
/// At 0x214 / 0x101 / 0x104 also sets ptm's sleep and wake LED bytes (battery_ptm_patch.h) before main.c acks.
void BatteryLed_HandleNotification(u32 notificationId);

void BatteryLed_GetStatus(BatteryLedStatus *out);
/// Loads config.ini if that hasn't happened yet (menu thread).
void BatteryLed_EnsureConfigLoaded(void);
/// The saved settings (also ones saved during a sleep transition that aren't active yet).
void BatteryLed_GetConfig(BatteryLedConfig *out);
/// Applies (sanitized) settings, saves config.ini and wakes the thread. Clears a control error. Menu thread.
/// With the lid closed or sleep pending (the menu was closed by it) the settings are saved now but applied by the
/// thread only once the console is awake again. On a save error the settings are active anyway (until reboot).
Result BatteryLed_SetConfig(const BatteryLedConfig *cfg);

/// LED test screen: the thread stops controlling the LED until BatteryLed_TestEnd().
void BatteryLed_TestBegin(void);
/// Writes a whitelisted mode (0..6) while the test is active.
Result BatteryLed_TestSetMode(u8 mode);
/// Gives control back; the thread re-applies the normal mode (or automatic) right away.
void BatteryLed_TestEnd(void);
