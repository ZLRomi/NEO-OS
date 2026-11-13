//! M3 physical memory: frame allocator over the bootloader's Conventional
//! map, plus our own identity page tables.
//!
//! M6.1 adds frame recycling: freed ranges land on a sorted, coalesced
//! "recycled" list; contiguous allocations prefer it before the bump
//! path. The bump path stays strictly monotonic (never re-offers frames
//! below `next` -- that bug once zeroed our own page tables).
//!
//! M9: uniform 4K pages with per-page protection. Kernel .text and .rodata
//! are supervisor-only. Each ring3 task gets a private page-table root,
//! user image and stack; kernel mappings stay shared and supervisor-only.
//! User buffers passed to syscalls are validated against the active root.

use neo_common::{BootInfo, KERNEL_LOAD_ADDR, MMAP_MAX, USER_LOAD_ADDR};

use crate::{kinfo, kwarn};

unsafe extern "C" {
    static _kernel_end: u8;
    static _text_start: u8;
    static _text_end: u8;
    static _rodata_start: u8;
    static _rodata_end: u8;
    // User image sub-sections (linker-defined). Addresses are virtual = physical
    // under the identity map; used to set per-range permissions at spawn.
    static _user_text_start: u8;
    static _user_text_end: u8;
    static _user_data_start: u8;
    static _user_data_end: u8;
    static _user_bss_start: u8;
    static _user_bss_end: u8;
}

pub const FRAME: u64 = 4096;
const TWO_MIB: u64 = 0x20_0000;
const RECYCLED_MAX: usize = 64;

pub(crate) const PTE_P: u64 = 1 << 0;
pub(crate) const PTE_W: u64 = 1 << 1;
pub(crate) const PTE_U: u64 = 1 << 2; // user/supervisor: ring3 can access (NOT bit 8!)
pub(crate) const PTE_PS: u64 = 1 << 7;
pub(crate) const PTE_COW: u64 = 1 << 9; // Software bit: Copy-on-Write marker
pub(crate) const PTE_NX: u64 = 1 << 63; // No-Execute (needs EFER.NXE)
// Intermediate tables stay traversable by ring3; leaves enforce policy.
const TABLE_FLAGS: u64 = PTE_P | PTE_W | PTE_U;

/// Leaf protection sets (masked by PROT_MASK in set_prot).
pub const PROT_KERN: u64 = PTE_P | PTE_W; // supervisor RWX (default)
pub const PROT_KTEXT: u64 = PTE_P; // kernel .text: supervisor +x, no W
pub const PROT_KRO: u64 = PTE_P | PTE_NX; // kernel .rodata: supervisor RO
pub const PROT_USER_CODE: u64 = PTE_P | PTE_U; // per-task user image: RX only
pub const PROT_UDATA: u64 = PTE_P | PTE_W | PTE_U | PTE_NX; // user stacks
const PROT_MASK: u64 = PTE_P | PTE_W | PTE_U | PTE_NX;
pub(crate) const ADDR_MASK: u64 = 0x000F_FFFF_FFFF_F000;

fn align_up(x: u64, a: u64) -> u64 {
    x.div_ceil(a) * a
}

/// Bump allocator: hands out 4K frames from Conventional regions.
pub struct FrameAlloc {
    regions: [(u64, u64); MMAP_MAX], // (base, len)
    nregions: usize,
    next: u64, // next candidate frame address
    used: u64, // frames currently handed out
    total: u64, // usable bytes across regions
    // Freed ranges awaiting reuse: sorted by base, adjacent coalesced.
    recycled: [(u64, u64); RECYCLED_MAX], // (base, len) in bytes
    nrecycled: usize,
    // Allocation tracking for leak detection.
    pub alloc_count: u64,
    pub free_count: u64,
}

impl FrameAlloc {
    pub const fn empty() -> FrameAlloc {
        FrameAlloc {
            regions: [(0, 0); MMAP_MAX],
            nregions: 0,
            next: 0,
            used: 0,
            total: 0,
            recycled: [(0, 0); RECYCLED_MAX],
            nrecycled: 0,
            alloc_count: 0,
            free_count: 0,
        }
    }

    pub fn init(&mut self, bi: &BootInfo) {
        // Our own image must never be handed out, even if the loader
        // under-reserved it (e.g. .bss missing from the flat image).
        let img_start = KERNEL_LOAD_ADDR;
        let img_end = align_up(unsafe { &_kernel_end as *const u8 as u64 }, FRAME);
        let mut n = 0usize;
        let mut total = 0u64;
        for r in bi
            .mmap
            .iter()
            .take(bi.mmap_len.min(MMAP_MAX as u64) as usize)
        {
            if n + 1 >= MMAP_MAX {
                break; // one entry can split in two; never overflow
            }
            let mut base = align_up(r.base, FRAME);
            let end = r.base.saturating_add(r.len).min(1 << 32);
            if base >= end {
                continue;
            }
            // Carve out [img_start, img_end) if overlapped.
            if base < img_end && end > img_start {
                if base < img_start && n < MMAP_MAX {
                    self.regions[n] = (base.max(FRAME), img_start);
                    total += img_start - base.max(FRAME);
                    n += 1;
                }
                base = img_end;
            }
            // Carve out AP trampoline page at 0x8000 and AP stack at 0x9000.
            if base <= 0x8000 && end > 0x8000 {
                if base < 0x8000 && n < MMAP_MAX {
                    self.regions[n] = (base.max(FRAME), 0x8000);
                    total += 0x8000 - base.max(FRAME);
                    n += 1;
                }
                base = 0xA000; // skip trampoline + AP stack pages
            }
            if end > base && base >= FRAME {
                // skip frame 0 (BIOS data area / real-mode vectors)
                self.regions[n] = (base.max(FRAME), end);
                total += end - base.max(FRAME);
                n += 1;
            }
        }
        self.nregions = n;
        self.next = self.regions[0].0;
        self.total = total;
        kinfo!("mem: {} regions, {} MiB usable", n, total / (1 << 20));
    }

    fn alloc_frame(&mut self) -> u64 {
        loop {
            let mut progressed = false;
            for i in 0..self.nregions {
                let (base, end) = self.regions[i];
                let cand = align_up(self.next.max(base), FRAME);
                if cand + FRAME <= end {
                    self.next = cand + FRAME;
                    self.used += 1;
                    unsafe {
                        core::ptr::write_bytes(cand as *mut u8, 0, FRAME as usize);
                    }
                    return cand;
                }
                if end > self.next {
                    self.next = end;
                    progressed = true;
                }
            }
            if !progressed {
                panic!("mem: out of frames");
            }
        }
    }

    /// Contiguous run of `n` frames (for the heap / task stacks).
    ///
    /// Order: recycled ranges first (first fit, taken from the front),
    /// then the strictly monotonic bump path. A candidate is always at
    /// or above the global bump pointer `next` -- never re-offers a
    /// region's front (our page tables at 0x88000 live there; the old
    /// "restart region" candidate handed them out AGAIN and the M6
    /// task-stack zeroing wiped the PML4 mid-boot).
    pub fn alloc_contiguous(&mut self, n: u64) -> u64 {
        let bytes = n * FRAME;
        self.alloc_count += 1;
        for i in 0..self.nrecycled {
            let (b, l) = self.recycled[i];
            if l >= bytes {
                self.recycled[i] = (b + bytes, l - bytes);
                if self.recycled[i].1 == 0 {
                    let mut j = i;
                    while j + 1 < self.nrecycled {
                        self.recycled[j] = self.recycled[j + 1];
                        j += 1;
                    }
                    self.nrecycled -= 1;
                }
                self.used += n;
                unsafe {
                    core::ptr::write_bytes(b as *mut u8, 0, bytes as usize);
                }
                return b;
            }
        }
        let mut next = self.next;
        for i in 0..self.nregions {
            let (base, end) = self.regions[i];
            let cand = align_up(next.max(base), FRAME);
            if cand + bytes <= end {
                self.next = cand + bytes;
                self.used += n;
                unsafe {
                    core::ptr::write_bytes(cand as *mut u8, 0, bytes as usize);
                }
                return cand;
            }
            if end > next {
                next = end; // region exhausted for this size; keep monotonic
            }
        }
        panic!("mem: no contiguous run of {} frames", n);
    }

    /// Return a previously allocated range to the pool. Sorted insert +
    /// adjacent coalesce; leaks (with a warning) if the list is full.
    pub fn free_frames(&mut self, base: u64, len: u64) {
        if len == 0 || base % FRAME != 0 || len % FRAME != 0 {
            kwarn!("mem: free_frames bad range {:#x}+{:#x}", base, len);
            return;
        }
        self.free_count += 1;
        // List full: absorb into an adjacent entry instead of dropping.
        if self.nrecycled >= RECYCLED_MAX {
            for i in 0..self.nrecycled {
                let (b, l) = self.recycled[i];
                if b + l == base {
                    self.recycled[i].1 = l + len;
                    self.used -= len / FRAME;
                    return;
                }
                if base + len == b {
                    self.recycled[i].0 = base;
                    self.recycled[i].1 = l + len;
                    self.used -= len / FRAME;
                    return;
                }
            }
            kwarn!("mem: recycled list full, leaking {:#x}+{:#x}", base, len);
            return;
        }
        let mut pos = 0usize;
        while pos < self.nrecycled && self.recycled[pos].0 < base {
            pos += 1;
        }
        let mut i = self.nrecycled;
        while i > pos {
            self.recycled[i] = self.recycled[i - 1];
            i -= 1;
        }
        self.recycled[pos] = (base, len);
        self.nrecycled += 1;
        // Coalesce with neighbors until stable.
        let mut k = 0usize;
        while k + 1 < self.nrecycled {
            let (b, l) = self.recycled[k];
            let (nb, nl) = self.recycled[k + 1];
            if b + l == nb {
                self.recycled[k].1 = l + nl;
                let mut j = k + 1;
                while j + 1 < self.nrecycled {
                    self.recycled[j] = self.recycled[j + 1];
                    j += 1;
                }
                self.nrecycled -= 1;
            } else {
                k += 1;
            }
        }
        self.used -= len / FRAME;
        kinfo!("mem: freed {:#x}+{:#x}", base, len);
    }

    pub fn used_frames(&self) -> u64 {
        self.used
    }

    pub fn used_bytes(&self) -> u64 {
        self.used * FRAME
    }

    pub fn total_bytes(&self) -> u64 {
        self.total
    }

    /// Hot-add a new memory region. The region must not overlap existing regions.
    /// Returns true on success, false if region table is full or overlaps.
    pub fn hot_add(&mut self, base: u64, len: u64) -> bool {
        let base = align_up(base, FRAME);
        let end = base + len;
        if base >= end || len == 0 { return false; }

        // Check for overlap with existing regions.
        for i in 0..self.nregions {
            let (rb, rl) = self.regions[i];
            let re = rb + rl;
            if base < re && end > rb {
                kwarn!("mem: hot_add {:#x}+{:#x} overlaps region {:#x}+{:#x}", base, len, rb, rl);
                return false;
            }
        }

        if self.nregions >= MMAP_MAX {
            kwarn!("mem: hot_add failed, region table full");
            return false;
        }

        self.regions[self.nregions] = (base, len);
        self.nregions += 1;
        self.total += len;
        kinfo!("mem: hot-added {:#x}+{:#x} ({} KiB)", base, len, len / 1024);
        true
    }

    /// Hot-remove a previously added memory region.
    /// Only succeeds if no frames from this region are currently allocated.
    /// Returns true on success.
    pub fn hot_remove(&mut self, base: u64, len: u64) -> bool {
        let base = align_up(base, FRAME);
        let mut found_idx = None;
        for i in 0..self.nregions {
            if self.regions[i].0 == base && self.regions[i].1 == len {
                found_idx = Some(i);
                break;
            }
        }

        match found_idx {
            Some(idx) => {
                // Check that no recycled blocks fall within this region.
                let end = base + len;
                for r in 0..self.nrecycled {
                    let (rb, rl) = self.recycled[r];
                    if rb >= base && rb < end {
                        kwarn!("mem: hot_remove blocked, recycled block at {:#x}", rb);
                        return false;
                    }
                    if rb + rl > base && rb < end {
                        kwarn!("mem: hot_remove blocked, recycled block overlaps {:#x}", rb);
                        return false;
                    }
                }

                // Remove the region by shifting subsequent entries.
                for i in idx..self.nregions - 1 {
                    self.regions[i] = self.regions[i + 1];
                }
                self.nregions -= 1;
                self.total -= len;
                kinfo!("mem: hot-removed {:#x}+{:#x}", base, len);
                true
            }
            None => {
                kwarn!("mem: hot_remove {:#x}+{:#x} not found", base, len);
                false
            }
        }
    }

    /// Get the number of registered memory regions.
    pub fn region_count(&self) -> usize {
        self.nregions
    }
}

static mut FRAMES: FrameAlloc = FrameAlloc::empty();
static mut OUR_PML4: u64 = 0;

pub(crate) fn table_mut(phys: u64) -> &'static mut [u64; 512] {
    unsafe { &mut *(phys as *mut [u64; 512]) }
}

pub(crate) fn table_ptr(phys: u64) -> *const u64 {
    phys as *const u64
}

fn get_table(parent: u64, idx: usize, alloc: &mut FrameAlloc) -> u64 {
    let t = table_mut(parent);
    if t[idx] & PTE_P == 0 {
        let n = alloc.alloc_frame();
        t[idx] = n | TABLE_FLAGS;
    }
    t[idx] & 0x000F_FFFF_FFFF_F000
}

/// Identity-map [base, base+len) with 4K pages. Both must be 2MB-aligned
/// (callers overmap); walking per 2MB chunk keeps boot fast (~2k chunks
/// for the low 4G instead of 1M walks).
fn map_range_4k(pml4: u64, base: u64, len: u64, flags: u64, alloc: &mut FrameAlloc) {
    debug_assert!(base % TWO_MIB == 0 && len % TWO_MIB == 0);
    let mut chunk = base;
    let end = base + len;
    while chunk < end {
        let pml4e = get_table(pml4, ((chunk >> 39) & 0x1FF) as usize, alloc);
        let pdpt = get_table(pml4e, ((chunk >> 30) & 0x1FF) as usize, alloc);
        let pd = get_table(pdpt, ((chunk >> 21) & 0x1FF) as usize, alloc);
        let pt = table_mut(pd);
        for k in 0..512 {
            if pt[k] & PTE_P == 0 {
                pt[k] = (chunk + k as u64 * FRAME) | flags;
            }
        }
        chunk += TWO_MIB;
    }
}

/// Walk OUR tables: translate a virtual address. Self-test helper.
/// Handles 1G/2M PS entries and 4K PTEs alike.
pub fn translate(virt: u64) -> Option<u64> {
    let pml4 = unsafe { OUR_PML4 };
    if pml4 == 0 {
        return None;
    }
    let mask = 0x000F_FFFF_FFFF_F000;
    let pml4e = table_mut(pml4)[((virt >> 39) & 0x1FF) as usize];
    if pml4e & PTE_P == 0 {
        return None;
    }
    let pdpt_e = table_mut(pml4e & mask)[((virt >> 30) & 0x1FF) as usize];
    if pdpt_e & PTE_P == 0 {
        return None;
    }
    if pdpt_e & PTE_PS != 0 {
        return Some((pdpt_e & !0x3FFF_FFFF) + (virt & 0x3FFF_FFFF));
    }
    let pd_e = table_mut(pdpt_e & mask)[((virt >> 21) & 0x1FF) as usize];
    if pd_e & PTE_P == 0 {
        return None;
    }
    if pd_e & PTE_PS != 0 {
        return Some((pd_e & !0x1F_FFFF) + (virt & 0x1F_FFFF));
    }
    let pt_e = table_mut(pd_e & mask)[((virt >> 12) & 0x1FF) as usize];
    if pt_e & PTE_P == 0 {
        return None;
    }
    Some((pt_e & !0xFFF) + (virt & 0xFFF))
}

fn user_page_ok(pml4: u64, virt: u64) -> bool {
    if pml4 == 0 {
        return false;
    }
    let required = PTE_P | PTE_U;
    let pml4e = table_mut(pml4)[((virt >> 39) & 0x1FF) as usize];
    if pml4e & required != required {
        return false;
    }
    let pdpt_e = table_mut(pml4e & ADDR_MASK)[((virt >> 30) & 0x1FF) as usize];
    if pdpt_e & required != required || pdpt_e & PTE_PS != 0 {
        return false;
    }
    let pd_e = table_mut(pdpt_e & ADDR_MASK)[((virt >> 21) & 0x1FF) as usize];
    if pd_e & required != required || pd_e & PTE_PS != 0 {
        return false;
    }
    let pt_e = table_mut(pd_e & ADDR_MASK)[((virt >> 12) & 0x1FF) as usize];
    pt_e & required == required
}

/// True when every page in `base..base+len` is present and user-readable
/// in the selected address space.
fn user_range_ok_in(pml4: u64, base: u64, len: u64) -> bool {
    if len == 0 {
        return true;
    }
    let Some(end) = base.checked_add(len) else {
        return false;
    };
    if end > (1 << 32) {
        return false;
    }
    let mut addr = base & !(FRAME - 1);
    while addr < end {
        if !user_page_ok(pml4, addr) {
            return false;
        }
        addr = match addr.checked_add(FRAME) {
            Some(next) => next,
            None => return false,
        };
    }
    true
}

/// True when a user buffer is mapped in the currently active address space.
pub fn user_range_ok(base: u64, len: u64) -> bool {
    user_range_ok_in(crate::cpu::read_cr3() & ADDR_MASK, base, len)
}

/// Ensure that the page-table path covering `[virt, virt+FRAME)` under
/// `root` is private: at each level (PML4→PDPT→PD→PT), if the entry
/// still points to a kernel-shared intermediate table, allocate a new
/// table and copy it.  After this call every intermediate node on the
/// path belongs exclusively to `root` and can be freed independently.
///
/// Returns the number of new frames allocated (for accounting / debug).
pub(crate) unsafe fn ensure_private_path(root: u64, virt: u64, alloc: &mut FrameAlloc) -> u32 {
    let mut cloned = 0u32;
    // Walk levels 4→3→2 (stop before leaf PT which replace_page handles).
    let indices: [usize; 3] = [
        ((virt >> 39) & 0x1FF) as usize, // PML4 → PDPT
        ((virt >> 30) & 0x1FF) as usize, // PDPT → PD
        ((virt >> 21) & 0x1FF) as usize, // PD   → PT
    ];
    let mut table_phys = root;
    for level in 0..3 {
        let tbl = table_mut(table_phys);
        let entry = tbl[indices[level]];
        assert!(entry & PTE_P != 0, "mem: ensure_private_path missing entry");
        assert!(entry & PTE_PS == 0, "mem: ensure_private_path hit huge page");
        let child = entry & ADDR_MASK;
        // Check whether this child is already private to `root` by seeing
        // if it was allocated during this create call.  We use a simpler
        // heuristic: if the child address equals the corresponding entry
        // in the kernel PML4 tree at the same level, it is shared and
        // needs cloning.  Walk the kernel tree in parallel.
        let kern_root = OUR_PML4;
        let kern_child = walk_kernel_table(kern_root, virt, level);
        if child == kern_child {
            // Still pointing at the kernel's table — clone it.
            let new_tbl = alloc.alloc_contiguous(1);
            core::ptr::copy_nonoverlapping(child as *const u64, new_tbl as *mut u64, 512);
            tbl[indices[level]] = (entry & !ADDR_MASK) | new_tbl;
            table_phys = new_tbl;
            cloned += 1;
        } else {
            // Already private (cloned earlier for another page in this range).
            table_phys = child;
        }
    }
    cloned
}

/// Walk the kernel page table to the intermediate table at `level`
/// (0=PML4→PDPT, 1=PDPT→PD, 2=PD→PT) for virtual address `virt`.
/// Returns the physical address of that intermediate table.
unsafe fn walk_kernel_table(root: u64, virt: u64, level: usize) -> u64 {
    let indices: [usize; 3] = [
        ((virt >> 39) & 0x1FF) as usize,
        ((virt >> 30) & 0x1FF) as usize,
        ((virt >> 21) & 0x1FF) as usize,
    ];
    let mut phys = root;
    for l in 0..=level {
        let entry = table_mut(phys)[indices[l]];
        phys = entry & ADDR_MASK;
    }
    phys
}

/// Free only the private intermediate tables along the user-mapped
/// paths under `root`.  Mirrors `ensure_private_path`: walks each
/// user-page slot and frees any intermediate table that does NOT match
/// the kernel's corresponding table (i.e. was privately cloned).
unsafe fn free_user_paths(root: u64, image_virt: u64, image_pages: u64, stack_virt: u64, stack_pages: u64, alloc: &mut FrameAlloc) {
    let kern_root = OUR_PML4;
    // Collect unique intermediate-table frames to free (avoid double-free
    // when multiple pages share a PDPT/PD/PT).
    // Max distinct intermediates: 3 levels × (image_pages + stack_pages)
    // but realistically ≤ ~30.  Use a small stack buffer.
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
            let entry = table_mut(phys)[indices[l]];
            if entry & PTE_P == 0 || entry & PTE_PS != 0 { break; }
            let child = entry & ADDR_MASK;
            let kern_child = walk_kernel_table(kern_root, virt, l);
            if child != kern_child {
                push_unique(child);
            }
            phys = child;
        }
    };

    for p in 0..image_pages {
        collect_path(image_virt + p * FRAME);
    }
    for p in 0..stack_pages {
        collect_path(stack_virt + p * FRAME);
    }
    for k in 0..nfree {
        alloc.free_frames(to_free[k], FRAME);
    }
    // Free the PML4 itself (always private).
    alloc.free_frames(root, FRAME);
}

pub(crate) fn replace_page(root: u64, virt: u64, phys: u64, prot: u64) {
    let pml4e = table_mut(root)[((virt >> 39) & 0x1FF) as usize];
    assert!(pml4e & PTE_P != 0, "mem: address-space map missing PML4E");
    let pdpt = pml4e & ADDR_MASK;
    let pdpt_e = table_mut(pdpt)[((virt >> 30) & 0x1FF) as usize];
    assert!(pdpt_e & PTE_P != 0 && pdpt_e & PTE_PS == 0, "mem: address-space map missing PDPTE");
    let pd = pdpt_e & ADDR_MASK;
    let pd_e = table_mut(pd)[((virt >> 21) & 0x1FF) as usize];
    assert!(pd_e & PTE_P != 0 && pd_e & PTE_PS == 0, "mem: address-space map missing PDE");
    let pt = table_mut(pd_e & ADDR_MASK);
    let index = ((virt >> 12) & 0x1FF) as usize;
    assert!(pt[index] & PTE_P != 0, "mem: address-space map missing PTE");
    pt[index] = (phys & ADDR_MASK) | (prot & PROT_MASK);
}

/// Clone the kernel mappings and install private user image and stack pages.
/// The entire page-table tree is private, while supervisor leaf mappings keep
/// the same physical frames. User tasks therefore cannot access one another's
/// writable image or stack mappings.
pub fn create_user_address_space(
    image_len: u64,
    stack_base: u64,
    stack_pages: u64,
) -> (u64, u64, u64) {
    assert!(image_len > 0, "mem: empty user image");
    assert!(stack_pages > 0 && stack_base % FRAME == 0);
    let kernel_root = unsafe { OUR_PML4 };
    assert!(kernel_root != 0, "mem: page tables not initialized");

    let alloc = unsafe { frames() };
    // Shallow-copy PML4: all entries initially point at kernel-shared tables.
    let root = alloc.alloc_contiguous(1);
    unsafe {
        core::ptr::copy_nonoverlapping(kernel_root as *const u64, root as *mut u64, 512);
    }
    let image_pages = image_len.div_ceil(FRAME);
    let image_phys = alloc.alloc_contiguous(image_pages);
    let stack_phys = alloc.alloc_contiguous(stack_pages);
    unsafe {
        core::ptr::copy_nonoverlapping(
            USER_LOAD_ADDR as *const u8,
            image_phys as *mut u8,
            image_len as usize,
        );
    }

    // Make the page-table paths for user pages private, then replace leaves.
    // Code pages (.user.text) get RX; data/bss pages get RW+NX.
    let data_start = unsafe { &_user_data_start as *const u8 as u64 };
    let data_end = unsafe { &_user_data_end as *const u8 as u64 };
    let bss_start = unsafe { &_user_bss_start as *const u8 as u64 };
    let bss_end = unsafe { &_user_bss_end as *const u8 as u64 };
    for page in 0..image_pages {
        let virt = USER_LOAD_ADDR + page * FRAME;
        unsafe { ensure_private_path(root, virt, alloc) };
        let prot = if virt >= data_start && virt < data_end
            || virt >= bss_start && virt < bss_end
        {
            PROT_UDATA   // RW + NX + User
        } else {
            PROT_USER_CODE // RX + User
        };
        replace_page(root, virt, image_phys + page * FRAME, prot);
    }
    for page in 0..stack_pages {
        let virt = stack_base + page * FRAME;
        unsafe { ensure_private_path(root, virt, alloc) };
        replace_page(root, virt, stack_phys + page * FRAME, PROT_UDATA);
    }
    (root, image_phys, stack_phys)
}

/// Reclaim a user address space after switching to another task's CR3/stack.
pub fn destroy_user_address_space(root: u64, image_phys: u64, image_len: u64, stack_phys: u64, stack_pages: u64) {
    assert!(root != 0 && root != unsafe { OUR_PML4 });
    let alloc = unsafe { frames() };
    let image_pages = image_len.div_ceil(FRAME);
    // Only free the privately-cloned intermediate tables, not the shared
    // kernel ones.
    unsafe {
        free_user_paths(root, USER_LOAD_ADDR, image_pages, 
                        // stack_phys is the physical base; since identity-mapped
                        // it equals the virtual base used during create.
                        stack_phys, stack_pages, alloc);
    }
    alloc.free_frames(image_phys, image_len.div_ceil(FRAME) * FRAME);
    alloc.free_frames(stack_phys, stack_pages * FRAME);
}

/// Set per-page protection on an already-mapped range (page-aligned
/// internally), then flush the whole TLB. Tables must exist -- the uniform
/// map covers everything, so this only ORs/clears leaf flags, never
/// allocates. Panics on a missing level (kernel invariant).
pub fn set_prot(base: u64, len: u64, prot: u64) {
    let pml4 = unsafe { OUR_PML4 };
    debug_assert!(pml4 != 0);
    let mask = 0x000F_FFFF_FFFF_F000;
    let mut addr = base & !(FRAME - 1);
    let end = align_up(base + len, FRAME);
    while addr < end {
        let pml4e = table_mut(pml4)[((addr >> 39) & 0x1FF) as usize];
        assert!(pml4e & PTE_P != 0, "mem: set_prot missing PML4E");
        let pdpt_e = table_mut(pml4e & mask)[((addr >> 30) & 0x1FF) as usize];
        assert!(pdpt_e & PTE_P != 0, "mem: set_prot missing PDPTE");
        assert!(pdpt_e & PTE_PS == 0, "mem: set_prot hit 1G page");
        let pd_e = table_mut(pdpt_e & mask)[((addr >> 21) & 0x1FF) as usize];
        assert!(pd_e & PTE_P != 0, "mem: set_prot missing PDE");
        assert!(pd_e & PTE_PS == 0, "mem: set_prot hit 2M page");
        let pt = table_mut(pd_e & mask);
        let i = ((addr >> 12) & 0x1FF) as usize;
        assert!(pt[i] & PTE_P != 0, "mem: set_prot missing PTE");
        pt[i] = (pt[i] & !PROT_MASK) | (prot & PROT_MASK);
        addr += FRAME;
    }
    // Full TLB flush; set_prot runs rarely (boot + task spawn).
    unsafe { crate::cpu::write_cr3(crate::cpu::read_cr3()) };
}

/// Change page protections in a user address space.
/// `root` is the PML4 physical address of the target task.
/// `prot` uses mprotect convention: 1=READ, 2=WRITE, 4=EXEC.
/// Returns true on success, false if any page is not mapped.
pub fn user_mprotect(root: u64, addr: u64, len: u64, prot: u8) -> bool {
    if root == 0 || len == 0 { return false; }

    // Convert mprotect flags to PTE flags.
    let mut pte_flags: u64 = PTE_P | PTE_U; // always present + user
    if prot & 2 != 0 { pte_flags |= PTE_W; }   // write
    if prot & 4 == 0 { pte_flags |= PTE_NX; }   // no execute unless EXEC bit set

    let mask = ADDR_MASK;
    let mut va = addr & !(FRAME - 1);
    let end = align_up(addr + len, FRAME);

    while va < end {
        // Only allow user-space addresses (below 0x8000_0000_0000).
        if va >= 0x8000_0000_0000 { return false; }

        let pml4e = unsafe { *table_ptr(root).add(((va >> 39) & 0x1FF) as usize) };
        if pml4e & PTE_P == 0 { return false; }
        let pdpt_e = unsafe { *table_ptr(pml4e & mask).add(((va >> 30) & 0x1FF) as usize) };
        if pdpt_e & PTE_P == 0 || pdpt_e & PTE_PS != 0 { return false; }
        let pd_e = unsafe { *table_ptr(pdpt_e & mask).add(((va >> 21) & 0x1FF) as usize) };
        if pd_e & PTE_P == 0 || pd_e & PTE_PS != 0 { return false; }
        let pt = table_mut(pd_e & mask);
        let idx = ((va >> 12) & 0x1FF) as usize;
        if pt[idx] & PTE_P == 0 { return false; }
        // Preserve physical address, update protection bits.
        pt[idx] = (pt[idx] & !PROT_MASK) | (pte_flags & PROT_MASK);
        va += FRAME;
    }

    // Flush TLB for this address space.
    unsafe { crate::cpu::write_cr3(root) };
    true
}

/// Unmap pages in a user address space and free the physical frames.
/// `addr` must be page-aligned, `len` is rounded up to page boundary.
/// Returns true on success, false if any page is not mapped or addr is invalid.
pub fn user_munmap(root: u64, addr: u64, len: u64) -> bool {
    if root == 0 || len == 0 { return false; }
    // Only allow unmapping in user heap region.
    if addr < 0x800000 || addr >= 0xA00000 { return false; }

    let mask = ADDR_MASK;
    let mut va = addr & !(FRAME - 1);
    let end = align_up(addr + len, FRAME);

    while va < end {
        if va >= 0xA00000 { break; } // don't go past user region

        let pml4e = unsafe { *table_ptr(root).add(((va >> 39) & 0x1FF) as usize) };
        if pml4e & PTE_P == 0 { break; } // not mapped, stop
        let pdpt_e = unsafe { *table_ptr(pml4e & mask).add(((va >> 30) & 0x1FF) as usize) };
        if pdpt_e & PTE_P == 0 || pdpt_e & PTE_PS != 0 { break; }
        let pd_e = unsafe { *table_ptr(pdpt_e & mask).add(((va >> 21) & 0x1FF) as usize) };
        if pd_e & PTE_P == 0 || pd_e & PTE_PS != 0 { break; }
        let pt = table_mut(pd_e & mask);
        let idx = ((va >> 12) & 0x1FF) as usize;
        if pt[idx] & PTE_P == 0 { break; } // not mapped

        // Extract physical address and free the frame.
        let phys = pt[idx] & mask;
        unsafe { frames() }.free_frames(phys, FRAME);
        // Clear the PTE.
        pt[idx] = 0;
        va += FRAME;
    }

    // Flush TLB.
    unsafe { crate::cpu::write_cr3(root) };
    true
}

/// Mark user pages as Copy-on-Write: remove W bit, set COW bit (bit 9).
/// Pages become read-only; a write triggers #PF which clones the page.
pub fn mark_pages_cow(root: u64, addr: u64, len: u64) -> bool {
    if root == 0 || len == 0 { return false; }
    let mask = ADDR_MASK;
    let mut va = addr & !(FRAME - 1);
    let end = align_up(addr + len, FRAME);

    while va < end {
        if va >= 0x800000 && va < 0xA00000 {
            let pml4e = unsafe { *table_ptr(root).add(((va >> 39) & 0x1FF) as usize) };
            if pml4e & PTE_P == 0 { break; }
            let pdpt_e = unsafe { *table_ptr(pml4e & mask).add(((va >> 30) & 0x1FF) as usize) };
            if pdpt_e & PTE_P == 0 || pdpt_e & PTE_PS != 0 { break; }
            let pd_e = unsafe { *table_ptr(pdpt_e & mask).add(((va >> 21) & 0x1FF) as usize) };
            if pd_e & PTE_P == 0 || pd_e & PTE_PS != 0 { break; }
            let pt = table_mut(pd_e & mask);
            let idx = ((va >> 12) & 0x1FF) as usize;
            if pt[idx] & PTE_P != 0 && pt[idx] & PTE_W != 0 {
                // Remove W, add COW marker.
                pt[idx] = (pt[idx] & !PTE_W) | PTE_COW;
            }
        }
        va += FRAME;
    }
    unsafe { crate::cpu::write_cr3(root) };
    true
}

/// Build our page tables, switch CR3.
/// Uniform 4K identity map of low RAM + the framebuffer if above 4G,
/// then per-section protection: kernel .text/.rodata supervisor-only,
/// the USER.BIN image U+RW+X (its tasks get U+NX stacks at spawn).
/// Returns our PML4 physical address.
pub fn init(bi: &BootInfo) -> u64 {
    let alloc = unsafe { frames() };
    alloc.init(bi);

    crate::cpu::enable_nxe(); // before any NX-marked PTE goes live
    let pml4 = alloc.alloc_frame();
    map_range_4k(pml4, 0, 1 << 32, PROT_KERN, alloc); // supervisor default
    if bi.fb_base + bi.fb_size > 1 << 32 {
        let fb0 = bi.fb_base & !(TWO_MIB - 1);
        let fb1 = align_up(bi.fb_base + bi.fb_size, TWO_MIB);
        map_range_4k(pml4, fb0, fb1 - fb0, PROT_KERN, alloc);
        kwarn!("mem: framebuffer above 4G, mapped extra");
    }
    kinfo!("mem: tables built, {} frames used", alloc.used_frames());
    unsafe {
        OUR_PML4 = pml4;
        let (ts, te) = (
            &_text_start as *const u8 as u64,
            &_text_end as *const u8 as u64,
        );
        let (rs, re) = (
            &_rodata_start as *const u8 as u64,
            &_rodata_end as *const u8 as u64,
        );
        // M9 lockdown: kernel .text/.rodata back to supervisor-only now
        // that ring3 programs live in their own image; the user image is
        // the only U-mapped code left (plus per-task U stacks at spawn).
        set_prot(ts, te - ts, PROT_KTEXT);
        set_prot(rs, re - rs, PROT_KRO);
        kinfo!("mem: .text supervisor, .rodata supervisor-RO");
        // ELF staging buffer (bi.user_base) is just temporary data; the
        // kernel's ELF loader copies segments to freshly allocated frames.
        // No need to lock it down in the kernel page table.
        crate::cpu::write_cr3(pml4);
        kinfo!(
            "mem: our PML4 @{:#x} active (cr3={:#x})",
            pml4,
            crate::cpu::read_cr3()
        );
    }
    pml4
}

/// Global frame allocator accessor (init() ran at boot).
///
/// # Safety
///
/// Call only after [`init`]; single-threaded + IRQ handlers never allocate.
pub unsafe fn frames() -> &'static mut FrameAlloc {
    unsafe { &mut *core::ptr::addr_of_mut!(FRAMES) }
}

/// Snapshot for the shell `mem` command: (regions, total_bytes, used_frames,
/// recycled_bytes). IRQ handlers never touch FRAMES, so this is race-free.
pub fn stats() -> (usize, u64, u64, u64) {
    unsafe {
        let f = &*core::ptr::addr_of!(FRAMES);
        let free: u64 = f.recycled[..f.nrecycled].iter().map(|&(_, l)| l).sum();
        (f.nregions, f.total, f.used, free)
    }
}

/// Get allocation/free counts for leak detection.
pub fn alloc_stats() -> (u64, u64) {
    unsafe {
        let f = &*core::ptr::addr_of!(FRAMES);
        (f.alloc_count, f.free_count)
    }
}

/// Return a previously allocated frame range to the pool (task stacks).
/// Call with interrupts disabled (single CPU: any IRQ must not allocate).
pub fn free_frames(base: u64, len: u64) {
    unsafe { frames() }.free_frames(base, len);
}

/// Hot-add a memory region at runtime.
pub fn hot_add(base: u64, len: u64) -> bool {
    unsafe { frames() }.hot_add(base, len)
}

/// Hot-remove a previously added memory region.
pub fn hot_remove(base: u64, len: u64) -> bool {
    unsafe { frames() }.hot_remove(base, len)
}

/// Physical address of the kernel's shared PML4.
pub fn kernel_pml4() -> u64 {
    unsafe { OUR_PML4 }
}
