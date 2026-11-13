//! NEO-OS v3 kernel M11: BootInfo validation, serial + screen console,
//! uniform-4K identity paging with per-page protection, GDT/IDT, 8259 PIC
//! (PIT 100Hz + PS/2 keyboard), interactive shell, kernel + ring3 tasks
//! (own user image) with timer preemption, fault isolation and checked syscall
//! user buffers.

#![no_std]
#![no_main]
#![feature(abi_x86_interrupt)]
#![feature(alloc_error_handler)]

extern crate alloc;

mod console;
mod cpu;
mod dns;
mod driver;
mod elf;
mod font;
mod gdt;
mod gfx;
mod gui;
mod heap;
mod initrd;
mod interrupts;
mod ipi;
mod ip;
mod keyboard;
mod klog;
mod mem;
mod mouse;
mod pci;
mod rtc;
mod serial;
mod shell;
mod smp;
mod slab;
mod sync;
mod task;
mod tcp;
mod test;
mod usb;
mod virtio_net;
mod virtio_blk;
mod acpi;
mod ahci;
mod arp;
mod block;
mod vfs;
mod neofs;

use core::arch::asm;
use core::panic::PanicInfo;
use cpu::{cli, hlt, sti};
use neo_common::{BootInfo, BOOT_MAGIC};

#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    // Text first (console may be uninitialized -> serial only), then a red
    // frame so the crash is visible without covering the message.
    kerror!("KERNEL PANIC: {}", info);
    console::frame(0x00C00000);
    loop {
        hlt();
    }
}

/// Kernel entry point, jumped to by the bootloader after ExitBootServices.
/// The bootloader must have switched to a safe RWX page table before jumping.
///
/// # Safety
///
/// The caller must guarantee:
/// - `info` points to a valid, initialized `BootInfo`;
/// - paging is on with an identity map covering the kernel, stack,
///   framebuffer and `info`;
/// - the image is loaded at its linked address (`KERNEL_LOAD_ADDR`).
#[no_mangle]
pub unsafe extern "C" fn _start(info: *const BootInfo) -> ! {
    cli(); // firmware may leave IF set; init before any IRQ

    let bi: BootInfo = unsafe { *info };
    assert!(bi.magic == BOOT_MAGIC);

    serial::init();
    console::init(&bi);
    gfx::init(&bi);
    kinfo!("Hello NEO-OS v3 from kernel M9");
    kinfo!(
        "fb {}x{} stride={} fmt={:?} console {}x{}",
        bi.width,
        bi.height,
        bi.stride,
        bi.pixel_format,
        bi.width / 8,
        bi.height / 16
    );

    mem::init(&bi); // frames + our page tables (still under entry cli)
    heap::init(); // 1MiB heap + Box/Vec/String smoke tests
    slab::init(); // Fixed-size object caches

    // M10: relocate ELF staging buffer into frame-allocator-owned memory.
    // The bootloader placed USER.ELF at a fixed LOADER_DATA address, but
    // that region may overlap with Conventional RAM the allocator manages.
    // Copy it to freshly allocated frames so it survives allocation.
    let mut bi = bi;
    if bi.user_len > 0 && bi.user_base != 0 {
        let pages = (bi.user_len as usize).div_ceil(4096);
        let new_base = unsafe { mem::frames() }.alloc_contiguous(pages as u64);
        unsafe {
            core::ptr::copy_nonoverlapping(
                bi.user_base as *const u8,
                new_base as *mut u8,
                bi.user_len as usize,
            );
        }
        kinfo!("mem: ELF relocated {:#x} -> {:#x} ({} bytes)", bi.user_base, new_base, bi.user_len);
        bi.user_base = new_base;
    }

    gdt::init();
    kinfo!("gdt+tss loaded");

    // Initialize initrd from BootInfo.
    initrd::init(bi.initrd_base, bi.initrd_len);

    // Register drivers.
    let drv_serial = driver::register("serial", driver::DriverType::Serial);
    let drv_kbd = driver::register("ps2kbd", driver::DriverType::Keyboard);
    let drv_mouse = driver::register("ps2mouse", driver::DriverType::Mouse);
    let drv_fb = driver::register("framebuffer", driver::DriverType::Display);
    let drv_pit = driver::register("pit", driver::DriverType::Other);

    interrupts::init(); // idt + pic + pit; sti inside
    kinfo!("idt+pic+pit(100Hz) ready");
    if let Some(idx) = drv_pit { driver::set_ready(idx); }
    if let Some(idx) = drv_serial { driver::set_ready(idx); }

    mouse::init(bi.width, bi.height);
    if let Some(idx) = drv_kbd { driver::set_ready(idx); }
    if let Some(idx) = drv_mouse { driver::set_ready(idx); }
    if let Some(idx) = drv_fb { driver::set_ready(idx); }

    // PCI bus scan.
    let pci_count = pci::scan();
    kinfo!("pci: {} devices found", pci_count);
    pci::dump();

    // RTC: read real-time clock.
    rtc::init();

    // ACPI: find RSDP and parse FADT.
    acpi::init();

    // Network: find and init virtio-net.
    virtio_net::init();

    // Block device: find and init virtio-blk.
    virtio_blk::init();

    // AHCI: detect SATA controller (framework, no read/write yet).
    ahci::init();

    // USB: detect host controllers.
    usb::init();

    // VFS + filesystem.
    vfs::init();
    // Auto-mount NEO-FS on block device 0 if available.
    if block::count() > 0 {
        match neofs::mount(0) {
            Ok(()) => kinfo!("fs: NEO-FS auto-mounted"),
            Err(_) => kinfo!("fs: no NEO-FS on dev 0 (use mkfs to create)"),
        }
    }

    sti();
    unsafe { asm!("int3") }; // breakpoint self-test
    kinfo!("breakpoint self-test passed; tasks+shell starting");

    task::init(&bi);
    task::spawn(task::demo_a);
    task::spawn(task::demo_b);

    // Start UDP echo server task.
    task::spawn(virtio_net::echo_task);

    // SMP: detect topology (actual AP startup is a TODO).
    smp::init();

    // IPI: initialize inter-processor interrupt support.
    ipi::init();

    // DEBUG ONLY (M10 input saga): auto-enter the GUI at boot so mouse
    // delivery can be judged with zero typing. Revert after the verdict.
    const AUTO_GUI: bool = false;
    if AUTO_GUI {
        gui::enter();
    }

    // Run /init script from NEO-FS if available.
    run_init();

    // Load shell command history from NEO-FS.
    shell::load_history();

    shell::prompt();
    loop {
        // Drain input; hlt only when idle (PIT wakes us every 10ms, so no
        // lost-wakeup deadlock even if an IRQ lands between pump and hlt).
        if shell::pump() == 0 {
            sti();
            hlt();
        }
    }
}

/// Run /init script from NEO-FS on boot.
/// Each line is executed as a shell command. Lines starting with '#' are comments.
fn run_init() {
    if !neofs::is_mounted() { return; }

    // Try to open and read /init file via VFS.
    let fd = vfs::vfs_open(b"init", 0, 0); // read-only, task_id=0
    if fd < 0 { return; }

    kinfo!("init: running /init script");
    let mut buf = [0u8; 512];
    let n = vfs::vfs_read(fd as u32, &mut buf, 0);
    vfs::vfs_close(fd as u32, 0);

    if n <= 0 { return; }

    // Process line by line.
    let data = &buf[..n as usize];
    let mut line_start = 0;
    for i in 0..data.len() {
        if data[i] == b'\n' || data[i] == b'\r' {
            let line = &data[line_start..i];
            let trimmed = {
                let mut s = 0;
                while s < line.len() && (line[s] == b' ' || line[s] == b'\t') { s += 1; }
                &line[s..]
            };
            if !trimmed.is_empty() && trimmed[0] != b'#' {
                kprint!("init> ");
                for &b in trimmed { kprint!("{}", b as char); }
                kprint!("\n");
                shell::run_line(trimmed);
            }
            line_start = i + 1;
        }
    }
    // Handle last line without trailing newline.
    if line_start < data.len() {
        let line = &data[line_start..];
        let trimmed = {
            let mut s = 0;
            while s < line.len() && (line[s] == b' ' || line[s] == b'\t') { s += 1; }
            &line[s..]
        };
        if !trimmed.is_empty() && trimmed[0] != b'#' {
            kprint!("init> ");
            for &b in trimmed { kprint!("{}", b as char); }
            kprint!("\n");
            shell::run_line(trimmed);
        }
    }
    kinfo!("init: script complete");
}
