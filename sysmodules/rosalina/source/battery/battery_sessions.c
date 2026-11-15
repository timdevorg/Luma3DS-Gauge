/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: SD file helpers and recorded-session parsing / classification.
*/

#include <3ds.h>
#include "ifile.h"
#include "fmt.h"
#include "memory.h"
#include "battery/battery_sessions.h"

#define FS_RES_ALREADY_EXISTS   0xC82044BE // directory already exists (same check as the screenshot code)

static inline FS_Path batteryPath(const char *path)
{
    return fsMakePath(PATH_ASCII, path);
}

void BatterySessions_MakePath(char *out, u32 number)
{
    sprintf(out, BATTERY_SESSIONS_DIR "/%04lu.csv", number);
}

Result BatteryFs_EnsureDirs(void)
{
    FS_Archive archive;
    Result res = FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if (R_FAILED(res))
        return res;

    res = FSUSER_CreateDirectory(archive, batteryPath(BATTERY_DIR), 0);
    if ((u32)res == FS_RES_ALREADY_EXISTS)
        res = 0;
    if (R_SUCCEEDED(res))
    {
        res = FSUSER_CreateDirectory(archive, batteryPath(BATTERY_SESSIONS_DIR), 0);
        if ((u32)res == FS_RES_ALREADY_EXISTS)
            res = 0;
    }

    FSUSER_CloseArchive(archive);
    return res;
}

bool BatteryFs_SdReady(void)
{
    FS_Archive archive;
    if (R_FAILED(FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""))))
        return false;
    FSUSER_CloseArchive(archive);
    return true;
}

static Result batteryFsOpen(IFile *file, const char *path, u32 flags)
{
    memset(file, 0, sizeof(*file));
    return IFile_Open(file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), batteryPath(path), flags);
}

Result BatteryFs_Append(const char *path, const void *data, u32 len)
{
    IFile file;
    u64 size = 0, total = 0;

    Result res = batteryFsOpen(&file, path, FS_OPEN_CREATE | FS_OPEN_WRITE);
    if (R_FAILED(res))
        return res;

    res = IFile_GetSize(&file, &size);
    if (R_SUCCEEDED(res))
    {
        file.pos = size;
        res = IFile_Write(&file, &total, data, len, FS_WRITE_FLUSH);
        if (R_SUCCEEDED(res) && total != len)
            res = -1;
    }

    Result res2 = IFile_Close(&file);
    return R_FAILED(res) ? res : res2;
}

Result BatteryFs_WriteWhole(const char *path, const void *data, u32 len)
{
    IFile file;
    u64 total = 0;

    Result res = batteryFsOpen(&file, path, FS_OPEN_CREATE | FS_OPEN_WRITE);
    if (R_FAILED(res))
        return res;

    res = IFile_SetSize(&file, 0);
    if (R_SUCCEEDED(res))
        res = IFile_Write(&file, &total, data, len, FS_WRITE_FLUSH);

    Result res2 = IFile_Close(&file);
    return R_FAILED(res) ? res : res2;
}

Result BatteryFs_WriteSafely(const char *path, const char *tmpPath, const void *data, u32 len)
{
    Result res = BatteryFs_EnsureDirs();
    if (R_SUCCEEDED(res))
        res = BatteryFs_WriteWhole(tmpPath, data, len);
    if (R_FAILED(res))
        return res;

    FS_Archive archive;
    res = FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if (R_FAILED(res))
        return res;

    FSUSER_DeleteFile(archive, batteryPath(path)); // may not exist
    res = FSUSER_RenameFile(archive, batteryPath(tmpPath), archive, batteryPath(path));
    FSUSER_CloseArchive(archive);
    return res;
}

static Result batteryFsReadAt(const char *path, bool tail, void *data, u32 len, u32 *outLen)
{
    IFile file;
    u64 size = 0, total = 0;

    *outLen = 0;
    Result res = batteryFsOpen(&file, path, FS_OPEN_READ);
    if (R_FAILED(res))
        return res;

    res = IFile_GetSize(&file, &size);
    if (R_SUCCEEDED(res))
    {
        u32 n = size < len ? (u32)size : len;
        file.pos = tail ? size - n : 0;
        res = IFile_Read(&file, &total, data, n);
        *outLen = (u32)total;
    }

    IFile_Close(&file);
    return res;
}

Result BatteryFs_ReadTail(const char *path, void *data, u32 len, u32 *outLen)
{
    return batteryFsReadAt(path, true, data, len, outLen);
}

Result BatteryFs_ReadHead(const char *path, void *data, u32 len, u32 *outLen)
{
    return batteryFsReadAt(path, false, data, len, outLen);
}

// ---- Directory listing ----

// "NNNN.csv" -> NNNN, or 0 if the name doesn't match
static u32 batterySessionNumberFromName(const u16 *name)
{
    u32 n = 0;
    for (u32 i = 0; i < 4; i++)
    {
        if (name[i] < '0' || name[i] > '9')
            return 0;
        n = 10 * n + (name[i] - '0');
    }

    if (name[4] != '.' || (name[5] | 0x20) != 'c' || (name[6] | 0x20) != 's' || (name[7] | 0x20) != 'v' || name[8] != 0)
        return 0;
    return n;
}

static FS_DirectoryEntry g_dirEntry; // big (0x228 bytes), keep it off the stacks

// Calls cb(number, ctx) for each session file. Not reentrant.
static Result batterySessionsForEach(void (*cb)(u32 number, void *ctx), void *ctx)
{
    FS_Archive archive;
    Handle dir;
    Result res = FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if (R_FAILED(res))
        return res;

    res = FSUSER_OpenDirectory(&dir, archive, batteryPath(BATTERY_SESSIONS_DIR));
    if (R_SUCCEEDED(res))
    {
        u32 count = 0;
        while (R_SUCCEEDED(FSDIR_Read(dir, &count, 1, &g_dirEntry)) && count == 1)
        {
            if (g_dirEntry.attributes & FS_ATTRIBUTE_DIRECTORY)
                continue;
            u32 number = batterySessionNumberFromName(g_dirEntry.name);
            if (number != 0)
                cb(number, ctx);
        }
        FSDIR_Close(dir);
    }

    FSUSER_CloseArchive(archive);
    return res;
}

static void batteryMaxCb(u32 number, void *ctx)
{
    u32 *max = (u32 *)ctx;
    if (number > *max)
        *max = number;
}

u32 BatterySessions_NextNumber(void)
{
    u32 max = 0;
    if (R_FAILED(batterySessionsForEach(batteryMaxCb, &max)))
        return 0;
    return max >= BATTERY_MAX_SESSION_NUMBER ? 0 : max + 1;
}

typedef struct BatteryCollectCtx
{
    u16 numbers[BATTERY_MAX_LISTED_SESSIONS];
    u32 count;
} BatteryCollectCtx;

static BatteryCollectCtx g_collectCtx;

// Keeps the highest BATTERY_MAX_LISTED_SESSIONS numbers, sorted ascending
static void batteryCollectCb(u32 number, void *ctx)
{
    BatteryCollectCtx *c = (BatteryCollectCtx *)ctx;
    u32 i;

    if (c->count == BATTERY_MAX_LISTED_SESSIONS)
    {
        if (number <= c->numbers[0])
            return;
        // drop the smallest
        memmove(c->numbers, c->numbers + 1, (c->count - 1) * sizeof(c->numbers[0]));
        c->count--;
    }

    for (i = c->count; i > 0 && c->numbers[i - 1] > number; i--)
        c->numbers[i] = c->numbers[i - 1];
    c->numbers[i] = (u16)number;
    c->count++;
}

Result BatterySessions_Scan(BatterySessionInfo *out, u32 maxCount, u32 *outCount)
{
    *outCount = 0;
    g_collectCtx.count = 0;

    Result res = batterySessionsForEach(batteryCollectCb, &g_collectCtx);
    if (R_FAILED(res))
        return res;

    u32 first = g_collectCtx.count > maxCount ? g_collectCtx.count - maxCount : 0;
    for (u32 i = first; i < g_collectCtx.count; i++)
    {
        BatterySessions_Parse(g_collectCtx.numbers[i], &out[*outCount]);
        (*outCount)++;
    }

    return 0;
}

// ---- Parsing ----

static u8 g_parseChunk[1024];

static bool batteryStartsWith(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

// Parses an unsigned decimal; advances *s past it
static u32 batteryParseU32(const char **s)
{
    u32 n = 0;
    while (**s >= '0' && **s <= '9')
        n = 10 * n + (u32)(*(*s)++ - '0');
    return n;
}

static void batteryParseLine(const char *line, BatterySessionInfo *info)
{
    if (line[0] == '#')
    {
        if (batteryStartsWith(line, "# sleep "))
            info->hasSleepGap = true;
        else if (batteryStartsWith(line, "# end reason="))
        {
            const char *r = line + sizeof("# end reason=") - 1;
            if (batteryStartsWith(r, "stop"))
                info->endReason = BATTERY_END_STOP;
            else if (batteryStartsWith(r, "charger"))
                info->endReason = BATTERY_END_CHARGER;
            else if (batteryStartsWith(r, "shutdown"))
                info->endReason = BATTERY_END_SHUTDOWN;
            else if (batteryStartsWith(r, "cutoff"))
                info->endReason = BATTERY_END_CUTOFF;
            else
                info->endReason = BATTERY_END_OTHER;
        }
        return;
    }

    if (line[0] < '0' || line[0] > '9')
        return; // column header or garbage

    // t_ms,wall_ms,pct_int,pct_frac,...
    const char *s = line;
    u32 tMs = batteryParseU32(&s);
    if (*s++ != ',') return;
    if (*s == '-') s++; // wall_ms can go negative if the clock was changed
    batteryParseU32(&s);
    if (*s++ != ',') return;
    u32 pctInt = batteryParseU32(&s);
    if (*s++ != ',') return;
    u32 pctFrac = batteryParseU32(&s);
    if (*s != ',' || pctInt > 100 || pctFrac > 255) return;

    u16 pctX10 = (u16)(pctInt * 10u + (pctFrac * 10u + 128u) / 256u);
    if (!info->hasData)
    {
        info->startPctX10 = pctX10;
        info->hasData = true;
    }
    info->endPctX10 = pctX10;
    info->durationMs = tMs;
    info->samples++;
}

static void batteryClassify(BatterySessionInfo *info)
{
    bool endedByShutdown = info->endReason == BATTERY_END_SHUTDOWN || info->endReason == BATTERY_END_CUTOFF ||
                           info->endReason == BATTERY_END_NONE; // no end line: cut off (unless it's the active session)
    if (info->hasSleepGap)
        info->cls = BATTERY_SESSION_INVALID;
    else if (info->hasData && info->startPctX10 >= 950 && endedByShutdown)
        info->cls = BATTERY_SESSION_COMPLETE;
    else
        info->cls = BATTERY_SESSION_INCOMPLETE;
}

Result BatterySessions_Parse(u32 number, BatterySessionInfo *out)
{
    char path[40];
    char line[96];
    u32 lineLen = 0;
    IFile file;
    u64 size = 0;

    memset(out, 0, sizeof(*out));
    out->number = (u16)number;
    BatterySessions_MakePath(path, number);

    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), batteryPath(path), FS_OPEN_READ);
    if (R_FAILED(res))
    {
        batteryClassify(out);
        return res;
    }

    if (R_SUCCEEDED(IFile_GetSize(&file, &size)))
        out->sizeBytes = (u32)size;

    for (;;)
    {
        u64 total = 0;
        res = IFile_Read(&file, &total, g_parseChunk, sizeof(g_parseChunk));
        if (R_FAILED(res) || total == 0)
            break;

        for (u32 i = 0; i < (u32)total; i++)
        {
            char c = (char)g_parseChunk[i];
            if (c == '\n' || c == '\r')
            {
                if (lineLen != 0)
                {
                    line[lineLen] = 0;
                    batteryParseLine(line, out);
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
        batteryParseLine(line, out);
    }

    IFile_Close(&file);
    batteryClassify(out);
    return res;
}

Result BatterySessions_Delete(u32 number)
{
    char path[40];
    FS_Archive archive;

    BatterySessions_MakePath(path, number);
    Result res = FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if (R_FAILED(res))
        return res;

    res = FSUSER_DeleteFile(archive, batteryPath(path));
    FSUSER_CloseArchive(archive);
    return res;
}

const char *BatterySessions_ClassName(u8 cls)
{
    switch (cls)
    {
        case BATTERY_SESSION_COMPLETE:  return "complete";
        case BATTERY_SESSION_INVALID:   return "invalid";
        default:                        return "incomplete";
    }
}

const char *BatterySessions_EndReasonName(u8 reason)
{
    switch (reason)
    {
        case BATTERY_END_STOP:      return "stop";
        case BATTERY_END_CHARGER:   return "charger";
        case BATTERY_END_SHUTDOWN:  return "shutdown";
        case BATTERY_END_CUTOFF:    return "cutoff";
        case BATTERY_END_OTHER:     return "other";
        default:                    return "none";
    }
}
