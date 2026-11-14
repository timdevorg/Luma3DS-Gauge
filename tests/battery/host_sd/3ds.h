// Host stub of <3ds.h> for sd_host_test.c: just enough FS / LightLock API for battery_sessions.c and
// battery_curve_sd.c. The SD card is a directory on the host (see sd_host_test.c).
#pragma once
#include <3ds/types.h>

typedef u32 Handle;
typedef u64 FS_Archive;
typedef int LightLock;

#define R_SUCCEEDED(res)   ((res) >= 0)
#define R_FAILED(res)      ((res) < 0)

typedef enum { PATH_INVALID = 0, PATH_EMPTY = 1, PATH_BINARY = 2, PATH_ASCII = 3, PATH_UTF16 = 4 } FS_PathType;
typedef enum { ARCHIVE_SDMC = 9 } FS_ArchiveID;
typedef struct { FS_PathType type; u32 size; const void *data; } FS_Path;

#define FS_OPEN_READ    (1u << 0)
#define FS_OPEN_WRITE   (1u << 1)
#define FS_OPEN_CREATE  (1u << 2)
#define FS_WRITE_FLUSH  (1u << 0)
#define FS_ATTRIBUTE_DIRECTORY (1u << 0)

typedef struct
{
    u16 name[0x106];
    char shortName[0x0A];
    char shortExt[0x04];
    u8 valid;
    u8 reserved;
    u32 attributes;
    u64 fileSize;
} FS_DirectoryEntry;

FS_Path fsMakePath(FS_PathType type, const void *path);
Result FSUSER_OpenArchive(FS_Archive *archive, FS_ArchiveID id, FS_Path path);
Result FSUSER_CloseArchive(FS_Archive archive);
Result FSUSER_CreateDirectory(FS_Archive archive, FS_Path path, u32 attributes);
Result FSUSER_DeleteFile(FS_Archive archive, FS_Path path);
Result FSUSER_RenameFile(FS_Archive srcArchive, FS_Path srcPath, FS_Archive dstArchive, FS_Path dstPath);
Result FSUSER_OpenDirectory(Handle *out, FS_Archive archive, FS_Path path);
Result FSDIR_Read(Handle handle, u32 *entriesRead, u32 entryCount, FS_DirectoryEntry *entries);
Result FSDIR_Close(Handle handle);

static inline void LightLock_Init(LightLock *l) { *l = 0; }
static inline void LightLock_Lock(LightLock *l) { *l = 1; }
static inline void LightLock_Unlock(LightLock *l) { *l = 0; }
