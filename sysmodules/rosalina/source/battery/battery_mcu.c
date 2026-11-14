/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: the ONLY place battery code talks to mcu::HWC.
*
*   Session handling copies menuUpdateMcuInfo() in menu.c: get a session (or steal
*   one if the port is full), do the access, close it again. Unlike menu.c we use
*   our own local Handle and raw IPC instead of libctru's global mcuHwc handle, so
*   we can never clobber or close a session that menu.c / sysconfig.c /
*   luma_config.c opened through mcuHwcInit()/mcuHwcGetSessionHandle().
*   A LightLock serialises our own users (recorder thread, LED thread, menu thread,
*   notification handler).
*
*   Write path: BatteryMcu_SetPowerLedMode() is the only function that sends a write
*   request. It is hard-wired to reg 0x29, size 1, and to the mode whitelist
*   (docs/battery-manager.md, section 12.2). It uses WriteRegister (IPC 0x00020082)
*   with size 1, i.e. exactly one byte to 0x29, rather than mcu::HWC SetPowerLedState
*   (IPC 0x00060080), because what the MCU sysmodule does internally for the latter is
*   not documented.
*/

#include <3ds.h>
#include "csvc.h"
#include "utils.h"
#include "memory.h"
#include "battery/battery_mcu.h"

#define MCUHWC_CMD_READ_REGISTER    0x1 // IPC 0x00010082, as used by libctru MCUHWC_ReadRegister
#define MCUHWC_CMD_WRITE_REGISTER   0x2 // IPC 0x00020082, as used by libctru MCUHWC_WriteRegister

static LightLock g_mcuLock;

void BatteryMcu_Init(void)
{
    LightLock_Init(&g_mcuLock);
}

static void batteryMcuLock(void)
{
    LightLock_Lock(&g_mcuLock);
}

static void batteryMcuUnlock(void)
{
    LightLock_Unlock(&g_mcuLock);
}

static Result batteryMcuOpen(Handle *h)
{
    *h = 0;
    if (!isServiceUsable("mcu::HWC"))
        return -1;

    Result res = srvGetServiceHandle(h, "mcu::HWC");
    // Try to steal the handle if some other process is using the service (custom SVC)
    if (R_FAILED(res))
        res = svcControlService(SERVICEOP_STEAL_CLIENT_SESSION, h, "mcu::HWC");
    if (R_FAILED(res))
        *h = 0;
    return res;
}

static void batteryMcuClose(Handle h)
{
    if (h != 0)
        svcCloseHandle(h);
}

static bool batteryMcuRangeAllowedForRead(u8 reg, u32 size)
{
    if (size == 0 || size > 0x20)
        return false;

    // Multi-byte registers are read as a whole; single-byte registers auto-increment.
    if (reg == BATTERY_MCU_REG_INFO_TABLE)
        return size <= BATTERY_MCU_INFO_TABLE_SIZE;

    u32 first = reg, last = reg + size - 1;
    if (last >= 0x80)
        return false;
    if (first <= 0x07 && last >= 0x05) // danger zone: MCU unlock sequence
        return false;
    if (first <= 0x13 && last >= 0x10) // received interrupt bitmask, owned by the MCU sysmodule
        return false;
    return true;
}

static Result batteryMcuRawRead(Handle h, u8 reg, void *data, u32 size)
{
    u32 *cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(MCUHWC_CMD_READ_REGISTER, 2, 2);
    cmdbuf[1] = reg;
    cmdbuf[2] = size;
    cmdbuf[3] = IPC_Desc_Buffer(size, IPC_BUFFER_W);
    cmdbuf[4] = (u32)data;

    Result res = svcSendSyncRequest(h);
    return R_FAILED(res) ? res : (Result)cmdbuf[1];
}

Result BatteryMcu_ReadRegister(u8 reg, void *data, u32 size)
{
    if (!batteryMcuRangeAllowedForRead(reg, size))
        return -2;

    Handle h;
    batteryMcuLock();
    Result res = batteryMcuOpen(&h);
    if (R_SUCCEEDED(res))
    {
        res = batteryMcuRawRead(h, reg, data, size);
        batteryMcuClose(h);
    }
    batteryMcuUnlock();
    return res;
}

Result BatteryMcu_ReadSample(BatteryMcuSample *out)
{
    u8 regs[8]; // 0x08..0x0F

    memset(out, 0, sizeof(*out));

    Handle h;
    batteryMcuLock();
    Result res = batteryMcuOpen(&h);
    if (R_SUCCEEDED(res))
    {
        res = batteryMcuRawRead(h, BATTERY_MCU_REG_SLIDER3D, regs, sizeof(regs));
        if (R_SUCCEEDED(res))
        {
            out->slider3d    = regs[0];
            out->volume      = regs[1];
            out->temperature = regs[2];
            out->pctInt      = regs[3];
            out->pctFrac     = regs[4];
            out->voltageRaw  = regs[5];
            out->flags       = regs[7];
        }

        out->ledModeValid = R_SUCCEEDED(batteryMcuRawRead(h, BATTERY_MCU_REG_POWER_LED, &out->ledMode, 1));
        out->infoValid = R_SUCCEEDED(batteryMcuRawRead(h, BATTERY_MCU_REG_INFO_TABLE, out->info, sizeof(out->info)));
        batteryMcuClose(h);
    }
    batteryMcuUnlock();
    return res;
}

Result BatteryMcu_SetPowerLedMode(u8 mode)
{
    // Whitelist (docs/battery-manager.md, sections 3 and 12.2): reg 0x29, mode byte only, known modes only.
    if (!BatteryMcu_LedModeAllowed(mode))
        return -2;

    static u8 s_mode; // IPC buffer, static so it isn't on the caller's stack (written under the lock)

    Handle h;
    batteryMcuLock();
    Result res = batteryMcuOpen(&h);
    if (R_SUCCEEDED(res))
    {
        s_mode = mode;
        u32 *cmdbuf = getThreadCommandBuffer();
        cmdbuf[0] = IPC_MakeHeader(MCUHWC_CMD_WRITE_REGISTER, 2, 2);
        cmdbuf[1] = BATTERY_MCU_REG_POWER_LED;
        cmdbuf[2] = 1;
        cmdbuf[3] = IPC_Desc_Buffer(1, IPC_BUFFER_R);
        cmdbuf[4] = (u32)&s_mode;

        res = svcSendSyncRequest(h);
        if (R_SUCCEEDED(res))
            res = (Result)cmdbuf[1];
        batteryMcuClose(h);
    }
    batteryMcuUnlock();
    return res;
}
