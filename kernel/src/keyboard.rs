//! Scancode set 1 decoding + SPSC byte queue (IRQ pushes, shell pops).
//! IRQ path never prints or allocates: it only decodes to ASCII and drops
//! on overflow, so `kinfo!` from IRQ1 (M4 behavior) can't interleave shell I/O.

use core::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

use crate::cpu::{cli, interrupts_enabled, sti};

static SHIFT: AtomicBool = AtomicBool::new(false);
static CAPS: AtomicBool = AtomicBool::new(false);
static E0: AtomicBool = AtomicBool::new(false);
// Trace every raw scancode byte to serial (screen untouched). Answers
// "are keys arriving at all" without needing a working shell.
static TRACE: AtomicBool = AtomicBool::new(true);

pub fn set_trace(on: bool) {
    TRACE.store(on, Ordering::Relaxed);
}

pub fn tracing() -> bool {
    TRACE.load(Ordering::Relaxed)
}

const KBUF: usize = 256;
static mut BUF: [u8; KBUF] = [0; KBUF];
static HEAD: AtomicUsize = AtomicUsize::new(0); // producer (IRQ1)
static TAIL: AtomicUsize = AtomicUsize::new(0); // consumer (shell)

fn push(b: u8) {
    // IRQ runs with IF=0 (interrupt gate), consumer uses cli: no torn update.
    let h = HEAD.load(Ordering::Relaxed);
    let t = TAIL.load(Ordering::Relaxed);
    if h.wrapping_sub(t) < KBUF as usize {
        unsafe {
            BUF[h % KBUF] = b;
        }
        HEAD.store(h.wrapping_add(1), Ordering::Relaxed);
    }
    // else overflow: drop (shell stays responsive; no print from IRQ).
}

/// Queue a byte from a non-scancode source (serial input). Same drop-on-
/// full discipline as the IRQ path.
pub(crate) fn push_byte(b: u8) {
    push(b);
}

/// Pop one queued byte (main context). IRQ-safe via cli/sti pairing.
pub fn pop() -> Option<u8> {
    let was = interrupts_enabled();
    cli();
    let h = HEAD.load(Ordering::Relaxed);
    let t = TAIL.load(Ordering::Relaxed);
    let r = if t == h {
        None
    } else {
        let b = unsafe { BUF[t % KBUF] };
        TAIL.store(t.wrapping_add(1), Ordering::Relaxed);
        Some(b)
    };
    if was {
        sti();
    }
    r
}

/// Returns the printable char for a press-code, or None.
fn decode(code: u8, shift: bool) -> Option<char> {
    let c = match code {
        0x02 => {
            if shift {
                '!'
            } else {
                '1'
            }
        }
        0x03 => {
            if shift {
                '@'
            } else {
                '2'
            }
        }
        0x04 => {
            if shift {
                '#'
            } else {
                '3'
            }
        }
        0x05 => {
            if shift {
                '$'
            } else {
                '4'
            }
        }
        0x06 => {
            if shift {
                '%'
            } else {
                '5'
            }
        }
        0x07 => {
            if shift {
                '^'
            } else {
                '6'
            }
        }
        0x08 => {
            if shift {
                '&'
            } else {
                '7'
            }
        }
        0x09 => {
            if shift {
                '*'
            } else {
                '8'
            }
        }
        0x0A => {
            if shift {
                '('
            } else {
                '9'
            }
        }
        0x0B => {
            if shift {
                ')'
            } else {
                '0'
            }
        }
        0x0C => {
            if shift {
                '_'
            } else {
                '-'
            }
        }
        0x0D => {
            if shift {
                '+'
            } else {
                '='
            }
        }
        0x01 => '\x1b', // Esc (the gui loop watches for it to exit)
        0x0E => '\x08', // backspace
        0x0F => '\t',
        0x10 => {
            if shift {
                'Q'
            } else {
                'q'
            }
        }
        0x11 => {
            if shift {
                'W'
            } else {
                'w'
            }
        }
        0x12 => {
            if shift {
                'E'
            } else {
                'e'
            }
        }
        0x13 => {
            if shift {
                'R'
            } else {
                'r'
            }
        }
        0x14 => {
            if shift {
                'T'
            } else {
                't'
            }
        }
        0x15 => {
            if shift {
                'Y'
            } else {
                'y'
            }
        }
        0x16 => {
            if shift {
                'U'
            } else {
                'u'
            }
        }
        0x17 => {
            if shift {
                'I'
            } else {
                'i'
            }
        }
        0x18 => {
            if shift {
                'O'
            } else {
                'o'
            }
        }
        0x19 => {
            if shift {
                'P'
            } else {
                'p'
            }
        }
        0x1C => '\n',
        0x1A => {
            if shift {
                '{'
            } else {
                '['
            }
        }
        0x1B => {
            if shift {
                '}'
            } else {
                ']'
            }
        }
        0x1E => {
            if shift {
                'A'
            } else {
                'a'
            }
        }
        0x1F => {
            if shift {
                'S'
            } else {
                's'
            }
        }
        0x20 => {
            if shift {
                'D'
            } else {
                'd'
            }
        }
        0x21 => {
            if shift {
                'F'
            } else {
                'f'
            }
        }
        0x22 => {
            if shift {
                'G'
            } else {
                'g'
            }
        }
        0x23 => {
            if shift {
                'H'
            } else {
                'h'
            }
        }
        0x24 => {
            if shift {
                'J'
            } else {
                'j'
            }
        }
        0x25 => {
            if shift {
                'K'
            } else {
                'k'
            }
        }
        0x26 => {
            if shift {
                'L'
            } else {
                'l'
            }
        }
        0x27 => {
            if shift {
                ':'
            } else {
                ';'
            }
        }
        0x28 => {
            if shift {
                '"'
            } else {
                '\''
            }
        }
        0x29 => {
            if shift {
                '~'
            } else {
                '`'
            }
        }
        0x2B => {
            if shift {
                '|'
            } else {
                '\\'
            }
        }
        0x2C => {
            if shift {
                'Z'
            } else {
                'z'
            }
        }
        0x2D => {
            if shift {
                'X'
            } else {
                'x'
            }
        }
        0x2E => {
            if shift {
                'C'
            } else {
                'c'
            }
        }
        0x2F => {
            if shift {
                'V'
            } else {
                'v'
            }
        }
        0x30 => {
            if shift {
                'B'
            } else {
                'b'
            }
        }
        0x31 => {
            if shift {
                'N'
            } else {
                'n'
            }
        }
        0x32 => {
            if shift {
                'M'
            } else {
                'm'
            }
        }
        0x33 => {
            if shift {
                '<'
            } else {
                ','
            }
        }
        0x34 => {
            if shift {
                '>'
            } else {
                '.'
            }
        }
        0x35 => {
            if shift {
                '?'
            } else {
                '/'
            }
        }
        // Numpad (NumLock ON assumed; with it off these are nav keys and
        // will wrongly print digits -- proper NumLock tracking is a
        // follow-up). Shift-agnostic on purpose.
        0x37 => '*',
        0x47 => '7',
        0x48 => '8',
        0x49 => '9',
        0x4A => '-',
        0x4B => '4',
        0x4C => '5',
        0x4D => '6',
        0x4E => '+',
        0x4F => '1',
        0x50 => '2',
        0x51 => '3',
        0x52 => '0',
        0x53 => '.',
        0x39 => ' ',
        _ => return None,
    };
    Some(c)
}

/// Letter scancodes (case obeys Shift ^ CapsLock); digits/symbols obey Shift.
fn is_letter(code: u8) -> bool {
    matches!(code, 0x10..=0x19 | 0x1E..=0x26 | 0x2C..=0x32)
}

/// Called from the IRQ1 handler with the raw scancode byte.
/// Never prints to screen or allocates: decode to ASCII and queue, drop
/// if full/unmapped. Raw bytes go to serial when tracing (default on).
pub fn on_scancode(code: u8) {
    if TRACE.load(Ordering::Relaxed) {
        crate::sinfo!("kbd: {:#04x}", code);
    }
    if code == 0xE0 {
        E0.store(true, Ordering::Relaxed); // extended prefix; drop + next byte
        return;
    }
    if E0.swap(false, Ordering::Relaxed) {
        // Extended keys: arrows and keypad enter reach the shell.
        match code {
            0x48 => push(b'\x01'), // Up arrow → history back
            0x50 => push(b'\x02'), // Down arrow → history forward
            0x1C => push(b'\n'),   // keypad Enter
            0x35 => push(b'/'),    // keypad /
            _ => {}
        }
        return;
    }
    match code {
        0x2A | 0x36 => SHIFT.store(true, Ordering::Relaxed), // shift press
        0xAA | 0xB6 => SHIFT.store(false, Ordering::Relaxed), // shift release
        0x3A => CAPS.store(!CAPS.load(Ordering::Relaxed), Ordering::Relaxed), // caps toggle
        0xBA => {}                                    // caps release ignored
        _ if code & 0x80 == 0 => {
            let shift = SHIFT.load(Ordering::Relaxed);
            let caps = CAPS.load(Ordering::Relaxed);
            let eff = if is_letter(code) {
                shift ^ caps
            } else {
                shift
            };
            if let Some(c) = decode(code, eff) {
                push(c as u8);
            }
            // unmapped (ctrl/alt/win): drop silently, shell stays clean
        }
        _ => {} // other releases ignored
    }
}
