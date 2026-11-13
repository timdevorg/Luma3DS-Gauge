# Battery Manager (Luma3DS-Gauge)

Luma3DS-Gauge is an unofficial fork of [Luma3DS](https://github.com/LumaTeam/Luma3DS) (based on **v13.5**).
It adds a **Battery Manager** to the Rosalina menu that learns the real discharge
curve of an aftermarket battery and corrects the power LED, so that red / blinking
red only appear when the battery is actually low.

> Status: research / planning. Nothing implemented yet.
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
