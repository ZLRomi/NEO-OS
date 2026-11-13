//! GDT + TSS, hand-rolled: null / kcode / kdata / ucode / udata / TSS
//! descriptors, plus a 4K double-fault IST stack. Layouts per Intel SDM.
//!
//! M7 adds ring3: the user segments (DPL3) and TSS.RSP0 -- the stack the
//! CPU loads on every ring3 -> ring0 transition (int 0x80, IRQs, faults).

use core::arch::asm;
use core::ptr::addr_of_mut;
use spin::Once;

use crate::cpu::{lgdt, load_tr, reload_cs};

pub const CODE_SEL: u16 = 0x08; // kernel code
pub const TSS_SEL: u16 = 0x28; // TSS (lo at index 5)
pub const USER_CS: u16 = 0x1B; // user code   | RPL3
pub const USER_SS: u16 = 0x23; // user data   | RPL3
pub const DOUBLE_FAULT_IST: u8 = 0; // -> IST1

/// 64-bit code segments (base 0, limit 4G, L=1); data uses G=1.
const KCODE: u64 = 0x00AF9B000000FFFF;
const KDATA: u64 = 0x00CF93000000FFFF;
/// Same shapes with DPL=3 (bits 45-46 set in byte 5).
const UCODE: u64 = 0x00AFFB000000FFFF;
const UDATA: u64 = 0x00CFF3000000FFFF;

pub static mut GDT: [u64; 7] = [0, KCODE, KDATA, UCODE, UDATA, 0, 0];
static mut TSS: [u8; 104] = [0; 104];
static mut DF_STACK: [u8; 4096] = [0; 4096];

static READY: Once<()> = Once::new();

fn tss_desc_lo(base: u64) -> u64 {
    let limit: u64 = 103; // sizeof(TSS)-1
    let mut d: u64 = 0;
    d |= limit & 0xFFFF; // limit 0:15
    d |= (base & 0xFFFF) << 16; // base 0:15
    d |= ((base >> 16) & 0xFF) << 32; // base 16:23
    d |= 0x89 << 40; // type=9 (available 64-bit TSS), S=0, P=1
    d |= ((limit >> 16) & 0xF) << 48; // limit 16:19
    d |= ((base >> 24) & 0xFF) << 56; // base 24:31
    d
}

/// TSS.RSP0 (offset 4): kernel stack top for ring3 -> ring0 entries.
/// Updated by the scheduler on every switch into a user task.
pub fn set_rsp0(top: u64) {
    unsafe {
        TSS[4..12].copy_from_slice(&top.to_le_bytes());
    }
}

pub fn init() {
    READY.call_once(|| unsafe {
        // IST1 (index 0) -> top of the double-fault stack.
        let top = addr_of_mut!(DF_STACK) as u64 + 4096;
        TSS[36..44].copy_from_slice(&top.to_le_bytes());
        // I/O map base past the end = empty map (all ports allowed).
        TSS[102] = 104u8;
        TSS[103] = 0;

        let base = addr_of_mut!(TSS) as u64;
        GDT[5] = tss_desc_lo(base);
        GDT[6] = base >> 32; // base 32:63

        lgdt(addr_of_mut!(GDT) as u64, (7 * 8 - 1) as u16);
        reload_cs(CODE_SEL);
        // Drop stale firmware selectors: indices outside our GDT would #GP
        // on the first iretq restoring SS. Null data selectors are legal in
        // 64-bit mode.
        asm!(
            "xor ax, ax",
            "mov ds, ax",
            "mov es, ax",
            "mov ss, ax",
            options(nomem, nostack, preserves_flags),
        );
        load_tr(TSS_SEL);
    });
}
