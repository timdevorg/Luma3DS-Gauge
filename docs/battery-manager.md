# Battery Manager (Luma3DS-Gauge)

Luma3DS-Gauge is an unofficial fork of [Luma3DS](https://github.com/LumaTeam/Luma3DS) (based on **v13.5**).
It adds a **Battery Manager** to the Rosalina menu that learns the real discharge
curve of an aftermarket battery and corrects the power LED, so that red / blinking
red only appear when the battery is actually low.

> Status: recorder (milestone 2) and curve builder + graph (milestone 3) tested on hardware
> (sessions 0001 and 0002, [section 10](#10-findings-from-session-0001)).
> LED control (milestone 4, [section 12](#12-led-control-milestone-4)) tested on hardware (LED test,
> override on); four issues found there (lid, charger, boot, power off) are fixed in
> [section 13](#13-on-device-results-and-refinements-milestone-4b), **not yet re-tested on hardware**.
> It is the only code that writes to the MCU (reg `0x29`, mode byte only) and is **off** until you
> switch it on in the menu. Since [section 14](#14-ptm-sleep-led-patch-milestone-4c) it also changes one
> verified byte in the running ptm process's RAM so that ptm's own sleep LED command matches the Sleep LED
> setting (lid closed at a blue level); **not yet tested on hardware**.
>
> AI disclosure: this document was written with the help of AI (Claude Code) and reviewed
> by the maintainer. See [README – AI disclosure](../README.md#ai-disclosure).

---

## 1. The problem

After replacing a New 3DS XL battery with an aftermarket cell (iFixit), the
power LED turns red and starts blinking ("1–5% left") while the console can
still run for about another hour.

## 2. Findings

### 2.1 How the 3DS measures the battery

- Battery state is owned by the **MCU** (the power-management microcontroller),
  not the ARM11/ARM9. The MCU talks to a fuel gauge chip ("MGIC", likely a Maxim
  ModelGauge-style part) on its own private I2C bus. The ARM side **cannot**
  reach the gauge directly.
- The gauge estimates state of charge **from voltage** using a model / RCOMP
  value supplied by the MCU firmware. There is no coulomb counting, so the
  usual "run it flat 2–3 times to calibrate" advice **does not retrain anything**.
- The MCU selects a battery profile from the battery's **middle pin**
  ("battery vendor code", MCU reg `0x7F` byte `0x02`). All listed OEM batteries are
  Maxell (`0x00`, middle pin tied to GND), including the New 3DS XL's SPR-003.
- hardware.hacks.guide confirms reports of wrong percentages with aftermarket
  batteries. It attributes them to hardcoded MCU tables that a battery "selects",
  with the curves not yet understood.

### 2.2 Measured on the target console

| Item | Value |
|---|---|
| Console | New 3DS XL, Luma3DS CFW |
| Battery | iFixit replacement |
| Battery vendor code (Rosalina → System info) | **0** (Maxell profile) |

**Conclusion:** the MCU applies the stock Maxell curve to a cell whose voltage
curve differs. The fix is to learn the real curve in software and correct the LED.

### 2.3 Relevant MCU registers (I2C device 3, via `mcu::HWC`)

Source: [3dbrew – I2C Registers](https://www.3dbrew.org/wiki/I2C_Registers).

| Reg | Access | Meaning | Use |
|---|---|---|---|
| `0x0A` | ro | Battery temperature (°C) | log |
| `0x0B` | ro | Battery % (integer, 0–100) | log, curve input |
| `0x0C` | ro | Battery % fraction: `0x0B + 0x0C/256` | log, curve input |
| `0x0D` | ro | System voltage: `0x0D * 5/256` V (load side, sags under load) | log |
| `0x0F` | ro | Flags: bit3 adapter, bit4 charging, bit5/6 backlights | log, session end |
| `0x29` | rw | Power LED mode (see below) | **only register we write** (mode byte), read back |
| `0x7F` | ro | Info table: `[2]` battery vendor, `[3..4]` MGIC version, `[5]` RCOMP(?), `[0x0A]` red LED on/off, `[0x0B]` blue LED intensity | log, LED watch |

Power LED modes (`0x29`):

| Value | Effect |
|---|---|
| 1 | forced default blue |
| 2 | sleep animation |
| 3 | "power off" mode |
| 4 | blue off, red on |
| 5 | red off, blue on |
| 6 | animate blue off, flash red |
| other (we use 0) | automatic (MCU decides) |

The extra 4 bytes of `0x29` (red LED pattern, write-only) are **not touched**. `0x29` is a 5-byte
"dynamic shared" register (`sd(5)` on 3dbrew): dynamic registers "often don't retain their set value",
so a read-back of the mode byte may not show what was written (section 12.3).

MCU interrupts (reg `0x10–0x13`): bit13 fires on battery % status change at
**10 %, 5 %, 0 %** while discharging. These are the moments the MCU might take
the LED back.

> Note: Rosalina computes voltage as `0.02 * reg` (5.12/256), while 3dbrew says 5/256.
> The difference is tiny. Confirm on hardware.

### 2.4 Existing code to build on (Luma3DS v13.5)

- `sysmodules/rosalina/source/menu.c` → `menuUpdateMcuInfo()`: opens `mcu::HWC`
  (steals the session if busy), reads `0x0A..0x0D` and the `0x7F` info table, and closes it.
- `sysmodules/rosalina/source/menus/sysconfig.c`: already writes `0x28`
  (LED brightness) via `MCUHWC_WriteRegister`.
- `sysmodules/rosalina/source/menus.c` → `RosalinaMenu_ShowSystemInfo()`: shows
  the battery vendor code.
- Prior art: BiPoLAR-3ds turns the power LED off via `mcu::HWC`.

### 2.5 What is out of reach (by design)

- **MCU firmware** (gauge model, hard shutdown point): not touched. Bricking risk.
- **Fuel gauge chip**: not reachable from the ARM side.
- **NAND**: never written.

---

## 3. Safety rules

1. The **only** MCU register ever written is `0x29`, mode byte only. Enforce
   this with a whitelist in code.
   The Battery Manager has exactly **two write paths** (besides its own files under `/luma/battery/`):
   - MCU reg `0x29`, mode byte, through `BatteryMcu_SetPowerLedMode()` (whitelist 0..6, section 12.2);
   - **one byte** in the RAM of the running ptm process (New 3DS ptm `0004013020002202`, VA `0x101a28`,
     values `02` / `01` / `05` only), through `BatteryPtmPatch_Apply()` after full verification
     (section 14). Never another process, never another address, never NAND / firmware / SD.
2. Never touch `0x05–0x07` (MCU unlock sequence, "danger zone").
3. Open `mcu::HWC` briefly for each access and close it again, like Rosalina does
   (reasoning in section 12.4).
4. Everything lives in `boot.firm` plus files under `/luma/battery/`. To revert,
   copy the official Luma `boot.firm` back.
5. Keep the NAND backup and `essential.exefs` off the SD card.

---

## 4. Design

### 4.1 Architecture

- A background **thread inside Rosalina** (always running, already has
  `mcu::HWC` access). The menu only controls it.
- Code lives in `sysmodules/rosalina/source/battery/` and `include/battery/`.
  Upstream files get minimal changes: one menu entry and one thread start.
  This keeps rebasing onto new Luma releases easy.
- Files on SD:
  - `/luma/battery/config.ini`: LED settings ([section 12.6](#126-settings-lumabatteryconfigini))
  - `/luma/battery/sessions/NNNN.csv`: one file per recorded discharge
  - `/luma/battery/curve.bin`: built curve (161-point MCU table + 81-point voltage table, [section 11](#11-curve-builder--graph-milestone-3))
  - `/luma/battery/curve_select.ini`: which sessions the curve builder uses

### 4.2 Menu

```
Battery Manager
├─ Status            MCU %, corrected %, voltage, temp, charging, LED state, recorder state
├─ Start recording
├─ Stop recording
├─ Sessions          list (complete / incomplete / invalid), delete
├─ Build curve       select sessions (checkbox), build → curve.bin
├─ Show graph        corrected curve vs stock (diagonal) + min/max band
├─ LED override      on / off (default off)
├─ Thresholds        solid red at X % (default 10), blinking red at Y % (default 3), blue mode 5 / 1
└─ LED test          write each LED mode by hand, with read-back
```

### 4.3 Recording

- One sample every ~10 s: timestamp, MCU %, voltage, temperature, flags,
  red-LED state, 3D slider.
- Flush every few samples. The last line is the shutdown point.
- The session ends automatically when the charger is plugged in.
- A session is **complete** only if it starts at ≥ 95 % and ends with the
  console powering itself off.
- Sleep gaps (time jumps) mark the session **invalid** for curve building.

Implementation details (milestone 2) are in [section 9](#9-recorder-implementation-milestone-2).

### 4.4 Curve building (on console)

- For each complete session: at each MCU % grid point, compute
  *real % = remaining runtime / total runtime*. This equals remaining charge
  as long as the load stays roughly constant.
- Average the selected sessions point by point (**mean**). Warn when a session
  deviates strongly from the others (outlier flag).
- Stream the CSVs line by line. Memory use stays small.
- Session 0001 showed that 1 % MCU steps are far too coarse at the bottom and that the
  MCU % stops moving at 1.000 %, so the implementation uses a non-uniform grid and a
  voltage table for the last few percent. Details in [section 11](#11-curve-builder--graph-milestone-3).

### 4.5 LED control

Original plan below; the implementation is described in [section 12](#12-led-control-milestone-4).

| Corrected % | Power LED |
|---|---|
| > X (10 %) | forced blue (`5`) |
| X..Y | solid red (`4`) |
| < Y (3 %) | blinking red (`6`) |
| charging / override off / no curve | automatic |

- Poll every 1–2 s. Poll every ~250 ms when MCU % is near 10 / 5 / 0 to hide any
  red flicker if the MCU takes the LED back.
- Watch `0x7F[0x0A]` (red LED state) and re-apply on mismatch. Count
  re-asserts so they show up in the log.
- Fallback if polling isn't enough: hook the MCU battery interrupt (bit13).

### 4.6 Later / optional

- Corrected % in the Rosalina menu header.
- Corrected HOME Menu battery icon (patch/hook `ptm` battery level). This is harder.
- Load-aware curve (brightness, 3D, wifi). Only if the data shows load matters.

---

## 5. Recording guide

- Charge until the charging LED turns off, wait 15–30 min more, then unplug.
- Turn power-saving mode **off**. Use your usual brightness, 3D and wifi settings.
- Run **one steady activity** per session (e.g. Animal Crossing, idle-ish).
- **No sleep** (don't close the lid) and no charger. Keep it at room temperature.
- Run until the console shuts itself off.
- Record 2–3 sessions. Optionally do light / normal / heavy sessions to see,
  from the graph's spread band, whether load matters.

---

## 6. Open questions (to answer with the logger)

- [ ] Does the MCU re-assert the red LED after `0x29 = 5`? When (10/5/0 %, wake, charger)?
  Milestone 4 counts re-asserts (Status, CSV v2 `reasserts`); needs a session with the override on.
- [ ] Does `0x29` survive sleep / wake? (We hand back to automatic before sleep anyway.)
- [ ] Does reading `0x29` return the mode that was written? (Status / LED test show the read-back.)
- [ ] Which blue mode does the MCU respect better, `5` or `1`?
- [x] At what MCU % / voltage does the console actually shut down?
  **MCU 1.0 %, `volt_raw` 155** (3.03 V at 5/256, 3.10 V at 0.02), session 0001.
- [x] Does `0x7F[0x0A]` track the red LED? **Yes**: 0 while blue, 1 when solid red, toggles
  between samples while blinking (session 0001).
- [x] Is a clean end line written when the console powers itself off at 0 %?
  **Yes**: `# end reason=shutdown` (session 0001).
- [ ] Voltage scale: 5/256 or 5.12/256? Still open (needs a multimeter reading).
- [ ] What is `0x7F[5]` (RCOMP?) on this console?
- [ ] How wide is the spread between light / normal / heavy sessions?

## 7. Milestones

1. **Build setup**: build vanilla v13.5 `boot.firm` locally. Needs `makerom` and `firmtool`.
2. **Recorder**: background thread + Status / Start / Stop / Sessions. No writes to the MCU.
   *Done, tested on hardware (session 0001).* See sections 9 and 10.
3. **Curve builder + graph** (originally planned after the LED experiment; done first because
   the recorded data was ready). *Done (code complete, builds, host test matches
   `build_curve.py`; awaiting on-device test).* See section 11.
4. **LED control** (the planned "LED experiment" and "automatic LED control" in one): override toggle,
   thresholds, LED test screen, automatic control from `BatteryCurve_Corrected()`, re-assert
   detection, sleep / shutdown hand-back. *Done (code complete, builds, host tests pass; awaiting
   on-device test).* See section 12. Refinements after the on-device tests: section 13; ptm sleep LED
   patch (lid closed at a blue level): section 14.
5. Optional extras (4.6). Document findings on 3dbrew.

## 8. Sources

- [3dbrew – I2C Registers](https://www.3dbrew.org/wiki/I2C_Registers)
- [3dbrew – MCUHWC:GetBatteryVoltage](https://www.3dbrew.org/wiki/MCUHWC:GetBatteryVoltage)
- [libctru – mcuhwc.h](https://libctru.devkitpro.org/mcuhwc_8h.html)
- [hardware.hacks.guide – Batteries](https://hardware.hacks.guide/wiki/Guides/Batteries)
- [BiPoLAR-3ds (GameBrew)](https://www.gamebrew.org/index.php?title=BiPoLAR-3ds)
- [iFixit – New 3DS XL battery calibration](https://ts.ifixit.com/Answers/View/840023/'New'-3DS+XL+Battery+Calibration)

## 9. Recorder implementation (milestone 2)

### 9.1 Code

| File | Role |
|---|---|
| `sysmodules/rosalina/source/battery/battery_mcu.c` | The **only** code that talks to `mcu::HWC`. Reads, plus (since milestone 4) the single whitelisted write `BatteryMcu_SetPowerLedMode()` (section 12.2). |
| `sysmodules/rosalina/source/battery/battery_recorder.c` | Recorder thread, start / stop, CSV writing, state file |
| `sysmodules/rosalina/source/battery/battery_sessions.c` | SD helpers, session list, parsing, classification, delete |
| `sysmodules/rosalina/source/battery/battery_menu.c` | Rosalina submenu |

Upstream changes: `source/battery` added to `SOURCES` in the Rosalina Makefile,
one menu entry in `menus.c` ("Battery Manager..." after "System configuration..."),
and the thread start + join in `main.c`.

- **Thread**: `MyThread_Create`, static 4 KiB stack, priority 58 (same as the task runner,
  the lowest of Rosalina's threads), `CORE_SYSTEM`. It waits 5 s after boot, then wakes up
  every 1 s (waiting on Rosalina's sticky `preTerminationEvent`) and takes a sample every 10 s
  while recording.
- **`mcu::HWC` access**: same pattern as `menuUpdateMcuInfo()` (get a session, steal one with
  `svcControlService(SERVICEOP_STEAL_CLIENT_SESSION)` if the port is full, access, close), but
  with a local handle and raw IPC (`0x00010082` read) instead of libctru's global
  `mcuHwc` handle. That way the recorder can never overwrite or close a handle that
  `menu.c`, `sysconfig.c` or `luma_config.c` opened. A `LightLock` serialises the recorder
  thread and the Status screen.
- **Reads per sample**: `0x08..0x0F` in one read (3D slider, volume, temperature, % int,
  % frac, voltage, `0x0E`, flags) and the `0x7F` info table (`0x13` bytes) in one read.
  The read wrapper refuses ranges touching `0x05–0x07` and `0x10–0x13` (interrupt bitmask).

### 9.2 Files on SD

All under `/luma/battery/` on the SD card (created when recording starts).

**`sessions/NNNN.csv`**: one per session; `NNNN` = highest existing number + 1.

Recorder v2 (milestone 4) appends four LED columns to the sample rows; see the end of this section.

```
# luma3ds-gauge recorder v2
# session=0003
# start=2026-11-13 21:40:00
# start_wall_ms=<osGetTime(), ms since 1900>
# interval_ms=10000
# battery_vendor=0
# mgic_version=<0x7F[3]>.<0x7F[4]>
# rcomp=0x..
# console_type=<0x7F[0]> pmic_vendor=<0x7F[1]>
# info_7f=<0x13 bytes of reg 0x7F, hex>
# mcu_fw=<major>.<minor>            (only if Rosalina already read it)
# volt=volt_raw*5/256 (3dbrew); Rosalina shows volt_raw*0.02
t_ms,wall_ms,pct_int,pct_frac,pct,volt_raw,volt,temp_c,flags,adapter,charging,red_led,slider3d,blue_led,corrected,led_desired,led_actual,reasserts
0,0,99,128,99.500,200,3.906,27,0xe2,0,0,0,0,255,99.71,5,5,0
10000,10001,99,120,99.469,200,3.906,27,0xe2,0,0,0,0,255,99.70,5,5,0
...
# sleep gap=<ms>                    (gap between two samples > 60 s)
...
# end reason=<stop|charger|shutdown|cutoff>
```

| Column | Meaning |
|---|---|
| `t_ms` | ms since session start, monotonic (`svcGetSystemTick`) |
| `wall_ms` | ms since session start by the wall clock (`osGetTime`); can jump if the clock changes |
| `pct_int`, `pct_frac` | raw `0x0B`, `0x0C` |
| `pct` | `pct_int + pct_frac/256`, 3 decimals |
| `volt_raw` | raw `0x0D` |
| `volt` | `volt_raw * 5/256` V (3dbrew scale). Rosalina's scale is `volt_raw * 0.02` |
| `temp_c` | `0x0A`, °C |
| `flags` | raw `0x0F` (hex); `adapter` = bit3, `charging` = bit4 |
| `red_led`, `blue_led` | `0x7F[0x0A]`, `0x7F[0x0B]` (empty if the info table couldn't be read) |
| `slider3d` | raw `0x08` |
| `corrected` | v2: corrected % the LED thread uses (median voltage), 2 decimals; `-1` without curve / data |
| `led_desired` | v2: LED mode the LED thread wants (`0` = automatic, also while the override is off) |
| `led_actual` | v2: read-back of `0x29` (mode byte; empty if unreadable) |
| `reasserts` | v2: LED re-asserts after a mismatch since this session started |

**v1 / v2 compatibility**: all parsers (`battery_sessions.c`, `battery_curve.c`, `build_curve.py`, the
host tests) read the columns **by position**, and only the first six (`t_ms` … `volt_raw`); v2 only
appends columns, so v1 files (0001, 0002) keep working. The host test converts the sessions to v2 and
checks that both give the same session info and a byte-identical curve.

The first sample is taken when recording starts. Samples are buffered in RAM and appended
(open, append, flush, close) every 6 samples (~1 min), so a hard power cut loses at most
about one minute and the file is closed most of the time.

End lines:

| Reason | When |
|---|---|
| `stop` | Stop recording in the menu |
| `charger` | a sample sees the adapter flag (`0x0F` bit3); recording stops (within ~10 s of plugging in). Starting with the charger plugged in is refused |
| `shutdown` | Rosalina gets the pre-termination notification (pm `0x2000`, sent on power-off / reboot): buffer flushed, end line written |
| `cutoff` | written at the next boot when the state file still says `recording=NNNN` and the CSV has no end line (power was lost without notice) |

**`state`**: `recording=NNNN` while recording, `recording=none` otherwise. Written at
start, and set back to `none` after the end line is written. At boot (5 s after Rosalina
starts) a leftover `recording=NNNN` closes that session as `cutoff`.
Recording **never** resumes automatically after boot.

### 9.3 Session classification (Sessions list)

| Class | Rule |
|---|---|
| `invalid` | contains a `# sleep` line |
| `complete` | first sample ≥ 95.0 % **and** ended by power-off (`shutdown`, `cutoff`, or no end line at all) **and** no sleep gap |
| `incomplete` | everything else (e.g. `stop`, `charger`, started below 95 %) |

The session being recorded is shown as `recording` and can't be deleted.
Note: a manual power-off also ends with `shutdown`, so a session you power off
yourself at 60 % looks "complete". Check the end % in the list (it shows
start % → end %). The curve builder flags complete sessions ending above 5 % as
"high end %" and leaves them unselected by default.

Sessions list keys: Up/Down select, A details, X or Y delete (asks for confirmation), B back.
It shows the 64 highest-numbered sessions, newest first. Opening it reads every CSV
(a few seconds for many long sessions).

### 9.4 Upload script (`scripts/upload.sh`)

Uploads `boot.firm` to a 3DS running [ftpd](https://github.com/mtheall/ftpd) (anonymous login).

```sh
echo 192.168.1.50 > scripts/.3ds-ip        # or: export THREEDS_IP=192.168.1.50
scripts/upload.sh                          # -> /luma/payloads/x_gauge.firm (hold X at boot)
scripts/upload.sh --build                  # run make first
scripts/upload.sh --boot                   # -> /boot.firm, after backing up the remote one
THREEDS_PORT=5000 scripts/upload.sh        # port (default 5000)
```

With `--boot` the existing remote `/boot.firm` is first downloaded to
`scripts/backups/boot.firm.<timestamp>`; if that fails, nothing is overwritten.
`scripts/.3ds-ip` and `scripts/backups/` are gitignored.
Testing via `x_gauge.firm` first is safer: a plain boot still starts the old `boot.firm`.

### 9.5 To verify on hardware

- [ ] Status screen values match the Rosalina menu footer (%, voltage, temperature).
- [x] `0x7F[0x0A]` really follows the red LED (session 0001).
- [x] `# end reason=shutdown` appears after the console powers itself off at 0 % (session 0001).
- [ ] `# end reason=shutdown` after a normal (manual) power-off.
- [ ] `cutoff` recovery works after pulling the battery (or holding power).
- [ ] Sleep (closing the lid) produces a `# sleep gap=` line (does `svcGetSystemTick` stop during sleep? the wall clock is used as a fallback).
- [ ] Charger plug-in ends the session.
- [ ] No noticeable effect on games (thread wakes once per second).

## 10. Findings from session 0001

First full discharge recorded with the recorder: New 3DS XL, iFixit battery, vendor code 0,
mostly idle Animal Crossing. **7 h 28 min** (7.47 h), 2688 samples, started at MCU 97.8 %,
ended with `# end reason=shutdown` when the console powered itself off.
The first part was **active play** (heavier load, temperature 25 → 31 °C), the rest idle (back
to 25–26 °C), so the load wasn't constant over the session.

*Real %* is time-based: remaining runtime / total runtime.

| MCU % | time left | real % (time-based) | volt (5/256) |
|---|---|---|---|
| 90 | 414.8 min | 92.6 | 3.926 |
| 75 | 320.4 | 71.5 | 3.789 |
| 50 | 231.3 | 51.6 | 3.633 |
| 25 | 177.1 | 39.5 | 3.535 |
| 10 (LED solid red) | 148.6 | 33.2 | 3.477 |
| 5 (LED blinking) | 137.8 | 30.7 | 3.477 |
| 3 | 111.2 | 24.8 | 3.418 |
| 2 | 58.7 | 13.1 | 3.320 |
| 1 | 17.5 | 3.9 | 3.223 |

Conclusions:

- **Top half is accurate**: within ~2 points at MCU 50 %.
- **Bottom is compressed**: the MCU's last 5 % last **2 h 18 min ≈ 31 % of the runtime**.
  The MCU's "empty" zone starts at ≈ 3.48 V while the iFixit cell still has ~30 % left there.
- **LED**: solid red (`red_led=1`, `blue_led=0`) from MCU **10.9 %**, with ~2 h 30 min left
  (2 h 31 min from the first sample with `red_led=1`). Blinking from **5.95 %**: `red_led`
  toggles between samples.
- **MCU % plateau**: the MCU % sat at exactly **1.000 for the final ~18 min**, so below about
  2 % the MCU % alone can't tell how much is left.
- **Shutdown** at `volt_raw` **155** (3.03 V at 5/256, 3.10 V at 0.02), MCU 1.0 %.
- Sample interval 10.0–10.2 s, no gaps.

## 11. Curve builder + graph (milestone 3)

### 11.1 Code

| File | Role |
|---|---|
| `sysmodules/rosalina/source/battery/battery_curve.c` | Pure math: CSV line parsing, per-session tables, combining, file checksum, lookup. No 3DS services, so it also compiles on the host. |
| `sysmodules/rosalina/source/battery/battery_curve_sd.c` | Builds from session CSVs on SD (streamed line by line), safe write of `curve.bin`, loaded curve + lock, `curve_select.ini` |
| `sysmodules/rosalina/source/battery/battery_graph.c` | Graph on the bottom screen, with minimal pixel / line helpers (Rosalina's `Draw_*` API only draws text) |
| `sysmodules/rosalina/source/battery/battery_menu.c` | "Build curve" and "Show graph" entries, "Corrected" line in Status |
| `scripts/build_curve.py` | Host reference implementation (Python 3, stdlib only) |
| `tests/battery/` | Host test: compiles `battery_curve.c` natively and compares it with `build_curve.py` |

No upstream (non-battery) files changed. The recorder thread loads `curve.bin` once after its
5 s boot delay. Static RAM: about 12 KiB (two 2 KiB curve buffers, build state); no heap.

### 11.2 Algorithm

Per selected session, in one streaming pass over its CSV:

- `pct = pct_int + pct_frac / 256` (exact; the `pct` text column is not used), `t = t_ms`,
  `T_end` = `t_ms` of the last sample.
- **MCU table**: for each grid point *g* (walking the grid downwards), the time *t(g)* when the
  MCU % is **first ≤ g**, linearly interpolated between the previous sample (above *g*) and this
  one. Grid points above the first sample get *t = 0* (real 100 %); grid points never reached
  (below the shutdown %) get *t = T_end* (real 0 %). Then `real(g) = 100 * (T_end - t(g)) / T_end`.
  "First time ≤ g" makes every session table monotonic by construction, also when the MCU %
  briefly goes up again.
- **Voltage table**: `volt_raw` is load-side and noisy, so it is smoothed with a **centered median
  over 7 samples (~1 min)**, attributed to the middle sample's time. Then the same first-crossing
  rule on the voltage grid.

Combining (sessions in **ascending session-number order**, on the console and in `build_curve.py`,
whatever order they were selected in; before milestone 4 the console used the menu order, newest
first, so its `curve.bin` listed `[0002, 0001]` and differed from the host's):

- per grid point: **mean** of the unrounded real % values, plus **min** and **max** (of the values
  rounded to 0.01) for the spread band. Mean, min and max are monotonic (a final pass enforces it
  anyway).
- **Outliers** (second pass over the CSVs, only with ≥ 2 sessions): for each session the mean
  absolute deviation over the MCU grid from the mean of the *other* sessions.
  More than **5 points** → flagged as outlier (the curve is still written; deselect it and rebuild).
- **Max spread**: largest max − min over the MCU grid.
- Rejected: sessions with a sleep gap, fewer than 30 samples or shorter than 10 min.

Session selection (Build curve list): only `complete` sessions are listed (the one being recorded
is not). A complete session ending above **5 %** is marked `high end %` (probably powered off by
hand) and unselected by default. Choices are saved in `/luma/battery/curve_select.ini`
(`NNNN=1` / `NNNN=0` lines); sessions not in the file get the default.

### 11.3 Grids, voltage fallback and lookup

**MCU grid** (161 points): 0.0–5.0 % in 0.1 steps, 5.5–20 % in 0.5 steps, 21–100 % in 1 steps.
Below 5 % one MCU % is worth up to ~12 real % (MCU 2 → 1 = 9 points), and the MCU % moves in steps
of ~0.1–0.3 %, so 0.1 matches its resolution. 5–20 % is nearly flat in real % (5 → 10 % is
only 2.5 points), and the top is close to linear, so coarser steps lose nothing.

**Voltage grid** (81 points): `volt_raw` 140.5–220.5 in steps of 1 (half-integers). The smoothed
voltage is an integer, so the first crossing of *v + 0.5* lies halfway between the last sample at
*v + 1* and the first at *v*. A lookup at integer *v* therefore lands in the middle of the time the
voltage sat at *v*. 140–220 covers this cell (155 at shutdown, ~205 when full) with margin.

**Lookup** `BatteryCurve_Corrected(pct, voltRaw)` (thread-safe; −1 if no curve is loaded):

```
m = MCU table at pct           (linear interpolation, clamped)
v = voltage table at voltRaw
w = 0 for pct >= 3.0, 1 for pct <= 2.0, linear in between     (w = 0 if voltRaw <= 0)
corrected = (1 - w) * m + w * min(v, m)
```

The voltage can only pull the estimate **down** (`min(v, m)`). With that, the result is
continuous and monotonic in both inputs (it never increases when the MCU % or the voltage
drops): with *w* falling as pct rises and *v ≤ m*, d/dpct = (1 − w)·m′ + (−w′)·(m − v) ≥ 0.
In the 1.000 % plateau *m* stays at 3.9 % and the voltage takes over. Heavy load sags the
voltage, which errs on the side of "lower". The LED milestone should pass a smoothed voltage
(the argument is a float for that).

Status shows `Corrected: xx.x%` (instantaneous `volt_raw`) once a curve is loaded.

### 11.4 `curve.bin` format (version 1)

`struct BatteryCurveFile` in `include/battery/battery_curve.h`, written as-is (little endian,
2024 bytes). Real % values are `u16` in hundredths (0–10000).

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | magic `0x56524342` ("BCRV") |
| 4 | u16 | version (1) |
| 6 | u16 | number of sessions used (1–16) |
| 8 | u16 | flags (bit0: voltage table valid) |
| 10 | u16 | MCU points (161) |
| 12 | u16 | voltage points (81) |
| 14 | u16 | max spread ×100 |
| 16 | u16 | number of outliers |
| 18 | u16[16] | session numbers used (0-padded) |
| 50 | u16[16] | per-session deviation ×100 |
| 82 | u16[161] ×4 | MCU grid (% ×10), mean, min, max |
| 1370 | u16[81] ×4 | voltage grid (`volt_raw` ×10), mean, min, max |
| 2018 | u16 | reserved (0) |
| 2020 | u32 | CRC-32 (zlib polynomial) of bytes 0–2019 |

The loader checks magic, version, point counts, ascending grids and the CRC.

**Safe write**: the curve goes to `curve.tmp` (flushed), then `curve.bin` is deleted and
`curve.tmp` renamed to `curve.bin`. After a power cut at any point there is either a valid
`curve.bin`, or a valid `curve.tmp` that the loader uses instead (or the old curve.bin plus an
invalid curve.tmp, which is ignored).

### 11.5 Menu

- **Build curve**: checkbox list of complete sessions, newest first:
  `>[x] 0001   7:28 end   1.0% in curve`. Flags: `high end %` (red), `outlier` / `in curve`
  (from the loaded curve). Up/Down select, **A** toggles, **X** or **START** builds, **B** back
  (the selection is saved). Building shows "Building... Pass n/2: session #NNNN (i of n)", then a
  summary: sessions used, max spread, outliers with each session's deviation, and the real % at
  MCU 50 / 25 / 10 / 5 / 2 / 1 %. At most 16 sessions per curve.
- **Show graph**: bottom screen. X = MCU %, Y = real %. Grey diagonal = stock, bright green =
  learned mean, dark green band = min..max. **Y** toggles the zoom (MCU 0–10 %, real 0–50 %),
  **B** back. Without a curve it shows the axes, the diagonal and a hint.

### 11.6 Host tools

```sh
# Tables, report and optional outputs
scripts/build_curve.py sessions/0001.csv [more.csv ...] [--bin curve.bin] [--csv table.csv] [--svg plot.svg]
#   --include-high-end    use complete sessions ending above 5 %
#   --include-incomplete  use sessions that aren't "complete"
#   --quiet               summary + report only

# Host test: C (battery_curve.c, compiled with clang) vs build_curve.py
tests/battery/run_host_test.sh sessions/0001.csv [more.csv ...]
GRAPH_OUT=/tmp/graph tests/battery/run_host_test.sh sessions/0001.csv   # also renders the graph to PPM
```

The host test checks that both produce a **byte-identical `curve.bin`**, that the report values
agree, and that the corrected % is monotonic and continuous over a pct × voltage sweep, and that
corrupted files are rejected. A `--svg` plot writes `plot.svg` and `plot_zoom.svg`.

Result on session 0001 (curve built from that session alone, so it reproduces its own
time-based real % by construction; this checks the math, not the prediction quality):

| Sample | MCU % | `volt_raw` | real % (time) | MCU table | corrected |
|---|---|---|---|---|---|
| first ≤ 50 % | 49.680 | 186 | 51.62 | 51.48 | 51.48 |
| first ≤ 25 % | 24.441 | 181 | 39.52 | 39.29 | 39.29 |
| first ≤ 10 % | 9.719 | 178 | 33.16 | 33.10 | 33.10 |
| first ≤ 5 % | 4.793 | 178 | 30.74 | 30.73 | 30.73 |
| first ≤ 3 % | 2.996 | 175 | 24.82 | 24.76 | 24.76 |
| first ≤ 2 % | 1.992 | 170 | 13.10 | 13.03 | 13.03 |
| first ≤ 1 % | 1.000 | 165 | 3.91 | 3.91 | 3.91 |
| 15 min before end | 1.000 | 164 | 3.31 | 3.91 | 3.02 |
| 10 min before end | 1.000 | 162 | 2.20 | 3.91 | 2.08 |
| 5 min before end | 1.000 | 159 | 1.08 | 3.91 | 0.93 |
| 2 min before end | 1.000 | 157 | 0.41 | 3.91 | 0.34 |
| last sample | 1.000 | 155 | 0.00 | 3.91 | 0.00 |

In the plateau the MCU table alone is stuck at 3.9 %, while the voltage fallback follows the
real remaining charge down to 0.

### 11.7 To verify on hardware

- [ ] Build curve from session 0001: summary matches the table above (MCU 10 % → 33.1 %).
- [ ] `curve.bin` on SD is byte-identical to `scripts/build_curve.py --bin` on the same CSV.
- [ ] Build time and UI: progress screen updates, no freeze; menu stays responsive afterwards.
- [ ] Graph: layout, colours and zoom on the real screen.
- [ ] Status shows "Corrected" after a reboot (curve loaded by the recorder thread at boot).
- [ ] Selection survives a reboot (`curve_select.ini`).
- [ ] No effect on the recording session / games (Rosalina RAM use grew by ~12 KiB static).

## 12. LED control (milestone 4)

### 12.1 Code

| File | Role |
|---|---|
| `sysmodules/rosalina/source/battery/battery_led_logic.c` | Pure decision logic (thresholds, "only gets worse", hysteresis, mismatch rules, voltage median, `config.ini` parsing). No 3DS services; compiled unchanged into the host test. |
| `sysmodules/rosalina/source/battery/battery_led.c` | LED thread: polls the MCU, decides, writes; notification hook; settings; LED test support |
| `sysmodules/rosalina/source/battery/battery_mcu.c` | `BatteryMcu_SetPowerLedMode()`, the only MCU write; `0x29` read-back added to the sample |
| `sysmodules/rosalina/source/battery/battery_menu.c` | "LED override", "Thresholds", "LED test" entries; LED lines in Status |
| `sysmodules/rosalina/source/battery/battery_recorder.c` | CSV v2 columns |
| `tests/battery/led_host_test.c` | LED unit tests + replay of recorded sessions |
| `tests/battery/sd_host_test.c`, `tests/battery/host_sd/` | Console SD code (session parser, curve builder, safe write) on host stubs |

Upstream file changed: `sysmodules/rosalina/source/main.c` only — the LED thread start + join, one
`BatteryLed_HandleNotification()` call in each of the sleep, shell and pre-termination handlers, and
three notification table entries (`0x108`, `0x211`, `0x212`).

Static RAM: 2 KiB thread stack + ~0.5 KiB state. Measured stack use (`-fstack-usage`): the deepest
battery frame on the LED thread is ~100 bytes, so 2 KiB leaves ample room for the libctru / FS calls.

### 12.2 The write path and the whitelist

- **One function writes to the MCU**: `BatteryMcu_SetPowerLedMode(u8 mode)` in `battery_mcu.c`. It is
  hard-wired to register `0x29`, size **1** (the mode byte; the 4 red-pattern bytes are never written),
  and refuses any mode outside **{0, 1, 2, 3, 4, 5, 6}** (0 automatic, 1 forced blue, 2 sleep animation,
  3 "power off", 4 red on / blue off, 5 red off / blue on, 6 blink red), i.e. exactly the modes 3dbrew
  documents. (Until section 13, 2 and 3 were refused.) The old generic `BatteryMcu_WriteRegister()` was
  removed.
- Its only caller is `batteryLedWriteLocked()` in `battery_led.c` (used by the thread, the notification
  hook, the shutdown latch and the LED test screen), which first asks the shutdown latch
  (`BatteryLedGate_Allow`, section 13.5). Verified on the build: `nm` / `objdump` of `rosalina.elf` show one
  `bl BatteryMcu_SetPowerLedMode` (from `batteryLedWriteLocked`) and one `WriteRegister` IPC header
  (`0x00020082`) in battery code. The other `MCUHWC_WriteRegister` calls in the binary are upstream
  Rosalina's (`0x28` LED brightness, `0x18` IRQ mask for the power button option, `0x60` in
  `LumaConfig_SaveMcuConfig`), unchanged.
- **WriteRegister (IPC `0x00020082`, size 1) rather than `SetPowerLedState`.** Sources: libctru
  (`mcuhwc.h` and the disassembled `libctru.a`), 3dbrew "MCU Services" and "I2C Registers" (saved
  copies). libctru's `MCUHWC_SetPowerLedState` sends header `0x00060080` (2 parameter words, only one
  filled in), while 3dbrew lists mcu::HWC command 6 as `0x00060040` **SetPowerLEDPattern** (1 word).
  Header and name disagree, and nothing documents whether the MCU sysmodule writes only the mode byte or
  also the red-pattern bytes for it. WriteRegister with size 1 is exactly one byte to `0x29` by
  construction, uses the same command upstream Rosalina already uses for `0x28`, and the read side uses
  the same register. The mcu::RTC command `0x002E0000` "SetPowerModeIndicatorState" was not used:
  mcu::RTC is ptm's session, and the header (no parameters) looks wrong.
- "Automatic" (0) is only ever written to undo our own earlier write: if nothing was written this boot,
  nothing is written (also not at boot, sleep or shutdown). Since section 13, sleep and shutdown no
  longer write automatic at all.

### 12.3 Behaviour

The corrected % is `BatteryCurve_Corrected(pct, median voltage)`. The voltage median is over the last
6 samples, taken every **10 s** (about 1 min, like the 7-sample median the voltage table was built
with). The voltage only matters at MCU ≤ 3 %, where it changes over tens of minutes.

Updated by section 13 (charging, lid, shutdown):

| Condition | Mode written |
|---|---|
| override off, no curve, control error | automatic (0), once, and only if we had written something else |
| adapter / charging | as below, from the MCU-table corrected %, level only gets better (13.3) |
| lid closed or sleep requested | blue → Sleep LED setting (default 5; 1, 2, or nothing for `system`, 12.11); red / blinking kept; nothing if nothing written this boot (13.2, 13.9) |
| shutdown | 3 ("power off") once if we own the LED, then nothing (13.5) |
| corrected > red threshold (default 10 %) | `blue_mode` (default 5, or 1) |
| blink threshold (default 3 %) < corrected ≤ red threshold | 4 (solid red) |
| corrected ≤ blink threshold | 6 (blinking red) |

- **Only gets worse**: within one discharge the level goes blue → red → blinking, never back, so the
  voltage recovering after a load drop can't make the LED flap. Unplugging the charger restarts it from
  the corrected % at that moment (13.3).
- **Hysteresis (0.5 points)**: after a settings change (thresholds, blue mode, override switched on) the
  level is re-evaluated once and may get better, but only if the corrected % is more than 0.5 above the
  threshold (e.g. red → blue after lowering X from 10 to 8 at 9.0 %, but not at 8.3 %). Raising a
  threshold applies at once.
- **Writes**: only when the desired mode differs from the last written one, or to re-apply after a
  mismatch; at most one write per 250 ms.
- **Mismatch** (checked on every poll after a grace period of 1 s after writing blue, 3 s after writing
  red / blinking, while the LED animates):

  | Desired | Mismatch when |
  |---|---|
  | blue (1 / 5) | `0x7F[0x0A]` red = 1, or `0x7F[0x0B]` blue = 0 |
  | solid red (4) | blue > 0, or red off on 2 polls in a row |
  | blinking (6) | blue > 0 (red toggles while blinking) |
  | any | `0x29` read-back ≠ desired, **only** once the read-back has been seen to return what we wrote ("trusted"); if it doesn't while the LED itself is right, it is ignored ("untrusted") |

  On a mismatch the mode is re-applied, the **re-assert counter** goes up (per boot; per recording
  session in the CSV / Status), the MCU % is remembered, and polling goes fast for 10 s.
- **Giving up**: 5 failed writes in a row, or more than 12 re-asserts within 60 s, switch control off
  (automatic is written, retried every 5 s if writing fails) and Status shows the error in red.
  Toggling the override clears it. Note: with Rosalina's "LED brightness" at 0 the blue intensity reads
  0 and blue would always look like a mismatch, so this would end in the error state.

### 12.4 Polling and the `mcu::HWC` session

- Poll = one session (`srvGetServiceHandle`, or steal), three reads (`0x08..0x0F`, `0x29`, `0x7F`),
  close. Cadence: every **10 s** while the override is off (only to keep the corrected % for Status and
  the recorder), every **1 s** while it is on, every **250 ms** when the MCU % is within ±0.6 of 10, 5 or
  0, for 10 s after a mismatch, and for 10 s after ptm's battery-low notifications (`0x212` at 10 %,
  `0x211` at 5 %).
- 3dbrew (MCU Services): the MCU sysmodule accepts **one session per service at a time**; a new session
  is accepted only after the thread of the previous one has finished (after the client closed it). Who
  normally holds mcu::HWC isn't documented (upstream Rosalina itself opens it briefly in `menu.c`,
  `sysconfig.c`, `luma_config.c`; homebrew can too). Hence:
  - **Not** keeping a session open while polling fast: anyone else connecting (including Rosalina's own
    `mcuHwcInit()`, which is non-blocking and would just fail) would be locked out for the whole fast
    phase. A brief open/read/close holds the slot for well under a millisecond per poll; at 4 Hz that is
    a negligible share, and only near the MCU's 10 / 5 / 0 % events.
  - When the slot is taken, the existing steal path (`svcControlService(SERVICEOP_STEAL_CLIENT_SESSION)`)
    gets a second handle to **that** session; the server thread handles its requests one at a time, so
    sharing it is safe, and closing our handle doesn't close the owner's session. Same as upstream
    `menuUpdateMcuInfo()`.
  - mcu::RTC (ptm's) is never opened; shell and adapter state come from `0x0F` via mcu::HWC.
- The LED thread is a sibling of the recorder thread (priority **56**: above the recorder / task runner
  at 58, so an SD flush can't delay it; below the menu at 52; `CORE_SYSTEM`; 2 KiB static stack). It
  runs whether or not a recording is active and waits on `preTerminationEvent` + a kick event.

### 12.5 Sleep, wake and shutdown

> Changed after the on-device test: sleep writes the sleep animation / keeps red, shutdown writes
> "power off" once and nothing after ([13.2](#132-sleep-issue-1), [13.5](#135-shutdown-issue-4)).
> The table below is the original milestone 4 behaviour.

Rosalina's notification thread (main.c) calls `BatteryLed_HandleNotification()`; the write happens
**synchronously** there, before the system can continue:

| Notification | Action |
|---|---|
| `0x214` shell closed (NS) | lid closed: write automatic (if we had written something) |
| `0x101` sleep requested (ptm) | same, before Rosalina replies to the sleep query, so ptm's sleep animation comes after it |
| `0x213` shell opened (also at boot), `0x102` sleep denied, `0x106` fully awake | clear the flags, wake the thread, which re-applies the mode |
| `0x212` / `0x211` battery low / very low (ptm) | fast polling for 10 s |
| `0x108` shutdown (ptm), `0x2000` pre-termination (pm) | write automatic, stop controlling, so the power-off animation works |

Fallbacks: the thread also treats "`0x0F` bit1 (shell open) clear" as lid closed, and if `0x213` were
missed it clears the lid flag after 3 polls that see the shell open; on exit the thread writes automatic
again if needed. Rosalina may subscribe to at most 17 notifications (sm); with the three new entries it
uses 15.

### 12.6 Settings (`/luma/battery/config.ini`)

```
# Battery Manager settings (docs/battery-manager.md, section 12)
led_override=0
red_pct=10
blink_pct=3
blue_mode=5
# sleep_mode: 5 blue on/red off, 1 forced blue, 2 MCU sleep animation, 0 system (write nothing)
sleep_mode=5
```

| Key | Default | Range |
|---|---|---|
| `led_override` | 0 | 0 / 1 |
| `red_pct` | 10 | 1–50 |
| `blink_pct` | 3 | 0 to `red_pct` − 1 |
| `blue_mode` | 5 | 5 (red off, blue on) or 1 (forced default blue) |
| `sleep_mode` | 5 | 5, 1, 2 or 0 / `system` (12.11); anything else (3, 4, 6, 7, 255, `-1`, text) = 5 |

Missing file or keys = defaults (so the override is **off** after the first install). Written by the
menu only, safely like `curve.bin` (`config.tmp`, delete, rename; the loader falls back to
`config.tmp`). Loaded by the LED thread as soon as the SD card can be opened (13.4), or earlier when a
menu entry needs it.

### 12.7 Menu

- **LED override: ON/OFF** (title shows the state): toggles and saves, shows what it means.
- **Sleep LED: blue(5) / blue(1) / anim(2) / system** (title shows the setting): each selection moves to
  the next value (5 → 1 → 2 → system → 5), saves `config.ini` and shows what it means (12.11).
- **Thresholds**: Up/Down select, Left/Right change: solid red X %, blinking Y % (Y < X enforced), blue
  mode 5 / 1. B saves (only if changed).
- **LED test**: Up/Down select a mode (0–6), A writes it; shows what was written, the `0x29`
  read-back and the red / blue state (refreshed 4× per second). Normal control pauses while the
  screen is open and is restored on exit.
- **Status**: corrected % (the LED's input, with the median voltage), override state + reason
  (`active`, `override off`, `no curve`, `charger`, `lid closed`, …, `fast poll`), desired and read-back
  mode, red / blue state, last written mode, re-asserts (boot / session, MCU % at the last one), the
  `Sleep:` diagnostics line (12.11), the `ptm sleep patch:` line (14.5), errors.

### 12.8 Host tests

`tests/battery/run_host_test.sh` (see 13.7 for running it without session CSVs) now also runs:

- `led_host_test`: unit tests (thresholds, only-worse, hysteresis after threshold edits, charger reset,
  lid closed / shutdown / override off / no curve / error → automatic, mismatch rules incl. grace,
  red-off streak, blink toggling, read-back trust, voltage median, `config.ini` parsing) and a replay
  of each session with the curve built from all given sessions (override on, defaults 10 / 3 / 5).
- `sd_host_test`: `battery_sessions.c` + `battery_curve_sd.c` on host stubs: v1 and v2 CSVs parse the
  same; `BatteryCurve_Build` with the sessions **newest first** (menu order) writes a `curve.bin`
  **byte-identical** to `build_curve.py --bin`; `BatteryFs_WriteSafely` leaves only the target.
- `build_curve.py` on the v2 copies gives the same `curve.bin` as on v1.

Replay result (curve from sessions 0001 + 0002):

| Session | LED | elapsed | time left | MCU % | corrected |
|---|---|---|---|---|---|
| 0001 (7 h 28 min) | blue (5) | 0:00 | 7:28 | 97.8 | 99.5 |
| | solid red | 6:43 | **0:44** | 1.70 | 9.97 |
| | blinking | 7:13 | **0:14** | 1.00 | 2.97 |
| 0002 (7 h 12 min) | blue (5) | 0:00 | 7:12 | 98.5 | 99.8 |
| | solid red | 6:28 | **0:43** | 1.70 | 9.97 |
| | blinking | 6:59 | **0:13** | 1.00 | 2.97 |

For comparison, the stock LED went solid red at MCU 10.9 % with **2 h 31 min / 2 h 28 min** left and
started blinking at MCU 5.95 % with **2 h 20 min / 2 h 17 min** left. Exactly two LED changes per
session, no flapping; without the "only gets worse" rule the thresholds would have been crossed 4
times in session 0001 (voltage recovery in the plateau) and 2 times in 0002.

### 12.9 Risks and limits

- Whether the MCU / ptm takes the LED back at 10 / 5 % (or after wake) is unknown until tested; the
  re-assert logic handles it, at the cost of a brief flash of the system's colour.
- The `0x29` read-back may not reflect writes ("dynamic" register); it is then ignored.
- If anything goes wrong: switch the override off (writes automatic), or boot the official Luma
  `boot.firm`; a reboot / power cycle also hands the LED back to the system (the MCU is never told
  anything persistent, and automatic is written on shutdown).

### 12.10 On-device test checklist

Superseded by [13.8](#138-on-device-checklist); results of this first run in 13.1.

1. Status: override shows **OFF**, reason `override off`, nothing written (`Written: none yet`).
2. **LED test** first: write 1, 4, 5, 6, then 0. Check the LED for each, note the `0x29` read-back and the
   red / blue values (answers "does the read-back work?"). Leave the screen: LED back to normal.
3. Build the curve from 0001 + 0002 if not done yet, then switch **LED override: ON**. At a high % the
   LED stays blue; Status: `active`, desired `blue(5)`, re-asserts 0.
4. Try `blue_mode` 1 in Thresholds once; note whether either mode behaves differently.
5. Start a recording (session 3), ideally **heavy load: 3D max, high brightness**, override on.
6. Watch the MCU 10 % and 5 % crossings (fast polling, `fast poll` in Status): does the red flash? Does
   the re-assert counter go up? Note the "last at" MCU %.
7. Lid close → the LED shows the normal sleep animation; open → back to the override colour within ~1 s.
   (The recording gets a sleep gap; do this outside the session 3 recording or accept an invalid session.)
8. Plug in the charger → automatic (charging LED behaviour normal); unplug → override again.
9. Power off from HOME → the normal power-off LED animation.
10. Later in the session: solid red at corrected ≤ 10 %, blinking at ≤ 3 %, never back to blue.
11. Afterwards: check `led_desired`, `led_actual`, `reasserts` in the CSV, and that the Sessions list
    and Build curve still read it (CSV v2).

### 12.11 Sleep LED setting (`sleep_mode`)

What the sleep hook writes when the lid closes (`0x214`) or sleep is requested (`0x101`) while the
override is on and the level is **blue**:

| `sleep_mode` | Menu | Written at sleep (blue level) |
|---|---|---|
| `5` (default) | `blue(5)` | 5: blue on, red off |
| `1` | `blue(1)` | 1: forced default blue |
| `2` | `anim(2)` | 2: the MCU's sleep animation (the milestone 4b behaviour; red when the MCU thinks the battery is low, 13.9) |
| `0` or the word `system` | `system` | nothing: the LED is left to the system / MCU. `0` here never means "write automatic"; the menu saves it as `0` |

- Missing key, a missing file, or an invalid value: 5.
- At the **first** sleep signal of a sleep the configured mode is written even if the LED should already
  show it (5 after 5), so the MCU has been told exactly that value right before sleep. A second signal of
  the same sleep (`0x101` after `0x214`) writes only if the mode isn't there yet.
- Unchanged: red / blinking red are kept (no write); nothing at all with the override off, no curve, the
  error state, during shutdown, or if nothing was written this boot; shutdown latch, hysteresis and the
  "only gets worse" rule. The write goes through `batteryLedWriteLocked` → `BatteryMcu_SetPowerLedMode`
  like every other (whitelist, gate). While closed, 5 / 1 are watched for mismatches as when open
  (re-asserted if the MCU shows red); `system` and 2 are not watched.
- Code: `BatteryLedLogic_SleepModeForLevel(cfg, level)` and `BatteryLedLogic_SleepWrite()` in
  `battery_led_logic.c` (host-tested); `batteryLedSleepLocked()` in `battery_led.c`.
- ptm sends its own power LED mode (2) later in its sleep routine; with `blue(5)` / `blue(1)` that byte
  in ptm is patched to the same value (section 14), otherwise it stays stock.

**Diagnostics (read-only).** Status shows

```
Sleep: wrote 5 / wake read 5 (n=3)
```

- `wrote`: the mode the sleep hook wrote at the last sleep entry, or `none` (setting `system`, red /
  blinking kept, override off, ...).
- `wake read`: `0x29` as read by the LED thread's first poll after wake that is no longer "lid closed"
  (from that poll's sample, i.e. before anything is written after wake); `-` while still closed / asleep,
  `?` if the read failed.
- `n`: sleep entries this boot (first of `0x214` / `0x101` per sleep).
- Before the first sleep: `Sleep: setting blue(5), no sleep yet`.

No extra MCU access: the wake value comes from the normal poll's `0x29` read. Not logged in the session
CSV (a marker line would need changes to the v1 / v2 parsers; a session with a sleep is invalid for the
curve anyway).

## 13. On-device results and refinements (milestone 4b)

### 13.1 Results of the first on-device test

Console: New 3DS XL, iFixit battery. At the time the MCU thought the battery was low (MCU 2.9 %,
corrected 23.3 %) and blinked red in automatic mode.

LED test screen:

| Mode written | Result | `0x29` read-back | Red / blue (`0x7F`) | LED |
|---|---|---|---|---|
| 0 | 0 | 0 | off / 0 (sampled while the MCU blinks low) | MCU's low-battery blinking |
| 1 | 0 | 1 | | blue |
| 4 | 0 | 4 | | red |
| 5 | 0 | 5 | | blue |
| 6 | 0 | 6 | red off (off phase of the blink) | blink red; from 5, blue fades over the first 1–2 blinks ("animate blue off") |

The read-back of `0x29` always equalled the written value, so the read-back check becomes "trusted".

- **Override on** at MCU 2.9 % / corrected 23.3 %: solid blue, stays blue, re-asserts 0 boot / 0 session.
  The MCU does not fight the override.
- **Notification (RGB) LED**: its low-battery red blinking stops while the override is on. It follows the
  power LED mode; nothing extra is needed for it.
- **Issues found**:
  1. Lid closed → handed back to automatic → the MCU showed its low-battery red blinking instead of a
     sleep indication. Lid open → blue again (good).
  2. Charger plugged in → automatic → red. Wanted: the corrected state while charging too.
  3. Boot → the MCU blinks red from power-on until the LED thread took over (6 s after Rosalina start).
  4. Power off → the normal power-off animation, **then** ~2 s of red flashing, then off. Cause: the
     pre-termination hand-back wrote automatic (0) after the system had set its power-off indication.

### 13.2 Sleep (issue 1)

On `0x214` (lid closed) and `0x101` (sleep requested) the notification hook writes, synchronously:

| LED level | Written |
|---|---|
| blue | **2** (sleep animation) |
| solid red | nothing (4 stays) |
| blinking red | nothing (6 stays) |

Never automatic. Nothing at all if the override is off, there is no curve, control is in the error state,
or nothing was written this boot (last written = none / automatic). The thread uses the same mapping
(fallback if `0x214` is missed: `0x0F` bit1 clear), and the mismatch watch ignores mode 2.
On wake (`0x213`, `0x102`, `0x106`) the thread is kicked and writes the normal mode again, as before.

**Red vs. blink during sleep: kept as is.** The stock LED also shows low battery instead of the sleep
animation; blinking uses less LED power than solid red, which matters while asleep; and switching
blink → solid red on every lid close and back on open would be confusing. It also means no write at all
during sleep entry when the LED is red.

### 13.3 Charging (issue 2)

The LED stays under control while the adapter is plugged in (`0x0F` bit3, or bit4):

- Corrected % from the curve's **MCU table only** (no voltage blend: the voltage is elevated while charging).
- The level starts from the discharge level and may only get **better** during one charge (red → blue,
  with the 0.5-point hysteresis), never worse. Booting on the charger (or a threshold change while
  charging) starts from the corrected % as it is.
- Fully charged (adapter in, charging flag clear, MCU ≥ 95 %): blue.
- Unplugged: the voltage median is reset (no charging voltages in it) and the discharge logic restarts from
  the corrected % at that moment, in either direction; from there it only gets worse again.
- The `0x7F` / `0x29` mismatch watch keeps running. Status reason: `charging`.
- Override off / no curve: unchanged (automatic only to undo our own write, else nothing).

### 13.4 Boot (issue 3)

The 5 s (recorder) and 6 s (LED) delays were arbitrary ("give the SD / FS time"). What they actually
waited for:

- **fs:USER** is already usable: Rosalina's own init calls `fsInit()` before `main()`.
- **The SD card** (`ARCHIVE_SDMC`) may not be openable yet right after Rosalina starts.
- **mcu::HWC** is registered only once the mcu sysmodule has been launched (after Rosalina, which is
  started with the KIPs); `batteryMcuOpen()` already checks `isServiceUsable("mcu::HWC")`.
- The recorder loading `curve.bin` before the LED thread started (no longer needed: the LED thread loads it).

Now: the LED thread checks `BatteryFs_SdReady()` (open + close the SD archive) every **200 ms** (at most
30 s, then defaults), loads `curve.bin` and `config.ini`, and then polls the MCU every **200 ms** until
the first read succeeds (for up to 30 s, then the normal cadence). The first poll with the override on
writes the mode at once. The recorder waits for the SD the same way, then does the cut-off recovery under
its lock (unchanged; it touches only `state` and the session CSV, the LED thread only `curve.bin` /
`config.ini`, so they don't race).

Expected takeover: about as soon as the mcu sysmodule is up plus ≤ 200 ms, estimated **~1–2 s after
Rosalina starts** (was ≥ 6 s). Not measured yet: Status shows a line `Boot s: Rosalina X SD Y 1st LED Z`
(seconds since the ARM11 tick counter started, ≈ power-on) when there are no write failures. The red
blinking before Rosalina runs (boot ROM, boot9strap, Luma, kernel start) is unavoidable: nothing of ours
runs then, and the MCU is in automatic after a power cycle.

### 13.5 Shutdown (issue 4)

A one-way latch, `BatteryLedGate` (pure code, host-tested), sits in front of every write:

- The **first** shutdown signal — `0x108` (ptm shutdown), `0x2000` (pre-termination) or the thread's exit
  path, whichever comes first — reads `0x29` and writes **3** ("power off") once, if we own the LED
  (last written mode is ours: not none, not automatic, not 3) and `0x29` doesn't already read 3. If the
  system has already set 3, nothing is written. If nothing was written this boot, nothing is written.
- After that the gate is closed: pre-termination, the thread exit, the error retry, the menu and the LED
  test screen can't write anything (no automatic that would override the system's power-off indication).
- Decision on "override active": the condition used is "we own the LED". With the override on that is
  the same; in the short window after switching the override off (before the thread writes automatic)
  or after the LED test screen, writing 3 is still better than leaving our mode or writing 0.
- Risk: on a **reboot** without `0x108`, mode 3 is written at pre-termination. If the MCU keeps it over
  the reboot, the LED may stay in "power off" mode until our thread writes again (~1–2 s after Rosalina
  start with the override on). To check on hardware (13.8).

### 13.6 Code

| File | Change |
|---|---|
| `battery_mcu.h` | whitelist 0..6, `BATTERY_MCU_LED_SLEEP` / `_POWER_OFF` |
| `battery_led_logic.c/.h` | charge ratchet, `levelValid` / `onCharger`, sleep mapping, `KEEP` (write nothing), `NeedsWrite`, `BatteryLedGate` |
| `battery_led.c/.h` | sleep / shutdown hooks, MCU-table corrected % on the adapter, boot readiness, boot timing in Status |
| `battery_recorder.c` | SD readiness instead of 5 s delay; no longer loads `curve.bin` |
| `battery_sessions.c/.h` | `BatteryFs_SdReady()` |
| `battery_menu.c` | LED test modes 2 and 3, boot timing line, texts |

`led_desired` in the CSV may now be 2, 3 or 254 (`keep`: nothing to write). Still exactly one MCU write
function: `nm` / `objdump` show one `bl BatteryMcu_SetPowerLedMode` (from `batteryLedWriteLocked`), one
battery `0x00020082` header; `batteryLedWriteLocked` is called from the thread, the notification hook,
`batteryLedShutdownLocked` and the LED test.

### 13.7 Host tests

Session CSVs now live in `sessions/` at the repo root (gitignored). `tests/battery/run_host_test.sh`
without arguments uses `sessions/0001.csv` + `0002.csv` if both exist; otherwise it runs the unit tests
only (LED logic, safe write) and prints that the curve comparison, session parsing and replay are
**skipped**. New unit tests: sleep → 2 / red / blink by level, wake resumes, sleep with nothing written,
charging only-better ratchet, unplug restart, full charge blue, charging with override off / no curve,
mismatch watch while charging, shutdown writes 3 exactly once and nothing after (pre-termination, thread
exit, menu, test screen), 0x29 already 3, nothing written when nothing was written this boot, whitelist
rejects 7 and other values. With the earlier session CSVs the replay is unchanged (2 LED changes per
session, same times as in 12.8).

### 13.8 On-device checklist

1. **LED test 2 and 3 first**: write 5, then 2 (sleep animation?), then 3 (what does "power off" look
   like: fade, off?). Note the `0x29` read-back. Then 0 and leave the screen.
2. Override on: Status `active`, blue; note the `Boot s:` line after a cold boot (expected 1st LED
   ~1–2 s after Rosalina).
3. **Lid** close → the Sleep LED setting (13.9, default solid blue) instead of red blinking; open → blue
   within ~1 s.
4. **Charger** plugged in → stays blue (Status `charging`); with a red level it may turn blue once the
   MCU-table corrected % is 0.5 above the threshold; never worse. Unplug → normal discharge logic.
5. **Boot** (cold, override on): red blinking only until shortly after Rosalina starts.
6. **Power off** from HOME → the normal power-off animation, **no** red flashing after it.
7. **Reboot** (Rosalina menu → Reboot) → LED back to normal on the next boot.
8. Later: run `tests/battery/run_host_test.sh` with the session CSVs in `sessions/`.

### 13.9 Lid closed at a blue level pulses red (sleep animation)

Second on-device run, override on, corrected level **blue**: closing the lid wrote mode 2 (13.2) and the
power LED **blinked / pulsed red**. The LED test screen with mode 2 showed the red LED toggling and blue
intensity 0. So the MCU's sleep animation uses the red LED when the MCU itself thinks the battery is low
(it did, at MCU ~3 %), just like its automatic mode. A second possibility, not excluded yet: ptm writes
`0x29` itself after our hook during sleep entry.

Change: the mode written at sleep is now the Sleep LED setting, default **5** (12.11). The `Sleep:` line
on the Status screen shows what was written and what `0x29` reads on wake, which tells the two
explanations apart: if we write 5 and wake reads 5 but the LED was red, the MCU ignores the mode during
sleep; if wake reads something else, someone (ptm / MCU) changed it.

> **Resolved:** ptm does overwrite it. Reverse engineering of the console's own ptm module shows a
> sleep LED callback that sends mode 2 after our hook; fixed by a runtime patch of that one byte, see
> [section 14](#14-ptm-sleep-led-patch-milestone-4c).

**Test plan** (override on, corrected level blue; ideally at a low MCU % where the stock LED would be red):

1. Menu: **Sleep LED** until it shows `blue(5)` (the default). Close the lid for about **1 minute**, note
   the LED colour (solid blue? red? pulsing? off?). Open, then Status: note the `Sleep:` line.
2. Same with **`blue(1)`**.
3. Same with **`system`** (nothing written; expected: whatever the MCU does, probably red).
4. Report for each: setting, LED colour while closed, `Sleep: wrote X / wake read Y (n=N)`.

## 14. ptm sleep LED patch (milestone 4c)

### 14.1 Finding

Result of section 13.9 and [ptm-dump-guide.md](ptm-dump-guide.md): with the lid closed at a blue level the
power LED pulses red because **ptm overwrites our mode**. Reverse engineered from the console's own ptm
(New 3DS, title `0004013020002202`, process name `ptm`, code at VA `0x00100000`; the GodMode9 "Extract
.code" dump):

- ptm's sleep routine publishes `0x104` (going to sleep), waits for the acks, then calls a sleep LED
  callback, and only after that `mcu::RTC` EnterSleep. So the callback runs **after** every notification
  hook of ours, and its mode wins.
- The callback, Thumb, at VA `0x101a1c`:

  ```
  101a1c  4806      ldr   r0, =0x10e290
  101a1e  2132      movs  r1, #0x32         ; brightness 50
  101a20  b510      push  {r4, lr}
  101a22  f000fd9b  bl    0x10255c          ; mcu::RTC 0x30 SetLedBrightness(50)
  101a26  4804      ldr   r0, =0x10e290
  101a28  2102      movs  r1, #2            ; power LED mode 2 = MCU sleep animation
  101a2a  f000fce1  bl    0x1023f0          ; mcu::RTC 0x2E SetPowerLedState(2)
  ```

- The 16 bytes at VA `0x101a1e` (file offset `0x1a1e` of the raw `.code`) are unique in the module:
  `32 21 10 B5 00 F0 9B FD 04 48 02 21 00 F0 E1 FC`. Byte 10 (VA `0x101a28`) is the immediate of
  `movs r1, #2`, i.e. the mode ptm sends.
- The wake callback sends mode 0 and brightness 255; left alone (our LED thread re-applies the mode after
  wake anyway).
- Mode 2 is the MCU's sleep animation, which is red when the MCU itself thinks the battery is low (13.9).

### 14.2 What the patch does

Rosalina changes byte 10 (VA `0x101a28`) in the running ptm process's code in RAM, so that ptm itself
sends the Sleep LED setting:

| Situation (decided like the sleep hook, lid closed) | Byte |
|---|---|
| override on, curve, level **blue**, Sleep LED `blue(5)` | `05` |
| same with Sleep LED `blue(1)` | `01` |
| level **red** or **blinking** | `02` (stock: the MCU's genuine low warning during sleep) |
| Sleep LED `anim(2)` or `system` | `02` |
| override off, no curve, control error, shutdown, LED test, no MCU data, nothing written this boot | `02` |

The brightness (`32`, i.e. 50) is never touched. The decision is `BatteryLedLogic_Decide` with the lid
closed on a **copy** of the level state (no side effects), then `BatteryPtmPatch_DesiredByte(mode, reason)`:
mode 5 / 1 with reason "lid closed" → that byte, everything else → `02`.

**When it is applied** (only written if the byte differs; if it equals the byte last verified / written,
nothing is mapped at all, so normally only the first call per change costs anything):

- In the notification hook for `0x214` (lid closed), `0x101` (sleep requested) and `0x104` (going to
  sleep), **after** the sleep hook's MCU write and **before** `main.c` acks / replies. ptm waits for those
  acks, so it cannot be executing the callback while we patch it. `0x104` is the last one before the
  callback: normally a no-op check.
- Once after the LED thread's first successful MCU read at boot (so Status shows the verification result
  before the first sleep).
- After every settings change from the menu (override, Sleep LED, thresholds). The menu is closed during
  sleep, so ptm is not in its sleep routine then; the write is a single byte, so even then ptm would
  execute either the old or the new instruction.
- Nothing at shutdown / pre-termination (ptm's shutdown does not use the sleep callback); the byte is gone
  at the next boot anyway.

### 14.3 Verification (before every write; any failure → nothing written)

1. `OpenProcessByName("ptm")` (Rosalina's `process_patches.c`; it opens the processes one at a time and
   closes every handle except ptm's). Not found → `off: no ptm`, retried at the next apply.
2. Title ID via `svcGetProcessInfo(..., 0x10001)` must be `0004013020002202`. Else → `off: title id` for
   the rest of the boot (e.g. the Old 3DS ptm `0004013000002202`).
3. `.text` start (`0x10005`) must be `0x00100000` and `.text` size (`0x10002`) must cover `0x101a2e`. Else
   → `off: layout` for the rest of the boot.
4. Map the one page `0x101000` and read the 16 bytes at `0x101a1e`: all must equal the pattern, except
   byte 10, which must be `02`, `01` or `05`. Else → `off: mismatch` for the rest of the boot (other ptm
   version). The pattern is **not** searched for anywhere else.
5. The value to write must be `02`, `01` or `05` (`BatteryPtmPatch_ByteAllowed`, also checked on entry).
6. After the write the 16 bytes are read back and verified again; byte 10 must be the new value, else
   `write error` (retried at the next apply).

### 14.4 Mechanism

`battery_ptm_patch.c`, reusing what Rosalina itself does for code patches (`input_redirection.c` for
hid / ir, `plugin/file_loader.c`, `gdb/mem.c`):

1. `OpenProcessByName` → handle; `svcGetProcessInfo` 0x10001 / 0x10005 / 0x10002 (checks above).
2. `svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, 0x0E800000, ptm, 0x101000, 0x1000, 0)`: maps the one code
   page read-write into Rosalina. Not at `0x00100000` like the upstream helpers, because the process list /
   memory viewer, the DSP dump, input redirection and the plugin loader map other processes there,
   possibly at the same time; `0x0E800000` is outside Rosalina's own fixed regions (`0x07000000` plugin
   block, `0x08000000` soc, `0x0D000000` / `0x0D800000` buffers, `0x14000000` code). A collision would only
   make the map fail (`map error`, retried).
3. Read and verify the 16 bytes, then one `strb` to `0x0E800A28` (= ptm VA `0x101a28`).
4. Cache maintenance, as Rosalina's GDB stub after writing code: `svcFlushEntireDataCache()` (our write
   sits in the data cache), then `svcInvalidateEntireInstructionCache()` (drop a stale copy of the
   instruction). The kernel's `svcUnmapProcessMemoryEx` does both again.
5. Read back + verify, `svcUnmapProcessMemoryEx`, `svcCloseHandle` on every path.

Serialized by its own lock and called with the LED lock held, so the decision and the write can't be
reordered between the notification, menu and LED threads. Stack: ~0.5 KiB on top of the caller
(`OpenProcessByName` has a 256-byte PID list). Time: one process scan plus map / unmap (each with a full
cache flush in the kernel), only when the byte changes; expected on the order of a millisecond (not
measured).

### 14.5 Status line

```
ptm sleep patch: 05 (active), writes 1
ptm sleep patch: 02 (stock), writes 2
ptm sleep patch: not checked yet, writes 0
ptm sleep patch: off: no ptm 0xFFFFFFFF
ptm sleep patch: off: mismatch, writes 0       (also: off: title id, off: layout)
ptm sleep patch: map error 0x........          (also: write error)
```

The hex value is the byte currently in ptm (`active` = not stock); `writes` counts successful writes this
boot. `off: …` / errors are shown in red.

### 14.6 Safety argument

- **One byte, in RAM, of one running process.** The only store is to VA `0x101a28` of the process named
  `ptm` with title `0004013020002202`, after the checks in 14.3, and only the values `02` / `01` / `05`.
  `02` is ptm's own value; `01` and `05` are documented reg `0x29` modes that this console's MCU has
  already shown correctly (LED test, 13.1). The instruction stays `movs r1, #imm8`; no code flow changes.
- **Gone at reboot.** Nothing is written to NAND, firmware, the SD card (no patch file, no
  `/luma/sysmodules`, no loader change) or the MCU. ptm's code is loaded fresh from NAND at every boot.
- **No new MCU register write.** ptm still sends its own `SetLedBrightness` / `SetPowerLedState`; we only
  change the mode value in its command. `BatteryMcu_SetPowerLedMode()` remains the only MCU write of the
  Battery Manager.
- **Refuses on anything unexpected.** Another firmware version, an Old 3DS ptm or a different layout turns
  the patch off for the boot; it never searches for the pattern elsewhere.
- Verified on the build: in `rosalina.elf` the only store in `BatteryPtmPatch_Apply` that does not go to
  its own state or stack buffer is `strb r5, [r3, #40]` with `r3 = 0x0E800A00` (= `0x0E800A28`, the
  mapped `0x101a28`); `BatteryPtmPatch_Apply` is called only from `batteryLedPtmPatchLocked`.

### 14.7 Code and host tests

| File | Role |
|---|---|
| `include/battery/battery_ptm_patch.h` | addresses, pattern constants, API |
| `source/battery/battery_ptm_patch_logic.c` | pure: `BatteryPtmPatch_DesiredByte`, `_Verify`, `_ByteAllowed`, `_StateName` |
| `source/battery/battery_ptm_patch.c` | Rosalina: `BatteryPtmPatch_Apply` (open, verify, map, write, cache, unmap, close), status |
| `source/battery/battery_led.c` | `batteryLedPtmPatchLocked()`: decision on a copy, called from the hook (`0x214` / `0x101` / `0x104`), boot, `BatteryLed_SetConfig` |
| `source/battery/battery_menu.c` | Status line |
| `tests/battery/ptm_host_test.c` | host test |

`tests/battery/run_host_test.sh` runs `ptm_host_test` in every mode:

- Decision: `05` / `01` for blue(5) / blue(1) at a blue level; `02` for anim(2), system, red, blinking,
  override off, no curve, error, shutdown and "nothing written this boot"; `DesiredByte` exhaustively over
  all reasons × modes.
- Verification: the pattern with `02` / `01` / `05` accepted, every other byte-10 value and all
  3 × 15 × 255 single-byte changes elsewhere rejected; the whitelist.
- With the dump (`PTM_CODE=<path>`, default `sessions/ptm/0004013020002202.dec.code`, gitignored; SKIPPED
  if missing): the pattern verifies at file offset `0x1a1e` and nowhere else in the file.

### 14.8 On-device test plan

Override on, corrected level **blue**, ideally at a low MCU % where the stock LED would be red.

1. After boot, Status: `ptm sleep patch: 02 (stock)` (with Sleep LED `blue(5)` it may already read
   `05 (active)`, set at boot). Anything with `off:` → note it and stop here (the patch refused; nothing
   was written).
2. Sleep LED **`blue(5)`**: Status shows `05 (active)`. Close the lid ~1 minute: expected **blue** (possibly
   dimmer than awake: ptm sets brightness 50 before the mode), **no red pulsing**. Open: LED back to the override
   colour within ~1 s, wake works normally (HOME Menu, sound, Wi-Fi).
3. Sleep LED **`blue(1)`**: Status `01 (active)`; lid closed → blue.
4. Sleep LED **`anim(2)`**: Status `02 (stock)`; lid closed → the MCU's pulse (red when the MCU thinks the
   battery is low, like 13.9).
5. Sleep LED **`system`**: `02 (stock)`; lid closed → stock behaviour.
6. Override **off**: `02 (stock)`.
7. Later, at a red level with `blue(5)`: closing the lid sets `02`; expected the MCU's red low warning.
8. Report: the Status line after each step (byte, state, `writes`), the `Sleep:` line and the LED colour
   while closed.
