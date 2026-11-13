//! Raw CPU primitives: flags, halting, port I/O, descriptor tables.
//! Hand-rolled on purpose (M2): no `x86_64` crate, just Intel SDM layouts.

use core::arch::asm;

pub fn cli() {
    unsafe { asm!("cli", options(nomem, nostack, preserves_flags)) };
}

pub fn sti() {
    unsafe { asm!("sti", options(nomem, nostack, preserves_flags)) };
}

pub fn hlt() {
    unsafe { asm!("hlt", options(nomem, nostack, preserves_flags)) };
}

/// Busy-wait delay in milliseconds (approximate, PIT-dependent).
pub fn delay_ms(ms: u32) {
    // PIT runs at ~100Hz (10ms per tick). Use ticks for coarse delay.
    let target = crate::interrupts::ticks().wrapping_add((ms as u64) / 10);
    while crate::interrupts::ticks() < target {
        core::hint::spin_loop();
    }
}

/// True if RFLAGS.IF is set (interrupts enabled).
pub fn interrupts_enabled() -> bool {
    let f: u64;
    unsafe {
        asm!("pushfq", "pop {}", out(reg) f, options(nomem, preserves_flags));
    }
    f & 0x200 != 0
}

pub fn outb(port: u16, val: u8) {
    unsafe {
        asm!("out dx, al",
            in("dx") port, in("al") val,
            options(nomem, nostack, preserves_flags))
    };
}

pub fn inb(port: u16) -> u8 {
    let v: u8;
    unsafe {
        asm!("in al, dx",
            in("dx") port, out("al") v,
            options(nomem, nostack, preserves_flags))
    };
    v
}

pub fn outw(port: u16, val: u16) {
    unsafe {
        asm!("out dx, ax",
            in("dx") port, in("ax") val,
            options(nomem, nostack, preserves_flags))
    };
}

/// Hit the (unused) debug port: ~1us delay for PIC-era hardware.
pub fn io_wait() {
    outb(0x80, 0);
}

const MSR_EFER: u32 = 0xC000_0080;
const EFER_NXE: u64 = 1 << 11; // No-Execute Enable

/// CPUID leaf 0x80000001 EDX bit 20: NX supported.
fn has_nxe() -> bool {
    // core's __cpuid handles the rbx-clobber correctly (raw asm can't name
    // ebx: LLVM reserves it).
    let max = core::arch::x86_64::__cpuid(0x8000_0000).eax;
    if max < 0x8000_0001 {
        return false;
    }
    core::arch::x86_64::__cpuid(0x8000_0001).edx & (1 << 20) != 0
}

/// Enable EFER.NXE so PTE bit 63 acts as No-Execute. Without this the NX
/// bit is reserved and using it faults. Call before any NX-marked PTE
/// goes live (mem::init, ahead of the CR3 switch).
pub fn enable_nxe() {
    assert!(has_nxe(), "CPU lacks NX (EFER.NXE)");
    unsafe {
        let lo: u32;
        let hi: u32;
        core::arch::asm!(
            "rdmsr",
            in("ecx") MSR_EFER,
            out("eax") lo,
            out("edx") hi,
            options(nomem, nostack, preserves_flags),
        );
        let v = ((hi as u64) << 32) | lo as u64 | EFER_NXE;
        core::arch::asm!(
            "wrmsr",
            in("ecx") MSR_EFER,
            in("eax") v as u32,
            in("edx") (v >> 32) as u32,
            options(nomem, nostack, preserves_flags),
        );
    }
}

/// 10-byte pseudo-descriptor for lgdt/lidt.
#[repr(C, packed)]
struct DtPtr {
    limit: u16,
    base: u64,
}

pub fn lgdt(base: u64, limit: u16) {
    let ptr = DtPtr { limit, base };
    unsafe {
        asm!("lgdt [{}]", in(reg) &ptr, options(nostack, preserves_flags));
    }
}

pub fn lidt(base: u64, limit: u16) {
    let ptr = DtPtr { limit, base };
    unsafe {
        asm!("lidt [{}]", in(reg) &ptr, options(nostack, preserves_flags));
    }
}

/// Reload CS from our GDT (retfq trampoline), selector e.g. 0x08.
pub unsafe fn reload_cs(sel: u16) {
    unsafe {
        asm!(
            "push {sel}",
            "lea {tmp}, [rip + 2f]",
            "push {tmp}",
            "retfq",
            "2:",
            sel = in(reg) sel as u64,
            tmp = out(reg) _,
            options(nomem, preserves_flags),
        );
    }
}

/// Load task register with the TSS selector (e.g. 0x10).
pub unsafe fn load_tr(sel: u16) {
    unsafe {
        asm!("ltr ax", in("ax") sel, options(nomem, nostack, preserves_flags));
    }
}

/// Current PML4 physical address (CR3 bits 12..=51).
pub fn read_cr3() -> u64 {
    let v: u64;
    unsafe {
        asm!("mov {}, cr3", out(reg) v, options(nomem, nostack, preserves_flags));
    }
    v & 0x000F_FFFF_FFFF_F000
}

/// Switch address space. Flushes the entire TLB (no global pages used).
///
/// # Safety
///
/// `pml4` must point to a valid, mapped PML4 covering all memory the
/// kernel will touch after the switch (code, stack, framebuffer, heap).
pub unsafe fn write_cr3(pml4: u64) {
    unsafe {
        asm!("mov cr3, {}", in(reg) pml4, options(nostack, preserves_flags));
    }
}
