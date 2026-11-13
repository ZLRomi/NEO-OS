//! PS/2 mouse on IRQ12: 3-byte packets -> screen coords + buttons.
//!
//! IRQ path never prints or allocates: bytes assemble into a phase machine
//! and committed packets update position state. The gui loop samples it.
//! Cross-forwarding with the keyboard handler (see interrupts.rs) keeps
//! packet framing alive when both devices have bytes pending at once.

use core::sync::atomic::{AtomicU64, Ordering};

use crate::cpu::{cli, inb, interrupts_enabled, outb, sti};

const DATA: u16 = 0x60;
const STAT: u16 = 0x64;

static mut MX: u32 = 0;
static mut MY: u32 = 0;
static mut MW: u32 = 0;
static mut MH: u32 = 0;
static mut BTN: u8 = 0;
static mut PHASE: u8 = 0;
static mut PB: [u8; 3] = [0; 3];
static SEQ: AtomicU64 = AtomicU64::new(0);

fn stat() -> u8 {
    inb(STAT)
}

/// Spin until the controller input buffer is empty (safe to write).
fn wait_out_empty() {
    for _ in 0..200_000 {
        if stat() & 0x02 == 0 {
            return;
        }
    }
}

/// Spin until a byte is available with the expected source.
/// aux=true: mouse/aux channel (status bit 5); false: controller replies.
fn wait_in(aux: bool) -> bool {
    for _ in 0..200_000 {
        let s = stat();
        if s & 0x01 != 0 && (s & 0x20 != 0) == aux {
            return true;
        }
    }
    false
}

fn read_aux() -> Option<u8> {
    if !wait_in(true) {
        return None;
    }
    Some(inb(DATA))
}

fn controller_config() -> u8 {
    wait_out_empty();
    outb(STAT, 0x20);
    if wait_in(false) {
        inb(DATA)
    } else {
        0x01
    }
}

fn write_controller_config(cfg: u8) {
    wait_out_empty();
    outb(STAT, 0x60);
    wait_out_empty();
    outb(DATA, cfg);
}

/// Send a command byte to the mouse, return its ack byte (expect 0xFA).
fn mouse_cmd(cmd: u8) -> Option<u8> {
    wait_out_empty();
    outb(STAT, 0xD4); // next data byte goes to aux
    wait_out_empty();
    outb(DATA, cmd);
    read_aux()
}

/// Enable the mouse: aux port on, IRQ1+IRQ12 in the controller config
/// (other bits preserved -- notably the kbd translation bit, so the
/// already-working keyboard mapping is untouched), then reset/defaults/
/// enable-data-reporting with ack checks. Polled with IF clear so a racy
/// keyboard IRQ can't steal our ack bytes mid-handshake.
pub fn init(w: u32, h: u32) {
    let was = interrupts_enabled();
    cli();
    unsafe {
        MW = w;
        MH = h;
        MX = w / 2;
        MY = h / 2;
    }
    wait_out_empty();
    outb(STAT, 0xA8); // enable aux port
    // Drain stale output first: a key pressed during boot would otherwise
    // be mistaken for the config byte below, misconfiguring the controller
    // (e.g. killing IRQ1) for the rest of the session.
    for _ in 0..16 {
        if inb(STAT) & 0x01 == 0 {
            break;
        }
        inb(DATA);
    }
    let mut cfg = controller_config();
    cfg |= 0x43; // keyboard IRQ, keyboard translation, and mouse IRQ
    cfg &= !0x30; // enable both device clocks
    write_controller_config(cfg);

    // Reset -> BAT (0xAA) -> ID; then defaults + enable reporting.
    let mut id: u8 = 0xFE;
    let mut ok = false;
    if mouse_cmd(0xFF) == Some(0xFA) && read_aux() == Some(0xAA) {
        id = read_aux().unwrap_or(0xFE);
        ok = mouse_cmd(0xF6) == Some(0xFA) && mouse_cmd(0xF4) == Some(0xFA);
    }
    crate::kinfo!(
        "mouse: cfg={:#04x} id={:#04x} streaming={} at ({},{})",
        cfg,
        id,
        ok,
        w / 2,
        h / 2
    );
    if was {
        sti();
    }
}

/// Feed one aux byte from the IRQ12 handler (IF=0 already).
pub fn on_byte(b: u8) {
    unsafe {
        if PHASE == 0 {
            if b & 0x08 == 0 {
                return; // resync: byte0 always has bit 3 set
            }
            PB[0] = b;
            PHASE = 1;
            return;
        }
        PB[PHASE as usize] = b;
        PHASE += 1;
        if PHASE == 3 {
            PHASE = 0;
            let dx = PB[1] as i8 as i32;
            let dy = PB[2] as i8 as i32;
            MX = (MX as i32 + dx).clamp(0, MW as i32 - 1) as u32;
            MY = (MY as i32 - dy).clamp(0, MH as i32 - 1) as u32;
            BTN = PB[0] & 0x07;
            SEQ.fetch_add(1, Ordering::Relaxed);
        }
    }
}

/// Snapshot (x, y, buttons bitmask, packet sequence). IRQ-safe.
pub fn sample() -> (u32, u32, u8, u64) {
    let was = interrupts_enabled();
    cli();
    let r = unsafe { (MX, MY, BTN, SEQ.load(Ordering::Relaxed)) };
    if was {
        sti();
    }
    r
}
