# Dumping the ptm system module with GodMode9 (read-only)

> AI disclosure: this document was written with the help of AI (Claude Code) and reviewed
> by the maintainer. See [README – AI disclosure](../README.md#ai-disclosure).

**Why:** when the lid closes, the power LED blinks red although the Battery Manager writes mode 5
to MCU register `0x29`. The suspect is the system module **ptm**: late in its sleep sequence it
probably sends `mcu::RTC` command `0x2E` SetPowerLedState (header `0x002E0040`; 3dbrew calls it
SetPowerModeIndicatorState, i.e. reg `0x29`) with mode 2, and mode 0 on wake. To confirm this before thinking about a loader patch we need ptm's
executable code (`.code`) to disassemble on the Mac.

This guide only **reads** from the internal memory (SysNAND) and writes **only to the SD card**
(`0:/gm9/out/`). It never changes the console's system files.

---

## 0. Before you start

- You need GodMode9 as a Luma payload, normally `sd:/luma/payloads/GodMode9.firm`
  (this is where [3ds.hacks.guide](https://3ds.hacks.guide/) puts it). If it is missing, download
  `GodMode9.firm` from the latest release at
  [github.com/d0k3/GodMode9/releases](https://github.com/d0k3/GodMode9/releases) and copy it to
  `/luma/payloads/` (by ftpd or with the SD card in the Mac).
- Battery reasonably charged, or the charger plugged in.
- **Golden rule:** in GodMode9, whenever a prompt mentions **SysNAND**, **EmuNAND**,
  **"unlock write permissions"**, **"lvl1 / lvl2 / lvl3"**, or asks you to enter a button
  combination to confirm writing — press **B** (No / cancel). Nothing in this guide needs it.
  Also: no NAND restore, no "Fix CMACs", no scripts (`.gm9` / `.lua`), no "Inject".

### Why copying from `1:` to `0:` is safe

GodMode9 has write-permission levels. Its default level ("base", green) already allows writing to
the SD card (`0:`) and needs no prompt. Reading from SysNAND CTRNAND (`1:`) never needs a
permission at all. Every step below reads from `1:` and writes to `0:/gm9/out/`, so at most you
might see a prompt about **SD card** writing permissions — that one is fine to accept. Any prompt
that names SysNAND or EmuNAND is not part of this guide: press B.

## 1. Which ptm title is the right one

ptm has two title IDs ([3dbrew Title list](https://www.3dbrew.org/wiki/Title_list), System Modules):

| Title ID | What |
|---|---|
| `0004013000002202` | ptm (Old 3DS, and fallback) |
| `0004013020002202` | **New 3DS** ptm (since 8.1.0-0_New3DS) |

On a New 3DS the process manager first tries the TID with the `0x20000000` bit set and only
falls back to the plain TID if that fails (this repo's `sysmodules/pm/source/info.c`,
`N3DS_TID_BIT`). So on the New 3DS XL the module that actually runs is
**`0004013020002202`**, folder:

```
1:/title/00040130/20002202/content/
```

Inside you should see one `.app` file (e.g. `000000xx.app`, the number depends on the system
version) and one `.tmd` file. When you press A on the `.app`, GodMode9 shows
`tid: <0004013020002202>` at the top of the menu; that is how you know it is the right file.

If `00040130/00002202` also exists, it is harmless to dump it too (same steps), but
`20002202` is the important one.

> Do **not** use `00040130/20002203` / `00002203`: that is the SAFE_MODE ptm.

## 2. Start GodMode9

1. Power off the console completely.
2. Hold **START** and press **POWER**; keep holding START until a menu or GodMode9 appears.
   - With more than one file in `/luma/payloads/` the Luma chainloader menu appears: choose
     `GodMode9` with the D-Pad and press A.
3. GodMode9 may show prompts at start-up. Answer them like this:
   - "Essential files backup not found. Create one now?" — press **B** (No). Creating it embeds
     data into the NAND image, which is a NAND write. (It is useful, but not for today.)
   - "RTC date&time seems to be wrong. Set it now?" — press **B**.
   - Anything else mentioning SysNAND — press **B**.

## 3. Easiest route: "Extract .code" (gives decrypted, decompressed code)

GodMode9 can decrypt the module and decompress its `.code` in one step, writing the result to the
SD card. No keys are needed on the Mac afterwards.

1. In the drive list, select **`[1:] SYSNAND CTRNAND`** and press **A**.
2. Open `title` → `00040130` → `20002202` → `content` (press A on each folder).
   - Folders are sorted by name; scroll with the D-Pad (left/right jumps a page).
3. Put the cursor on the **`.app`** file (not the `.tmd`) and press **A**.
   - Check the top of the menu shows `tid: <0004013020002202>`.
4. Choose **"NCCH image options..."** and press A.
5. Choose **"Extract .code"** and press A.
   - It shows "Extracting .code, please wait..." and then
     "`.code` extracted to 0:/gm9/out". Press A to dismiss.
   - If a prompt about SD card write permission appears, accept it. If one about SysNAND
     appears, press B and stop (that should not happen).
6. Expected result on the SD card:

   ```
   sd:/gm9/out/0004013020002202.dec.code
   ```

   (`.dec` means GodMode9 decompressed it. If the module was stored uncompressed, the name is
   `0004013020002202.code` instead. Both are fine.)

Do **not** choose any other entry in "NCCH image options..." (in particular nothing with
"Inject", "Build", "Install", "Restore" or "Encrypt").

## 4. Backup route: also copy the raw files

Do this too, even if step 3 worked; it costs a few seconds and gives us the original files.

1. Still in `1:/title/00040130/20002202/content/`, put the cursor on the `.app` file and press A.
2. Choose **"Copy to 0:/gm9/out"** and press A.
3. Do the same for the **`.tmd`** file.
4. Optional, only needed if step 3 failed: on the `.app` press A → "NCCH image options..." →
   **"Decrypt file (0:/gm9/out)"**. This writes a *decrypted* copy into `0:/gm9/out/` and
   leaves the original in `1:` untouched. (For a file in `1:` GodMode9 only offers the
   variant with `(0:/gm9/out)`; if you ever see "Decrypt file (...)" without that destination,
   you are not in `1:` — press B.)

Expected files on the SD card:

```
sd:/gm9/out/0004013020002202.dec.code   (step 3)
sd:/gm9/out/000000xx.app                (step 4, still encrypted)
sd:/gm9/out/000000xx.tmd                (step 4)
```

If you did step 4.4, the decrypted `.app` also ends up in `sd:/gm9/out/` (it may replace the
encrypted copy with the same name; that is fine, the original stays in NAND).

The copied `.app` is still **encrypted** with the console's NAND keys. Decrypting it on the Mac
would need the 3DS key data (`boot9.bin` / `aes_keys.txt`), which is why the GodMode9 outputs
from step 3 (or 4.4) are preferred.

## 5. Back to normal

1. If GodMode9 ever asks "Write permissions were changed. Relock them?", press **A** (Yes).
2. Hold **R** and press **START** to power off (START alone reboots; HOME → "Poweroff system"
   also works).
3. Boot normally, start **ftpd** from the HOME Menu (Homebrew Launcher), and tell me (Claude) the
   console is ready. I download everything from `/gm9/out/` over FTP (anonymous, port 5000) and
   run the script below. Nothing needs to be deleted from the SD card.

## 6. On the Mac: `scripts/ptm_extract.sh`

```sh
scripts/ptm_extract.sh /path/to/0004013020002202.dec.code  ptm-out
# or with a decrypted .app from step 4.4:
scripts/ptm_extract.sh /path/to/000000xx.app  ptm-out
```

- Accepts GodMode9's `.code` / `.dec.code` (already decrypted and decompressed), or a
  **decrypted** NCCH (`.app` / `.cxi`): then it extracts `.code` from the ExeFS and decompresses
  it (3DS reverse-LZSS/BLZ, built-in Python) if the extended header says it is compressed.
  An encrypted `.app` is refused with a message.
- Writes `ptm-out/code.bin`, a full ARM disassembly `ptm-out/code.arm.s` (base `0x00100000`)
  made with devkitARM `arm-none-eabi-objdump`, and `ptm-out/hits.txt`: every word-aligned
  `0x002E0040` literal and the `ldr rX, [pc, ...]` instructions that load it, with context.
- Needs only `python3` and `/opt/devkitpro/devkitARM/bin/arm-none-eabi-objdump`
  (override with `OBJDUMP=...`). No ctrtool / 3dstool.
- If the literal is not found, the header may be built with MOV/ORR; `hits.txt` says what to
  grep for next.

The command header, its mode byte (expected `2` before sleep, `0` after wake) and the call
site found there decide whether a loader patch for ptm is needed. Note for that later step: Luma's
loader matches patches with the N3DS bit cleared (`sysmodules/loader/source/patcher.c`,
`progId &= ~0xF0000000ull`), so a ptm patch keyed on `0004013000002202` also applies to
`0004013020002202`.

> **Outcome:** the dump showed the call site (sleep LED callback at VA `0x101a1c`, mode byte at
> `0x101a28`) and the wake LED callback (VA `0x101990`, mode byte at `0x10199e`). Both are handled by a
> Rosalina runtime patch of those bytes in RAM instead of a loader patch, see [battery-manager.md section 14](battery-manager.md#14-ptm-sleep-and-wake-led-patch-milestone-4c).
> The `0x002E0040` literal is not in the dump: ptm's wrapper at VA `0x1023f0` passes the command number
> (`movs r1, #0x2e`) and the parameter count (`movs r2, #1`) to a helper that builds the header. The command is
> called mcu::RTC `0x2E` SetPowerLedState in the rest of the project.

## Sources

- GodMode9 source and README, [github.com/d0k3/GodMode9](https://github.com/d0k3/GodMode9)
  (checked 2026-11-15, commit `b1e0a97`): menu strings "NCCH image options...",
  "Extract .code", "Copy to 0:/gm9/out", "Decrypt file (0:/gm9/out)"; `ExtractCodeFromCxiFile`
  decrypts + decompresses and writes `0:/gm9/out/<programId>[.dec].code`; default write
  permission `PERM_BASE` includes SD card writes; the essential-backup start-up prompt embeds
  into the NAND image.
- [3dbrew Title list](https://www.3dbrew.org/wiki/Title_list) (ptm TIDs),
  3dbrew MCU Services (command `0x2E`, listed there as `0x002E0000` SetPowerModeIndicatorState, reg `0x29`;
  called SetPowerLedState in this project).
- [3ds.hacks.guide](https://3ds.hacks.guide/) (GodMode9 location `luma/payloads/`, START to
  launch).
