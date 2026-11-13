//! 2D framebuffer primitives for the M10 desktop: clipped rect fills,
//! opaque 8x16 text, and a save-under mouse cursor. Same no-lock
//! discipline as console (single CPU + cli/sti pairing).

use neo_common::{BootInfo, PixelFormat};

use crate::font::GLYPHS;

struct Mode {
    base: u64,
    stride: u32, // pixels per scanline
    w: u32,
    h: u32,
    rgb: bool,
}

static mut MODE: Option<Mode> = None;

pub fn init(bi: &BootInfo) {
    unsafe {
        MODE = Some(Mode {
            base: bi.fb_base,
            stride: bi.stride,
            w: bi.width,
            h: bi.height,
            rgb: bi.pixel_format == PixelFormat::Rgb,
        });
    }
}

fn m() -> &'static Mode {
    unsafe { (*core::ptr::addr_of!(MODE)).as_ref().expect("gfx::init not called") }
}

pub fn size() -> (u32, u32) {
    let m = m();
    (m.w, m.h)
}

pub fn color(r: u8, g: u8, b: u8) -> u32 {
    if m().rgb {
        (r as u32) | (g as u32) << 8 | (b as u32) << 16
    } else {
        (b as u32) | (g as u32) << 8 | (r as u32) << 16
    }
}

fn px_put(x: u32, y: u32, c: u32) {
    let m = m();
    unsafe {
        ((m.base + ((y * m.stride + x) * 4) as u64) as *mut u32).write_volatile(c);
    }
}

fn px_get(x: u32, y: u32) -> u32 {
    let m = m();
    unsafe {
        ((m.base + ((y * m.stride + x) * 4) as u64) as *const u32).read_volatile()
    }
}

/// Clipped solid rectangle; 64-bit batches like console::clear.
pub fn fill_rect(x: u32, y: u32, w: u32, h: u32, c: u32) {
    let m = m();
    let x0 = x.min(m.w);
    let y0 = y.min(m.h);
    let x1 = x.saturating_add(w).min(m.w);
    let y1 = y.saturating_add(h).min(m.h);
    if x1 <= x0 || y1 <= y0 {
        return;
    }
    let c64: u64 = c as u64 | ((c as u64) << 32);
    for yy in y0..y1 {
        let row = m.base + (yy * m.stride) as u64 * 4;
        let mut xx = x0;
        if xx & 1 == 1 && xx < x1 {
            // Odd leading pixel to reach 8-alignment.
            unsafe { ((row + xx as u64 * 4) as *mut u32).write_volatile(c) };
            xx += 1;
        }
        let mut p = row + xx as u64 * 4;
        let end = row + x1 as u64 * 4;
        while p + 8 <= end {
            unsafe { (p as *mut u64).write_volatile(c64) };
            p += 8;
        }
        if p < end {
            unsafe { (p as *mut u32).write_volatile(c) };
        }
    }
}

/// Opaque 8x16 text (one cell per byte; `\n` starts a new line).
pub fn put_text(mut x: u32, y: u32, s: &str, fg: u32, bg: u32) {
    let start_x = x;
    let m = m();
    for b in s.bytes() {
        if b == b'\n' {
            x = start_x;
            continue;
        }
        if x + 8 > m.w {
            break;
        }
        let g = GLYPHS[b as usize];
        for r in 0..16u32 {
            if y + r >= m.h {
                break;
            }
            let bits = g[r as usize];
            for bit in 0..8u32 {
                let on = bits & (0x80 >> bit) != 0;
                px_put(x + bit, y + r, if on { fg } else { bg });
            }
        }
        x += 8;
    }
}

// ---- mouse cursor: 12x14 arrow, save-under restore ----

const CUR_W: u32 = 12;
const CUR_H: u32 = 14;
/// MSB = left pixel, 1 = cursor color.
const CUR_BITS: [u16; 14] = [
    0b110000000000,
    0b111000000000,
    0b101100000000,
    0b100110000000,
    0b100011000000,
    0b100001100000,
    0b100000110000,
    0b100000011000,
    0b100000001100,
    0b100000000110,
    0b100001100000,
    0b100100110000,
    0b101100011000,
    0b110000001100,
];

static mut UNDER: [u32; (12 * 16) as usize] = [0; (12 * 16) as usize];
static mut SAVED: (u32, u32, u32, u32) = (0, 0, 0, 0); // x, y, w, h

/// Save the pixels under the cursor rect (clipped) and draw the arrow.
pub fn cursor_draw(x: u32, y: u32, c: u32) {
    let m = m();
    let w = CUR_W.min(m.w.saturating_sub(x));
    let h = CUR_H.min(m.h.saturating_sub(y));
    unsafe { SAVED = (x, y, w, h) };
    for r in 0..h {
        for col in 0..w {
            unsafe { UNDER[(r * CUR_W + col) as usize] = px_get(x + col, y + r) };
            if CUR_BITS[r as usize] & (0x800 >> col) != 0 {
                px_put(x + col, y + r, c);
            }
        }
    }
}

/// Restore exactly the rect saved by the last cursor_draw.
pub fn cursor_erase() {
    let (x, y, w, h) = unsafe { SAVED };
    for r in 0..h {
        for col in 0..w {
            px_put(x + col, y + r, unsafe {
                UNDER[(r * CUR_W + col) as usize]
            });
        }
    }
}
