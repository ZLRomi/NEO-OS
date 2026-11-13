//! NEO-OS-3 system call ABI — the single source of truth shared by the
//! kernel and every user-space runtime.
//!
//! ## Design rules
//!
//! * **One table.** The kernel dispatcher and the user-space wrappers both
//!   refer to the constants in this file. No magic numbers on either side.
//! * **Three registers.** `RAX` carries the syscall number on entry and the
//!   result on return; `RDI`, `RSI`, `RDX` carry arguments. Keeping the ABI
//!   this narrow is what makes the user runtime small enough to audit.
//! * **Negative results are errors.** Any return value in `[-4095, -1]` is a
//!   negated `Errno`. This is the Linux convention and it means callers can
//!   test `ret < 0` without a separate out-parameter. Success values are
//!   never negative.
//! * **Buffers are always (ptr, len) pairs.** Every pointer the kernel
//!   dereferences is validated against the caller's address space
//!   (`mem::user_range_ok`), so a user program cannot make the kernel read
//!   or write arbitrary physical memory.
//!
//! This module is `no_std` and dependency-free so the same bytes compile
//! into the kernel and into ring-3 programs.

#![no_std]

// ---------------------------------------------------------------------------
// Syscall numbers
// ---------------------------------------------------------------------------

/// Console / process basics.
pub const SYS_WRITE: u64 = 0;
pub const SYS_EXIT: u64 = 1;
pub const SYS_UPTIME: u64 = 2;
pub const SYS_BRK: u64 = 3;
pub const SYS_MEMINFO: u64 = 4;
pub const SYS_READ_CHAR: u64 = 5;
pub const SYS_SLEEP: u64 = 6;
pub const SYS_PRINT_INT: u64 = 7;

/// Networking.
pub const SYS_NET_SEND: u64 = 8;
pub const SYS_NET_RECV: u64 = 9;
pub const SYS_TCP_SEND: u64 = 10;
pub const SYS_TCP_RECV: u64 = 16;

/// File system.
pub const SYS_OPEN: u64 = 11;
pub const SYS_CLOSE: u64 = 12;
pub const SYS_READ: u64 = 13;
pub const SYS_WRITE_FD: u64 = 14;
pub const SYS_STAT: u64 = 15;
pub const SYS_READDIR: u64 = 17;
pub const SYS_FTRUNCATE: u64 = 18;
pub const SYS_MKDIR: u64 = 23;
pub const SYS_UNLINK: u64 = 24;
pub const SYS_RMDIR: u64 = 35;

/// Processes.
pub const SYS_GETPID: u64 = 19;
pub const SYS_TASKLIST: u64 = 20;
pub const SYS_FORK: u64 = 29;
pub const SYS_WAITPID: u64 = 30;
pub const SYS_GETPPID: u64 = 31;
pub const SYS_KILL: u64 = 32;
pub const SYS_GETPGID: u64 = 33;
pub const SYS_EXEC: u64 = 34;

/// Memory.
pub const SYS_MPROTECT: u64 = 26;
pub const SYS_MMAP: u64 = 27;
pub const SYS_MUNMAP: u64 = 28;

/// Raw block device access (privileged; ring-3 use is a known compromise).
pub const SYS_BLOCK_READ: u64 = 21;
pub const SYS_BLOCK_WRITE: u64 = 22;

/// UDP.
pub const SYS_UDP_SEND: u64 = 25;

/// Highest assigned syscall number. Anything above returns `ENOSYS`.
pub const SYS_MAX: u64 = 35;

// ---------------------------------------------------------------------------
// Error numbers (classic Unix numbering, negated on return)
// ---------------------------------------------------------------------------

pub const EPERM: i64 = 1;
pub const ENOENT: i64 = 2;
pub const ESRCH: i64 = 3;
pub const EIO: i64 = 5;
pub const EBADF: i64 = 9;
pub const EAGAIN: i64 = 11;
pub const ENOMEM: i64 = 12;
pub const EFAULT: i64 = 14;
pub const EEXIST: i64 = 17;
pub const EINVAL: i64 = 22;
pub const ENOSPC: i64 = 28;
pub const ENOSYS: i64 = 38;
pub const ENAMETOOLONG: i64 = 36;
pub const ENOTDIR: i64 = 20;
pub const EISDIR: i64 = 21;
pub const ENOTEMPTY: i64 = 39;

// ---------------------------------------------------------------------------
// File open flags
// ---------------------------------------------------------------------------

pub const O_RDONLY: u32 = 0;
pub const O_WRONLY: u32 = 1;
pub const O_RDWR: u32 = 2;
pub const O_APPEND: u32 = 3;

// ---------------------------------------------------------------------------
// mprotect / mmap protection bits
// ---------------------------------------------------------------------------

pub const PROT_READ: u8 = 1;
pub const PROT_WRITE: u8 = 2;
pub const PROT_EXEC: u8 = 4;

// ---------------------------------------------------------------------------
// Signals
// ---------------------------------------------------------------------------

pub const SIGTERM: u32 = 15;

// ---------------------------------------------------------------------------
// Helpers shared by both sides
// ---------------------------------------------------------------------------

/// True when a raw syscall return value denotes an error.
#[inline]
pub const fn is_err(ret: i64) -> bool {
    ret < 0 && ret >= -4095
}

/// The minimum number of bytes that can hold a serialized task list header.
pub const TASKLIST_HEADER: usize = 4;

/// Byte size of one serialized task entry: id(8) state(1) is_user(1)
/// pgid(8) parent_id(8) name(16).
pub const TASK_ENTRY_SIZE: usize = 42;
