#!/usr/bin/env python3
"""Convert image assets (PNG/JPG/SVG) into C header arrays for NEO-OS kernel.

Usage: python3 tools/convert_assets.py [assets_dir] [output_dir]
Default: assets -> kernel/generated
"""
import os
import sys
import io
import tempfile
import subprocess
import math

ASSET_DIR = sys.argv[1] if len(sys.argv) > 1 else "assets"
OUT_DIR = sys.argv[2] if len(sys.argv) > 2 else "kernel/generated"


def rgba_to_argb32(r, g, b, a=255):
    return (a << 24) | (r << 16) | (g << 8) | b


def convert_png_jpg(path):
    try:
        from PIL import Image
    except ImportError:
        print("Error: Pillow not installed. Run: pip install Pillow")
        sys.exit(1)
    im = Image.open(path).convert("RGBA")
    w, h = im.size
    pixels = []
    for y in range(h):
        for x in range(w):
            r, g, b, a = im.getpixel((x, y))
            pixels.append(rgba_to_argb32(r, g, b, a))
    return w, h, pixels


def convert_svg(path):
    """Try external SVG rasterizers, fallback to error."""
    try:
        import cairosvg
        png_data = cairosvg.svg2png(url=path)
        from PIL import Image
        im = Image.open(io.BytesIO(png_data)).convert("RGBA")
        w, h = im.size
        pixels = []
        for y in range(h):
            for x in range(w):
                r, g, b, a = im.getpixel((x, y))
                pixels.append(rgba_to_argb32(r, g, b, a))
        return w, h, pixels
    except Exception:
        pass

    try:
        with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tmp:
            tmp_path = tmp.name
        subprocess.run(["rsvg-convert", path, "-o", tmp_path], check=True)
        from PIL import Image
        im = Image.open(tmp_path).convert("RGBA")
        w, h = im.size
        pixels = []
        for y in range(h):
            for x in range(w):
                r, g, b, a = im.getpixel((x, y))
                pixels.append(rgba_to_argb32(r, g, b, a))
        os.unlink(tmp_path)
        return w, h, pixels
    except Exception:
        pass

    print(f"Error: cannot convert SVG {path}. Install cairosvg or rsvg-convert.")
    sys.exit(1)


def write_header(name, w, h, pixels, out_path):
    safe = name.replace(".", "_").replace("-", "_")
    with open(out_path, "w") as f:
        f.write(f"#ifndef ASSET_{safe.upper()}\n")
        f.write(f"#define ASSET_{safe.upper()}\n\n")
        f.write(f"#define ASSET_{safe.upper()}_W {w}\n")
        f.write(f"#define ASSET_{safe.upper()}_H {h}\n")
        f.write(f"static const unsigned int asset_{safe}[] = {{\n")
        for i in range(0, len(pixels), 8):
            line = ", ".join(f"0x{p:08X}" for p in pixels[i:i+8])
            f.write("    " + line + ",\n")
        f.write("};\n\n")
        f.write(f"#endif\n")


def svg_wrap(size, body):
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {size} {size}" width="{size}" height="{size}">\n'
        f"{body}\n"
        "</svg>\n"
    )


def app_icon(size, stops, content, gid='bg'):
    rx = 7 if size == 32 else 3
    defs = (
        f'<defs>'
        f'<linearGradient id="{gid}" x1="0" y1="0" x2="0" y2="1">'
        f'<stop offset="0" stop-color="{stops[0]}"/>'
        f'<stop offset="1" stop-color="{stops[1]}"/>'
        f'</linearGradient>'
        f'</defs>'
    )
    bg = f'<rect x="1" y="1" width="{size-2}" height="{size-2}" rx="{rx}" fill="url(#{gid})"/>'
    return svg_wrap(size, defs + bg + content)


# ---------- app icons ----------

def svg_terminal(size):
    if size == 32:
        content = (
            '<polygon points="10,10 18,16 10,22" fill="#ECEFF1"/>'
            '<rect x="13" y="24" width="9" height="2" rx="0.5" fill="#ECEFF1"/>'
        )
    else:
        content = (
            '<polygon points="5,5 9,8 5,11" fill="#ECEFF1"/>'
            '<rect x="6" y="12" width="5" height="1.5" rx="0.5" fill="#ECEFF1"/>'
        )
    return app_icon(size, ["#37474F", "#102027"], content)


def svg_demo(size):
    if size == 32:
        content = (
            '<rect x="7" y="7" width="18" height="18" rx="2" fill="#FFFFFF"/>'
            '<rect x="10" y="14" width="12" height="2" rx="0.5" fill="#FFB74D"/>'
            '<rect x="10" y="19" width="8" height="2" rx="0.5" fill="#FFB74D"/>'
        )
    else:
        content = (
            '<rect x="4" y="4" width="8" height="8" rx="1" fill="#FFFFFF"/>'
            '<rect x="6" y="7" width="4" height="1" rx="0.5" fill="#FFB74D"/>'
            '<rect x="6" y="10" width="3" height="1" rx="0.5" fill="#FFB74D"/>'
        )
    return app_icon(size, ["#FFCA28", "#F57F17"], content)


def svg_about(size):
    if size == 32:
        content = (
            '<circle cx="16" cy="16" r="10" fill="url(#ring)" opacity="0.25"/>'
            '<rect x="15" y="9" width="2" height="10" rx="1" fill="#FFFFFF"/>'
            '<circle cx="16" cy="6" r="2" fill="#FFFFFF"/>'
        )
        extra = (
            '<defs>'
            '<linearGradient id="ring" x1="0" y1="0" x2="0" y2="1">'
            '<stop offset="0" stop-color="#FFFFFF"/>'
            '<stop offset="1" stop-color="#FFFFFF" stop-opacity="0"/>'
            '</linearGradient>'
            '</defs>'
        )
        return app_icon(size, ["#4FC3F7", "#0288D1"], extra + content)
    else:
        content = (
            '<rect x="7" y="4" width="2" height="6" rx="1" fill="#FFFFFF"/>'
            '<circle cx="8" cy="2.5" r="1.5" fill="#FFFFFF"/>'
        )
        return app_icon(size, ["#4FC3F7", "#0288D1"], content)


def svg_calc(size):
    if size == 32:
        content = (
            '<rect x="7" y="6" width="18" height="20" rx="2" fill="#FFFFFF"/>'
            '<rect x="9" y="8" width="14" height="5" rx="1" fill="#263238"/>'
            '<rect x="9" y="16" width="5" height="4" rx="1" fill="#37474F"/>'
            '<rect x="16" y="16" width="5" height="4" rx="1" fill="#37474F"/>'
            '<rect x="9" y="22" width="5" height="4" rx="1" fill="#37474F"/>'
            '<rect x="16" y="22" width="5" height="4" rx="1" fill="#37474F"/>'
        )
    else:
        content = (
            '<rect x="4" y="3" width="8" height="11" rx="1" fill="#FFFFFF"/>'
            '<rect x="5" y="4" width="6" height="2" rx="0.5" fill="#263238"/>'
            '<rect x="5" y="7" width="2" height="2" rx="0.5" fill="#37474F"/>'
            '<rect x="8" y="7" width="2" height="2" rx="0.5" fill="#37474F"/>'
            '<rect x="5" y="10" width="2" height="2" rx="0.5" fill="#37474F"/>'
            '<rect x="8" y="10" width="2" height="2" rx="0.5" fill="#37474F"/>'
        )
    return app_icon(size, ["#B0BEC5", "#546E7A"], content)


def svg_user(size):
    if size == 32:
        content = (
            '<circle cx="16" cy="11" r="5.5" fill="#FFFFFF"/>'
            '<path d="M6 30 Q6 19 16 19 Q26 19 26 30 Z" fill="#FFFFFF"/>'
        )
    else:
        content = (
            '<circle cx="8" cy="5" r="3" fill="#FFFFFF"/>'
            '<path d="M3 13 Q3 8 8 8 Q13 8 13 13 Z" fill="#FFFFFF"/>'
        )
    return app_icon(size, ["#B0BEC5", "#607D8B"], content)


def svg_power():
    bg = (
        '<defs>'
        '<linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">'
        '<stop offset="0" stop-color="#EF5350"/>'
        '<stop offset="1" stop-color="#C62828"/>'
        '</linearGradient>'
        '</defs>'
        '<circle cx="8" cy="8" r="7" fill="url(#bg)"/>'
    )
    sym = (
        '<path d="M8 3.5 L8 8" stroke="#FFFFFF" stroke-width="1.5" stroke-linecap="round"/>'
        '<path d="M3.5 8 A4.5 4.5 0 1 0 12.5 8" stroke="#FFFFFF" stroke-width="1.5" fill="none" stroke-linecap="round"/>'
    )
    return svg_wrap(16, bg + sym)


def svg_folder():
    defs = (
        '<defs>'
        '<linearGradient id="f" x1="0" y1="0" x2="0" y2="1">'
        '<stop offset="0" stop-color="#FFECB3"/>'
        '<stop offset="1" stop-color="#FFCA28"/>'
        '</linearGradient>'
        '</defs>'
    )
    body = (
        '<path d="M0,4 L5,4 L7,6 L16,6 L16,16 L0,16 Z" fill="url(#f)"/>'
        '<path d="M0,4 L5,4 L7,6 L16,6 L16,9 L0,9 Z" fill="#FFE082"/>'
    )
    return svg_wrap(16, defs + body)


def svg_settings():
    bg = (
        '<defs>'
        '<linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">'
        '<stop offset="0" stop-color="#CFD8DC"/>'
        '<stop offset="1" stop-color="#78909A"/>'
        '</linearGradient>'
        '</defs>'
        '<circle cx="8" cy="8" r="7" fill="url(#bg)"/>'
    )
    # simple 8-tooth gear path
    outer, inner = 5.5, 3.2
    pts = []
    for i in range(16):
        ang = i * math.pi / 8
        r = outer if i % 2 == 0 else inner
        pts.append(f"{8 + r * math.cos(ang - math.pi/2):.2f},{8 + r * math.sin(ang - math.pi/2):.2f}")
    gear = f'<polygon points="{" ".join(pts)}" fill="#FFFFFF"/>'
    gear += '<circle cx="8" cy="8" r="1.8" fill="url(#bg)"/>'
    return svg_wrap(16, bg + gear)


# ---------- tile / menu icons (transparent background) ----------

def svg_weather():
    sun = (
        '<defs>'
        '<linearGradient id="sun" x1="0" y1="0" x2="0" y2="1">'
        '<stop offset="0" stop-color="#FFF176"/>'
        '<stop offset="1" stop-color="#FFD54F"/>'
        '</linearGradient>'
        '</defs>'
        '<circle cx="23" cy="10" r="7" fill="url(#sun)"/>'
    )
    cloud = (
        '<defs>'
        '<linearGradient id="cloud" x1="0" y1="0" x2="0" y2="1">'
        '<stop offset="0" stop-color="#FFFFFF"/>'
        '<stop offset="1" stop-color="#E3F2FD"/>'
        '</linearGradient>'
        '</defs>'
        '<rect x="5" y="20" width="24" height="8" rx="2" fill="url(#cloud)"/>'
        '<circle cx="12" cy="20" r="7" fill="url(#cloud)"/>'
        '<circle cx="20" cy="20" r="8" fill="url(#cloud)"/>'
        '<circle cx="27" cy="20" r="6" fill="url(#cloud)"/>'
    )
    return svg_wrap(32, sun + cloud)


def svg_news():
    paper = (
        '<rect x="3" y="3" width="26" height="26" rx="2" fill="#FFFFFF" stroke="#CFD8DC" stroke-width="1"/>'
        '<rect x="6" y="6" width="15" height="6" rx="1" fill="#EF5350"/>'
        '<rect x="6" y="15" width="18" height="2" rx="0.5" fill="#90A4AE"/>'
        '<rect x="6" y="19" width="18" height="2" rx="0.5" fill="#90A4AE"/>'
        '<rect x="6" y="23" width="12" height="2" rx="0.5" fill="#90A4AE"/>'
    )
    return svg_wrap(32, paper)


def svg_mail():
    defs = (
        '<defs>'
        '<linearGradient id="m" x1="0" y1="0" x2="0" y2="1">'
        '<stop offset="0" stop-color="#4FC3F7"/>'
        '<stop offset="1" stop-color="#0277BD"/>'
        '</linearGradient>'
        '</defs>'
    )
    env = (
        '<rect x="3" y="6" width="26" height="20" rx="2" fill="url(#m)"/>'
        '<rect x="6" y="9" width="20" height="14" rx="1" fill="#FFFFFF"/>'
        '<path d="M6 9 L16 18 L26 9" stroke="url(#m)" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round"/>'
    )
    return svg_wrap(32, defs + env)


def svg_photos():
    defs = (
        '<defs>'
        '<linearGradient id="p" x1="0" y1="0" x2="0" y2="1">'
        '<stop offset="0" stop-color="#AED581"/>'
        '<stop offset="1" stop-color="#558B2F"/>'
        '</linearGradient>'
        '</defs>'
    )
    scene = (
        '<rect x="3" y="3" width="26" height="26" rx="2" fill="url(#p)"/>'
        '<circle cx="24" cy="10" r="4" fill="#FFFFFF"/>'
        '<path d="M4 28 L12 17 L20 24 L28 13 L28 28 Z" fill="#FFFFFF" opacity="0.9"/>'
    )
    return svg_wrap(32, defs + scene)


def svg_refresh():
    sym = (
        '<path d="M3.5 8 A4.5 4.5 0 1 0 12.5 8" stroke="#2196F3" stroke-width="2" fill="none" stroke-linecap="round"/>'
        '<polygon points="12.5,5.5 15.5,8 12.5,10.5" fill="#2196F3"/>'
    )
    return svg_wrap(16, sym)


def svg_toolbar():
    bars = (
        '<rect x="1" y="3" width="14" height="2" rx="1" fill="#212121"/>'
        '<rect x="1" y="7" width="14" height="2" rx="1" fill="#212121"/>'
        '<rect x="1" y="11" width="14" height="2" rx="1" fill="#212121"/>'
    )
    return svg_wrap(16, bars)


def svg_cascade():
    win = (
        '<rect x="1" y="1" width="10" height="8" rx="1" fill="#212121"/>'
        '<rect x="5" y="5" width="10" height="8" rx="1" fill="#FFFFFF" stroke="#212121" stroke-width="1"/>'
        '<rect x="6" y="6" width="8" height="1.5" rx="0.5" fill="#212121"/>'
    )
    return svg_wrap(16, win)


def svg_showdesktop():
    desk = (
        '<rect x="1" y="1" width="14" height="10" rx="1" fill="#212121"/>'
        '<rect x="1" y="13" width="14" height="2" rx="1" fill="#212121"/>'
    )
    return svg_wrap(16, desk)


def svg_browser(size):
    if size == 32:
        content = (
            '<rect x="4" y="7" width="24" height="18" rx="2" fill="#FFFFFF" stroke="#CFD8DC" stroke-width="1"/>'
            '<rect x="4" y="7" width="24" height="5" rx="2" fill="#2196F3"/>'
            '<circle cx="7" cy="9.5" r="1" fill="#FFFFFF"/>'
            '<circle cx="11" cy="9.5" r="1" fill="#FFFFFF"/>'
            '<circle cx="15" cy="9.5" r="1" fill="#FFFFFF"/>'
            '<rect x="7" y="16" width="18" height="2" rx="0.5" fill="#90A4AE"/>'
            '<rect x="7" y="20" width="14" height="2" rx="0.5" fill="#90A4AE"/>'
        )
    else:
        content = (
            '<rect x="2" y="3" width="12" height="10" rx="1" fill="#FFFFFF" stroke="#CFD8DC" stroke-width="0.5"/>'
            '<rect x="2" y="3" width="12" height="3" rx="1" fill="#2196F3"/>'
            '<circle cx="4" cy="4.5" r="0.6" fill="#FFFFFF"/>'
            '<circle cx="7" cy="4.5" r="0.6" fill="#FFFFFF"/>'
            '<rect x="4" y="8" width="8" height="1.2" rx="0.3" fill="#90A4AE"/>'
            '<rect x="4" y="10" width="6" height="1.2" rx="0.3" fill="#90A4AE"/>'
        )
    return app_icon(size, ["#64B5F6", "#1976D2"], content, gid='browserbg')


SVG_GENERATORS = {
    "terminal_32.svg": svg_terminal(32),
    "terminal_16.svg": svg_terminal(16),
    "demo_32.svg": svg_demo(32),
    "demo_16.svg": svg_demo(16),
    "about_32.svg": svg_about(32),
    "about_16.svg": svg_about(16),
    "calc_32.svg": svg_calc(32),
    "calc_16.svg": svg_calc(16),
    "user_32.svg": svg_user(32),
    "user_16.svg": svg_user(16),
    "power_16.svg": svg_power(),
    "folder_16.svg": svg_folder(),
    "settings_16.svg": svg_settings(),
    "weather_32.svg": svg_weather(),
    "news_32.svg": svg_news(),
    "mail_32.svg": svg_mail(),
    "photos_32.svg": svg_photos(),
    "refresh_16.svg": svg_refresh(),
    "toolbar_16.svg": svg_toolbar(),
    "cascade_16.svg": svg_cascade(),
    "showdesktop_16.svg": svg_showdesktop(),
    "browser_32.svg": svg_browser(32),
    "browser_16.svg": svg_browser(16),
}


def ensure_assets():
    os.makedirs(ASSET_DIR, exist_ok=True)
    os.makedirs(OUT_DIR, exist_ok=True)
    generated = []
    for name, svg in SVG_GENERATORS.items():
        path = os.path.join(ASSET_DIR, name)
        if not os.path.exists(path):
            with open(path, "w") as f:
                f.write(svg)
        generated.append(path)
    return generated


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    ensure_assets()

    for fname in os.listdir(ASSET_DIR):
        path = os.path.join(ASSET_DIR, fname)
        if not os.path.isfile(path):
            continue
        ext = os.path.splitext(fname)[1].lower()
        if ext not in (".png", ".jpg", ".jpeg", ".svg"):
            continue
        print(f"Converting {fname}...")
        if ext == ".svg":
            w, h, pixels = convert_svg(path)
        else:
            w, h, pixels = convert_png_jpg(path)
        base = os.path.splitext(fname)[0]
        out_path = os.path.join(OUT_DIR, f"asset_{base}.h")
        write_header(base, w, h, pixels, out_path)
    print("Done.")


if __name__ == "__main__":
    main()
