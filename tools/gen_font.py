#!/usr/bin/env python3
import sys
from PIL import Image, ImageDraw, ImageFont

FONT_PATH = r"/mnt/c/Windows/Fonts/NotoSansSC-VF.ttf"
FONT_SIZE = 24
CELL_W = 24
CELL_H = 24
FONT_PITCH = 3
OUT_PATH = r"/mnt/f/NEO-OS/kernel/font_noto.h"

GB2312_TXT = r"/mnt/f/NEO-OS/tools/gb2312_chars.txt"

EXTRA_PUNCT = [
    '\u00B7',  # ·
    '\u2014',  # —
    '\u2018',  # '
    '\u2019',  # '
    '\u201C',  # "
    '\u201D',  # "
    '\u2026',  # …
    '\u2600',  # ☀
    '\u2601',  # ☁
    '\u2602',  # ☂
    '\u2603',  # ☃
    '\u2605',  # ★
    '\u2606',  # ☆
    '\u263A',  # ☺
    '\u263B',  # ☻
    '\u263C',  # ☼
    '\u263D',  # ☽
    '\u263E',  # ☾
    '\u2660',  # ♠
    '\u2661',  # ♡
    '\u2662',  # ♢
    '\u2663',  # ♣
    '\u2664',  # ♤
    '\u2665',  # ♥
    '\u2666',  # ♦
    '\u2667',  # ♧
    '\u266A',  # ♪
    '\u266B',  # ♫
    '\u266C',  # ♬
    '\u3001',  # 、
    '\u3002',  # 。
    '\u300A',  # 《
    '\u300B',  # 》
    '\u300C',  # 「
    '\u300D',  # 」
    '\u300E',  # 『
    '\u300F',  # 』
    '\uFF01',  # ！
    '\uFF08',  # （
    '\uFF09',  # ）
    '\uFF0C',  # ，
    '\uFF1A',  # ：
    '\uFF1B',  # ；
    '\uFF1F',  # ？
]

def load_gb2312_chars():
    chars = []
    try:
        with open(GB2312_TXT, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                for ch in line:
                    if '\u4e00' <= ch <= '\u9fff':
                        if ch not in chars:
                            chars.append(ch)
    except FileNotFoundError:
        pass
    for ch in EXTRA_PUNCT:
        if ch not in chars:
            chars.append(ch)
    return chars

_FONT_CACHE = None

def render_glyph(ch):
    global _FONT_CACHE
    img = Image.new("L", (CELL_W, CELL_H), 0)
    draw = ImageDraw.Draw(img)
    if _FONT_CACHE is None:
        try:
            _FONT_CACHE = ImageFont.truetype(FONT_PATH, FONT_SIZE)
        except Exception as e:
            print(f"load font failed: {e}")
            sys.exit(1)
    font = _FONT_CACHE

    bbox = draw.textbbox((0, 0), ch, font=font)
    w = bbox[2] - bbox[0]
    h = bbox[3] - bbox[1]
    x = (CELL_W - w) // 2 - bbox[0]
    y = (CELL_H - h) // 2 - bbox[1]
    draw.text((x, y), ch, font=font, fill=255)

    data = []
    for row in range(CELL_H):
        row_bytes = [0] * FONT_PITCH
        for col in range(CELL_W):
            px = img.getpixel((col, row))
            if px > 128:
                row_bytes[col // 8] |= 1 << (7 - (col % 8))
        data.extend(row_bytes)
    return data

def utf8_encode(cp):
    if cp < 0x80:
        return [cp]
    elif cp < 0x800:
        return [0xC0 | (cp >> 6), 0x80 | (cp & 0x3F)]
    elif cp < 0x10000:
        return [0xE0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F)]
    else:
        return [0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3F),
                0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F)]

def main():
    cn_chars = load_gb2312_chars()
    cn_chars.sort(key=lambda c: ord(c))
    print(f"loaded {len(cn_chars)} GB2312 chars")

    lines = [
        "#ifndef FONT_NOTO_H",
        "#define FONT_NOTO_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define FONT_WIDTH  {CELL_W}",
        f"#define FONT_HEIGHT {CELL_H}",
        f"#define FONT_PITCH  {FONT_PITCH}",
        "",
        f"#define FONT_CN_COUNT {len(cn_chars)}",
        "",
        f"static const uint8_t font_noto[128][FONT_HEIGHT * FONT_PITCH] = {{",
    ]

    for code in range(128):
        ch = chr(code) if 32 <= code <= 126 else ' '
        data = render_glyph(ch) if 32 <= code <= 126 else [0] * (CELL_H * FONT_PITCH)
        vals = ",".join(f"0x{b:02X}" for b in data)
        if 32 <= code <= 126:
            lines.append(f"    /* '{ch}' */ {{{vals}}},")
        else:
            lines.append(f"    /* 0x{code:02X} */ {{{vals}}},")

    lines.append("};")
    lines.append("")

    if cn_chars:
        lines.append(f"static const uint16_t font_cn_codes[FONT_CN_COUNT] = {{")
        for i in range(0, len(cn_chars), 16):
            chunk = cn_chars[i:i+16]
            vals = ",".join(f"0x{ord(c):04X}" for c in chunk)
            lines.append(f"    {vals},")
        lines.append("};")
        lines.append("")

        lines.append(f"static const uint8_t font_cn[FONT_CN_COUNT][FONT_HEIGHT * FONT_PITCH] = {{")
        for i, ch in enumerate(cn_chars):
            data = render_glyph(ch)
            vals = ",".join(f"0x{b:02X}" for b in data)
            if i % 100 == 0:
                print(f"rendering {i}/{len(cn_chars)}")
            lines.append(f"    /* U+{ord(ch):04X} '{ch}' */ {{{vals}}},")
        lines.append("};")
        lines.append("")

    lines.append("#endif")

    with open(OUT_PATH, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    print(f"generated {OUT_PATH}")

if __name__ == "__main__":
    main()
