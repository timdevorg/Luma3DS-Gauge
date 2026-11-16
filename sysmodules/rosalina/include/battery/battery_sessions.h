/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: SD file helpers and recorded-session parsing / classification.
*   All files live on the SD card (ARCHIVE_SDMC) under /luma/battery/.
*/

#pragma once

#include <3ds/types.h>

#define BATTERY_DIR             "/luma/battery"
#define BATTERY_SESSIONS_DIR    "/luma/battery/sessions"
#define BATTERY_STATE_PATH      "/luma/battery/state"

#define BATTERY_MAX_SESSION_NUMBER  9999
#define BATTERY_SESSION_COMPLETE_START_X10  950 // "complete" needs a first sample at 95.0 % or more
#define BATTERY_MAX_LISTED_SESSIONS 64

typedef enum BatterySessionClass
{
    BATTERY_SESSION_INCOMPLETE = 0,
    BATTERY_SESSION_COMPLETE,
    BATTERY_SESSION_INVALID,
} BatterySessionClass;

typedef enum BatteryEndReason
{
    BATTERY_END_NONE = 0,   // no end line (still recording, or cut off and not yet recovered)
    BATTERY_END_STOP,       // user stopped it
    BATTERY_END_CHARGER,    // charger plugged in
    BATTERY_END_SHUTDOWN,   // Rosalina got the pre-termination (shutdown/reboot) notification
    BATTERY_END_CUTOFF,     // power was lost without notice (detected on next boot via the state file)
    BATTERY_END_OTHER,
} BatteryEndReason;

typedef struct BatterySessionInfo
{
    u16 number;
    u8 cls;          // BatterySessionClass
    u8 endReason;    // BatteryEndReason
    bool hasSleepGap;
    bool hasData;
    u16 startPctX10; // first sample, tenths of a percent
    u16 endPctX10;   // last sample
    u32 samples;
    u32 durationMs;  // t_ms of the last sample
    u32 sizeBytes;
} BatterySessionInfo;

/// Path of session NNNN. out must hold at least 40 chars.
void BatterySessions_MakePath(char *out, u32 number);

/// Creates /luma/battery and /luma/battery/sessions if missing.
Result BatteryFs_EnsureDirs(void);
/// True once the SD card archive can be opened (boot readiness check).
bool BatteryFs_SdReady(void);
/// Appends len bytes at the end of the file (created if missing), flushed, then closes it.
Result BatteryFs_Append(const char *path, const void *data, u32 len);
/// Replaces the whole file content.
Result BatteryFs_WriteWhole(const char *path, const void *data, u32 len);
/// Power-cut safe replace: writes tmpPath (flushed), deletes path, renames tmpPath to path.
/// Readers should fall back to tmpPath if path is missing or invalid.
Result BatteryFs_WriteSafely(const char *path, const char *tmpPath, const void *data, u32 len);
/// Same, but keeps the old file: writes tmpPath (flushed), deletes bakPath, renames path to bakPath (if path can't be
/// renamed, e.g. it doesn't exist, it is deleted as in WriteSafely), renames tmpPath to path. Nothing is touched if
/// writing tmpPath fails. At any power cut there is a valid path or a valid tmpPath (like WriteSafely).
Result BatteryFs_WriteSafelyKeepOld(const char *path, const char *tmpPath, const char *bakPath, const void *data, u32 len);
/// Reads up to len bytes ending at the end of the file (len bytes from offset size-len). Returns bytes read in *outLen.
Result BatteryFs_ReadTail(const char *path, void *data, u32 len, u32 *outLen);
/// Reads up to len bytes from the start of the file.
Result BatteryFs_ReadHead(const char *path, void *data, u32 len, u32 *outLen);

/// Next free session number (max existing + 1), or 0 if none is left / the directory can't be read.
u32 BatterySessions_NextNumber(void);

/// Lists (up to BATTERY_MAX_LISTED_SESSIONS of the highest-numbered) sessions, ascending,
/// and parses / classifies each of them. Not reentrant (static buffers): menu thread only.
Result BatterySessions_Scan(BatterySessionInfo *out, u32 maxCount, u32 *outCount);

/// Parses / classifies one session file. Not reentrant: menu thread only.
Result BatterySessions_Parse(u32 number, BatterySessionInfo *out);

Result BatterySessions_Delete(u32 number);

const char *BatterySessions_ClassName(u8 cls);

typedef enum BatteryStartCheck
{
    BATTERY_START_OK = 0,       // can become a complete session
    BATTERY_START_LOW,          // below BATTERY_SESSION_COMPLETE_START_X10: can only become incomplete (menu asks first)
    BATTERY_START_CHARGER,      // adapter plugged in: the recorder refuses to start (BATTERY_REC_CHARGER_PLUGGED)
} BatteryStartCheck;

/// Start recording check (menu), from the MCU % (tenths) and the adapter flag. Pure.
u8 BatterySessions_StartCheck(u32 pctX10, bool adapter);
const char *BatterySessions_EndReasonName(u8 reason);
