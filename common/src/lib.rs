//! NEO-OS-3 bootloader <-> kernel handoff types.
//! Shared by both crates so the ABI can't drift.
//!
//! `no_std`: this crate is linked into freestanding bootloader/kernel.

#![no_std]

pub mod syscall;

/// Physical load address of KERNEL.BIN.
pub const KERNEL_LOAD_ADDR: u64 = 0x40_0000; // 4 MiB
/// Virtual/physical address of user program segments (must match kernel.ld).
pub const USER_LOAD_ADDR: u64 = 0x80_0000; // 8 MiB
/// Magic checked by the kernel on entry.
pub const BOOT_MAGIC: u32 = 0x4F454E33; // "NEO3" LE-ish

/// Framebuffer pixel format reported by the bootloader (GOP).
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PixelFormat {
    Rgb = 0,
    Bgr = 1,
}

/// Max usable-RAM regions passed to the kernel (OVMF gives ~dozens).
pub const MMAP_MAX: usize = 128;

/// One Conventional (free) RAM range: base + length in bytes.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct MemRegion {
    pub base: u64,
    pub len: u64,
}

impl MemRegion {
    pub const ZERO: MemRegion = MemRegion { base: 0, len: 0 };
}

/// Initrd archive format: simple binary archive of named files.
/// Layout: [u32 num_entries] [entry...] where each entry is:
///   [u32 name_len] [u32 data_len] [name bytes] [data bytes]
/// All integers are little-endian. Names are NOT null-terminated.
pub const INITRD_MAGIC: u32 = 0x494E5244; // "INRD"

/// One entry in the initrd archive (parsed by kernel).
#[derive(Debug, Clone, Copy)]
pub struct InitrdEntry {
    pub name_offset: usize, // offset within initrd buffer
    pub name_len: usize,
    pub data_offset: usize, // offset within initrd buffer
    pub data_len: usize,
}

/// Max user programs the bootloader can stage.
pub const MAX_USER_PROGS: usize = 16;

/// One staged user program: name + ELF data in memory.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct UserProg {
    pub name: [u8; 16],   // null-terminated ASCII name
    pub elf_base: u64,    // pointer to ELF data in memory
    pub elf_len: u64,     // ELF file size
}

impl UserProg {
    pub const ZERO: UserProg = UserProg { name: [0; 16], elf_base: 0, elf_len: 0 };
}

/// Handoff block passed by pointer from bootloader to kernel `_start`.
/// `repr(C)` so both sides agree on layout.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct BootInfo {
    pub magic: u32,
    pub fb_base: u64,
    pub fb_size: u64,
    pub width: u32,
    pub height: u32,
    pub stride: u32, // pixels per scanline
    pub pixel_format: PixelFormat,
    pub mmap_len: u64,
    pub mmap: [MemRegion; MMAP_MAX],
    pub user_base: u64, // default USER.ELF staging address (0 if absent)
    pub user_len: u64,  // default USER.ELF file size in bytes
    pub nuser_progs: u64,
    pub user_progs: [UserProg; MAX_USER_PROGS],
    pub initrd_base: u64, // initrd archive base address (0 if absent)
    pub initrd_len: u64,  // initrd archive size
}
