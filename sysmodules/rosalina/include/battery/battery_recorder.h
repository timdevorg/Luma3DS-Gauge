/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: background discharge recorder (milestone 2). Reads the MCU only.
*   CSV v2 (milestone 4) adds the LED columns.
*/

#pragma once

#include <3ds/types.h>
#include "MyThread.h"

#define BATTERY_SAMPLE_INTERVAL_MS  10000u
#define BATTERY_FLUSH_EVERY_SAMPLES 6u       // ~1 min of data lost at most on a hard power cut
#define BATTERY_SLEEP_GAP_MS        60000u   // larger gap between two samples => "# sleep gap=<ms>"

typedef enum BatteryRecorderError
{
    BATTERY_REC_OK = 0,
    BATTERY_REC_ALREADY_RECORDING,
    BATTERY_REC_NOT_RECORDING,
    BATTERY_REC_CHARGER_PLUGGED,
    BATTERY_REC_MCU_READ_FAILED,
    BATTERY_REC_SD_FAILED,
    BATTERY_REC_NO_FREE_NUMBER,
    BATTERY_REC_SHUTTING_DOWN,
} BatteryRecorderError;

typedef struct BatteryRecorderStatus
{
    bool recording;
    bool sleepGap;
    u16 number;          // current (or last) session
    u32 samples;
    u32 elapsedMs;       // monotonic, since start
    u32 bufferedLines;   // not yet written to SD
    u32 readErrors;
    u32 writeErrors;
    Result lastWriteRes;
    u16 recoveredNumber; // session closed as "cutoff" at boot, 0 if none
    u32 reassertBase;    // LED re-assert counter (per boot) at the start of the session
} BatteryRecorderStatus;

/// Creates the recorder thread (call once from main()).
MyThread *BatteryRecorder_CreateThread(void);

BatteryRecorderError BatteryRecorder_Start(u16 *outNumber, Result *outRes);
BatteryRecorderError BatteryRecorder_Stop(u16 *outNumber);
void BatteryRecorder_GetStatus(BatteryRecorderStatus *out);
const char *BatteryRecorder_ErrorString(BatteryRecorderError err);

/// Monotonic milliseconds from svcGetSystemTick().
u64 BatteryRecorder_TicksToMs(u64 ticks);
