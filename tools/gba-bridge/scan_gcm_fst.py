#!/usr/bin/env python3
"""scan_gcm_fst.py — parse a GameCube ISO's FST to find the file containing
a specific offset. Used to locate the exact bounds of Madden's GBA ROM.
"""
import sys, struct
from pathlib import Path

iso = Path(sys.argv[1]).read_bytes()
print(f"ISO size: {len(iso):,}")

# Header fields
game_code = iso[0:4]
print(f"Game code: {game_code!r}")

# FST offset + size at 0x0424 / 0x0428 (big-endian on GameCube)
fst_off = int.from_bytes(iso[0x0424:0x0428], 'big')
fst_size = int.from_bytes(iso[0x0428:0x042c], 'big')
print(f"FST offset: 0x{fst_off:x}  size: 0x{fst_size:x}")

# First entry is root dir; bytes 8..11 = total number of entries
n_entries = int.from_bytes(iso[fst_off+8:fst_off+12], 'big')
print(f"FST entries: {n_entries}")

# String table starts after entries
str_table_off = fst_off + n_entries * 12
print(f"String table offset: 0x{str_table_off:x}")

target = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x0398a880
print(f"\nLooking for file containing offset 0x{target:x}...")

for i in range(n_entries):
    e = fst_off + i * 12
    is_dir = iso[e]
    name_off = int.from_bytes(iso[e+1:e+4], 'big')
    f_off = int.from_bytes(iso[e+4:e+8], 'big')
    f_size = int.from_bytes(iso[e+8:e+12], 'big')
    # Get name from string table (null-terminated ASCII)
    nm_end = iso.find(b'\x00', str_table_off + name_off)
    name = iso[str_table_off + name_off:nm_end].decode('ascii', errors='replace')
    if not is_dir and f_off <= target < f_off + f_size:
        print(f"  ➤ HIT  file={name!r}  offset=0x{f_off:x}  size={f_size} (0x{f_size:x})  ends=0x{f_off+f_size:x}")
    elif not is_dir and abs(f_off - target) < 0x10000:
        print(f"    near file={name!r} offset=0x{f_off:x} size={f_size}")
