/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Host test for the ptm sleep LED patch, pure part
*   (sysmodules/rosalina/source/battery/battery_ptm_patch_logic.c, with battery_led_logic.c).
*
*   1. Decision: the byte for ptm's sleep LED callback per Sleep LED setting, level, override, curve,
*      error, shutdown and "nothing written yet", through the same path as battery_led.c
*      (BatteryLedLogic_Decide with the lid closed -> BatteryPtmPatch_DesiredByte).
*   2. Verification: accepts the pattern with byte 10 = 02 / 01 / 05, rejects every other single-byte
*      change at all 16 positions, and the allowed-value whitelist.
*   3. Optional: the real ptm .code dump (New 3DS, 0004013020002202, GodMode9 "Extract .code"): the
*      pattern is at file offset 0x1a1e (VA 0x101a1e), exactly once, and passes the verification;
*      no other offset in the file verifies.
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

typedef struct Case
{
    bool override;
    u8 sleepMode;
    float corrected;    // < 0: no curve
    bool error, shuttingDown;
    u8 lastWritten;
} Case;

static Case baseCase(void)
{
    Case c;
    c.override = true;
    c.sleepMode = BATTERY_LED_MODE_BLUE;
    c.corrected = 50.0f;
    c.error = false;
    c.shuttingDown = false;
    c.lastWritten = BATTERY_LED_MODE_BLUE;
    return c;
}

// Same as battery_led.c batteryLedPtmPatchLocked: Decide on a copy with the lid closed, then DesiredByte
static u8 ptmByte(const Case *c)
{
    BatteryLedConfig cfg;
    BatteryLedConfig_Defaults(&cfg);
    cfg.override = c->override;
    cfg.sleepMode = c->sleepMode;
    BatteryLedConfig_Sanitize(&cfg);

    BatteryLedLogic l;
    BatteryLedLogic_Init(&l);

    BatteryLedInputs in;
    memset(&in, 0, sizeof(in));
    in.override = cfg.override;
    in.shellClosed = true;
    in.shuttingDown = c->shuttingDown;
    in.error = c->error;
    in.pct = 50.0f;
    in.corrected = c->corrected;
    in.lastWritten = c->lastWritten;

    u8 reason;
    u8 mode = BatteryLedLogic_Decide(&l, &cfg, &in, &reason);
    return BatteryPtmPatch_DesiredByte(mode, reason);
}

static void testDecision(void)
{
    Case c;

    // Blue level, override on: the Sleep LED setting
    c = baseCase();
    CHECK(ptmByte(&c) == 0x05);
    c.sleepMode = BATTERY_LED_MODE_BLUE_FORCED;
    CHECK(ptmByte(&c) == 0x01);
    c.sleepMode = BATTERY_LED_MODE_SLEEP;              // anim(2): stock
    CHECK(ptmByte(&c) == 0x02);
    c.sleepMode = BATTERY_LED_SLEEP_SYSTEM;            // system: stock
    CHECK(ptmByte(&c) == 0x02);
    c.sleepMode = 7;                                   // invalid -> sanitized to 5
    CHECK(ptmByte(&c) == 0x05);

    // Red / blinking level (default thresholds 10 / 3): stock, the MCU gives its own low warning
    c = baseCase();
    c.corrected = 8.0f;
    CHECK(ptmByte(&c) == 0x02);
    c.corrected = 2.0f;
    CHECK(ptmByte(&c) == 0x02);
    c.sleepMode = BATTERY_LED_MODE_BLUE_FORCED;
    CHECK(ptmByte(&c) == 0x02);
    c.corrected = 10.5f;                               // just above the red threshold: blue
    CHECK(ptmByte(&c) == 0x01);

    // Override off, no curve, error, shutdown: stock, whatever the setting / level
    for (u32 s = 0; s < 2; s++)
    {
        u8 mode = s == 0 ? BATTERY_LED_MODE_BLUE : BATTERY_LED_MODE_BLUE_FORCED;
        c = baseCase(); c.sleepMode = mode; c.override = false;
        CHECK(ptmByte(&c) == 0x02);
        c = baseCase(); c.sleepMode = mode; c.corrected = -1.0f;
        CHECK(ptmByte(&c) == 0x02);
        c = baseCase(); c.sleepMode = mode; c.error = true;
        CHECK(ptmByte(&c) == 0x02);
        c = baseCase(); c.sleepMode = mode; c.shuttingDown = true;
        CHECK(ptmByte(&c) == 0x02);
        // Nothing written this boot (the sleep hook writes nothing then either)
        c = baseCase(); c.sleepMode = mode; c.lastWritten = BATTERY_LED_MODE_UNKNOWN;
        CHECK(ptmByte(&c) == 0x02);
        c = baseCase(); c.sleepMode = mode; c.lastWritten = BATTERY_LED_MODE_AUTO;
        CHECK(ptmByte(&c) == 0x02);
    }

    // DesiredByte itself: only reason "lid closed" with 5 / 1 gives a non-stock byte
    for (u32 reason = 0; reason <= BATTERY_LED_REASON_TEST; reason++)
        for (u32 mode = 0; mode < 256; mode++)
        {
            u8 b = BatteryPtmPatch_DesiredByte((u8)mode, (u8)reason);
            CHECK(BatteryPtmPatch_ByteAllowed(b));
            if (reason == BATTERY_LED_REASON_SHELL && (mode == 5 || mode == 1))
                CHECK(b == mode);
            else
                CHECK(b == 0x02);
        }
}

static void testVerify(void)
{
    const u8 *pat = BatteryPtmPatch_Pattern();
    static const u8 expected[16] = { 0x32, 0x21, 0x10, 0xB5, 0x00, 0xF0, 0x9B, 0xFD,
                                     0x04, 0x48, 0x02, 0x21, 0x00, 0xF0, 0xE1, 0xFC };
    CHECK(memcmp(pat, expected, 16) == 0);
    CHECK(BATTERY_PTM_MODE_VA == 0x00101A28u);
    CHECK(BATTERY_PTM_PAGE_VA == 0x00101000u);
    CHECK((BATTERY_PTM_PATTERN_VA & 0xFFFu) + BATTERY_PTM_PATTERN_LEN <= 0x1000u); // one page

    // Whitelist
    for (u32 v = 0; v < 256; v++)
        CHECK(BatteryPtmPatch_ByteAllowed((u8)v) == (v == 0x02 || v == 0x01 || v == 0x05));

    u8 buf[16];
    memcpy(buf, expected, 16);
    CHECK(BatteryPtmPatch_Verify(buf));

    // Byte 10: 02 / 01 / 05 accepted, everything else rejected
    for (u32 v = 0; v < 256; v++)
    {
        memcpy(buf, expected, 16);
        buf[10] = (u8)v;
        CHECK(BatteryPtmPatch_Verify(buf) == (v == 0x02 || v == 0x01 || v == 0x05));
    }

    // Any change to any other byte is rejected, also with byte 10 already patched
    u32 rejected = 0, tried = 0;
    for (u32 mode = 0; mode < 3; mode++)
    {
        static const u8 modes[3] = { 0x02, 0x01, 0x05 };
        for (u32 i = 0; i < 16; i++)
        {
            if (i == 10)
                continue;
            for (u32 v = 0; v < 256; v++)
            {
                if (v == expected[i])
                    continue;
                memcpy(buf, expected, 16);
                buf[10] = modes[mode];
                buf[i] = (u8)v;
                tried++;
                if (!BatteryPtmPatch_Verify(buf))
                    rejected++;
            }
        }
    }
    CHECK(tried == 3 * 15 * 255 && rejected == tried);

    // The brightness byte (0x32) is not variable
    memcpy(buf, expected, 16);
    buf[0] = 0x05;
    CHECK(!BatteryPtmPatch_Verify(buf));

    printf("verification: %lu single-byte changes outside byte 10 rejected, 02/01/05 accepted\n", (unsigned long)tried);
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

    u32 off = BATTERY_PTM_PATTERN_VA - BATTERY_PTM_CODE_BASE; // .code file offset 0 = VA 0x00100000
    CHECK((long)off + BATTERY_PTM_PATTERN_LEN <= size);
    if ((long)off + BATTERY_PTM_PATTERN_LEN <= size)
    {
        CHECK(BatteryPtmPatch_Verify(data + off));
        CHECK(data[off + BATTERY_PTM_MODE_INDEX] == BATTERY_PTM_BYTE_STOCK);
        CHECK(memcmp(data + off, BatteryPtmPatch_Pattern(), BATTERY_PTM_PATTERN_LEN) == 0);
        // The instruction around it: "ldr r0,=..." at 0x101a1c, "ldr r0,=..." at 0x101a26
        CHECK(data[off - 2] == 0x06 && data[off - 1] == 0x48);

        // Patched copies verify; a changed brightness doesn't
        u8 tmp[16];
        memcpy(tmp, data + off, 16);
        tmp[10] = 0x05;
        CHECK(BatteryPtmPatch_Verify(tmp));
        tmp[10] = 0x01;
        CHECK(BatteryPtmPatch_Verify(tmp));
        tmp[0] = 0xFF;
        CHECK(!BatteryPtmPatch_Verify(tmp));
    }

    // Unique: no other offset in the whole file verifies (with any of the three mode bytes)
    u32 hits = 0, hitOff = 0;
    for (long i = 0; i + BATTERY_PTM_PATTERN_LEN <= size; i++)
        if (BatteryPtmPatch_Verify(data + i))
        {
            hits++;
            hitOff = (u32)i;
        }
    CHECK(hits == 1 && hitOff == off);
    bool atOff = (long)off + BATTERY_PTM_PATTERN_LEN <= size && BatteryPtmPatch_Verify(data + off);
    printf("ptm dump: %ld bytes, pattern %s at offset 0x%lx (VA 0x%08lx), %lu match(es) in the file\n",
           size, atOff ? "verified" : "NOT found", (unsigned long)off, (unsigned long)(BATTERY_PTM_CODE_BASE + off),
           (unsigned long)hits);
    free(data);
    return 1;
}

int main(int argc, char **argv)
{
    testDecision();
    testVerify();
    int dump = testDump(argc > 1 && argv[1][0] != 0 ? argv[1] : NULL);
    printf("%s\n", g_failures == 0 ? (dump ? "ptm host test: OK" : "ptm host test: OK (dump check skipped)")
                                   : "ptm host test: FAILED");
    return g_failures == 0 ? 0 : 1;
}
