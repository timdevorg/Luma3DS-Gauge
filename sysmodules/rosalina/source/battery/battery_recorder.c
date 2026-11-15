/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: background discharge recorder (milestone 2).
*
*   - One CSV per session in /luma/battery/sessions/NNNN.csv (format: docs/battery-manager.md).
*   - Samples are buffered in RAM and appended (open/append/flush/close) every
*     BATTERY_FLUSH_EVERY_SAMPLES samples, so a hard power cut loses ~1 min at most
*     and the file is closed (FAT consistent) most of the time.
*   - /luma/battery/state holds "recording=NNNN" while recording. If it still says so
*     at boot, the session was cut off without notice: "# end reason=cutoff" is appended.
*   - On Rosalina's pre-termination notification (shutdown / reboot), the buffer is
*     flushed and "# end reason=shutdown" is written.
*
*   This file never writes to the MCU; MCU access goes through battery_mcu.c (reads only).
*   The LED columns (CSV v2) come from the LED thread (battery_led.c).
*/

#include <3ds.h>
#include "menu.h"
#include "fmt.h"
#include "memory.h"
#include "utils.h"
#include "battery/battery_mcu.h"
#include "battery/battery_sessions.h"
#include "battery/battery_recorder.h"
#include "battery/battery_curve.h"
#include "battery/battery_led.h"

#define BATTERY_THREAD_PRIORITY     58 // same as the task runner, lowest of Rosalina's threads
#define BATTERY_THREAD_STACK_SIZE   0x1000
#define BATTERY_BOOT_RETRY_MS       200u    // boot: SD readiness checked this often
#define BATTERY_BOOT_MAX_WAIT_MS    30000u  // ... for at most this long
#define BATTERY_WAKEUP_MS           1000u

static MyThread g_recorderThread;
static u8 CTR_ALIGN(8) g_recorderThreadStack[BATTERY_THREAD_STACK_SIZE];

static struct
{
    LightLock lock;
    bool recording;
    bool sleepGap;
    u16 number;
    u16 recoveredNumber;
    u32 samples;
    u64 startTick;
    u64 lastSampleTick;
    u64 nextSampleTick;
    u64 startWallMs;
    u64 lastSampleWallMs;
    u32 lastElapsedMs;
    u32 readErrors;
    u32 writeErrors;
    Result lastWriteRes;
    u32 reassertBase;   // LED re-assert counter (per boot) when the session started
    char path[40];
    u32 bufLines;
    u32 bufLen;
    char buf[1024];
} g_rec;

u64 BatteryRecorder_TicksToMs(u64 ticks)
{
    return (ticks / SYSCLOCK_ARM11) * 1000u + ((ticks % SYSCLOCK_ARM11) * 1000u) / SYSCLOCK_ARM11;
}

static u64 batteryNowMs(void)
{
    return BatteryRecorder_TicksToMs(svcGetSystemTick());
}

// ---- Buffer / file helpers (g_rec.lock must be held) ----

static Result batteryFlushLocked(void)
{
    if (g_rec.bufLen == 0)
        return 0;

    Result res = BatteryFs_Append(g_rec.path, g_rec.buf, g_rec.bufLen);
    g_rec.lastWriteRes = res;
    if (R_SUCCEEDED(res))
    {
        g_rec.bufLen = 0;
        g_rec.bufLines = 0;
    }
    else
        g_rec.writeErrors++;
    return res;
}

static void batteryAppendLocked(const char *line, u32 len)
{
    if (g_rec.bufLen + len > sizeof(g_rec.buf))
    {
        // SD keeps failing: drop what we have rather than block the recorder
        if (R_FAILED(batteryFlushLocked()))
        {
            g_rec.bufLen = 0;
            g_rec.bufLines = 0;
        }
    }

    if (len <= sizeof(g_rec.buf) - g_rec.bufLen)
    {
        memcpy(g_rec.buf + g_rec.bufLen, line, len);
        g_rec.bufLen += len;
        g_rec.bufLines++;
    }
}

static void batteryWriteStateIdle(void)
{
    static const char idle[] = "recording=none\n";
    BatteryFs_WriteWhole(BATTERY_STATE_PATH, idle, sizeof(idle) - 1);
}

static void batteryEndLocked(const char *reason)
{
    char line[48];
    int n = sprintf(line, "# end reason=%s\n", reason);
    batteryAppendLocked(line, (u32)n);
    batteryFlushLocked();

    g_rec.lastElapsedMs = (u32)(batteryNowMs() - g_rec.startTick);
    g_rec.recording = false;

    // Written after the end line: if we die in between, the boot recovery sees the end line and does nothing
    batteryWriteStateIdle();
}

static int batteryFormatSample(char *out, const BatteryMcuSample *s, u64 tMs, s64 wallMs)
{
    u32 pctMilli = ((u32)s->pctFrac * 1000u + 128u) / 256u;
    u32 mv = BatteryMcu_VoltageMv(s->voltageRaw);
    char red[4] = "", blue[4] = "", ledActual[4] = "", corrected[12] = "-1";
    BatteryLedStatus led;

    if (s->infoValid)
    {
        sprintf(red, "%lu", (u32)s->info[0x0A]);
        sprintf(blue, "%lu", (u32)s->info[0x0B]);
    }
    if (s->ledModeValid)
        sprintf(ledActual, "%lu", (u32)s->ledMode);

    // v2 columns: corrected % (LED thread's smoothed value, -1 without curve / data), desired LED mode,
    // read-back of reg 0x29, LED re-asserts in this session
    BatteryLed_GetStatus(&led);
    if (led.haveData && led.corrected >= 0.0f)
    {
        u32 c100 = (u32)(led.corrected * 100.0f + 0.5f);
        sprintf(corrected, "%lu.%02lu", c100 / 100, c100 % 100);
    }

    return sprintf(out, "%llu,%lld,%lu,%lu,%lu.%03lu,%lu,%lu.%03lu,%lu,0x%02lx,%lu,%lu,%s,%lu,%s,%s,%lu,%s,%lu\n",
        tMs, wallMs,
        (u32)s->pctInt, (u32)s->pctFrac, (u32)s->pctInt, pctMilli,
        (u32)s->voltageRaw, mv / 1000u, mv % 1000u,
        (u32)s->temperature,
        (u32)s->flags,
        (u32)((s->flags & BATTERY_MCU_FLAG_ADAPTER) != 0),
        (u32)((s->flags & BATTERY_MCU_FLAG_CHARGING) != 0),
        red,
        (u32)s->slider3d,
        blue,
        corrected,
        (u32)led.desired,
        ledActual,
        led.reasserts - g_rec.reassertBase
    );
}

static void batteryRecordSampleLocked(const BatteryMcuSample *s, u64 nowMs, u64 wallMs)
{
    char line[160];
    int n;

    if (g_rec.samples != 0)
    {
        // Sleep detection: the wall clock keeps running while asleep; the tick counter may not.
        u64 tickGap = nowMs - g_rec.lastSampleTick;
        s64 wallGap = (s64)(wallMs - g_rec.lastSampleWallMs);
        u64 gap = tickGap;
        if (wallGap > 0 && (u64)wallGap > gap)
            gap = (u64)wallGap;

        if (gap > BATTERY_SLEEP_GAP_MS)
        {
            n = sprintf(line, "# sleep gap=%llu\n", gap);
            batteryAppendLocked(line, (u32)n);
            g_rec.sleepGap = true;
        }
    }

    n = batteryFormatSample(line, s, nowMs - g_rec.startTick, (s64)(wallMs - g_rec.startWallMs));
    batteryAppendLocked(line, (u32)n);

    g_rec.samples++;
    g_rec.lastSampleTick = nowMs;
    g_rec.nextSampleTick = nowMs + BATTERY_SAMPLE_INTERVAL_MS;
    g_rec.lastSampleWallMs = wallMs;

    if (s->flags & BATTERY_MCU_FLAG_ADAPTER)
        batteryEndLocked("charger");
    else if (g_rec.bufLines >= BATTERY_FLUSH_EVERY_SAMPLES)
        batteryFlushLocked();
}

static int batteryFormatHeader(char *out, u32 number, const BatteryMcuSample *s, u64 wallMs)
{
    char dateStr[32];
    int n = 0;

    dateTimeToString(dateStr, wallMs, false);
    n += sprintf(out + n, "# luma3ds-gauge recorder v2\n");
    n += sprintf(out + n, "# session=%04lu\n", number);
    n += sprintf(out + n, "# start=%s\n", dateStr);
    n += sprintf(out + n, "# start_wall_ms=%llu\n", wallMs);
    n += sprintf(out + n, "# interval_ms=%lu\n", (u32)BATTERY_SAMPLE_INTERVAL_MS);
    if (s->infoValid)
    {
        n += sprintf(out + n, "# battery_vendor=%lu\n", (u32)s->info[2]);
        n += sprintf(out + n, "# mgic_version=%lu.%lu\n", (u32)s->info[3], (u32)s->info[4]);
        n += sprintf(out + n, "# rcomp=0x%02lx\n", (u32)s->info[5]);
        n += sprintf(out + n, "# console_type=%lu pmic_vendor=%lu\n", (u32)s->info[0], (u32)s->info[1]);
        n += sprintf(out + n, "# info_7f=");
        for (u32 i = 0; i < sizeof(s->info); i++)
            n += sprintf(out + n, "%02lx", (u32)s->info[i]);
        n += sprintf(out + n, "\n");
    }
    else
        n += sprintf(out + n, "# info_7f=unavailable\n");
    if (mcuFwVersion != 0)
        n += sprintf(out + n, "# mcu_fw=%lu.%lu\n", GET_VERSION_MAJOR(mcuFwVersion), GET_VERSION_MINOR(mcuFwVersion));
    n += sprintf(out + n, "# volt=volt_raw*5/256 (3dbrew); Rosalina shows volt_raw*0.02\n");
    n += sprintf(out + n, "t_ms,wall_ms,pct_int,pct_frac,pct,volt_raw,volt,temp_c,flags,adapter,charging,red_led,slider3d,blue_led,corrected,led_desired,led_actual,reasserts\n");
    return n;
}

// ---- Public API (menu thread) ----

BatteryRecorderError BatteryRecorder_Start(u16 *outNumber, Result *outRes)
{
    BatteryRecorderError err = BATTERY_REC_OK;
    BatteryMcuSample s;
    Result res = 0;
    char header[640];

    *outNumber = 0;
    *outRes = 0;

    LightLock_Lock(&g_rec.lock);

    if (g_rec.recording)
    {
        *outNumber = g_rec.number;
        err = BATTERY_REC_ALREADY_RECORDING;
        goto end;
    }
    if (preTerminationRequested)
    {
        err = BATTERY_REC_SHUTTING_DOWN;
        goto end;
    }

    res = BatteryMcu_ReadSample(&s);
    if (R_FAILED(res))
    {
        err = BATTERY_REC_MCU_READ_FAILED;
        goto end;
    }
    if (s.flags & BATTERY_MCU_FLAG_ADAPTER)
    {
        err = BATTERY_REC_CHARGER_PLUGGED;
        goto end;
    }

    res = BatteryFs_EnsureDirs();
    if (R_FAILED(res))
    {
        err = BATTERY_REC_SD_FAILED;
        goto end;
    }

    u32 number = BatterySessions_NextNumber();
    if (number == 0)
    {
        err = BATTERY_REC_NO_FREE_NUMBER;
        goto end;
    }

    u64 nowMs = batteryNowMs();
    u64 wallMs = osGetTime();

    BatterySessions_MakePath(g_rec.path, number);
    int n = batteryFormatHeader(header, number, &s, wallMs);
    res = BatteryFs_WriteWhole(g_rec.path, header, (u32)n);
    if (R_FAILED(res))
    {
        err = BATTERY_REC_SD_FAILED;
        goto end;
    }

    char state[32];
    n = sprintf(state, "recording=%04lu\n", number);
    res = BatteryFs_WriteWhole(BATTERY_STATE_PATH, state, (u32)n);
    if (R_FAILED(res))
    {
        err = BATTERY_REC_SD_FAILED;
        goto end;
    }

    g_rec.recording = true;
    g_rec.sleepGap = false;
    g_rec.number = (u16)number;
    g_rec.samples = 0;
    g_rec.startTick = nowMs;
    g_rec.startWallMs = wallMs;
    g_rec.lastElapsedMs = 0;
    g_rec.readErrors = 0;
    g_rec.writeErrors = 0;
    g_rec.lastWriteRes = 0;
    BatteryLedStatus led;
    BatteryLed_GetStatus(&led);
    g_rec.reassertBase = led.reasserts;
    g_rec.bufLen = 0;
    g_rec.bufLines = 0;

    // First sample right away (t_ms = 0), then every BATTERY_SAMPLE_INTERVAL_MS from the thread
    batteryRecordSampleLocked(&s, nowMs, wallMs);
    batteryFlushLocked();
    *outNumber = (u16)number;

end:
    LightLock_Unlock(&g_rec.lock);
    *outRes = res;
    return err;
}

BatteryRecorderError BatteryRecorder_Stop(u16 *outNumber)
{
    BatteryRecorderError err = BATTERY_REC_OK;

    LightLock_Lock(&g_rec.lock);
    *outNumber = g_rec.number;
    if (!g_rec.recording)
        err = BATTERY_REC_NOT_RECORDING;
    else
        batteryEndLocked("stop");
    LightLock_Unlock(&g_rec.lock);

    return err;
}

void BatteryRecorder_GetStatus(BatteryRecorderStatus *out)
{
    LightLock_Lock(&g_rec.lock);
    out->recording = g_rec.recording;
    out->sleepGap = g_rec.sleepGap;
    out->number = g_rec.number;
    out->samples = g_rec.samples;
    out->elapsedMs = g_rec.recording ? (u32)(batteryNowMs() - g_rec.startTick) : g_rec.lastElapsedMs;
    out->bufferedLines = g_rec.bufLines;
    out->readErrors = g_rec.readErrors;
    out->writeErrors = g_rec.writeErrors;
    out->lastWriteRes = g_rec.lastWriteRes;
    out->recoveredNumber = g_rec.recoveredNumber;
    out->reassertBase = g_rec.reassertBase;
    LightLock_Unlock(&g_rec.lock);
}

const char *BatteryRecorder_ErrorString(BatteryRecorderError err)
{
    switch (err)
    {
        case BATTERY_REC_OK:                return "OK.";
        case BATTERY_REC_ALREADY_RECORDING: return "Already recording.";
        case BATTERY_REC_NOT_RECORDING:     return "Not recording.";
        case BATTERY_REC_CHARGER_PLUGGED:   return "Unplug the charger first.";
        case BATTERY_REC_MCU_READ_FAILED:   return "Could not read the MCU.";
        case BATTERY_REC_SD_FAILED:         return "SD card write failed.";
        case BATTERY_REC_NO_FREE_NUMBER:    return "No free session number.";
        case BATTERY_REC_SHUTTING_DOWN:     return "System is shutting down.";
        default:                            return "Unknown error.";
    }
}

// ---- Thread ----

// Boot: if the state file still says "recording=NNNN", that session was cut off without notice.
// g_rec.lock must be held.
static void batteryRecoverCutoffSessionLocked(void)
{
    char state[32];
    char tail[128];
    char path[40];
    u32 len = 0;

    if (R_FAILED(BatteryFs_ReadHead(BATTERY_STATE_PATH, state, sizeof(state) - 1, &len)))
        return;
    state[len] = 0;

    if (strncmp(state, "recording=", 10) != 0 || state[10] < '0' || state[10] > '9')
        return;

    u32 number = 0;
    for (const char *p = state + 10; *p >= '0' && *p <= '9'; p++)
        number = 10 * number + (u32)(*p - '0');

    if (number != 0 && number <= BATTERY_MAX_SESSION_NUMBER)
    {
        BatterySessions_MakePath(path, number);
        if (R_SUCCEEDED(BatteryFs_ReadTail(path, tail, sizeof(tail) - 1, &len)))
        {
            tail[len] = 0;
            if (strstr(tail, "# end ") == NULL)
            {
                // Lines may contain NULs if the FS didn't finish its last write; that only hides an end line,
                // which would at worst produce a second end line (the parser keeps the last one).
                static const char endLine[] = "\n# end reason=cutoff\n";
                bool needsNewline = len != 0 && tail[len - 1] != '\n';
                const char *p = needsNewline ? endLine : endLine + 1;
                BatteryFs_Append(path, p, strlen(p));
            }

            g_rec.recoveredNumber = (u16)number;
            g_rec.number = (u16)number;
        }
    }

    batteryWriteStateIdle();
}

static void batteryRecorderTick(void)
{
    LightLock_Lock(&g_rec.lock);

    u64 nowMs = batteryNowMs();
    if (g_rec.recording && nowMs >= g_rec.nextSampleTick)
    {
        BatteryMcuSample s;
        if (R_SUCCEEDED(BatteryMcu_ReadSample(&s)))
            batteryRecordSampleLocked(&s, nowMs, osGetTime());
        else
        {
            g_rec.readErrors++;
            g_rec.nextSampleTick = nowMs + BATTERY_WAKEUP_MS; // retry in ~1s
        }
    }

    LightLock_Unlock(&g_rec.lock);
}

static void batteryRecorderThreadMain(void)
{
    // Wait until the SD card can be opened (no fixed delay; curve.bin is loaded by the LED thread).
    // Bail out early on shutdown.
    u64 startMs = batteryNowMs();
    while (!BatteryFs_SdReady() && batteryNowMs() - startMs < BATTERY_BOOT_MAX_WAIT_MS)
    {
        if (svcWaitSynchronization(preTerminationEvent, BATTERY_BOOT_RETRY_MS * 1000000LL) == 0)
            return;
    }

    // Under g_rec.lock: BatteryRecorder_Start (menu) can't interleave with the recovery
    LightLock_Lock(&g_rec.lock);
    if (!g_rec.recording) // else the user was faster than the boot delay and the state file is current
        batteryRecoverCutoffSessionLocked();
    LightLock_Unlock(&g_rec.lock);

    for (;;)
    {
        // preTerminationEvent is sticky and signaled on shutdown / reboot (pm notification 0x2000)
        if (svcWaitSynchronization(preTerminationEvent, BATTERY_WAKEUP_MS * 1000000LL) == 0)
            break;
        batteryRecorderTick();
    }

    LightLock_Lock(&g_rec.lock);
    if (g_rec.recording)
        batteryEndLocked("shutdown");
    LightLock_Unlock(&g_rec.lock);
}

MyThread *BatteryRecorder_CreateThread(void)
{
    BatteryMcu_Init();
    BatteryCurve_Init();
    LightLock_Init(&g_rec.lock);

    if (R_FAILED(MyThread_Create(&g_recorderThread, batteryRecorderThreadMain, g_recorderThreadStack,
                                 sizeof(g_recorderThreadStack), BATTERY_THREAD_PRIORITY, CORE_SYSTEM)))
        svcBreak(USERBREAK_PANIC);
    return &g_recorderThread;
}
