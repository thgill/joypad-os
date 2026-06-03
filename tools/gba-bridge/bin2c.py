#!/usr/bin/env python3
"""bin2c.py — turn a .gba file into a C source file like gba_payload.c"""
import sys
from pathlib import Path

if len(sys.argv) < 3:
    print("usage: bin2c.py <input.gba> <output.c>"); sys.exit(2)

in_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])

data = in_path.read_bytes()
# Trim trailing zero padding (common in extracts)
while len(data) and data[-1] == 0:
    data = data[:-1]
# 8-byte align (multiboot requirement)
while len(data) % 8:
    data += b'\x00'

with out_path.open('w') as f:
    f.write(f"// gba_payload.c — generated from {in_path.name} ({len(data)} bytes)\n")
    f.write("// Do not edit by hand — regenerate with tools/gba-bridge/bin2c.py\n")
    f.write("#include <stdint.h>\n\n")
    f.write("const uint8_t gba_payload[] = {\n")
    for i in range(0, len(data), 12):
        chunk = data[i:i+12]
        f.write("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
    f.write("};\n")
    f.write(f"const uint32_t gba_payload_len = {len(data)};\n")

print(f"wrote {out_path} ({len(data)} bytes embedded)")
