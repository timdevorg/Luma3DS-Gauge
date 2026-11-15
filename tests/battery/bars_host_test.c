/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Host test for the ptm battery bars patch, pure part
*   (sysmodules/rosalina/source/battery/battery_ptm_bars_logic.c, with battery_led_logic.c).
*
*   1. Decision: BatteryBars_Decide over corrected % x thresholds x charging x setting x curve x MCU error, against
*      an independent statement of the table (docs/battery-manager.md, section 15.3); the 1-point hysteresis; the
*      red / blink steps switching at the same decision as the LED (through BatteryLedLogic_Decide, as in
*      battery_led.c, also with the override off); the debounced unplug; never 0 when fed.
*   2. A model of both ptm level functions (copy A 0x106f38, copy B 0x104cc0) for % 0..255, cmp byte {3C, 00} x
*      movs byte 01..05 (every mix of old and new bytes): the level is always 0..5, % 0 gives 0, stock bytes give
*      the stock mapping, fed bytes give L for every % > 0. With the dump, the same is run on ptm's own instructions
*      by a small Thumb interpreter and must agree with the model for every input.
*   3. Verification: bytes 8 and 12 of both windows over all 256 values; every single-byte change at the other 14
*      positions rejected (with each whitelisted combination of bytes 8 / 12).
*   4. Optional: the real ptm .code dump: both windows in place and stock, each exactly once in the file (also
*      with bytes 8 and 12 ignored), also after patching a copy.
*   5. config.ini: bars / bars_4 / bars_3 parsing and sanitising.
*
*   Usage: bars_host_test [0004013020002202.dec.code]   (run through tests/battery/run_host_test.sh)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "battery/battery_led_logic.h"
#include "battery/battery_ptm_bars.h"

static int g_failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

// ---- 1. Decision ----

static BatteryBarsInputs barsIn(float c, bool charger)
{
    BatteryBarsInputs in;
    memset(&in, 0, sizeof(in));
    in.haveData = true;
    in.corrected = c;
    in.onCharger = charger;
    in.ledLevelValid = false;
    return in;
}

// Independent statement of the table (fresh, no hysteresis): the LED rule for red / blink (c <= threshold)
static u8 expectedFresh(const BatteryLedConfig *cfg, float c)
{
    if (c <= cfg->blinkPct)
        return 1;
    if (c <= cfg->redPct)
        return 2;
    if (c >= cfg->bars4Pct)
        return 5;
    if (c >= cfg->bars3Pct)
        return 4;
    return 3;
}

static void testDecisionMatrix(void)
{
    static const u8 cfgs[][4] = {   // red, blink, bars3, bars4
        { 10, 3, 40, 70 }, { 15, 5, 30, 60 }, { 5, 0, 6, 7 }, { 50, 49, 51, 100 }, { 1, 0, 2, 100 },
    };
    u32 cases = 0;
    for (u32 k = 0; k < sizeof(cfgs) / sizeof(cfgs[0]); k++)
        for (u32 setting = 0; setting < 2; setting++)
            for (u32 curve = 0; curve < 2; curve++)
                for (u32 charging = 0; charging < 2; charging++)
                    for (u32 err = 0; err < 3; err++)   // 0 ok, 1 MCU error, 2 no data
                        for (int ci = 0; ci <= 1010; ci++)
                        {
                            BatteryLedConfig cfg;
                            BatteryLedConfig_Defaults(&cfg);
                            cfg.redPct = cfgs[k][0];
                            cfg.blinkPct = cfgs[k][1];
                            cfg.bars3Pct = cfgs[k][2];
                            cfg.bars4Pct = cfgs[k][3];
                            cfg.bars = setting != 0;
                            BatteryLedConfig before = cfg;
                            BatteryLedConfig_Sanitize(&cfg);
                            CHECK(memcmp(&before, &cfg, sizeof(cfg)) == 0); // all valid configurations

                            float c = curve ? ci / 10.0f : -1.0f;
                            BatteryBarsInputs in = barsIn(c, charging != 0);
                            in.mcuError = err == 1;
                            in.haveData = err != 2;
                            u8 reason;
                            u8 L = BatteryBars_Decide(&cfg, &in, BATTERY_BARS_STOCK, &reason);
                            cases++;

                            u8 expL, expR;
                            if (!cfg.bars)
                                expL = 0, expR = BATTERY_BARS_REASON_OFF;
                            else if (err == 2)
                                expL = 0, expR = BATTERY_BARS_REASON_NO_DATA;
                            else if (err == 1)
                                expL = 0, expR = BATTERY_BARS_REASON_MCU_ERROR;
                            else if (!curve)
                                expL = 0, expR = BATTERY_BARS_REASON_NO_CURVE;
                            else if (charging)
                                expL = 0, expR = BATTERY_BARS_REASON_CHARGING;
                            else
                                expL = expectedFresh(&cfg, c), expR = BATTERY_BARS_REASON_FED;
                            CHECK(L == expL && reason == expR);
                            CHECK(L <= 5 && (reason == BATTERY_BARS_REASON_FED) == (L != 0)); // never 0 when fed

                            // With the LED level given, the red / blink steps come from it whatever c says
                            if (expR == BATTERY_BARS_REASON_FED)
                            {
                                in.ledLevelValid = true;
                                in.ledLevel = BATTERY_LED_LEVEL_RED;
                                CHECK(BatteryBars_Decide(&cfg, &in, 5, NULL) == 2);
                                in.ledLevel = BATTERY_LED_LEVEL_BLINK;
                                CHECK(BatteryBars_Decide(&cfg, &in, 5, NULL) == 1);
                                in.ledLevel = BATTERY_LED_LEVEL_BLUE;
                                if (c > cfg.redPct)
                                    CHECK(BatteryBars_Decide(&cfg, &in, BATTERY_BARS_STOCK, NULL) == expL);
                            }

                            // The bytes: stock 3C / 05, fed 00 / L
                            u8 cmp, movs;
                            CHECK(BatteryBars_BytesFor(L, &cmp, &movs));
                            CHECK(L == 0 ? (cmp == 0x3C && movs == 0x05) : (cmp == 0x00 && movs == L));
                            CHECK(BatteryBars_CmpAllowed(cmp) && BatteryBars_MovsAllowed(movs));
                        }
    for (u32 v = 0; v < 256; v++)
    {
        u8 cmp = 0xAA, movs = 0xAA;
        bool ok = BatteryBars_BytesFor((u8)v, &cmp, &movs);
        CHECK(ok == (v <= 5));
        CHECK(BatteryBars_CmpAllowed(cmp) && BatteryBars_MovsAllowed(movs)); // stock bytes when refused
    }

    // Defaults: the table of section 15.3
    BatteryLedConfig cfg;
    BatteryLedConfig_Defaults(&cfg);
    CHECK(cfg.bars && cfg.bars4Pct == 70 && cfg.bars3Pct == 40 && cfg.redPct == 10 && cfg.blinkPct == 3);
    static const struct { float c; u8 L; } table[] = {
        { 100.0f, 5 }, { 70.0f, 5 }, { 69.9f, 4 }, { 40.0f, 4 }, { 39.9f, 3 }, { 10.1f, 3 }, { 10.0f, 2 },
        { 3.1f, 2 }, { 3.0f, 1 }, { 0.5f, 1 }, { 0.0f, 1 },
    };
    for (u32 i = 0; i < sizeof(table) / sizeof(table[0]); i++)
    {
        BatteryBarsInputs in = barsIn(table[i].c, false);
        CHECK(BatteryBars_Decide(&cfg, &in, BATTERY_BARS_STOCK, NULL) == table[i].L);
    }
    printf("bars decision: %lu cases (corrected 0..101 x 5 threshold sets x setting x curve x charging x MCU error)\n",
           (unsigned long)cases);
}

static u8 step(const BatteryLedConfig *cfg, float c, u8 *prev)
{
    BatteryBarsInputs in = barsIn(c, false);
    *prev = BatteryBars_Decide(cfg, &in, *prev, NULL);
    return *prev;
}

static void testHysteresis(void)
{
    BatteryLedConfig cfg;
    BatteryLedConfig_Defaults(&cfg);
    u8 L = BATTERY_BARS_STOCK;
    CHECK(step(&cfg, 80.0f, &L) == 5);
    CHECK(step(&cfg, 69.9f, &L) == 4);  // down at once
    CHECK(step(&cfg, 70.0f, &L) == 4);  // up only from 71
    CHECK(step(&cfg, 70.9f, &L) == 4);
    CHECK(step(&cfg, 71.0f, &L) == 5);
    CHECK(step(&cfg, 39.9f, &L) == 3);  // two steps down at once
    CHECK(step(&cfg, 40.5f, &L) == 3);
    CHECK(step(&cfg, 41.0f, &L) == 4);
    CHECK(step(&cfg, 75.0f, &L) == 5);  // two steps up at once when far enough above
    CHECK(step(&cfg, 39.0f, &L) == 3);
    CHECK(step(&cfg, 70.5f, &L) == 4);  // 70.5 >= 40 + 1 but < 70 + 1
    // From stock (charging -> unplugged, setting switched on) the level is fresh, no hysteresis
    L = BATTERY_BARS_STOCK;
    CHECK(step(&cfg, 70.0f, &L) == 5);
    // Wobbling around a threshold never flaps
    L = BATTERY_BARS_STOCK;
    u32 changes = 0;
    u8 last = step(&cfg, 40.2f, &L);
    for (u32 i = 0; i < 1000; i++)
    {
        float c = 39.5f + (float)(i % 13) * 0.1f;   // 39.5 .. 40.7
        u8 now = step(&cfg, c, &L);
        changes += now != last;
        last = now;
    }
    CHECK(changes == 1); // 4 -> 3 once, never back (needs 41)
    // Threshold change: up only with hysteresis, down at once
    L = BATTERY_BARS_STOCK;
    CHECK(step(&cfg, 65.0f, &L) == 4);
    cfg.bars4Pct = 64;
    CHECK(step(&cfg, 65.0f, &L) == 5);  // 65 >= 64 + 1
    cfg.bars4Pct = 70;
    CHECK(step(&cfg, 65.0f, &L) == 4);
    cfg.bars4Pct = 65;
    CHECK(step(&cfg, 65.5f, &L) == 4);  // 65.5 < 66
    printf("bars hysteresis: 1 point up, down at once, no flapping around a threshold\n");
}

// Through the LED logic as in battery_led.c (Decide each poll, then the bars from its level / charger state)
typedef struct Sim
{
    BatteryLedConfig cfg;
    BatteryLedLogic l;
    u8 bars;
    u8 led;
} Sim;

static void simPoll(Sim *s, float c, bool adapter, bool charging)
{
    BatteryLedInputs in;
    memset(&in, 0, sizeof(in));
    in.override = s->cfg.override;
    in.adapter = adapter;
    in.charging = charging;
    in.pct = 50.0f;
    in.corrected = c;
    in.lastWritten = BATTERY_LED_MODE_BLUE;
    s->led = BatteryLedLogic_Decide(&s->l, &s->cfg, &in, NULL);
    BatteryBarsInputs b;
    memset(&b, 0, sizeof(b));
    b.haveData = true;
    b.onCharger = s->l.onCharger;
    b.corrected = c;
    b.ledLevelValid = s->l.levelValid;
    b.ledLevel = s->l.level;
    s->bars = BatteryBars_Decide(&s->cfg, &b, s->bars, NULL);
}

static void testWithLed(void)
{
    for (u32 override = 0; override < 2; override++)
    {
        Sim s;
        BatteryLedConfig_Defaults(&s.cfg);
        s.cfg.override = override != 0;
        BatteryLedLogic_Init(&s.l);
        s.bars = BATTERY_BARS_STOCK;

        // Discharge 100 -> 0 with a voltage-recovery wobble: red / blink bars switch at the same poll as the LED,
        // and (like the LED) never come back within the discharge
        Sim led;  // override on, for the LED's decision when this one runs with the override off
        led = s;
        led.cfg.override = true;
        u8 prev = 0xFF;
        u32 changes = 0;
        for (int i = 1000; i >= 0; i--)
        {
            float c = i / 10.0f + ((i % 7) == 0 ? 0.8f : 0.0f);
            simPoll(&s, c, false, false);
            simPoll(&led, c, false, false);
            CHECK((s.bars == 2) == (led.led == BATTERY_LED_MODE_RED));
            CHECK((s.bars == 1) == (led.led == BATTERY_LED_MODE_BLINK));
            CHECK(s.bars >= 1 && s.bars <= 5);
            if (prev != 0xFF && s.bars != prev)
            {
                CHECK(s.bars == prev - 1); // one step at a time, never up
                changes++;
            }
            prev = s.bars;
        }
        CHECK(changes == 4);
        // Override off: the LED decision is automatic, but the level state behind the bars is kept anyway
        if (!override)
            CHECK(s.led == BATTERY_LED_MODE_AUTO && s.l.levelValid && s.l.level == BATTERY_LED_LEVEL_BLINK);

        // Charger: stock at once; after unplugging stock until the LED's debounce says unplugged, then the
        // corrected level as it is (a new discharge)
        simPoll(&s, 2.0f, true, false);
        CHECK(s.bars == BATTERY_BARS_STOCK);
        simPoll(&s, 50.0f, false, true);
        CHECK(s.bars == BATTERY_BARS_STOCK);
        for (u32 k = 0; k + 1 < BATTERY_LED_UNPLUG_DECISIONS; k++)
        {
            simPoll(&s, 50.0f, false, false);
            CHECK(s.bars == BATTERY_BARS_STOCK);
        }
        simPoll(&s, 50.0f, false, false);
        CHECK(s.bars == 4);
        // A charging flag flickering for one poll: stock again (and the debounce starts over)
        simPoll(&s, 50.0f, false, true);
        CHECK(s.bars == BATTERY_BARS_STOCK);
        for (u32 k = 0; k < BATTERY_LED_UNPLUG_DECISIONS; k++)
            simPoll(&s, 9.0f, false, false);
        CHECK(s.bars == 2); // unplugged at a red level: red bar, together with the red level
        // Setting off -> stock, on again -> fresh level
        s.cfg.bars = false;
        simPoll(&s, 9.0f, false, false);
        CHECK(s.bars == BATTERY_BARS_STOCK);
        s.cfg.bars = true;
        simPoll(&s, 9.0f, false, false);
        CHECK(s.bars == 2);
        // No curve -> stock
        simPoll(&s, -1.0f, false, false);
        CHECK(s.bars == BATTERY_BARS_STOCK);
    }
    printf("bars with the LED logic: red / blink bars switch at the same poll as the LED (override on and off), "
           "4 steps per discharge, stock while charging with the LED's unplug debounce\n");
}

// ---- 2. Model of ptm's two level functions ----

// Copy A, 0x106f38..0x106f96 (r1 = 1 before the cmp at 0x106f5c; the chain after "bls" is untouched)
static int modelA(u32 pct, u8 cmpImm, u8 movsImm)
{
    u32 r1 = 1;
    if (pct > cmpImm)                       // cmp r0, #imm; bls not taken
        return movsImm;                     // movs r1, #imm
    if (pct - 31u <= 29u) return 4;
    if (pct - 11u <= 19u) return 3;
    if (pct - 6u <= 4u) return 2;
    if (pct - 1u <= 4u) return (int)r1;     // bls to the store with r1 = 1
    if (pct == 0) return 0;
    return (int)r1;                         // bne to the store with r1 = 1
}

// Copy B, 0x104cc0..0x104d14: -1 = returns without storing a level
static int modelB(u32 pct, u8 cmpImm, u8 movsImm)
{
    if (pct > cmpImm)
        return movsImm;
    if (pct - 31u <= 29u) return 4;
    if (pct - 11u <= 19u) return 3;
    if (pct - 6u <= 4u) return 2;
    if (pct - 1u <= 4u) return 1;
    if (pct == 0) return 0;
    return -1;
}

static int stockLevel(u32 pct)
{
    return pct > 60 ? 5 : pct >= 31 ? 4 : pct >= 11 ? 3 : pct >= 6 ? 2 : pct >= 1 ? 1 : 0;
}

static void testModel(void)
{
    static const u8 cmps[2] = { 0x3C, 0x00 };
    u32 n = 0;
    for (u32 ci = 0; ci < 2; ci++)
        for (u32 m = 1; m <= 5; m++)
            for (u32 pct = 0; pct < 256; pct++)
            {
                int a = modelA(pct, cmps[ci], (u8)m), b = modelB(pct, cmps[ci], (u8)m);
                CHECK(a >= 0 && a <= 5 && b >= 0 && b <= 5);
                CHECK(a == b);
                if (pct == 0)
                    CHECK(a == 0);              // shutdown semantics kept in every state
                if (ci == 1 && pct > 0)
                    CHECK(a == (int)m);         // fed: L for every % > 0
                if (ci == 0)
                    CHECK(a == (pct > 60 ? (int)m : stockLevel(pct)));
                if (ci == 0 && m == 5)
                    CHECK(a == stockLevel(pct)); // stock bytes: stock mapping
                n++;
            }

    // The write orders of battery_ptm_patch.c, step by step (both copies): every state on the way is valid
    for (u32 L = 1; L <= 5; L++)
        for (u32 L2 = 1; L2 <= 5; L2++)
        {
            const u8 seq[][2] = {
                { 0x3C, 0x05 }, { 0x3C, (u8)L }, { 0x00, (u8)L },   // enable: movs first, then cmp
                { 0x00, (u8)L2 },                                   // change: movs only
                { 0x3C, (u8)L2 }, { 0x3C, 0x05 },                   // disable: cmp first, then movs
            };
            for (u32 s = 0; s < sizeof(seq) / sizeof(seq[0]); s++)
                for (u32 pct = 0; pct < 256; pct++)
                {
                    int a = modelA(pct, seq[s][0], seq[s][1]);
                    CHECK(a >= 0 && a <= 5 && a == modelB(pct, seq[s][0], seq[s][1]) && (pct != 0 || a == 0));
                }
        }
    printf("ptm level model: %lu cases (%% 0..255 x cmp 3C/00 x movs 01..05, copies A and B): always 0..5, "
           "%% 0 -> 0, stock bytes -> stock mapping, fed -> L\n", (unsigned long)n);
}

// Minimal Thumb interpreter for the instructions in ptm's level chains (movs/cmp/subs imm8, subs imm3, mov hi,
// b, b<cond>). Runs from `start` with the given registers until pc hits one of the stop addresses.
typedef struct Cpu { u32 r[16]; bool n, z, c, v; } Cpu;

static void setSubFlags(Cpu *cpu, u32 a, u32 b)
{
    u32 r = a - b;
    cpu->n = (r >> 31) != 0;
    cpu->z = r == 0;
    cpu->c = a >= b;
    cpu->v = (((a ^ b) & (a ^ r)) >> 31) != 0;
}

static bool condOk(const Cpu *cpu, u32 cond)
{
    switch (cond)
    {
        case 0x0: return cpu->z;
        case 0x1: return !cpu->z;
        case 0x8: return cpu->c && !cpu->z;
        case 0x9: return !cpu->c || cpu->z;
        default:  return false;
    }
}

// Returns the index of the stop address reached, or -1 on an unknown instruction / too many steps
static int runThumb(const u8 *code, u32 base, u32 start, Cpu *cpu, const u32 *stops, u32 nStops)
{
    u32 pc = start;
    for (u32 steps = 0; steps < 64; steps++)
    {
        for (u32 k = 0; k < nStops; k++)
            if (pc == stops[k])
                return (int)k;
        u32 op = code[pc - base] | ((u32)code[pc - base + 1] << 8);
        u32 next = pc + 2;
        if ((op >> 11) == 0x04)                         // movs rd, #imm8
        {
            cpu->r[(op >> 8) & 7] = op & 0xFF;
            cpu->n = false;
            cpu->z = (op & 0xFF) == 0;
        }
        else if ((op >> 11) == 0x05)                    // cmp rn, #imm8
            setSubFlags(cpu, cpu->r[(op >> 8) & 7], op & 0xFF);
        else if ((op >> 11) == 0x07)                    // subs rdn, #imm8
        {
            u32 d = (op >> 8) & 7, a = cpu->r[d];
            setSubFlags(cpu, a, op & 0xFF);
            cpu->r[d] = a - (op & 0xFF);
        }
        else if ((op >> 9) == 0x0F)                     // subs rd, rn, #imm3
        {
            u32 a = cpu->r[(op >> 3) & 7], imm = (op >> 6) & 7;
            setSubFlags(cpu, a, imm);
            cpu->r[op & 7] = a - imm;
        }
        else if ((op >> 8) == 0x46)                     // mov rd, rm
            cpu->r[((op >> 4) & 8) | (op & 7)] = cpu->r[(op >> 3) & 0xF];
        else if ((op >> 12) == 0xD && ((op >> 8) & 0xF) < 0xE)    // b<cond>
        {
            if (condOk(cpu, (op >> 8) & 0xF))
                next = pc + 4 + (u32)((s32)(s8)(op & 0xFF) * 2);
            else if (((op >> 8) & 0xF) != 0x0 && ((op >> 8) & 0xF) != 0x1 && ((op >> 8) & 0xF) != 0x8 &&
                     ((op >> 8) & 0xF) != 0x9)
                return -1;
        }
        else if ((op >> 11) == 0x1C)                    // b
        {
            s32 imm = (s32)(op & 0x7FF);
            if (imm & 0x400)
                imm -= 0x800;
            next = pc + 4 + (u32)(imm * 2);
        }
        else
            return -1;
        pc = next;
    }
    return -1;
}

// Copy A from the cmp at 0x106f5c (r0 = %, r1 = 1) to the store at 0x106f92: level in r1
static int dumpA(const u8 *code, u32 pct, u8 cmpImm, u8 movsImm)
{
    u8 tmp[0x60];
    const u32 base = 0x106f50;
    memcpy(tmp, code + (base - 0x100000), sizeof(tmp));
    tmp[BATTERY_BARS_A_CMP_VA - base] = cmpImm;
    tmp[BATTERY_BARS_A_MOVS_VA - base] = movsImm;
    Cpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[0] = pct;
    cpu.r[1] = 1;
    cpu.r[2] = 0xE3;
    cpu.r[3] = 0xDEADBEEF;
    static const u32 stops[] = { 0x106f92 };
    if (runThumb(tmp, base, BATTERY_BARS_A_CMP_VA, &cpu, stops, 1) != 0)
        return -2;
    return (int)cpu.r[1];
}

// Copy B from the cmp at 0x104cd8 (r2 = %, r5 = 0): store r1 at 0x104cea, store r5 at 0x104d12, no store 0x104cec
static int dumpB(const u8 *code, u32 pct, u8 cmpImm, u8 movsImm)
{
    u8 tmp[0x50];
    const u32 base = 0x104cd0;
    memcpy(tmp, code + (base - 0x100000), sizeof(tmp));
    tmp[BATTERY_BARS_B_CMP_VA - base] = cmpImm;
    tmp[BATTERY_BARS_B_MOVS_VA - base] = movsImm;
    Cpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[2] = pct;
    cpu.r[1] = 0x0FFFFF00;
    cpu.r[5] = 0;
    static const u32 stops[] = { 0x104cea, 0x104d12, 0x104cec };
    switch (runThumb(tmp, base, BATTERY_BARS_B_CMP_VA, &cpu, stops, 3))
    {
        case 0:  return (int)(cpu.r[1] & 0xFF);
        case 1:  return (int)(cpu.r[5] & 0xFF);
        case 2:  return -1;
        default: return -2;
    }
}

// ---- 3. Verification ----

typedef bool (*VerifyFn)(const u8 *);

static void checkWindow(const char *name, const u8 *expected, VerifyFn verify)
{
    u8 buf[16];
    memcpy(buf, expected, 16);
    CHECK(verify(buf));

    // Bytes 8 and 12 over all 256 values (the other one at its stock value, and at the other whitelisted ones)
    for (u32 v = 0; v < 256; v++)
    {
        CHECK(BatteryBars_CmpAllowed((u8)v) == (v == 0x3C || v == 0x00));
        CHECK(BatteryBars_MovsAllowed((u8)v) == (v >= 1 && v <= 5));
        for (u32 other = 1; other <= 5; other++)
        {
            memcpy(buf, expected, 16);
            buf[8] = (u8)v;
            buf[12] = (u8)other;
            CHECK(verify(buf) == (v == 0x3C || v == 0x00));
        }
        for (u32 oi = 0; oi < 2; oi++)
        {
            memcpy(buf, expected, 16);
            buf[8] = oi ? 0x00 : 0x3C;
            buf[12] = (u8)v;
            CHECK(verify(buf) == (v >= 1 && v <= 5));
        }
    }

    // Every single-byte change at the other 14 positions, with each whitelisted combination of bytes 8 / 12
    u32 rejected = 0, tried = 0;
    for (u32 ci = 0; ci < 2; ci++)
        for (u32 m = 1; m <= 5; m++)
            for (u32 i = 0; i < 16; i++)
            {
                if (i == 8 || i == 12)
                    continue;
                for (u32 v = 0; v < 256; v++)
                {
                    if (v == expected[i])
                        continue;
                    memcpy(buf, expected, 16);
                    buf[8] = ci ? 0x00 : 0x3C;
                    buf[12] = (u8)m;
                    buf[i] = (u8)v;
                    tried++;
                    rejected += !verify(buf);
                }
            }
    CHECK(tried == 10 * 14 * 255 && rejected == tried);
    printf("verification window %s: %lu single-byte changes outside bytes 8 / 12 rejected; byte 8 accepted only in 3c 00, "
           "byte 12 only in 01..05\n", name, (unsigned long)rejected);
}

static void testVerify(void)
{
    static const u8 aExp[16] = { 0x02, 0x40, 0x68, 0x46, 0x22, 0x70, 0x00, 0x78,
                                 0x3C, 0x28, 0x01, 0xD9, 0x05, 0x21, 0x16, 0xE0 };
    static const u8 bExp[16] = { 0x01, 0xF0, 0xF2, 0xFB, 0x69, 0x46, 0x0A, 0x78,
                                 0x3C, 0x2A, 0x01, 0xD9, 0x05, 0x21, 0x04, 0xE0 };
    CHECK(memcmp(BatteryBars_WindowA(), aExp, 16) == 0);
    CHECK(memcmp(BatteryBars_WindowB(), bExp, 16) == 0);
    CHECK(BATTERY_BARS_A_CMP_VA == 0x00106F5Cu && BATTERY_BARS_A_MOVS_VA == 0x00106F60u);
    CHECK(BATTERY_BARS_B_CMP_VA == 0x00104CD8u && BATTERY_BARS_B_MOVS_VA == 0x00104CDCu);
    CHECK(BATTERY_BARS_MAP_SRC_VA == 0x00104000u && BATTERY_BARS_MAP_SIZE == 0x3000u);
    CHECK(BATTERY_BARS_CODE_END_VA == 0x00106F64u);
    // Both windows inside the mapped range
    CHECK(BATTERY_BARS_B_WINDOW_VA >= BATTERY_BARS_MAP_SRC_VA &&
          BATTERY_BARS_A_WINDOW_VA + 16 <= BATTERY_BARS_MAP_SRC_VA + BATTERY_BARS_MAP_SIZE);

    checkWindow("A", aExp, BatteryBars_VerifyA);
    checkWindow("B", bExp, BatteryBars_VerifyB);
    CHECK(!BatteryBars_VerifyA(bExp) && !BatteryBars_VerifyB(aExp));   // not interchangeable
    // The register field of the cmp is part of the window: "cmp r0" in A, "cmp r2" in B
    u8 buf[16];
    memcpy(buf, aExp, 16); buf[9] = 0x2A;
    CHECK(!BatteryBars_VerifyA(buf));
    memcpy(buf, bExp, 16); buf[9] = 0x28;
    CHECK(!BatteryBars_VerifyB(buf));
}

// ---- 4. Dump ----

static u32 countHits(const u8 *data, long size, VerifyFn verify, u32 *lastOff)
{
    u32 hits = 0;
    for (long i = 0; i + BATTERY_BARS_WINDOW_LEN <= size; i++)
        if (verify(data + i))
        {
            hits++;
            *lastOff = (u32)i;
        }
    return hits;
}

// Bytes 8 and 12 ignored entirely (stronger uniqueness than the whitelists)
static u32 countHitsIgnoring(const u8 *data, long size, const u8 *window, u32 *lastOff)
{
    u32 hits = 0;
    for (long i = 0; i + BATTERY_BARS_WINDOW_LEN <= size; i++)
    {
        bool ok = true;
        for (u32 k = 0; k < BATTERY_BARS_WINDOW_LEN && ok; k++)
            ok = k == BATTERY_BARS_CMP_INDEX || k == BATTERY_BARS_MOVS_INDEX || data[i + k] == window[k];
        if (ok)
        {
            hits++;
            *lastOff = (u32)i;
        }
    }
    return hits;
}

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

    u32 aoff = BATTERY_BARS_A_WINDOW_VA - 0x100000, boff = BATTERY_BARS_B_WINDOW_VA - 0x100000;
    CHECK((long)aoff + 0x60 <= size && (long)boff + 0x50 <= size);
    if ((long)aoff + 0x60 > size || (long)boff + 0x50 > size)
    {
        free(data);
        return 1;
    }
    CHECK(memcmp(data + aoff, BatteryBars_WindowA(), 16) == 0);
    CHECK(memcmp(data + boff, BatteryBars_WindowB(), 16) == 0);
    // Function starts and the instructions the interpreter assumes before the cmp: copy A "push {r3-r5,lr}" at
    // 0x106f38 and "movs r1, #1" at 0x106f52; copy B "push {r3-r5,lr}" at 0x104cc0 and "movs r5, #0" at 0x104cc8;
    // the stores "lsls r0, r1, #2; orrs r2, r0; strb r2, [r4]" at 0x106f92 and "strb r1, [r4]" at 0x104cea
    CHECK(data[0x6f38] == 0x38 && data[0x6f39] == 0xB5 && data[0x6f52] == 0x01 && data[0x6f53] == 0x21);
    CHECK(data[0x4cc0] == 0x38 && data[0x4cc1] == 0xB5 && data[0x4cc8] == 0x00 && data[0x4cc9] == 0x25);
    CHECK(data[0x6f92] == 0x88 && data[0x6f93] == 0x00 && data[0x6f94] == 0x02 && data[0x6f95] == 0x43 &&
          data[0x6f96] == 0x22 && data[0x6f97] == 0x70);
    CHECK(data[0x4cea] == 0x21 && data[0x4ceb] == 0x70 && data[0x4d12] == 0x25 && data[0x4d13] == 0x70);
    // Copy A stores to the shared page byte 0x1FF81085 (literal at 0x106f9c, loaded into r4 at 0x106f40)
    CHECK(data[0x6f9c] == 0x85 && data[0x6f9d] == 0x10 && data[0x6f9e] == 0xF8 && data[0x6f9f] == 0x1F);

    // ptm's own instructions, run for every input and byte state, agree with the model
    static const u8 cmps[2] = { 0x3C, 0x00 };
    u32 agree = 0, total = 0;
    for (u32 ci = 0; ci < 2; ci++)
        for (u32 m = 1; m <= 5; m++)
            for (u32 pct = 0; pct < 256; pct++)
            {
                int a = dumpA(data, pct, cmps[ci], (u8)m), b = dumpB(data, pct, cmps[ci], (u8)m);
                total++;
                if (a == modelA(pct, cmps[ci], (u8)m) && b == modelB(pct, cmps[ci], (u8)m) && a >= 0 && a <= 5 &&
                    b >= 0 && b <= 5)
                    agree++;
            }
    CHECK(agree == total);
    // Stock bytes: the stock mapping, from the dump's code
    for (u32 pct = 0; pct < 256; pct++)
        CHECK(dumpA(data, pct, 0x3C, 0x05) == stockLevel(pct) && dumpB(data, pct, 0x3C, 0x05) == stockLevel(pct));

    // Unique: no other offset verifies (whitelists), also with bytes 8 and 12 ignored
    u32 aHit = 0, bHit = 0, aAnyHit = 0, bAnyHit = 0;
    u32 aHits = countHits(data, size, BatteryBars_VerifyA, &aHit);
    u32 bHits = countHits(data, size, BatteryBars_VerifyB, &bHit);
    u32 aAny = countHitsIgnoring(data, size, BatteryBars_WindowA(), &aAnyHit);
    u32 bAny = countHitsIgnoring(data, size, BatteryBars_WindowB(), &bAnyHit);
    CHECK(aHits == 1 && aHit == aoff && aAny == 1 && aAnyHit == aoff);
    CHECK(bHits == 1 && bHit == boff && bAny == 1 && bAnyHit == boff);

    // After patching a copy (fed, L = 2): still exactly once, in place
    u8 *copy = malloc((size_t)size);
    CHECK(copy != NULL);
    if (copy != NULL)
    {
        memcpy(copy, data, (size_t)size);
        copy[aoff + 8] = copy[boff + 8] = 0x00;
        copy[aoff + 12] = copy[boff + 12] = 0x02;
        CHECK(countHits(copy, size, BatteryBars_VerifyA, &aHit) == 1 && aHit == aoff);
        CHECK(countHits(copy, size, BatteryBars_VerifyB, &bHit) == 1 && bHit == boff);
        free(copy);
    }

    printf("ptm dump: window A at offset 0x%lx (VA 0x%08lx): %lu match(es), %lu with bytes 8/12 ignored; window B at "
           "offset 0x%lx (VA 0x%08lx): %lu match(es), %lu with bytes 8/12 ignored; ptm's own code agrees with the model "
           "in %lu/%lu cases\n", (unsigned long)aoff, (unsigned long)BATTERY_BARS_A_WINDOW_VA, (unsigned long)aHits,
           (unsigned long)aAny, (unsigned long)boff, (unsigned long)BATTERY_BARS_B_WINDOW_VA, (unsigned long)bHits,
           (unsigned long)bAny, (unsigned long)agree, (unsigned long)total);
    free(data);
    return 1;
}

// ---- 5. config.ini ----

static void testConfig(void)
{
    BatteryLedConfig c;
    BatteryLedConfig_Defaults(&c);
    CHECK(c.bars && c.bars4Pct == 70 && c.bars3Pct == 40);
    CHECK(!BatteryLedConfig_Parse("# nothing\nfoo=1\n", &c) && c.bars);
    CHECK(BatteryLedConfig_Parse("bars=0\nbars_4=80\nbars_3=50\n", &c));
    BatteryLedConfig_Sanitize(&c);
    CHECK(!c.bars && c.bars4Pct == 80 && c.bars3Pct == 50);
    CHECK(BatteryLedConfig_Parse("bars=2\n", &c) && c.bars);
    BatteryLedConfig_Parse("bars=x\n", &c);
    CHECK(c.bars); // not a number: unchanged
    // A full file as BatteryLed_SetConfig writes it
    BatteryLedConfig_Defaults(&c);
    CHECK(BatteryLedConfig_Parse("# Battery Manager settings (docs/battery-manager.md, section 12)\nled_override=1\n"
                                 "red_pct=12\nblink_pct=4\nblue_mode=5\n# sleep_mode: 1 dim blue (default), 5 bright blue\n"
                                 "sleep_mode=1\n# bars: 1 HOME Menu battery bars from the corrected % (default), 0 stock\n"
                                 "bars=1\nbars_4=75\nbars_3=35\n", &c));
    BatteryLedConfig_Sanitize(&c);
    CHECK(c.override && c.redPct == 12 && c.blinkPct == 4 && c.bars && c.bars4Pct == 75 && c.bars3Pct == 35);
    // An old config.ini without the keys: defaults (bars on)
    BatteryLedConfig_Defaults(&c);
    CHECK(BatteryLedConfig_Parse("led_override=1\nred_pct=10\n", &c));
    BatteryLedConfig_Sanitize(&c);
    CHECK(c.bars && c.bars4Pct == 70 && c.bars3Pct == 40);

    // Sanitising: bars_4 > bars_3 > red_pct, bars_4 <= 100, else the defaults
    static const struct { const char *text; u8 red, b4, b3; } cases[] = {
        { "bars_4=40\nbars_3=40\n", 10, 70, 40 },     // equal
        { "bars_4=30\nbars_3=40\n", 10, 70, 40 },     // reversed
        { "bars_4=50\nbars_3=10\n", 10, 70, 40 },     // bars_3 == red_pct
        { "bars_4=50\nbars_3=9\n", 10, 70, 40 },      // bars_3 below red_pct
        { "bars_4=101\nbars_3=40\n", 10, 70, 40 },    // above 100
        { "bars_4=255\nbars_3=40\n", 10, 70, 40 },
        { "bars_4=99999\nbars_3=40\n", 10, 70, 40 },
        { "bars_4=100\nbars_3=11\n", 10, 100, 11 },   // extremes that are valid
        { "bars_4=12\nbars_3=11\n", 10, 12, 11 },
        { "red_pct=45\n", 45, 70, 46 },               // red_pct >= 40: defaults invalid, bars_3 just above it
        { "red_pct=50\nbars_4=55\nbars_3=60\n", 50, 70, 51 },
        { "red_pct=50\nbars_4=60\nbars_3=55\n", 50, 60, 55 },
        { "red_pct=39\n", 39, 70, 40 },
        { "red_pct=40\n", 40, 70, 41 },
        { "red_pct=0\nbars_4=3\nbars_3=2\n", 1, 3, 2 }, // red_pct clamped to 1 first
    };
    for (u32 i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        BatteryLedConfig_Defaults(&c);
        BatteryLedConfig_Parse(cases[i].text, &c);
        BatteryLedConfig_Sanitize(&c);
        CHECK(c.redPct == cases[i].red && c.bars4Pct == cases[i].b4 && c.bars3Pct == cases[i].b3);
        CHECK(c.bars4Pct <= 100 && c.bars4Pct > c.bars3Pct && c.bars3Pct > c.redPct && c.redPct > c.blinkPct);
    }
    // Exhaustive: any red / bars_3 / bars_4 ends valid, and valid input is kept
    for (u32 r = 0; r < 60; r++)
        for (u32 b3 = 0; b3 < 256; b3 += 3)
            for (u32 b4 = 0; b4 < 256; b4 += 3)
            {
                BatteryLedConfig_Defaults(&c);
                c.redPct = (u8)r;
                c.blinkPct = 0;
                c.bars3Pct = (u8)b3;
                c.bars4Pct = (u8)b4;
                BatteryLedConfig_Sanitize(&c);
                CHECK(c.bars4Pct <= 100 && c.bars4Pct > c.bars3Pct && c.bars3Pct > c.redPct);
                if (r >= 1 && r <= 50 && b4 <= 100 && b4 > b3 && b3 > r)
                    CHECK(c.bars3Pct == b3 && c.bars4Pct == b4);
            }
    printf("config.ini: bars / bars_4 / bars_3 parsed and sanitised\n");
}

int main(int argc, char **argv)
{
    testDecisionMatrix();
    testHysteresis();
    testWithLed();
    testModel();
    testVerify();
    testConfig();
    int dump = testDump(argc > 1 && argv[1][0] != 0 ? argv[1] : NULL);
    printf("%s\n", g_failures == 0 ? (dump ? "bars host test: OK" : "bars host test: OK (dump check skipped)")
                                   : "bars host test: FAILED");
    return g_failures == 0 ? 0 : 1;
}
