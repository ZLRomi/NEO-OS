//! Minimal klog: leveled one-line messages over serial + screen.
//!
//! Supports output redirection: when `redirect_start()` is called,
//! all subsequent kprint! output is also captured into a buffer.
//! Call `redirect_stop()` to retrieve the captured bytes.

use core::sync::atomic::{AtomicBool, Ordering};

/// Redirect capture buffer size.
const REDIR_BUF_SIZE: usize = 2048;
static mut REDIR_BUF: [u8; REDIR_BUF_SIZE] = [0; REDIR_BUF_SIZE];
static mut REDIR_POS: usize = 0;
static REDIR_ACTIVE: AtomicBool = AtomicBool::new(false);

/// Start capturing kprint output. Resets the buffer.
pub fn redirect_start() {
    unsafe { REDIR_POS = 0; }
    REDIR_ACTIVE.store(true, Ordering::SeqCst);
}

/// Stop capturing and return the captured bytes.
pub fn redirect_stop() -> &'static [u8] {
    REDIR_ACTIVE.store(false, Ordering::SeqCst);
    unsafe { &REDIR_BUF[..REDIR_POS] }
}

/// Append formatted text to the redirect buffer.
/// Called by kprint! macro when redirect is active.
pub fn redirect_print(args: core::fmt::Arguments) {
    if !REDIR_ACTIVE.load(Ordering::SeqCst) { return; }
    // Use a simple writer that appends to the buffer.
    struct RedirWriter;
    impl core::fmt::Write for RedirWriter {
        fn write_str(&mut self, s: &str) -> core::fmt::Result {
            unsafe {
                for &b in s.as_bytes() {
                    if REDIR_POS < REDIR_BUF_SIZE {
                        REDIR_BUF[REDIR_POS] = b;
                        REDIR_POS += 1;
                    }
                }
            }
            Ok(())
        }
    }
    let _ = core::fmt::write(&mut RedirWriter, args);
}

#[macro_export]
macro_rules! kprint {
    ($($arg:tt)*) => {{
        let args = ::core::format_args!($($arg)*);
        $crate::serial::_print(args);
        $crate::console::_print(args);
        $crate::klog::redirect_print(args);
    }};
}

#[macro_export]
macro_rules! kinfo {
    ($($arg:tt)*) => {
        $crate::kprint!("[INFO] {}\n", ::core::format_args!($($arg)*))
    };
}

/// Serial-only log line: never touches the screen or redirect buffer.
#[macro_export]
macro_rules! sinfo {
    ($($arg:tt)*) => {
        $crate::serial::_print(::core::format_args!("[INFO] {}\n", ::core::format_args!($($arg)*)))
    };
}

#[macro_export]
macro_rules! kwarn {
    ($($arg:tt)*) => {
        $crate::kprint!("[WARN] {}\n", ::core::format_args!($($arg)*))
    };
}

#[macro_export]
macro_rules! kerror {
    ($($arg:tt)*) => {
        $crate::kprint!("[ERROR] {}\n", ::core::format_args!($($arg)*))
    };
}
