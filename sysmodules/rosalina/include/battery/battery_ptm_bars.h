/*
*   This file is part of Luma3DS-Gauge, an unofficial fork of Luma3DS.
*   Licensed under the GNU General Public License v3 or later, see LICENSE.txt.
*
*   Battery Manager: ptm battery bars patch (milestone 5). HOME Menu's battery icon from the corrected %.
*
*   The New 3DS ptm sysmodule (title 0004013020002202) maps the MCU's integer battery % (mcu::RTC 0x2D) to a
*   battery level 0..5 in two identical code copies:
*
*   Copy A, 0x106f38: writes the shared-page byte 0x1FF81085 (bits 2..4 = level). Called at init, from a periodic
*   timer tick in ptm's event handler (0x10069c) and at the end of the wake callback.
*       106f54  ands  r2, r0            ; shared byte & 0xe3 (level bits cleared)
*       106f56  mov   r0, sp
*       106f58  strb  r2, [r4]
*       106f5a  ldrb  r0, [r0]          ; MCU %
*       106f5c  cmp   r0, #60           ; <- byte 8 of window A (cmp immediate): 3C stock, 00 fed
*       106f5e  bls   0x106f64          ; untouched chain: 31-60 -> 4, 11-30 -> 3, 6-10 -> 2, 1-5 -> 1, 0 -> 0
*       106f60  movs  r1, #5            ; <- byte 12 of window A (movs immediate): 05 stock, 01..05 fed
*       106f62  b     0x106f92          ; store level << 2
*
*   Copy B, 0x104cc0: answers GetBatteryLevel (ptm:u 0x0007, all ptm services), computed on each request.
*       104cd0  bl    0x1064b8          ; read the MCU %
*       104cd4  mov   r1, sp
*       104cd6  ldrb  r2, [r1]
*       104cd8  cmp   r2, #60           ; <- byte 8 of window B
*       104cda  bls   0x104ce0
*       104cdc  movs  r1, #5            ; <- byte 12 of window B
*       104cde  b     0x104cea          ; store the level
*
*   Fed: "cmp #0; bls chain; movs r1, #L": % == 0 still goes down the untouched chain (level 0, shutdown semantics
*   kept), any % > 0 gives L (1..5). Every mix of old and new bytes gives a level in 0..5 (host-tested on a model
*   of both functions and on the dump's own instructions).
*
*   Verified before every write: process "ptm", its title ID, code at 0x00100000 covering window A, and both exact
*   16-byte windows with byte 8 in {3C, 00} and byte 12 in {01..05}. If either window fails, the bars patch is off
*   for the rest of the boot; the sleep / wake LED patch (battery_ptm_patch.h) is independent of it.
*   Design: docs/battery-manager.md, section 15.
*
*   The pure part (decision, verification) is in battery_ptm_bars_logic.c and is host-tested; the Rosalina part
*   (BatteryBars_Apply) is in battery_ptm_patch.c next to the sleep / wake patch, sharing its process checks.
*/

#pragma once

#include <3ds/types.h>
#include "battery/battery_led_logic.h"

#define BATTERY_BARS_WINDOW_LEN         16
#define BATTERY_BARS_CMP_INDEX          8       // window[8]  = imm8 of "cmp rX, #60"
#define BATTERY_BARS_MOVS_INDEX         12      // window[12] = imm8 of "movs r1, #5"

// Copy A (shared-page byte): window at 0x106f54, cmp byte 0x106f5c, movs byte 0x106f60
#define BATTERY_BARS_A_WINDOW_VA        0x00106F54u
#define BATTERY_BARS_A_CMP_VA           (BATTERY_BARS_A_WINDOW_VA + BATTERY_BARS_CMP_INDEX)    // 0x00106F5C
#define BATTERY_BARS_A_MOVS_VA          (BATTERY_BARS_A_WINDOW_VA + BATTERY_BARS_MOVS_INDEX)   // 0x00106F60
// Copy B (GetBatteryLevel): window at 0x104cd0, cmp byte 0x104cd8, movs byte 0x104cdc
#define BATTERY_BARS_B_WINDOW_VA        0x00104CD0u
#define BATTERY_BARS_B_CMP_VA           (BATTERY_BARS_B_WINDOW_VA + BATTERY_BARS_CMP_INDEX)    // 0x00104CD8
#define BATTERY_BARS_B_MOVS_VA          (BATTERY_BARS_B_WINDOW_VA + BATTERY_BARS_MOVS_INDEX)   // 0x00104CDC

// One mapping covers both windows: the pages 0x104000, 0x105000, 0x106000 (all .text)
#define BATTERY_BARS_MAP_SRC_VA         (BATTERY_BARS_B_WINDOW_VA & ~0xFFFu)                    // 0x00104000
#define BATTERY_BARS_MAP_SIZE           (((BATTERY_BARS_A_WINDOW_VA & ~0xFFFu) + 0x1000u) - BATTERY_BARS_MAP_SRC_VA) // 0x3000
#define BATTERY_BARS_CODE_END_VA        (BATTERY_BARS_A_WINDOW_VA + BATTERY_BARS_WINDOW_LEN)   // 0x00106F64

_Static_assert(BATTERY_BARS_B_WINDOW_VA < BATTERY_BARS_A_WINDOW_VA, "map starts at window B's page");
_Static_assert((BATTERY_BARS_A_WINDOW_VA & ~0xFFFu) == ((BATTERY_BARS_A_WINDOW_VA + BATTERY_BARS_WINDOW_LEN - 1) & ~0xFFFu), "window A in one page");
_Static_assert((BATTERY_BARS_B_WINDOW_VA & ~0xFFFu) == ((BATTERY_BARS_B_WINDOW_VA + BATTERY_BARS_WINDOW_LEN - 1) & ~0xFFFu), "window B in one page");
_Static_assert(BATTERY_BARS_MAP_SIZE == 0x3000u, "three pages mapped");

#define BATTERY_BARS_CMP_STOCK          0x3C    // cmp #60
#define BATTERY_BARS_CMP_FED            0x00    // cmp #0: every % > 0 takes the movs
#define BATTERY_BARS_MOVS_STOCK         0x05    // movs r1, #5
#define BATTERY_BARS_LEVEL_MIN          1       // fed levels: 1 (blinking red bar) .. 5 (4 bars); 0 is never written
#define BATTERY_BARS_LEVEL_MAX          5
#define BATTERY_BARS_STOCK              0       // decided level 0 = stock bytes (3C / 05)

#define BATTERY_BARS_HYSTERESIS         1.0f    // points above a bars_4 / bars_3 threshold needed to step up again
#define BATTERY_BARS_ERROR_POLLS        3       // MCU reads failing this many polls in a row -> stock

// Settings (config.ini bars / bars_4 / bars_3; the fields live in BatteryLedConfig)
#define BATTERY_BARS_DEFAULT_ON         true
#define BATTERY_BARS_DEFAULT_4          70      // 4 bars at corrected >= bars_4
#define BATTERY_BARS_DEFAULT_3          40      // 3 bars at corrected >= bars_3
#define BATTERY_BARS_MAX_PCT            100

/// Why the bars are what they are (Status screen).
typedef enum BatteryBarsReason
{
    BATTERY_BARS_REASON_FED = 0,    // from the corrected %
    BATTERY_BARS_REASON_OFF,        // setting "stock"
    BATTERY_BARS_REASON_NO_DATA,    // no MCU reading yet
    BATTERY_BARS_REASON_MCU_ERROR,  // MCU reads failing
    BATTERY_BARS_REASON_NO_CURVE,
    BATTERY_BARS_REASON_CHARGING,   // charger connected / charging (the LED's debounced state)
} BatteryBarsReason;

typedef struct BatteryBarsInputs
{
    bool haveData;      // at least one MCU reading this boot
    bool mcuError;      // the last BATTERY_BARS_ERROR_POLLS MCU reads failed
    bool onCharger;     // BatteryLedLogic.onCharger (debounced unplug, the same state the LED uses)
    float corrected;    // BatteryCurve_Corrected, < 0 if no curve
    bool ledLevelValid; // BatteryLedLogic.levelValid
    u8 ledLevel;        // BatteryLedLogic.level: the red / blink steps switch together with the LED
} BatteryBarsInputs;

/// State of the patch (Status screen).
typedef enum BatteryBarsState
{
    BATTERY_BARS_STATE_UNCHECKED = 0,   // not looked at yet this boot
    BATTERY_BARS_STATE_OK,              // verified; the bytes are BatteryBarsStatus.cmpCur / movsCur
    BATTERY_BARS_STATE_NO_PTM,          // no process named "ptm" (retried)
    BATTERY_BARS_STATE_MAP_ERROR,       // mapping failed (retried)
    BATTERY_BARS_STATE_WRITE_ERROR,     // the read-back after a write didn't match (retried)
    BATTERY_BARS_STATE_OFF_TITLE,       // "ptm" has another title ID: off for this boot
    BATTERY_BARS_STATE_OFF_LAYOUT,      // code not at 0x00100000 or .text too small: off for this boot
    BATTERY_BARS_STATE_OFF_MISMATCH,    // a 16-byte window doesn't match: off for this boot
} BatteryBarsState;

#define BATTERY_BARS_MISMATCH_A         0x01    // BatteryBarsStatus.mismatch bits (OFF_MISMATCH)
#define BATTERY_BARS_MISMATCH_B         0x02

typedef struct BatteryBarsStatus
{
    u8 state;           // BatteryBarsState
    u8 cmpCur;          // byte 8 of both windows when last verified / written (state OK)
    u8 movsCur;         // byte 12 of both windows (state OK)
    u8 levelWant;       // last level asked for (0 = stock)
    u8 mismatch;        // BATTERY_BARS_MISMATCH_* (OFF_MISMATCH)
    u32 writes;         // successful byte writes this boot (each changed byte counts once)
    u32 checks;         // verifications this boot (each maps the pages once)
    Result lastRes;     // last failing SVC result (NO_PTM / MAP_ERROR)
    u64 titleId;        // title ID read from "ptm" (OFF_TITLE)
} BatteryBarsStatus;

// ---- Pure (host-tested, battery_ptm_bars_logic.c) ----

/// Level 1..5 for HOME Menu from the corrected %, or BATTERY_BARS_STOCK (0). prev: the level decided last time
/// (for the 1-point hysteresis on bars_4 / bars_3). Stock with the setting off, no data, MCU errors, no curve,
/// or on the charger. Else: LED level blink -> 1, red -> 2; blue -> 5 at c >= bars_4, 4 at c >= bars_3, else 3.
/// Stepping up within 3..5 needs c >= threshold + BATTERY_BARS_HYSTERESIS; stepping down is immediate.
/// *outReason may be NULL.
u8 BatteryBars_Decide(const BatteryLedConfig *cfg, const BatteryBarsInputs *in, u8 prev, u8 *outReason);
/// The two immediates for a decided level: 0 -> 3C / 05 (stock), 1..5 -> 00 / L. False (and stock) otherwise.
bool BatteryBars_BytesFor(u8 level, u8 *cmp, u8 *movs);
/// Whitelists: byte 8 in {3C, 00}, byte 12 in {01..05}.
bool BatteryBars_CmpAllowed(u8 b);
bool BatteryBars_MovsAllowed(u8 b);
/// True if the 16 bytes equal window A / B with byte 8 and byte 12 in their whitelists and the other 14 exact.
bool BatteryBars_VerifyA(const u8 *bytes);
bool BatteryBars_VerifyB(const u8 *bytes);
/// The expected windows (stock: byte 8 = 3C, byte 12 = 05).
const u8 *BatteryBars_WindowA(void);
const u8 *BatteryBars_WindowB(void);
const char *BatteryBars_ReasonName(u8 reason);
const char *BatteryBars_StateName(u8 state);

// ---- Rosalina (battery_ptm_patch.c) ----

/// Call once before any other call (BatteryLed_CreateThread, after BatteryPtmPatch_Init).
void BatteryBars_Init(void);
/// Makes both ptm copies give `level` (1..5) for any % > 0, or restores the stock bytes (level 0). Does nothing for
/// other values, once the patch is off for this boot, or (no mapping) if the bytes last verified / written are
/// already right. Verifies both windows first; writes only bytes that differ, in a safe order (fed: movs then cmp;
/// stock: cmp then movs), with cache maintenance after each step; reads back and verifies. Thread-safe.
void BatteryBars_Apply(u8 level);
void BatteryBars_GetStatus(BatteryBarsStatus *out);
/// True in a state where a later Apply may succeed (no ptm, map error, write error).
bool BatteryBars_Retryable(u8 state);
