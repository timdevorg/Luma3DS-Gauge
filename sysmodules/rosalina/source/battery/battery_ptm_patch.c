/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: ptm sleep / wake LED patch, Rosalina part.
*
*   Changes at most two bytes in the running ptm process's code in RAM: the immediates of the
*   "movs r1, #mode" that ptm passes to mcu::RTC SetPowerLedState in its sleep LED callback (VA 0x101a28)
*   and in its wake LED callback (VA 0x10199e), see battery_ptm_patch.h. Only after verifying the process
*   name, title ID, code layout and both 16-byte patterns; only whitelisted values (sleep 02 / 01 / 04 / 05,
*   wake 00 / 01 / 04 / 05). Nothing is written to NAND, SD or firmware, and no MCU register is written:
*   ptm still sends its own commands. Gone at the next boot.
*
*   Mechanism, as Rosalina's own code patches (input_redirection.c, gdb/mem.c, plugin/file_loader.c):
*   OpenProcessByName (process_patches.c), svcMapProcessMemoryEx of the one code page (both bytes are on
*   0x101000) into Rosalina (RW), write, svcFlushEntireDataCache + svcInvalidateEntireInstructionCache,
*   svcUnmapProcessMemoryEx, svcCloseHandle. Design: docs/battery-manager.md, section 14.
*
*   Also the battery bars patch (battery_ptm_bars.h, section 15): four more bytes in the same process (the cmp / movs
*   immediates of ptm's two battery level copies, VAs 0x106f5c / 0x106f60 / 0x104cd8 / 0x104cdc), with the same
*   process checks, its own lock, state and scratch mapping (0x0E810000, three pages), and its own verification.
*   The two patches are independent: one being off never turns the other off.
*/

#include <3ds.h>
#include "csvc.h"
#include "process_patches.h"
#include "utils.h" // isN3DS
#include "battery/battery_ptm_patch.h"
#include "battery/battery_ptm_bars.h"

// Where the page is mapped in Rosalina. Not 0x00100000: the process list / memory viewer, the DSP dump,
// input redirection and the plugin loader map other processes' code there, maybe at the same time.
// Rosalina's own fixed regions: 0x07000000 (plugin block), 0x08000000 (soc), 0x0D000000 / 0x0D800000
// (framebuffer / screenshot buffers), 0x14000000 (code). A collision would only make the map fail.
#define BATTERY_PTM_MAP_ADDR    0x0E800000u
#define BATTERY_PTM_PAGE_SIZE   0x1000u
// Bars patch: its own scratch region right after (0x0E810000..0x0E813000), so the two mappings can never collide
// even if the locks were ever taken from different threads
#define BATTERY_BARS_MAP_ADDR   0x0E810000u

_Static_assert(BATTERY_BARS_MAP_ADDR >= BATTERY_PTM_MAP_ADDR + BATTERY_PTM_PAGE_SIZE, "separate scratch mappings");

static struct
{
    LightLock lock;
    BatteryPtmPatchStatus st;
} g_ptm;

void BatteryPtmPatch_Init(void)
{
    LightLock_Init(&g_ptm.lock);
    g_ptm.st.state = BATTERY_PTM_STATE_UNCHECKED;
    g_ptm.st.sleepCur = g_ptm.st.sleepWant = BATTERY_PTM_SLEEP_STOCK;
    g_ptm.st.wakeCur = g_ptm.st.wakeWant = BATTERY_PTM_WAKE_STOCK;
}

static bool batteryPtmIsOff(u8 state)
{
    return state == BATTERY_PTM_STATE_OFF_TITLE || state == BATTERY_PTM_STATE_OFF_LAYOUT ||
           state == BATTERY_PTM_STATE_OFF_MISMATCH || state == BATTERY_PTM_STATE_OFF_UNMAP;
}

static void batteryPtmRead(volatile const u8 *p, u8 *out)
{
    for (u32 i = 0; i < BATTERY_PTM_PATTERN_LEN; i++)
        out[i] = p[i];
}

typedef enum BatteryPtmProcessCheck
{
    BATTERY_PTM_PROCESS_OK = 0,
    BATTERY_PTM_PROCESS_TITLE,      // another title ID
    BATTERY_PTM_PROCESS_LAYOUT,     // .text not at 0x00100000, or too small to hold codeEndVa
} BatteryPtmProcessCheck;

// Title ID (the N3DS ptm; without the N3DS bit only on a New 3DS, S1) and layout (.text starts at 0x00100000 and
// covers everything up to codeEndVa). Shared by both patches.
static u8 batteryPtmCheckProcess(Handle h, u32 codeEndVa, u64 *titleId)
{
    s64 tid = 0;
    Result tidRes = svcGetProcessInfo(&tid, h, 0x10001);
    *titleId = (u64)tid;
    if (R_FAILED(tidRes) || !BatteryPtmPatch_TitleAllowed((u64)tid, isN3DS))
        return BATTERY_PTM_PROCESS_TITLE;

    s64 textStart = 0, textSize = 0;
    if (R_FAILED(svcGetProcessInfo(&textStart, h, 0x10005)) || R_FAILED(svcGetProcessInfo(&textSize, h, 0x10002)) ||
        (u64)textStart != BATTERY_PTM_CODE_BASE || (u64)textSize < (u64)(codeEndVa - BATTERY_PTM_CODE_BASE))
        return BATTERY_PTM_PROCESS_LAYOUT;

    return BATTERY_PTM_PROCESS_OK;
}

// Both patterns: 0 if they verify, else BATTERY_PTM_MISMATCH_* bits
static u8 batteryPtmVerifyBoth(const u8 *sleepBuf, const u8 *wakeBuf)
{
    u8 m = 0;
    if (!BatteryPtmPatch_VerifySleep(sleepBuf))
        m |= BATTERY_PTM_MISMATCH_SLEEP;
    if (!BatteryPtmPatch_VerifyWake(wakeBuf))
        m |= BATTERY_PTM_MISMATCH_WAKE;
    return m;
}

// Verifies and (if needed) writes, with ptm's process handle open. g_ptm.lock held.
static void batteryPtmApplyToProcessLocked(Handle h, u8 sleepByte, u8 wakeByte)
{
    // 1. Title ID (N3DS ptm only, BatteryPtmPatch_TitleAllowed)
    // 2. Layout: .text starts at 0x00100000 and covers both patterns
    u8 check = batteryPtmCheckProcess(h, BATTERY_PTM_CODE_END_VA, &g_ptm.st.titleId);
    if (check != BATTERY_PTM_PROCESS_OK)
    {
        g_ptm.st.state = check == BATTERY_PTM_PROCESS_TITLE ? BATTERY_PTM_STATE_OFF_TITLE : BATTERY_PTM_STATE_OFF_LAYOUT;
        return;
    }

    // 3. Map the one code page (0x101000, holds both patterns) into Rosalina, read-write
    Result res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, BATTERY_PTM_MAP_ADDR, h, BATTERY_PTM_PAGE_VA, BATTERY_PTM_PAGE_SIZE, 0);
    if (R_FAILED(res))
    {
        g_ptm.st.state = BATTERY_PTM_STATE_MAP_ERROR;
        g_ptm.st.lastRes = res;
        return;
    }

    volatile u8 *ps = (volatile u8 *)(BATTERY_PTM_MAP_ADDR + (BATTERY_PTM_SLEEP_PATTERN_VA - BATTERY_PTM_PAGE_VA));
    volatile u8 *pw = (volatile u8 *)(BATTERY_PTM_MAP_ADDR + (BATTERY_PTM_WAKE_PATTERN_VA - BATTERY_PTM_PAGE_VA));
    u8 sbuf[BATTERY_PTM_PATTERN_LEN], wbuf[BATTERY_PTM_PATTERN_LEN];
    batteryPtmRead(ps, sbuf);
    batteryPtmRead(pw, wbuf);
    g_ptm.st.checks++;

    // 4. Both 16-byte patterns: exact, except byte 10 of each within its whitelist. Either one wrong: both
    //    patches off for this boot, nothing written
    u8 mismatch = batteryPtmVerifyBoth(sbuf, wbuf);
    if (mismatch != 0)
    {
        g_ptm.st.state = BATTERY_PTM_STATE_OFF_MISMATCH;
        g_ptm.st.mismatch = mismatch;
    }
    else
    {
        // 5. The only writes: one byte at ptm VA 0x101a28 (sleep) and one at 0x10199e (wake), each only if it
        //    differs, values whitelisted (checked by BatteryPtmPatch_Apply)
        u32 n = 0;
        if (sbuf[BATTERY_PTM_MODE_INDEX] != sleepByte)
        {
            ps[BATTERY_PTM_MODE_INDEX] = sleepByte;
            n++;
        }
        if (wbuf[BATTERY_PTM_MODE_INDEX] != wakeByte)
        {
            pw[BATTERY_PTM_MODE_INDEX] = wakeByte;
            n++;
        }

        if (n != 0)
        {
            // Our writes sit in this core's data cache: clean it to memory, then drop stale copies of those
            // instructions from the instruction caches, as Rosalina's GDB stub does after writing code
            svcFlushEntireDataCache();
            svcInvalidateEntireInstructionCache();

            // 6. Read back both and verify again
            batteryPtmRead(ps, sbuf);
            batteryPtmRead(pw, wbuf);
        }

        if (batteryPtmVerifyBoth(sbuf, wbuf) == 0 && sbuf[BATTERY_PTM_MODE_INDEX] == sleepByte &&
            wbuf[BATTERY_PTM_MODE_INDEX] == wakeByte)
        {
            g_ptm.st.state = BATTERY_PTM_STATE_OK;
            g_ptm.st.sleepCur = sleepByte;
            g_ptm.st.wakeCur = wakeByte;
            g_ptm.st.writes += n;
        }
        else
            g_ptm.st.state = BATTERY_PTM_STATE_WRITE_ERROR;
    }

    // The kernel invalidates the instruction cache and flushes the data cache again on unmap.
    // R4: if the unmap fails, an RW alias of ptm's code would stay mapped at BATTERY_PTM_MAP_ADDR. Nothing writes
    // through it outside this function (ps / pw are locals), and OFF_UNMAP is an "off" state: Apply never maps or
    // writes again this boot.
    res = svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, BATTERY_PTM_MAP_ADDR, BATTERY_PTM_PAGE_SIZE);
    if (R_FAILED(res))
    {
        g_ptm.st.state = BATTERY_PTM_STATE_OFF_UNMAP;
        g_ptm.st.lastRes = res;
    }
}

void BatteryPtmPatch_Apply(u8 sleepByte, u8 wakeByte)
{
    if (!BatteryPtmPatch_SleepByteAllowed(sleepByte) || !BatteryPtmPatch_WakeByteAllowed(wakeByte))
        return; // never written

    LightLock_Lock(&g_ptm.lock);
    g_ptm.st.sleepWant = sleepByte;
    g_ptm.st.wakeWant = wakeByte;

    // Off for this boot, or both already there (we are the only writer: no need to map the page again)
    bool skip = batteryPtmIsOff(g_ptm.st.state) ||
                (g_ptm.st.state == BATTERY_PTM_STATE_OK && g_ptm.st.sleepCur == sleepByte && g_ptm.st.wakeCur == wakeByte);
    if (!skip)
    {
        // OpenProcessByName opens the processes one at a time and closes every handle but ptm's
        Handle h = 0;
        Result res = OpenProcessByName(BATTERY_PTM_PROCESS_NAME, &h);
        if (R_FAILED(res))
        {
            g_ptm.st.state = BATTERY_PTM_STATE_NO_PTM;
            g_ptm.st.lastRes = res;
        }
        else
        {
            batteryPtmApplyToProcessLocked(h, sleepByte, wakeByte);
            svcCloseHandle(h);
        }
    }
    LightLock_Unlock(&g_ptm.lock);
}

void BatteryPtmPatch_GetStatus(BatteryPtmPatchStatus *out)
{
    LightLock_Lock(&g_ptm.lock);
    *out = g_ptm.st;
    LightLock_Unlock(&g_ptm.lock);
}

// ---- Battery bars patch (section 15) ----

static struct
{
    LightLock lock;
    BatteryBarsStatus st;
} g_bars;

void BatteryBars_Init(void)
{
    LightLock_Init(&g_bars.lock);
    g_bars.st.state = BATTERY_BARS_STATE_UNCHECKED;
    g_bars.st.cmpCur = BATTERY_BARS_CMP_STOCK;
    g_bars.st.movsCur = BATTERY_BARS_MOVS_STOCK;
    g_bars.st.levelWant = BATTERY_BARS_STOCK;
}

static bool batteryBarsIsOff(u8 state)
{
    return state == BATTERY_BARS_STATE_OFF_TITLE || state == BATTERY_BARS_STATE_OFF_LAYOUT ||
           state == BATTERY_BARS_STATE_OFF_MISMATCH || state == BATTERY_BARS_STATE_OFF_UNMAP;
}

bool BatteryBars_Retryable(u8 state)
{
    return state == BATTERY_BARS_STATE_NO_PTM || state == BATTERY_BARS_STATE_MAP_ERROR ||
           state == BATTERY_BARS_STATE_WRITE_ERROR;
}

static void batteryBarsRead(volatile const u8 *p, u8 *out)
{
    for (u32 i = 0; i < BATTERY_BARS_WINDOW_LEN; i++)
        out[i] = p[i];
}

// Both windows: 0 if they verify, else BATTERY_BARS_MISMATCH_* bits
static u8 batteryBarsVerifyBoth(const u8 *aBuf, const u8 *bBuf)
{
    u8 m = 0;
    if (!BatteryBars_VerifyA(aBuf))
        m |= BATTERY_BARS_MISMATCH_A;
    if (!BatteryBars_VerifyB(bBuf))
        m |= BATTERY_BARS_MISMATCH_B;
    return m;
}

// One step of the write order: byte `index` of both windows := value, each only if it differs. Then (if anything was
// written) the data cache is cleaned and the instruction cache invalidated, so this step is in memory before the
// next one is written. Returns the number of bytes written.
static u32 batteryBarsWriteStep(volatile u8 *pa, volatile u8 *pb, u32 index, u8 value)
{
    u32 n = 0;
    if (pa[index] != value)
    {
        pa[index] = value;
        n++;
    }
    if (pb[index] != value)
    {
        pb[index] = value;
        n++;
    }
    if (n != 0)
    {
        svcFlushEntireDataCache();
        svcInvalidateEntireInstructionCache();
    }
    return n;
}

// Verifies and (if needed) writes, with ptm's process handle open. g_bars.lock held.
static void batteryBarsApplyToProcessLocked(Handle h, u8 cmp, u8 movs)
{
    // 1. + 2. Title ID and layout (.text covers window A, the higher one)
    u8 check = batteryPtmCheckProcess(h, BATTERY_BARS_CODE_END_VA, &g_bars.st.titleId);
    if (check != BATTERY_PTM_PROCESS_OK)
    {
        g_bars.st.state = check == BATTERY_PTM_PROCESS_TITLE ? BATTERY_BARS_STATE_OFF_TITLE : BATTERY_BARS_STATE_OFF_LAYOUT;
        return;
    }

    // 3. Map ptm 0x104000..0x107000 (window B on 0x104000, window A on 0x106000) into Rosalina, read-write
    Result res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, BATTERY_BARS_MAP_ADDR, h, BATTERY_BARS_MAP_SRC_VA, BATTERY_BARS_MAP_SIZE, 0);
    if (R_FAILED(res))
    {
        g_bars.st.state = BATTERY_BARS_STATE_MAP_ERROR;
        g_bars.st.lastRes = res;
        return;
    }

    volatile u8 *pa = (volatile u8 *)(BATTERY_BARS_MAP_ADDR + (BATTERY_BARS_A_WINDOW_VA - BATTERY_BARS_MAP_SRC_VA));
    volatile u8 *pb = (volatile u8 *)(BATTERY_BARS_MAP_ADDR + (BATTERY_BARS_B_WINDOW_VA - BATTERY_BARS_MAP_SRC_VA));
    u8 abuf[BATTERY_BARS_WINDOW_LEN], bbuf[BATTERY_BARS_WINDOW_LEN];
    batteryBarsRead(pa, abuf);
    batteryBarsRead(pb, bbuf);
    g_bars.st.checks++;

    // 4. Both 16-byte windows: exact, except byte 8 in {3C, 00} and byte 12 in {01..05}. Either one wrong: the bars
    //    patch is off for this boot, nothing written (the sleep / wake patch is not affected)
    u8 mismatch = batteryBarsVerifyBoth(abuf, bbuf);
    if (mismatch != 0)
    {
        g_bars.st.state = BATTERY_BARS_STATE_OFF_MISMATCH;
        g_bars.st.mismatch = mismatch;
    }
    else
    {
        // 5. The only writes: byte 8 (cmp) and byte 12 (movs) of both windows, i.e. ptm VAs 0x106f5c, 0x106f60,
        //    0x104cd8, 0x104cdc, values whitelisted (checked by BatteryBars_Apply). Order: fed (cmp 00) -> movs = L
        //    first, then cmp; stock (cmp 3C) -> cmp first, then movs = 05. Every intermediate state is valid anyway
        //    (host-tested); the order only keeps "cmp 00 with a stale movs" from ever being in memory.
        u32 n = 0;
        if (cmp == BATTERY_BARS_CMP_FED)
        {
            n += batteryBarsWriteStep(pa, pb, BATTERY_BARS_MOVS_INDEX, movs);
            n += batteryBarsWriteStep(pa, pb, BATTERY_BARS_CMP_INDEX, cmp);
        }
        else
        {
            n += batteryBarsWriteStep(pa, pb, BATTERY_BARS_CMP_INDEX, cmp);
            n += batteryBarsWriteStep(pa, pb, BATTERY_BARS_MOVS_INDEX, movs);
        }

        // 6. Read back both and verify again; both copies must now hold exactly the wanted bytes
        if (n != 0)
        {
            batteryBarsRead(pa, abuf);
            batteryBarsRead(pb, bbuf);
        }
        if (batteryBarsVerifyBoth(abuf, bbuf) == 0 &&
            abuf[BATTERY_BARS_CMP_INDEX] == cmp && bbuf[BATTERY_BARS_CMP_INDEX] == cmp &&
            abuf[BATTERY_BARS_MOVS_INDEX] == movs && bbuf[BATTERY_BARS_MOVS_INDEX] == movs)
        {
            g_bars.st.state = BATTERY_BARS_STATE_OK;
            g_bars.st.cmpCur = cmp;
            g_bars.st.movsCur = movs;
            g_bars.st.writes += n;
        }
        else
            g_bars.st.state = BATTERY_BARS_STATE_WRITE_ERROR;
    }

    // The kernel invalidates the instruction cache and flushes the data cache again on unmap. R4: on failure the
    // bars patch is off for this boot (never mapped or written again; pa / pb are locals), as for the sleep patch.
    res = svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, BATTERY_BARS_MAP_ADDR, BATTERY_BARS_MAP_SIZE);
    if (R_FAILED(res))
    {
        g_bars.st.state = BATTERY_BARS_STATE_OFF_UNMAP;
        g_bars.st.lastRes = res;
    }
}

void BatteryBars_Apply(u8 level)
{
    u8 cmp, movs;
    if (!BatteryBars_BytesFor(level, &cmp, &movs) || !BatteryBars_CmpAllowed(cmp) || !BatteryBars_MovsAllowed(movs))
        return; // never written (BytesFor only gives 3C / 05 or 00 / 01..05)

    LightLock_Lock(&g_bars.lock);
    g_bars.st.levelWant = level;

    // Off for this boot, or already there (we are the only writer: no need to map again)
    bool skip = batteryBarsIsOff(g_bars.st.state) ||
                (g_bars.st.state == BATTERY_BARS_STATE_OK && g_bars.st.cmpCur == cmp && g_bars.st.movsCur == movs);
    if (!skip)
    {
        Handle h = 0;
        Result res = OpenProcessByName(BATTERY_PTM_PROCESS_NAME, &h);
        if (R_FAILED(res))
        {
            g_bars.st.state = BATTERY_BARS_STATE_NO_PTM;
            g_bars.st.lastRes = res;
        }
        else
        {
            batteryBarsApplyToProcessLocked(h, cmp, movs);
            svcCloseHandle(h);
        }
    }
    LightLock_Unlock(&g_bars.lock);
}

void BatteryBars_GetStatus(BatteryBarsStatus *out)
{
    LightLock_Lock(&g_bars.lock);
    *out = g_bars.st;
    LightLock_Unlock(&g_bars.lock);
}
