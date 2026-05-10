#!/usr/bin/env python3
"""Generate font12x12 C array by nearest-neighbor upscaling the embedded 8x8 font."""

import re
import sys

DISPLAY_C = "src/drivers/display.c"

def parse_font8x8(path):
    glyphs = []
    in_font = False
    with open(path) as f:
        for line in f:
            if 'font8x8[95][8]' in line and '=' in line:
                in_font = True
                continue
            if not in_font:
                continue
            # strip comments
            stripped = re.sub(r'/\*.*?\*/', '', line).strip()
            if stripped.startswith('};'):
                break
            # find hex values in this line
            vals = re.findall(r'0x[0-9A-Fa-f]+', stripped)
            if vals:
                glyphs.append([int(v, 16) for v in vals])
    return glyphs

def upscale(glyph8, src_w=8, src_h=8, dst_w=12, dst_h=12):
    rows = []
    for dy in range(dst_h):
        sy = int(dy * src_h / dst_h)
        src_row = glyph8[sy]
        word = 0
        for dx in range(dst_w):
            sx = int(dx * src_w / dst_w)
            if src_row & (1 << sx):
                word |= (1 << dx)
        rows.append(word)
    return rows

def main():
    glyphs = parse_font8x8(DISPLAY_C)
    if len(glyphs) != 95:
        sys.exit(f"Expected 95 glyphs, got {len(glyphs)}")

    print("/* font12x12 — nearest-neighbor upscale of font8x8 to 12×12.")
    print(" * Each row is a uint16_t; bit N = column N (0 = leftmost). */")
    print("static const uint16_t font12x12[95][12] = {")
    for i, g in enumerate(glyphs):
        code = 0x20 + i
        label = chr(code) if code >= 0x21 else ' '
        up = upscale(g)
        vals = ', '.join(f'0x{v:04X}' for v in up)
        print(f"    {{ {vals} }}, /* 0x{code:02X} {label} */")
    print("};")

if __name__ == "__main__":
    main()
