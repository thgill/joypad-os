#!/usr/bin/env python3
"""extract_gba_rom.py — scan a GameCube ISO for an embedded GBA multiboot ROM.

GBA ROMs all carry a fixed 156-byte Nintendo logo at offset 0x04 of the
header. We search the ISO for that byte pattern, then walk backwards 4
bytes to find the ROM start (the ARM branch instruction).

Multiboot ROMs in GC ISOs are usually a few-KB to ~256KB. We extract a
reasonable chunk from the start and write it as a .gba file the firmware
can multiboot directly.

Usage:
  ./extract_gba_rom.py <iso> [out.gba]

The Nintendo logo starts with: 24 ff ae 51 69 9a a2 21 3d 84 82 0a
(common to every legitimate GBA ROM).
"""
import sys
from pathlib import Path

NINTENDO_LOGO_PREFIX = bytes([
    0x24, 0xff, 0xae, 0x51, 0x69, 0x9a, 0xa2, 0x21,
    0x3d, 0x84, 0x82, 0x0a, 0x84, 0xe4, 0x09, 0xad,
    0x11, 0x24, 0x8b, 0x98, 0xc0, 0x81, 0x7f, 0x21,
    0xa3, 0x52, 0xbe, 0x19, 0x93, 0x09, 0xce, 0x20,
])

iso_path = Path(sys.argv[1])
data = iso_path.read_bytes()
print(f"[scan] iso size: {len(data):,} bytes")

matches = []
pos = 0
while True:
    i = data.find(NINTENDO_LOGO_PREFIX, pos)
    if i < 0: break
    # ROM start is 4 bytes before the logo (ARM branch instruction)
    rom_start = i - 4
    if rom_start < 0:
        pos = i + 1
        continue
    matches.append(rom_start)
    pos = i + 1

print(f"[scan] found {len(matches)} candidate GBA ROM headers")
for i, off in enumerate(matches):
    # Game code lives at offset 0xAC..0xAF, 4 ASCII bytes
    code = data[off+0xAC:off+0xB0]
    title = data[off+0xA0:off+0xAC].rstrip(b'\x00').decode('ascii', errors='replace')
    print(f"  [{i}] @ 0x{off:08x} ({off:,})  title={title!r}  code={code!r}")

if not matches:
    print("[scan] no GBA ROM signature found in ISO")
    sys.exit(1)

# Extract each match — write a generous 256 KB max chunk. The actual ROM
# is usually self-describing; firmware multiboot only needs the bytes the
# header says (length encoded into our_key during real multiboot, but for
# a raw ISO extract we'll just dump up to 256KB).
out_dir = Path('/tmp/gba_roms')
out_dir.mkdir(exist_ok=True)
for i, off in enumerate(matches):
    title = data[off+0xA0:off+0xAC].rstrip(b'\x00').decode('ascii', errors='replace')
    safe = ''.join(c if c.isalnum() else '_' for c in title) or f'rom{i}'
    chunk = data[off:off+256*1024]  # 256 KB max
    out = out_dir / f"{i:02d}_{safe}.gba"
    out.write_bytes(chunk)
    print(f"  → wrote {out} ({len(chunk):,} bytes)")
