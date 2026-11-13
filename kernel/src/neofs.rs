//! NEO-FS: A simple filesystem for NEO-OS-3.
//!
//! Operates through the block device abstraction layer ([`crate::block`]).
//! Provides inode-based file storage with direct + single indirect block pointers.
//!
//! # On-Disk Layout (all in 512-byte sectors)
//!
//! | Sector(s) | Content |
//! |-----------|---------|
//! | 0 | Superblock (magic `NEO1`, version, inode/data counts) |
//! | 1 | Inode bitmap (64 inodes max) |
//! | 2 | Data block bitmap (4096 blocks max) |
//! | 3–18 | Inode table (16 sectors × 8 inodes = 128 slots, 64 used) |
//! | 19+ | Data blocks |
//!
//! # Inode Format (64 bytes each, 8 per sector)
//!
//! | Offset | Size | Field |
//! |--------|------|-------|
//! | 0 | 4 | mode (low 4 bits: type; high 12 bits: rwxrwxrwx) |
//! | 4 | 4 | size (bytes) |
//! | 8 | 32 | blocks\[8\] (1-based direct block pointers, 0=unused) |
//! | 40 | 16 | name (null-padded, max 16 chars) |
//! | 56 | 4 | parent_ino (directory hierarchy) |
//! | 60 | 4 | indirect_block (1-based, 0=none) |
//!
//! # Limits
//! - Max 64 inodes
//! - Max file size: ~68 KB (8 direct + 128 indirect × 512B)
//! - Max filename: 16 characters
//! - Single-sector bitmap: 4096 data blocks (~2 MB usable)

use crate::{kinfo, kprint};

/// Filesystem constants.
pub const SECTOR_SIZE: usize = 512;
const SUPERBLOCK_SECTOR: u64 = 0;
const INODE_BITMAP_SECTOR: u64 = 1;
const DATA_BITMAP_SECTOR: u64 = 2;
const INODE_TABLE_START: u64 = 3;
const INODE_TABLE_SECTORS: u64 = 16;
const DATA_START: u64 = INODE_TABLE_START + INODE_TABLE_SECTORS; // sector 19
pub const MAX_INODES: usize = 64;
const INODES_PER_SECTOR: usize = SECTOR_SIZE / 64; // 8
pub const MAX_NAME_LEN: usize = 16;
pub const DIRECT_BLOCKS: usize = 8;

const NEO_FS_MAGIC: u32 = 0x4E454F31; // "NEO1"

/// File type (low 4 bits of mode).
const MODE_FREE: u32 = 0;
const MODE_FILE: u32 = 1;
const MODE_DIR: u32 = 2;
const TYPE_MASK: u32 = 0xF;

/// Permission bits (bits 4-15 of mode): rwxrwxrwx (owner/group/other).
const PERM_OR: u32 = 0x10;  // owner read
const PERM_OW: u32 = 0x20;  // owner write
const PERM_OX: u32 = 0x40;  // owner execute
const PERM_GR: u32 = 0x80;  // group read
const PERM_GW: u32 = 0x100; // group write
const PERM_GX: u32 = 0x200; // group execute
const PERM_OTH_R: u32 = 0x400; // other read
const PERM_OTH_W: u32 = 0x800; // other write
const PERM_OTH_X: u32 = 0x1000; // other execute

/// Default permissions: rwxr-xr-x (0755 equivalent).
const DEFAULT_FILE_PERMS: u32 = PERM_OR | PERM_OW | PERM_GR | PERM_GX | PERM_OTH_R | PERM_OTH_X;
const DEFAULT_DIR_PERMS: u32 = PERM_OR | PERM_OW | PERM_OX | PERM_GR | PERM_GX | PERM_OTH_R | PERM_OTH_X;

/// Combine type and permissions into a mode value.
fn make_mode(file_type: u32, perms: u32) -> u32 {
    (file_type & TYPE_MASK) | (perms & !TYPE_MASK)
}

/// Extract file type from mode.
fn mode_type(mode: u32) -> u32 {
    mode & TYPE_MASK
}

/// Format permission bits as "rwxrwxrwx" string.
fn format_perms(mode: u32, buf: &mut [u8; 9]) {
    buf[0] = if mode & PERM_OR != 0 { b'r' } else { b'-' };
    buf[1] = if mode & PERM_OW != 0 { b'w' } else { b'-' };
    buf[2] = if mode & PERM_OX != 0 { b'x' } else { b'-' };
    buf[3] = if mode & PERM_GR != 0 { b'r' } else { b'-' };
    buf[4] = if mode & PERM_GW != 0 { b'w' } else { b'-' };
    buf[5] = if mode & PERM_GX != 0 { b'x' } else { b'-' };
    buf[6] = if mode & PERM_OTH_R != 0 { b'r' } else { b'-' };
    buf[7] = if mode & PERM_OTH_W != 0 { b'w' } else { b'-' };
    buf[8] = if mode & PERM_OTH_X != 0 { b'x' } else { b'-' };
}

/// Public wrapper for format_perms (used by shell).
pub fn format_perms_pub(mode: u32, buf: &mut [u8; 9]) {
    format_perms(mode, buf);
}

pub const ROOT_INO: usize = 0;

/// Which block device NEO-FS is mounted on.
static mut MOUNT_DEV: Option<usize> = None;

/// On-disk inode structure (64 bytes).
#[repr(C)]
#[derive(Clone, Copy)]
struct Inode {
    mode: u32,
    size: u32,
    blocks: [u32; DIRECT_BLOCKS],   // 8 direct blocks (4KB)
    name: [u8; MAX_NAME_LEN],
    parent_ino: u32,
    indirect_block: u32,            // 1-based block number for indirect block (0=none)
}

/// Number of block pointers that fit in one indirect block (512/4 = 128).
pub const INDIRECT_PTRS: usize = SECTOR_SIZE / 4; // 128
/// Max file size with direct + single indirect: 8*512 + 128*512 = 69632 bytes (~68KB).
/// Max file size with direct + single indirect: 8*512 + 128*512 = 69632 bytes (~68KB).
pub const MAX_FILE_SIZE: usize = (DIRECT_BLOCKS + INDIRECT_PTRS) * SECTOR_SIZE;

impl Inode {
    fn is_free(&self) -> bool { mode_type(self.mode) == MODE_FREE }
    fn is_file(&self) -> bool { mode_type(self.mode) == MODE_FILE }
    #[allow(dead_code)]
    fn is_dir(&self) -> bool { mode_type(self.mode) == MODE_DIR }
}

// ---- Low-level disk access through block layer ----

fn dev() -> Result<usize, &'static str> {
    unsafe { MOUNT_DEV.ok_or("neofs: not mounted") }
}

fn read_sector(sector: u64, buf: &mut [u8; SECTOR_SIZE]) -> Result<(), &'static str> {
    crate::block::read(dev()?, sector, buf)
}

fn write_sector(sector: u64, buf: &[u8; SECTOR_SIZE]) -> Result<(), &'static str> {
    crate::block::write(dev()?, sector, buf)
}

// ---- Inode I/O ----

fn read_inode(ino: usize) -> Result<Inode, &'static str> {
    if ino >= MAX_INODES { return Err("inode out of range"); }
    let sector = INODE_TABLE_START + (ino / INODES_PER_SECTOR) as u64;
    let offset = (ino % INODES_PER_SECTOR) * 64;
    let mut buf = [0u8; SECTOR_SIZE];
    read_sector(sector, &mut buf)?;
    let inode: Inode = unsafe { core::ptr::read(buf[offset..].as_ptr() as *const Inode) };
    Ok(inode)
}

fn write_inode(ino: usize, inode: &Inode) -> Result<(), &'static str> {
    if ino >= MAX_INODES { return Err("inode out of range"); }
    let sector = INODE_TABLE_START + (ino / INODES_PER_SECTOR) as u64;
    let offset = (ino % INODES_PER_SECTOR) * 64;
    let mut buf = [0u8; SECTOR_SIZE];
    read_sector(sector, &mut buf)?;
    unsafe {
        core::ptr::write(buf[offset..].as_mut_ptr() as *mut Inode, *inode);
    }
    write_sector(sector, &buf)
}

// ---- Bitmap operations ----

fn bitmap_test(sector: u64, bit: usize) -> Result<bool, &'static str> {
    let mut buf = [0u8; SECTOR_SIZE];
    read_sector(sector, &mut buf)?;
    let byte_idx = bit / 8;
    let bit_idx = bit % 8;
    if byte_idx >= SECTOR_SIZE { return Err("bitmap bit out of range"); }
    Ok(buf[byte_idx] & (1 << bit_idx) != 0)
}

fn bitmap_set(sector: u64, bit: usize) -> Result<(), &'static str> {
    let mut buf = [0u8; SECTOR_SIZE];
    read_sector(sector, &mut buf)?;
    let byte_idx = bit / 8;
    let bit_idx = bit % 8;
    if byte_idx >= SECTOR_SIZE { return Err("bitmap bit out of range"); }
    buf[byte_idx] |= 1 << bit_idx;
    write_sector(sector, &buf)
}

fn bitmap_clear(sector: u64, bit: usize) -> Result<(), &'static str> {
    let mut buf = [0u8; SECTOR_SIZE];
    read_sector(sector, &mut buf)?;
    let byte_idx = bit / 8;
    let bit_idx = bit % 8;
    if byte_idx >= SECTOR_SIZE { return Err("bitmap bit out of range"); }
    buf[byte_idx] &= !(1 << bit_idx);
    write_sector(sector, &buf)
}

fn bitmap_find_free(sector: u64, max_bits: usize) -> Result<Option<usize>, &'static str> {
    let mut buf = [0u8; SECTOR_SIZE];
    read_sector(sector, &mut buf)?;
    // Clamp to single-sector bitmap capacity.
    let limit = core::cmp::min(max_bits, SECTOR_SIZE * 8);
    for i in 0..limit {
        let byte_idx = i / 8;
        let bit_idx = i % 8;
        if buf[byte_idx] & (1 << bit_idx) == 0 {
            return Ok(Some(i));
        }
    }
    Ok(None)
}

// ---- Allocation ----

fn alloc_inode() -> Result<usize, &'static str> {
    match bitmap_find_free(INODE_BITMAP_SECTOR, MAX_INODES)? {
        Some(ino) => {
            bitmap_set(INODE_BITMAP_SECTOR, ino)?;
            let inode = Inode {
                mode: MODE_FREE, size: 0, blocks: [0; DIRECT_BLOCKS],
                name: [0; MAX_NAME_LEN], parent_ino: 0, indirect_block: 0,
            };
            write_inode(ino, &inode)?;
            Ok(ino)
        }
        None => Err("no free inodes"),
    }
}

/// Allocate a data block. Returns 1-based block number (0 = unused sentinel).
fn alloc_block() -> Result<u32, &'static str> {
    // Read actual data block count from superblock (clamped to bitmap capacity).
    let mut sb = [0u8; SECTOR_SIZE];
    read_sector(SUPERBLOCK_SECTOR, &mut sb)?;
    let max_data_blocks = u32::from_le_bytes([sb[12], sb[13], sb[14], sb[15]]) as usize;
    match bitmap_find_free(DATA_BITMAP_SECTOR, max_data_blocks)? {
        Some(blk) => {
            bitmap_set(DATA_BITMAP_SECTOR, blk)?;
            Ok((blk + 1) as u32)
        }
        None => Err("no free data blocks"),
    }
}

fn free_inode(ino: usize) -> Result<(), &'static str> {
    if ino >= MAX_INODES { return Err("inode out of range"); }
    let inode = read_inode(ino)?;
    // Free direct blocks.
    for &blk in &inode.blocks {
        if blk != 0 {
            bitmap_clear(DATA_BITMAP_SECTOR, (blk - 1) as usize)?;
        }
    }
    // Free indirect block and its data blocks.
    if inode.indirect_block != 0 {
        let mut ind_buf = [0u8; SECTOR_SIZE];
        read_sector(DATA_START + (inode.indirect_block - 1) as u64, &mut ind_buf)?;
        for i in 0..INDIRECT_PTRS {
            let blk = u32::from_le_bytes([
                ind_buf[i * 4], ind_buf[i * 4 + 1],
                ind_buf[i * 4 + 2], ind_buf[i * 4 + 3],
            ]);
            if blk != 0 {
                bitmap_clear(DATA_BITMAP_SECTOR, (blk - 1) as usize)?;
            }
        }
        bitmap_clear(DATA_BITMAP_SECTOR, (inode.indirect_block - 1) as usize)?;
    }
    let empty = Inode {
        mode: MODE_FREE, size: 0, blocks: [0; DIRECT_BLOCKS],
        name: [0; MAX_NAME_LEN], parent_ino: 0, indirect_block: 0,
    };
    write_inode(ino, &empty)?;
    bitmap_clear(INODE_BITMAP_SECTOR, ino)
}

// ---- Public API for VFS and shell ----

/// Format the block device with NEO-FS.
pub fn mkfs() -> Result<(), &'static str> {
    // mkfs works on block device 0 even if not yet mounted.
    let d = unsafe { MOUNT_DEV.unwrap_or(0) };
    if crate::block::count() == 0 {
        return Err("no block device");
    }
    // Set MOUNT_DEV early so write_sector/read_sector can access the disk.
    unsafe { MOUNT_DEV = Some(d); }
    let total_sectors = crate::block::capacity(d);
    if total_sectors < DATA_START + 1 {
        return Err("disk too small");
    }

    // Data bitmap is one sector = 4096 bits max.
    let raw_data_blocks = (total_sectors - DATA_START) as usize;
    let data_blocks = core::cmp::min(raw_data_blocks, SECTOR_SIZE * 8) as u32;

    kinfo!("neofs: formatting {} sectors ({} usable blocks) on dev {}...",
           total_sectors, data_blocks, d);

    let zero = [0u8; SECTOR_SIZE];

    // Superblock.
    let mut sb = [0u8; SECTOR_SIZE];
    sb[0..4].copy_from_slice(&NEO_FS_MAGIC.to_le_bytes());
    sb[4..8].copy_from_slice(&1u32.to_le_bytes());
    sb[8..12].copy_from_slice(&(MAX_INODES as u32).to_le_bytes());
    sb[12..16].copy_from_slice(&data_blocks.to_le_bytes());
    write_sector(SUPERBLOCK_SECTOR, &sb)?;

    write_sector(INODE_BITMAP_SECTOR, &zero)?;
    write_sector(DATA_BITMAP_SECTOR, &zero)?;

    for s in INODE_TABLE_START..INODE_TABLE_START + INODE_TABLE_SECTORS {
        write_sector(s, &zero)?;
    }

    // Root directory.
    bitmap_set(INODE_BITMAP_SECTOR, ROOT_INO)?;
    let root = Inode {
        mode: make_mode(MODE_DIR, DEFAULT_DIR_PERMS), size: 0, blocks: [0; DIRECT_BLOCKS],
        name: *b"/\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0",
        parent_ino: ROOT_INO as u32, indirect_block: 0,
    };
    write_inode(ROOT_INO, &root)?;

    // Create default /init script.
    let init_content = b"# NEO-OS-3 init script\n# Runs at boot after filesystem mount\necho NEO-OS-3 booted!\nfsinfo\n";
    if let Some(ino) = create_file(b"init") {
        let _ = write_file_data(ino, 0, init_content);
    }

    kinfo!("neofs: format complete");
    Ok(())
}

/// Mount the filesystem on the given block device.
pub fn mount(dev_idx: usize) -> Result<(), &'static str> {
    // Verify superblock.
    let mut buf = [0u8; SECTOR_SIZE];
    crate::block::read(dev_idx, SUPERBLOCK_SECTOR, &mut buf)?;
    let magic = u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]);
    if magic != NEO_FS_MAGIC {
        return Err("invalid filesystem (run mkfs first)");
    }
    unsafe { MOUNT_DEV = Some(dev_idx); }
    kinfo!("neofs: mounted on block device {}", dev_idx);
    Ok(())
}

/// Unmount the filesystem.
pub fn unmount() {
    unsafe { MOUNT_DEV = None; }
    kinfo!("neofs: unmounted");
}

/// Check if filesystem is mounted and valid.
pub fn is_mounted() -> bool {
    unsafe { MOUNT_DEV.is_some() }
}

/// Find an inode by filename in root directory.
pub fn find_inode(name: &[u8]) -> Option<usize> {
    if !is_mounted() { return None; }
    for ino in 0..MAX_INODES {
        if !bitmap_test(INODE_BITMAP_SECTOR, ino).unwrap_or(false) { continue; }
        let inode = read_inode(ino).ok()?;
        if inode.is_free() { continue; }
        if inode.parent_ino != ROOT_INO as u32 { continue; }
        let name_end = inode.name.iter().position(|&b| b == 0).unwrap_or(MAX_NAME_LEN);
        if &inode.name[..name_end] == name {
            return Some(ino);
        }
    }
    None
}

/// Create a new regular file in root directory. Returns inode number.
pub fn create_file(name: &[u8]) -> Option<usize> {
    create_file_in_dir(name, ROOT_INO)
}

/// Create a new regular file in a specific directory. Returns inode number.
pub fn create_file_in_dir(name: &[u8], parent: usize) -> Option<usize> {
    if !is_mounted() || name.len() > MAX_NAME_LEN { return None; }
    let ino = alloc_inode().ok()?;
    let mut inode = Inode {
        mode: make_mode(MODE_FILE, DEFAULT_FILE_PERMS), size: 0, blocks: [0; DIRECT_BLOCKS],
        name: [0; MAX_NAME_LEN], parent_ino: parent as u32, indirect_block: 0,
    };
    inode.name[..name.len()].copy_from_slice(name);
    write_inode(ino, &inode).ok()?;
    Some(ino)
}

/// Truncate a file to zero (free all data blocks).
pub fn truncate_inode(ino: usize) -> Result<(), &'static str> {
    truncate_to_size(ino, 0)
}

/// Truncate or extend a file to a specific size.
/// If new_size < current size, frees blocks beyond new_size.
/// If new_size > current size, extends with zeros (sparse — no blocks allocated until written).
pub fn truncate_to_size(ino: usize, new_size: usize) -> Result<(), &'static str> {
    if new_size > MAX_FILE_SIZE { return Err("size exceeds maximum"); }
    let mut inode = read_inode(ino)?;
    if !inode.is_file() { return Err("not a file"); }

    let old_size = inode.size as usize;

    if new_size < old_size {
        // Shrinking: free blocks that are entirely beyond new_size.
        let last_needed_block = if new_size == 0 { 0 } else { (new_size - 1) / SECTOR_SIZE + 1 };

        // Free direct blocks beyond the needed count.
        for bi in last_needed_block..DIRECT_BLOCKS {
            if inode.blocks[bi] != 0 {
                bitmap_clear(DATA_BITMAP_SECTOR, (inode.blocks[bi] - 1) as usize)?;
                inode.blocks[bi] = 0;
            }
        }

        // If no indirect blocks needed, free the entire indirect block.
        if last_needed_block <= DIRECT_BLOCKS {
            if inode.indirect_block != 0 {
                let mut ind_buf = [0u8; SECTOR_SIZE];
                read_sector(DATA_START + (inode.indirect_block - 1) as u64, &mut ind_buf)?;
                for i in 0..INDIRECT_PTRS {
                    let blk = u32::from_le_bytes([
                        ind_buf[i * 4], ind_buf[i * 4 + 1],
                        ind_buf[i * 4 + 2], ind_buf[i * 4 + 3],
                    ]);
                    if blk != 0 {
                        bitmap_clear(DATA_BITMAP_SECTOR, (blk - 1) as usize)?;
                    }
                }
                bitmap_clear(DATA_BITMAP_SECTOR, (inode.indirect_block - 1) as usize)?;
                inode.indirect_block = 0;
            }
        } else {
            // Free indirect block entries beyond what's needed.
            if inode.indirect_block != 0 {
                let ind_start = last_needed_block - DIRECT_BLOCKS;
                let mut ind_buf = [0u8; SECTOR_SIZE];
                read_sector(DATA_START + (inode.indirect_block - 1) as u64, &mut ind_buf)?;
                for i in ind_start..INDIRECT_PTRS {
                    let blk = u32::from_le_bytes([
                        ind_buf[i * 4], ind_buf[i * 4 + 1],
                        ind_buf[i * 4 + 2], ind_buf[i * 4 + 3],
                    ]);
                    if blk != 0 {
                        bitmap_clear(DATA_BITMAP_SECTOR, (blk - 1) as usize)?;
                        ind_buf[i * 4..i * 4 + 4].copy_from_slice(&0u32.to_le_bytes());
                    }
                }
                write_sector(DATA_START + (inode.indirect_block - 1) as u64, &ind_buf)?;
            }
        }

        // Zero partial last block if it exists.
        if new_size > 0 && new_size % SECTOR_SIZE != 0 {
            let last_bi = (new_size - 1) / SECTOR_SIZE;
            let blk = resolve_block(&inode, last_bi)?;
            if blk != 0 {
                let mut sector_buf = [0u8; SECTOR_SIZE];
                read_sector(DATA_START + (blk - 1) as u64, &mut sector_buf)?;
                let zero_from = new_size % SECTOR_SIZE;
                for b in sector_buf[zero_from..].iter_mut() { *b = 0; }
                write_sector(DATA_START + (blk - 1) as u64, &sector_buf)?;
            }
        }
    }
    // Extending (new_size > old_size): just update size; blocks allocated on write.

    inode.size = new_size as u32;
    write_inode(ino, &inode)
}

/// Resolve a logical block index to a physical block number (1-based).
/// Returns 0 if the block is not allocated.
fn resolve_block(inode: &Inode, logical: usize) -> Result<u32, &'static str> {
    if logical < DIRECT_BLOCKS {
        Ok(inode.blocks[logical])
    } else if logical < DIRECT_BLOCKS + INDIRECT_PTRS {
        // Single indirect block.
        if inode.indirect_block == 0 { return Ok(0); }
        let mut ind_buf = [0u8; SECTOR_SIZE];
        read_sector(DATA_START + (inode.indirect_block - 1) as u64, &mut ind_buf)?;
        let idx = logical - DIRECT_BLOCKS;
        let blk = u32::from_le_bytes([
            ind_buf[idx * 4], ind_buf[idx * 4 + 1],
            ind_buf[idx * 4 + 2], ind_buf[idx * 4 + 3],
        ]);
        Ok(blk)
    } else {
        Err("block index out of range")
    }
}

/// Allocate or get an indirect block for an inode. Returns the 1-based block number.
fn ensure_indirect_block(inode: &mut Inode) -> Result<u32, &'static str> {
    if inode.indirect_block != 0 {
        return Ok(inode.indirect_block);
    }
    let blk = alloc_block()?;
    // Zero the indirect block.
    let zero = [0u8; SECTOR_SIZE];
    write_sector(DATA_START + (blk - 1) as u64, &zero)?;
    inode.indirect_block = blk;
    Ok(blk)
}

/// Set a block pointer in the indirect block.
fn set_indirect_ptr(indirect_blk: u32, index: usize, data_blk: u32) -> Result<(), &'static str> {
    let mut ind_buf = [0u8; SECTOR_SIZE];
    read_sector(DATA_START + (indirect_blk - 1) as u64, &mut ind_buf)?;
    let off = index * 4;
    ind_buf[off..off + 4].copy_from_slice(&data_blk.to_le_bytes());
    write_sector(DATA_START + (indirect_blk - 1) as u64, &ind_buf)
}

/// Read file data starting at offset. Returns bytes actually read.
pub fn read_file_data(ino: usize, offset: usize, buf: &mut [u8]) -> Result<usize, &'static str> {
    let inode = read_inode(ino)?;
    if !inode.is_file() { return Err("not a file"); }
    if offset >= inode.size as usize { return Ok(0); }

    let remaining = (inode.size as usize) - offset;
    let to_read = core::cmp::min(buf.len(), remaining);
    let mut bytes_read = 0;
    let start_block = offset / SECTOR_SIZE;
    let block_offset = offset % SECTOR_SIZE;
    let max_blocks = DIRECT_BLOCKS + INDIRECT_PTRS;

    for bi in start_block..max_blocks {
        if bytes_read >= to_read { break; }
        let blk = resolve_block(&inode, bi)?;
        if blk == 0 { break; }
        let mut sector_buf = [0u8; SECTOR_SIZE];
        read_sector(DATA_START + (blk - 1) as u64, &mut sector_buf)?;
        let src_start = if bi == start_block { block_offset } else { 0 };
        let avail = SECTOR_SIZE - src_start;
        let chunk = core::cmp::min(avail, to_read - bytes_read);
        buf[bytes_read..bytes_read + chunk].copy_from_slice(&sector_buf[src_start..src_start + chunk]);
        bytes_read += chunk;
    }
    Ok(bytes_read)
}

/// Write file data starting at offset. Returns bytes actually written.
pub fn write_file_data(ino: usize, offset: usize, data: &[u8]) -> Result<usize, &'static str> {
    let mut inode = read_inode(ino)?;
    if !inode.is_file() { return Err("not a file"); }
    if offset + data.len() > MAX_FILE_SIZE { return Err("file too large"); }

    let mut bytes_written = 0;
    let start_block = offset / SECTOR_SIZE;
    let block_offset = offset % SECTOR_SIZE;
    let max_blocks = DIRECT_BLOCKS + INDIRECT_PTRS;

    for bi in start_block..max_blocks {
        if bytes_written >= data.len() { break; }

        // Get or allocate the physical block for this logical index.
        let blk = if bi < DIRECT_BLOCKS {
            if inode.blocks[bi] == 0 {
                inode.blocks[bi] = alloc_block()?;
            }
            inode.blocks[bi]
        } else {
            // Indirect block territory.
            let ind_blk = ensure_indirect_block(&mut inode)?;
            let idx = bi - DIRECT_BLOCKS;
            let existing = {
                let mut ind_buf = [0u8; SECTOR_SIZE];
                read_sector(DATA_START + (ind_blk - 1) as u64, &mut ind_buf)?;
                u32::from_le_bytes([
                    ind_buf[idx * 4], ind_buf[idx * 4 + 1],
                    ind_buf[idx * 4 + 2], ind_buf[idx * 4 + 3],
                ])
            };
            if existing == 0 {
                let new_blk = alloc_block()?;
                set_indirect_ptr(ind_blk, idx, new_blk)?;
                new_blk
            } else {
                existing
            }
        };

        let mut sector_buf = [0u8; SECTOR_SIZE];

        // If partial write, read existing data first.
        if block_offset != 0 && bi == start_block {
            read_sector(DATA_START + (blk - 1) as u64, &mut sector_buf)?;
        } else if data.len() - bytes_written < SECTOR_SIZE {
            read_sector(DATA_START + (blk - 1) as u64, &mut sector_buf)?;
        }

        let dst_start = if bi == start_block { block_offset } else { 0 };
        let avail = SECTOR_SIZE - dst_start;
        let chunk = core::cmp::min(avail, data.len() - bytes_written);
        sector_buf[dst_start..dst_start + chunk].copy_from_slice(&data[bytes_written..bytes_written + chunk]);
        write_sector(DATA_START + (blk - 1) as u64, &sector_buf)?;
        bytes_written += chunk;
    }

    // Update size if we extended the file.
    let new_size = offset + bytes_written;
    if new_size > inode.size as usize {
        inode.size = new_size as u32;
    }
    write_inode(ino, &inode)?;
    Ok(bytes_written)
}

/// Get stat info for an inode.
pub fn stat_inode(ino: usize) -> Option<crate::vfs::Stat> {
    let inode = read_inode(ino).ok()?;
    if inode.is_free() { return None; }
    Some(crate::vfs::Stat {
        ftype: inode.mode,
        size: inode.size,
        inode: ino as u32,
        _pad: 0,
    })
}

/// List root directory entries via callback.
pub fn list_root(callback: &mut dyn FnMut(&[u8], u32, u32)) -> usize {
    list_dir(ROOT_INO, callback)
}

/// List entries in a directory by inode number.
pub fn list_dir(dir_ino: usize, callback: &mut dyn FnMut(&[u8], u32, u32)) -> usize {
    if !is_mounted() { return 0; }
    let mut count = 0;
    for ino in 0..MAX_INODES {
        if !bitmap_test(INODE_BITMAP_SECTOR, ino).unwrap_or(false) { continue; }
        let inode = match read_inode(ino) { Ok(i) => i, Err(_) => continue };
        if inode.is_free() || ino == ROOT_INO { continue; }
        if inode.parent_ino != dir_ino as u32 { continue; }
        let name_end = inode.name.iter().position(|&b| b == 0).unwrap_or(MAX_NAME_LEN);
        callback(&inode.name[..name_end], inode.mode, inode.size);
        count += 1;
    }
    count
}

/// Find files matching a glob pattern across all directories.
/// Calls callback with (name, mode, size, parent_ino) for each match.
pub fn find_files(pattern: &[u8], callback: &mut dyn FnMut(&[u8], u32, u32, u32)) -> usize {
    if !is_mounted() { return 0; }
    let mut count = 0;
    for ino in 0..MAX_INODES {
        if !bitmap_test(INODE_BITMAP_SECTOR, ino).unwrap_or(false) { continue; }
        let inode = match read_inode(ino) { Ok(i) => i, Err(_) => continue };
        if inode.is_free() || ino == ROOT_INO { continue; }
        let name_end = inode.name.iter().position(|&b| b == 0).unwrap_or(MAX_NAME_LEN);
        let name = &inode.name[..name_end];
        // Match against pattern using glob.
        if crate::shell::glob_match(pattern, name) {
            callback(name, inode.mode, inode.size, inode.parent_ino);
            count += 1;
        }
    }
    count
}

/// Get the name of an inode by its number. Writes into provided buffer.
/// Returns length of name written.
pub fn inode_name_into(ino: usize, out: &mut [u8; MAX_NAME_LEN]) -> usize {
    if ino >= MAX_INODES { return 0; }
    let sector = INODE_TABLE_START + (ino / INODES_PER_SECTOR) as u64;
    let offset = (ino % INODES_PER_SECTOR) * 64; // inode size = 64 bytes
    let mut buf = [0u8; SECTOR_SIZE];
    if read_sector(sector, &mut buf).is_err() { return 0; }
    let name_start = offset + 40; // name offset in inode struct
    let name_end_offset = name_start + MAX_NAME_LEN;
    let mut end = name_start;
    while end < name_end_offset && buf[end] != 0 { end += 1; }
    let len = end - name_start;
    out[..len].copy_from_slice(&buf[name_start..end]);
    len
}

/// Create a directory. Returns inode number.
pub fn create_dir(name: &[u8], parent: usize) -> Option<usize> {
    if !is_mounted() || name.len() > MAX_NAME_LEN { return None; }
    // Check if name already exists in parent.
    for ino in 0..MAX_INODES {
        if !bitmap_test(INODE_BITMAP_SECTOR, ino).unwrap_or(false) { continue; }
        let inode = read_inode(ino).ok()?;
        if inode.is_free() { continue; }
        if inode.parent_ino != parent as u32 { continue; }
        let name_end = inode.name.iter().position(|&b| b == 0).unwrap_or(MAX_NAME_LEN);
        if &inode.name[..name_end] == name { return None; } // already exists
    }
    let ino = alloc_inode().ok()?;
    let mut inode = Inode {
        mode: make_mode(MODE_DIR, DEFAULT_DIR_PERMS), size: 0, blocks: [0; DIRECT_BLOCKS],
        name: [0; MAX_NAME_LEN], parent_ino: parent as u32, indirect_block: 0,
    };
    inode.name[..name.len()].copy_from_slice(name);
    write_inode(ino, &inode).ok()?;
    Some(ino)
}

/// Resolve a simple path like "dir/file" or "file".
/// Returns (parent_ino, filename). For "file", parent is ROOT_INO.
/// For "dir/file", finds dir inode and returns (dir_ino, "file").
pub fn resolve_path(path: &[u8]) -> Option<(usize, &[u8])> {
    // Find first '/'.
    if let Some(slash_pos) = path.iter().position(|&b| b == b'/') {
        let dir_name = &path[..slash_pos];
        let file_name = &path[slash_pos + 1..];
        if dir_name.is_empty() || file_name.is_empty() { return None; }
        // Find directory in root.
        let dir_ino = find_inode_in_dir(dir_name, ROOT_INO)?;
        let inode = read_inode(dir_ino).ok()?;
        if !inode.is_dir() { return None; }
        Some((dir_ino, file_name))
    } else {
        Some((ROOT_INO, path))
    }
}

/// Find an inode by name within a specific directory.
fn find_inode_in_dir(name: &[u8], dir_ino: usize) -> Option<usize> {
    for ino in 0..MAX_INODES {
        if !bitmap_test(INODE_BITMAP_SECTOR, ino).unwrap_or(false) { continue; }
        let inode = read_inode(ino).ok()?;
        if inode.is_free() { continue; }
        if inode.parent_ino != dir_ino as u32 { continue; }
        let name_end = inode.name.iter().position(|&b| b == 0).unwrap_or(MAX_NAME_LEN);
        if &inode.name[..name_end] == name {
            return Some(ino);
        }
    }
    None
}

/// Find an inode by path (supports "dir/file" syntax).
pub fn find_inode_by_path(path: &[u8]) -> Option<usize> {
    let (parent, name) = resolve_path(path)?;
    find_inode_in_dir(name, parent)
}

/// Remove a file by name.
pub fn remove_file(name: &[u8]) -> Result<(), &'static str> {
    let ino = find_inode(name).ok_or("file not found")?;
    if ino == ROOT_INO { return Err("cannot remove root"); }
    let inode = read_inode(ino)?;
    if inode.is_dir() { return Err("cannot rm directory (use rmdir)"); }
    free_inode(ino)
}

/// Check if a directory is empty (no children).
fn is_dir_empty(dir_ino: usize) -> bool {
    for ino in 0..MAX_INODES {
        if !bitmap_test(INODE_BITMAP_SECTOR, ino).unwrap_or(false) { continue; }
        let inode = match read_inode(ino) { Ok(i) => i, Err(_) => continue };
        if inode.is_free() || ino == ROOT_INO { continue; }
        if inode.parent_ino == dir_ino as u32 {
            return false; // found a child
        }
    }
    true
}

/// Remove an empty directory by name.
pub fn remove_dir(name: &[u8]) -> Result<(), &'static str> {
    let ino = find_inode(name).ok_or("directory not found")?;
    if ino == ROOT_INO { return Err("cannot remove root"); }
    let inode = read_inode(ino)?;
    if !inode.is_dir() { return Err("not a directory"); }
    if !is_dir_empty(ino) { return Err("directory not empty"); }
    free_inode(ino)
}

/// Rename a file or directory within the same filesystem.
/// If `new_name` already exists and is a regular file, it is replaced.
/// Returns error if new_name is an existing directory, or names are invalid.
pub fn rename(old_name: &[u8], new_name: &[u8]) -> Result<(), &'static str> {
    if old_name.is_empty() || new_name.is_empty() { return Err("empty name"); }
    if new_name.len() > MAX_NAME_LEN { return Err("new name too long"); }
    if old_name == new_name { return Ok(()); } // no-op

    let old_ino = find_inode(old_name).ok_or("source not found")?;
    if old_ino == ROOT_INO { return Err("cannot rename root"); }

    // Check if target exists.
    if let Some(existing_ino) = find_inode(new_name) {
        if existing_ino == ROOT_INO { return Err("cannot overwrite root"); }
        let existing = read_inode(existing_ino)?;
        if existing.is_dir() { return Err("target is a directory"); }
        // Target is a file — remove it first (overwrite semantics).
        free_inode(existing_ino)?;
    }

    // Update the source inode's name.
    let mut inode = read_inode(old_ino)?;
    inode.name = [0; MAX_NAME_LEN];
    inode.name[..new_name.len()].copy_from_slice(new_name);
    write_inode(old_ino, &inode)
}

/// Count set bits in a bitmap sector up to max_bits.
/// Note: one sector holds 4096 bits (512 bytes × 8).
fn count_bitmap_set(sector: u64, max_bits: usize) -> usize {
    let mut buf = [0u8; SECTOR_SIZE];
    if read_sector(sector, &mut buf).is_err() { return 0; }
    // Clamp to what fits in one sector.
    let limit = core::cmp::min(max_bits, SECTOR_SIZE * 8);
    let mut count = 0usize;
    for i in 0..limit {
        let byte_idx = i / 8;
        let bit_idx = i % 8;
        if buf[byte_idx] & (1 << bit_idx) != 0 {
            count += 1;
        }
    }
    count
}

/// Dump filesystem info (df-style).
pub fn fs_info() {
    if !is_mounted() {
        kprint!("neofs: not mounted\n");
        return;
    }
    let mut buf = [0u8; SECTOR_SIZE];
    let _ = read_sector(SUPERBLOCK_SECTOR, &mut buf);
    let version = u32::from_le_bytes([buf[4], buf[5], buf[6], buf[7]]);
    let max_inodes = u32::from_le_bytes([buf[8], buf[9], buf[10], buf[11]]);
    let total_data_blocks = u32::from_le_bytes([buf[12], buf[13], buf[14], buf[15]]);

    let used_inodes = count_bitmap_set(INODE_BITMAP_SECTOR, MAX_INODES);
    let used_data_blocks = count_bitmap_set(DATA_BITMAP_SECTOR, total_data_blocks as usize);

    let free_inodes = max_inodes as usize - used_inodes;
    let total_kb = total_data_blocks as usize / 2; // 512B per block = KB/2
    let used_kb = used_data_blocks / 2;
    let free_kb = (total_data_blocks as usize - used_data_blocks) / 2;
    let use_pct = if total_data_blocks > 0 {
        (used_data_blocks * 100) / total_data_blocks as usize
    } else { 0 };

    kprint!("NEO-FS v{} dev {}\n", version, unsafe { MOUNT_DEV.unwrap_or(99) });
    kprint!("  {}K total {}K used {}K free {}%\n", total_kb, used_kb, free_kb, use_pct);
    kprint!("  inodes {}/{} free={}\n", used_inodes, max_inodes, free_inodes);
}

/// Filesystem consistency check. Returns number of errors found.
pub fn fsck() -> usize {
    if !is_mounted() {
        kprint!("fsck: not mounted\n");
        return 1;
    }

    let mut errors = 0usize;

    // Phase 1: Validate all allocated inodes.
    for ino in 0..MAX_INODES {
        if !bitmap_test(INODE_BITMAP_SECTOR, ino).unwrap_or(false) { continue; }
        let inode = match read_inode(ino) {
            Ok(i) => i,
            Err(_) => {
                kprint!("fsck: error reading inode {}\n", ino);
                errors += 1;
                continue;
            }
        };

        // Check mode validity.
        if inode.mode != MODE_FILE && inode.mode != MODE_DIR {
            kprint!("fsck: inode {} has invalid mode {}\n", ino, inode.mode);
            errors += 1;
        }

        // Check parent_ino validity (except root).
        if ino != ROOT_INO {
            if inode.parent_ino as usize >= MAX_INODES {
                kprint!("fsck: inode {} has invalid parent {}\n", ino, inode.parent_ino);
                errors += 1;
            } else if !bitmap_test(INODE_BITMAP_SECTOR, inode.parent_ino as usize).unwrap_or(false) {
                kprint!("fsck: inode {} parent {} not allocated\n", ino, inode.parent_ino);
                errors += 1;
            }
        }

        // Check data block references for files.
        if inode.is_file() {
            let expected_blocks = if inode.size == 0 { 0 }
                else { ((inode.size as usize) - 1) / SECTOR_SIZE + 1 };

            // Check direct blocks.
            for bi in 0..DIRECT_BLOCKS {
                let blk = inode.blocks[bi];
                if bi < expected_blocks {
                    if blk == 0 {
                        kprint!("fsck: inode {} missing block at index {}\n", ino, bi);
                        errors += 1;
                    } else if !bitmap_test(DATA_BITMAP_SECTOR, (blk - 1) as usize).unwrap_or(false) {
                        kprint!("fsck: inode {} block {} not in bitmap\n", ino, blk);
                        errors += 1;
                    }
                } else if blk != 0 {
                    kprint!("fsck: inode {} has extra block {} at index {}\n", ino, blk, bi);
                    errors += 1;
                }
            }

            // Check indirect block.
            if expected_blocks > DIRECT_BLOCKS && inode.indirect_block == 0 {
                kprint!("fsck: inode {} needs indirect block but none set\n", ino);
                errors += 1;
            } else if expected_blocks <= DIRECT_BLOCKS && inode.indirect_block != 0 {
                kprint!("fsck: inode {} has unnecessary indirect block\n", ino);
                errors += 1;
            }
        }
    }

    if errors == 0 {
        kprint!("fsck: clean (no errors)\n");
    } else {
        kprint!("fsck: {} error(s) found\n", errors);
    }
    errors
}

/// Disk usage statistics.
pub struct DiskStats {
    pub total_blocks: u32,
    pub used_blocks: u32,
    pub free_blocks: u32,
    pub total_inodes: u32,
    pub used_inodes: u32,
    pub free_inodes: u32,
}

/// Get disk usage statistics for the mounted filesystem.
pub fn disk_stats() -> Option<DiskStats> {
    if !is_mounted() { return None; }

    // Read superblock to get total block count.
    let mut sb_buf = [0u8; SECTOR_SIZE];
    let d = match dev() { Ok(d) => d, Err(_) => return None };
    if crate::block::read(d, SUPERBLOCK_SECTOR, &mut sb_buf).is_err() {
        return None;
    }
    let total_blocks = u32::from_le_bytes([sb_buf[4], sb_buf[5], sb_buf[6], sb_buf[7]]);

    // Count used data blocks from bitmap.
    let mut used_blocks = 0u32;
    for i in 0..total_blocks as usize {
        if bitmap_test(DATA_BITMAP_SECTOR, i).unwrap_or(false) {
            used_blocks += 1;
        }
    }

    // Count used inodes.
    let mut used_inodes = 0u32;
    for i in 0..MAX_INODES {
        if bitmap_test(INODE_BITMAP_SECTOR, i).unwrap_or(false) {
            used_inodes += 1;
        }
    }

    Some(DiskStats {
        total_blocks,
        used_blocks,
        free_blocks: total_blocks.saturating_sub(used_blocks),
        total_inodes: MAX_INODES as u32,
        used_inodes,
        free_inodes: MAX_INODES as u32 - used_inodes,
    })
}
