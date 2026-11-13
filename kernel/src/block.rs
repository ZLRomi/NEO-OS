//! NEO-OS-3 Block Device Abstraction Layer.
//!
//! Provides a hardware-independent interface for block devices.
//! Drivers (virtio-blk, etc.) register themselves here.
//! The VFS and filesystem layers use this API instead of talking to hardware directly.

use crate::kinfo;

/// Maximum number of registered block devices.
const MAX_BLOCK_DEVS: usize = 4;

/// Block device operations.
pub struct BlockDev {
    /// Human-readable name (e.g., "virtio-blk0").
    pub name: &'static str,
    /// Total capacity in 512-byte sectors.
    pub capacity: u64,
    /// Read sectors into buf. buf.len() must be multiple of 512.
    pub read: fn(sector: u64, buf: &mut [u8]) -> Result<(), &'static str>,
    /// Write sectors from buf. buf.len() must be multiple of 512.
    pub write: fn(sector: u64, buf: &[u8]) -> Result<(), &'static str>,
}

static mut DEVS: [Option<BlockDev>; MAX_BLOCK_DEVS] = {
    const NONE: Option<BlockDev> = None;
    [NONE; MAX_BLOCK_DEVS]
};
static mut NDEVS: usize = 0;

/// Register a block device. Returns device index or None if full.
pub fn register(dev: BlockDev) -> Option<usize> {
    unsafe {
        if NDEVS >= MAX_BLOCK_DEVS {
            return None;
        }
        let idx = NDEVS;
        DEVS[idx] = Some(dev);
        NDEVS += 1;
        kinfo!("block: registered '{}' ({} sectors)", 
               DEVS[idx].as_ref().unwrap().name,
               DEVS[idx].as_ref().unwrap().capacity);
        Some(idx)
    }
}

/// Get a block device by index.
pub fn get(idx: usize) -> Option<&'static BlockDev> {
    unsafe {
        if idx < NDEVS {
            DEVS[idx].as_ref()
        } else {
            None
        }
    }
}

/// Get the number of registered block devices.
pub fn count() -> usize {
    unsafe { NDEVS }
}

/// Read sectors from a block device.
pub fn read(dev_idx: usize, sector: u64, buf: &mut [u8]) -> Result<(), &'static str> {
    match get(dev_idx) {
        Some(dev) => (dev.read)(sector, buf),
        None => Err("no such block device"),
    }
}

/// Write sectors to a block device.
pub fn write(dev_idx: usize, sector: u64, buf: &[u8]) -> Result<(), &'static str> {
    match get(dev_idx) {
        Some(dev) => (dev.write)(sector, buf),
        None => Err("no such block device"),
    }
}

/// Get capacity of a block device.
pub fn capacity(dev_idx: usize) -> u64 {
    match get(dev_idx) {
        Some(dev) => dev.capacity,
        None => 0,
    }
}

/// Dump all registered block devices.
pub fn dump() {
    let n = count();
    kinfo!("block: {} device(s)", n);
    for i in 0..n {
        if let Some(dev) = get(i) {
            kinfo!("  [{}] {} — {} sectors ({} MiB)",
                   i, dev.name, dev.capacity, dev.capacity * 512 / 1024 / 1024);
        }
    }
}
