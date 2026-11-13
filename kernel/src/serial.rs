//! COM1 (0x3F8) serial console, both directions, proper 16550A init.
//! Single CPU; `_print` preserves the IF flag so it is safe from both
//! normal code and interrupt handlers.
//!
//! Input arrives on IRQ4 (received-data-available) and feeds the same
//! keyboard queue the PS/2 handler uses, so the shell can't tell serial
//! typing apart from real keys. PuTTY sends DEL for backspace -- mapped
//! to BS here; CR/LF both act as Enter downstream.

use core::fmt::{self, Write};

use crate::cpu::{cli, inb, interrupts_enabled, outb, sti};

const COM1: u16 = 0x3F8;

fn data_wait() {
    while inb(COM1 + 5) & 0x20 == 0 {}
}

/// Initialize 16550A: 38400 8N1, FIFOs on, RDA interrupt on.
/// Call with interrupts disabled.
pub fn init() {
    outb(COM1 + 1, 0x00); // IRQs off while programming
    outb(COM1 + 3, 0x80); // DLAB on
    outb(COM1, 0x03); // divisor lo = 3 -> 38400
    outb(COM1 + 1, 0x00); // divisor hi
    outb(COM1 + 3, 0x03); // 8N1
    outb(COM1 + 2, 0xC7); // FIFO on, clear, 14B threshold
    outb(COM1 + 4, 0x0B); // RTS/DSR, IRQs enabled
    outb(COM1 + 1, 0x01); // RDA (received data available) interrupt on
}

/// Feed one received byte from the IRQ4 handler (IF=0 already).
pub fn on_byte(b: u8) {
    crate::keyboard::push_byte(if b == 0x7F { 0x08 } else { b });
}

fn putc(b: u8) {
    data_wait();
    outb(COM1, b);
}

struct Writer;

impl Write for Writer {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        for b in s.bytes() {
            if b == b'\n' {
                putc(b'\r');
            }
            putc(b);
        }
        Ok(())
    }
}

#[doc(hidden)]
pub fn _print(args: fmt::Arguments) {
    let was = interrupts_enabled();
    cli();
    Writer.write_fmt(args).ok();
    if was {
        sti();
    }
}
