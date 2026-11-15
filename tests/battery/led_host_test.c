/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Host test for the Battery Manager LED logic (sysmodules/rosalina/source/battery/battery_led_logic.c).
*
*   1. Unit tests: thresholds, hysteresis, "only gets worse", charging ("only gets better", unplug,
*      full), threshold edits, sleep (sleep animation / red kept), wake, shutdown latch ("power off"
*      once, nothing after), whitelist, mismatch detection, voltage median, config parsing, sleep transition
*      (no re-assert between sleep entry and wake, ptm sleep / wake bytes, charging at sleep entry).
*   2. Replay: feeds recorded sessions (CSV v1 or v2) through the same code path the LED thread
*      uses (voltage median sampled every 10 s, BatteryCurve_CorrectedFrom, BatteryLedLogic_Decide)
*      with the override on and default settings, and prints when the LED changes.
*
*   Usage: led_host_test [curve.bin session.csv [...]]
*   Without arguments only the unit tests run (the replay is skipped).
*   Run it through tests/battery/run_host_test.sh.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "battery/battery_curve.h"
#include "battery/battery_led_logic.h"
#include "battery/battery_mcu.h"
#include "battery/battery_ptm_patch.h"

static int g_failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

static BatteryLedInputs inputs(float corrected)
{
    BatteryLedInputs in;
    memset(&in, 0, sizeof(in));
    in.override = true;
    in.pct = 50.0f;
    in.corrected = corrected;
    in.lastWritten = BATTERY_LED_MODE_BLUE;
    return in;
}

static u8 decideCharging(BatteryLedLogic *l, const BatteryLedConfig *cfg, float corrected, bool charging, float pct)
{
    BatteryLedInputs in = inputs(corrected);
    in.adapter = true;
    in.charging = charging;
    in.pct = pct;
    return BatteryLedLogic_Decide(l, cfg, &in, NULL);
}

static u8 decideShell(BatteryLedLogic *l, const BatteryLedConfig *cfg, float corrected, u8 lastWritten, u8 *reason)
{
    BatteryLedInputs in = inputs(corrected);
    in.shellClosed = true;
    in.lastWritten = lastWritten;
    return BatteryLedLogic_Decide(l, cfg, &in, reason);
}

// Minimal model of battery_led.c's write path: the gate is asked before every write, the "MCU" counts writes
typedef struct FakeLed
{
    BatteryLedGate gate;
    u8 lastWritten;
    u32 writes, powerOffWrites;
    u8 reg29;
} FakeLed;

static void fakeInit(FakeLed *f)
{
    memset(f, 0, sizeof(*f));
    BatteryLedGate_Init(&f->gate);
    f->lastWritten = BATTERY_LED_MODE_UNKNOWN;
}

static bool fakeWrite(FakeLed *f, u8 mode)
{
    if (!BatteryMcu_LedModeAllowed(mode) || !BatteryLedGate_Allow(&f->gate, mode))
        return false;
    f->lastWritten = mode;
    f->reg29 = mode;
    f->writes++;
    if (mode == BATTERY_LED_MODE_POWER_OFF)
        f->powerOffWrites++;
    return true;
}

// The thread / notification hook's write rule for a desired mode
static void fakeApply(FakeLed *f, u8 desired)
{
    if (BatteryLedLogic_NeedsWrite(desired, f->lastWritten))
        fakeWrite(f, desired);
}

// batteryLedShutdownLocked: read 0x29 if we own the LED, then the gate decides
static void fakeShutdown(FakeLed *f)
{
    u8 mode = BatteryLedGate_Shutdown(&f->gate, f->lastWritten, true, f->reg29);
    if (mode != BATTERY_LED_MODE_KEEP)
        fakeWrite(f, mode);
}

static u8 decide(BatteryLedLogic *l, const BatteryLedConfig *cfg, float corrected)
{
    BatteryLedInputs in = inputs(corrected);
    return BatteryLedLogic_Decide(l, cfg, &in, NULL);
}

// The sleep hook (batteryLedSleepLocked): Decide with the lid closed, then BatteryLedLogic_SleepWrite
static u8 sleepWrite(BatteryLedLogic *l, const BatteryLedConfig *cfg, float corrected, u8 lastWritten, bool entering)
{
    u8 reason;
    BatteryLedInputs in = inputs(corrected);
    in.shellClosed = true;
    in.lastWritten = lastWritten;
    u8 desired = BatteryLedLogic_Decide(l, cfg, &in, &reason);
    return BatteryLedLogic_SleepWrite(desired, reason, l->level, lastWritten, entering);
}

// Sleep LED setting (sleep_mode): which mode, if any, the sleep hook writes for each setting and level
static void sleepSettingTests(void)
{
    static const u8 settings[] = { BATTERY_LED_MODE_BLUE, BATTERY_LED_MODE_BLUE_FORCED, BATTERY_LED_MODE_SLEEP, BATTERY_LED_SLEEP_SYSTEM };
    BatteryLedConfig cfg;
    BatteryLedLogic l;
    BatteryLedConfig_Defaults(&cfg);
    cfg.override = true;

    for (u32 i = 0; i < sizeof(settings); i++)
    {
        u8 set = settings[i];
        u8 expBlue = set == BATTERY_LED_SLEEP_SYSTEM ? BATTERY_LED_MODE_KEEP : set;
        cfg.sleepMode = set;
        for (u32 b = 0; b < 2; b++)
        {
            cfg.blueMode = b ? BATTERY_LED_MODE_BLUE_FORCED : BATTERY_LED_MODE_BLUE;
            u8 blue = cfg.blueMode;

            // Blue level: the setting, written on entry even if it equals the last written mode
            BatteryLedLogic_Init(&l);
            CHECK(decide(&l, &cfg, 50.0f) == blue);
            CHECK(BatteryLedLogic_SleepModeForLevel(&cfg, BATTERY_LED_LEVEL_BLUE) == expBlue);
            CHECK(sleepWrite(&l, &cfg, 50.0f, blue, true) == expBlue);
            // Second signal of the same sleep (0x101 after 0x214): only if it isn't there yet
            CHECK(sleepWrite(&l, &cfg, 50.0f, set == BATTERY_LED_SLEEP_SYSTEM ? blue : set, false) == BATTERY_LED_MODE_KEEP);
            if (set != BATTERY_LED_SLEEP_SYSTEM && set != blue)
                CHECK(sleepWrite(&l, &cfg, 50.0f, blue, false) == set);
            // Wake: back to the blue mode
            CHECK(decide(&l, &cfg, 50.0f) == blue);

            // Red / blinking red: unchanged, nothing written (they stay), whatever the setting
            BatteryLedLogic_Init(&l);
            CHECK(decide(&l, &cfg, 9.0f) == BATTERY_LED_MODE_RED);
            CHECK(BatteryLedLogic_SleepModeForLevel(&cfg, BATTERY_LED_LEVEL_RED) == BATTERY_LED_MODE_RED);
            CHECK(sleepWrite(&l, &cfg, 9.0f, BATTERY_LED_MODE_RED, true) == BATTERY_LED_MODE_KEEP);
            CHECK(sleepWrite(&l, &cfg, 30.0f, BATTERY_LED_MODE_RED, true) == BATTERY_LED_MODE_KEEP); // only worse: no blue
            CHECK(sleepWrite(&l, &cfg, 2.0f, BATTERY_LED_MODE_RED, true) == BATTERY_LED_MODE_BLINK); // got worse
            CHECK(BatteryLedLogic_SleepModeForLevel(&cfg, BATTERY_LED_LEVEL_BLINK) == BATTERY_LED_MODE_BLINK);
            CHECK(sleepWrite(&l, &cfg, 2.0f, BATTERY_LED_MODE_BLINK, true) == BATTERY_LED_MODE_KEEP);
            CHECK(decide(&l, &cfg, 50.0f) == BATTERY_LED_MODE_BLINK); // wake: still blinking

            // Nothing written this boot / last write automatic: nothing on sleep
            BatteryLedLogic_Init(&l);
            CHECK(sleepWrite(&l, &cfg, 50.0f, BATTERY_LED_MODE_UNKNOWN, true) == BATTERY_LED_MODE_KEEP);
            CHECK(sleepWrite(&l, &cfg, 50.0f, BATTERY_LED_MODE_AUTO, true) == BATTERY_LED_MODE_KEEP);
        }
        cfg.blueMode = BATTERY_LED_MODE_BLUE;

        // Override off / no curve / error / shutdown: the setting never writes anything
        BatteryLedInputs in;
        u8 reason;
        BatteryLedLogic_Init(&l);
        in = inputs(50.0f);
        in.shellClosed = true;
        in.override = false;
        in.lastWritten = BATTERY_LED_MODE_UNKNOWN;
        u8 d = BatteryLedLogic_Decide(&l, &cfg, &in, &reason);
        CHECK(BatteryLedLogic_SleepWrite(d, reason, l.level, in.lastWritten, true) == BATTERY_LED_MODE_KEEP);
        in = inputs(-1.0f);
        in.shellClosed = true;
        in.lastWritten = BATTERY_LED_MODE_UNKNOWN;
        d = BatteryLedLogic_Decide(&l, &cfg, &in, &reason);
        CHECK(BatteryLedLogic_SleepWrite(d, reason, l.level, in.lastWritten, true) == BATTERY_LED_MODE_KEEP);
        in = inputs(50.0f);
        in.shellClosed = true;
        in.shuttingDown = true;
        d = BatteryLedLogic_Decide(&l, &cfg, &in, &reason);
        CHECK(BatteryLedLogic_SleepWrite(d, reason, l.level, in.lastWritten, true) == BATTERY_LED_MODE_KEEP);
        // Override switched off while blue (thread hasn't handed back yet): automatic, never forced by the entry rule
        in = inputs(50.0f);
        in.shellClosed = true;
        in.override = false;
        d = BatteryLedLogic_Decide(&l, &cfg, &in, &reason);
        CHECK(d == BATTERY_LED_MODE_AUTO && BatteryLedLogic_SleepWrite(d, reason, l.level, BATTERY_LED_MODE_AUTO, true) == BATTERY_LED_MODE_KEEP);

        // Mismatch watch: system (KEEP) and the animation are not watched while closed; 5 / 1 are
        BatteryLedWatch w;
        BatteryLedObserved o = { true, 1, 0, true, 2 };
        BatteryLedWatch_Init(&w);
        if (expBlue != BATTERY_LED_MODE_KEEP)
            BatteryLedWatch_Written(&w, expBlue, 0);
        CHECK(BatteryLedWatch_Mismatch(&w, expBlue, &o, 5000) == (expBlue == BATTERY_LED_MODE_BLUE || expBlue == BATTERY_LED_MODE_BLUE_FORCED));
    }

    // Full sequence through the write path (default setting 5): lid close writes 5 again, wake resumes, all
    // writes whitelisted, the shutdown latch still allows exactly one "power off"
    {
        FakeLed f;
        fakeInit(&f);
        BatteryLedConfig_Defaults(&cfg);
        cfg.override = true;
        BatteryLedLogic_Init(&l);
        fakeApply(&f, decide(&l, &cfg, 50.0f));
        CHECK(f.lastWritten == BATTERY_LED_MODE_BLUE && f.writes == 1);
        u8 m = sleepWrite(&l, &cfg, 50.0f, f.lastWritten, true);                   // 0x214
        CHECK(m == BATTERY_LED_MODE_BLUE && fakeWrite(&f, m) && f.writes == 2);
        CHECK(sleepWrite(&l, &cfg, 50.0f, f.lastWritten, false) == BATTERY_LED_MODE_KEEP); // 0x101
        fakeApply(&f, decide(&l, &cfg, 50.0f));                                       // wake
        CHECK(f.writes == 2 && f.lastWritten == BATTERY_LED_MODE_BLUE);
        cfg.sleepMode = BATTERY_LED_SLEEP_SYSTEM;
        CHECK(sleepWrite(&l, &cfg, 50.0f, f.lastWritten, true) == BATTERY_LED_MODE_KEEP);
        f.reg29 = BATTERY_LED_MODE_BLUE;
        fakeShutdown(&f);
        CHECK(f.powerOffWrites == 1 && f.writes == 3);
        cfg.sleepMode = BATTERY_LED_MODE_BLUE;
        CHECK(sleepWrite(&l, &cfg, 50.0f, BATTERY_LED_MODE_BLUE, true) == BATTERY_LED_MODE_BLUE && !fakeWrite(&f, BATTERY_LED_MODE_BLUE));
    }

    // Menu cycle and names
    CHECK(BatteryLedLogic_NextSleepSetting(5) == 1 && BatteryLedLogic_NextSleepSetting(1) == 2 &&
          BatteryLedLogic_NextSleepSetting(2) == BATTERY_LED_SLEEP_SYSTEM && BatteryLedLogic_NextSleepSetting(0) == 5 &&
          BatteryLedLogic_NextSleepSetting(7) == 5);
    CHECK(strcmp(BatteryLedLogic_SleepSettingName(0), "system") == 0 && strcmp(BatteryLedLogic_SleepSettingName(5), "blue(5)") == 0);
    // An invalid setting that bypassed Sanitize never reaches the MCU
    cfg.sleepMode = 3;
    CHECK(BatteryLedLogic_SleepModeForLevel(&cfg, BATTERY_LED_LEVEL_BLUE) == BATTERY_LED_MODE_KEEP);

    // config.ini: sleep_mode
    static const struct { const char *text; u8 expect; } parse[] = {
        { "sleep_mode=5\n", 5 }, { "sleep_mode=1\n", 1 }, { "sleep_mode=2\n", 2 }, { "sleep_mode=0\n", 0 },
        { "sleep_mode=system\n", 0 }, { "sleep_mode=system\r\n", 0 }, { "sleep_mode=system", 0 },
        { "sleep_mode=3\n", 5 }, { "sleep_mode=4\n", 5 }, { "sleep_mode=6\n", 5 }, { "sleep_mode=7\n", 5 },
        { "sleep_mode=255\n", 5 }, { "sleep_mode=99999\n", 5 }, { "sleep_mode=-1\n", 5 }, { "sleep_mode=x\n", 5 },
        { "sleep_mode=systemx\n", 5 }, { "sleep_mode=\n", 5 }, { "#sleep_mode=1\n", 5 }, { "sleep_modes=1\n", 5 },
        { "led_override=1\n", 5 }, { "", 5 },
    };
    for (u32 i = 0; i < sizeof(parse) / sizeof(parse[0]); i++)
    {
        BatteryLedConfig c;
        BatteryLedConfig_Defaults(&c);
        BatteryLedConfig_Parse(parse[i].text, &c);
        BatteryLedConfig_Sanitize(&c);
        if (c.sleepMode != parse[i].expect)
            printf("  sleep_mode parse case %lu (\"%s\"): got %u\n", (unsigned long)i, parse[i].text, c.sleepMode);
        CHECK(c.sleepMode == parse[i].expect);
    }
    {
        // Other keys unaffected; sleep_mode in a full file as written by BatteryLed_SetConfig
        BatteryLedConfig c;
        BatteryLedConfig_Defaults(&c);
        CHECK(BatteryLedConfig_Parse("# Battery Manager settings\nled_override=1\nred_pct=12\nblink_pct=4\nblue_mode=1\n"
                                     "# sleep_mode: 5 blue on/red off, 1 forced blue, 2 MCU sleep animation, 0 system (write nothing)\n"
                                     "sleep_mode=0\n", &c));
        BatteryLedConfig_Sanitize(&c);
        CHECK(c.override && c.redPct == 12 && c.blinkPct == 4 && c.blueMode == 1 && c.sleepMode == BATTERY_LED_SLEEP_SYSTEM);
        BatteryLedConfig_Defaults(&c);
        CHECK(BatteryLedConfig_Parse("sleep_mode=system\n", &c));
    }
}

// ---- Sleep transition (section 14): no re-assert over ptm's sleep / wake byte ----

// What the MCU shows for a reg 0x29 mode while the MCU itself thinks the battery is low (the case seen on the
// console): automatic and the sleep animation pick the red LED
static BatteryLedObserved observedFor(u8 reg29, u32 phase)
{
    BatteryLedObserved o;
    o.infoValid = true;
    o.modeValid = true;
    o.mode = reg29;
    switch (reg29)
    {
        case BATTERY_LED_MODE_BLUE:
        case BATTERY_LED_MODE_BLUE_FORCED:  o.red = 0; o.blue = 255; break;
        case BATTERY_LED_MODE_RED:
        case BATTERY_LED_MODE_AUTO:         o.red = 1; o.blue = 0; break;
        default:                            o.red = (u8)(phase & 1); o.blue = 0; break; // 2 / 6: red toggles
    }
    return o;
}

typedef struct FakeThread
{
    BatteryLedLogic logic;
    BatteryLedWatch watch;
    u8 reg29;
    u32 writes, reasserts, phase;
    u64 now;
} FakeThread;

static void ftWrite(FakeThread *t, u8 mode)
{
    CHECK(BatteryMcu_LedModeAllowed(mode));
    t->reg29 = mode;
    t->writes++;
    BatteryLedWatch_Written(&t->watch, mode, t->now);
}

// One poll of batteryLedControlLocked (fresh data, rate limit passed)
static void ftPoll(FakeThread *t, const BatteryLedConfig *cfg, float corrected, bool charging, bool shellClosed, bool sleepPending)
{
    t->now += 1000;
    t->phase++;
    BatteryLedInputs in = inputs(corrected);
    in.adapter = charging;
    in.charging = charging;
    in.shellClosed = shellClosed || sleepPending;
    in.lastWritten = t->watch.lastWritten;
    u8 reason;
    u8 desired = BatteryLedLogic_Decide(&t->logic, cfg, &in, &reason);
    if (desired == BATTERY_LED_MODE_KEEP)
        return;
    if (desired == BATTERY_LED_MODE_AUTO)
    {
        if (BatteryLedLogic_NeedsWrite(desired, t->watch.lastWritten))
            ftWrite(t, desired);
        return;
    }
    u8 act = BatteryLedLogic_ThreadAction(desired, t->watch.lastWritten, reason, sleepPending);
    BatteryLedObserved o = observedFor(t->reg29, t->phase);
    if (act == BATTERY_LED_ACTION_WRITE)
        ftWrite(t, desired);
    else if (act == BATTERY_LED_ACTION_CHECK && BatteryLedWatch_Mismatch(&t->watch, desired, &o, t->now))
    {
        t->reasserts++;
        ftWrite(t, desired);
    }
}

// The sleep hook at 0x214 (entering) / 0x101, then ptm's bytes as battery_led.c computes them
static void ftSleep(FakeThread *t, const BatteryLedConfig *cfg, float corrected, bool charging, bool entering, u8 *sleepByte, u8 *wakeByte)
{
    BatteryLedInputs in = inputs(corrected);
    in.adapter = charging;
    in.charging = charging;
    in.shellClosed = true;
    in.lastWritten = t->watch.lastWritten;
    u8 reason;
    u8 desired = BatteryLedLogic_Decide(&t->logic, cfg, &in, &reason);
    u8 m = BatteryLedLogic_SleepWrite(desired, reason, BatteryLedLogic_SleepLevel(cfg, t->logic.level, charging),
                                      t->watch.lastWritten, entering);
    if (m != BATTERY_LED_MODE_KEEP)
        ftWrite(t, m);

    BatteryLedLogic copy = t->logic;
    u8 r2;
    u8 d2 = BatteryLedLogic_Decide(&copy, cfg, &in, &r2);
    BatteryPtmPatch_DesiredBytes(cfg, d2, r2, sleepByte, wakeByte);
}

// One full lid close -> sleep -> lid open cycle at a fixed level. Returns the LED thread's re-asserts during it.
// ptmSleepOverride: what ptm actually sends at sleep (0xFF = the patched sleep byte; 2 = an unpatched ptm).
static u32 sleepCycle(FakeThread *t, const BatteryLedConfig *cfg, float corrected, bool charging, u8 ptmSleepOverride,
                      u8 *sleepSeen, u8 *wakeSeen)
{
    u32 before = t->reasserts;
    u8 sb, wb;
    ftSleep(t, cfg, corrected, charging, true, &sb, &wb);              // 0x214
    ftPoll(t, cfg, corrected, charging, true, false);                  // thread, lid closed
    ftSleep(t, cfg, corrected, charging, false, &sb, &wb);             // 0x101
    for (u32 i = 0; i < 3; i++)
        ftPoll(t, cfg, corrected, charging, true, true);               // thread, sleep pending
    t->reg29 = ptmSleepOverride == 0xFF ? sb : ptmSleepOverride;       // ptm's sleep LED callback
    *sleepSeen = t->reg29;
    for (u32 i = 0; i < 3; i++)
        ftPoll(t, cfg, corrected, charging, true, true);               // still before EnterSleep
    t->now += 3600000;                                                 // asleep (ARM11 off)
    t->reg29 = wb;                                                     // ptm's wake LED callback
    *wakeSeen = t->reg29;
    ftPoll(t, cfg, corrected, charging, true, false);                  // awake, 0x106 seen, lid still closed
    for (u32 i = 0; i < 3; i++)
        ftPoll(t, cfg, corrected, charging, false, false);             // lid open
    return t->reasserts - before;
}

static void ftInit(FakeThread *t, const BatteryLedConfig *cfg, float corrected, bool charging)
{
    memset(t, 0, sizeof(*t));
    BatteryLedLogic_Init(&t->logic);
    BatteryLedWatch_Init(&t->watch);
    t->now = 100000;
    for (u32 i = 0; i < 4; i++)
        ftPoll(t, cfg, corrected, charging, false, false);   // awake: first write + read-back becomes trusted
    CHECK(t->watch.readback == BATTERY_LED_READBACK_TRUSTED);
}

static void sleepTransitionTests(void)
{
    BatteryLedConfig cfg;
    BatteryLedConfig_Defaults(&cfg);
    cfg.override = true;
    FakeThread t;
    u8 sl, wk;

    // ThreadAction table
    CHECK(BatteryLedLogic_ThreadAction(5, 5, BATTERY_LED_REASON_ACTIVE, false) == BATTERY_LED_ACTION_CHECK);
    CHECK(BatteryLedLogic_ThreadAction(4, 5, BATTERY_LED_REASON_ACTIVE, false) == BATTERY_LED_ACTION_WRITE);
    CHECK(BatteryLedLogic_ThreadAction(4, 4, BATTERY_LED_REASON_CHARGER, false) == BATTERY_LED_ACTION_CHECK);
    CHECK(BatteryLedLogic_ThreadAction(4, 4, BATTERY_LED_REASON_SHELL, false) == BATTERY_LED_ACTION_NONE);
    CHECK(BatteryLedLogic_ThreadAction(6, 4, BATTERY_LED_REASON_SHELL, false) == BATTERY_LED_ACTION_WRITE);
    for (u32 d = 0; d < 256; d++)
        for (u32 lw = 0; lw < 256; lw += 17)
            for (u32 r = 0; r <= BATTERY_LED_REASON_TEST; r++)
            {
                u8 a = BatteryLedLogic_ThreadAction((u8)d, (u8)lw, (u8)r, true);
                CHECK(a == BATTERY_LED_ACTION_NONE); // sleep pending: never a write
                a = BatteryLedLogic_ThreadAction((u8)d, (u8)lw, (u8)r, false);
                if (r == BATTERY_LED_REASON_SHELL)
                    CHECK(a != BATTERY_LED_ACTION_CHECK); // lid closed: never a re-assert
                if (d == BATTERY_LED_MODE_KEEP || d == BATTERY_LED_MODE_AUTO)
                    CHECK(a == BATTERY_LED_ACTION_NONE);
            }

    // Charging at sleep entry: blue sleep level with blue(5) / blue(1); level as it is with anim(2) / system
    cfg.sleepMode = 5;
    CHECK(BatteryLedLogic_SleepLevel(&cfg, BATTERY_LED_LEVEL_RED, true) == BATTERY_LED_LEVEL_BLUE);
    CHECK(BatteryLedLogic_SleepLevel(&cfg, BATTERY_LED_LEVEL_BLINK, true) == BATTERY_LED_LEVEL_BLUE);
    CHECK(BatteryLedLogic_SleepLevel(&cfg, BATTERY_LED_LEVEL_RED, false) == BATTERY_LED_LEVEL_RED);
    cfg.sleepMode = 2;
    CHECK(BatteryLedLogic_SleepLevel(&cfg, BATTERY_LED_LEVEL_RED, true) == BATTERY_LED_LEVEL_RED);
    cfg.sleepMode = BATTERY_LED_SLEEP_SYSTEM;
    CHECK(BatteryLedLogic_SleepLevel(&cfg, BATTERY_LED_LEVEL_RED, true) == BATTERY_LED_LEVEL_RED);
    cfg.sleepMode = 5;

    // 1. Blue -> lid -> blue: ptm sends 5 at sleep and 5 on wake, no re-assert, no extra write
    ftInit(&t, &cfg, 50.0f, false);
    u32 w0 = t.writes;
    CHECK(sleepCycle(&t, &cfg, 50.0f, false, 0xFF, &sl, &wk) == 0);
    CHECK(sl == 5 && wk == 5 && t.reg29 == 5 && t.writes == w0 + 1); // the one forced sleep-entry write of 5

    // 2. Red -> lid -> steady red: ptm sends 4 / 4; the thread never re-asserts
    ftInit(&t, &cfg, 8.0f, false);
    CHECK(t.reg29 == BATTERY_LED_MODE_RED);
    w0 = t.writes;
    CHECK(sleepCycle(&t, &cfg, 8.0f, false, 0xFF, &sl, &wk) == 0);
    CHECK(sl == 4 && wk == 4 && t.reg29 == 4 && t.writes == w0);
    //    The fight seen on the console (ptm sending 2 at a red level): the mismatch watch would flag it awake,
    //    but the guard keeps the thread quiet during the transition
    ftInit(&t, &cfg, 8.0f, false);
    {
        BatteryLedObserved o = observedFor(2, 1);
        BatteryLedWatch wc = t.watch;
        CHECK(BatteryLedWatch_Mismatch(&wc, BATTERY_LED_MODE_RED, &o, t.now + 10000)); // would re-assert
    }
    w0 = t.writes;
    u32 ra = sleepCycle(&t, &cfg, 8.0f, false, 2, &sl, &wk);
    CHECK(ra == 0 && t.writes == w0);

    // 3. Blink level -> stock: ptm sends 2 at sleep (MCU's own low warning), 0 on wake; the thread restores 6
    //    once after wake (one re-assert per wake, after the lid is open)
    ftInit(&t, &cfg, 2.0f, false);
    CHECK(t.reg29 == BATTERY_LED_MODE_BLINK);
    ra = sleepCycle(&t, &cfg, 2.0f, false, 0xFF, &sl, &wk);
    CHECK(sl == 2 && wk == 0 && t.reg29 == 6 && ra == 1);

    // 4. Charging at a red level -> lid -> blue: the hook writes 5, ptm sends 5 / 5; on wake, still charging at red,
    //    the thread writes 4 again (a normal change, not a re-assert)
    ftInit(&t, &cfg, 8.0f, true);
    CHECK(t.reg29 == BATTERY_LED_MODE_RED);
    ra = sleepCycle(&t, &cfg, 8.0f, true, 0xFF, &sl, &wk);
    CHECK(sl == 5 && wk == 5 && ra == 0 && t.reg29 == 4);

    // 5. Lid open at a blue level: the wake byte is the awake blue mode, so 0x29 is right before the thread runs
    cfg.blueMode = 1;
    ftInit(&t, &cfg, 50.0f, false);
    ra = sleepCycle(&t, &cfg, 50.0f, false, 0xFF, &sl, &wk);
    CHECK(sl == 5 && wk == 1 && ra == 0 && t.reg29 == 1);
    cfg.blueMode = 5;

    // 6. sleep_mode system: ptm stock (2 / 0); nothing written at sleep; after wake the thread restores blue
    //    (the stock wake byte 0 shows as a mismatch once the lid is open, as before this change)
    cfg.sleepMode = BATTERY_LED_SLEEP_SYSTEM;
    ftInit(&t, &cfg, 50.0f, false);
    w0 = t.writes;
    ra = sleepCycle(&t, &cfg, 50.0f, false, 0xFF, &sl, &wk);
    CHECK(sl == 2 && wk == 0 && t.reg29 == 5 && ra == 1 && t.writes == w0 + 1);
    cfg.sleepMode = 5;

    printf("sleep transition: no re-assert between sleep entry and wake (blue / red / blink / charging / system)\n");
}

static void unitTests(void)
{
    BatteryLedConfig cfg;
    BatteryLedLogic l;
    BatteryLedInputs in;
    u8 reason;

    BatteryLedConfig_Defaults(&cfg);
    CHECK(!cfg.override && cfg.redPct == 10 && cfg.blinkPct == 3 && cfg.blueMode == 5 && cfg.sleepMode == 5);

    // Thresholds (corrected <= X: red, <= Y: blink)
    BatteryLedLogic_Init(&l);
    CHECK(decide(&l, &cfg, 50.0f) == BATTERY_LED_MODE_BLUE);
    CHECK(decide(&l, &cfg, 10.01f) == BATTERY_LED_MODE_BLUE);
    CHECK(decide(&l, &cfg, 10.0f) == BATTERY_LED_MODE_RED);
    CHECK(decide(&l, &cfg, 3.01f) == BATTERY_LED_MODE_RED);
    CHECK(decide(&l, &cfg, 3.0f) == BATTERY_LED_MODE_BLINK);

    // Only gets worse within a discharge (voltage recovery after a load drop)
    CHECK(decide(&l, &cfg, 20.0f) == BATTERY_LED_MODE_BLINK);
    BatteryLedLogic_Init(&l);
    CHECK(decide(&l, &cfg, 9.9f) == BATTERY_LED_MODE_RED);
    CHECK(decide(&l, &cfg, 10.4f) == BATTERY_LED_MODE_RED);
    CHECK(decide(&l, &cfg, 30.0f) == BATTERY_LED_MODE_RED);

    // Charging: controlled, MCU-table corrected %, only gets better (with hysteresis), never worse
    BatteryLedLogic_Init(&l);
    CHECK(decide(&l, &cfg, 2.0f) == BATTERY_LED_MODE_BLINK);
    in = inputs(2.0f);
    in.adapter = true;
    in.charging = true;
    CHECK(BatteryLedLogic_Decide(&l, &cfg, &in, &reason) == BATTERY_LED_MODE_BLINK && reason == BATTERY_LED_REASON_CHARGER);
    CHECK(decideCharging(&l, &cfg, 3.4f, true, 3.0f) == BATTERY_LED_MODE_BLINK);  // 3.4 <= 3.5: not yet
    CHECK(decideCharging(&l, &cfg, 3.6f, true, 3.0f) == BATTERY_LED_MODE_RED);    // > blink + 0.5
    CHECK(decideCharging(&l, &cfg, 1.0f, true, 3.0f) == BATTERY_LED_MODE_RED);    // never worse while charging
    CHECK(decideCharging(&l, &cfg, 10.4f, true, 9.0f) == BATTERY_LED_MODE_RED);
    CHECK(decideCharging(&l, &cfg, 10.6f, true, 9.0f) == BATTERY_LED_MODE_BLUE);  // > red + 0.5
    CHECK(decideCharging(&l, &cfg, 5.0f, true, 9.0f) == BATTERY_LED_MODE_BLUE);   // stays blue
    // Adapter without the charging flag (charge paused) at a low %: not "full", the ratchet still applies
    BatteryLedLogic_Init(&l);
    CHECK(decide(&l, &cfg, 8.0f) == BATTERY_LED_MODE_RED);
    CHECK(decideCharging(&l, &cfg, 8.0f, false, 20.0f) == BATTERY_LED_MODE_RED);
    // Fully charged (adapter in, not charging, MCU >= 95): blue whatever the curve says
    CHECK(decideCharging(&l, &cfg, 8.0f, false, 96.0f) == BATTERY_LED_MODE_BLUE);
    CHECK(decideCharging(&l, &cfg, 8.0f, true, 96.0f) == BATTERY_LED_MODE_BLUE);  // and never worse after
    // Unplugged: the discharge restarts from the corrected % at that moment (either direction)
    CHECK(decide(&l, &cfg, 9.0f) == BATTERY_LED_MODE_RED);    // blue during the charge, red now
    CHECK(decide(&l, &cfg, 30.0f) == BATTERY_LED_MODE_RED);   // and only worse again
    BatteryLedLogic_Init(&l);
    CHECK(decide(&l, &cfg, 2.0f) == BATTERY_LED_MODE_BLINK);
    CHECK(decideCharging(&l, &cfg, 3.2f, true, 2.0f) == BATTERY_LED_MODE_BLINK); // short charge, no improvement
    CHECK(decide(&l, &cfg, 20.0f) == BATTERY_LED_MODE_BLUE);  // unplugged at corrected 20: restarts from there
    CHECK(decide(&l, &cfg, 9.0f) == BATTERY_LED_MODE_RED);
    BatteryLedLogic_Init(&l);
    CHECK(decide(&l, &cfg, 2.0f) == BATTERY_LED_MODE_BLINK);
    CHECK(decideCharging(&l, &cfg, 40.0f, true, 40.0f) == BATTERY_LED_MODE_BLUE);
    CHECK(decide(&l, &cfg, 40.0f) == BATTERY_LED_MODE_BLUE);  // unplugged after charging
    CHECK(decide(&l, &cfg, 2.0f) == BATTERY_LED_MODE_BLINK);
    CHECK(decideCharging(&l, &cfg, 2.2f, true, 1.0f) == BATTERY_LED_MODE_BLINK);
    CHECK(decide(&l, &cfg, 2.5f) == BATTERY_LED_MODE_BLINK);  // unplugged again right away: still low
    // Boot on the charger: starts from the corrected % as it is
    BatteryLedLogic_Init(&l);
    CHECK(decideCharging(&l, &cfg, 5.0f, true, 1.0f) == BATTERY_LED_MODE_RED);
    BatteryLedLogic_Init(&l);
    CHECK(decideCharging(&l, &cfg, 50.0f, true, 30.0f) == BATTERY_LED_MODE_BLUE);
    // Charging + override off / no curve: automatic (nothing to write if nothing was written)
    in = inputs(50.0f);
    in.adapter = true;
    in.override = false;
    CHECK(BatteryLedLogic_Decide(&l, &cfg, &in, &reason) == BATTERY_LED_MODE_AUTO && reason == BATTERY_LED_REASON_OFF);
    in = inputs(-1.0f);
    in.adapter = true;
    CHECK(BatteryLedLogic_Decide(&l, &cfg, &in, &reason) == BATTERY_LED_MODE_AUTO && reason == BATTERY_LED_REASON_NO_CURVE);
    CHECK(!BatteryLedLogic_NeedsWrite(BATTERY_LED_MODE_AUTO, BATTERY_LED_MODE_UNKNOWN));
    // Mismatch watch keeps working while charging (desired blue, MCU shows red)
    {
        BatteryLedWatch cw;
        BatteryLedObserved co = { true, 1, 0, true, 5 };
        BatteryLedWatch_Init(&cw);
        BatteryLedWatch_Written(&cw, decideCharging(&l, &cfg, 50.0f, true, 30.0f), 0);
        CHECK(BatteryLedWatch_Mismatch(&cw, BATTERY_LED_MODE_BLUE, &co, 2000));
    }

    // Threshold edits: may get better only with hysteresis (> threshold + 0.5)
    BatteryLedLogic_Init(&l);
    CHECK(decide(&l, &cfg, 9.0f) == BATTERY_LED_MODE_RED);
    cfg.redPct = 8;
    BatteryLedLogic_Reevaluate(&l);
    CHECK(decide(&l, &cfg, 9.0f) == BATTERY_LED_MODE_BLUE);  // 9.0 > 8.5
    cfg.redPct = 10;
    CHECK(decide(&l, &cfg, 9.0f) == BATTERY_LED_MODE_RED);
    cfg.redPct = 8;
    BatteryLedLogic_Reevaluate(&l);
    CHECK(decide(&l, &cfg, 8.3f) == BATTERY_LED_MODE_RED);   // 8.3 <= 8.5: stays red
    CHECK(decide(&l, &cfg, 9.0f) == BATTERY_LED_MODE_RED);   // re-evaluation is one-shot
    cfg.blinkPct = 5;                                         // raising a threshold can make it worse at once
    BatteryLedLogic_Reevaluate(&l);
    CHECK(decide(&l, &cfg, 4.5f) == BATTERY_LED_MODE_BLINK);
    cfg.redPct = 10;
    cfg.blinkPct = 3;
    BatteryLedLogic_Reevaluate(&l);
    CHECK(decide(&l, &cfg, 4.5f) == BATTERY_LED_MODE_RED);   // 4.5 > 3.5 but <= 10.5
    cfg.blueMode = BATTERY_LED_MODE_BLUE_FORCED;
    BatteryLedLogic_Init(&l);
    CHECK(decide(&l, &cfg, 50.0f) == BATTERY_LED_MODE_BLUE_FORCED);
    cfg.blueMode = BATTERY_LED_MODE_BLUE;

    // Sleep (lid closed / sleep requested) with sleep_mode=2 (the milestone 4b behaviour):
    // blue -> sleep animation (2), red / blinking red kept, never automatic
    cfg.sleepMode = BATTERY_LED_MODE_SLEEP;
    BatteryLedLogic_Init(&l);
    CHECK(decide(&l, &cfg, 50.0f) == BATTERY_LED_MODE_BLUE);
    CHECK(decideShell(&l, &cfg, 50.0f, BATTERY_LED_MODE_BLUE, &reason) == BATTERY_LED_MODE_SLEEP && reason == BATTERY_LED_REASON_SHELL);
    CHECK(decideShell(&l, &cfg, 50.0f, BATTERY_LED_MODE_SLEEP, NULL) == BATTERY_LED_MODE_SLEEP);
    CHECK(!BatteryLedLogic_NeedsWrite(BATTERY_LED_MODE_SLEEP, BATTERY_LED_MODE_SLEEP));       // written once
    CHECK(decide(&l, &cfg, 50.0f) == BATTERY_LED_MODE_BLUE);                                  // wake: resumes
    CHECK(BatteryLedLogic_NeedsWrite(BATTERY_LED_MODE_BLUE, BATTERY_LED_MODE_SLEEP));
    cfg.blueMode = BATTERY_LED_MODE_BLUE_FORCED;
    CHECK(decideShell(&l, &cfg, 50.0f, BATTERY_LED_MODE_BLUE_FORCED, NULL) == BATTERY_LED_MODE_SLEEP);
    cfg.blueMode = BATTERY_LED_MODE_BLUE;
    CHECK(decide(&l, &cfg, 9.0f) == BATTERY_LED_MODE_RED);
    CHECK(decideShell(&l, &cfg, 9.0f, BATTERY_LED_MODE_RED, NULL) == BATTERY_LED_MODE_RED);
    CHECK(!BatteryLedLogic_NeedsWrite(BATTERY_LED_MODE_RED, BATTERY_LED_MODE_RED));           // nothing written
    CHECK(decide(&l, &cfg, 10.3f) == BATTERY_LED_MODE_RED);  // voltage recovered during sleep: still red
    CHECK(decideShell(&l, &cfg, 2.0f, BATTERY_LED_MODE_RED, NULL) == BATTERY_LED_MODE_BLINK); // got worse while closed
    CHECK(decideShell(&l, &cfg, 2.0f, BATTERY_LED_MODE_BLINK, NULL) == BATTERY_LED_MODE_BLINK);
    CHECK(decide(&l, &cfg, 2.0f) == BATTERY_LED_MODE_BLINK);
    // Lid closed while charging: same mapping
    BatteryLedLogic_Init(&l);
    in = inputs(50.0f);
    in.adapter = true;
    in.charging = true;
    in.shellClosed = true;
    CHECK(BatteryLedLogic_Decide(&l, &cfg, &in, &reason) == BATTERY_LED_MODE_SLEEP && reason == BATTERY_LED_REASON_SHELL);
    // Nothing written this boot / last write was automatic: write nothing on sleep
    BatteryLedLogic_Init(&l);
    CHECK(decideShell(&l, &cfg, 50.0f, BATTERY_LED_MODE_UNKNOWN, NULL) == BATTERY_LED_MODE_KEEP);
    CHECK(decideShell(&l, &cfg, 50.0f, BATTERY_LED_MODE_AUTO, NULL) == BATTERY_LED_MODE_KEEP);
    CHECK(!BatteryLedLogic_NeedsWrite(BATTERY_LED_MODE_KEEP, BATTERY_LED_MODE_BLUE));
    // Override off while the lid is closed: automatic, only written to undo our own write
    in = inputs(50.0f);
    in.shellClosed = true;
    in.override = false;
    in.lastWritten = BATTERY_LED_MODE_UNKNOWN;
    CHECK(BatteryLedLogic_Decide(&l, &cfg, &in, &reason) == BATTERY_LED_MODE_AUTO && reason == BATTERY_LED_REASON_OFF);
    CHECK(!BatteryLedLogic_NeedsWrite(BATTERY_LED_MODE_AUTO, in.lastWritten));

    // Sleep / wake sequence through the write path
    {
        FakeLed f;
        fakeInit(&f);
        BatteryLedLogic_Init(&l);
        in = inputs(50.0f);
        in.lastWritten = f.lastWritten;
        fakeApply(&f, BatteryLedLogic_Decide(&l, &cfg, &in, NULL));
        CHECK(f.lastWritten == BATTERY_LED_MODE_BLUE && f.writes == 1);
        in.lastWritten = f.lastWritten;
        in.shellClosed = true;                                              // 0x214
        fakeApply(&f, BatteryLedLogic_Decide(&l, &cfg, &in, NULL));
        in.lastWritten = f.lastWritten;
        fakeApply(&f, BatteryLedLogic_Decide(&l, &cfg, &in, NULL));        // 0x101 / thread poll: no 2nd write
        CHECK(f.lastWritten == BATTERY_LED_MODE_SLEEP && f.writes == 2);
        in.shellClosed = false;                                             // 0x213
        in.lastWritten = f.lastWritten;
        fakeApply(&f, BatteryLedLogic_Decide(&l, &cfg, &in, NULL));
        CHECK(f.lastWritten == BATTERY_LED_MODE_BLUE && f.writes == 3);
    }
    cfg.sleepMode = BATTERY_LED_DEFAULT_SLEEP_MODE;

    sleepSettingTests();
    sleepTransitionTests();

    // Shutdown latch: "power off" exactly once at the first signal, nothing afterwards
    {
        FakeLed f;
        fakeInit(&f);
        fakeApply(&f, BATTERY_LED_MODE_BLUE);
        f.reg29 = BATTERY_LED_MODE_BLUE;
        fakeShutdown(&f);                                                   // 0x108
        CHECK(f.powerOffWrites == 1 && f.lastWritten == BATTERY_LED_MODE_POWER_OFF && f.writes == 2);
        fakeShutdown(&f);                                                   // 0x2000 pre-termination
        fakeShutdown(&f);                                                   // thread exit
        in = inputs(50.0f);
        in.shuttingDown = true;
        in.lastWritten = f.lastWritten;
        CHECK(BatteryLedLogic_Decide(&l, &cfg, &in, &reason) == BATTERY_LED_MODE_KEEP && reason == BATTERY_LED_REASON_SHUTDOWN);
        fakeApply(&f, BatteryLedLogic_Decide(&l, &cfg, &in, NULL));        // thread poll
        CHECK(!fakeWrite(&f, BATTERY_LED_MODE_AUTO));                       // menu / hand-back / error retry
        CHECK(!fakeWrite(&f, BATTERY_LED_MODE_BLUE));                       // LED test screen
        CHECK(!fakeWrite(&f, BATTERY_LED_MODE_POWER_OFF));
        CHECK(f.writes == 2 && f.powerOffWrites == 1 && !BatteryLedGate_IsOpen(&f.gate));

        // Pre-termination first (no 0x108), from the sleep animation or red: also exactly once
        fakeInit(&f);
        fakeApply(&f, BATTERY_LED_MODE_SLEEP);
        fakeShutdown(&f);
        fakeShutdown(&f);
        CHECK(f.writes == 2 && f.powerOffWrites == 1);
        fakeInit(&f);
        fakeApply(&f, BATTERY_LED_MODE_BLINK);
        fakeShutdown(&f);
        CHECK(f.writes == 2 && f.powerOffWrites == 1);

        // The system has already set "power off" (0x29 reads 3): nothing written, and nothing later
        fakeInit(&f);
        fakeApply(&f, BATTERY_LED_MODE_BLUE);
        f.reg29 = BATTERY_LED_MODE_POWER_OFF;
        fakeShutdown(&f);
        fakeShutdown(&f);
        CHECK(f.writes == 1 && f.powerOffWrites == 0 && !fakeWrite(&f, BATTERY_LED_MODE_AUTO));

        // Nothing written this boot (override off): nothing on shutdown, nothing after
        fakeInit(&f);
        fakeShutdown(&f);
        fakeShutdown(&f);
        CHECK(f.writes == 0 && !fakeWrite(&f, BATTERY_LED_MODE_AUTO) && f.writes == 0);
        // Last write was automatic (override switched off earlier): we don't own the LED, nothing written
        fakeInit(&f);
        fakeApply(&f, BATTERY_LED_MODE_BLUE);
        fakeApply(&f, BATTERY_LED_MODE_AUTO);
        fakeShutdown(&f);
        CHECK(f.writes == 2 && f.powerOffWrites == 0);
        // Read of 0x29 failed: write "power off" anyway (once)
        BatteryLedGate g;
        BatteryLedGate_Init(&g);
        CHECK(BatteryLedGate_Shutdown(&g, BATTERY_LED_MODE_RED, false, BATTERY_LED_MODE_POWER_OFF) == BATTERY_LED_MODE_POWER_OFF);
        CHECK(!BatteryLedGate_Allow(&g, BATTERY_LED_MODE_AUTO));   // only "power off" may use the last write
        CHECK(BatteryLedGate_Allow(&g, BATTERY_LED_MODE_POWER_OFF));
        CHECK(!BatteryLedGate_Allow(&g, BATTERY_LED_MODE_POWER_OFF));
        CHECK(BatteryLedGate_Shutdown(&g, BATTERY_LED_MODE_RED, false, 0) == BATTERY_LED_MODE_KEEP);
    }

    // Override off, no curve, error -> automatic; never written if nothing was written this boot
    in = inputs(50.0f);
    in.override = false;
    CHECK(BatteryLedLogic_Decide(&l, &cfg, &in, &reason) == BATTERY_LED_MODE_AUTO && reason == BATTERY_LED_REASON_OFF);
    in = inputs(-1.0f);
    CHECK(BatteryLedLogic_Decide(&l, &cfg, &in, &reason) == BATTERY_LED_MODE_AUTO && reason == BATTERY_LED_REASON_NO_CURVE);
    in = inputs(50.0f);
    in.error = true;
    CHECK(BatteryLedLogic_Decide(&l, &cfg, &in, &reason) == BATTERY_LED_MODE_AUTO && reason == BATTERY_LED_REASON_ERROR);
    CHECK(BatteryLedLogic_NeedsWrite(BATTERY_LED_MODE_AUTO, BATTERY_LED_MODE_BLUE));
    CHECK(!BatteryLedLogic_NeedsWrite(BATTERY_LED_MODE_AUTO, BATTERY_LED_MODE_AUTO));
    CHECK(!BatteryLedLogic_NeedsWrite(BATTERY_LED_MODE_AUTO, BATTERY_LED_MODE_UNKNOWN));

    // Whitelist: reg 0x29 modes 0..6 only
    for (u32 m = 0; m <= 6; m++)
        CHECK(BatteryMcu_LedModeAllowed(m));
    static const u32 refused[] = { 7, 8, 0x10, 0x7F, 0x80, BATTERY_LED_MODE_KEEP, BATTERY_LED_MODE_UNKNOWN, 0x100, 0x106, 0xFFFFFFFFu };
    for (u32 i = 0; i < sizeof(refused) / sizeof(refused[0]); i++)
        CHECK(!BatteryMcu_LedModeAllowed(refused[i]));

    // Mismatch detection
    BatteryLedWatch w;
    BatteryLedObserved o = { true, 0, 255, false, 0 };
    BatteryLedWatch_Init(&w);
    CHECK(!BatteryLedWatch_Mismatch(&w, BATTERY_LED_MODE_BLUE, &o, 10000)); // not written yet
    BatteryLedWatch_Written(&w, BATTERY_LED_MODE_BLUE, 10000);
    o.red = 1;
    CHECK(!BatteryLedWatch_Mismatch(&w, BATTERY_LED_MODE_BLUE, &o, 10500)); // grace
    CHECK(BatteryLedWatch_Mismatch(&w, BATTERY_LED_MODE_BLUE, &o, 11000));
    o.red = 0; o.blue = 0;
    CHECK(BatteryLedWatch_Mismatch(&w, BATTERY_LED_MODE_BLUE, &o, 11000));
    o.blue = 200;
    CHECK(!BatteryLedWatch_Mismatch(&w, BATTERY_LED_MODE_BLUE, &o, 11000));
    CHECK(!BatteryLedWatch_Mismatch(&w, BATTERY_LED_MODE_AUTO, &o, 11000));
    BatteryLedWatch_Written(&w, BATTERY_LED_MODE_SLEEP, 12000);   // sleep animation / power off: not watched
    o.red = 1; o.blue = 0;
    CHECK(!BatteryLedWatch_Mismatch(&w, BATTERY_LED_MODE_SLEEP, &o, 20000));
    BatteryLedWatch_Written(&w, BATTERY_LED_MODE_POWER_OFF, 12000);
    CHECK(!BatteryLedWatch_Mismatch(&w, BATTERY_LED_MODE_POWER_OFF, &o, 20000));

    BatteryLedWatch_Written(&w, BATTERY_LED_MODE_RED, 20000);
    o.red = 1; o.blue = 0;
    CHECK(!BatteryLedWatch_Mismatch(&w, BATTERY_LED_MODE_RED, &o, 22000)); // red grace 3 s
    o.blue = 100;
    CHECK(!BatteryLedWatch_Mismatch(&w, BATTERY_LED_MODE_RED, &o, 22999));
    CHECK(BatteryLedWatch_Mismatch(&w, BATTERY_LED_MODE_RED, &o, 23000));
    o.blue = 0; o.red = 0;
    CHECK(!BatteryLedWatch_Mismatch(&w, BATTERY_LED_MODE_RED, &o, 24000)); // red off once
    CHECK(BatteryLedWatch_Mismatch(&w, BATTERY_LED_MODE_RED, &o, 25000));  // twice in a row
    o.red = 0;
    CHECK(!BatteryLedWatch_Mismatch(&w, BATTERY_LED_MODE_RED, &o, 26000));
    o.red = 1;
    CHECK(!BatteryLedWatch_Mismatch(&w, BATTERY_LED_MODE_RED, &o, 27000));
    o.red = 0;
    CHECK(!BatteryLedWatch_Mismatch(&w, BATTERY_LED_MODE_RED, &o, 28000)); // streak was reset

    BatteryLedWatch_Written(&w, BATTERY_LED_MODE_BLINK, 30000);
    for (u32 i = 0; i < 6; i++)
    {
        o.red = (u8)(i & 1); o.blue = 0;
        CHECK(!BatteryLedWatch_Mismatch(&w, BATTERY_LED_MODE_BLINK, &o, 33000 + 250 * i)); // red toggles: fine
    }
    o.blue = 10;
    CHECK(BatteryLedWatch_Mismatch(&w, BATTERY_LED_MODE_BLINK, &o, 35000));

    // Reg 0x29 read-back: trusted once it reads back what was written, ignored otherwise
    BatteryLedWatch_Init(&w);
    BatteryLedWatch_Written(&w, BATTERY_LED_MODE_BLUE, 0);
    BatteryLedObserved ob = { true, 0, 255, true, 5 };
    CHECK(!BatteryLedWatch_Mismatch(&w, BATTERY_LED_MODE_BLUE, &ob, 2000) && w.readback == BATTERY_LED_READBACK_TRUSTED);
    ob.mode = 0;
    CHECK(BatteryLedWatch_Mismatch(&w, BATTERY_LED_MODE_BLUE, &ob, 2250));
    BatteryLedWatch_Init(&w);
    BatteryLedWatch_Written(&w, BATTERY_LED_MODE_BLUE, 0);
    CHECK(!BatteryLedWatch_Mismatch(&w, BATTERY_LED_MODE_BLUE, &ob, 2000) && w.readback == BATTERY_LED_READBACK_UNTRUSTED);
    CHECK(!BatteryLedWatch_Mismatch(&w, BATTERY_LED_MODE_BLUE, &ob, 2250));

    // Voltage median
    BatteryLedSmoother sm;
    BatteryLedSmoother_Init(&sm);
    CHECK(BatteryLedSmoother_Median(&sm) == 0.0f);
    BatteryLedSmoother_Push(&sm, 170);
    CHECK(BatteryLedSmoother_Median(&sm) == 170.0f);
    BatteryLedSmoother_Push(&sm, 160);
    CHECK(BatteryLedSmoother_Median(&sm) == 165.0f);
    BatteryLedSmoother_Push(&sm, 200);
    CHECK(BatteryLedSmoother_Median(&sm) == 170.0f);
    static const u8 more[] = { 150, 171, 172, 173, 174 };
    for (u32 i = 0; i < sizeof(more); i++)
        BatteryLedSmoother_Push(&sm, more[i]); // window now 173,174,200,150,171,172
    CHECK(BatteryLedSmoother_Median(&sm) == 172.5f);

    CHECK(BatteryLedLogic_NearMcuEvent(10.5f) && BatteryLedLogic_NearMcuEvent(4.4f) && BatteryLedLogic_NearMcuEvent(0.0f));
    CHECK(!BatteryLedLogic_NearMcuEvent(10.7f) && !BatteryLedLogic_NearMcuEvent(1.0f) && !BatteryLedLogic_NearMcuEvent(50.0f));

    // Config parsing / sanitizing
    BatteryLedConfig c;
    BatteryLedConfig_Defaults(&c);
    CHECK(!BatteryLedConfig_Parse("", &c) && !c.override);
    CHECK(BatteryLedConfig_Parse("# comment\nled_override=1\r\nred_pct=12\nblink_pct=4\nblue_mode=1\nfoo=3\n", &c));
    BatteryLedConfig_Sanitize(&c);
    CHECK(c.override && c.redPct == 12 && c.blinkPct == 4 && c.blueMode == 1);
    BatteryLedConfig_Defaults(&c);
    BatteryLedConfig_Parse("red_pct=5\nblink_pct=9\nblue_mode=3\nled_override=x\n", &c);
    BatteryLedConfig_Sanitize(&c);
    CHECK(!c.override && c.redPct == 5 && c.blinkPct == 4 && c.blueMode == 5);
    BatteryLedConfig_Parse("red_pct=0\n", &c);
    BatteryLedConfig_Sanitize(&c);
    CHECK(c.redPct == 1 && c.blinkPct == 0);
    BatteryLedConfig_Parse("red_pct=99999\n", &c);
    BatteryLedConfig_Sanitize(&c);
    CHECK(c.redPct == BATTERY_LED_MAX_RED_PCT);

    printf("unit tests: %s\n", g_failures == 0 ? "OK" : "FAILED");
}

// ---- Replay ----

static BatteryCurveFile g_curve;

typedef struct Row { u32 t; float pct; u8 volt; u8 flags; } Row;
static Row g_rows[20000];

static u32 readSession(const char *path, u32 *number)
{
    char line[256];
    u32 n = 0;
    FILE *f = fopen(path, "r");
    if (f == NULL)
    {
        perror(path);
        exit(2);
    }
    *number = 0;
    while (fgets(line, sizeof(line), f) != NULL)
    {
        unsigned t, pi, pf, v, temp, flags;
        long long wall;
        if (strncmp(line, "# session=", 10) == 0)
            *number = (u32)atoi(line + 10);
        if (n < sizeof(g_rows) / sizeof(g_rows[0]) &&
            sscanf(line, "%u,%lld,%u,%u,%*[^,],%u,%*[^,],%u,0x%x,", &t, &wall, &pi, &pf, &v, &temp, &flags) == 7)
        {
            g_rows[n].t = t;
            g_rows[n].pct = pi + pf / 256.0f;
            g_rows[n].volt = (u8)v;
            g_rows[n].flags = (u8)flags;
            n++;
        }
    }
    fclose(f);
    return n;
}

static void fmtMin(char *out, u32 ms)
{
    u32 m = ms / 60000;
    sprintf(out, "%u:%02u", m / 60, m % 60);
}

static void replay(const char *path)
{
    u32 number;
    u32 n = readSession(path, &number);
    if (n == 0)
        return;

    BatteryLedConfig cfg;
    BatteryLedConfig_Defaults(&cfg);
    cfg.override = true;
    BatteryLedLogic l;
    BatteryLedLogic_Init(&l);
    BatteryLedSmoother sm;
    BatteryLedSmoother_Init(&sm);

    u32 tEnd = g_rows[n - 1].t;
    u8 prevMode = 0xFF;
    u32 changes = 0, rawFlaps = 0;
    u8 rawPrev = 0xFF;
    u64 nextVolt = 0;
    float minCorr = 1000.0f;

    printf("\nReplay #%04u (%s): %u samples, %.2f h, override on, red %u %%, blink %u %%, blue mode %u\n",
           number, path, n, tEnd / 3600000.0, cfg.redPct, cfg.blinkPct, cfg.blueMode);
    printf("  %-7s %-8s %7s %4s %8s %9s  %s\n", "elapsed", "left", "MCU %", "volt", "median", "corrected", "LED");

    bool prevCharger = false;
    for (u32 i = 0; i < n; i++)
    {
        const Row *r = &g_rows[i];
        // Same as batteryLedPoll(): median reset on unplug, MCU table only on the adapter
        bool charger = (r->flags & 0x18) != 0;
        if (prevCharger && !charger)
            BatteryLedSmoother_Init(&sm);
        prevCharger = charger;
        if (sm.count == 0 || r->t >= nextVolt)
        {
            BatteryLedSmoother_Push(&sm, r->volt);
            nextVolt = (u64)r->t + BATTERY_LED_VOLT_SAMPLE_MS;
        }
        float med = BatteryLedSmoother_Median(&sm);
        float c = BatteryCurve_CorrectedFrom(&g_curve, r->pct, charger ? 0.0f : med);
        if (c < minCorr)
            minCorr = c;

        BatteryLedInputs in = inputs(c);
        in.adapter = (r->flags & 0x08) != 0;
        in.charging = (r->flags & 0x10) != 0;
        in.shellClosed = (r->flags & 0x02) == 0;
        in.pct = r->pct;
        in.lastWritten = prevMode == 0xFF ? BATTERY_LED_MODE_UNKNOWN : prevMode;
        u8 mode = BatteryLedLogic_Decide(&l, &cfg, &in, NULL);

        // Same thresholds without the "only gets worse" rule, to count the flapping it prevents
        u8 raw = c <= cfg.blinkPct ? 2 : c <= cfg.redPct ? 1 : 0;
        if (rawPrev != 0xFF && raw != rawPrev)
            rawFlaps++;
        rawPrev = raw;

        if (mode != prevMode)
        {
            char el[16], left[16];
            fmtMin(el, r->t);
            fmtMin(left, tEnd - r->t);
            printf("  %-7s %-8s %7.3f %4u %8.1f %9.2f  %s\n", el, left, r->pct, r->volt, med, c, BatteryLedLogic_ModeName(mode));
            if (prevMode != 0xFF)
                changes++;
            // Order must be blue -> red -> blink
            CHECK(prevMode == 0xFF || (prevMode == cfg.blueMode && mode == BATTERY_LED_MODE_RED) ||
                  (prevMode == BATTERY_LED_MODE_RED && mode == BATTERY_LED_MODE_BLINK));
            prevMode = mode;
        }
    }
    printf("  LED changes: %u (expected 2: blue -> red -> blink), threshold crossings without the ratchet: %u, min corrected %.2f\n",
           changes, rawFlaps, minCorr);
    CHECK(changes == 2);
}

int main(int argc, char **argv)
{
    unitTests();

    if (argc < 2)
    {
        printf("replay: SKIPPED (no curve.bin / session CSVs given; usage: %s curve.bin session.csv [...])\n", argv[0]);
        printf("\n%s\n", g_failures == 0 ? "led host test: OK (unit tests only)" : "led host test: FAILED");
        return g_failures == 0 ? 0 : 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (f == NULL || fread(&g_curve, sizeof(g_curve), 1, f) != 1 || !BatteryCurve_FileIsValid(&g_curve))
    {
        fprintf(stderr, "%s: not a valid curve.bin\n", argv[1]);
        return 2;
    }
    fclose(f);
    printf("curve: %u session(s):", g_curve.numSessions);
    for (u32 i = 0; i < g_curve.numSessions; i++)
        printf(" #%04u", g_curve.sessions[i]);
    printf("\n");

    for (int i = 2; i < argc; i++)
        replay(argv[i]);

    printf("\n%s\n", g_failures == 0 ? "led host test: OK" : "led host test: FAILED");
    return g_failures == 0 ? 0 : 1;
}
