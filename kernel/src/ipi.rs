//! NEO-OS-3 Inter-Processor Interrupt (IPI) support.
//!
//! Sends interrupts between CPUs via the local APIC's Interrupt Command Register.
//! Used for cross-core scheduling, TLB shootdowns, and signaling.

use crate::kinfo;

/// Local APIC base address (default for x86-64).
const LAPIC_BASE: u64 = 0xFEE00000;

/// ICR register offsets.
const ICR_LOW: u64 = 0x300;
const ICR_HIGH: u64 = 0x310;

/// ICR delivery modes.
pub const ICR_FIXED: u32 = 0x00000;
pub const ICR_SMI: u32 = 0x00200;
pub const ICR_NMI: u32 = 0x00400;
pub const ICR_INIT: u32 = 0x00500;
pub const ICR_STARTUP: u32 = 0x00600;

/// ICR shorthand destinations.
pub const ICR_SELF: u32 = 0x40000;     // Send to self
pub const ICR_ALL: u32 = 0x80000;      // Send to all including self
pub const ICR_OTHERS: u32 = 0xC0000;   // Send to all excluding self

/// Read a 32-bit value from the local APIC.
fn lapic_read(offset: u64) -> u32 {
    unsafe { core::ptr::read_volatile((LAPIC_BASE + offset) as *const u32) }
}

/// Write a 32-bit value to the local APIC.
fn lapic_write(offset: u64, val: u32) {
    unsafe { core::ptr::write_volatile((LAPIC_BASE + offset) as *mut u32, val); }
}

/// Wait for ICR delivery status bit to clear (previous IPI completed).
fn wait_icr_idle() {
    while lapic_read(ICR_LOW) & (1 << 12) != 0 {
        core::hint::spin_loop();
    }
}

/// Send an IPI to a specific APIC ID with the given vector and delivery mode.
pub fn send_ipi(target_apic_id: u8, vector: u8, delivery_mode: u32) {
    wait_icr_idle();

    // Set destination APIC ID in ICR high (bits 24-31).
    lapic_write(ICR_HIGH, (target_apic_id as u32) << 24);

    // Set vector, delivery mode, level assert, and destination mode (physical).
    let icr_low = (vector as u32) | delivery_mode | (1 << 14); // level assert
    lapic_write(ICR_LOW, icr_low);

    kinfo!("ipi: sent vector {} to APIC {}", vector, target_apic_id);
}

/// Send an IPI to all other CPUs (excluding self).
pub fn send_ipi_others(vector: u8, delivery_mode: u32) {
    wait_icr_idle();

    let icr_low = (vector as u32) | delivery_mode | ICR_OTHERS | (1 << 14);
    lapic_write(ICR_LOW, icr_low);
}

/// Send an IPI to self (useful for testing).
pub fn send_ipi_self(vector: u8, delivery_mode: u32) {
    wait_icr_idle();

    let icr_low = (vector as u32) | delivery_mode | ICR_SELF | (1 << 14);
    lapic_write(ICR_LOW, icr_low);
}

/// Get the current CPU's APIC ID.
pub fn current_apic_id() -> u8 {
    (lapic_read(0x20) >> 24) as u8
}

/// Initialize IPI subsystem (just logs APIC ID).
pub fn init() {
    let id = current_apic_id();
    kinfo!("ipi: BSP APIC ID={}, LAPIC at {:#x}", id, LAPIC_BASE);
}
