#!/usr/bin/env python3
"""
Generate a VLW smooth font for TFT_eSPI from a TTF/TTC source.

Includes ASCII printable + CJK Symbols/Punctuation + BIG5 level-1 common
Traditional Chinese chars (~5400 glyphs). Output is binary VLW format
loadable via TFT_eSPI's loadFont() from LittleFS.

Usage:
    python make_vlw_font.py [output.vlw] [font.ttf|ttc] [size]

Defaults:
    output  = data/ZhTW12.vlw
    font    = C:/Windows/Fonts/NotoSansTC-VF.ttf  (fallback msjh.ttc)
    size    = 12
"""
import os
import struct
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont


def collect_chars():
    """ASCII printable + CJK punctuation + BIG5 level-1 common chars."""
    chars = []
    seen = set()

    # ASCII printable
    for cp in range(0x20, 0x7F):
        ch = chr(cp)
        if ch not in seen:
            seen.add(ch)
            chars.append(ch)

    # Latin-1 supplement (some european chars sometimes appear in transcripts)
    for cp in range(0xA0, 0x100):
        ch = chr(cp)
        if ch not in seen:
            seen.add(ch)
            chars.append(ch)

    # CJK Symbols and Punctuation, full-width forms
    extras = [0x3000, 0x3001, 0x3002, 0x3003, 0x3004, 0x3005, 0x3006, 0x3007,
              0x3008, 0x3009, 0x300A, 0x300B, 0x300C, 0x300D, 0x300E, 0x300F,
              0x3010, 0x3011, 0x3012, 0x3013, 0x3014, 0x3015, 0x3016, 0x3017,
              0x3018, 0x3019, 0x301A, 0x301B, 0x301C, 0x301D, 0x301E,
              0xFF01, 0xFF02, 0xFF03, 0xFF04, 0xFF05, 0xFF06, 0xFF07, 0xFF08,
              0xFF09, 0xFF0A, 0xFF0B, 0xFF0C, 0xFF0D, 0xFF0E, 0xFF0F, 0xFF1A,
              0xFF1B, 0xFF1C, 0xFF1D, 0xFF1E, 0xFF1F, 0xFF20, 0xFF3B, 0xFF3D,
              0xFF5B, 0xFF5D]
    for cp in extras:
        ch = chr(cp)
        if ch not in seen:
            seen.add(ch)
            chars.append(ch)

    # BIG5 level-1 common (lead bytes A4-C6) — about 5401 chars
    for lead in range(0xA4, 0xC7):
        for trail in list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)):
            try:
                ch = bytes([lead, trail]).decode('big5')
            except UnicodeDecodeError:
                continue
            if ch not in seen:
                seen.add(ch)
                chars.append(ch)

    return chars


def render_glyph(font, ch, ascent):
    """Returns dict of glyph metrics + bitmap, or None if char has no glyph."""
    bbox = font.getbbox(ch)
    if bbox is None:
        return None
    x0, y0, x1, y1 = bbox
    w = x1 - x0
    h = y1 - y0

    try:
        xAdv = round(font.getlength(ch))
    except Exception:
        xAdv = w

    if w <= 0 or h <= 0:
        # Whitespace-style char (no bitmap, just advance)
        return {
            'unicode': ord(ch),
            'width': 0,
            'height': 0,
            'xAdvance': max(xAdv, 1),
            'dY': 0,
            'dX': 0,
            'bitmap': b'',
        }

    # Render anti-aliased grayscale (0=transparent, 255=opaque)
    img = Image.new('L', (w, h), 0)
    draw = ImageDraw.Draw(img)
    draw.text((-x0, -y0), ch, fill=255, font=font)
    return {
        'unicode': ord(ch),
        'width': w,
        'height': h,
        'xAdvance': xAdv,
        'dY': ascent - y0,
        'dX': x0,
        'bitmap': img.tobytes(),
    }


def write_vlw(out_path, font_path, font_size, font_index=0):
    if not Path(font_path).exists():
        raise FileNotFoundError(f"Source font not found: {font_path}")

    font = ImageFont.truetype(font_path, font_size, index=font_index)
    ascent, descent = font.getmetrics()
    yAdvance = ascent + descent

    chars = collect_chars()
    print(f"Source font   : {font_path}")
    print(f"Render size   : {font_size}px")
    print(f"Ascent/Desc   : {ascent}/{descent}, yAdvance={yAdvance}")
    print(f"Char count    : {len(chars)}")

    glyphs = []
    skipped = 0
    for ch in chars:
        g = render_glyph(font, ch, ascent)
        if g is None:
            skipped += 1
            continue
        glyphs.append(g)

    print(f"Glyphs encoded: {len(glyphs)} (skipped {skipped})")

    Path(out_path).parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, 'wb') as f:
        # Header (24 bytes, big-endian uint32). TFT_eSPI reads SIX fields:
        # gCount, version (discarded), yAdvance (discarded), reserved,
        # ascent, descent. Don't drop ascent/descent — TFT_eSPI uses them
        # to compute baseline placement.
        f.write(struct.pack('>I', len(glyphs)))   # gCount
        f.write(struct.pack('>I', 11))             # version (discarded)
        f.write(struct.pack('>I', font_size))      # yAdvance (discarded)
        f.write(struct.pack('>I', 0))              # reserved
        f.write(struct.pack('>I', ascent))         # ascent — top of 'd'
        f.write(struct.pack('>I', descent))        # descent — bottom of 'p'

        # Per-glyph metadata (28 bytes each)
        for g in glyphs:
            f.write(struct.pack('>I', g['unicode']))
            f.write(struct.pack('>I', g['height']))
            f.write(struct.pack('>I', g['width']))
            f.write(struct.pack('>I', g['xAdvance']))
            f.write(struct.pack('>i', g['dY']))    # signed
            f.write(struct.pack('>i', g['dX']))    # signed
            f.write(struct.pack('>I', 0))          # reserved

        # Concatenated bitmaps
        for g in glyphs:
            f.write(g['bitmap'])

    size_kb = Path(out_path).stat().st_size / 1024
    print(f"Output        : {out_path} ({size_kb:.1f} KB)")


if __name__ == '__main__':
    out_path = sys.argv[1] if len(sys.argv) > 1 else 'data/ZhTW12.vlw'

    # Prefer Noto Sans TC; fall back to Microsoft JhengHei
    candidates = [
        'C:/Windows/Fonts/NotoSansTC-VF.ttf',
        'C:/Windows/Fonts/msjh.ttc',
        'C:/Windows/Fonts/mingliu.ttc',
    ]
    font_path = sys.argv[2] if len(sys.argv) > 2 else None
    if font_path is None:
        for p in candidates:
            if Path(p).exists():
                font_path = p
                break
        if font_path is None:
            print("ERROR: No suitable Chinese TTF found. Install Noto Sans TC.")
            sys.exit(1)

    size = int(sys.argv[3]) if len(sys.argv) > 3 else 12

    write_vlw(out_path, font_path, font_size=size)
