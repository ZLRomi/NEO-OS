//! NEO-OS-3 kernel synchronization primitives.
//!
//! Provides spinlock and ticket lock for multi-core synchronization.
//! All locks are interrupt-safe (disable local interrupts while held).
//!
//! # Usage
//! ```ignore
//! let lock = SpinLock::new(42u32);
//! {
//!     let mut guard = lock.lock();
//!     *guard = 100;
//! } // lock released here
//! ```
//!
//! # Safety Notes
//! - Locks save/restore RFLAGS to prevent deadlock with IRQ handlers
//! - Guards implement `Deref`/`DerefMut` for ergonomic access
//! - `try_lock()` returns `None` without spinning if already held

use core::sync::atomic::{AtomicU32, AtomicBool, Ordering};
use core::cell::UnsafeCell;

/// A simple test-and-set spinlock with interrupt disabling.
///
/// Suitable for short critical sections. For longer or contended sections,
/// prefer `TicketLock` which provides fairness.
pub struct SpinLock<T> {
    locked: AtomicBool,
    data: UnsafeCell<T>,
}

unsafe impl<T: Send> Sync for SpinLock<T> {}
unsafe impl<T: Send> Send for SpinLock<T> {}

impl<T> SpinLock<T> {
    /// Create a new unlocked spinlock wrapping the given data.
    pub const fn new(data: T) -> Self {
        Self {
            locked: AtomicBool::new(false),
            data: UnsafeCell::new(data),
        }
    }

    /// Acquire the lock, disabling local interrupts.
    /// Returns a guard that releases the lock on drop.
    pub fn lock(&self) -> SpinLockGuard<'_, T> {
        // Disable interrupts to prevent deadlock with IRQ handlers.
        let flags = save_flags_and_cli();

        // Spin until we acquire the lock.
        while self.locked.compare_exchange_weak(
            false, true,
            Ordering::Acquire,
            Ordering::Relaxed,
        ).is_err() {
            // Spin hint to reduce power/bus contention.
            while self.locked.load(Ordering::Relaxed) {
                core::hint::spin_loop();
            }
        }

        SpinLockGuard {
            lock: self,
            flags,
        }
    }

    /// Try to acquire the lock without spinning.
    /// Returns Some(guard) if acquired, None if already locked.
    pub fn try_lock(&self) -> Option<SpinLockGuard<'_, T>> {
        let flags = save_flags_and_cli();

        if self.locked.compare_exchange(
            false, true,
            Ordering::Acquire,
            Ordering::Relaxed,
        ).is_ok() {
            Some(SpinLockGuard {
                lock: self,
                flags,
            })
        } else {
            restore_flags(flags);
            None
        }
    }
}

/// RAII guard for SpinLock. Releases lock and restores interrupts on drop.
pub struct SpinLockGuard<'a, T> {
    lock: &'a SpinLock<T>,
    flags: u64,
}

impl<'a, T> core::ops::Deref for SpinLockGuard<'a, T> {
    type Target = T;
    fn deref(&self) -> &T {
        unsafe { &*self.lock.data.get() }
    }
}

impl<'a, T> core::ops::DerefMut for SpinLockGuard<'a, T> {
    fn deref_mut(&mut self) -> &mut T {
        unsafe { &mut *self.lock.data.get() }
    }
}

impl<'a, T> Drop for SpinLockGuard<'a, T> {
    fn drop(&mut self) {
        self.lock.locked.store(false, Ordering::Release);
        restore_flags(self.flags);
    }
}

/// A fair ticket lock with interrupt disabling.
///
/// Guarantees FIFO ordering: threads acquire the lock in the order they
/// requested it. Prevents starvation under high contention.
pub struct TicketLock<T> {
    next_ticket: AtomicU32,
    now_serving: AtomicU32,
    data: UnsafeCell<T>,
}

unsafe impl<T: Send> Sync for TicketLock<T> {}
unsafe impl<T: Send> Send for TicketLock<T> {}

impl<T> TicketLock<T> {
    /// Create a new unlocked ticket lock wrapping the given data.
    pub const fn new(data: T) -> Self {
        Self {
            next_ticket: AtomicU32::new(0),
            now_serving: AtomicU32::new(0),
            data: UnsafeCell::new(data),
        }
    }

    /// Acquire the lock, disabling local interrupts.
    pub fn lock(&self) -> TicketLockGuard<'_, T> {
        let flags = save_flags_and_cli();

        // Take a ticket.
        let my_ticket = self.next_ticket.fetch_add(1, Ordering::Relaxed);

        // Wait until our ticket is served.
        while self.now_serving.load(Ordering::Acquire) != my_ticket {
            core::hint::spin_loop();
        }

        TicketLockGuard {
            lock: self,
            flags,
        }
    }

    /// Try to acquire without spinning.
    pub fn try_lock(&self) -> Option<TicketLockGuard<'_, T>> {
        let flags = save_flags_and_cli();

        let current = self.now_serving.load(Ordering::Acquire);
        // Only succeed if we're the next in line.
        if self.next_ticket.compare_exchange(
            current, current + 1,
            Ordering::AcqRel,
            Ordering::Relaxed,
        ).is_ok() {
            Some(TicketLockGuard {
                lock: self,
                flags,
            })
        } else {
            restore_flags(flags);
            None
        }
    }
}

/// RAII guard for TicketLock.
pub struct TicketLockGuard<'a, T> {
    lock: &'a TicketLock<T>,
    flags: u64,
}

impl<'a, T> core::ops::Deref for TicketLockGuard<'a, T> {
    type Target = T;
    fn deref(&self) -> &T {
        unsafe { &*self.lock.data.get() }
    }
}

impl<'a, T> core::ops::DerefMut for TicketLockGuard<'a, T> {
    fn deref_mut(&mut self) -> &mut T {
        unsafe { &mut *self.lock.data.get() }
    }
}

impl<'a, T> Drop for TicketLockGuard<'a, T> {
    fn drop(&mut self) {
        self.lock.now_serving.fetch_add(1, Ordering::Release);
        restore_flags(self.flags);
    }
}

// ---- Interrupt flag save/restore ----

/// Save RFLAGS and disable interrupts. Returns saved flags.
fn save_flags_and_cli() -> u64 {
    let flags: u64;
    unsafe {
        core::arch::asm!(
            "pushfq",
            "pop {0}",
            "cli",
            out(reg) flags,
            options(nomem),
        );
    }
    flags
}

/// Restore previously saved RFLAGS (re-enables interrupts if they were on).
fn restore_flags(flags: u64) {
    unsafe {
        core::arch::asm!(
            "push {0}",
            "popfq",
            in(reg) flags,
            options(nomem),
        );
    }
}
