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
*/

#include <3ds.h>
#include "csvc.h"
#include "process_patches.h"
#include "battery/battery_ptm_patch.h"

// Where the page is mapped in Rosalina. Not 0x00100000: the process list / memory viewer, the DSP dump,
// input redirection and the plugin loader map other processes' code there, maybe at the same time.
// Rosalina's own fixed regions: 0x07000000 (plugin block), 0x08000000 (soc), 0x0D000000 / 0x0D800000
// (framebuffer / screenshot buffers), 0x14000000 (code). A collision would only make the map fail.
#define BATTERY_PTM_MAP_ADDR    0x0E800000u
#define BATTERY_PTM_PAGE_SIZE   0x1000u

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
           state == BATTERY_PTM_STATE_OFF_MISMATCH;
}

static void batteryPtmRead(volatile const u8 *p, u8 *out)
{
    for (u32 i = 0; i < BATTERY_PTM_PATTERN_LEN; i++)
        out[i] = p[i];
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
    // 1. Title ID (with or without the N3DS bit; the O3DS ptm fails the code checks below)
    s64 tid = 0;
    Result tidRes = svcGetProcessInfo(&tid, h, 0x10001);
    g_ptm.st.titleId = (u64)tid;
    if (R_FAILED(tidRes) || ((u64)tid != BATTERY_PTM_TITLE_ID && (u64)tid != BATTERY_PTM_TITLE_ID_BASE))
    {
        g_ptm.st.state = BATTERY_PTM_STATE_OFF_TITLE;
        return;
    }

    // 2. Layout: .text starts at 0x00100000 and covers both patterns
    s64 textStart = 0, textSize = 0;
    if (R_FAILED(svcGetProcessInfo(&textStart, h, 0x10005)) || R_FAILED(svcGetProcessInfo(&textSize, h, 0x10002)) ||
        (u64)textStart != BATTERY_PTM_CODE_BASE ||
        (u64)textSize < (u64)(BATTERY_PTM_CODE_END_VA - BATTERY_PTM_CODE_BASE))
    {
        g_ptm.st.state = BATTERY_PTM_STATE_OFF_LAYOUT;
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

    // The kernel invalidates the instruction cache and flushes the data cache again on unmap
    svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, BATTERY_PTM_MAP_ADDR, BATTERY_PTM_PAGE_SIZE);
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
