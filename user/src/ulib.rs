//! NEO-OS-3 user-space runtime (`ulib`).
//!
//! This is the piece that turns the kernel from "a kernel" into "an
//! operating system": every ring-3 program links against this crate instead
//! of open-coding `int 0x80` and re-implementing `malloc` and `print`.
//!
//! ## Layering
//!
//! ```text
//!   application (user/src/bin/*)
//!        ↓  uses
//!   ulib  ── syscall wrappers, errno, heap, string/formatting helpers
//!        ↓  int 0x80 with the ABI from neo_common::syscall
//!   kernel
//! ```
//!
//! ## Design notes
//!
//! * **Errors are `i64`** in the Linux style: a negative value in
//!   `[-4095, -1]` is a negated errno. `SysResult` makes that explicit so
//!   callers stop comparing against `!0u64`.
//! * **No hidden allocation.** `malloc`/{free} take pages from the kernel via
//!   `brk` and carve them into blocks; nothing in the syscall layer allocates,
//!   which keeps error paths simple and deterministic.
//! * **Everything here is `no_std`.** There is no libc in this system; this
//!   module *is* the libc.
//!
//! The crate is deliberately small. If a helper is not needed by at least two
//! programs it belongs in the program, not here.

#![no_std]

use core::arch::asm;

pub use neo_common::syscall::*;

// ---------------------------------------------------------------------------
// Raw syscall entry
// ---------------------------------------------------------------------------

/// Perform a raw system call.
///
/// # Safety
/// The caller is responsible for the validity of any pointer arguments.
#[inline]
pub unsafe fn syscall4(nr: u64, a: u64, b: u64, c: u64) -> u64 {
    let ret: u64;
    unsafe {
        asm!(
            "int 0x80",
            inlateout("rax") nr => ret,
            in("rdi") a,
            in("rsi") b,
            in("rdx") c,
            options(nostack),
        );
    }
    ret
}

/// Typed result of a system call.
pub type SysResult = Result<u64, i64>;

/// Convert a raw syscall return into a `SysResult`.
#[inline]
pub fn to_result(raw: u64) -> SysResult {
    let s = raw as i64;
    if is_err(s) { Err(-s) } else { Ok(raw) }
}

/// Write a byte slice to the console.
pub fn write(buf: &[u8]) -> SysResult {
    if buf.is_empty() {
        return Ok(0);
    }
    to_result(unsafe { syscall4(SYS_WRITE, 0, buf.as_ptr() as u64, buf.len() as u64) })
}

/// Exit the current process. Never returns.
pub fn exit(code: u64) -> ! {
    unsafe { syscall4(SYS_EXIT, code, 0, 0) };
    loop {
        core::hint::spin_loop();
    }
}

/// Monotonic uptime in PIT ticks (10 ms each).
pub fn uptime_ticks() -> u64 {
    unsafe { syscall4(SYS_UPTIME, 0, 0, 0) }
}

/// Sleep for `ticks` PIT ticks. Blocks the process rather than spinning.
pub fn sleep_ticks(ticks: u64) {
    unsafe { syscall4(SYS_SLEEP, ticks, 0, 0) };
}

/// Sleep for a number of milliseconds.
pub fn sleep_ms(ms: u64) {
    sleep_ticks(ms.div_ceil(10));
}

/// Current process id.
pub fn getpid() -> u64 {
    unsafe { syscall4(SYS_GETPID, 0, 0, 0) }
}

/// Parent process id.
pub fn getppid() -> u64 {
    unsafe { syscall4(SYS_GETPPID, 0, 0, 0) }
}

/// Process group id.
pub fn getpgid() -> u64 {
    unsafe { syscall4(SYS_GETPGID, 0, 0, 0) }
}

/// Fork the current process.
///
/// Returns `Ok(0)` in the child and `Ok(child_pid)` in the parent.
pub fn fork() -> SysResult {
    to_result(unsafe { syscall4(SYS_FORK, 0, 0, 0) })
}

/// Wait for a child to exit. `pid == 0` waits for any child.
/// Returns the reaped pid.
pub fn waitpid(pid: u64) -> SysResult {
    to_result(unsafe { syscall4(SYS_WAITPID, pid, 0, 0) })
}

/// Send a signal to a process.
pub fn kill(pid: u64, sig: u32) -> SysResult {
    to_result(unsafe { syscall4(SYS_KILL, pid, sig as u64, 0) })
}

/// Open a file. Returns a file descriptor.
pub fn open(name: &[u8], flags: u32) -> SysResult {
    to_result(unsafe {
        syscall4(SYS_OPEN, name.as_ptr() as u64, name.len() as u64, flags as u64)
    })
}

/// Close a file descriptor.
pub fn close(fd: u64) -> SysResult {
    to_result(unsafe { syscall4(SYS_CLOSE, fd, 0, 0) })
}

/// Read from a file descriptor into `buf`.
pub fn read(fd: u64, buf: &mut [u8]) -> SysResult {
    if buf.is_empty() {
        return Ok(0);
    }
    to_result(unsafe {
        syscall4(SYS_READ, fd, buf.as_mut_ptr() as u64, buf.len() as u64)
    })
}

/// Write to a file descriptor from `buf`.
pub fn write_fd(fd: u64, buf: &[u8]) -> SysResult {
    if buf.is_empty() {
        return Ok(0);
    }
    to_result(unsafe {
        syscall4(SYS_WRITE_FD, fd, buf.as_ptr() as u64, buf.len() as u64)
    })
}

/// Read one character from the keyboard. `None` when no input is pending.
pub fn read_char() -> Option<u8> {
    let c = unsafe { syscall4(SYS_READ_CHAR, 0, 0, 0) };
    if c == 0 { None } else { Some(c as u8) }
}

/// Adjust the program break. `new_brk == 0` queries without changing it.
pub fn brk(new_brk: u64) -> u64 {
    unsafe { syscall4(SYS_BRK, new_brk, 0, 0) }
}

// ---------------------------------------------------------------------------
// Console helpers
// ---------------------------------------------------------------------------

/// Print a string with no trailing newline.
pub fn print(s: &str) {
    let _ = write(s.as_bytes());
}

/// Print a string followed by a newline.
pub fn println(s: &str) {
    print(s);
    print("\n");
}

/// Print an unsigned integer in decimal.
pub fn print_u64(mut v: u64) {
    let mut buf = [0u8; 20];
    let mut i = buf.len();
    if v == 0 {
        print("0");
        return;
    }
    while v > 0 {
        i -= 1;
        buf[i] = b'0' + (v % 10) as u8;
        v /= 10;
    }
    let _ = write(&buf[i..]);
}

/// Print a signed integer in decimal.
pub fn print_i64(v: i64) {
    if v < 0 {
        print("-");
        print_u64(v.unsigned_abs());
    } else {
        print_u64(v as u64);
    }
}

// ---------------------------------------------------------------------------
// Heap
// ---------------------------------------------------------------------------
//
// A first-fit free list over pages obtained from `brk`. Blocks are 8-byte
// aligned and carry a 16-byte header (size + next-free pointer), which is
// enough for a teaching-quality allocator without a `no_std` alloc crate.

const HDR: usize = 16;
static mut HEAP_END: u64 = 0;
static mut FREE_LIST: u64 = 0;

/// The program break as seen by the heap, or 0 before initialisation.
pub fn heap_base() -> u64 {
    unsafe { HEAP_END }
}

/// Allocate `size` bytes. Returns null on failure.
pub fn malloc(size: usize) -> *mut u8 {
    unsafe {
        if HEAP_END == 0 {
            HEAP_END = brk(0);
            if HEAP_END == 0 {
                return core::ptr::null_mut();
            }
        }
        let need = ((size + 7) & !7) + HDR;

        // First fit over the free list.
        let mut prev: u64 = 0;
        let mut cur = FREE_LIST;
        while cur != 0 {
            let entry = cur as *mut u64;
            if *entry as usize >= need {
                let next = *entry.add(1);
                if prev == 0 {
                    FREE_LIST = next;
                } else {
                    *(prev as *mut u64).add(1) = next;
                }
                return (cur + HDR as u64) as *mut u8;
            }
            prev = cur;
            cur = *entry.add(1);
        }

        // Bump the break.
        let base = HEAP_END;
        let new_end = base + need as u64;
        if brk(new_end) < new_end {
            return core::ptr::null_mut();
        }
        HEAP_END = new_end;
        *(base as *mut u64) = need as u64;
        (base + HDR as u64) as *mut u8
    }
}

/// Free memory previously returned by `malloc`.
///
/// # Safety
/// `ptr` must come from `malloc` and must not be freed twice.
pub unsafe fn free(ptr: *mut u8) {
    if ptr.is_null() {
        return;
    }
    unsafe {
        let base = ptr as u64 - HDR as u64;
        let next = FREE_LIST;
        *(base as *mut u64) = *(base as *mut u64); // size kept
        *(base as *mut u64).add(1) = next;
        FREE_LIST = base;
    }
}

/// Allocate and zero `count * size` bytes.
pub fn calloc(count: usize, size: usize) -> *mut u8 {
    let total = count.saturating_mul(size);
    let p = malloc(total);
    if !p.is_null() {
        unsafe { core::ptr::write_bytes(p, 0, total) };
    }
    p
}
