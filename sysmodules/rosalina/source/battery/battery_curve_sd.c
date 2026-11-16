/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: curve files on SD (build from session CSVs, safe write, load) and
*   the loaded curve used for lookups. The math is in battery_curve.c.
*
*   curve.bin is written as curve.tmp first, then the old curve.bin is renamed to curve.bak (the previous
*   curve, for Advanced > Restore previous curve) and curve.tmp to curve.bin. A power cut can at worst
*   leave curve.tmp next to (or instead of) curve.bin; the loader validates the checksum and falls back
*   to curve.tmp.
*/

#include <3ds.h>
#include "ifile.h"
#include "fmt.h"
#include "memory.h"
#include "battery/battery_sessions.h"
#include "battery/battery_curve.h"

static LightLock g_curveLock;
static BatteryCurveFile g_curve; // protected by g_curveLock
static bool g_curveLoaded;
static u32 g_curveGeneration; // BatteryCurve_Load calls (BatteryCurve_Generation)

void BatteryCurve_Init(void)
{
    LightLock_Init(&g_curveLock);
}

// Reads path into g_curve and validates it. g_curveLock must be held.
static bool batteryCurveLoadFileLocked(const char *path)
{
    u32 len = 0;
    if (R_FAILED(BatteryFs_ReadHead(path, &g_curve, sizeof(g_curve), &len)) || len != sizeof(g_curve))
        return false;
    return BatteryCurve_FileIsValid(&g_curve);
}

bool BatteryCurve_Load(void)
{
    LightLock_Lock(&g_curveLock);
    g_curveLoaded = batteryCurveLoadFileLocked(BATTERY_CURVE_PATH) || batteryCurveLoadFileLocked(BATTERY_CURVE_TMP_PATH);
    g_curveGeneration++;
    bool loaded = g_curveLoaded;
    LightLock_Unlock(&g_curveLock);
    return loaded;
}

u32 BatteryCurve_Generation(void)
{
    LightLock_Lock(&g_curveLock);
    u32 gen = g_curveGeneration;
    LightLock_Unlock(&g_curveLock);
    return gen;
}

bool BatteryCurve_IsLoaded(void)
{
    LightLock_Lock(&g_curveLock);
    bool loaded = g_curveLoaded;
    LightLock_Unlock(&g_curveLock);
    return loaded;
}

float BatteryCurve_Corrected(float pct, float voltRaw)
{
    float c = -1.0f;
    LightLock_Lock(&g_curveLock);
    if (g_curveLoaded)
        c = BatteryCurve_CorrectedFrom(&g_curve, pct, voltRaw);
    LightLock_Unlock(&g_curveLock);
    return c;
}

// ---- Building ----

static BatteryCurveSession g_buildSession;
static BatteryCurveAccum g_buildAccum;
static BatteryCurveFile g_buildFile; // also the menu's snapshot of the loaded curve
static u8 g_buildChunk[1024];

const BatteryCurveFile *BatteryCurve_Snapshot(void)
{
    LightLock_Lock(&g_curveLock);
    bool loaded = g_curveLoaded;
    if (loaded)
        memcpy(&g_buildFile, &g_curve, sizeof(g_curve));
    LightLock_Unlock(&g_curveLock);
    return loaded ? &g_buildFile : NULL;
}

// Streams session NNNN line by line into g_buildSession and ends it.
static Result batteryCurveFeedSession(u32 number, BatteryCurveSessionError *outErr)
{
    char path[40];
    char line[128];
    u32 lineLen = 0;
    IFile file;

    BatterySessions_MakePath(path, number);
    BatteryCurve_SessionBegin(&g_buildSession);

    memset(&file, 0, sizeof(file));
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, path), FS_OPEN_READ);
    if (R_FAILED(res))
        return res;

    for (;;)
    {
        u64 total = 0;
        res = IFile_Read(&file, &total, g_buildChunk, sizeof(g_buildChunk));
        if (R_FAILED(res) || total == 0)
            break;

        for (u32 i = 0; i < (u32)total; i++)
        {
            char c = (char)g_buildChunk[i];
            if (c == '\n' || c == '\r')
            {
                if (lineLen != 0)
                {
                    line[lineLen] = 0;
                    BatteryCurve_SessionFeedLine(&g_buildSession, line);
                    lineLen = 0;
                }
            }
            else if (lineLen < sizeof(line) - 1)
                line[lineLen++] = c;
        }
    }

    if (lineLen != 0)
    {
        line[lineLen] = 0;
        BatteryCurve_SessionFeedLine(&g_buildSession, line);
    }

    IFile_Close(&file);
    if (R_SUCCEEDED(res))
        *outErr = BatteryCurve_SessionEnd(&g_buildSession);
    return res;
}

bool BatteryCurve_Build(const u16 *unsortedNumbers, u32 count, BatteryCurveProgressCb cb, BatteryCurveBuildResult *out)
{
    bool used[BATTERY_CURVE_MAX_SESSIONS];
    u16 numbers[BATTERY_CURVE_MAX_SESSIONS];

    memset(out, 0, sizeof(*out));
    if (count > BATTERY_CURVE_MAX_SESSIONS)
        count = BATTERY_CURVE_MAX_SESSIONS;

    // Ascending session numbers (the menu lists newest first), so curve.bin doesn't depend on the
    // selection order and is byte-identical to scripts/build_curve.py --bin for the same sessions.
    for (u32 i = 0; i < count; i++)
    {
        u32 j = i;
        for (; j > 0 && numbers[j - 1] > unsortedNumbers[i]; j--)
            numbers[j] = numbers[j - 1];
        numbers[j] = unsortedNumbers[i];
    }

    // Pass 1: mean / min / max
    BatteryCurve_AccumInit(&g_buildAccum);
    for (u32 i = 0; i < count; i++)
    {
        BatteryCurveSessionError err = BATTERY_CURVE_SESSION_OK;
        if (cb != NULL)
            cb(1, i, count, numbers[i]);

        Result res = batteryCurveFeedSession(numbers[i], &err);
        if (R_FAILED(res))
        {
            out->res = res;
            return false;
        }

        used[i] = err == BATTERY_CURVE_SESSION_OK;
        if (used[i])
            BatteryCurve_AccumAdd(&g_buildAccum, &g_buildSession);
        else
            out->rejectedNumbers[out->rejected++] = numbers[i];
    }

    out->used = g_buildAccum.n;
    if (g_buildAccum.n == 0)
        return false;

    BatteryCurve_AccumFinish(&g_buildAccum, &g_buildFile);

    // Pass 2: deviation of each session from the mean of the others (outliers)
    u32 k = 0;
    for (u32 i = 0; i < count; i++)
    {
        if (!used[i])
            continue;

        BatteryCurveSessionError err = BATTERY_CURVE_SESSION_OK;
        if (cb != NULL)
            cb(2, i, count, numbers[i]);

        g_buildFile.sessions[k] = numbers[i];
        if (g_buildAccum.n >= 2)
        {
            Result res = batteryCurveFeedSession(numbers[i], &err);
            if (R_FAILED(res))
            {
                out->res = res;
                return false;
            }
            g_buildFile.sessionDevX100[k] = BatteryCurve_Deviation(&g_buildAccum, &g_buildSession);
            if (g_buildFile.sessionDevX100[k] > BATTERY_CURVE_OUTLIER_X100)
                g_buildFile.numOutliers++;
        }
        k++;
    }

    BatteryCurve_FileFinalize(&g_buildFile);

    // curve.tmp -> curve.bin renamed to curve.bak -> curve.tmp renamed to curve.bin
    out->res = BatteryFs_WriteSafelyKeepOld(BATTERY_CURVE_PATH, BATTERY_CURVE_TMP_PATH, BATTERY_CURVE_BAK_PATH,
                                            &g_buildFile, sizeof(g_buildFile));
    if (R_FAILED(out->res))
        return false;
    return BatteryCurve_Load();
}

// ---- Previous curve (curve.bak) ----

// Reads curve.bak into g_buildFile and validates it
static bool batteryCurveReadBackup(void)
{
    u32 len = 0;
    if (R_FAILED(BatteryFs_ReadHead(BATTERY_CURVE_BAK_PATH, &g_buildFile, sizeof(g_buildFile), &len)) || len != sizeof(g_buildFile))
        return false;
    return BatteryCurve_FileIsValid(&g_buildFile);
}

bool BatteryCurve_BackupInfo(u16 *outNumSessions, u16 *outSessions)
{
    if (!batteryCurveReadBackup())
        return false;
    if (outNumSessions != NULL)
        *outNumSessions = g_buildFile.numSessions;
    if (outSessions != NULL)
        memcpy(outSessions, g_buildFile.sessions, sizeof(g_buildFile.sessions));
    return true;
}

u8 BatteryCurve_RestoreBackup(Result *outRes)
{
    *outRes = 0;
    if (!batteryCurveReadBackup())
        return BATTERY_CURVE_RESTORE_NO_BACKUP;

    // Same sequence as a build: curve.tmp, current curve.bin -> curve.bak, curve.tmp -> curve.bin
    *outRes = BatteryFs_WriteSafelyKeepOld(BATTERY_CURVE_PATH, BATTERY_CURVE_TMP_PATH, BATTERY_CURVE_BAK_PATH,
                                           &g_buildFile, sizeof(g_buildFile));
    if (R_FAILED(*outRes))
        return BATTERY_CURVE_RESTORE_SD_ERROR;
    return BatteryCurve_Load() ? BATTERY_CURVE_RESTORE_OK : BATTERY_CURVE_RESTORE_LOAD_FAILED;
}

// ---- Session selection (curve_select.ini) ----

static struct
{
    u32 count;
    u16 numbers[BATTERY_MAX_LISTED_SESSIONS];
    bool selected[BATTERY_MAX_LISTED_SESSIONS];
    char buf[BATTERY_MAX_LISTED_SESSIONS * 8 + 96];
} g_sel;

void BatteryCurve_SelectionLoad(void)
{
    u32 len = 0;
    g_sel.count = 0;
    if (R_FAILED(BatteryFs_ReadHead(BATTERY_CURVE_SELECT_PATH, g_sel.buf, sizeof(g_sel.buf) - 1, &len)))
        return;
    g_sel.buf[len] = 0;

    // "NNNN=0|1" per line, '#' comments
    for (const char *p = g_sel.buf; *p != 0;)
    {
        u32 number = 0, digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 5)
        {
            number = 10 * number + (u32)(*p++ - '0');
            digits++;
        }
        if (digits != 0 && p[0] == '=' && (p[1] == '0' || p[1] == '1') && number != 0 &&
            number <= BATTERY_MAX_SESSION_NUMBER && g_sel.count < BATTERY_MAX_LISTED_SESSIONS)
            BatteryCurve_SelectionSet(number, p[1] == '1');

        while (*p != 0 && *p != '\n')
            p++;
        if (*p == '\n')
            p++;
    }
}

s32 BatteryCurve_SelectionGet(u32 number)
{
    for (u32 i = 0; i < g_sel.count; i++)
        if (g_sel.numbers[i] == number)
            return g_sel.selected[i] ? 1 : 0;
    return -1;
}

void BatteryCurve_SelectionSet(u32 number, bool selected)
{
    for (u32 i = 0; i < g_sel.count; i++)
        if (g_sel.numbers[i] == number)
        {
            g_sel.selected[i] = selected;
            return;
        }

    if (g_sel.count == BATTERY_MAX_LISTED_SESSIONS) // forget the oldest entry
    {
        memmove(g_sel.numbers, g_sel.numbers + 1, (g_sel.count - 1) * sizeof(g_sel.numbers[0]));
        memmove(g_sel.selected, g_sel.selected + 1, (g_sel.count - 1) * sizeof(g_sel.selected[0]));
        g_sel.count--;
    }
    g_sel.numbers[g_sel.count] = (u16)number;
    g_sel.selected[g_sel.count] = selected;
    g_sel.count++;
}

Result BatteryCurve_SelectionSave(void)
{
    Result res = BatteryFs_EnsureDirs();
    if (R_FAILED(res))
        return res;

    int n = sprintf(g_sel.buf, "# Battery Manager: sessions used for the curve (1 = use, 0 = skip)\n");
    for (u32 i = 0; i < g_sel.count; i++)
        n += sprintf(g_sel.buf + n, "%04lu=%u\n", (u32)g_sel.numbers[i], g_sel.selected[i] ? 1u : 0u);
    return BatteryFs_WriteWhole(BATTERY_CURVE_SELECT_PATH, g_sel.buf, (u32)n);
}
