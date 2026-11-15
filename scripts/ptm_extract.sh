#!/usr/bin/env bash
# Turn a GodMode9 dump of the ptm system module into a decompressed code.bin and search
# the ARM disassembly for the mcu::RTC command header 0x002E0040 (SetPowerModeIndicatorState,
# i.e. the power LED mode in MCU reg 0x29; see docs/ptm-dump-guide.md).
#
# Usage: scripts/ptm_extract.sh <dump> [outdir]
#
#   <dump>   one of:
#            - 0004013020002202.dec.code  (GodMode9 "Extract .code": decrypted + decompressed)
#            - 0004013020002202.code      (same, the code was not compressed)
#            - a DECRYPTED .app / .cxi    (GodMode9 "Decrypt file (0:/gm9/out)")
#   outdir   default: ./ptm-out
#
# Output: <outdir>/code.bin, <outdir>/code.arm.s (full ARM disassembly, base 0x00100000),
#         <outdir>/hits.txt (literal-pool hits for 0x002E0040 and the LDRs that load them).
# Needs: python3, devkitARM arm-none-eabi-objdump ($OBJDUMP or /opt/devkitpro/devkitARM/bin).
# A still-encrypted .app is refused: decrypt it with GodMode9 first (no keys are needed then).
#
# AI disclosure: written with the help of AI (Claude Code), reviewed by the maintainer.

set -euo pipefail

die() { echo "error: $*" >&2; exit 1; }

[[ $# -ge 1 && $# -le 2 ]] || { sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
in="$1"
out="${2:-ptm-out}"
[[ -f "$in" ]] || die "$in not found"

OBJDUMP="${OBJDUMP:-/opt/devkitpro/devkitARM/bin/arm-none-eabi-objdump}"
[[ -x "$OBJDUMP" ]] || die "objdump not found at $OBJDUMP (set OBJDUMP=...)"
command -v python3 >/dev/null 2>&1 || die "python3 not found"

mkdir -p "$out"

# --- 1. produce code.bin (and the text base address) ----------------------------------
base="$(python3 -I - "$in" "$out/code.bin" <<'PY'
import struct, sys

src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()

def blz_decompress(comp):
    """3DS ExeFS .code 'reverse LZSS' (same algorithm as ctrtool/GodMode9)."""
    top_bottom, orig_bottom = struct.unpack_from("<II", comp, len(comp) - 8)
    size = len(comp)
    out_size = size + orig_bottom
    out = bytearray(out_size)
    out[:size] = comp
    index = size - ((top_bottom >> 24) & 0xFF)
    stop = size - (top_bottom & 0xFFFFFF)
    outi = out_size
    if stop < 0 or index > size:
        raise ValueError("bad BLZ footer")
    while index > stop:
        index -= 1
        control = comp[index]
        for _ in range(8):
            if index <= stop or outi <= 0:
                break
            if control & 0x80:
                if index < 2:
                    raise ValueError("BLZ underflow")
                index -= 2
                seg = comp[index] | (comp[index + 1] << 8)
                seglen = ((seg >> 12) & 0xF) + 3
                segoff = (seg & 0x0FFF) + 2
                for _ in range(seglen):
                    if outi + segoff >= out_size:
                        raise ValueError("BLZ back-reference out of range")
                    b = out[outi + segoff]
                    outi -= 1
                    out[outi] = b
            else:
                if outi < 1:
                    raise ValueError("BLZ overflow")
                outi -= 1
                index -= 1
                out[outi] = comp[index]
            control = (control << 1) & 0xFF
    return bytes(out)

text_base = 0x00100000
if len(data) >= 0x200 and data[0x100:0x104] == b"NCCH":
    flags7 = data[0x18F]
    if not (flags7 & 0x04):
        sys.exit("NCCH is encrypted (NoCrypto flag not set). In GodMode9 use "
                 "'NCCH image options... > Extract .code' or 'Decrypt file (0:/gm9/out)' first.")
    exh_size = struct.unpack_from("<I", data, 0x180)[0]
    if exh_size < 0x10:
        sys.exit("NCCH has no extended header (not a CXI?)")
    exh = data[0x200:0x200 + 0x400]
    compressed = bool(exh[0x0D] & 0x01)
    text_base = struct.unpack_from("<I", exh, 0x10)[0]
    exefs_off = struct.unpack_from("<I", data, 0x1A0)[0] * 0x200
    hdr = data[exefs_off:exefs_off + 0x200]
    code = None
    for i in range(10):
        name, off, size = struct.unpack_from("<8sII", hdr, i * 16)
        if name.rstrip(b"\0") == b".code" and size:
            start = exefs_off + 0x200 + off
            code = data[start:start + size]
    if code is None:
        sys.exit("no .code in ExeFS")
    if compressed:
        code = blz_decompress(code)
    print(f"NCCH: compressed={compressed} text=0x{text_base:08X} code={len(code)} bytes",
          file=sys.stderr)
else:
    code = data  # GodMode9 "Extract .code" output: already decrypted and decompressed
    print(f"raw code: {len(code)} bytes (assuming text base 0x{text_base:08X})", file=sys.stderr)

open(dst, "wb").write(code)
print(f"0x{text_base:08X}")
PY
)"

echo "==> code.bin: $(wc -c < "$out/code.bin" | tr -d ' ') bytes, base $base"
shasum -a 256 "$out/code.bin"

# --- 2. disassemble -------------------------------------------------------------------
"$OBJDUMP" -D -b binary -marm --adjust-vma="$base" "$out/code.bin" > "$out/code.arm.s"
echo "==> disassembly: $out/code.arm.s"

# --- 3. find 0x002E0040 literals and the PC-relative loads that use them --------------
python3 -I - "$out/code.bin" "$base" "$out/code.arm.s" > "$out/hits.txt" <<'PY'
import re, sys
code = open(sys.argv[1], "rb").read()
base = int(sys.argv[2], 16)
lines = open(sys.argv[3]).read().splitlines()
needle = (0x002E0040).to_bytes(4, "little")

lits = [base + i for i in range(0, len(code) - 3, 4) if code[i:i + 4] == needle]
print(f"literal 0x002E0040 at {len(lits)} aligned address(es):",
      " ".join(f"0x{a:08X}" for a in lits) or "none")
for a in lits:
    tag = f"@ 0x{a:x}"
    refs = [n for n, l in enumerate(lines) if l.rstrip().endswith(tag)]
    print(f"\n=== 0x{a:08X}: {len(refs)} PC-relative load(s)")
    for n in refs:
        print("\n".join(lines[max(0, n - 12):n + 14]))
        print("    ...")
if not lits:
    print("Not found as a literal: the header may be built with MOV/ORR; grep code.arm.s "
          "for '#3014656' (0x2E0000) and '#64' or for 'svc 0x00000032' (SendSyncRequest).")
PY
echo "==> $(head -n 1 "$out/hits.txt")"
echo "    details: $out/hits.txt"
