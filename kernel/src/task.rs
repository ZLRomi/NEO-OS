//! M6/M7 kernel tasks: fixed 16-slot table, kernel + user stacks carved
//! from the frame allocator, timer-driven round-robin preemption (20ms
//! slices).
//!
//! Single CPU. Context switch stores just rsp: callee-saved regs and any
//! in-flight interrupt frame live on the task's own stack and unwind
//! naturally on resume. Switching happens inside the timer IRQ after EOI
//! with IF clear, so the PIC is never left in-service. On exit the task
//! returns its stacks to the allocator and its slot becomes reusable.
//!
//! M7: `spawn_user` tasks enter ring3 via an iretq frame (trampoline);
//! from then on every ring3 -> ring0 entry lands on the task's kernel
//! stack through TSS.RSP0, which the scheduler refreshes on switch-in.

use core::arch::global_asm;
use core::sync::atomic::{AtomicBool, AtomicU64, Ordering};

use crate::cpu::{cli, hlt, interrupts_enabled, sti};
use crate::gdt::{USER_CS, USER_SS};
use crate::interrupts::{sleep_ms, ticks};
use crate::{kinfo, kprint, mem, sinfo};
use crate::elf::{ElfSegment, MAX_SEGMENTS};
use neo_common::BootInfo;

static mut BI: BootInfo = BootInfo {
    magic: 0,
    fb_base: 0,
    fb_size: 0,
    width: 0,
    height: 0,
    stride: 0,
    pixel_format: neo_common::PixelFormat::Rgb,
    mmap_len: 0,
    mmap: [neo_common::MemRegion::ZERO; neo_common::MMAP_MAX],
    user_base: 0,
    user_len: 0,
    nuser_progs: 0,
    user_progs: [neo_common::UserProg::ZERO; neo_common::MAX_USER_PROGS],
    initrd_base: 0,
    initrd_len: 0,
};

pub const MAX_TASKS: usize = 16;
/// Kernel stack size per task. Re-exports the size used by the scheduler
/// so the address-space builder and the TSS refresh stay in step.
pub const STACK_PAGES: u64 = KSTACK_PAGES;
/// User stack size in pages (alias kept for the spawn/exec paths).
pub const USTACK_PAGES: u64 = USER_STACK_PAGES;

// __switch_to(cur, next): save callee-saved regs on the current stack, park
// rsp in *cur, load rsp from *next, pop, ret. The pushed `ret` slot is what
// a freshly crafted stack points at (-> task_entry / __user_trampoline).
global_asm!(
    ".globl __switch_to",
    "__switch_to:",
    "push rbp",
    "push rbx",
    "push r12",
    "push r13",
    "push r14",
    "push r15",
    "mov [rdi], rsp",
    "mov rsp, [rsi]",
    "pop r15",
    "pop r14",
    "pop r13",
    "pop r12",
    "pop rbx",
    "pop rbp",
    "ret",
);

// User task first entry: switch_to rets here with rsp pointing at a
// hand-built iret frame (rip/cs/rflags/rsp/ss); iretq drops us to ring3.
global_asm!(
    ".globl __user_trampoline",
    "__user_trampoline:",
    "iretq",
);

// Dispatch trampoline: pops program index into RDI before iretq.
// Stack layout when entered: [prog_index][iret frame: rip/cs/rfl/rsp/ss]
global_asm!(
    ".globl __user_trampoline_dispatch",
    "__user_trampoline_dispatch:",
    "pop rdi",       // prog_index → RDI (first arg to _start)
    "iretq",
);

extern "C" {
    fn __switch_to(cur: *mut u64, next: *const u64);
    fn __user_trampoline();
    fn __user_trampoline_dispatch();
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum State {
    Empty,
    Ready,
    /// Blocked until a wake deadline (absolute PIT tick) or until woken by
    /// another task. This is the state that makes `sleep` a *real* sleep
    /// rather than a spin: the task is not eligible for scheduling and
    /// therefore consumes no CPU while it waits.
    Sleeping,
    ExitPend,
    Exited,
}

/// Default per-task address-space layout.
///
/// These are *virtual* addresses and deliberately independent of the
/// physical frames backing them. Early revisions used the physical frame
/// address as the user stack pointer, which made `fork` meaningless: the
/// child got a stack pointer that only meant anything in the parent's
/// translation. Pinning the stack to a fixed high user address is what
/// lets two processes share a code/data layout while owning different
/// physical pages.
pub const USER_STACK_TOP: u64 = 0x0000_0000_8000_0000; // 2 GiB
pub const USER_STACK_PAGES: u64 = 8; // 32 KiB
pub const USER_STACK_BASE: u64 = USER_STACK_TOP - USER_STACK_PAGES * 4096;

/// Kernel stack size per task (16 KiB).
const KSTACK_PAGES: u64 = 4;

/// Maximum tracked sleeping tasks; keep in step with MAX_TASKS.
const SLEEP_MAX: usize = 16;
/// Sleep deadline table: absolute PIT tick, 0 = slot unused.
static mut SLEEP_DEADLINE: [u64; SLEEP_MAX] = [0; SLEEP_MAX];

#[derive(Clone, Copy)]
struct Task {
    state: State,
    rsp_slot: u64, // saved rsp while not running
    kstack: u64,   // kernel stack base; 0 = boot task (firmware stack)
    ustack: u64,   // user stack physical base; 0 = kernel task
    root: u64,     // per-task PML4; 0 = shared kernel root
    segments: [ElfSegment; MAX_SEGMENTS],
    nsegments: usize,
    stack_phys: u64,
    brk: u64,
    id: u64,
    parent_id: u64, // parent task ID (0 for root tasks)
    pgid: u64,       // process group ID (0 = not in a group)
    name: [u8; 16],  // program name (null-terminated)
    entry: Option<fn()>,
}

const EMPTY_TASK: Task = Task {
    state: State::Empty,
    rsp_slot: 0,
    kstack: 0,
    ustack: 0,
    root: 0,
    segments: [ElfSegment { vaddr: 0, phys: 0, len: 0, prot: 0 }; MAX_SEGMENTS],
    nsegments: 0,
    stack_phys: 0,
    brk: 0,
    id: 0,
    parent_id: 0,
    pgid: 0,
    name: [0; 16],
    entry: None,
};

static mut TASKS: [Task; MAX_TASKS] = [EMPTY_TASK; MAX_TASKS];
static mut CURRENT: usize = 0;
static NEXT_ID: AtomicU64 = AtomicU64::new(1);
static INITED: AtomicBool = AtomicBool::new(false);


unsafe fn table() -> &'static mut [Task; MAX_TASKS] {
    unsafe { &mut *core::ptr::addr_of_mut!(TASKS) }
}

/// True when `slot` may be handed out to a new task.
///
/// Only `Empty` qualifies. The older predicate was `state != Ready`, which
/// silently treated a `Sleeping` task as free and would overwrite a blocked
/// process — a use-after-free waiting to happen once sleeping existed.
pub(crate) unsafe fn is_free_slot(i: usize) -> bool {
    i < MAX_TASKS && table()[i].state == State::Empty
}

/// Mark the running main context as task 0. Call once after
/// [`crate::interrupts::init`]; the timer may fire before this and
/// `schedule` self-disables until INITED is set.
pub fn init(bi: &BootInfo) {
    let was = interrupts_enabled();
    cli();
    unsafe {
        BI = *bi;
        table()[0] = Task {
            state: State::Ready,
            rsp_slot: 0,
            kstack: 0,
            ustack: 0,
            root: 0,
            segments: [ElfSegment { vaddr: 0, phys: 0, len: 0, prot: 0 }; MAX_SEGMENTS],
            nsegments: 0,
            stack_phys: 0,
            brk: 0,
            id: 0,
            parent_id: 0,
            pgid: 0,
            name: *b"shell\0\0\0\0\0\0\0\0\0\0\0",
            entry: None,
        };
        CURRENT = 0;
    }
    INITED.store(true, Ordering::SeqCst);
    kinfo!("task: table ready, main = task 0");
    if was {
        sti();
    }
}

/// Spawn `f` as a new task; returns its id. Reuses `Exited` slots (their
/// stacks were already returned); `Ready` slots are live and skipped.
pub fn spawn(f: fn()) -> Option<u64> {
    let was = interrupts_enabled();
    cli();
    let slot = (0..MAX_TASKS).find(|&i| unsafe { crate::task::is_free_slot(i) });
    let r = match slot {
        None => {
            crate::kwarn!("task: table full, spawn rejected");
            None
        }
        Some(i) => {
            let base = unsafe { mem::frames() }.alloc_contiguous(STACK_PAGES);
            let top = base + STACK_PAGES * mem::FRAME;
            // Crafted switch frame: [r15 r14 r13 r12 rbx rbp][ret->task_entry]
            let sp = top - 7 * 8;
            unsafe {
                for k in 0..6 {
                    ((sp + k * 8) as *mut u64).write_volatile(0);
                }
                ((sp + 48) as *mut u64).write_volatile(task_entry as *const () as u64);
                let t = &mut table()[i];
                t.state = State::Ready;
                t.rsp_slot = sp;
                t.kstack = base;
                t.ustack = 0; // kernel task: no user stack
                t.root = 0;
                t.nsegments = 0;
                t.stack_phys = 0;
                t.brk = 0;
                t.id = NEXT_ID.fetch_add(1, Ordering::Relaxed);
                t.parent_id = 0; // kernel-spawned tasks have no parent
                t.pgid = t.id;   // new process group leader
                t.name = [0; 16]; // kernel tasks have no name
                t.entry = Some(f);
                let id = t.id;
                kinfo!("task: spawn id={} stack={:#x}", id, base);
                Some(id)
            }
        }
    };
    if was {
        sti();
    }
    r
}

/// Spawn a ring3 task by program name. Searches the bootloader-staged
/// user program table for a matching name, then loads that ELF.
/// Check if a user program exists by name. Returns true if found.
pub fn find_prog_by_name(name: &[u8]) -> bool {
    let bi = unsafe { &BI };
    for i in 0..bi.nuser_progs as usize {
        let prog = &bi.user_progs[i];
        let pname = &prog.name;
        let mut matches = true;
        for (j, &b) in name.iter().enumerate() {
            if j >= 15 || pname[j] != b { matches = false; break; }
        }
        if matches && name.len() < 16 && pname[name.len()] == 0 {
            return true;
        }
    }
    false
}

pub fn spawn_user_by_name(name: &[u8]) -> Option<u64> {
    let bi = unsafe { &BI };
    for i in 0..bi.nuser_progs as usize {
        let prog = &bi.user_progs[i];
        let pname = &prog.name;
        // Compare null-terminated name.
        let mut matches = true;
        for (j, &b) in name.iter().enumerate() {
            if j >= 15 || pname[j] != b { matches = false; break; }
        }
        if matches && pname[name.len()] == 0 {
            let result = spawn_user_from_elf_with_index(prog.elf_base as *const u8, prog.elf_len, 0);
            // Set task name after spawn.
            if let Some(id) = result {
                unsafe {
                    for s in 0..MAX_TASKS {
                        if table()[s].id == id {
                            let mut nbuf = [0u8; 16];
                            let copy_len = core::cmp::min(name.len(), 15);
                            nbuf[..copy_len].copy_from_slice(&name[..copy_len]);
                            table()[s].name = nbuf;
                            break;
                        }
                    }
                }
            }
            return result;
        }
    }
    crate::kwarn!("task: program not found");
    None
}

/// Spawn a ring3 task from the default ELF image using dispatch index.
pub fn spawn_user(prog_index: u64) -> Option<u64> {
    let elf_data = unsafe { BI.user_base } as *const u8;
    let elf_len = unsafe { BI.user_len };
    // For dispatch mode, we still use spawn_user_from_elf but pass prog_index
    // via the iret frame's RDI (handled inside spawn_user_from_elf).
    // We need to store prog_index somewhere accessible... 
    // Actually, spawn_user_from_elf doesn't know about prog_index.
    // Let's add it as a parameter.
    spawn_user_from_elf_with_index(elf_data, elf_len, prog_index)
}

/// Spawn a ring3 task from an ELF buffer with a specific program index.
fn spawn_user_from_elf_with_index(elf_data: *const u8, elf_len: u64, prog_index: u64) -> Option<u64> {
    let was = interrupts_enabled();
    cli();
    let slot = (0..MAX_TASKS).find(|&i| unsafe { crate::task::is_free_slot(i) });
    let r = match slot {
        None => {
            crate::kwarn!("task: table full, user spawn rejected");
            None
        }
        Some(i) => {
            let kbase = unsafe { mem::frames() }.alloc_contiguous(STACK_PAGES);
            let ktop = kbase + STACK_PAGES * mem::FRAME;
            let ubase = unsafe { mem::frames() }.alloc_contiguous(USTACK_PAGES);

            if elf_data.is_null() || elf_len == 0 {
                crate::kwarn!("task: no ELF image available");
                mem::free_frames(kbase, STACK_PAGES * mem::FRAME);
                mem::free_frames(ubase, USTACK_PAGES * mem::FRAME);
                return None;
            }

            let alloc = unsafe { mem::frames() };
            let loaded = unsafe { crate::elf::load_elf(elf_data, elf_len, alloc) };
            let loaded = match loaded {
                Ok(l) => l,
                Err(e) => {
                    crate::kwarn!("task: ELF load failed: {:?}", e);
                    mem::free_frames(kbase, STACK_PAGES * mem::FRAME);
                    mem::free_frames(ubase, USTACK_PAGES * mem::FRAME);
                    return None;
                }
            };

            // Build per-task page table with ELF segments + stack.
            let root = unsafe {
                crate::elf::build_user_page_table(
                    &loaded.segments,
                    loaded.nsegments,
                    ubase,
                    USTACK_PAGES,
                    alloc,
                )
            };

            // Stack pointer names the fixed virtual address, not the frame.
            let utop = USER_STACK_TOP;
            // Kernel switch frame then a 5-slot iret frame:
            // [r15 r14 r13 r12 rbx rbp][ret->trampoline][prog_index][rip cs rfl rsp ss]
            // trampoline pops prog_index into RDI, then iretq to ring3 _start.
            let sp = ktop - 13 * 8;
            unsafe {
                for k in 0..6 {
                    ((sp + k * 8) as *mut u64).write_volatile(0);
                }
                ((sp + 48) as *mut u64).write_volatile(__user_trampoline_dispatch as *const () as u64);
                ((sp + 56) as *mut u64).write_volatile(prog_index);
                ((sp + 64) as *mut u64).write_volatile(loaded.entry_point);
                ((sp + 72) as *mut u64).write_volatile(USER_CS as u64);
                ((sp + 80) as *mut u64).write_volatile(0x202);
                ((sp + 88) as *mut u64).write_volatile(utop - 8);
                ((sp + 96) as *mut u64).write_volatile(USER_SS as u64);
                let t = &mut table()[i];
                t.state = State::Ready;
                t.rsp_slot = sp;
                t.kstack = kbase;
                t.ustack = ubase;
                t.root = root;
                t.segments = loaded.segments;
                t.nsegments = loaded.nsegments;
                t.stack_phys = ubase;
                // Initialize brk to end of highest ELF segment (top of .bss).
                let mut max_end: u64 = 0;
                for s in 0..loaded.nsegments {
                    let seg_end = loaded.segments[s].vaddr + loaded.segments[s].len;
                    if seg_end > max_end { max_end = seg_end; }
                }
                t.brk = (max_end + 0xFFF) & !0xFFF; // page-align up
                t.id = NEXT_ID.fetch_add(1, Ordering::Relaxed);
                t.parent_id = 0; // kernel-spawned user tasks have no parent
                t.pgid = t.id;   // new process group leader
                t.name = [0; 16]; // will be set by caller if needed
                t.entry = None;
                let id = t.id;
                kinfo!(
                    "task: user spawn id={} kstack={:#x} ustack={:#x} root={:#x} segs={}",
                    id,
                    kbase,
                    ubase,
                    root,
                    loaded.nsegments
                );
                Some(id)
            }
        }
    };
    if was {
        sti();
    }
    r
}

/// First entry of a spawned task: logically still inside another task's
/// timer IRQ (IF clear). Enable interrupts -- tasks must be preemptable.
fn task_entry() -> ! {
    sti();
    let f = unsafe { table()[CURRENT].entry.take() };
    if let Some(f) = f {
        f();
    }
    exit()
}

/// Terminate the current task. Marks the slot Exited; actual resource
/// reclamation happens in `schedule()` after we have switched to a
/// different task (different CR3 + different kernel stack), so we never
/// free memory that is still in use.
pub fn exit() -> ! {
    cli();
    unsafe {
        let cur = CURRENT;
        let t = &mut table()[cur];
        kinfo!("task: id={} exiting", t.id);
        t.state = State::Exited;
    }
    loop {
        schedule();
        // Only reachable if every task exited (main never does): park.
        hlt();
    }
}

/// Reclaim resources of any Exited tasks. Safe to call from any task
/// context because we only touch slots that are no longer running (their
/// CR3 and stacks are not in use). Skips the current slot defensively.
unsafe fn reap_exited() {
    let cur = CURRENT;
    for i in 0..MAX_TASKS {
        if i == cur {
            continue;
        }
        let t = &mut table()[i];
        if t.state != State::Exited {
            continue;
        }
        let kstack = t.kstack;
        let ustack = t.ustack;
        let root = t.root;
        let nsegments = t.nsegments;
        let stack_phys = t.stack_phys;
        // Copy segments before clearing for the destroy call.
        let segments = t.segments;
        // Clear before freeing so a double-reap is harmless.
        t.kstack = 0;
        t.ustack = 0;
        t.root = 0;
        t.nsegments = 0;
        t.stack_phys = 0;
        t.state = State::Empty;
        if kstack != 0 {
            mem::free_frames(kstack, STACK_PAGES * mem::FRAME);
        }
        if ustack != 0 {
            mem::free_frames(ustack, USTACK_PAGES * mem::FRAME);
        }
        if root != 0 {
            // Free ELF segment frames + page table.
            let alloc = mem::frames();
            for s in 0..nsegments {
                let seg = &segments[s];
                if seg.phys != 0 && seg.len > 0 {
                    let pages = seg.len.div_ceil(mem::FRAME);
                    alloc.free_frames(seg.phys, pages * mem::FRAME);
                }
            }
            // Free the PML4 and any private intermediate tables.
            // Walk user-mapped paths to find privately-cloned nodes.
            crate::elf::destroy_user_page_table(root, &segments, nsegments, stack_phys, USTACK_PAGES, alloc);
        }
        kinfo!("task: id={} reaped", t.id);
    }
}

/// Pick the next Ready task and switch. Caller must run with IF clear
/// (timer IRQ after EOI, or exit). Keeps current running if it's the only
/// Ready task. Also refreshes TSS.RSP0 so any ring3 -> ring0 entry lands
/// on the incoming user task's kernel stack.
pub fn schedule() {
    if !INITED.load(Ordering::Relaxed) {
        return;
    }
    unsafe {
        reap_exited();
        let cur = CURRENT;
        let mut i = (cur + 1) % MAX_TASKS;
        while i != cur && table()[i].state != State::Ready {
            i = (i + 1) % MAX_TASKS;
        }
        if i == cur {
            // No other candidate. If the current task is no longer runnable
            // it is blocked; the timer will re-enter us and pick up whatever
            // the wakeup promoted. Either way, keep the current stack.
            return;
        }
        let cur_slot = core::ptr::addr_of_mut!(table()[cur].rsp_slot);
        let next_slot = core::ptr::addr_of!(table()[i].rsp_slot);
        let next_kstack = table()[i].kstack;
        let next_root = table()[i].root;
        CURRENT = i;
        if next_kstack != 0 {
            crate::gdt::set_rsp0(next_kstack + STACK_PAGES * mem::FRAME);
        }
        if next_root != 0 {
            crate::cpu::write_cr3(next_root);
        } else {
            crate::cpu::write_cr3(mem::kernel_pml4());
        }
        __switch_to(cur_slot, next_slot);
    }
}

// ---------------------------------------------------------------------------
// Blocking
// ---------------------------------------------------------------------------
//
// The context switch (`__switch_to`) only saves and restores `rsp`; all other
// state lives on the task's own kernel stack. That constraint decides the
// design: a task blocks *on its own stack* and lets the timer preempt it,
// rather than hand-rolling a separate idle context.
//
// `sleep_current` therefore re-enables interrupts, then halts until the
// scheduler marks it runnable again. Halting is what makes this a real sleep:
// the task is off the run queue and consumes no CPU while it waits.

/// Put the current task to sleep for `ticks` PIT ticks, then return.
///
/// This replaces the old busy-wait, which could not work at all: `int 0x80`
/// is an interrupt gate, so IF is clear inside the syscall and the PIT tick
/// counter could never advance while the loop spun.
pub fn sleep_current(ticks: u64) {
    if ticks == 0 {
        return;
    }
    unsafe {
        let cur = CURRENT;
        let deadline = crate::interrupts::ticks().wrapping_add(ticks);
        sleep_set(cur, deadline);
        table()[cur].state = State::Sleeping;

        // Interrupts must be on for `hlt` to ever return: the timer is the
        // only thing that can wake us.
        crate::cpu::sti();
        while table()[cur].state == State::Sleeping {
            crate::cpu::hlt();
        }
        // Restore the syscall-path invariant (IF clear) before returning to
        // `do_syscall`; the final `iretq` reinstates the user's own flags.
        crate::cpu::cli();
        sleep_clear(cur);
    }
}

unsafe fn sleep_set(slot: usize, deadline: u64) {
    if slot < SLEEP_MAX {
        SLEEP_DEADLINE[slot] = deadline;
    }
}

unsafe fn sleep_clear(slot: usize) {
    if slot < SLEEP_MAX {
        SLEEP_DEADLINE[slot] = 0;
    }
}

/// Promote expired sleepers back to Ready. Called from the timer IRQ, so it
/// runs with interrupts disabled and stays O(MAX_TASKS).
pub fn wake_expired() {
    unsafe {
        let now = crate::interrupts::ticks();
        for i in 0..MAX_TASKS {
            if table()[i].state != State::Sleeping {
                continue;
            }
            let dl = if i < SLEEP_MAX { SLEEP_DEADLINE[i] } else { 0 };
            // Deadline 0 means "blocked with no timer" (e.g. waitpid); such a
            // task is woken explicitly, never by the clock.
            if dl != 0 && now.wrapping_sub(dl) < (1u64 << 63) {
                SLEEP_DEADLINE[i] = 0;
                table()[i].state = State::Ready;
            }
        }
    }
}

/// Wake a specific task if it is sleeping. Used by `waitpid` (child exit)
/// and `kill`.
pub fn wake_task(pid: u64) {
    unsafe {
        for i in 0..MAX_TASKS {
            if table()[i].id == pid && table()[i].state == State::Sleeping {
                sleep_clear(i);
                table()[i].state = State::Ready;
                return;
            }
        }
    }
}
/// Shell `tasks` command: one line, `*` marks the running task, `u` marks
/// ring3 tasks.
/// sys_brk implementation: extend or query the program break.
/// new_brk == 0 → return current brk.
/// new_brk > current brk → map new pages (RW+NX) up to new_brk.
/// Returns new brk on success, current brk on failure.
pub fn sys_brk(new_brk: u64) -> u64 {
    unsafe {
        let cur = CURRENT;
        let t = &mut table()[cur];
        if t.brk == 0 {
            return 0; // kernel task, no heap
        }
        if new_brk == 0 {
            return t.brk;
        }
        let old_brk = t.brk;
        let new_brk_aligned = (new_brk + 0xFFF) & !0xFFF;
        if new_brk_aligned <= old_brk {
            // Shrink: just update brk (don't unmap pages for simplicity).
            t.brk = new_brk_aligned;
            return t.brk;
        }
        // Extend: map new pages from old_brk to new_brk_aligned.
        let alloc = mem::frames();
        let root = t.root;
        let mut addr = old_brk;
        while addr < new_brk_aligned {
            let phys = alloc.alloc_contiguous(1);
            if phys == 0 {
                return old_brk; // allocation failed
            }
            // Zero the new page.
            core::ptr::write_bytes(phys as *mut u8, 0, mem::FRAME as usize);
            mem::ensure_private_path(root, addr, alloc);
            mem::replace_page(root, addr, phys, mem::PROT_UDATA);
            addr += mem::FRAME;
        }
        t.brk = new_brk_aligned;
        t.brk
    }
}

/// Read-only access to BootInfo (for shell commands etc.).
pub fn bi_ref() -> &'static BootInfo {
    unsafe { &BI }
}

/// Get the current task's ID.
pub fn current_id() -> u64 {
    unsafe { TASKS[CURRENT].id }
}

/// Get the parent task ID of the current task.
pub fn current_parent_id() -> u64 {
    unsafe { TASKS[CURRENT].parent_id }
}

/// Get the process group ID of the current task.
pub fn current_pgid() -> u64 {
    unsafe { TASKS[CURRENT].pgid }
}

/// Get the program name of the current task.
pub fn current_name() -> [u8; 16] {
    unsafe { TASKS[CURRENT].name }
}

/// Task info for /proc/tasks listing.
pub struct TaskInfo {
    pub id: u64,
    pub state: u8, // 0=empty, 1=ready, 2=exited
    pub pgid: u64,
}

/// Get task info by slot index. Returns None if slot is empty.
pub fn get_task_info(slot: usize) -> Option<TaskInfo> {
    if slot >= MAX_TASKS { return None; }
    unsafe {
        let t = &TASKS[slot];
        match t.state {
            State::Empty => None,
            State::Ready => Some(TaskInfo { id: t.id, state: 1, pgid: t.pgid }),
            State::Exited => Some(TaskInfo { id: t.id, state: 2, pgid: t.pgid }),
            State::Sleeping => Some(TaskInfo { id: t.id, state: 3, pgid: t.pgid }),
            State::ExitPend => Some(TaskInfo { id: t.id, state: 4, pgid: t.pgid }),
        }
    }
}

/// Get the page table root (CR3) of the current task.
/// Returns 0 for kernel tasks (no private address space).
pub fn current_root() -> u64 {
    unsafe { TASKS[CURRENT].root }
}

/// Fork the current task using COW (Copy-on-Write).
/// Returns child task ID on success, 0 in child context, !0 on failure.
/// This is a simplified fork: copies page tables with shared physical pages,
/// marks user data pages as COW, allocates new stacks for child.
pub fn fork_current() -> Option<u64> {
    let was = interrupts_enabled();
    cli();

    unsafe {
        let parent_idx = CURRENT;
        let parent = &table()[parent_idx];

        // Only ring3 tasks can fork.
        if parent.root == 0 || parent.ustack == 0 {
            if was { sti(); }
            return None;
        }

        // Find free slot.
        let child_idx = match (0..MAX_TASKS).find(|&i| table()[i].state == State::Empty && i != parent_idx) {
            Some(i) => i,
            None => {
                crate::kwarn!("task: fork failed, table full");
                if was { sti(); }
                return None;
            }
        };

        // Allocate new kernel stack for child.
        let kstack_base = mem::frames().alloc_contiguous(STACK_PAGES);
        if kstack_base == 0 {
            if was { sti(); }
            return None;
        }

        // Allocate new user stack for child.
        let ustack_phys = mem::frames().alloc_contiguous(STACK_PAGES);
        if ustack_phys == 0 {
            mem::frames().free_frames(kstack_base, STACK_PAGES * mem::FRAME);
            if was { sti(); }
            return None;
        }
        // Copy parent's user stack content to child's stack.
        core::ptr::copy_nonoverlapping(
            parent.stack_phys as *const u8,
            ustack_phys as *mut u8,
            (STACK_PAGES * mem::FRAME) as usize,
        );

        // Clone the parent's page table tree (share physical pages).
        let child_root = clone_page_table(parent.root);
        if child_root == 0 {
            mem::frames().free_frames(kstack_base, STACK_PAGES * mem::FRAME);
            mem::frames().free_frames(ustack_phys, STACK_PAGES * mem::FRAME);
            if was { sti(); }
            return None;
        }

        // Mark user data pages as COW in BOTH parent and child.
        // User code region: 0x400000-0x7FFFFF (read-only, no COW needed)
        // User data/heap: 0x800000-0x9FFFFF (COW)
        crate::mem::mark_pages_cow(parent.root, 0x800000, 0x200000);
        crate::mem::mark_pages_cow(child_root, 0x800000, 0x200000);

        // Set up child task.
        let child_kstack_top = kstack_base + STACK_PAGES * mem::FRAME;
        let sp = child_kstack_top - 7 * 8;
        for k in 0..6 {
            ((sp + k * 8) as *mut u64).write_volatile(0);
        }
        ((sp + 48) as *mut u64).write_volatile(task_entry as *const () as u64);

        let child_id = NEXT_ID.fetch_add(1, Ordering::Relaxed);
        let child = &mut table()[child_idx];
        child.state = State::Ready;
        child.rsp_slot = sp;
        child.kstack = kstack_base;
        child.ustack = ustack_phys;
        child.root = child_root;
        child.nsegments = parent.nsegments;
        child.segments = parent.segments; // share segment info
        child.stack_phys = ustack_phys;
        child.brk = parent.brk;
        child.id = child_id;
        child.parent_id = parent.id;
        child.pgid = parent.pgid; // inherit process group
        child.name = parent.name;  // inherit program name

        if was { sti(); }
        Some(child_id)
    }
}

/// Exec: replace current task's image with a new ELF program.
/// Returns true on success (task image replaced, scheduler will resume new image).
/// Returns false on failure (not found, load error, OOM).
pub fn exec_current(prog_name: &[u8]) -> bool {
    let bi = unsafe { &BI };
    let mut elf_data: *const u8 = core::ptr::null();
    let mut elf_len: u64 = 0;

    for i in 0..bi.nuser_progs as usize {
        let prog = &bi.user_progs[i];
        let pname = &prog.name;
        // Compare name.
        let mut matches = true;
        for (j, &b) in prog_name.iter().enumerate() {
            if j >= 15 || pname[j] != b { matches = false; break; }
        }
        if matches && prog_name.len() < 16 && pname[prog_name.len()] == 0 {
            elf_data = prog.elf_base as *const u8;
            elf_len = prog.elf_len;
            break;
        }
    }

    if elf_data.is_null() || elf_len == 0 {
        crate::kwarn!("task: exec '{}' not found", core::str::from_utf8(prog_name).unwrap_or("?"));
        return false;
    }

    // Destroy old address space and free old stacks.
    let old_root = unsafe { TASKS[CURRENT].root };
    let old_kstack = unsafe { TASKS[CURRENT].kstack };
    let old_ustack = unsafe { TASKS[CURRENT].ustack };
    let old_segs = unsafe { TASKS[CURRENT].segments };
    let old_nsegs = unsafe { TASKS[CURRENT].nsegments };
    let old_stack_phys = unsafe { TASKS[CURRENT].stack_phys };

    // Free old user page table and image.
    if old_root != 0 && old_nsegs > 0 {
        unsafe {
            let alloc = mem::frames();
            crate::elf::destroy_user_page_table(old_root, &old_segs, old_nsegs, old_stack_phys, USTACK_PAGES, alloc);
        }
    }

    // Free old stacks.
    if old_kstack != 0 {
        mem::free_frames(old_kstack, STACK_PAGES * mem::FRAME);
    }
    if old_ustack != 0 {
        mem::free_frames(old_ustack, USTACK_PAGES * mem::FRAME);
    }

    // Allocate new stacks.
    let kbase = unsafe { mem::frames() }.alloc_contiguous(STACK_PAGES);
    let ubase = unsafe { mem::frames() }.alloc_contiguous(USTACK_PAGES);
    if kbase == 0 || ubase == 0 {
        crate::kwarn!("task: exec OOM");
        return false;
    }

    // Load new ELF.
    let alloc = unsafe { mem::frames() };
    let loaded = unsafe { crate::elf::load_elf(elf_data, elf_len, alloc) };
    let loaded = match loaded {
        Ok(l) => l,
        Err(e) => {
            crate::kwarn!("task: exec ELF load failed: {:?}", e);
            return false;
        }
    };

    // Build new page table.
    let root = unsafe {
        crate::elf::build_user_page_table(&loaded.segments, loaded.nsegments, ubase, USTACK_PAGES, alloc)
    };

    // Set up new task state in the same slot.
    // exec: same fixed virtual stack top as the spawn path.
    let utop = USER_STACK_TOP;
    let sp = (kbase + STACK_PAGES * mem::FRAME) - 13 * 8;
    unsafe {
        for k in 0..6 {
            ((sp + k * 8) as *mut u64).write_volatile(0);
        }
        ((sp + 48) as *mut u64).write_volatile(__user_trampoline_dispatch as *const () as u64);
        ((sp + 56) as *mut u64).write_volatile(0); // prog_index = 0 (default dispatch)
        ((sp + 64) as *mut u64).write_volatile(loaded.entry_point);
        ((sp + 72) as *mut u64).write_volatile(USER_CS as u64);
        ((sp + 80) as *mut u64).write_volatile(0x202);
        ((sp + 88) as *mut u64).write_volatile(utop - 8);
        ((sp + 96) as *mut u64).write_volatile(USER_SS as u64);

        let cur = CURRENT;
        let t = &mut table()[cur];
        let old_id = t.id;
        let old_parent = t.parent_id;
        let old_pgid = t.pgid;
        t.state = State::Ready;
        t.rsp_slot = sp;
        t.kstack = kbase;
        t.ustack = ubase;
        t.root = root;
        t.segments = loaded.segments;
        t.nsegments = loaded.nsegments;
        t.stack_phys = ubase;
        let mut max_end: u64 = 0;
        for s in 0..loaded.nsegments {
            let seg_end = loaded.segments[s].vaddr + loaded.segments[s].len;
            if seg_end > max_end { max_end = seg_end; }
        }
        t.brk = (max_end + 0xFFF) & !0xFFF;
        t.id = NEXT_ID.fetch_add(1, Ordering::Relaxed);
        t.parent_id = old_parent;
        t.pgid = old_pgid;
        // Update task name to new program name.
        let mut nbuf = [0u8; 16];
        let copy_len = core::cmp::min(prog_name.len(), 15);
        nbuf[..copy_len].copy_from_slice(&prog_name[..copy_len]);
        t.name = nbuf;
        t.entry = None;
        kinfo!("task: exec id={} -> id={}", old_id, t.id);

        // The new stack frame is set up in rsp_slot.
        // When the scheduler switches back to this task, it will resume at the new entry point.
    }
    true
}

/// Wait for a child task to exit. Returns child ID when it exits.

/// Send a signal to a task. Currently supports SIGTERM (15) = terminate.
/// Returns 0 on success, -1 if task not found, -2 if signal unsupported.
pub fn kill_task(pid: u64, sig: u32) -> i32 {
    if sig != 15 { return -2; } // only SIGTERM supported
    unsafe {
        for i in 0..MAX_TASKS {
            if table()[i].id == pid && table()[i].state == State::Ready {
                table()[i].state = State::Exited;
                crate::sinfo!("task: killed pid={}", pid);
                return 0;
            }
        }
    }
    -1 // not found
}

/// Kill all tasks in a process group. Returns number of tasks killed.
pub fn kill_pgid(pgid: u64, sig: u32) -> i32 {
    if sig != 15 { return -2; }
    let mut killed = 0i32;
    unsafe {
        for i in 0..MAX_TASKS {
            if table()[i].pgid == pgid && table()[i].state == State::Ready && table()[i].id != 0 {
                table()[i].state = State::Exited;
                killed += 1;
            }
        }
    }
    if killed > 0 { killed } else { -1 }
}

/// Wait for a child task to exit. Returns child ID when it exits.
/// `pid` = specific child ID, or 0 for any child.
/// This is a busy-wait implementation (no blocking scheduler yet).
pub fn waitpid(pid: u64) -> Option<u64> {
    let parent_id = unsafe { TASKS[CURRENT].id };
    // Poll until a matching child exits (max ~5 seconds).
    for _ in 0..500 {
        unsafe {
            for i in 0..MAX_TASKS {
                let t = &table()[i];
                if t.parent_id == parent_id && t.state == State::Exited {
                    if pid == 0 || t.id == pid {
                        let child_id = t.id;
                        // Reap: mark slot as empty so it can be reused.
                        table()[i].state = State::Empty;
                        return Some(child_id);
                    }
                }
            }
        }
        crate::interrupts::sleep_ms(10);
    }
    None // timeout
}

/// Clone a PML4 tree, sharing all leaf physical pages.
/// Allocates new intermediate page tables but shares data pages.
fn clone_page_table(parent_root: u64) -> u64 {
    unsafe {
        let new_root = crate::mem::frames().alloc_contiguous(1);
        if new_root == 0 { return 0; }
        core::ptr::write_bytes(new_root as *mut u8, 0, 4096);

        // Copy kernel mappings (entries 256-511) from parent.
        let parent_pml4 = crate::mem::table_mut(parent_root);
        let child_pml4 = crate::mem::table_mut(new_root);
        for i in 256..512 {
            child_pml4[i] = parent_pml4[i];
        }

        // Clone user mappings (entries 0-255): allocate new PDPT/PD/PT but share pages.
        for pml4_idx in 0..256usize {
            let pml4e = parent_pml4[pml4_idx];
            if pml4e & crate::mem::PTE_P == 0 { continue; }

            let pdpt_phys = crate::mem::frames().alloc_contiguous(1);
            if pdpt_phys == 0 { return 0; }
            core::ptr::write_bytes(pdpt_phys as *mut u8, 0, 4096);
            child_pml4[pml4_idx] = (pdpt_phys & crate::mem::ADDR_MASK) | (pml4e & !crate::mem::ADDR_MASK);

            let parent_pdpt = crate::mem::table_mut(pml4e & crate::mem::ADDR_MASK);
            let child_pdpt = crate::mem::table_mut(pdpt_phys);

            for pdpt_idx in 0..512usize {
                let pdpte = parent_pdpt[pdpt_idx];
                if pdpte & crate::mem::PTE_P == 0 { continue; }
                if pdpte & crate::mem::PTE_PS != 0 {
                    child_pdpt[pdpt_idx] = pdpte;
                    continue;
                }

                let pd_phys = crate::mem::frames().alloc_contiguous(1);
                if pd_phys == 0 { return 0; }
                core::ptr::write_bytes(pd_phys as *mut u8, 0, 4096);
                child_pdpt[pdpt_idx] = (pd_phys & crate::mem::ADDR_MASK) | (pdpte & !crate::mem::ADDR_MASK);

                let parent_pd = crate::mem::table_mut(pdpte & crate::mem::ADDR_MASK);
                let child_pd = crate::mem::table_mut(pd_phys);

                for pd_idx in 0..512usize {
                    let pde = parent_pd[pd_idx];
                    if pde & crate::mem::PTE_P == 0 { continue; }
                    if pde & crate::mem::PTE_PS != 0 {
                        child_pd[pd_idx] = pde;
                        continue;
                    }

                    let pt_phys = crate::mem::frames().alloc_contiguous(1);
                    if pt_phys == 0 { return 0; }
                    core::ptr::write_bytes(pt_phys as *mut u8, 0, 4096);
                    child_pd[pd_idx] = (pt_phys & crate::mem::ADDR_MASK) | (pde & !crate::mem::ADDR_MASK);

                    let parent_pt = crate::mem::table_mut(pde & crate::mem::ADDR_MASK);
                    let child_pt = crate::mem::table_mut(pt_phys);
                    for pt_idx in 0..512usize {
                        child_pt[pt_idx] = parent_pt[pt_idx];
                    }
                }
            }
        }

        new_root
    }
}

/// Dump all task slots to serial output (debugging).
pub fn dump() {
    let was = interrupts_enabled();
    cli();
    unsafe {
        let cur = CURRENT;
        for i in 0..MAX_TASKS {
            let t = &table()[i];
            if t.state == State::Empty {
                continue;
            }
            let mark = if i == cur { "*" } else { " " };
            let kind = if t.ustack != 0 { "u" } else { " " };
            let st = match t.state {
                State::Ready => "ready",
                State::Sleeping => "sleep",
                State::ExitPend => "zombie",
                State::Exited => "exited",
                State::Empty => "empty",
            };
            kprint!("T{}{}{} {} | ", t.id, mark, kind, st);
        }
        kprint!("(stacks freed on exit; slots recycle)\n");
    }
    if was {
        sti();
    }
}

/// Serialize task list into a buffer for syscall return.
/// Format: [u32 count][entry: u64 id, u8 state, u8 is_user, u64 pgid, u64 parent_id, [u8;16] name]
/// Each entry is 42 bytes. Returns bytes written.
pub fn serialize_tasks(buf: &mut [u8]) -> usize {
    if buf.len() < 4 { return 0; }
    let mut pos = 4usize;
    let mut count = 0u32;

    unsafe {
        for i in 0..MAX_TASKS {
            let t = &table()[i];
            if t.state == State::Empty { continue; }
            if pos + 42 > buf.len() { break; }
            buf[pos..pos+8].copy_from_slice(&t.id.to_le_bytes());
            buf[pos+8] = match t.state {
                State::Ready => 1,
                State::Exited => 2,
                State::Sleeping => 3,
                State::ExitPend => 4,
                State::Empty => 0,
            };
            buf[pos+9] = if t.ustack != 0 { 1 } else { 0 };
            buf[pos+10..pos+18].copy_from_slice(&t.pgid.to_le_bytes());
            buf[pos+18..pos+26].copy_from_slice(&t.parent_id.to_le_bytes());
            buf[pos+26..pos+42].copy_from_slice(&t.name);
            pos += 42;
            count += 1;
        }
    }

    buf[0..4].copy_from_slice(&count.to_le_bytes());
    pos
}

// ---- demo tasks ----

/// Serial-only heartbeat every 3s: proof of preemption without stealing
/// the shell's screen.
pub fn demo_a() {
    loop {
        sleep_ms(3000);
        sinfo!("demo: task A beat at {} ticks", ticks());
    }
}

pub fn demo_b() {
    loop {
        sleep_ms(5000);
        sinfo!("demo: task B beat at {} ticks", ticks());
    }
}

/// One-shot for the shell `test` command: spawn + exit round-trip.
pub fn demo_once() {
    sinfo!("demo: one-shot task running");
    sleep_ms(1000);
    sinfo!("demo: one-shot done");
    exit();
}
