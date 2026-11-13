//! NEO-OS-3 SMP: Application Processor startup and per-CPU data.
//!
//! The BSP sends INIT + SIPI to wake APs. Each AP starts in real mode at
//! the SIPI vector page (0x8000), switches through protected mode to long
//! mode, and enters `ap_entry()`.
//!
//! Per-CPU data is stored in a fixed array indexed by APIC ID.

use core::sync::atomic::{AtomicU32, AtomicBool, Ordering};
use crate::cpu;

/// Number of APs that have checked in.
static AP_COUNT: AtomicU32 = AtomicU32::new(0);
/// Flag: BSP has finished SMP init.
static SMP_READY: AtomicBool = AtomicBool::new(false);

/// Trampoline code page address (must be < 1MiB for SIPI).
const TRAMPOLINE_ADDR: u64 = 0x8000;

// ---- Per-CPU data ----

/// Maximum CPUs supported (including BSP).
pub const MAX_CPUS: usize = 16;

/// Per-CPU state, indexed by APIC ID.
#[repr(C)]
pub struct CpuData {
    /// APIC ID of this CPU.
    pub apic_id: u32,
    /// Current task index running on this CPU (usize::MAX = idle).
    pub current_task: usize,
    /// Per-CPU tick counter (incremented by PIT IRQ on this CPU).
    pub ticks: u64,
    /// Whether this CPU is online.
    pub online: bool,
    /// Per-CPU runqueue: bitmap of ready task indices assigned to this CPU.
    /// Bit N set = task N is in this CPU's runqueue.
    pub runqueue: u16, // supports up to 16 tasks
}

static mut CPU_DATA: [CpuData; MAX_CPUS] = {
    const EMPTY: CpuData = CpuData {
        apic_id: 0,
        current_task: usize::MAX,
        ticks: 0,
        online: false,
        runqueue: 0,
    };
    [EMPTY; MAX_CPUS]
};

/// Initialize per-CPU data for the BSP.
pub fn init_bsp_cpu_data() {
    let apic_id = read_apic_id();
    unsafe {
        CPU_DATA[apic_id as usize].apic_id = apic_id;
        CPU_DATA[apic_id as usize].current_task = 0; // BSP runs task 0 (shell)
        CPU_DATA[apic_id as usize].online = true;
    }
}

/// Initialize per-CPU data for an AP. Called from ap_entry.
pub fn init_ap_cpu_data(apic_id: u32) {
    unsafe {
        CPU_DATA[apic_id as usize].apic_id = apic_id;
        CPU_DATA[apic_id as usize].current_task = usize::MAX; // idle
        CPU_DATA[apic_id as usize].online = true;
    }
}

/// Get the current CPU's APIC ID.
pub fn read_apic_id() -> u32 {
    unsafe {
        core::ptr::read_volatile(0xFEE00020u64 as *const u32) >> 24
    }
}

// ---- Per-CPU Runqueue API ----

/// Add a task to a specific CPU's runqueue.
pub fn enqueue_task(cpu_id: usize, task_idx: usize) {
    if cpu_id >= MAX_CPUS || task_idx >= 16 { return; }
    unsafe {
        CPU_DATA[cpu_id].runqueue |= 1 << task_idx;
    }
}

/// Remove a task from a specific CPU's runqueue.
pub fn dequeue_task(cpu_id: usize, task_idx: usize) {
    if cpu_id >= MAX_CPUS || task_idx >= 16 { return; }
    unsafe {
        CPU_DATA[cpu_id].runqueue &= !(1 << task_idx);
    }
}

/// Pop the next ready task from a CPU's runqueue. Returns task index or None.
pub fn pop_runqueue(cpu_id: usize) -> Option<usize> {
    if cpu_id >= MAX_CPUS { return None; }
    unsafe {
        let rq = CPU_DATA[cpu_id].runqueue;
        if rq == 0 { return None; }
        // Find lowest set bit (first ready task).
        let bit = rq.trailing_zeros() as usize;
        CPU_DATA[cpu_id].runqueue &= !(1 << bit);
        Some(bit)
    }
}

/// Steal a task from another CPU's runqueue (work stealing).
/// Tries all other online CPUs and steals from the one with the most tasks.
pub fn steal_task(my_cpu: usize) -> Option<usize> {
    unsafe {
        let mut best_cpu = None;
        let mut best_count = 0u32;
        for i in 0..MAX_CPUS {
            if i == my_cpu || !CPU_DATA[i].online { continue; }
            let count = CPU_DATA[i].runqueue.count_ones();
            if count > best_count && count > 1 { // only steal if donor has >1 task
                best_count = count;
                best_cpu = Some(i);
            }
        }
        if let Some(donor) = best_cpu {
            // Steal the highest-priority (lowest index) task from donor.
            let rq = CPU_DATA[donor].runqueue;
            if rq != 0 {
                let bit = rq.trailing_zeros() as usize;
                CPU_DATA[donor].runqueue &= !(1 << bit);
                return Some(bit);
            }
        }
    }
    None
}

/// Get the number of tasks in a CPU's runqueue.
pub fn runqueue_len(cpu_id: usize) -> u32 {
    if cpu_id >= MAX_CPUS { return 0; }
    unsafe { CPU_DATA[cpu_id].runqueue.count_ones() }
}

/// Get a reference to the current CPU's data.
/// Safety: caller must ensure interrupts are disabled or preemption is off.
pub fn current_cpu() -> &'static mut CpuData {
    let id = read_apic_id() as usize;
    unsafe { &mut CPU_DATA[id] }
}

/// Get current CPU's APIC ID (fast path, no volatile read if cached).
pub fn current_cpu_id() -> u32 {
    read_apic_id()
}

/// Maximum number of APs we support.
pub const MAX_APS: usize = 16;

/// Get the number of APs that successfully started.
pub fn ap_count() -> u32 {
    AP_COUNT.load(Ordering::SeqCst)
}

/// Check if SMP initialization is complete.
pub fn is_ready() -> bool {
    SMP_READY.load(Ordering::SeqCst)
}

/// Raw trampoline machine code, assembled by nasm from ap_trampoline.asm.
static TRAMPOLINE_BYTES: &[u8] = include_bytes!("ap_trampoline.bin");

/// Patch offsets in the trampoline binary.
const PATCH_CR3_OFFSET: usize = 0xF0;          // label PATCH_CR3 (read by mov eax, [0x80F0])
const PATCH_STACK_BASE_OFFSET: usize = 0x69;   // movabs rax imm64: AP stacks base address
const PATCH_STACK_SIZE_OFFSET: usize = 0x75;   // movabs rcx imm64: per-AP stack size
const PATCH_ENTRY_OFFSET: usize = 0x84;        // movabs rax imm64: ap_entry address

/// Per-AP stacks (8 KiB each).
static mut AP_STACKS: [[u8; 8192]; MAX_APS] = [[0; 8192]; MAX_APS];
const AP_STACK_SIZE: u64 = 8192;

/// Entry point for Application Processors.
/// Called from trampoline with APIC ID in edi (passed as first arg).
#[no_mangle]
pub extern "C" fn ap_entry(ap_id: u32) -> ! {
    AP_COUNT.fetch_add(1, Ordering::SeqCst);

    // Initialize per-CPU data for this AP.
    init_ap_cpu_data(ap_id);

    // Load the kernel GDT (shared across all cores).
    unsafe {
        crate::cpu::lgdt(
            core::ptr::addr_of!(crate::gdt::GDT) as u64,
            (7 * 8 - 1) as u16,
        );
        crate::cpu::reload_cs(crate::gdt::CODE_SEL);
        core::arch::asm!(
            "xor ax, ax",
            "mov ds, ax",
            "mov es, ax",
            options(nomem, nostack, preserves_flags),
        );
    }

    // Load the kernel IDT (shared).
    unsafe {
        crate::interrupts::load_idt();
    }

    // Enable interrupts on this AP.
    crate::cpu::sti();

    // Signal that this AP is online.
    let msg = b"AP online\r\n";
    for &b in msg.iter() {
        unsafe {
            core::arch::asm!("out dx, al", in("dx") 0x3f8u16, in("al") b, options(nostack));
        }
    }

    // Enter idle loop. AP scheduling requires per-CPU timer (task #4 prerequisite).
    // For now, just hlt — APs are online and ready for IPI wake-up.
    loop {
        crate::cpu::hlt();
    }
}

/// Initialize SMP: install trampoline, patch addresses, send INIT+SIPI.
pub fn init() {
    // Initialize BSP per-CPU data.
    let bsp_apic_id = read_apic_id();
    init_bsp_cpu_data();
    crate::kinfo!("smp: BSP APIC ID={}", bsp_apic_id);

    // Copy trampoline to 0x8000.
    unsafe {
        core::ptr::copy_nonoverlapping(
            TRAMPOLINE_BYTES.as_ptr(),
            TRAMPOLINE_ADDR as *mut u8,
            TRAMPOLINE_BYTES.len(),
        );
    }

    // Patch CR3 (kernel's PML4).
    let pml4 = crate::mem::kernel_pml4();
    unsafe {
        let dst = (TRAMPOLINE_ADDR + PATCH_CR3_OFFSET as u64) as *mut u32;
        core::ptr::write_unaligned(dst, pml4 as u32);
    }

    // Patch AP stack base and per-AP stack size.
    // Trampoline calculates: rsp = stack_base + (apic_id + 1) * stack_size
    let stack_base: u64 = unsafe { &AP_STACKS as *const _ as u64 };
    unsafe {
        let dst = (TRAMPOLINE_ADDR + PATCH_STACK_BASE_OFFSET as u64) as *mut u64;
        core::ptr::write_unaligned(dst, stack_base);
        let dst2 = (TRAMPOLINE_ADDR + PATCH_STACK_SIZE_OFFSET as u64) as *mut u64;
        core::ptr::write_unaligned(dst2, AP_STACK_SIZE);
    }

    // Patch ap_entry address.
    let entry_addr = ap_entry as *const () as u64;
    unsafe {
        let dst = (TRAMPOLINE_ADDR + PATCH_ENTRY_OFFSET as u64) as *mut u64;
        core::ptr::write_unaligned(dst, entry_addr);
    }

    // Send INIT + SIPI.
    let apic_base: u64 = 0xFEE00000;
    unsafe {
        core::ptr::write_volatile((apic_base + 0x300) as *mut u32, 0x000C4500);
        cpu::delay_ms(10);
        core::ptr::write_volatile((apic_base + 0x300) as *mut u32, 0x000C4608);
        cpu::delay_ms(1);
        core::ptr::write_volatile((apic_base + 0x300) as *mut u32, 0x000C4608);
        cpu::delay_ms(1);
    }

    // Wait up to 100ms for APs.
    for _ in 0..100 {
        cpu::delay_ms(1);
        if AP_COUNT.load(Ordering::SeqCst) > 0 { break; }
    }

    SMP_READY.store(true, Ordering::SeqCst);
    crate::kinfo!("smp: {} AP(s) started", AP_COUNT.load(Ordering::SeqCst));
}
