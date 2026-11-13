//! NEO-OS v3 UEFI bootloader (M10).
//! Loads kernel to a fixed address, user ELF to staging area.

#![no_main]
#![no_std]

use core::mem::transmute;
use core::ptr::NonNull;
use neo_common::{BootInfo, MemRegion, PixelFormat, BOOT_MAGIC, KERNEL_LOAD_ADDR, MMAP_MAX, UserProg, MAX_USER_PROGS};
use uefi::data_types::CStr16;
use uefi::mem::memory_map::MemoryMap;
use uefi::proto::console::gop::{GraphicsOutput, Mode, PixelFormat as GopFormat};
use uefi::proto::console::serial::Serial;
use uefi::proto::media::file::{File, FileAttribute, FileHandle, FileInfo, FileMode};
use uefi::{boot, cstr16, println, Status};

/// Program names to stage from /BIN/, generated at build time from
/// tools/programs.list so the bootloader and the build cannot disagree.
pub mod program_names_include {
    include!(concat!(env!("OUT_DIR"), "/program_names.rs"));
}
use program_names_include::PROGRAM_NAMES;

#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    println!("[BOOT] PANIC: {}", info);
    loop { unsafe { core::arch::asm!("hlt") }; }
}

fn slog(msg: &str) {
    if let Ok(h) = boot::get_handle_for_protocol::<Serial>() {
        if let Ok(mut s) = boot::open_protocol_exclusive::<Serial>(h) {
            let _ = s.write(msg.as_bytes());
        }
    }
}

fn setup_gop() -> uefi::Result<(Mode, u64, u64)> {
    let gop_handle = boot::get_handle_for_protocol::<GraphicsOutput>()?;
    let mut gop = boot::open_protocol_exclusive::<GraphicsOutput>(gop_handle)?;
    let mut best: Option<Mode> = None;
    let mut best_area = 0usize;
    for m in gop.modes() {
        let info = m.info();
        let area = info.resolution().0 * info.resolution().1;
        if info.resolution().0 == 1024 && info.resolution().1 == 768 {
            best = Some(m); break;
        }
        if area > best_area { best_area = area; best = Some(m); }
    }
    let mode = best.ok_or(Status::NOT_FOUND)?;
    gop.set_mode(&mode)?;
    let info = mode.info();
    Ok((mode, gop.frame_buffer().as_mut_ptr() as u64, gop.frame_buffer().size() as u64))
}

fn read_image(path: &CStr16) -> uefi::Result<(NonNull<u8>, usize)> {
    let mut fs = boot::get_image_file_system(boot::image_handle())?;
    let mut root = fs.open_volume()?;
    let handle: FileHandle = root.open(path, FileMode::Read, FileAttribute::READ_ONLY)?;
    let mut file = match handle.into_regular_file() {
        Some(f) => f,
        None => return Err(Status::NOT_FOUND.into()),
    };
    let mut info_buf = [0u8; 256];
    let size = file.get_info::<FileInfo>(&mut info_buf)
        .map_err(|e| e.status())?.file_size() as usize;
    let pages = size.div_ceil(0x1000) + 1;
    let ptr: NonNull<u8> = boot::allocate_pages(
        boot::AllocateType::AnyPages, boot::MemoryType::LOADER_DATA, pages,
    )?;
    let mut done = 0usize;
    while done < size {
        let buf = unsafe {
            core::slice::from_raw_parts_mut(
                (ptr.as_ptr() as u64 + done as u64) as *mut u8, size - done,
            )
        };
        let n = file.read(buf)?;
        if n == 0 { break; }
        done += n;
    }
    if done != size { return Err(Status::DEVICE_ERROR.into()); }
    Ok((ptr, done))
}

fn run() -> uefi::Result<()> {
    let (mode, fb_base, fb_size) = setup_gop()?;
    let info = mode.info();
    let (w, h) = info.resolution();
    let stride = info.stride();
    let pixfmt = match info.pixel_format() {
        GopFormat::Rgb => PixelFormat::Rgb,
        GopFormat::Bgr => PixelFormat::Bgr,
        _ => PixelFormat::Rgb,
    };
    slog("[BOOT] GOP mode set\r\n");

    // Load kernel to temp buffer first.
    let (ktmp, ksize) = read_image(cstr16!("\\EFI\\NEO\\KERNEL.BIN"))?;
    slog("[BOOT] kernel staged\r\n");

    // Try fixed address first; fall back to MaxAddress if unavailable.
    let kpages = ksize.div_ceil(0x1000);
    let kernel_base = if boot::allocate_pages(
        boot::AllocateType::Address(KERNEL_LOAD_ADDR),
        boot::MemoryType::LOADER_CODE, kpages,
    ).is_ok() {
        slog("[BOOT] fixed addr CODE OK\r\n");
        KERNEL_LOAD_ADDR
    } else {
        slog("[BOOT] ALLOC FAILED\r\n");
        return Err(Status::LOAD_ERROR.into());
    };
    unsafe {
        core::ptr::copy_nonoverlapping(ktmp.as_ptr(), kernel_base as *mut u8, ksize);
        boot::free_pages(ktmp, kpages + 1)?;
    }
    slog("[BOOT] kernel copied\r\n");

    // Load user ELF to any available address (kernel ELF loader handles mapping).
    let (utmp, usize) = read_image(cstr16!("\\EFI\\NEO\\USER.ELF"))?;
    slog("[BOOT] user ELF read\r\n");

    // Load individual user programs from /BIN/.
    //
    // The list is generated at build time from tools/programs.list (see
    // bootloader/build.rs). It used to be hand-maintained here and had drifted
    // to 17 entries while the build produced 31, so programs such as `ls` were
    // never staged: spawn_user_by_name then ran on a zeroed UserProg and the
    // ring-3 task faulted at rip=0 straight after spawn.
    let mut user_progs = [UserProg::ZERO; MAX_USER_PROGS];
    let mut nuser_progs: u64 = 0;
    let prog_names: &[&str] = PROGRAM_NAMES;
    for name in prog_names.iter() {
        if nuser_progs as usize >= MAX_USER_PROGS { break; }
        // Build path: \BIN\<name>
        let mut path_buf = [0u16; 64];
        let prefix = b"\\BIN\\";
        let mut idx = 0;
        for &b in prefix.iter() { path_buf[idx] = b as u16; idx += 1; }
        for b in name.bytes() { path_buf[idx] = b as u16; idx += 1; }
        path_buf[idx] = 0;
        // SAFETY: we just built a valid null-terminated UCS-2 string.
        let path = unsafe { CStr16::from_u16_with_nul_unchecked(&path_buf[..idx+1]) };
        match read_image(path) {
            Ok((ptr, size)) => {
                let mut prog_name = [0u8; 16];
                for (i, b) in name.bytes().enumerate().take(15) {
                    prog_name[i] = b;
                }
                user_progs[nuser_progs as usize] = UserProg {
                    name: prog_name,
                    elf_base: ptr.as_ptr() as u64,
                    elf_len: size as u64,
                };
                nuser_progs += 1;
            }
            Err(_) => {} // skip missing programs
        }
    }

    // Build initrd archive from staged user programs.
    // Format: [u32 num_entries][entry: u32 name_len, u32 data_len, name..., data...]...
    let mut initrd_size: usize = 4; // num_entries
    for i in 0..nuser_progs as usize {
        let p = &user_progs[i];
        if p.elf_base == 0 || p.elf_len == 0 { continue; }
        let name_len = p.name.iter().position(|&b| b == 0).unwrap_or(16);
        initrd_size += 4 + 4 + name_len + p.elf_len as usize;
    }
    let initrd_pages = initrd_size.div_ceil(0x1000).max(1);
    let initrd_alloc = boot::allocate_pages(
        boot::AllocateType::AnyPages, boot::MemoryType::LOADER_DATA, initrd_pages,
    );
    let (initrd_base_val, initrd_size_val) = match initrd_alloc {
        Ok(ptr) => {
            let buf = ptr.as_ptr();
            unsafe {
                // Zero the buffer.
                for i in 0..initrd_pages * 4096 {
                    *buf.add(i) = 0u8;
                }
                let mut off = 0usize;
                // Write num_entries (little-endian u32).
                let ne = nuser_progs as u32;
                *buf.add(off) = ne as u8;
                *buf.add(off+1) = (ne >> 8) as u8;
                *buf.add(off+2) = (ne >> 16) as u8;
                *buf.add(off+3) = (ne >> 24) as u8;
                off += 4;
                for i in 0..nuser_progs as usize {
                    let p = &user_progs[i];
                    if p.elf_base == 0 || p.elf_len == 0 { continue; }
                    let name_len = p.name.iter().position(|&b| b == 0).unwrap_or(16);
                    // Write name_len.
                    let nl = name_len as u32;
                    *buf.add(off) = nl as u8;
                    *buf.add(off+1) = (nl >> 8) as u8;
                    *buf.add(off+2) = (nl >> 16) as u8;
                    *buf.add(off+3) = (nl >> 24) as u8;
                    off += 4;
                    // Write data_len.
                    let dl = p.elf_len as u32;
                    *buf.add(off) = dl as u8;
                    *buf.add(off+1) = (dl >> 8) as u8;
                    *buf.add(off+2) = (dl >> 16) as u8;
                    *buf.add(off+3) = (dl >> 24) as u8;
                    off += 4;
                    // Copy name.
                    for j in 0..name_len {
                        *buf.add(off) = p.name[j];
                        off += 1;
                    }
                    // Copy ELF data byte-by-byte.
                    let src = p.elf_base as *const u8;
                    for j in 0..p.elf_len as usize {
                        *buf.add(off) = *src.add(j);
                        off += 1;
                    }
                }
            }
            slog("[BOOT] initrd built\r\n");
            (ptr.as_ptr() as u64, initrd_size as u64)
        }
        Err(_) => {
            slog("[BOOT] initrd alloc failed\r\n");
            (0u64, 0u64)
        }
    };

    let mut boot_info = BootInfo {
        magic: BOOT_MAGIC,
        fb_base, fb_size,
        width: w as u32, height: h as u32,
        stride: stride as u32, pixel_format: pixfmt,
        mmap_len: 0,
        mmap: [MemRegion::ZERO; MMAP_MAX],
        user_base: utmp.as_ptr() as u64,
        user_len: usize as u64,
        nuser_progs,
        user_progs,
        initrd_base: initrd_base_val,
        initrd_len: initrd_size_val,
    };

    // OVMF 202608+ marks all pages as NX after ExitBootServices.
    // Build our own identity-map page table BEFORE ExitBootServices,
    // covering [0, 4GiB) with 2M RWX pages. Switch CR3 to it so the
    // firmware's NX enforcement doesn't affect us.
    let pt_pages = boot::allocate_pages(
        boot::AllocateType::AnyPages, boot::MemoryType::LOADER_DATA, 6,
    ).map_err(|_| Status::LOAD_ERROR)?;
    let pt_base = pt_pages.as_ptr() as u64;
    unsafe {
        core::ptr::write_bytes(pt_base as *mut u8, 0, 6 * 4096);
        let pml4 = pt_base as *mut u64;
        let pdpt = (pt_base + 4096) as *mut u64;
        let pd0  = (pt_base + 8192) as *mut u64;
        let pd1  = (pt_base + 12288) as *mut u64;
        let pd2  = (pt_base + 16384) as *mut u64;
        let pd3  = (pt_base + 20480) as *mut u64;
        for gi in 0..4u64 {
            let pd = match gi { 0 => pd0, 1 => pd1, 2 => pd2, _ => pd3 };
            for i in 0..512u64 {
                *pd.add(i as usize) = (gi << 30) | (i << 21) | 0x83; // P|W|PS
            }
            *pdpt.add(gi as usize) = (pd as u64) | 0x03; // P|W
        }
        *pml4 = (pdpt as u64) | 0x03;
        core::arch::asm!("mov cr3, {}", in(reg) pml4 as u64, options(nostack));
    }
    slog("[BOOT] own pagetable active\r\n");

    slog("[BOOT] exiting boot services\r\n");
    let map = unsafe { boot::exit_boot_services(Some(boot::MemoryType::LOADER_DATA)) };

    let mut n = 0usize;
    for d in map.entries() {
        if d.ty == boot::MemoryType::CONVENTIONAL && n < MMAP_MAX {
            boot_info.mmap[n] = MemRegion { base: d.phys_start, len: d.page_count * 4096 };
            n += 1;
        }
    }
    boot_info.mmap_len = n as u64;

    // Copy boot_info to low address in our identity map.
    let bi_dest = 0x8000u64 as *mut BootInfo;
    unsafe { core::ptr::write(bi_dest, boot_info) };

    // Jump to kernel via function pointer. The kernel is at KERNEL_LOAD_ADDR
    // which is in our identity-mapped region.
    type KernelEntry = unsafe extern "sysv64" fn(*const BootInfo) -> !;
    let entry: KernelEntry = unsafe { core::mem::transmute(kernel_base as usize) };
    unsafe { entry(bi_dest) };
}

#[uefi::entry]
fn efi_main() -> Status {
    match run() {
        Ok(()) => Status::SUCCESS,
        Err(e) => {
            println!("[BOOT] FAILED: {:?}", e.status());
            e.status()
        }
    }
}
// force rebuild
