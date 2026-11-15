/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: ptm sleep LED patch, Rosalina part.
*
*   Changes exactly one byte in the running ptm process's code in RAM: the immediate of the
*   "movs r1, #2" at VA 0x101a28 that ptm passes to mcu::RTC SetPowerLedState in its sleep LED callback
*   (see battery_ptm_patch.h). Only after verifying the process name, title ID, code layout and the
*   16 bytes around it; only values 0x02 (stock) / 0x01 / 0x05. Nothing is written to NAND, SD or
*   firmware, and no MCU register is written: ptm still sends its own command. Gone at the next boot.
*
*   Mechanism, as Rosalina's own code patches (input_redirection.c, gdb/mem.c, plugin/file_loader.c):
*   OpenProcessByName (process_patches.c), svcMapProcessMemoryEx of the one code page into Rosalina (RW),
*   write, svcFlushEntireDataCache + svcInvalidateEntireInstructionCache, svcUnmapProcessMemoryEx,
*   svcCloseHandle. Design: docs/battery-manager.md, section 14.
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
    g_ptm.st.current = BATTERY_PTM_BYTE_STOCK;
    g_ptm.st.desired = BATTERY_PTM_BYTE_STOCK;
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

// Verifies and (if needed) writes, with ptm's process handle open. g_ptm.lock held.
static void batteryPtmApplyToProcessLocked(Handle h, u8 desired)
{
    // 1. Title ID: New 3DS ptm only (the O3DS ptm, 0004013000002202, has a different layout)
    s64 tid = 0;
    Result tidRes = svcGetProcessInfo(&tid, h, 0x10001);
    g_ptm.st.titleId = (u64)tid;
    if (R_FAILED(tidRes) || ((u64)tid != BATTERY_PTM_TITLE_ID && (u64)tid != BATTERY_PTM_TITLE_ID_BASE))
    {
        g_ptm.st.state = BATTERY_PTM_STATE_OFF_TITLE;
        return;
    }

    // 2. Layout: .text starts at 0x00100000 and covers the 16 bytes
    s64 textStart = 0, textSize = 0;
    if (R_FAILED(svcGetProcessInfo(&textStart, h, 0x10005)) || R_FAILED(svcGetProcessInfo(&textSize, h, 0x10002)) ||
        (u64)textStart != BATTERY_PTM_CODE_BASE ||
        (u64)textSize < (u64)(BATTERY_PTM_PATTERN_VA + BATTERY_PTM_PATTERN_LEN - BATTERY_PTM_CODE_BASE))
    {
        g_ptm.st.state = BATTERY_PTM_STATE_OFF_LAYOUT;
        return;
    }

    // 3. Map the one code page (0x101000) into Rosalina, read-write
    Result res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, BATTERY_PTM_MAP_ADDR, h, BATTERY_PTM_PAGE_VA, BATTERY_PTM_PAGE_SIZE, 0);
    if (R_FAILED(res))
    {
        g_ptm.st.state = BATTERY_PTM_STATE_MAP_ERROR;
        g_ptm.st.lastRes = res;
        return;
    }

    volatile u8 *p = (volatile u8 *)(BATTERY_PTM_MAP_ADDR + (BATTERY_PTM_PATTERN_VA - BATTERY_PTM_PAGE_VA));
    u8 buf[BATTERY_PTM_PATTERN_LEN];
    batteryPtmRead(p, buf);
    g_ptm.st.checks++;

    // 4. The 16 bytes: exact, except byte 10 in {02, 01, 05}. Anything else: off for this boot, nothing written
    if (!BatteryPtmPatch_Verify(buf))
        g_ptm.st.state = BATTERY_PTM_STATE_OFF_MISMATCH;
    else if (buf[BATTERY_PTM_MODE_INDEX] == desired)
    {
        g_ptm.st.state = BATTERY_PTM_STATE_OK;
        g_ptm.st.current = desired;
    }
    else
    {
        // 5. The only write: one byte at ptm VA 0x101a28, value 02 / 01 / 05 (checked by the caller)
        p[BATTERY_PTM_MODE_INDEX] = desired;
        // Our write sits in this core's data cache: clean it to memory, then drop stale copies of that
        // instruction from the instruction caches, as Rosalina's GDB stub does after writing code
        svcFlushEntireDataCache();
        svcInvalidateEntireInstructionCache();

        batteryPtmRead(p, buf);
        if (BatteryPtmPatch_Verify(buf) && buf[BATTERY_PTM_MODE_INDEX] == desired)
        {
            g_ptm.st.state = BATTERY_PTM_STATE_OK;
            g_ptm.st.current = desired;
            g_ptm.st.writes++;
        }
        else
            g_ptm.st.state = BATTERY_PTM_STATE_WRITE_ERROR;
    }

    // The kernel invalidates the instruction cache and flushes the data cache again on unmap
    svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, BATTERY_PTM_MAP_ADDR, BATTERY_PTM_PAGE_SIZE);
}

void BatteryPtmPatch_Apply(u8 desired)
{
    if (!BatteryPtmPatch_ByteAllowed(desired))
        return; // never written

    LightLock_Lock(&g_ptm.lock);
    g_ptm.st.desired = desired;

    // Off for this boot, or already there (we are the only writer: no need to map the page again)
    bool skip = batteryPtmIsOff(g_ptm.st.state) || (g_ptm.st.state == BATTERY_PTM_STATE_OK && g_ptm.st.current == desired);
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
            batteryPtmApplyToProcessLocked(h, desired);
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
