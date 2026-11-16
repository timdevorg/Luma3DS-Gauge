/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Host test for the ptm sleep / wake LED patch, pure part
*   (sysmodules/rosalina/source/battery/battery_ptm_patch_logic.c, with battery_led_logic.c).
*
*   1. Decision: both bytes over the full matrix level x charging x override x curve x Sleep LED x blue
*      mode x error x shutdown x "nothing written yet", through the same path as battery_led.c
*      (BatteryLedLogic_Decide with the lid closed on a copy -> BatteryPtmPatch_DesiredBytes), against an
*      independent statement of the rules (docs/battery-manager.md, section 14.2). The sleep byte, when not
*      stock, equals what the sleep hook writes to the MCU (no conflicting writes).
*   2. Verification: both patterns accept byte 10 exactly within their whitelists (sleep 01/02/04/05, wake
*      00/01/04/05) and reject every other single-byte change at the other 15 positions; the whitelists.
*   3. Optional: the real ptm .code dump (New 3DS, 0004013020002202, GodMode9 "Extract .code"): the sleep
*      pattern is at file offset 0x1a1e (VA 0x101a1e), the wake pattern at 0x1994 (VA 0x101994), both stock,
*      each exactly once in the file (also with any value in byte 10), both on page 0x101000.
*
*   Usage: ptm_host_test [0004013020002202.dec.code]
*   Without a dump (or if the file doesn't exist) part 3 is skipped.
*   Run it through tests/battery/run_host_test.sh (PTM_CODE=<path>).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "battery/battery_led_logic.h"
#include "battery/battery_ptm_patch.h"

static int g_failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

enum { LV_BLUE = 0, LV_RED, LV_BLINK, LV_COUNT };
enum { CH_NONE = 0, CH_ADAPTER, CH_CHARGING, CH_FULL, CH_COUNT };   // full: adapter in, not charging, MCU 100 %

typedef struct Case
{
    u8 level;           // LV_*, via the corrected % (default thresholds 10 / 3)
    u8 charge;          // CH_*
    bool override, curve, error, shuttingDown;
    u8 sleepMode;       // 1 dim, 5 bright, 0 off (2: unsanitized legacy, acts as off)
    u8 blueMode;        // 5, 1
    u8 lastWritten;     // 5 (something written) / UNKNOWN / AUTO
} Case;

static float correctedFor(u8 level)
{
    return level == LV_BLUE ? 50.0f : level == LV_RED ? 8.0f : 2.0f;
}

static void configFor(const Case *c, BatteryLedConfig *cfg)
{
    BatteryLedConfig_Defaults(cfg);
    cfg->override = c->override;
    cfg->sleepMode = c->sleepMode;
    cfg->blueMode = c->blueMode;
    BatteryLedConfig_Sanitize(cfg);
}

static void inputsFor(const Case *c, BatteryLedInputs *in)
{
    memset(in, 0, sizeof(*in));
    in->override = c->override;
    in->adapter = c->charge == CH_ADAPTER || c->charge == CH_FULL;
    in->charging = c->charge == CH_CHARGING;
    in->shellClosed = true;
    in->shuttingDown = c->shuttingDown;
    in->error = c->error;
    in->pct = c->charge == CH_FULL ? 100.0f : 50.0f;
    in->corrected = c->curve ? correctedFor(c->level) : -1.0f;
    in->lastWritten = c->lastWritten;
}

// Same as battery_led.c batteryLedPtmPatchLocked: Decide on a copy with the lid closed, then DesiredBytes.
// Also returns what the sleep hook (batteryLedSleepLocked, entering) would write to the MCU.
static void ptmBytes(const Case *c, u8 *sleepByte, u8 *wakeByte, u8 *hookWrite)
{
    BatteryLedConfig cfg;
    configFor(c, &cfg);
    BatteryLedLogic l;
    BatteryLedLogic_Init(&l);
    BatteryLedInputs in;
    inputsFor(c, &in);

    u8 reason;
    u8 mode = BatteryLedLogic_Decide(&l, &cfg, &in, &reason);
    BatteryPtmPatch_DesiredBytes(&cfg, mode, reason, sleepByte, wakeByte);

    bool charger = in.adapter || in.charging;
    *hookWrite = BatteryLedLogic_SleepWrite(mode, reason, BatteryLedLogic_SleepLevel(&cfg, l.level, charger), in.lastWritten, true);
}

// The rules, stated independently of the code (section 14.2)
static void expectedBytes(const Case *c, u8 *sleepByte, u8 *wakeByte)
{
    *sleepByte = 0x02;
    *wakeByte = 0x00;
    bool active = c->override && c->curve && !c->error && !c->shuttingDown &&
                  c->lastWritten != BATTERY_LED_MODE_UNKNOWN && c->lastWritten != BATTERY_LED_MODE_AUTO;
    if (!active || (c->sleepMode != 5 && c->sleepMode != 1))
        return;
    bool fullyCharged = c->charge == CH_FULL;   // the charge ratchet makes this blue anyway
    if (c->charge != CH_NONE || c->level == LV_BLUE || fullyCharged)
    {
        *sleepByte = c->sleepMode;
        *wakeByte = c->blueMode;
    }
    else if (c->level == LV_RED)
    {
        *sleepByte = 0x04;
        *wakeByte = 0x04;
    }
    // blink: stock
}

static void testDecisionMatrix(void)
{
    static const u8 sleepModes[] = { 1, 5, BATTERY_LED_SLEEP_SYSTEM };  // dim, bright, off
    static const u8 blueModes[] = { 5, 1 };
    static const u8 lastW[] = { BATTERY_LED_MODE_BLUE, BATTERY_LED_MODE_UNKNOWN, BATTERY_LED_MODE_AUTO };
    u32 cases = 0, active = 0;

    for (u32 lv = 0; lv < LV_COUNT; lv++)
    for (u32 ch = 0; ch < CH_COUNT; ch++)
    for (u32 ov = 0; ov < 2; ov++)
    for (u32 cu = 0; cu < 2; cu++)
    for (u32 er = 0; er < 2; er++)
    for (u32 sd = 0; sd < 2; sd++)
    for (u32 sm = 0; sm < sizeof(sleepModes); sm++)
    for (u32 bm = 0; bm < sizeof(blueModes); bm++)
    for (u32 lw = 0; lw < sizeof(lastW); lw++)
    {
        Case c = { (u8)lv, (u8)ch, ov != 0, cu != 0, er != 0, sd != 0, sleepModes[sm], blueModes[bm], lastW[lw] };
        u8 s, w, hook, es, ew;
        ptmBytes(&c, &s, &w, &hook);
        expectedBytes(&c, &es, &ew);
        cases++;
        if (s != es || w != ew)
            printf("  case lv=%lu ch=%lu ov=%lu curve=%lu err=%lu shut=%lu sleep=%u blue=%u lw=%u: got %02x/%02x want %02x/%02x\n",
                   (unsigned long)lv, (unsigned long)ch, (unsigned long)ov, (unsigned long)cu, (unsigned long)er,
                   (unsigned long)sd, c.sleepMode, c.blueMode, c.lastWritten, s, w, es, ew);
        CHECK(s == es && w == ew);
        CHECK(BatteryPtmPatch_SleepByteAllowed(s) && BatteryPtmPatch_WakeByteAllowed(w));
        if (s != 0x02 || w != 0x00)
            active++;

        // No conflicting writes: a non-stock sleep byte is exactly what the sleep hook writes to the MCU (or has
        // already written: KEEP at the red level means 4 stays). Stock (02) is only sent at blink / off /
        // inactive, where the hook writes nothing different from what ptm is allowed to do.
        if (s != 0x02)
            CHECK(hook == s || (hook == BATTERY_LED_MODE_KEEP && s == 0x04));
        // The wake byte is never a mode the awake logic wouldn't use
        CHECK(w == 0x00 || w == c.blueMode || w == 0x04);
    }
    CHECK(cases == LV_COUNT * CH_COUNT * 2 * 2 * 2 * 2 * 3 * 2 * 3);
    printf("decision matrix: %lu cases, %lu with a non-stock byte\n", (unsigned long)cases, (unsigned long)active);

    // The user's table, spelled out (override on, curve, Sleep LED bright, blue_mode 5)
    Case c = { LV_BLUE, CH_NONE, true, true, false, false, 5, 5, BATTERY_LED_MODE_BLUE };
    u8 s, w, hook;
    ptmBytes(&c, &s, &w, &hook);  CHECK(s == 0x05 && w == 0x05 && hook == 0x05);   // blue
    c.level = LV_RED;
    ptmBytes(&c, &s, &w, &hook);  CHECK(s == 0x04 && w == 0x04);                   // red: 4 / 4
    c.level = LV_BLINK;
    ptmBytes(&c, &s, &w, &hook);  CHECK(s == 0x02 && w == 0x00);                   // blink: stock
    c.level = LV_RED; c.charge = CH_CHARGING;
    ptmBytes(&c, &s, &w, &hook);  CHECK(s == 0x05 && w == 0x05 && hook == 0x05);   // charging at red: blue
    c.level = LV_BLINK; c.charge = CH_ADAPTER;
    ptmBytes(&c, &s, &w, &hook);  CHECK(s == 0x05 && w == 0x05 && hook == 0x05);   // adapter at blink: blue
    c.sleepMode = 1; c.blueMode = 5;
    ptmBytes(&c, &s, &w, &hook);  CHECK(s == 0x01 && w == 0x05 && hook == 0x01);   // sleep dim(1), awake 5
    c.sleepMode = 2;
    ptmBytes(&c, &s, &w, &hook);  CHECK(s == 0x02 && w == 0x00);                   // unsanitized legacy 2: stock
    c.sleepMode = BATTERY_LED_SLEEP_SYSTEM;
    ptmBytes(&c, &s, &w, &hook);  CHECK(s == 0x02 && w == 0x00);                   // off: stock
}

static void testDesiredBytesExhaustive(void)
{
    static const u8 sleepModes[] = { 5, 1, 2, BATTERY_LED_SLEEP_SYSTEM };
    for (u32 sm = 0; sm < sizeof(sleepModes); sm++)
    for (u32 bm = 0; bm < 2; bm++)
    for (u32 reason = 0; reason <= BATTERY_LED_REASON_TEST; reason++)
    for (u32 mode = 0; mode < 256; mode++)
    {
        BatteryLedConfig cfg;
        BatteryLedConfig_Defaults(&cfg);
        cfg.sleepMode = sleepModes[sm];
        cfg.blueMode = bm ? 1 : 5;
        u8 s, w;
        BatteryPtmPatch_DesiredBytes(&cfg, (u8)mode, (u8)reason, &s, &w);
        CHECK(BatteryPtmPatch_SleepByteAllowed(s) && BatteryPtmPatch_WakeByteAllowed(w));
        bool on = reason == BATTERY_LED_REASON_SHELL && (cfg.sleepMode == 5 || cfg.sleepMode == 1);
        if (on && (mode == 5 || mode == 1))
            CHECK(s == mode && w == cfg.blueMode);
        else if (on && mode == 4)
            CHECK(s == 4 && w == 4);
        else
            CHECK(s == 0x02 && w == 0x00);
    }
}

typedef bool (*VerifyFn)(const u8 *);
typedef bool (*AllowedFn)(u8);

// Byte 10 accepted iff whitelisted; every other single-byte change at the 15 other positions rejected, with
// byte 10 at each whitelisted value. Returns the number of rejected changes.
static u32 checkPattern(const char *name, const u8 *expected, VerifyFn verify, AllowedFn allowed, const u8 *wl, u32 wlCount)
{
    u8 buf[16];
    memcpy(buf, expected, 16);
    CHECK(verify(buf));

    for (u32 v = 0; v < 256; v++)
    {
        bool inWl = false;
        for (u32 k = 0; k < wlCount; k++)
            inWl |= wl[k] == v;
        CHECK(allowed((u8)v) == inWl);
        memcpy(buf, expected, 16);
        buf[10] = (u8)v;
        CHECK(verify(buf) == inWl);
    }

    u32 rejected = 0, tried = 0;
    for (u32 m = 0; m < wlCount; m++)
        for (u32 i = 0; i < 16; i++)
        {
            if (i == 10)
                continue;
            for (u32 v = 0; v < 256; v++)
            {
                if (v == expected[i])
                    continue;
                memcpy(buf, expected, 16);
                buf[10] = wl[m];
                buf[i] = (u8)v;
                tried++;
                if (!verify(buf))
                    rejected++;
            }
        }
    CHECK(tried == wlCount * 15 * 255 && rejected == tried);
    printf("verification %s: %lu single-byte changes outside byte 10 rejected, byte 10 accepted only in", name,
           (unsigned long)rejected);
    for (u32 k = 0; k < wlCount; k++)
        printf(" %02x", wl[k]);
    printf("\n");
    return rejected;
}

static void testVerify(void)
{
    static const u8 sleepExp[16] = { 0x32, 0x21, 0x10, 0xB5, 0x00, 0xF0, 0x9B, 0xFD,
                                     0x04, 0x48, 0x02, 0x21, 0x00, 0xF0, 0xE1, 0xFC };
    static const u8 wakeExp[16]  = { 0xFF, 0x21, 0x86, 0xB0, 0x00, 0xF0, 0xE0, 0xFD,
                                     0x1C, 0x48, 0x00, 0x21, 0x00, 0xF0, 0x26, 0xFD };
    static const u8 sleepWl[4] = { 0x01, 0x02, 0x04, 0x05 };
    static const u8 wakeWl[4] = { 0x00, 0x01, 0x04, 0x05 };

    CHECK(memcmp(BatteryPtmPatch_SleepPattern(), sleepExp, 16) == 0);
    CHECK(memcmp(BatteryPtmPatch_WakePattern(), wakeExp, 16) == 0);
    CHECK(BATTERY_PTM_SLEEP_MODE_VA == 0x00101A28u);
    CHECK(BATTERY_PTM_WAKE_MODE_VA == 0x0010199Eu);
    CHECK(BATTERY_PTM_PAGE_VA == 0x00101000u);
    // Both 16-byte windows inside the one page that is mapped
    CHECK((BATTERY_PTM_SLEEP_PATTERN_VA & ~0xFFFu) == BATTERY_PTM_PAGE_VA &&
          ((BATTERY_PTM_SLEEP_PATTERN_VA + 15) & ~0xFFFu) == BATTERY_PTM_PAGE_VA);
    CHECK((BATTERY_PTM_WAKE_PATTERN_VA & ~0xFFFu) == BATTERY_PTM_PAGE_VA &&
          ((BATTERY_PTM_WAKE_PATTERN_VA + 15) & ~0xFFFu) == BATTERY_PTM_PAGE_VA);
    CHECK(BATTERY_PTM_CODE_END_VA == 0x00101A2Eu);

    checkPattern("sleep", sleepExp, BatteryPtmPatch_VerifySleep, BatteryPtmPatch_SleepByteAllowed, sleepWl, 4);
    checkPattern("wake", wakeExp, BatteryPtmPatch_VerifyWake, BatteryPtmPatch_WakeByteAllowed, wakeWl, 4);

    // Not interchangeable; the brightness bytes (0x32 / 0xff) are fixed
    CHECK(!BatteryPtmPatch_VerifySleep(wakeExp) && !BatteryPtmPatch_VerifyWake(sleepExp));
    u8 buf[16];
    memcpy(buf, sleepExp, 16); buf[0] = 0x05;
    CHECK(!BatteryPtmPatch_VerifySleep(buf));
    memcpy(buf, wakeExp, 16); buf[0] = 0x32;
    CHECK(!BatteryPtmPatch_VerifyWake(buf));
    // The other's stock value is not accepted: 00 in the sleep slot, 02 in the wake slot
    memcpy(buf, sleepExp, 16); buf[10] = 0x00;
    CHECK(!BatteryPtmPatch_VerifySleep(buf));
    memcpy(buf, wakeExp, 16); buf[10] = 0x02;
    CHECK(!BatteryPtmPatch_VerifyWake(buf));

    // Status names
    CHECK(strcmp(BatteryPtmPatch_StateName(BATTERY_PTM_STATE_OK, 0x02, 0x00), "stock") == 0);
    CHECK(strcmp(BatteryPtmPatch_StateName(BATTERY_PTM_STATE_OK, 0x05, 0x05), "active") == 0);
    CHECK(strcmp(BatteryPtmPatch_StateName(BATTERY_PTM_STATE_OK, 0x02, 0x04), "active") == 0);
    CHECK(strcmp(BatteryPtmPatch_StateName(BATTERY_PTM_STATE_OFF_MISMATCH, 0x02, 0x00), "off: mismatch") == 0);
    CHECK(strcmp(BatteryPtmPatch_StateName(BATTERY_PTM_STATE_OFF_UNMAP, 0x02, 0x00), "off: unmap error") == 0);
    // S1: the N3DS ptm only; its running codeset ID without the N3DS bit only on a New 3DS
    CHECK(BatteryPtmPatch_TitleAllowed(0x0004013020002202ULL, true));
    CHECK(BatteryPtmPatch_TitleAllowed(0x0004013000002202ULL, true));
    CHECK(!BatteryPtmPatch_TitleAllowed(0x0004013000002202ULL, false));
    CHECK(!BatteryPtmPatch_TitleAllowed(0x0004013000002102ULL, true));
    CHECK(!BatteryPtmPatch_TitleAllowed(0, true));
}

static u32 countHits(const u8 *data, long size, VerifyFn verify, u32 *lastOff)
{
    u32 hits = 0;
    for (long i = 0; i + BATTERY_PTM_PATTERN_LEN <= size; i++)
        if (verify(data + i))
        {
            hits++;
            *lastOff = (u32)i;
        }
    return hits;
}

// Pattern with byte 10 ignored entirely (stronger uniqueness than the whitelist)
static u32 countHitsAnyMode(const u8 *data, long size, const u8 *pattern, u32 *lastOff)
{
    u32 hits = 0;
    for (long i = 0; i + BATTERY_PTM_PATTERN_LEN <= size; i++)
    {
        bool ok = true;
        for (u32 k = 0; k < BATTERY_PTM_PATTERN_LEN && ok; k++)
            ok = k == BATTERY_PTM_MODE_INDEX || data[i + k] == pattern[k];
        if (ok)
        {
            hits++;
            *lastOff = (u32)i;
        }
    }
    return hits;
}

// Part 3: the real dump. Returns 0 if skipped, 1 if checked.
static int testDump(const char *path)
{
    FILE *f = path != NULL ? fopen(path, "rb") : NULL;
    if (f == NULL)
    {
        printf("ptm dump: SKIPPED (%s)\n", path != NULL ? "file not found" : "no file given, set PTM_CODE");
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *data = malloc(size > 0 ? (size_t)size : 1);
    CHECK(data != NULL && size > 0 && fread(data, 1, (size_t)size, f) == (size_t)size);
    fclose(f);
    if (data == NULL || size <= 0)
        return 1;

    // .code file offset 0 = VA 0x00100000
    u32 soff = BATTERY_PTM_SLEEP_PATTERN_VA - BATTERY_PTM_CODE_BASE;
    u32 woff = BATTERY_PTM_WAKE_PATTERN_VA - BATTERY_PTM_CODE_BASE;
    CHECK((long)soff + BATTERY_PTM_PATTERN_LEN <= size && (long)woff + BATTERY_PTM_PATTERN_LEN <= size);
    if ((long)soff + BATTERY_PTM_PATTERN_LEN <= size && (long)woff + BATTERY_PTM_PATTERN_LEN <= size)
    {
        CHECK(memcmp(data + soff, BatteryPtmPatch_SleepPattern(), 16) == 0);
        CHECK(memcmp(data + woff, BatteryPtmPatch_WakePattern(), 16) == 0);
        CHECK(data[soff + BATTERY_PTM_MODE_INDEX] == BATTERY_PTM_SLEEP_STOCK);
        CHECK(data[woff + BATTERY_PTM_MODE_INDEX] == BATTERY_PTM_WAKE_STOCK);
        // The instructions around them: sleep callback starts "ldr r0,=..." at 0x101a1c; wake callback starts
        // "push {r4,lr}; ldr r0,=..." at 0x101990
        CHECK(data[soff - 2] == 0x06 && data[soff - 1] == 0x48);
        CHECK(data[woff - 4] == 0x10 && data[woff - 3] == 0xB5 && data[woff - 2] == 0x1F && data[woff - 1] == 0x48);

        // Every whitelisted value verifies in place; a changed brightness doesn't
        u8 tmp[16];
        static const u8 sv[4] = { 1, 2, 4, 5 }, wv[4] = { 0, 1, 4, 5 };
        for (u32 k = 0; k < 4; k++)
        {
            memcpy(tmp, data + soff, 16); tmp[10] = sv[k]; CHECK(BatteryPtmPatch_VerifySleep(tmp));
            memcpy(tmp, data + woff, 16); tmp[10] = wv[k]; CHECK(BatteryPtmPatch_VerifyWake(tmp));
        }
        memcpy(tmp, data + soff, 16); tmp[0] = 0xFF; CHECK(!BatteryPtmPatch_VerifySleep(tmp));
        memcpy(tmp, data + woff, 16); tmp[0] = 0x32; CHECK(!BatteryPtmPatch_VerifyWake(tmp));
    }

    // Unique: no other offset in the whole file verifies, also with byte 10 ignored altogether
    u32 sHit = 0, wHit = 0, sAnyHit = 0, wAnyHit = 0;
    u32 sHits = countHits(data, size, BatteryPtmPatch_VerifySleep, &sHit);
    u32 wHits = countHits(data, size, BatteryPtmPatch_VerifyWake, &wHit);
    u32 sAny = countHitsAnyMode(data, size, BatteryPtmPatch_SleepPattern(), &sAnyHit);
    u32 wAny = countHitsAnyMode(data, size, BatteryPtmPatch_WakePattern(), &wAnyHit);
    CHECK(sHits == 1 && sHit == soff && sAny == 1 && sAnyHit == soff);
    CHECK(wHits == 1 && wHit == woff && wAny == 1 && wAnyHit == woff);

    // The same with a patched copy: after our writes the patterns are still found exactly once, in place
    u8 *copy = malloc((size_t)size);
    CHECK(copy != NULL);
    if (copy != NULL)
    {
        memcpy(copy, data, (size_t)size);
        copy[soff + 10] = 0x04;
        copy[woff + 10] = 0x05;
        CHECK(countHits(copy, size, BatteryPtmPatch_VerifySleep, &sHit) == 1 && sHit == soff);
        CHECK(countHits(copy, size, BatteryPtmPatch_VerifyWake, &wHit) == 1 && wHit == woff);
        free(copy);
    }

    printf("ptm dump: %ld bytes; sleep pattern at offset 0x%lx (VA 0x%08lx): %lu match(es), %lu with byte 10 ignored; "
           "wake pattern at offset 0x%lx (VA 0x%08lx): %lu match(es), %lu with byte 10 ignored; both on page 0x%08lx\n",
           size, (unsigned long)soff, (unsigned long)(BATTERY_PTM_CODE_BASE + soff), (unsigned long)sHits, (unsigned long)sAny,
           (unsigned long)woff, (unsigned long)(BATTERY_PTM_CODE_BASE + woff), (unsigned long)wHits, (unsigned long)wAny,
           (unsigned long)BATTERY_PTM_PAGE_VA);
    free(data);
    return 1;
}

int main(int argc, char **argv)
{
    testDecisionMatrix();
    testDesiredBytesExhaustive();
    testVerify();
    int dump = testDump(argc > 1 && argv[1][0] != 0 ? argv[1] : NULL);
    printf("%s\n", g_failures == 0 ? (dump ? "ptm host test: OK" : "ptm host test: OK (dump check skipped)")
                                   : "ptm host test: FAILED");
    return g_failures == 0 ? 0 : 1;
}
