/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: discharge curve (milestone "curve builder + graph").
*
*   Two tables map what the MCU reports to the "real" remaining charge, defined per
*   session as real% = 100 * (T_end - t) / T_end (remaining runtime):
*     - MCU table:     real% vs MCU % (fractional, 0x0B + 0x0C/256), non-uniform grid
*     - voltage table: real% vs volt_raw (reg 0x0D, median over ~1 min), used where the
*                      MCU % is flat (MCU <= 3 %, fully at <= 2 %)
*   Algorithm and file format: docs/battery-manager.md, section 11.
*   scripts/build_curve.py implements the same algorithm on the host and must stay in sync.
*
*   The pure part (battery_curve.c) has no 3DS dependencies besides <3ds/types.h> so it can
*   be compiled and tested on the host (tests/battery/). SD access and the loaded curve live
*   in battery_curve_sd.c.
*/

#pragma once

#include <3ds/types.h>

#define BATTERY_CURVE_PATH          "/luma/battery/curve.bin"
#define BATTERY_CURVE_TMP_PATH      "/luma/battery/curve.tmp"
#define BATTERY_CURVE_BAK_PATH      "/luma/battery/curve.bak"   // the previous curve.bin (same format), kept by Build / Restore
#define BATTERY_CURVE_SELECT_PATH   "/luma/battery/curve_select.ini"

#define BATTERY_CURVE_MAGIC         0x56524342u // "BCRV"
#define BATTERY_CURVE_VERSION       1

// MCU grid (tenths of a percent): 0.0..5.0 step 0.1, 5.5..20.0 step 0.5, 21..100 step 1
#define BATTERY_CURVE_MCU_POINTS    161
// Voltage grid (tenths of a raw unit): 140.5..220.5 step 1 (half-integers, docs section 11.3)
#define BATTERY_CURVE_VOLT_POINTS   81
#define BATTERY_CURVE_VOLT_FIRST_X10 1405

#define BATTERY_CURVE_MAX_SESSIONS  16
#define BATTERY_CURVE_MEDIAN_WINDOW 7       // samples (~1 min at 10 s)
#define BATTERY_CURVE_MIN_SAMPLES   30
#define BATTERY_CURVE_MIN_DURATION_MS 600000u

#define BATTERY_CURVE_OUTLIER_X100  500     // mean abs deviation from the others > 5.0 points
#define BATTERY_CURVE_HIGH_END_X10  50      // session ending above 5.0 %: probably powered off by hand

// Voltage blend: weight 0 at MCU >= 3.0 %, 1 at MCU <= 2.0 %
#define BATTERY_CURVE_BLEND_HI      3.0f
#define BATTERY_CURVE_BLEND_LO      2.0f

#define BATTERY_CURVE_FLAG_VOLT_VALID   (1u << 0)

/// curve.bin: this struct as-is, little endian. real% values are in hundredths (0..10000).
typedef struct BatteryCurveFile
{
    u32 magic;              // BATTERY_CURVE_MAGIC
    u16 version;            // BATTERY_CURVE_VERSION
    u16 numSessions;        // sessions used (<= BATTERY_CURVE_MAX_SESSIONS)
    u16 flags;              // BATTERY_CURVE_FLAG_*
    u16 mcuPoints;          // BATTERY_CURVE_MCU_POINTS
    u16 voltPoints;         // BATTERY_CURVE_VOLT_POINTS
    u16 maxSpreadX100;      // max over the MCU grid of (max - min)
    u16 numOutliers;
    u16 sessions[BATTERY_CURVE_MAX_SESSIONS];       // session numbers used, 0-padded
    u16 sessionDevX100[BATTERY_CURVE_MAX_SESSIONS]; // mean abs deviation from the others (MCU table)
    u16 mcuGridX10[BATTERY_CURVE_MCU_POINTS];       // ascending, tenths of a percent
    u16 mcuMean[BATTERY_CURVE_MCU_POINTS];
    u16 mcuMin[BATTERY_CURVE_MCU_POINTS];
    u16 mcuMax[BATTERY_CURVE_MCU_POINTS];
    u16 voltGridX10[BATTERY_CURVE_VOLT_POINTS];     // ascending, tenths of a volt_raw unit
    u16 voltMean[BATTERY_CURVE_VOLT_POINTS];
    u16 voltMin[BATTERY_CURVE_VOLT_POINTS];
    u16 voltMax[BATTERY_CURVE_VOLT_POINTS];
    u16 reserved;
    u32 checksum;           // CRC-32 (zlib) of all bytes before this field
} BatteryCurveFile;

_Static_assert(sizeof(BatteryCurveFile) == 2024, "curve.bin layout changed: bump the version");

typedef enum BatteryCurveSessionError
{
    BATTERY_CURVE_SESSION_OK = 0,
    BATTERY_CURVE_SESSION_TOO_SHORT,
    BATTERY_CURVE_SESSION_SLEEP_GAP,
} BatteryCurveSessionError;

/// Streaming state for one session (one pass over its CSV).
typedef struct BatteryCurveSession
{
    u32 samples;
    bool sleepGap;
    double firstPct, lastPct, lastT;
    // MCU crossing
    s32 mcuNext;
    bool mcuHasPrev;
    double mcuPrevX, mcuPrevT;
    // voltage median window + crossing
    u32 voltCount;
    u8 voltWin[BATTERY_CURVE_MEDIAN_WINDOW];
    double voltWinT[BATTERY_CURVE_MEDIAN_WINDOW];
    s32 voltNext;
    bool voltHasPrev;
    double voltPrevX, voltPrevT;
    // crossing times, converted in place to real% by BatteryCurve_SessionEnd
    double mcu[BATTERY_CURVE_MCU_POINTS];
    double volt[BATTERY_CURVE_VOLT_POINTS];
} BatteryCurveSession;

/// Combines sessions (pass 1: Add for each session, then Finish; pass 2: Deviation for each session).
typedef struct BatteryCurveAccum
{
    u32 n;
    double mcuSum[BATTERY_CURVE_MCU_POINTS];
    double voltSum[BATTERY_CURVE_VOLT_POINTS];
    u16 mcuMin[BATTERY_CURVE_MCU_POINTS], mcuMax[BATTERY_CURVE_MCU_POINTS];
    u16 voltMin[BATTERY_CURVE_VOLT_POINTS], voltMax[BATTERY_CURVE_VOLT_POINTS];
} BatteryCurveAccum;

// ---- Pure functions (battery_curve.c, host-testable) ----

void BatteryCurve_MakeGrids(u16 *mcuGridX10, u16 *voltGridX10);

void BatteryCurve_SessionBegin(BatteryCurveSession *s);
/// Feeds one CSV line (without the line break): sample rows, comments and the column header.
void BatteryCurve_SessionFeedLine(BatteryCurveSession *s, const char *line);
/// Converts the crossing times to real%. The session must not be used if this fails.
BatteryCurveSessionError BatteryCurve_SessionEnd(BatteryCurveSession *s);

void BatteryCurve_AccumInit(BatteryCurveAccum *a);
void BatteryCurve_AccumAdd(BatteryCurveAccum *a, const BatteryCurveSession *s);
/// Fills means/min/max, grids, header and checksum (sessions[]/sessionDevX100[]/numOutliers are
/// set by the caller before calling BatteryCurve_FileFinalize).
void BatteryCurve_AccumFinish(const BatteryCurveAccum *a, BatteryCurveFile *out);
/// Mean absolute deviation (percentage points * 100) of a session from the mean of the others.
u16 BatteryCurve_Deviation(const BatteryCurveAccum *a, const BatteryCurveSession *s);
/// Computes maxSpreadX100 and the checksum. Call last.
void BatteryCurve_FileFinalize(BatteryCurveFile *f);

u32 BatteryCurve_Crc32(const void *data, u32 len);
/// Checks magic, version, point counts, grids and checksum.
bool BatteryCurve_FileIsValid(const BatteryCurveFile *f);

/// Linear interpolation of a table at xX10 (clamped to the grid). Returns real% (0..100).
float BatteryCurve_Interp(const u16 *gridX10, const u16 *values, u32 count, float xX10);
/// Corrected % for an MCU percentage (pct) and a raw voltage (voltRaw <= 0: MCU table only).
float BatteryCurve_CorrectedFrom(const BatteryCurveFile *f, float pct, float voltRaw);

// ---- SD / loaded curve (battery_curve_sd.c) ----

void BatteryCurve_Init(void);
/// (Re)loads curve.bin (or curve.tmp if a power cut hit the rename). Returns true if a valid curve is loaded.
bool BatteryCurve_Load(void);
bool BatteryCurve_IsLoaded(void);
/// Counts BatteryCurve_Load calls (boot, build, restore): the time-remaining estimate starts over when it changes.
u32 BatteryCurve_Generation(void);
/// Copies the loaded curve into a static buffer shared with BatteryCurve_Build (saves RAM) and
/// returns it, or NULL if no curve is loaded. Menu thread only; valid until the next call / build.
const BatteryCurveFile *BatteryCurve_Snapshot(void);
/// Corrected % from the loaded curve, or -1 if no curve is loaded. Thread-safe.
float BatteryCurve_Corrected(float pct, float voltRaw);

typedef void (*BatteryCurveProgressCb)(u32 pass, u32 index, u32 count, u32 number);

typedef struct BatteryCurveBuildResult
{
    Result res;              // SD error, 0 if none
    u32 used;
    u32 rejected;            // too short / sleep gap
    u16 rejectedNumbers[BATTERY_CURVE_MAX_SESSIONS];
} BatteryCurveBuildResult;

/// Builds the curve from the given sessions (any order; used in ascending session-number order, like
/// scripts/build_curve.py, so curve.bin is byte-identical to the host's), writes it safely
/// (curve.tmp, old curve.bin renamed to curve.bak, curve.tmp renamed to curve.bin) and loads it.
/// Menu thread only (static buffers).
bool BatteryCurve_Build(const u16 *numbers, u32 count, BatteryCurveProgressCb cb, BatteryCurveBuildResult *out);

/// Reads curve.bak and checks it (like the loader). Returns false if it is missing or invalid. *outNumSessions and
/// outSessions (BATTERY_CURVE_MAX_SESSIONS entries) may be NULL. Menu thread only (uses the Snapshot buffer: a pointer
/// returned by BatteryCurve_Snapshot before this call is overwritten).
bool BatteryCurve_BackupInfo(u16 *outNumSessions, u16 *outSessions);

typedef enum BatteryCurveRestore
{
    BATTERY_CURVE_RESTORE_OK = 0,
    BATTERY_CURVE_RESTORE_NO_BACKUP,    // curve.bak missing or invalid: nothing changed
    BATTERY_CURVE_RESTORE_SD_ERROR,     // *outRes
    BATTERY_CURVE_RESTORE_LOAD_FAILED,  // written, but curve.bin doesn't load back
} BatteryCurveRestore;

/// Makes curve.bak the curve again: its content goes through curve.tmp like a build, the current curve.bin becomes
/// curve.bak (so restoring twice swaps back), then the curve is reloaded. Menu thread only (static buffers).
u8 BatteryCurve_RestoreBackup(Result *outRes);

/// Selection file: "NNNN=1" / "NNNN=0" lines. Returns -1 if the session isn't listed.
void BatteryCurve_SelectionLoad(void);
s32 BatteryCurve_SelectionGet(u32 number);
void BatteryCurve_SelectionSet(u32 number, bool selected);
Result BatteryCurve_SelectionSave(void);
