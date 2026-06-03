#!/usr/bin/env python3
"""
png_to_splash.py — convert a PNG into a GBA Mode-3 (BGR555) splash image
emitted as a C `const uint16_t[]` array, ready to embed into the joypad
GBA payload's `splash_images/` directory.

Usage:
    python3 gba/tools/png_to_splash.py \
        --in  gba/joypad/assets/switch.png \
        --out gba/joypad/source/splash_images/img_switch.c \
        --name splash_img_switch \
        --max-width 96 --max-height 96

Output format:
    const uint16_t <NAME>_pixels[] = { 0x7C00, ... };
    const splash_image_t <NAME> = {
        .pixels = <NAME>_pixels,
        .width  = W,
        .height = H,
        .bg_color = 0x... (sampled from corners, or --bg overridden),
    };

GBA BGR555:
    bit 0..4 : R
    bit 5..9 : G
    bit 10..14: B
    bit 15   : unused (often 0; set to 1 for "transparent" in OBJ contexts
               but ignored in Mode 3).
"""

import argparse
import os
import sys
from collections import Counter

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow not installed. Run: pip install Pillow", file=sys.stderr)
    sys.exit(1)


def to_bgr555(rgba):
    """RGBA 8888 → GBA BGR555 (a uint16). Alpha is dropped; if alpha < 16
    the pixel is forced to 0x0000 (treated as a transparent / bg slot)."""
    r, g, b, a = rgba
    if a < 16:
        return 0x0000
    # 8 → 5 bit per channel (just shift right by 3; preserves brightness).
    r5 = (r >> 3) & 0x1F
    g5 = (g >> 3) & 0x1F
    b5 = (b >> 3) & 0x1F
    return (b5 << 10) | (g5 << 5) | r5


def sample_bg_color(img):
    """Pick the dominant color in the four corners as the background fill."""
    w, h = img.size
    corners = [
        img.getpixel((0, 0)),
        img.getpixel((w - 1, 0)),
        img.getpixel((0, h - 1)),
        img.getpixel((w - 1, h - 1)),
    ]
    c = Counter(tuple(p) for p in corners)
    return to_bgr555(c.most_common(1)[0][0])


def parse_color(spec):
    """Parse a CLI color spec like '#FF0000' or '255,0,0' or 'red' into BGR555."""
    if spec is None:
        return None
    s = spec.strip()
    if s.startswith("#") and len(s) == 7:
        r = int(s[1:3], 16)
        g = int(s[3:5], 16)
        b = int(s[5:7], 16)
        return to_bgr555((r, g, b, 255))
    if "," in s:
        parts = [int(p) for p in s.split(",")]
        return to_bgr555((parts[0], parts[1], parts[2], 255))
    # named color via PIL
    from PIL import ImageColor
    rgb = ImageColor.getrgb(s)
    return to_bgr555((rgb[0], rgb[1], rgb[2], 255))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in", dest="src", required=True, help="input PNG path")
    ap.add_argument("--out", required=True, help="output .c path")
    ap.add_argument("--name", required=True,
                    help="C identifier (e.g. splash_img_switch)")
    ap.add_argument("--max-width", type=int, default=128,
                    help="max width (default 128); image is fit-inside")
    ap.add_argument("--max-height", type=int, default=128,
                    help="max height (default 128)")
    ap.add_argument("--bg",
                    help="background color override (#RRGGBB, R,G,B, or name)."
                         " Default: dominant corner color.")
    args = ap.parse_args()

    if not os.path.isfile(args.src):
        print(f"ERROR: input not found: {args.src}", file=sys.stderr)
        sys.exit(2)

    img = Image.open(args.src).convert("RGBA")
    src_w, src_h = img.size

    # Fit-inside scaling (preserve aspect ratio).
    scale = min(args.max_width / src_w, args.max_height / src_h, 1.0)
    new_w = max(1, int(round(src_w * scale)))
    new_h = max(1, int(round(src_h * scale)))
    if (new_w, new_h) != (src_w, src_h):
        img = img.resize((new_w, new_h), Image.LANCZOS)

    bg_color = parse_color(args.bg) if args.bg else sample_bg_color(img)

    # 8bpp indexed quantize → 256-color palette (median-cut).
    # Palette layout:
    #   index 0    : bg color (so renderer's framebuffer fill works)
    #   index 1..254: quantized image colors (median-cut)
    #   index 255  : pure white — reserved for the mode-name text overlay
    #                that splash_image_render draws on top of the fallback
    #                generic image. Keeping it out of the quantizer's
    #                range means any image can be overlaid with white
    #                text without worrying about palette collisions.
    rgb = img.convert("RGB").quantize(colors=254, method=Image.MEDIANCUT)
    raw_pal = rgb.getpalette() or []
    # PIL pads with zeros to 768 bytes (256*3) for some Pillow versions but
    # not all. Make sure we have at least 254 entries' worth.
    raw_pal = list(raw_pal) + [0] * (254 * 3 - len(raw_pal))
    palette_bgr555 = [bg_color] + [
        to_bgr555((raw_pal[i * 3], raw_pal[i * 3 + 1],
                   raw_pal[i * 3 + 2], 255))
        for i in range(254)
    ] + [0x7FFF]  # index 255 = white text
    # Quantized image pixel values are 0..253 (range of `colors=254`).
    # Shift them up by 1 so they sit at 1..254, leaving 0 for bg and 255
    # for the text overlay.
    indexed = rgb.tobytes()
    pixels8 = bytes((b + 1) if b < 254 else 254 for b in indexed)

    name = args.name
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        f.write("// AUTOGENERATED by gba/tools/png_to_splash.py — do not edit.\n")
        f.write(f"// source: {os.path.relpath(args.src)}\n")
        f.write(f"// size:   {new_w}x{new_h} 8bpp ({new_w * new_h} bytes pixels + 512 bytes palette)\n")
        f.write('#include "../splash_image.h"\n\n')

        # Pixel data
        f.write(f"static const uint8_t {name}_pixels8[] = {{\n  ")
        line_count = 0
        for b in pixels8:
            f.write(f"0x{b:02X},")
            line_count += 1
            if line_count >= 16:
                f.write("\n  ")
                line_count = 0
            else:
                f.write(" ")
        f.write("\n};\n\n")

        # Palette
        f.write(f"static const uint16_t {name}_palette[] = {{\n  ")
        line_count = 0
        for px in palette_bgr555:
            f.write(f"0x{px:04X},")
            line_count += 1
            if line_count >= 12:
                f.write("\n  ")
                line_count = 0
            else:
                f.write(" ")
        f.write("\n};\n\n")

        # Struct
        f.write(f"const splash_image_t {name} = {{\n")
        f.write(f"    .pixels8  = {name}_pixels8,\n")
        f.write(f"    .palette  = {name}_palette,\n")
        f.write(f"    .pixels   = (void*)0,\n")
        f.write(f"    .width    = {new_w},\n")
        f.write(f"    .height   = {new_h},\n")
        f.write(f"    .bg_color = 0x{bg_color:04X},\n")
        f.write("};\n")

    print(f"wrote {args.out} ({new_w}x{new_h} 8bpp, bg=0x{bg_color:04X})")


if __name__ == "__main__":
    main()
