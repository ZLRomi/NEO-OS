//! Framebuffer text console: 8x16 VGA font, hardware scrolling, IF-safe.
//! Same no-lock discipline as serial (single CPU + cli/sti pairing), so
//! IRQ handlers can print without deadlocking.

use core::fmt::{self, Write};
use neo_common::{BootInfo, PixelFormat};

use crate::cpu::{cli, interrupts_enabled, sti};
use crate::font::GLYPHS;

const CW: u32 = 8;
const CH: u32 = 16;

struct Console {
    base: u64,
    stride: u32,
    cols: u32,
    rows: u32,
    col: u32,
    row: u32,
    fg: u32,
    bg: u32,
}

static mut CON: Option<Console> = None;

fn pack(rgb: bool, r: u8, g: u8, b: u8) -> u32 {
    if rgb {
        // Rgb (PixelRedGreenBlueReserved): byte0=R, byte1=G, byte2=B
        (r as u32) | (g as u32) << 8 | (b as u32) << 16
    } else {
        // Bgr (PixelBlueGreenRedReserved): byte0=B, byte1=G, byte2=R
        (b as u32) | (g as u32) << 8 | (r as u32) << 16
    }
}

fn px(base: u64, stride: u32, x: u32, y: u32, color: u32) {
    unsafe {
        ((base + ((y * stride + x) * 4) as u64) as *mut u32)
            .write_volatile(color);
    }
}

#[allow(dead_code)]
fn px_read(base: u64, stride: u32, x: u32, y: u32) -> u32 {
    unsafe {
        ((base + ((y * stride + x) * 4) as u64) as *const u32).read_volatile()
    }
}

/// Initialize from BootInfo and clear the screen.
pub fn init(bi: &BootInfo) {
    let rgb = bi.pixel_format == PixelFormat::Rgb;
    let c = Console {
        base: bi.fb_base,
        stride: bi.stride,
        cols: bi.width / CW,
        rows: bi.height / CH,
        col: 0,
        row: 0,
        fg: pack(rgb, 0xE8, 0xE8, 0xE8),
        bg: pack(rgb, 0x08, 0x18, 0x38),
    };
    unsafe { CON = Some(c) };
    clear();
}

fn con() -> Option<&'static mut Console> {
    unsafe { CON.as_mut() }
}

/// Fill the whole screen with the background color (64-bit batches).
pub fn clear() {
    if let Some(c) = con() {
        let bg64: u64 = c.bg as u64 | ((c.bg as u64) << 32);
        let rows = c.rows * CH;
        let stride = c.stride as u64;
        let base = c.base;
        let pairs = stride / 2;
        let odd = stride % 2 != 0;
        for y in 0..rows as u64 {
            let row_base = base + y * stride * 4;
            for i in 0..pairs {
                unsafe {
                    ((row_base + i * 8) as *mut u64).write_volatile(bg64);
                }
            }
            if odd {
                unsafe {
                    ((row_base + pairs * 8) as *mut u32).write_volatile(c.bg);
                }
            }
        }
        if let Some(c) = con() {
            c.col = 0;
            c.row = 0;
            draw_cursor(c);
        }
    }
}

/// Draw a solid frame (used by the panic handler); text stays readable.
pub fn frame(color: u32) {
    if let Some(c) = con() {
        let w = c.cols * CW;
        let h = c.rows * CH;
        let b = 8u32;
        for y in 0..h {
            for x in 0..w {
                if x < b || y < b || x >= w - b || y >= h - b {
                    px(c.base, c.stride, x, y, color);
                }
            }
        }
    }
}

fn scroll(c: &mut Console) {
    // Fast path: framebuffer is normal WB RAM, so `copy` (rep movs) beats
    // per-pixel volatile R/W by ~10x. Overlap is dest<src forward -- safe.
    let stride = c.stride as u64;
    let base = c.base;
    let last = (c.rows - 1) * CH;
    unsafe {
        let dst = base as *mut u32;
        let src = (base + (CH as u64 * stride * 4)) as *const u32;
        let n = (last as u64 * stride) as usize;
        core::ptr::copy(src, dst, n);
        // Clear the freed last row (64-bit batches).
        let bg64: u64 = c.bg as u64 | ((c.bg as u64) << 32);
        let row0 = base + (last as u64 * stride * 4);
        let pairs = stride / 2;
        for i in 0..pairs {
            ((row0 + i * 8) as *mut u64).write_volatile(bg64);
        }
        if stride % 2 != 0 {
            ((row0 + pairs * 8) as *mut u32).write_volatile(c.bg);
        }
    }
}

/// Underline block cursor at the next free cell. The cell is always empty
/// (bg) so erase-by-bg is exact; hide before a write batch, show after.
fn draw_cursor(c: &Console) {
    let y0 = c.row * CH + 13; // bottom 3 rows of the 8x16 cell
    for y in y0..y0 + 3 {
        for x in 0..CW {
            px(c.base, c.stride, c.col * CW + x, y, c.fg);
        }
    }
}

fn erase_cursor(c: &Console) {
    let y0 = c.row * CH + 13;
    for y in y0..y0 + 3 {
        for x in 0..CW {
            px(c.base, c.stride, c.col * CW + x, y, c.bg);
        }
    }
}

fn newline(c: &mut Console) {
    c.col = 0;
    c.row += 1;
    if c.row >= c.rows {
        scroll(c);
        c.row = c.rows - 1;
    }
}

fn draw_glyph(c: &Console, col: u32, row: u32, ch: u8) {
    let g = GLYPHS[ch as usize];
    for r in 0..16 {
        let bits = g[r as usize];
        for col_bit in 0..8 {
            // PSF/VGA: MSB = left pixel.
            let on = bits & (0x80 >> col_bit) != 0;
            px(
                c.base,
                c.stride,
                col * CW + col_bit as u32,
                row * CH + r as u32,
                if on { c.fg } else { c.bg },
            );
        }
    }
}

fn putc(c: &mut Console, b: u8) {
    match b {
        b'\n' => newline(c),
        b'\r' => c.col = 0,
        0x08 => {
            // backspace: step back and erase
            if c.col > 0 {
                c.col -= 1;
            } else if c.row > 0 {
                c.row -= 1;
                c.col = c.cols - 1;
            }
            draw_glyph(c, c.col, c.row, b' ');
        }
        b'\t' => {
            let tab = 8u32;
            let next = (c.col + tab) & !(tab - 1);
            while c.col < next.min(c.cols) {
                draw_glyph(c, c.col, c.row, b' ');
                c.col += 1;
            }
            if c.col >= c.cols {
                newline(c);
            }
        }
        _ => {
            if c.col >= c.cols {
                newline(c);
            }
            draw_glyph(c, c.col, c.row, b);
            c.col += 1;
        }
    }
}

struct ScreenWriter;

impl Write for ScreenWriter {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        if let Some(c) = con() {
            for b in s.bytes() {
                putc(c, b);
            }
        }
        Ok(())
    }
}

#[doc(hidden)]
pub fn _print(args: fmt::Arguments) {
    let was = interrupts_enabled();
    cli();
    if let Some(c) = con() {
        erase_cursor(c);
    }
    ScreenWriter.write_fmt(args).ok();
    if let Some(c) = con() {
        // `putc` may have scrolled; cursor pos is already final.
        if c.col >= c.cols {
            c.col = 0;
            c.row += 1;
            if c.row >= c.rows {
                scroll(c);
                c.row = c.rows - 1;
            }
        }
        draw_cursor(c);
    }
    if was {
        sti();
    }
}
