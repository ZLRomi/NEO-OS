//! IDT + 8259 PIC + PIT + int 0x80 syscalls, hand-rolled: CPU exceptions,
//! 100Hz timer (IRQ0), PS/2 keyboard (IRQ1). Gate layout per Intel SDM.
//!
//! M7: the syscall gate has DPL=3 (attr 0xEE) so ring3 can invoke it; its
//! entry is raw asm that spills the user's regs and calls [`do_syscall`].

use core::arch::global_asm;
use core::sync::atomic::{AtomicU64, Ordering};

use crate::cpu::{cli, hlt, inb, io_wait, lidt, outb, sti};
use crate::{gdt, kerror, kinfo, kprint, sinfo};

pub const TIMER_IRQ: u8 = 32;
pub const KEYBOARD_IRQ: u8 = 33;
pub const SERIAL_IRQ: u8 = 36; // COM1 on the master PIC
pub const MOUSE_IRQ: u8 = 44; // IRQ12 on the slave PIC
pub const SYSCALL_NR: u8 = 0x80;

/// CPU-pushed frame (no error code): RIP,CS,RFLAGS,RSP,SS low -> high.
#[repr(C)]
pub struct Frame {
    pub rip: u64,
    pub cs: u64,
    pub rflags: u64,
    pub rsp: u64,
    pub ss: u64,
}

#[derive(Clone, Copy)]
#[repr(C)]
struct Gate {
    off_lo: u16,
    sel: u16,
    ist: u8,
    attr: u8, // P=1, DPL=0, type=0xE (interrupt gate)
    off_mid: u16,
    off_hi: u32,
    zero: u32,
}

const NULL_GATE: Gate = Gate {
    off_lo: 0,
    sel: 0,
    ist: 0,
    attr: 0,
    off_mid: 0,
    off_hi: 0,
    zero: 0,
};

static mut IDT: [Gate; 256] = [NULL_GATE; 256];

fn set_gate(n: u8, handler: *const (), ist: u8) {
    set_gate_attr(n, handler, ist, 0x8E); // P=1, DPL=0, interrupt gate
}

/// Register a handler for a hardware IRQ (0-15). Maps to IDT entry IRQ+32.
pub fn register_irq_handler(irq: u8, handler: extern "x86-interrupt" fn(Frame)) {
    if irq < 16 {
        set_gate(32 + irq, handler as *const (), 0);
    }
}

/// DPL=3 variant for the syscall gate: callable from ring3.
fn set_gate_user(n: u8, handler: *const (), ist: u8) {
    set_gate_attr(n, handler, ist, 0xEE); // P=1, DPL=3, interrupt gate
}

fn set_gate_attr(n: u8, handler: *const (), ist: u8, attr: u8) {
    let addr = handler as u64;
    let g = Gate {
        off_lo: (addr & 0xFFFF) as u16,
        sel: gdt::CODE_SEL,
        ist: ist & 0x7,
        attr,
        off_mid: ((addr >> 16) & 0xFFFF) as u16,
        off_hi: ((addr >> 32) & 0xFFFF_FFFF) as u32,
        zero: 0,
    };
    unsafe { IDT[n as usize] = g };
}

static TICKS: AtomicU64 = AtomicU64::new(0);

/// Monotonic timer ticks since boot (PIT 100Hz). Lock-free for sleep/shell.
pub fn ticks() -> u64 {
    TICKS.load(Ordering::Relaxed)
}

pub fn uptime_secs() -> u64 {
    ticks() / 100
}

/// Sleep `ms` milliseconds (PIT 100Hz granularity, hlt to save power).
/// Call with interrupts enabled; timer IRQ wakes us.
pub fn sleep_ms(ms: u64) {
    let end = ticks().wrapping_add(ms.div_ceil(10));
    while ticks() < end {
        hlt();
    }
}

// ---- CPU exception handlers ----

/// Shared fatal path: visible over serial+screen, then halt.
fn exc_halt(name: &str, rip: u64, code: u64, has_code: bool) -> ! {
    if has_code {
        kerror!("EXC {} code={:#x} rip={:#x} -- halting", name, code, rip);
    } else {
        kerror!("EXC {} rip={:#x} -- halting", name, rip);
    }
    loop {
        hlt();
    }
}

extern "x86-interrupt" fn breakpoint_handler(frame: Frame) {
    kinfo!("EXC breakpoint rip={:#x}", frame.rip);
}

extern "x86-interrupt" fn de_handler(frame: Frame) -> ! {
    exc_halt("divide-error #DE", frame.rip, 0, false)
}

extern "x86-interrupt" fn db_handler(frame: Frame) -> ! {
    exc_halt("debug #DB", frame.rip, 0, false)
}

extern "x86-interrupt" fn nmi_handler(frame: Frame) -> ! {
    exc_halt("nmi", frame.rip, 0, false)
}

extern "x86-interrupt" fn of_handler(frame: Frame) -> ! {
    exc_halt("overflow #OF", frame.rip, 0, false)
}

extern "x86-interrupt" fn br_handler(frame: Frame) -> ! {
    exc_halt("bound-range #BR", frame.rip, 0, false)
}

extern "x86-interrupt" fn ud_handler(frame: Frame) -> ! {
    exc_halt("invalid-opcode #UD", frame.rip, 0, false)
}

extern "x86-interrupt" fn nm_handler(frame: Frame) -> ! {
    exc_halt("device-not-available #NM", frame.rip, 0, false)
}

extern "x86-interrupt" fn ts_handler(frame: Frame, code: u64) -> ! {
    exc_halt("invalid-tss #TS", frame.rip, code, true)
}

extern "x86-interrupt" fn np_handler(frame: Frame, code: u64) -> ! {
    exc_halt("segment-not-present #NP", frame.rip, code, true)
}

extern "x86-interrupt" fn ss_handler(frame: Frame, code: u64) -> ! {
    exc_halt("stack-fault #SS", frame.rip, code, true)
}

extern "x86-interrupt" fn mf_handler(frame: Frame) -> ! {
    exc_halt("x87-fpu #MF", frame.rip, 0, false)
}

extern "x86-interrupt" fn ac_handler(frame: Frame, code: u64) -> ! {
    exc_halt("alignment-check #AC", frame.rip, code, true)
}

extern "x86-interrupt" fn mc_handler(frame: Frame) -> ! {
    exc_halt("machine-check #MC", frame.rip, 0, false)
}

extern "x86-interrupt" fn xm_handler(frame: Frame) -> ! {
    exc_halt("simd #XM", frame.rip, 0, false)
}

extern "x86-interrupt" fn ve_handler(frame: Frame) -> ! {
    exc_halt("virtualization #VE", frame.rip, 0, false)
}

extern "x86-interrupt" fn cp_handler(frame: Frame, code: u64) -> ! {
    exc_halt("control-protection #CP", frame.rip, code, true)
}

extern "x86-interrupt" fn double_fault_handler(frame: Frame, _code: u64) -> ! {
    kerror!("EXC double fault rip={:#x} -- halting", frame.rip);
    loop {
        hlt();
    }
}

extern "x86-interrupt" fn page_fault_handler(frame: Frame, code: u64) {
    let addr: u64;
    unsafe {
        core::arch::asm!("mov {}, cr2", out(reg) addr, options(nomem, nostack, preserves_flags))
    };
    // Ring3 faults kill only the task (exit frees both stacks and switches
    // away from inside this handler, like a timer preemption). The shell
    // survives; kernel faults still halt everything.
    if frame.cs & 3 == 3 {
        let root = crate::task::current_root();
        if root != 0 && addr >= 0x400000 && addr < 0xA00000 {
            let is_write = (code & 2) != 0;
            let is_present = (code & 1) != 0;
            let page_addr = addr & !0xFFF;

            // COW fault: page is present, write attempted, COW bit set.
            if is_write && is_present {
                // Check if the PTE has the COW marker (bit 9).
                let pte = unsafe {
                    let mask = crate::mem::ADDR_MASK;
                    let pml4e = *crate::mem::table_ptr(root).add(((page_addr >> 39) & 0x1FF) as usize);
                    if pml4e & crate::mem::PTE_P == 0 { 0 }
                    else {
                        let pdpt_e = *crate::mem::table_ptr(pml4e & mask).add(((page_addr >> 30) & 0x1FF) as usize);
                        if pdpt_e & crate::mem::PTE_P == 0 || pdpt_e & crate::mem::PTE_PS != 0 { 0 }
                        else {
                            let pd_e = *crate::mem::table_ptr(pdpt_e & mask).add(((page_addr >> 21) & 0x1FF) as usize);
                            if pd_e & crate::mem::PTE_P == 0 || pd_e & crate::mem::PTE_PS != 0 { 0 }
                            else {
                                let pt = crate::mem::table_mut(pd_e & mask);
                                let idx = ((page_addr >> 12) & 0x1FF) as usize;
                                pt[idx]
                            }
                        }
                    }
                };

                if pte & crate::mem::PTE_COW != 0 {
                    // Clone the page: allocate new, copy old, remap as RW.
                    let old_phys = pte & crate::mem::ADDR_MASK;
                    let new_phys = unsafe { crate::mem::frames() }.alloc_contiguous(1);
                    if new_phys != 0 {
                        unsafe {
                            core::ptr::copy_nonoverlapping(
                                old_phys as *const u8,
                                new_phys as *mut u8,
                                4096,
                            );
                        }
                        let prot = crate::mem::PTE_P | crate::mem::PTE_W | crate::mem::PTE_U | crate::mem::PTE_NX;
                        crate::mem::replace_page(root, page_addr, new_phys, prot);
                        return; // retry instruction with writable copy
                    }
                }
            }

            // Demand paging: not-present fault in heap region.
            if !is_present && addr >= 0x800000 && addr < 0xA00000 {
                let phys = unsafe { crate::mem::frames() }.alloc_contiguous(1);
                if phys != 0 {
                    unsafe { core::ptr::write_bytes(phys as *mut u8, 0, 4096); }
                    let prot = crate::mem::PTE_P | crate::mem::PTE_W | crate::mem::PTE_U | crate::mem::PTE_NX;
                    crate::mem::replace_page(root, page_addr, phys, prot);
                    return;
                }
            }
        }

        kerror!(
            "user #PF: killing task addr={:#x} code={:#x} rip={:#x}",
            addr,
            code,
            frame.rip
        );
        crate::task::exit();
    }
    kerror!(
        "EXC page fault addr={:#x} code={:#x} rip={:#x} -- halting",
        addr,
        code,
        frame.rip
    );
    loop {
        hlt();
    }
}

extern "x86-interrupt" fn gp_handler(frame: Frame, code: u64) {
    if frame.cs & 3 == 3 {
        kerror!(
            "user #GP: killing task code={:#x} rip={:#x}",
            code,
            frame.rip
        );
        crate::task::exit();
    }
    kerror!(
        "EXC general-protection code={:#x} rip={:#x} -- halting",
        code,
        frame.rip
    );
    loop {
        hlt();
    }
}

// ---- IRQ handlers ----

fn eoi(irq: u8) {
    if irq >= 40 {
        outb(0xA0, 0x20);
    }
    outb(0x20, 0x20);
}

/// Spurious IRQ7: if master ISR bit7 is clear it's floating-bus noise --
/// must NOT EOI or we'd ACK a real IRQ. (Intel 8259A spec.)
extern "x86-interrupt" fn spurious7_handler(_frame: Frame) {
    outb(0x20, 0x0B); // OCW3: read ISR
    if inb(0x20) & 0x80 == 0 {
        return;
    }
    eoi(39);
}

/// Spurious IRQ15: slave noise -- EOI master only, not slave.
extern "x86-interrupt" fn spurious15_handler(_frame: Frame) {
    outb(0xA0, 0x0B);
    if inb(0xA0) & 0x80 == 0 {
        outb(0x20, 0x20);
        return;
    }
    eoi(47);
}

extern "x86-interrupt" fn timer_handler(_frame: Frame) {
    let t = TICKS.fetch_add(1, Ordering::Relaxed) + 1;
    // Serial-only heartbeat: the shell owns the screen; a screen print
    // here lands mid-input-line and looks like the user typed it.
    if t.is_multiple_of(1000) {
        sinfo!("tick {} ({}s uptime)", t, t / 100);
    }
    eoi(TIMER_IRQ);
    // Preempt on 2-tick (20ms) slices. IF is still clear here (interrupt
    // gate) and the PIC is already EOId, so switching is race- and
    // in-service-safe.
    if t.is_multiple_of(2) {
        // Promote sleepers whose deadline elapsed *before* choosing the next
        // task, otherwise a sleeping task would miss its own wakeup.
        crate::task::wake_expired();
        crate::task::schedule();
    }
}

extern "x86-interrupt" fn keyboard_handler(_frame: Frame) {
    // Drain kbd bytes; a pending aux byte (mouse moved at the same time)
    // is forwarded instead of eaten, keeping packet framing alive.
    for _ in 0..8 {
        let st = inb(0x64);
        if st & 0x01 == 0 {
            break;
        }
        let b = inb(0x60);
        if st & 0x20 != 0 {
            crate::mouse::on_byte(b);
        } else {
            crate::keyboard::on_scancode(b);
        }
    }
    eoi(KEYBOARD_IRQ);
}

extern "x86-interrupt" fn mouse_handler(_frame: Frame) {
    // Mirror of the above: stray kbd bytes go back to the keyboard.
    for _ in 0..8 {
        let st = inb(0x64);
        if st & 0x01 == 0 {
            break;
        }
        let b = inb(0x60);
        if st & 0x20 != 0 {
            crate::mouse::on_byte(b);
        } else {
            crate::keyboard::on_scancode(b);
        }
    }
    eoi(MOUSE_IRQ);
}

extern "x86-interrupt" fn serial_handler(_frame: Frame) {
    // COM1 received-data: drain the FIFO into the keyboard queue.
    for _ in 0..16 {
        if crate::cpu::inb(0x3F8 + 5) & 0x01 == 0 {
            break;
        }
        crate::serial::on_byte(crate::cpu::inb(0x3F8));
    }
    eoi(SERIAL_IRQ);
}

// ---- init ----

fn pic_init() {
    outb(0x20, 0x11);
    io_wait(); // ICW1
    outb(0xA0, 0x11);
    io_wait();
    outb(0x21, 32);
    io_wait(); // ICW2: vector offsets
    outb(0xA1, 40);
    io_wait();
    outb(0x21, 0x04);
    io_wait(); // ICW3: slave at IRQ2
    outb(0xA1, 0x02);
    io_wait();
    outb(0x21, 0x01);
    io_wait(); // ICW4: 8086 mode
    outb(0xA1, 0x01);
    io_wait();
    // Master IMR: unmask IRQ0 (PIT), IRQ1 (kbd), IRQ2 (cascade) and IRQ4
    // (serial). 0xEC was a bug: it left bit2 set, so IRQ2 stayed masked and
    // IRQ12 (mouse) could never reach the CPU. Stale aux bytes then clogged
    // the shared 8042 output queue and keyboard input died with it.
    outb(0x21, 0xE8);
    outb(0xA1, 0xEF); // slave: everything masked except IRQ12 (mouse)
}

fn pit_100hz() {
    let div: u32 = 1193182 / 100;
    outb(0x43, 0x36); // ch0, lo/hi latch, mode 3
    outb(0x40, (div & 0xFF) as u8);
    outb(0x40, (div >> 8) as u8);
}

fn keyboard_init() {
    for _ in 0..100_000 {
        if inb(0x64) & 0x02 == 0 {
            break;
        }
    }
    outb(0x64, 0xAE); // enable first PS/2 port
    for _ in 0..100_000 {
        if inb(0x64) & 0x02 == 0 {
            break;
        }
    }
    outb(0x64, 0x20); // read controller config
    let mut cfg = 0x01;
    for _ in 0..100_000 {
        if inb(0x64) & 0x01 != 0 {
            cfg = inb(0x60);
            break;
        }
    }
    cfg |= 0x41; // keyboard IRQ + set-1 translation
    cfg &= !0x10; // enable keyboard clock
    for _ in 0..100_000 {
        if inb(0x64) & 0x02 == 0 {
            break;
        }
    }
    outb(0x64, 0x60); // write controller config
    for _ in 0..100_000 {
        if inb(0x64) & 0x02 == 0 {
            break;
        }
    }
    outb(0x60, cfg);

    for _ in 0..100_000 {
        if inb(0x64) & 0x02 == 0 {
            break;
        }
    }
    outb(0x60, 0xF4); // enable keyboard scanning
    for _ in 0..100_000 {
        if inb(0x64) & 0x01 != 0 {
            let _ = inb(0x60); // discard ACK
            break;
        }
    }
}

pub fn init() {
    cli();
    set_gate(0, de_handler as *const (), 0);
    set_gate(1, db_handler as *const (), 0);
    set_gate(2, nmi_handler as *const (), 0);
    set_gate(3, breakpoint_handler as *const (), 0);
    set_gate(4, of_handler as *const (), 0);
    set_gate(5, br_handler as *const (), 0);
    set_gate(6, ud_handler as *const (), 0);
    set_gate(7, nm_handler as *const (), 0);
    set_gate(
        8,
        double_fault_handler as *const (),
        gdt::DOUBLE_FAULT_IST + 1,
    ); // field 1 = IST1
    set_gate(10, ts_handler as *const (), 0);
    set_gate(11, np_handler as *const (), 0);
    set_gate(12, ss_handler as *const (), 0);
    set_gate(13, gp_handler as *const (), 0);
    set_gate(14, page_fault_handler as *const (), 0);
    set_gate(16, mf_handler as *const (), 0);
    set_gate(17, ac_handler as *const (), 0);
    set_gate(18, mc_handler as *const (), 0);
    set_gate(19, xm_handler as *const (), 0);
    set_gate(20, ve_handler as *const (), 0);
    set_gate(21, cp_handler as *const (), 0);
    set_gate(TIMER_IRQ, timer_handler as *const (), 0);
    set_gate(KEYBOARD_IRQ, keyboard_handler as *const (), 0);
    set_gate(SERIAL_IRQ, serial_handler as *const (), 0);
    set_gate(MOUSE_IRQ, mouse_handler as *const (), 0);
    set_gate(39, spurious7_handler as *const (), 0);
    set_gate(47, spurious15_handler as *const (), 0);
    set_gate_user(SYSCALL_NR, __syscall_entry as *const (), 0); // int 0x80
    lidt(addr_of_idt(), (256 * 16 - 1) as u16);
    pic_init();
    keyboard_init();
    pit_100hz();
    sti();
}

// ---- int 0x80 syscalls ----

// Raw entry: spill the user regs used for args/nr, align to 16, call the
// Rust dispatcher, restore, iretq. Runs on the task's kstack (TSS.RSP0)
// with IF clear (interrupt gate). Only rax comes back (retval); the rest
// of the user regs are clobbered per the user ABI.
global_asm!(
    ".globl __syscall_entry",
    "__syscall_entry:",
    "push rax",
    "push rdi",
    "push rsi",
    "push rdx",
    "push rbp",
    "mov rbp, rsp",
    "and rsp, -16",
    "lea rdi, [rbp + 8]", // &SysRegs { rdx, rsi, rdi, rax }
    "call do_syscall",
    "mov rsp, rbp",
    "pop rbp",
    "pop rdx",
    "pop rsi",
    "pop rdi",
    "pop rax", // retval written back into this slot by do_syscall
    "iretq",
);

extern "C" {
    fn __syscall_entry();
}

/// User regs spilled by `__syscall_entry` (ascending addresses):
/// [rbp+8]=rdx (c), +16=rsi (b), +24=rdi (a), +32=rax (nr/ret).
#[repr(C)]
struct SysRegs {
    rdx: u64,
    rsi: u64,
    rdi: u64,
    rax: u64,
}

/// Syscall dispatcher (nr in rax): 0=write(fd ignored, buf, len),
/// 1=exit(code), 2=uptime()->ticks. Called with IF clear.
#[no_mangle]
extern "C" fn do_syscall(regs: &mut SysRegs) {
    match regs.rax {
        0 => {
            let len = regs.rdx.min(256) as usize;
            let buf = regs.rsi as u64;
            // Every page must be present and user-readable before the kernel
            // dereferences the pointer; supervisor-only pages are rejected.
            if len == 0 {
                regs.rax = 0;
                return;
            }
            if !crate::mem::user_range_ok(buf, len as u64) {
                regs.rax = !0u64;
                return;
            }
            let bytes =
                unsafe { core::slice::from_raw_parts(buf as *const u8, len) };
            let s = core::str::from_utf8(bytes).unwrap_or("<bad utf8>\n");
            kprint!("{}", s);
            regs.rax = regs.rdx;
        }
        1 => {
            // Never returns: marks the user task Exited, frees both stacks,
            // and switches away from inside this handler (same pattern as
            // timer preemption inside an IRQ).
            crate::task::exit();
        }
        2 => {
            regs.rax = ticks();
        }
        3 => {
            // sys_brk(new_brk)
            let new_brk = regs.rdi;
            regs.rax = crate::task::sys_brk(new_brk);
        }
        4 => {
            // sys_meminfo(buf, len): write memory stats to user buffer.
            // Returns bytes written or !0 on error.
            let buf = regs.rdi;
            let len = regs.rsi as usize;
            if len == 0 { regs.rax = 0; return; }
            if !crate::mem::user_range_ok(buf, len as u64) {
                regs.rax = !0u64;
                return;
            }
            let used = unsafe { crate::mem::frames() }.used_bytes();
            let total = unsafe { crate::mem::frames() }.total_bytes();
            // Format: "mem: used=X total=Y\n"
            let mut msg = [0u8; 128];
            let mut pos = 0;
            let prefix = b"mem: used=";
            msg[pos..pos+prefix.len()].copy_from_slice(prefix);
            pos += prefix.len();
            // Simple u64 to decimal.
            let mut digits = [0u8; 20];
            let mut d = 0;
            let mut v = used / 1024; // KiB
            if v == 0 { msg[pos] = b'0'; pos += 1; }
            else {
                while v > 0 { digits[d] = (v % 10) as u8 + b'0'; d += 1; v /= 10; }
                for i in (0..d).rev() { msg[pos] = digits[i]; pos += 1; }
            }
            let kb = b"KiB total=";
            msg[pos..pos+kb.len()].copy_from_slice(kb);
            pos += kb.len();
            v = total / 1024;
            d = 0;
            if v == 0 { msg[pos] = b'0'; pos += 1; }
            else {
                while v > 0 { digits[d] = (v % 10) as u8 + b'0'; d += 1; v /= 10; }
                for i in (0..d).rev() { msg[pos] = digits[i]; pos += 1; }
            }
            let suffix = b"KiB\n";
            msg[pos..pos+suffix.len()].copy_from_slice(suffix);
            pos += suffix.len();
            let copy_len = core::cmp::min(pos, len);
            unsafe {
                core::ptr::copy_nonoverlapping(msg.as_ptr(), buf as *mut u8, copy_len);
            }
            regs.rax = copy_len as u64;
        }
        5 => {
            // sys_read_char(): read one character from keyboard buffer.
            // Returns the ASCII char, or 0 if no input available.
            match crate::keyboard::pop() {
                Some(ch) => regs.rax = ch as u64,
                None => regs.rax = 0,
            }
        }
        6 => {
            // sys_sleep(ticks): block the task for N ticks (1 tick = 10ms).
            // The old busy-wait was a guaranteed deadlock: this gate clears
            // IF, so `ticks()` could never advance while the loop spun.
            crate::task::sleep_current(regs.rdi);
            regs.rax = 0;
        }
        7 => {
            // sys_print_int(val): print signed integer to console+serial.
            let val = regs.rdi as i64;
            kprint!("{}", val);
            regs.rax = 0;
        }
        8 => {
            // sys_net_send(buf, len): send raw packet through network device.
            let buf = regs.rdi;
            let len = regs.rsi as usize;
            if len == 0 { regs.rax = 0; return; }
            if !crate::mem::user_range_ok(buf, len as u64) {
                regs.rax = !0u64;
                return;
            }
            let data = unsafe { core::slice::from_raw_parts(buf as *const u8, len) };
            match crate::virtio_net::send(data) {
                Ok(()) => regs.rax = 0,
                Err(_) => regs.rax = !0u64,
            }
        }
        9 => {
            // sys_net_recv(buf, len): receive a packet into user buffer.
            // Returns bytes received, or 0 if no packet available.
            let buf = regs.rdi;
            let len = regs.rsi as usize;
            if len == 0 { regs.rax = 0; return; }
            if !crate::mem::user_range_ok(buf, len as u64) {
                regs.rax = !0u64;
                return;
            }
            match crate::virtio_net::poll() {
                Some((pkt, _buf_id)) => {
                    let copy_len = core::cmp::min(pkt.len(), len);
                    unsafe {
                        core::ptr::copy_nonoverlapping(pkt.as_ptr(), buf as *mut u8, copy_len);
                    }
                    regs.rax = copy_len as u64;
                }
                None => regs.rax = 0,
            }
        }
        10 => {
            // sys_tcp_send(buf, len): send TCP data over established connection.
            let buf = regs.rdi;
            let len = regs.rsi as usize;
            if len == 0 { regs.rax = 0; return; }
            if !crate::mem::user_range_ok(buf, len as u64) {
                regs.rax = !0u64;
                return;
            }
            let data = unsafe { core::slice::from_raw_parts(buf as *const u8, len) };
            match crate::tcp::send_data(data) {
                Ok(()) => regs.rax = 0,
                Err(_) => regs.rax = !0u64,
            }
        }
        11 => {
            // sys_open(name_ptr, name_len, flags) -> fd or negative error.
            // flags: 0=read, 1=write(create/trunc), 2=rw.
            let name_ptr = regs.rdi;
            let name_len = regs.rsi as usize;
            let flags = regs.rdx as u32;
            if name_len == 0 || name_len > 16 {
                regs.rax = (-1i64) as u64;
                return;
            }
            if !crate::mem::user_range_ok(name_ptr, name_len as u64) {
                regs.rax = (-1i64) as u64;
                return;
            }
            let name = unsafe { core::slice::from_raw_parts(name_ptr as *const u8, name_len) };
            let task_id = crate::task::current_id() as u8;
            let fd = crate::vfs::vfs_open(name, flags, task_id);
            regs.rax = fd as i64 as u64;
        }
        12 => {
            // sys_close(fd) -> 0 or negative error.
            let fd = regs.rdi as u32;
            let task_id = crate::task::current_id() as u8;
            let ret = crate::vfs::vfs_close(fd, task_id);
            regs.rax = ret as i64 as u64;
        }
        13 => {
            // sys_read(fd, buf, len) -> bytes read or negative error.
            let fd = regs.rdi as u32;
            let buf = regs.rsi;
            let len = regs.rdx as usize;
            if len == 0 { regs.rax = 0; return; }
            if !crate::mem::user_range_ok(buf, len as u64) {
                regs.rax = (-1i64) as u64;
                return;
            }
            let task_id = crate::task::current_id() as u8;
            let slice = unsafe { core::slice::from_raw_parts_mut(buf as *mut u8, len) };
            let ret = crate::vfs::vfs_read(fd, slice, task_id);
            regs.rax = ret as i64 as u64;
        }
        14 => {
            // sys_write_fd(fd, buf, len) -> bytes written or negative error.
            let fd = regs.rdi as u32;
            let buf = regs.rsi;
            let len = regs.rdx as usize;
            if len == 0 { regs.rax = 0; return; }
            if !crate::mem::user_range_ok(buf, len as u64) {
                regs.rax = (-1i64) as u64;
                return;
            }
            let task_id = crate::task::current_id() as u8;
            let slice = unsafe { core::slice::from_raw_parts(buf as *const u8, len) };
            let ret = crate::vfs::vfs_write(fd, slice, task_id);
            regs.rax = ret as i64 as u64;
        }
        15 => {
            // sys_stat(name_ptr, name_len, stat_buf) -> 0 or negative error.
            let name_ptr = regs.rdi;
            let name_len = regs.rsi as usize;
            let stat_buf = regs.rdx;
            if name_len == 0 || name_len > 16 {
                regs.rax = (-1i64) as u64;
                return;
            }
            if !crate::mem::user_range_ok(name_ptr, name_len as u64) {
                regs.rax = (-1i64) as u64;
                return;
            }
            if !crate::mem::user_range_ok(stat_buf, 16) {
                regs.rax = (-1i64) as u64;
                return;
            }
            let name = unsafe { core::slice::from_raw_parts(name_ptr as *const u8, name_len) };
            let mut stat = crate::vfs::Stat { ftype: 0, size: 0, inode: 0, _pad: 0 };
            let ret = crate::vfs::vfs_stat(name, &mut stat);
            if ret == 0 {
                unsafe {
                    core::ptr::write(stat_buf as *mut crate::vfs::Stat, stat);
                }
            }
            regs.rax = ret as i64 as u64;
        }
        16 => {
            // sys_tcp_recv(buf, len) -> bytes received or 0 if none.
            let buf = regs.rdi;
            let len = regs.rsi as usize;
            if len == 0 { regs.rax = 0; return; }
            if !crate::mem::user_range_ok(buf, len as u64) {
                regs.rax = !0u64;
                return;
            }
            let slice = unsafe { core::slice::from_raw_parts_mut(buf as *mut u8, len) };
            let n = crate::tcp::recv(slice);
            regs.rax = n as u64;
        }
        17 => {
            // sys_readdir(buf, len) -> bytes written (serialized directory listing).
            let buf = regs.rdi;
            let len = regs.rsi as usize;
            if len < 4 { regs.rax = 0; return; }
            if !crate::mem::user_range_ok(buf, len as u64) {
                regs.rax = !0u64;
                return;
            }
            let slice = unsafe { core::slice::from_raw_parts_mut(buf as *mut u8, len) };
            let n = crate::vfs::vfs_readdir_serialize(slice);
            regs.rax = n as u64;
        }
        18 => {
            // sys_ftruncate(name_ptr, name_len, new_size) -> 0 or negative error.
            let name_ptr = regs.rdi;
            let name_len = regs.rsi as usize;
            let new_size = regs.rdx as usize;
            if name_len == 0 || name_len > 16 {
                regs.rax = (-1i64) as u64;
                return;
            }
            if !crate::mem::user_range_ok(name_ptr, name_len as u64) {
                regs.rax = (-1i64) as u64;
                return;
            }
            let name = unsafe { core::slice::from_raw_parts(name_ptr as *const u8, name_len) };
            match crate::neofs::find_inode(name) {
                Some(ino) => {
                    match crate::neofs::truncate_to_size(ino, new_size) {
                        Ok(()) => regs.rax = 0,
                        Err(_) => regs.rax = (-2i64) as u64,
                    }
                }
                None => regs.rax = (-1i64) as u64,
            }
        }
        19 => {
            // sys_getpid() -> current task ID.
            regs.rax = crate::task::current_id();
        }
        20 => {
            // sys_tasklist(buf, len) -> bytes written (serialized task list).
            let buf = regs.rdi;
            let len = regs.rsi as usize;
            if len < 4 { regs.rax = 0; return; }
            if !crate::mem::user_range_ok(buf, len as u64) {
                regs.rax = !0u64;
                return;
            }
            let slice = unsafe { core::slice::from_raw_parts_mut(buf as *mut u8, len) };
            let n = crate::task::serialize_tasks(slice);
            regs.rax = n as u64;
        }
        21 => {
            // sys_block_read(sector, buf, len) -> 0 on success, !0 on error.
            let sector = regs.rdi;
            let buf = regs.rsi;
            let len = regs.rdx as usize;
            if len == 0 || len % 512 != 0 { regs.rax = !0u64; return; }
            if !crate::mem::user_range_ok(buf, len as u64) {
                regs.rax = !0u64;
                return;
            }
            let slice = unsafe { core::slice::from_raw_parts_mut(buf as *mut u8, len) };
            match crate::block::read(0, sector, slice) {
                Ok(()) => regs.rax = 0,
                Err(_) => regs.rax = !0u64,
            }
        }
        22 => {
            // sys_block_write(sector, buf, len) -> 0 on success, !0 on error.
            let sector = regs.rdi;
            let buf = regs.rsi;
            let len = regs.rdx as usize;
            if len == 0 || len % 512 != 0 { regs.rax = !0u64; return; }
            if !crate::mem::user_range_ok(buf, len as u64) {
                regs.rax = !0u64;
                return;
            }
            let slice = unsafe { core::slice::from_raw_parts(buf as *const u8, len) };
            match crate::block::write(0, sector, slice) {
                Ok(()) => regs.rax = 0,
                Err(_) => regs.rax = !0u64,
            }
        }
        23 => {
            // sys_mkdir(name_ptr, name_len) -> 0 or negative error.
            let name_ptr = regs.rdi;
            let name_len = regs.rsi as usize;
            if name_len == 0 || name_len > 16 {
                regs.rax = (-1i64) as u64;
                return;
            }
            if !crate::mem::user_range_ok(name_ptr, name_len as u64) {
                regs.rax = (-1i64) as u64;
                return;
            }
            let name = unsafe { core::slice::from_raw_parts(name_ptr as *const u8, name_len) };
            match crate::neofs::create_dir(name, 0) {
                Some(_) => regs.rax = 0,
                None => regs.rax = (-2i64) as u64,
            }
        }
        24 => {
            // sys_unlink(name_ptr, name_len) -> 0 or negative error.
            let name_ptr = regs.rdi;
            let name_len = regs.rsi as usize;
            if name_len == 0 || name_len > 16 {
                regs.rax = (-1i64) as u64;
                return;
            }
            if !crate::mem::user_range_ok(name_ptr, name_len as u64) {
                regs.rax = (-1i64) as u64;
                return;
            }
            let name = unsafe { core::slice::from_raw_parts(name_ptr as *const u8, name_len) };
            match crate::neofs::remove_file(name) {
                Ok(()) => regs.rax = 0,
                Err(_) => regs.rax = (-2i64) as u64,
            }
        }
        25 => {
            // sys_udp_send(buf, len, dst_port) -> 0 or !0.
            // dst_ip is always gateway 10.0.2.2 for simplicity.
            let buf_ptr = regs.rdi;
            let buf_len = regs.rsi as usize;
            let dport = regs.rdx as u16;
            if buf_len == 0 || buf_len > 512 { regs.rax = !0u64; return; }
            if !crate::mem::user_range_ok(buf_ptr, buf_len as u64) {
                regs.rax = !0u64;
                return;
            }
            let data = unsafe { core::slice::from_raw_parts(buf_ptr as *const u8, buf_len) };
            let src_ip = [10, 0, 2, 15];
            let dst_ip = [10, 0, 2, 2]; // QEMU gateway
            let src_port = 49152; // ephemeral port
            let mut pkt = [0u8; 600];
            let plen = crate::ip::build_udp_packet(
                &mut pkt,
                [0xFF; 6], // broadcast MAC
                [0x52, 0x54, 0x00, 0x12, 0x34, 0x56],
                src_ip, dst_ip,
                src_port, dport,
                data,
            );
            if plen == 0 { regs.rax = !0u64; return; }
            match crate::virtio_net::send(&pkt[..plen]) {
                Ok(()) => regs.rax = 0,
                Err(_) => regs.rax = !0u64,
            }
        }
        26 => {
            // sys_mprotect(addr, len, prot) -> 0 or !0.
            // prot: 1=READ, 2=WRITE, 4=EXEC.
            let addr = regs.rdi;
            let len = regs.rsi;
            let prot = regs.rdx as u8;
            // Get current task's page table root.
            let root = crate::task::current_root();
            if root == 0 {
                regs.rax = !0u64; // kernel task, no user page table
                return;
            }
            if crate::mem::user_mprotect(root, addr, len, prot) {
                regs.rax = 0;
            } else {
                regs.rax = !0u64;
            }
        }
        27 => {
            // sys_mmap - STUB: returns error until per-task VA allocator is ready.
            regs.rax = !0u64;
        }
        28 => {
            // sys_munmap(addr, len) -> 0 or !0.
            let addr = regs.rdi;
            let len = regs.rsi;
            let root = crate::task::current_root();
            if root == 0 || len == 0 {
                regs.rax = !0u64;
                return;
            }
            if crate::mem::user_munmap(root, addr, len) {
                regs.rax = 0;
            } else {
                regs.rax = !0u64;
            }
        }
        29 => {
            // sys_fork() -> child_pid in parent, 0 in child (not yet implemented fully).
            // Returns child task ID or !0 on failure.
            match crate::task::fork_current() {
                Some(child_id) => regs.rax = child_id,
                None => regs.rax = !0u64,
            }
        }
        30 => {
            // sys_waitpid(pid) -> child_id or 0 on timeout.
            let pid = regs.rdi;
            match crate::task::waitpid(pid) {
                Some(id) => regs.rax = id,
                None => regs.rax = 0,
            }
        }
        31 => {
            // sys_getppid() -> parent task ID.
            regs.rax = crate::task::current_parent_id();
        }
        32 => {
            // sys_kill(pid, sig) -> 0 on success, negative on error.
            // pid > 0: send to specific task
            // pid < 0: send to all tasks in process group |pid|
            let pid = regs.rdi as i64;
            let sig = regs.rsi as u32;
            if pid > 0 {
                let ret = crate::task::kill_task(pid as u64, sig);
                regs.rax = ret as i64 as u64;
            } else if pid < 0 {
                let pgid = (-pid) as u64;
                let ret = crate::task::kill_pgid(pgid, sig);
                regs.rax = ret as i64 as u64;
            } else {
                regs.rax = (-1i64) as u64; // pid=0 not supported
            }
        }
        33 => {
            // sys_getpgid() -> process group ID of current task.
            regs.rax = crate::task::current_pgid();
        }
        34 => {
            // sys_exec(name_ptr, name_len) -> !0 on failure (success replaces image).
            let name_ptr = regs.rdi;
            let name_len = regs.rsi as usize;
            if name_len == 0 || name_len > 16 {
                regs.rax = !0u64;
                return;
            }
            if !crate::mem::user_range_ok(name_ptr, name_len as u64) {
                regs.rax = !0u64;
                return;
            }
            let name = unsafe { core::slice::from_raw_parts(name_ptr as *const u8, name_len) };
            if crate::task::exec_current(name) {
                // exec succeeded — new image is set up in rsp_slot.
                // Force a reschedule so the new image runs.
                regs.rax = 0;
            } else {
                regs.rax = !0u64;
            }
        }
        35 => {
            // sys_rmdir(name_ptr, name_len) -> 0 or negative error.
            let name_ptr = regs.rdi;
            let name_len = regs.rsi as usize;
            if name_len == 0 || name_len > 16 {
                regs.rax = (-1i64) as u64;
                return;
            }
            if !crate::mem::user_range_ok(name_ptr, name_len as u64) {
                regs.rax = (-1i64) as u64;
                return;
            }
            let name = unsafe { core::slice::from_raw_parts(name_ptr as *const u8, name_len) };
            match crate::neofs::remove_dir(name) {
                Ok(()) => regs.rax = 0,
                Err(_) => regs.rax = (-2i64) as u64,
            }
        }
        _ => {
            regs.rax = !0u64;
        }
    }
}

fn addr_of_idt() -> u64 {
    core::ptr::addr_of!(IDT) as u64
}

/// Load the IDT on the current CPU. Called by APs during SMP startup.
pub fn load_idt() {
    unsafe {
        crate::cpu::lidt(addr_of_idt(), (256 * 16 - 1) as u16);
    }
}
