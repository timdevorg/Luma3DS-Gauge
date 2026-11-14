/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Host test for the on-console SD code: battery_sessions.c (session parsing / classification,
*   safe write) and battery_curve_sd.c (BatteryCurve_Build), compiled unchanged against the stubs
*   in tests/battery/host_sd/. A host directory plays the SD card.
*
*   Checks:
*   - recorder CSV v1 and v2 files parse to the same session info (BatterySessions_Parse)
*   - BatteryCurve_Build, given the sessions newest first (like the Build curve menu), writes a
*     curve.bin byte-identical to `build_curve.py --bin` (sessions sorted ascending)
*   - the v2 copies build the same curve.bin
*   - BatteryFs_WriteSafely leaves only the target file
*
*   Usage: sd_host_test sd_root_v1 sd_root_v2 expected_curve.bin number [...]
*   (each root holds luma/battery/sessions/NNNN.csv). Run it through tests/battery/run_host_test.sh.
*/

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "3ds.h"
#include "ifile.h"
#include "fmt.h"
#include "battery/battery_sessions.h"
#include "battery/battery_curve.h"

#undef sprintf

static const char *g_root;
static int g_failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

// ---- Stubs ----

int host_sprintf(char *buf, const char *fmt, ...)
{
    char f[512];
    u32 n = 0;
    for (const char *p = fmt; *p != 0 && n < sizeof(f) - 2; p++)
    {
        f[n++] = *p;
        if (*p != '%')
            continue;
        // Inside a conversion: copy flags / width, then turn a single 'l' into nothing (%lu -> %u)
        while (p[1] != 0 && strchr("-+ #0123456789.", p[1]) != NULL && n < sizeof(f) - 2)
            f[n++] = *++p;
        if (p[1] == 'l' && p[2] != 'l')
            p++;
        else if (p[1] == 'l' && p[2] == 'l')
        {
            f[n++] = *++p;
            f[n++] = *++p;
        }
    }
    f[n] = 0;

    va_list args;
    va_start(args, fmt);
    int r = vsnprintf(buf, 1024, f, args); // callers' buffers are smaller, but their output always fits
    va_end(args);
    return r;
}

static void hostPath(char *out, FS_Path p)
{
    snprintf(out, 1024, "%s%s", g_root, (const char *)p.data);
}

FS_Path fsMakePath(FS_PathType type, const void *path)
{
    FS_Path p = { type, (u32)strlen((const char *)path) + 1, path };
    return p;
}

Result FSUSER_OpenArchive(FS_Archive *archive, FS_ArchiveID id, FS_Path path) { (void)id; (void)path; *archive = 1; return 0; }
Result FSUSER_CloseArchive(FS_Archive archive) { (void)archive; return 0; }

Result FSUSER_CreateDirectory(FS_Archive archive, FS_Path path, u32 attributes)
{
    char p[1024];
    (void)archive; (void)attributes;
    hostPath(p, path);
    if (mkdir(p, 0755) == 0)
        return 0;
    return errno == EEXIST ? (Result)0xC82044BE : -1;
}

Result FSUSER_DeleteFile(FS_Archive archive, FS_Path path)
{
    char p[1024];
    (void)archive;
    hostPath(p, path);
    return remove(p) == 0 ? 0 : -1;
}

Result FSUSER_RenameFile(FS_Archive srcArchive, FS_Path srcPath, FS_Archive dstArchive, FS_Path dstPath)
{
    char a[1024], b[1024];
    struct stat st;
    (void)srcArchive; (void)dstArchive;
    hostPath(a, srcPath);
    hostPath(b, dstPath);
    if (stat(b, &st) == 0)
        return -1; // like FS: the destination must not exist
    return rename(a, b) == 0 ? 0 : -1;
}

static DIR *g_dirs[8];

Result FSUSER_OpenDirectory(Handle *out, FS_Archive archive, FS_Path path)
{
    char p[1024];
    (void)archive;
    hostPath(p, path);
    for (u32 i = 0; i < 8; i++)
        if (g_dirs[i] == NULL)
        {
            g_dirs[i] = opendir(p);
            if (g_dirs[i] == NULL)
                return -1;
            *out = i + 1;
            return 0;
        }
    return -1;
}

Result FSDIR_Read(Handle handle, u32 *entriesRead, u32 entryCount, FS_DirectoryEntry *entries)
{
    (void)entryCount;
    *entriesRead = 0;
    struct dirent *d;
    while ((d = readdir(g_dirs[handle - 1])) != NULL)
    {
        if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
            continue;
        memset(entries, 0, sizeof(*entries));
        for (u32 i = 0; d->d_name[i] != 0 && i < 0x105; i++)
            entries->name[i] = (u16)(unsigned char)d->d_name[i];
        entries->attributes = d->d_type == DT_DIR ? FS_ATTRIBUTE_DIRECTORY : 0;
        *entriesRead = 1;
        break;
    }
    return 0;
}

Result FSDIR_Close(Handle handle)
{
    closedir(g_dirs[handle - 1]);
    g_dirs[handle - 1] = NULL;
    return 0;
}

static FILE *g_files[16];

Result IFile_Open(IFile *file, FS_ArchiveID archiveId, FS_Path archivePath, FS_Path filePath, u32 flags)
{
    char p[1024];
    (void)archiveId; (void)archivePath;
    hostPath(p, filePath);
    FILE *f = fopen(p, (flags & FS_OPEN_WRITE) ? "r+b" : "rb");
    if (f == NULL && (flags & FS_OPEN_CREATE))
        f = fopen(p, "w+b");
    if (f == NULL)
        return (Result)0xC8804478; // not found
    for (u32 i = 0; i < 16; i++)
        if (g_files[i] == NULL)
        {
            g_files[i] = f;
            file->handle = i + 1;
            file->pos = 0;
            return 0;
        }
    fclose(f);
    return -1;
}

Result IFile_Close(IFile *file)
{
    fclose(g_files[file->handle - 1]);
    g_files[file->handle - 1] = NULL;
    return 0;
}

Result IFile_GetSize(IFile *file, u64 *size)
{
    FILE *f = g_files[file->handle - 1];
    fseek(f, 0, SEEK_END);
    *size = (u64)ftell(f);
    return 0;
}

Result IFile_SetSize(IFile *file, u64 size)
{
    FILE *f = g_files[file->handle - 1];
    fflush(f);
    return ftruncate(fileno(f), (off_t)size) == 0 ? 0 : -1;
}

Result IFile_Read(IFile *file, u64 *total, void *buffer, u32 len)
{
    FILE *f = g_files[file->handle - 1];
    fseek(f, (long)file->pos, SEEK_SET);
    size_t n = fread(buffer, 1, len, f);
    file->pos += n;
    *total = n;
    return 0;
}

Result IFile_Write(IFile *file, u64 *total, const void *buffer, u32 len, u32 flags)
{
    FILE *f = g_files[file->handle - 1];
    (void)flags;
    fseek(f, (long)file->pos, SEEK_SET);
    size_t n = fwrite(buffer, 1, len, f);
    fflush(f);
    file->pos += n;
    *total = n;
    return n == len ? 0 : -1;
}

// ---- Test ----

static void describe(char *out, const BatterySessionInfo *s)
{
    snprintf(out, 160, "#%04u %s end=%s samples=%u start=%u.%u end=%u.%u duration=%u ms sleep=%d",
            s->number, BatterySessions_ClassName(s->cls), BatterySessions_EndReasonName(s->endReason), s->samples,
            s->startPctX10 / 10, s->startPctX10 % 10, s->endPctX10 / 10, s->endPctX10 % 10, s->durationMs, (int)s->hasSleepGap);
}

static bool readFile(const char *path, void *buf, size_t size, size_t *outLen)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return false;
    *outLen = fread(buf, 1, size, f);
    fclose(f);
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 5)
    {
        fprintf(stderr, "usage: %s sd_root_v1 sd_root_v2 expected_curve.bin number [...]\n", argv[0]);
        return 2;
    }

    const char *roots[2] = { argv[1], argv[2] };
    u32 count = (u32)(argc - 4);
    u16 newestFirst[BATTERY_CURVE_MAX_SESSIONS];
    static char desc[2][BATTERY_CURVE_MAX_SESSIONS][160];
    static u8 expected[4096], got[4096];
    size_t expectedLen = 0, gotLen = 0;

    if (count > BATTERY_CURVE_MAX_SESSIONS || !readFile(argv[3], expected, sizeof(expected), &expectedLen))
        return 2;

    // Menu order: newest first (descending)
    for (u32 i = 0; i < count; i++)
        newestFirst[i] = (u16)atoi(argv[4 + i]);
    for (u32 i = 1; i < count; i++)
        for (u32 j = i; j > 0 && newestFirst[j - 1] < newestFirst[j]; j--)
        {
            u16 t = newestFirst[j]; newestFirst[j] = newestFirst[j - 1]; newestFirst[j - 1] = t;
        }

    BatteryCurve_Init();
    for (u32 r = 0; r < 2; r++)
    {
        g_root = roots[r];
        printf("SD %s (%s):\n", r == 0 ? "v1" : "v2", g_root);

        for (u32 i = 0; i < count; i++)
        {
            BatterySessionInfo info;
            CHECK(R_SUCCEEDED(BatterySessions_Parse(newestFirst[i], &info)));
            describe(desc[r][i], &info);
            printf("  %s\n", desc[r][i]);
            CHECK(info.cls == BATTERY_SESSION_COMPLETE);
        }

        BatteryCurveBuildResult res;
        printf("  Build curve with sessions in menu order:");
        for (u32 i = 0; i < count; i++)
            printf(" %04u", newestFirst[i]);
        printf("\n");
        CHECK(BatteryCurve_Build(newestFirst, count, NULL, &res));
        CHECK(res.used == count && res.rejected == 0);

        char p[1024], tmp[1024];
        snprintf(p, sizeof(p), "%s%s", g_root, BATTERY_CURVE_PATH);
        snprintf(tmp, sizeof(tmp), "%s%s", g_root, BATTERY_CURVE_TMP_PATH);
        CHECK(readFile(p, got, sizeof(got), &gotLen));
        bool same = gotLen == expectedLen && memcmp(got, expected, gotLen) == 0;
        printf("  curve.bin: %s build_curve.py --bin (%zu bytes)\n", same ? "byte-identical to" : "DIFFERS from", gotLen);
        CHECK(same);
        CHECK(access(tmp, F_OK) != 0);

        // Safe write: twice (second time the target exists), only the target is left
        char cfg[1024], cfgTmp[1024];
        snprintf(cfg, sizeof(cfg), "%s/luma/battery/test.ini", g_root);
        snprintf(cfgTmp, sizeof(cfgTmp), "%s/luma/battery/test.tmp", g_root);
        CHECK(R_SUCCEEDED(BatteryFs_WriteSafely("/luma/battery/test.ini", "/luma/battery/test.tmp", "a=1\n", 4)));
        CHECK(R_SUCCEEDED(BatteryFs_WriteSafely("/luma/battery/test.ini", "/luma/battery/test.tmp", "b=22\n", 5)));
        CHECK(readFile(cfg, got, sizeof(got), &gotLen) && gotLen == 5 && memcmp(got, "b=22\n", 5) == 0);
        CHECK(access(cfgTmp, F_OK) != 0);
    }

    bool sameInfo = true;
    for (u32 i = 0; i < count; i++)
        sameInfo = sameInfo && strcmp(desc[0][i], desc[1][i]) == 0;
    printf("sessions: v1 and v2 CSVs parse %s\n", sameInfo ? "to the same info" : "DIFFERENTLY");
    CHECK(sameInfo);

    printf("%s\n", g_failures == 0 ? "sd host test: OK" : "sd host test: FAILED");
    return g_failures == 0 ? 0 : 1;
}
