# Battery Manager (Luma3DS-Gauge)

Luma3DS-Gauge is an unofficial fork of [Luma3DS](https://github.com/LumaTeam/Luma3DS) (based on **v13.5**).
It adds a **Battery Manager** to the Rosalina menu that learns the real discharge
curve of an aftermarket battery and corrects the power LED, so that red / blinking
red only appear when the battery is actually low.

> Status: milestone 2 (recorder) implemented, **not yet tested on hardware**.
> No MCU writes yet; LED control comes in later milestones.
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
| `0x29` | rw | Power LED mode (see below) | **only register we write** |
| `0x7F` | ro | Info table: `[2]` battery vendor, `[3..4]` MGIC version, `[5]` RCOMP(?), `[0x0A]` red LED on/off | log, LED watch |

Power LED modes (`0x29`):

| Value | Effect |
|---|---|
| 1 | forced default blue |
| 2 | sleep animation |
| 3 | "power off" mode |
| 4 | blue off, red on |
| 5 | red off, blue on |
| 6 | animate blue off, flash red |
| other | automatic (MCU decides) |

The extra 4 bytes of `0x29` (red LED pattern, write-only) are **not touched**.

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
2. Never touch `0x05–0x07` (MCU unlock sequence, "danger zone").
3. Open `mcu::HWC` briefly for each access and close it again, like Rosalina does.
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
  - `/luma/battery/config.ini`: settings
  - `/luma/battery/sessions/NNNN.csv`: one file per recorded discharge
  - `/luma/battery/curve.bin`: built curve (101 points)

### 4.2 Menu

```
Battery Manager
├─ Status            MCU %, corrected %, voltage, temp, charging, LED state, recorder state
├─ Start recording
├─ Stop recording
├─ Sessions          list (complete / incomplete / invalid), delete
├─ Build curve       select sessions (checkbox), build → curve.bin
├─ Show graph        corrected curve vs stock (diagonal) + min/max band
├─ LED override      on / off
└─ Thresholds        solid red at X % (default 10), blinking red at Y % (default 3)
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

- For each complete session: at each MCU % (0..100), compute
  *real % = remaining runtime / total runtime*. This equals remaining charge
  as long as the load stays roughly constant.
- Average the selected sessions point by point (**mean**). Warn when a session
  deviates strongly from the others (outlier flag).
- Stream the CSVs line by line. Memory use stays small.

### 4.5 LED control

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
- [ ] Does `0x29` survive sleep / wake?
- [ ] At what MCU % / voltage does the console actually shut down?
- [ ] Voltage scale: 5/256 or 5.12/256?
- [ ] What is `0x7F[5]` (RCOMP?) on this console?
- [ ] How wide is the spread between light / normal / heavy sessions?

## 7. Milestones

1. **Build setup**: build vanilla v13.5 `boot.firm` locally. Needs `makerom` and `firmtool`.
2. **Recorder**: background thread + Status / Start / Stop / Sessions. No writes to the MCU.
   *Done (code complete, builds; awaiting on-device test).* See section 9.
3. **LED experiment**: manual LED override toggle. Answers the open questions.
4. **Curve builder + graph.**
5. **Automatic LED control** from the curve.
6. Optional extras (4.6). Document findings on 3dbrew.

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
| `sysmodules/rosalina/source/battery/battery_mcu.c` | The **only** code that talks to `mcu::HWC`. Reads only. Has a whitelisted write function (1 byte to `0x29` only) that nothing calls yet; the linker drops it from the binary. |
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

```
# luma3ds-gauge recorder v1
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
t_ms,wall_ms,pct_int,pct_frac,pct,volt_raw,volt,temp_c,flags,adapter,charging,red_led,slider3d,blue_led
0,0,99,128,99.500,200,3.906,27,0x00,0,0,0,0,255
10000,10001,99,120,99.469,200,3.906,27,0x00,0,0,0,0,255
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
start % → end %); the curve builder (milestone 4) should also check it.

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
- [ ] `0x7F[0x0A]` really follows the red LED.
- [ ] `# end reason=shutdown` appears after a normal power-off, and after the console powers itself off at 0 %.
- [ ] `cutoff` recovery works after pulling the battery (or holding power).
- [ ] Sleep (closing the lid) produces a `# sleep gap=` line (does `svcGetSystemTick` stop during sleep? the wall clock is used as a fallback).
- [ ] Charger plug-in ends the session.
- [ ] No noticeable effect on games (thread wakes once per second).
