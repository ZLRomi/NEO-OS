//! ELF64 loader for user-space programs.
//!
//! Parses an ELF64 binary from a memory buffer, allocates physical frames
//! for each PT_LOAD segment, copies data, zero-fills BSS, and builds a
//! per-task page table with correct permissions.
//!
//! This module is `#![no_std]` — no heap allocations; all state lives in
//! fixed-size arrays on the stack or in the returned `ElfLoadResult`.

use crate::mem::{self, FrameAlloc, FRAME, PROT_USER_CODE, PROT_UDATA, ADDR_MASK};

// ---------------------------------------------------------------------------
// ELF64 constants
// ---------------------------------------------------------------------------

const ELFMAGIC: u32 = 0x464c457f; // "\x7fELF" as little-endian u32
const ELFCLASS64: u8 = 2;
const ELFDATA2LSB: u8 = 1; // little-endian

const ET_EXEC: u16 = 2;
const ET_DYN: u16 = 3;

const PT_LOAD: u32 = 1;
const PT_NOTE: u32 = 4;
const PT_GNU_STACK: u32 = 0x6474e551;

const PF_X: u32 = 1;
const PF_W: u32 = 2;
const PF_R: u32 = 4;

/// Default load base for ET_DYN (PIE) binaries.
const DYN_BASE: u64 = 0x40_0000;

/// Maximum number of PT_LOAD segments we support.
pub const MAX_SEGMENTS: usize = 16;

// ---------------------------------------------------------------------------
// ELF64 structures (repr(C), read via unaligned accessors)
// ---------------------------------------------------------------------------

#[repr(C)]
struct Elf64Header {
    e_ident: [u8; 16],
    e_type: u16,
    e_machine: u16,
    e_version: u32,
    e_entry: u64,
    e_phoff: u64,
    e_shoff: u64,
    e_flags: u32,
    e_ehsize: u16,
    e_phentsize: u16,
    e_phnum: u16,
    e_shentsize: u16,
    e_shnum: u16,
    e_shstrndx: u16,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Elf64Phdr {
    p_type: u32,
    p_flags: u32,
    p_offset: u64,
    p_vaddr: u64,
    p_paddr: u64,
    p_filesz: u64,
    p_memsz: u64,
    p_align: u64,
}

/// ELF64 section header (used as fallback when no program headers exist).
#[repr(C)]
#[derive(Clone, Copy)]
struct Elf64Shdr {
    sh_name: u32,
    sh_type: u32,
    sh_flags: u64,
    sh_addr: u64,
    sh_offset: u64,
    sh_size: u64,
    sh_link: u32,
    sh_info: u32,
    sh_addralign: u64,
    sh_entsize: u64,
}

// Section header types
const SHT_PROGBITS: u32 = 1;
const SHT_NOBITS: u32 = 8;

// Section flags
const SHF_ALLOC: u64 = 0x2;
const SHF_WRITE: u64 = 0x1;
const SHF_EXECINSTR: u64 = 0x4;

// ---------------------------------------------------------------------------
// Error types
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ElfError {
    BadMagic,
    NotElf64,
    NotLittleEndian,
    UnsupportedType,
    InvalidPhdr,
    SegmentOutOfBounds,
    TooManySegments,
    EmptyImage,
}

// ---------------------------------------------------------------------------
// Load result
// ---------------------------------------------------------------------------

/// One mapped segment: virtual address, physical frame base, length in
/// bytes, and page-table protection flags.
#[derive(Clone, Copy)]
pub struct ElfSegment {
    pub vaddr: u64,
    pub phys: u64,
    pub len: u64,
    pub prot: u64,
}

/// Result of a successful ELF load: entry point + segment map.
pub struct ElfLoadResult {
    pub entry_point: u64,
    pub segments: [ElfSegment; MAX_SEGMENTS],
    pub nsegments: usize,
    pub total_mapped_pages: u64,
}

// ---------------------------------------------------------------------------
// Unaligned read helpers
// ---------------------------------------------------------------------------

unsafe fn read_u16(p: *const u8) -> u16 {
    core::ptr::read_unaligned(p as *const u16)
}

unsafe fn read_u32(p: *const u8) -> u32 {
    core::ptr::read_unaligned(p as *const u32)
}

unsafe fn read_u64(p: *const u8) -> u64 {
    core::ptr::read_unaligned(p as *const u64)
}

// ---------------------------------------------------------------------------
// Permission mapping
// ---------------------------------------------------------------------------

/// Map ELF p_flags to our page-table protection bits.
fn elf_flags_to_prot(flags: u32) -> u64 {
    let r = flags & PF_R != 0;
    let w = flags & PF_W != 0;
    let x = flags & PF_X != 0;
    match (r, w, x) {
        // R+X → code (RX, user)
        (true, false, true) => PROT_USER_CODE,
        // R+W → data (RW+NX, user)
        (true, true, false) => PROT_UDATA,
        // R only → read-only data (R+NX, user)
        (true, false, false) => mem::PTE_P | mem::PTE_U | mem::PTE_NX,
        // R+W+X → mixed code+data segment (RWX, user). This happens when
        // objcopy -I binary merges everything into one section. We must
        // allow execute for the code to run.
        (true, true, true) => mem::PTE_P | mem::PTE_W | mem::PTE_U,
        // Anything else (e.g. W-only) → deny all user access.
        _ => mem::PTE_P | mem::PTE_U | mem::PTE_NX,
    }
}

// ---------------------------------------------------------------------------
// align helpers
// ---------------------------------------------------------------------------

fn align_up(x: u64, a: u64) -> u64 {
    if a == 0 { x } else { x.div_ceil(a) * a }
}

// ---------------------------------------------------------------------------
// load_elf
// ---------------------------------------------------------------------------

/// Parse and load an ELF64 binary from a contiguous memory buffer.
///
/// # Safety
/// `elf_data` must point to at least `elf_len` readable bytes. The caller
/// owns the buffer; this function does not free it. `alloc` must be a
/// valid, initialized `FrameAlloc`.
pub unsafe fn load_elf(
    elf_data: *const u8,
    elf_len: u64,
    alloc: &mut FrameAlloc,
) -> Result<ElfLoadResult, ElfError> {
    if elf_len < core::mem::size_of::<Elf64Header>() as u64 {
        return Err(ElfError::BadMagic);
    }

    // -- Validate header ---------------------------------------------------
    let magic = read_u32(elf_data);
    if magic != ELFMAGIC {
        return Err(ElfError::BadMagic);
    }
    let ei_class = *elf_data.add(4);
    if ei_class != ELFCLASS64 {
        return Err(ElfError::NotElf64);
    }
    let ei_data = *elf_data.add(5);
    if ei_data != ELFDATA2LSB {
        return Err(ElfError::NotLittleEndian);
    }

    let e_type = read_u16(elf_data.add(16));
    if e_type != ET_EXEC && e_type != ET_DYN {
        return Err(ElfError::UnsupportedType);
    }

    let e_entry = read_u64(elf_data.add(24));
    let e_phoff = read_u64(elf_data.add(32));
    let e_shoff = read_u64(elf_data.add(40));
    let e_phentsize = read_u16(elf_data.add(42));
    let e_phnum = read_u16(elf_data.add(44));
    let e_shentsize = read_u16(elf_data.add(58));
    let e_shnum = read_u16(elf_data.add(60));

    let has_phdrs = e_phoff != 0 && e_phnum != 0
        && e_phentsize >= core::mem::size_of::<Elf64Phdr>() as u16;

    let has_shdrs = e_shoff != 0 && e_shnum != 0
        && e_shentsize >= core::mem::size_of::<Elf64Shdr>() as u16;

    if !has_phdrs && !has_shdrs {
        return Err(ElfError::InvalidPhdr);
    }

    // Bounds-check program header table if present.
    if has_phdrs {
        let ph_end = e_phoff + (e_phnum as u64) * (e_phentsize as u64);
        if ph_end > elf_len {
            return Err(ElfError::InvalidPhdr);
        }
    }

    // -- Determine load bias -----------------------------------------------
    let load_bias: u64 = if e_type == ET_DYN { DYN_BASE } else { 0 };

    // -- Synthesize segment list from phdrs or shdrs -----------------------
    // We build a unified list of (offset, vaddr, filesz, memsz, flags) tuples.
    #[derive(Clone, Copy)]
    struct SegInfo {
        offset: u64,
        vaddr: u64,
        filesz: u64,
        memsz: u64,
        flags: u32, // PF_R/PF_W/PF_X
    }
    let mut segs: [SegInfo; MAX_SEGMENTS] = [SegInfo { offset: 0, vaddr: 0, filesz: 0, memsz: 0, flags: 0 }; MAX_SEGMENTS];
    let mut seg_count: usize = 0;

    if has_phdrs {
        for i in 0..e_phnum as usize {
            let ph_off = e_phoff + (i as u64) * (e_phentsize as u64);
            let ph_ptr = elf_data.add(ph_off as usize);
            let p_type = read_u32(ph_ptr);
            if p_type != PT_LOAD { continue; }
            if seg_count >= MAX_SEGMENTS { return Err(ElfError::TooManySegments); }
            let p_flags = read_u32(ph_ptr.add(4));
            let p_offset = read_u64(ph_ptr.add(16));
            let p_vaddr = read_u64(ph_ptr.add(24));
            let p_filesz = read_u64(ph_ptr.add(32));
            let p_memsz = read_u64(ph_ptr.add(40));
            if p_memsz == 0 { continue; }
            if p_filesz > 0 && p_offset + p_filesz > elf_len { return Err(ElfError::SegmentOutOfBounds); }
            if p_memsz < p_filesz { return Err(ElfError::SegmentOutOfBounds); }
            segs[seg_count] = SegInfo { offset: p_offset, vaddr: p_vaddr, filesz: p_filesz, memsz: p_memsz, flags: p_flags };
            seg_count += 1;
        }
    } else {
        // Fallback: synthesize segments from allocatable section headers.
        //
        // Sections are mapped *per page*, so two sections that share a page
        // must end up in the same segment. Mapping them separately makes the
        // later one overwrite the earlier one's page: `.data` (8 bytes at
        // 0x802000) and `.got` (at 0x802008) share page 0x802000, and the
        // `.got` segment used to remap that page to a fresh frame, wiping
        // `.data`. The result was a process that faulted at rip=0 because the
        // GOT slot it jumped through had been zeroed.
        //
        // Fix: coalesce sections whose page ranges touch or overlap into one
        // segment, widening flags to the union.
        let mut i = 0usize;
        while i < e_shnum as usize {
            let sh_off = e_shoff + (i as u64) * (e_shentsize as u64);
            let sh_ptr = elf_data.add(sh_off as usize);
            let sh_type = read_u32(sh_ptr.add(4));
            let sh_flags = read_u64(sh_ptr.add(8));
            let sh_addr = read_u64(sh_ptr.add(16));
            let sh_offset = read_u64(sh_ptr.add(24));
            let sh_size = read_u64(sh_ptr.add(32));

            if sh_flags & SHF_ALLOC == 0
                || (sh_type != SHT_PROGBITS && sh_type != SHT_NOBITS)
                || sh_size == 0
            {
                i += 1;
                continue;
            }

            let mut pf: u32 = PF_R;
            if sh_flags & SHF_WRITE != 0 { pf |= PF_W; }
            if sh_flags & SHF_EXECINSTR != 0 { pf |= PF_X; }

            let mut seg_vaddr = sh_addr;
            let mut seg_off = sh_offset;
            let mut seg_filesz = if sh_type == SHT_NOBITS { 0 } else { sh_size };
            let mut seg_memsz = sh_size;
            let seg_end = sh_addr + sh_size;

            // Absorb following sections that live in the same page range.
            let mut j = i + 1;
            while j < e_shnum as usize {
                let o2 = e_shoff + (j as u64) * (e_shentsize as u64);
                let p2 = elf_data.add(o2 as usize);
                let t2 = read_u32(p2.add(4));
                let f2 = read_u64(p2.add(8));
                let a2 = read_u64(p2.add(16));
                let off2 = read_u64(p2.add(24));
                let sz2 = read_u64(p2.add(32));
                if f2 & SHF_ALLOC == 0
                    || (t2 != SHT_PROGBITS && t2 != SHT_NOBITS)
                    || sz2 == 0
                {
                    j += 1;
                    continue;
                }
                // Same page as the current segment?
                let page = align_up(seg_end, FRAME);
                if a2 >= page {
                    break;
                }
                // Extend the segment to cover this section.
                if a2 < seg_vaddr {
                    let back = seg_vaddr - a2;
                    seg_vaddr = a2;
                    seg_off = off2;
                    seg_filesz += back;
                    seg_memsz += back;
                }
                let new_end = a2 + sz2;
                if new_end > seg_vaddr + seg_memsz {
                    seg_memsz = new_end - seg_vaddr;
                    if t2 != SHT_NOBITS {
                        seg_filesz = seg_memsz;
                    }
                }
                if f2 & SHF_WRITE != 0 { pf |= PF_W; }
                if f2 & SHF_EXECINSTR != 0 { pf |= PF_X; }
                j += 1;
            }

            if seg_count >= MAX_SEGMENTS { return Err(ElfError::TooManySegments); }
            if seg_filesz > 0 && seg_off + seg_filesz > elf_len {
                return Err(ElfError::SegmentOutOfBounds);
            }
            segs[seg_count] = SegInfo {
                offset: seg_off,
                vaddr: seg_vaddr,
                filesz: seg_filesz,
                memsz: seg_memsz,
                flags: pf,
            };
            seg_count += 1;
            i = j;
        }
    }

    if seg_count == 0 {
        return Err(ElfError::EmptyImage);
    }

    // -- Allocate, copy, build result --------------------------------------
    let mut result = ElfLoadResult {
        entry_point: e_entry + load_bias,
        segments: [ElfSegment { vaddr: 0, phys: 0, len: 0, prot: 0 }; MAX_SEGMENTS],
        nsegments: 0,
        total_mapped_pages: 0,
    };

    for i in 0..seg_count {
        let s = &segs[i];
        let pages = align_up(s.memsz, FRAME) / FRAME;
        let phys = alloc.alloc_contiguous(pages);

        // Copy file content.
        if s.filesz > 0 {
            core::ptr::copy_nonoverlapping(
                elf_data.add(s.offset as usize),
                phys as *mut u8,
                s.filesz as usize,
            );
        }
        // BSS (memsz > filesz) is already zero from alloc_contiguous.

        let vaddr = s.vaddr + load_bias;
        let prot = elf_flags_to_prot(s.flags);

        let idx = result.nsegments;
        result.segments[idx] = ElfSegment {
            vaddr,
            phys,
            len: s.memsz,
            prot,
        };
        result.nsegments += 1;
        result.total_mapped_pages += pages;
    }

    Ok(result)
}

// ---------------------------------------------------------------------------
// build_user_page_table
// ---------------------------------------------------------------------------

/// Build a per-task PML4 that maps the given ELF segments plus a user stack.
///
/// Each segment's pages are mapped with the protection recorded during
/// `load_elf`. Stack pages are mapped RW+NX (`PROT_UDATA`).
///
/// # Safety
/// `segments[..nsegments]` must be valid. `stack_base` must be a
/// page-aligned physical address with at least `stack_pages` frames
/// already allocated. `alloc` must be initialized.
///
/// Returns the physical address of the new PML4.
pub unsafe fn build_user_page_table(
    segments: &[ElfSegment; MAX_SEGMENTS],
    nsegments: usize,
    stack_base: u64,
    stack_pages: u64,
    alloc: &mut FrameAlloc,
) -> u64 {
    let kernel_root = mem::kernel_pml4();

    // Shallow-copy PML4: all entries initially point at kernel-shared tables.
    let root = alloc.alloc_contiguous(1);
    core::ptr::copy_nonoverlapping(kernel_root as *const u64, root as *mut u64, 512);

    // Map each ELF segment.
    for i in 0..nsegments {
        let seg = &segments[i];
        let pages = align_up(seg.len, FRAME) / FRAME;
        for page in 0..pages {
            let virt = seg.vaddr + page * FRAME;
            let phys = seg.phys + page * FRAME;
            mem::ensure_private_path(root, virt, alloc);
            mem::replace_page(root, virt, phys, seg.prot);
        }
    }

    // Map the user stack at a FIXED VIRTUAL ADDRESS, backed by whatever
    // physical frames the caller allocated.
    //
    // The original code used `stack_base` for both the virtual and the
    // physical address, relying on the identity map. That silently welded the
    // stack's virtual address to a physical frame number, which has two
    // consequences: `fork` cannot give a child its own stack (the child's
    // stack pointer would still name the parent's frames), and the stack's
    // virtual address changes from run to run as the allocator moves.
    //
    // Decoupling them makes the layout process-independent: every process
    // sees its stack at the same address, but owns different frames.
    for page in 0..stack_pages {
        let virt = crate::task::USER_STACK_BASE + page * FRAME;
        let phys = stack_base + page * FRAME;
        mem::ensure_private_path(root, virt, alloc);
        mem::replace_page(root, virt, phys, PROT_UDATA);
    }

    root
}

// ---------------------------------------------------------------------------
// destroy_user_page_table
// ---------------------------------------------------------------------------

/// Free the per-task PML4 and any privately-cloned intermediate page-table
/// nodes.  Segment frames themselves are freed separately by the caller
/// (reap_exited).
///
/// # Safety
/// `root` must be a PML4 that was created by `build_user_page_table` and
/// is no longer the active CR3.  `segments[..nsegments]` must describe
/// the same mappings that were installed during build.
pub unsafe fn destroy_user_page_table(
    root: u64,
    segments: &[ElfSegment; MAX_SEGMENTS],
    nsegments: usize,
    _stack_phys: u64,
    stack_pages: u64,
    alloc: &mut FrameAlloc,
) {
    let kernel_root = mem::kernel_pml4();

    // Collect unique private intermediate-table frames to free.
    const MAX_FREE: usize = 128;
    let mut to_free: [u64; MAX_FREE] = [0u64; MAX_FREE];
    let mut nfree: usize = 0;

    let mut push_unique = |frame: u64| {
        for k in 0..nfree {
            if to_free[k] == frame { return; }
        }
        if nfree < MAX_FREE {
            to_free[nfree] = frame;
            nfree += 1;
        }
    };

    let mut collect_path = |virt: u64| {
        let indices: [usize; 3] = [
            ((virt >> 39) & 0x1FF) as usize,
            ((virt >> 30) & 0x1FF) as usize,
            ((virt >> 21) & 0x1FF) as usize,
        ];
        let mut phys = root;
        for l in 0..3 {
            let entry = mem::table_mut(phys)[indices[l]];
            if entry & mem::PTE_P == 0 || entry & mem::PTE_PS != 0 { break; }
            let child = entry & ADDR_MASK;
            // Compare against kernel's corresponding table at this level.
            let kern_child = walk_kernel_table(kernel_root, virt, l);
            if child != kern_child {
                push_unique(child);
            }
            phys = child;
        }
    };

    // Walk all ELF segment pages.
    for i in 0..nsegments {
        let seg = &segments[i];
        let pages = align_up(seg.len, FRAME) / FRAME;
        for page in 0..pages {
            collect_path(seg.vaddr + page * FRAME);
        }
    }
    // Walk stack pages. The stack lives at a fixed *virtual* address now, so
    // the walk uses USER_STACK_BASE; `_stack_phys` is kept in the signature
    // for callers that still free the frames themselves.
    for page in 0..stack_pages {
        collect_path(crate::task::USER_STACK_BASE + page * FRAME);
    }

    // Free collected private intermediate tables.
    for k in 0..nfree {
        alloc.free_frames(to_free[k], FRAME);
    }
    // Free the PML4 itself (always private).
    alloc.free_frames(root, FRAME);
}

/// Walk the kernel page table to the intermediate table at `level`
/// (0=PML4→PDPT, 1=PDPT→PD, 2=PD→PT) for virtual address `virt`.
unsafe fn walk_kernel_table(root: u64, virt: u64, level: usize) -> u64 {
    let indices: [usize; 3] = [
        ((virt >> 39) & 0x1FF) as usize,
        ((virt >> 30) & 0x1FF) as usize,
        ((virt >> 21) & 0x1FF) as usize,
    ];
    let mut phys = root;
    for l in 0..=level {
        let entry = mem::table_mut(phys)[indices[l]];
        phys = entry & ADDR_MASK;
    }
    phys
}
