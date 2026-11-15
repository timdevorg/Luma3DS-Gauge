/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: power LED control thread (milestone 4).
*
*   - Polls the MCU (battery_mcu.c, brief sessions): every 10 s while the override is off (only to
*     keep the smoothed corrected % for Status / the recorder), every 1 s while it's on, every
*     250 ms near MCU 10 / 5 / 0 % and for 10 s after a mismatch or a battery-low notification.
*   - Decides the LED mode with battery_led_logic.c and writes it through
*     BatteryMcu_SetPowerLedMode() (the only MCU write; reg 0x29, mode byte, whitelisted modes).
*     It writes only when the desired mode changes, or to re-apply it after a mismatch
*     (at most once per 250 ms).
*   - Keeps controlling while the adapter is plugged in (MCU table only, the level only gets better).
*   - Lid closed / sleep requested: writes the Sleep LED setting (default 5; 1, 2, or nothing for
*     "system") if the LED is blue, keeps red / blinking red; remembers what it wrote and reads 0x29
*     back on wake (before anything is written) for the Status screen; shutdown: writes "power off" (3) once, then never writes again (BatteryLedGate).
*     Both from the notification thread, synchronously (BatteryLed_HandleNotification, main.c).
*   - Boot: starts as soon as the SD card can be opened (config.ini, curve.bin) and mcu::HWC answers,
*     checking every 200 ms instead of waiting a fixed time.
*   - Never writes anything unless the override has been switched on (default off), or the
*     LED test screen is used. "Automatic" is only written to undo our own earlier write.
*
*   Design: docs/battery-manager.md, section 12.
*/

#include <3ds.h>
#include "menu.h"
#include "fmt.h"
#include "memory.h"
#include "battery/battery_mcu.h"
#include "battery/battery_sessions.h"
#include "battery/battery_curve.h"
#include "battery/battery_recorder.h"
#include "battery/battery_led.h"
#include "battery/battery_menu.h"

#define BATTERY_LED_THREAD_PRIORITY     56      // above the recorder / task runner (58), below the menu (52)
#define BATTERY_LED_THREAD_STACK_SIZE   0x800   // shallow call tree, no string formatting
#define BATTERY_LED_BOOT_RETRY_MS       200u    // boot: SD / mcu::HWC readiness checked this often
#define BATTERY_LED_BOOT_MAX_WAIT_MS    30000u  // boot: give up waiting for the SD after this (load defaults)
#define BATTERY_LED_BOOT_FAST_MS        30000u  // boot: retry the first MCU read every 200 ms for this long

#define BATTERY_LED_POLL_IDLE_MS        10000u  // override off: corrected % only
#define BATTERY_LED_POLL_MS             1000u
#define BATTERY_LED_POLL_FAST_MS        250u
#define BATTERY_LED_FAST_AFTER_MS       10000u  // fast polling after a mismatch / battery-low notification
#define BATTERY_LED_MIN_WRITE_GAP_MS    250u    // rate limit for writes from the thread
#define BATTERY_LED_MAX_WRITE_FAILS     5       // in a row -> give up (automatic, error shown)
#define BATTERY_LED_STORM_WINDOW_MS     60000u
#define BATTERY_LED_STORM_REASSERTS     12      // more re-asserts than this within the window -> give up
#define BATTERY_LED_ERROR_RETRY_MS      5000u   // in error state: retry writing "automatic" this often
#define BATTERY_LED_SHELL_OPEN_POLLS    3       // MCU says "open" this often while 0x213 is missing -> open

#define NOTIF_SHELL_OPENED              0x213
#define NOTIF_SHELL_CLOSED              0x214
#define NOTIF_PRE_TERMINATION           0x2000

static MyThread g_ledThread;
static u8 CTR_ALIGN(8) g_ledThreadStack[BATTERY_LED_THREAD_STACK_SIZE];
static Handle g_ledKick; // wakes the thread up (settings changed, wake-up, test end)

static struct
{
    LightLock lock;
    bool configLoaded;
    BatteryLedConfig cfg;
    BatteryLedLogic logic;
    BatteryLedWatch watch;
    BatteryLedSmoother volt;
    u64 nextVoltMs;

    bool haveData;
    float pct, voltSmoothed, corrected;
    u8 flags;
    BatteryLedObserved obs;

    bool shellClosed;       // 0x214 seen, cleared by 0x213
    bool sleepPending;      // sleep requested, cleared by "sleep denied" / "fully awake" / 0x213
    u8 shellOpenPolls;
    bool shuttingDown;
    bool testMode;
    bool prevCharger;       // adapter / charging on the previous poll (voltage median reset on unplug)
    BatteryLedGate gate;    // shutdown latch

    // Sleep diagnostics (read-only): what the sleep hook wrote, 0x29 read on wake before any write
    u32 sleepCount;         // sleep entries (lid closed / sleep requested) this boot
    u8 sleepWrote;          // mode written at the last sleep entry, BATTERY_LED_MODE_UNKNOWN = nothing
    u8 wakeRead;            // 0x29 on wake, BATTERY_LED_MODE_UNKNOWN = read failed
    bool wakePending;       // asleep / lid closed: wakeRead not taken yet

    u64 startMs, sdReadyMs, firstDataMs, firstWriteMs; // boot timing (ms since the tick counter started)

    u8 desired, reason;
    u64 lastAttemptMs;
    u32 reasserts, writes, writeFailures, writeFailStreak;
    Result lastWriteRes;
    bool haveMismatch;
    float lastMismatchPct;
    u64 fastUntilMs;
    u64 stormStartMs;
    u32 stormCount;
    u8 error;
    u64 lastErrorWriteMs;
    bool fastPolling;
} g_led;

static LightLock g_cfgFileLock; // config.ini reads / writes (kept out of g_led.lock: SD can be slow)
static char g_cfgBuf[256];

static u64 batteryLedNowMs(void)
{
    return BatteryRecorder_TicksToMs(svcGetSystemTick());
}

// ---- Writes (g_led.lock held) ----

static bool batteryLedWriteLocked(u8 mode, u64 nowMs)
{
    if (!BatteryLedGate_Allow(&g_led.gate, mode))
        return false; // after the first shutdown signal: nothing (but the one "power off") is written

    g_led.lastAttemptMs = nowMs;
    Result res = BatteryMcu_SetPowerLedMode(mode);
    g_led.lastWriteRes = res;
    if (R_SUCCEEDED(res))
    {
        BatteryLedWatch_Written(&g_led.watch, mode, nowMs);
        if (g_led.writes == 0)
            g_led.firstWriteMs = nowMs;
        g_led.writes++;
        g_led.writeFailStreak = 0;
        return true;
    }

    g_led.writeFailures++;
    if (++g_led.writeFailStreak >= BATTERY_LED_MAX_WRITE_FAILS && g_led.error == BATTERY_LED_ERR_NONE)
        g_led.error = BATTERY_LED_ERR_WRITE_FAILED;
    return false;
}

// Desired mode from the current state (g_led.lock held, g_led.haveData)
static u8 batteryLedDecideLocked(void)
{
    BatteryLedInputs in;
    in.override = g_led.cfg.override;
    in.adapter = (g_led.flags & BATTERY_MCU_FLAG_ADAPTER) != 0;
    in.charging = (g_led.flags & BATTERY_MCU_FLAG_CHARGING) != 0;
    in.shellClosed = g_led.shellClosed || g_led.sleepPending || !(g_led.flags & BATTERY_MCU_FLAG_SHELL_OPEN);
    in.shuttingDown = g_led.shuttingDown || preTerminationRequested;
    in.error = g_led.error != BATTERY_LED_ERR_NONE;
    in.pct = g_led.pct;
    in.corrected = g_led.corrected;
    in.lastWritten = g_led.watch.lastWritten;

    g_led.desired = BatteryLedLogic_Decide(&g_led.logic, &g_led.cfg, &in, &g_led.reason);
    return g_led.desired;
}

// Lid closed / sleep requested (notification thread): apply the sleep mode now, before the system goes on.
// Blue -> the Sleep LED setting (5 / 1 / 2, nothing for "system"); red / blinking red stay; nothing if we
// haven't written anything this boot. entering: first sleep signal of this sleep (counted for diagnostics).
static void batteryLedSleepLocked(u64 nowMs, bool entering)
{
    if (g_led.testMode)
        return; // the test screen closes on lid close; the thread applies the mode after it

    if (entering)
    {
        g_led.sleepCount++;
        g_led.sleepWrote = BATTERY_LED_MODE_UNKNOWN;
        g_led.wakePending = true;
    }
    if (!g_led.haveData)
        return;

    u8 desired = batteryLedDecideLocked();
    u8 mode = BatteryLedLogic_SleepWrite(desired, g_led.reason, g_led.logic.level, g_led.watch.lastWritten, entering);
    if (mode != BATTERY_LED_MODE_KEEP && batteryLedWriteLocked(mode, nowMs)) // not rate limited: last chance before sleep
        g_led.sleepWrote = mode;
}

// First shutdown signal (0x108, 0x2000 or thread exit): "power off" (3) once if we own the LED and the system
// hasn't set it already, then the gate stays closed: no "automatic" afterwards that would override the
// system's power-off indication. Later calls do nothing.
static void batteryLedShutdownLocked(u64 nowMs)
{
    g_led.shuttingDown = true;
    g_led.reason = BATTERY_LED_REASON_SHUTDOWN;
    if (!BatteryLedGate_IsOpen(&g_led.gate))
        return;

    u8 lw = g_led.watch.lastWritten, readMode = 0;
    bool readValid = false;
    if (BatteryLedLogic_OwnsLed(lw))
        readValid = R_SUCCEEDED(BatteryMcu_ReadRegister(BATTERY_MCU_REG_POWER_LED, &readMode, 1));

    u8 mode = BatteryLedGate_Shutdown(&g_led.gate, lw, readValid, readMode);
    g_led.desired = mode;
    if (mode != BATTERY_LED_MODE_KEEP)
        batteryLedWriteLocked(mode, nowMs);
}

static void batteryLedControlLocked(u64 nowMs, bool fresh)
{
    if (g_led.testMode)
    {
        g_led.reason = BATTERY_LED_REASON_TEST;
        return;
    }
    if (!g_led.haveData)
    {
        g_led.desired = BATTERY_LED_MODE_AUTO;
        g_led.reason = BATTERY_LED_REASON_NO_DATA;
        return;
    }

    // Missed 0x213 fallback: the MCU flag says "shell open" for a few polls while not going to sleep
    if (g_led.shellClosed && !g_led.sleepPending && (g_led.flags & BATTERY_MCU_FLAG_SHELL_OPEN))
    {
        if (++g_led.shellOpenPolls >= BATTERY_LED_SHELL_OPEN_POLLS)
            g_led.shellClosed = false;
    }
    else
        g_led.shellOpenPolls = 0;

    u8 desired = batteryLedDecideLocked();

    // Sleep diagnostics: the first fresh poll that is no longer "lid closed" records the 0x29 read-back of
    // this poll's sample, taken before this function writes anything after wake
    if (fresh && g_led.wakePending && g_led.reason != BATTERY_LED_REASON_SHELL)
    {
        g_led.wakeRead = g_led.obs.modeValid ? g_led.obs.mode : BATTERY_LED_MODE_UNKNOWN;
        g_led.wakePending = false;
    }

    if (!fresh || desired == BATTERY_LED_MODE_KEEP)
        return; // the MCU read failed (don't act on old data), or nothing to write (shutdown, lid closed)

    bool rateOk = nowMs - g_led.lastAttemptMs >= BATTERY_LED_MIN_WRITE_GAP_MS;

    if (desired == BATTERY_LED_MODE_AUTO)
    {
        if (!BatteryLedLogic_NeedsWrite(desired, g_led.watch.lastWritten))
            return;
        if (g_led.error != BATTERY_LED_ERR_NONE)
        {
            if (nowMs - g_led.lastErrorWriteMs >= BATTERY_LED_ERROR_RETRY_MS)
            {
                g_led.lastErrorWriteMs = nowMs;
                batteryLedWriteLocked(BATTERY_LED_MODE_AUTO, nowMs);
            }
        }
        else if (rateOk)
            batteryLedWriteLocked(BATTERY_LED_MODE_AUTO, nowMs);
        return;
    }

    if (!rateOk)
        return;

    if (desired != g_led.watch.lastWritten)
    {
        batteryLedWriteLocked(desired, nowMs);
        return;
    }

    if (BatteryLedWatch_Mismatch(&g_led.watch, desired, &g_led.obs, nowMs))
    {
        g_led.reasserts++;
        g_led.haveMismatch = true;
        g_led.lastMismatchPct = g_led.pct;
        g_led.fastUntilMs = nowMs + BATTERY_LED_FAST_AFTER_MS;

        if (g_led.stormCount == 0 || nowMs - g_led.stormStartMs > BATTERY_LED_STORM_WINDOW_MS)
        {
            g_led.stormStartMs = nowMs;
            g_led.stormCount = 0;
        }
        if (++g_led.stormCount > BATTERY_LED_STORM_REASSERTS)
        {
            g_led.error = BATTERY_LED_ERR_MCU_OVERRIDES; // the next poll hands the LED back
            return;
        }
        batteryLedWriteLocked(desired, nowMs);
    }
}

static u32 batteryLedIntervalLocked(u64 nowMs)
{
    if (!g_led.haveData && nowMs - g_led.sdReadyMs < BATTERY_LED_BOOT_FAST_MS)
    {
        g_led.fastPolling = false;
        return BATTERY_LED_BOOT_RETRY_MS; // boot: take over as soon as mcu::HWC answers
    }

    bool needsHandBack = g_led.watch.lastWritten != BATTERY_LED_MODE_AUTO && g_led.watch.lastWritten != BATTERY_LED_MODE_UNKNOWN;
    if (!g_led.cfg.override && !g_led.testMode && !needsHandBack)
    {
        g_led.fastPolling = false;
        return BATTERY_LED_POLL_IDLE_MS;
    }

    g_led.fastPolling = nowMs < g_led.fastUntilMs ||
                        (g_led.haveData && g_led.reason == BATTERY_LED_REASON_ACTIVE && BatteryLedLogic_NearMcuEvent(g_led.pct));
    return g_led.fastPolling ? BATTERY_LED_POLL_FAST_MS : BATTERY_LED_POLL_MS;
}

static void batteryLedPoll(void)
{
    BatteryMcuSample s;
    Result res = BatteryMcu_ReadSample(&s); // outside g_led.lock: the notification thread never waits for a read
    u64 nowMs = batteryLedNowMs();

    LightLock_Lock(&g_led.lock);
    bool fresh = R_SUCCEEDED(res);
    if (fresh)
    {
        if (!g_led.haveData)
            g_led.firstDataMs = nowMs;
        g_led.haveData = true;
        g_led.pct = s.pctInt + s.pctFrac / 256.0f;
        g_led.flags = s.flags;

        bool charger = (s.flags & (BATTERY_MCU_FLAG_ADAPTER | BATTERY_MCU_FLAG_CHARGING)) != 0;
        if (g_led.prevCharger && !charger)
            BatteryLedSmoother_Init(&g_led.volt); // unplugged: drop the elevated charging voltages
        g_led.prevCharger = charger;

        if (g_led.volt.count == 0 || nowMs >= g_led.nextVoltMs)
        {
            BatteryLedSmoother_Push(&g_led.volt, s.voltageRaw);
            g_led.nextVoltMs = nowMs + BATTERY_LED_VOLT_SAMPLE_MS;
        }
        g_led.voltSmoothed = BatteryLedSmoother_Median(&g_led.volt);
        // On the adapter the voltage is elevated: MCU table only (voltRaw 0), no voltage blend
        g_led.corrected = BatteryCurve_Corrected(g_led.pct, charger ? 0.0f : g_led.voltSmoothed);

        g_led.obs.infoValid = s.infoValid;
        g_led.obs.red = s.info[0x0A];
        g_led.obs.blue = s.info[0x0B];
        g_led.obs.modeValid = s.ledModeValid;
        g_led.obs.mode = s.ledMode;
    }
    batteryLedControlLocked(nowMs, fresh);
    LightLock_Unlock(&g_led.lock);
}

// ---- Settings (config.ini) ----

// g_cfgFileLock must be held
static bool batteryLedReadConfigFileLocked(const char *path, BatteryLedConfig *cfg)
{
    u32 len = 0;
    if (R_FAILED(BatteryFs_ReadHead(path, g_cfgBuf, sizeof(g_cfgBuf) - 1, &len)) || len == 0)
        return false;
    g_cfgBuf[len] = 0;
    return BatteryLedConfig_Parse(g_cfgBuf, cfg);
}

void BatteryLed_EnsureConfigLoaded(void)
{
    LightLock_Lock(&g_cfgFileLock);
    LightLock_Lock(&g_led.lock);
    bool loaded = g_led.configLoaded;
    LightLock_Unlock(&g_led.lock);

    if (!loaded)
    {
        BatteryLedConfig cfg;
        BatteryLedConfig_Defaults(&cfg);
        // config.ini, or config.tmp if a power cut hit between delete and rename
        if (!batteryLedReadConfigFileLocked(BATTERY_CONFIG_PATH, &cfg))
        {
            BatteryLedConfig_Defaults(&cfg);
            if (!batteryLedReadConfigFileLocked(BATTERY_CONFIG_TMP_PATH, &cfg))
                BatteryLedConfig_Defaults(&cfg);
        }
        BatteryLedConfig_Sanitize(&cfg);

        LightLock_Lock(&g_led.lock);
        g_led.cfg = cfg;
        g_led.configLoaded = true;
        BatteryLedLogic_Reevaluate(&g_led.logic);
        LightLock_Unlock(&g_led.lock);
        BatteryMenu_UpdateLedTitle(cfg.override);
        BatteryMenu_UpdateSleepLedTitle(cfg.sleepMode);
    }
    LightLock_Unlock(&g_cfgFileLock);
}

void BatteryLed_GetConfig(BatteryLedConfig *out)
{
    LightLock_Lock(&g_led.lock);
    *out = g_led.cfg;
    LightLock_Unlock(&g_led.lock);
}

Result BatteryLed_SetConfig(const BatteryLedConfig *newCfg)
{
    BatteryLedConfig cfg = *newCfg;
    BatteryLedConfig_Sanitize(&cfg);

    BatteryLed_EnsureConfigLoaded(); // so a late boot load can't overwrite this

    LightLock_Lock(&g_led.lock);
    BatteryLedConfig old = g_led.cfg;
    g_led.cfg = cfg;
    if (cfg.redPct != old.redPct || cfg.blinkPct != old.blinkPct || cfg.blueMode != old.blueMode || (cfg.override && !old.override))
        BatteryLedLogic_Reevaluate(&g_led.logic);
    if (cfg.override != old.override)
    {
        // A fresh start: clear a previous error and the counters that lead to it
        g_led.error = BATTERY_LED_ERR_NONE;
        g_led.writeFailStreak = 0;
        g_led.stormCount = 0;
    }
    LightLock_Unlock(&g_led.lock);
    svcSignalEvent(g_ledKick);

    LightLock_Lock(&g_cfgFileLock);
    int n = sprintf(g_cfgBuf,
        "# Battery Manager settings (docs/battery-manager.md, section 12)\n"
        "led_override=%lu\n"
        "red_pct=%lu\n"
        "blink_pct=%lu\n"
        "blue_mode=%lu\n"
        "# sleep_mode: 5 blue on/red off, 1 forced blue, 2 MCU sleep animation, 0 system (write nothing)\n"
        "sleep_mode=%lu\n",
        (u32)(cfg.override ? 1 : 0), (u32)cfg.redPct, (u32)cfg.blinkPct, (u32)cfg.blueMode, (u32)cfg.sleepMode);
    Result res = BatteryFs_WriteSafely(BATTERY_CONFIG_PATH, BATTERY_CONFIG_TMP_PATH, g_cfgBuf, (u32)n);
    LightLock_Unlock(&g_cfgFileLock);
    return res;
}

// ---- Status ----

void BatteryLed_GetStatus(BatteryLedStatus *out)
{
    LightLock_Lock(&g_led.lock);
    out->cfg = g_led.cfg;
    out->haveData = g_led.haveData;
    out->pct = g_led.pct;
    out->voltSmoothed = g_led.voltSmoothed;
    out->corrected = g_led.corrected;
    out->flags = g_led.flags;
    out->desired = g_led.desired;
    out->reason = g_led.reason;
    out->level = g_led.logic.level;
    out->lastWritten = g_led.watch.lastWritten;
    out->readback = g_led.watch.readback;
    out->obs = g_led.obs;
    out->reasserts = g_led.reasserts;
    out->writes = g_led.writes;
    out->writeFailures = g_led.writeFailures;
    out->lastWriteRes = g_led.lastWriteRes;
    out->haveMismatch = g_led.haveMismatch;
    out->lastMismatchPct = g_led.lastMismatchPct;
    out->error = g_led.error;
    out->fastPolling = g_led.fastPolling;
    out->startMs = g_led.startMs;
    out->sdReadyMs = g_led.sdReadyMs;
    out->firstDataMs = g_led.firstDataMs;
    out->firstWriteMs = g_led.firstWriteMs;
    out->sleepCount = g_led.sleepCount;
    out->sleepWrote = g_led.sleepWrote;
    out->wakeRead = g_led.wakeRead;
    out->wakePending = g_led.wakePending;
    LightLock_Unlock(&g_led.lock);
}

// ---- LED test ----

void BatteryLed_TestBegin(void)
{
    LightLock_Lock(&g_led.lock);
    g_led.testMode = true;
    g_led.reason = BATTERY_LED_REASON_TEST;
    LightLock_Unlock(&g_led.lock);
}

Result BatteryLed_TestSetMode(u8 mode)
{
    if (!BatteryMcu_LedModeAllowed(mode))
        return -2;

    Result res = -3;
    LightLock_Lock(&g_led.lock);
    if (g_led.testMode && BatteryLedGate_IsOpen(&g_led.gate) && !preTerminationRequested)
        res = batteryLedWriteLocked(mode, batteryLedNowMs()) ? 0 : g_led.lastWriteRes;
    LightLock_Unlock(&g_led.lock);
    return res;
}

void BatteryLed_TestEnd(void)
{
    LightLock_Lock(&g_led.lock);
    g_led.testMode = false;
    g_led.lastAttemptMs = 0; // re-apply right away
    LightLock_Unlock(&g_led.lock);
    svcSignalEvent(g_ledKick);
}

// ---- Notifications (Rosalina's notification thread) ----

void BatteryLed_HandleNotification(u32 notificationId)
{
    bool sleep = false, kick = false;
    u64 nowMs = batteryLedNowMs();

    LightLock_Lock(&g_led.lock);
    bool wasSleeping = g_led.shellClosed || g_led.sleepPending;
    switch (notificationId)
    {
        case NOTIF_SHELL_CLOSED:
            g_led.shellClosed = true;
            sleep = true;
            break;
        case PTMNOTIFID_SLEEP_REQUESTED:
            g_led.sleepPending = true;
            sleep = true;
            break;
        case NOTIF_SHELL_OPENED: // also fired at boot
            g_led.shellClosed = false;
            g_led.sleepPending = false;
            kick = true;
            break;
        case PTMNOTIFID_SLEEP_DENIED:
        case PTMNOTIFID_FULLY_AWAKE:
            g_led.sleepPending = false;
            kick = true;
            break;
        case PTMNOTIFID_BATTERY_LOW:
        case PTMNOTIFID_BATTERY_VERY_LOW:
            // The system may take the LED back now: look closely for a while
            g_led.fastUntilMs = nowMs + BATTERY_LED_FAST_AFTER_MS;
            kick = true;
            break;
        case PTMNOTIFID_SHUTDOWN:
        case NOTIF_PRE_TERMINATION:
            // Synchronous, before the system goes on: "power off" at the first signal, then nothing
            batteryLedShutdownLocked(nowMs);
            kick = true;
            break;
        default:
            break;
    }

    // Synchronous, before the system goes on with sleep
    if (sleep)
        batteryLedSleepLocked(nowMs, !wasSleeping);
    LightLock_Unlock(&g_led.lock);

    if (kick)
        svcSignalEvent(g_ledKick);
}

// ---- Thread ----

static void batteryLedThreadMain(void)
{
    // Boot: no fixed delay. Rosalina starts before the SD card can be opened and before the mcu sysmodule
    // registers mcu::HWC; check every 200 ms. The SD is needed for config.ini and curve.bin; mcu::HWC is
    // waited for by the poll loop below (200 ms retries until the first read succeeds).
    while (!BatteryFs_SdReady() && batteryLedNowMs() - g_led.startMs < BATTERY_LED_BOOT_MAX_WAIT_MS)
    {
        if (svcWaitSynchronization(preTerminationEvent, BATTERY_LED_BOOT_RETRY_MS * 1000000LL) == 0)
            return; // nothing written this boot: nothing to undo
    }

    LightLock_Lock(&g_led.lock);
    g_led.sdReadyMs = batteryLedNowMs();
    LightLock_Unlock(&g_led.lock);

    BatteryCurve_Load(); // for the LED, the Status screen and the recorder
    BatteryLed_EnsureConfigLoaded();

    Handle handles[2] = { preTerminationEvent, g_ledKick };
    for (;;)
    {
        batteryLedPoll();

        LightLock_Lock(&g_led.lock);
        u32 ms = batteryLedIntervalLocked(batteryLedNowMs());
        LightLock_Unlock(&g_led.lock);

        s32 idx = -1;
        Result res = svcWaitSynchronizationN(&idx, handles, 2, false, (s64)ms * 1000000LL);
        if (res == 0 && idx == 0) // preTerminationEvent is sticky
            break;
    }

    // Normally already done by the notification hook (0x108 / 0x2000): then the gate is closed and this does nothing
    LightLock_Lock(&g_led.lock);
    batteryLedShutdownLocked(batteryLedNowMs());
    LightLock_Unlock(&g_led.lock);
}

MyThread *BatteryLed_CreateThread(void)
{
    LightLock_Init(&g_led.lock);
    LightLock_Init(&g_cfgFileLock);
    BatteryLedConfig_Defaults(&g_led.cfg);
    BatteryLedLogic_Init(&g_led.logic);
    BatteryLedWatch_Init(&g_led.watch);
    BatteryLedSmoother_Init(&g_led.volt);
    BatteryLedGate_Init(&g_led.gate);
    g_led.startMs = batteryLedNowMs();
    g_led.desired = BATTERY_LED_MODE_AUTO;
    g_led.reason = BATTERY_LED_REASON_NO_DATA;
    g_led.corrected = -1.0f;
    g_led.sleepWrote = BATTERY_LED_MODE_UNKNOWN;
    g_led.wakeRead = BATTERY_LED_MODE_UNKNOWN;

    if (R_FAILED(svcCreateEvent(&g_ledKick, RESET_ONESHOT)))
        svcBreak(USERBREAK_PANIC);
    if (R_FAILED(MyThread_Create(&g_ledThread, batteryLedThreadMain, g_ledThreadStack,
                                 sizeof(g_ledThreadStack), BATTERY_LED_THREAD_PRIORITY, CORE_SYSTEM)))
        svcBreak(USERBREAK_PANIC);
    return &g_ledThread;
}
