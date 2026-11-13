//! M10 graphical desktop demo: background, one draggable window with a
//! live uptime clock, right-button dots, save-under mouse cursor.
//!
//! The text console owns no pixels while we're here: entering discards
//! scrollback, and Esc returns via console::clear + the shell prompt.
//! IRQ handlers never draw (keyboard/mouse only queue/sample state), so
//! repaints need no locking.

use alloc::format;

use crate::{console, cpu, gfx, interrupts, keyboard, kprint, mouse};

struct Win {
    x: u32,
    y: u32,
    w: u32,
    h: u32,
}

const TITLE_H: u32 = 20;
const MAX_DOTS: usize = 64;

static mut DOTS: [(u32, u32); MAX_DOTS] = [(0, 0); MAX_DOTS];
static mut NDOTS: usize = 0;

fn dot_add(x: u32, y: u32) {
    unsafe {
        if NDOTS < MAX_DOTS {
            DOTS[NDOTS] = (x, y);
            NDOTS += 1;
        }
    }
}

fn in_title(w: &Win, x: u32, y: u32) -> bool {
    x >= w.x && x < w.x + w.w && y >= w.y && y < w.y + TITLE_H
}

fn paint(w: &Win, mx: u32, my: u32, btn: u8, secs: u64) {
    let bg = gfx::color(0x08, 0x18, 0x38);
    let frame = gfx::color(0x90, 0x90, 0x90);
    let title = gfx::color(0x10, 0x40, 0x90);
    let body = gfx::color(0xE8, 0xE8, 0xE8);
    let fg = gfx::color(0x00, 0x00, 0x00);
    let accent = gfx::color(0xC0, 0x30, 0x30);

    gfx::fill_rect(0, 0, u32::MAX, u32::MAX, bg);
    // Right-button dots (persistent until Esc).
    for i in 0..unsafe { NDOTS } {
        let (dx, dy) = unsafe { DOTS[i] };
        gfx::fill_rect(dx.saturating_sub(1), dy.saturating_sub(1), 3, 3, accent);
    }
    // Window: shadow, frame, title bar, body.
    gfx::fill_rect(w.x + 4, w.y + 4, w.w, w.h, gfx::color(0x00, 0x00, 0x00));
    gfx::fill_rect(w.x, w.y, w.w, w.h, frame);
    gfx::fill_rect(w.x + 1, w.y + 1, w.w - 2, w.h - 2, body);
    gfx::fill_rect(w.x + 1, w.y + 1, w.w - 2, TITLE_H, title);
    gfx::put_text(w.x + 8, w.y + 3, "NEO-OS desktop (M10)", fg, title);
    gfx::put_text(
        w.x + 8,
        w.y + TITLE_H + 8,
        &format!("uptime: {}s   mouse: {},{} btn{}", secs, mx, my, btn),
        fg,
        body,
    );
    gfx::put_text(
        w.x + 8,
        w.y + TITLE_H + 28,
        "drag the title bar - right click dots - esc exits",
        fg,
        body,
    );
    gfx::cursor_draw(mx, my, gfx::color(0xFF, 0xFF, 0xFF));
}

pub fn enter() {
    let (sw, sh) = gfx::size();
    let mut win = Win {
        x: sw / 4,
        y: sh / 4,
        w: sw / 2,
        h: sh / 3,
    };
    let mut last_seq = 0u64;
    let mut last_sec = u64::MAX;
    let mut dragging = false;
    let mut ox = 0u32;
    let mut oy = 0u32;
    let mut prev_btn = 0u8;

    kprint!("gui: mouse moves, drag title, right=dot, esc=exit\n");
    // Fresh scene, then the event loop.
    gfx::cursor_erase(); // no-op first time (empty saved rect)
    let (mut mx, mut my, mut btn, _) = mouse::sample();
    mx = mx.min(sw.saturating_sub(1));
    my = my.min(sh.saturating_sub(1));
    paint(&win, mx, my, btn, interrupts::uptime_secs());

    loop {
        while let Some(b) = keyboard::pop() {
            if b == 0x1B {
                console::clear();
                return;
            }
        }
        let mut repaint = false;
        let (nx, ny, nbtn, seq) = mouse::sample();
        if seq != last_seq {
            last_seq = seq;
            // Left press inside the title bar grabs the window.
            if nbtn & 1 != 0 && !dragging && in_title(&win, nx, ny) {
                dragging = true;
                ox = nx - win.x;
                oy = ny - win.y;
            }
            if nbtn & 1 == 0 {
                dragging = false;
            }
            if dragging && nbtn & 1 != 0 {
                win.x = nx.saturating_sub(ox).min(sw.saturating_sub(win.w.min(sw)));
                win.y = ny.saturating_sub(oy).min(sh.saturating_sub(win.h.min(sh)));
            }
            // Right-button rising edge drops a dot.
            if nbtn & 2 != 0 && prev_btn & 2 == 0 {
                dot_add(nx, ny);
            }
            prev_btn = nbtn;
            mx = nx;
            my = ny;
            btn = nbtn;
            repaint = true;
        }
        let s = interrupts::uptime_secs();
        if s != last_sec {
            last_sec = s;
            repaint = true;
        }
        if repaint {
            gfx::cursor_erase(); // restores pixels under the old cursor
            paint(&win, mx, my, btn, s);
        }
        cpu::sti();
        cpu::hlt();
    }
}
