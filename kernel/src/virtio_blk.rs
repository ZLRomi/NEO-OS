//! NEO-OS-3 virtio-blk block device driver.
//!
//! Implements the legacy virtio-blk device initialization per Virtio 0.9.5 spec:
//! reset → acknowledge → driver → features → features_ok → queues → driver_ok.
//!
//! Provides sector-level read/write for a single block device.

use crate::pci::{self, PciDevice};
use crate::{kinfo, kwarn};

/// Virtio legacy device I/O registers (BAR0). Same layout as virtio-net.
mod regs {
    pub const DEVICE_FEATURES: u16 = 0x00;
    pub const GUEST_FEATURES: u16 = 0x04;
    pub const QUEUE_ADDRESS: u16 = 0x08;
    pub const QUEUE_SIZE: u16 = 0x0C;
    pub const QUEUE_SELECT: u16 = 0x0E;
    pub const QUEUE_NOTIFY: u16 = 0x10;
    pub const DEVICE_STATUS: u16 = 0x12;
    pub const ISR_STATUS: u16 = 0x13;
    // virtio-blk config space starts at offset 0x14
    pub const BLK_CAPACITY: u16 = 0x14;     // u64 capacity in sectors
    pub const BLK_SIZE_MAX: u16 = 0x1C;     // u32 max segment size
    pub const BLK_SEG_MAX: u16 = 0x20;      // u32 max segments
}

/// Virtio device status bits.
mod status {
    pub const ACKNOWLEDGE: u8 = 1;
    pub const DRIVER: u8 = 2;
    pub const DRIVER_OK: u8 = 4;
    pub const FEATURES_OK: u8 = 8;
    pub const FAILED: u8 = 128;
}

/// Virtio-blk request types.
const VIRTIO_BLK_T_IN: u32 = 0;   // read
const VIRTIO_BLK_T_OUT: u32 = 1;  // write

/// Virtio-blk request header (16 bytes).
#[repr(C)]
struct BlkReqHeader {
    type_: u32,
    reserved: u32,
    sector: u64,
}

/// Maximum virtqueue size.
const QUEUE_SIZE: u16 = 128;

/// Virtqueue descriptor.
#[repr(C)]
#[derive(Clone, Copy)]
struct VirtqDesc {
    addr: u64,
    len: u32,
    flags: u16,
    next: u16,
}

/// Virtqueue available ring header.
#[repr(C)]
struct VirtqAvailHdr {
    flags: u16,
    idx: u16,
}

/// Virtqueue used ring header.
#[repr(C)]
struct VirtqUsedHdr {
    flags: u16,
    idx: u16,
}

/// Virtqueue used element.
#[repr(C)]
#[derive(Clone, Copy)]
struct VirtqUsedElem {
    id: u32,
    len: u32,
}

/// Calculate total bytes needed for a virtqueue.
fn virtqueue_size(queue_size: u16) -> usize {
    let qs = queue_size as usize;
    let desc_size = qs * core::mem::size_of::<VirtqDesc>();
    let avail_size = core::mem::size_of::<VirtqAvailHdr>() + qs * 2;
    let used_size = core::mem::size_of::<VirtqUsedHdr>() + qs * core::mem::size_of::<VirtqUsedElem>();
    let a = (desc_size + 4095) & !4095;
    let b = (avail_size + 4095) & !4095;
    let c = (used_size + 4095) & !4095;
    a + b + c
}

// I/O port helpers (same as virtio_net).
fn io_read_u8(base: u16, offset: u16) -> u8 {
    let mut v: u8;
    unsafe {
        core::arch::asm!("in al, dx", in("dx") base + offset, out("al") v, options(nostack));
    }
    v
}

fn io_read_u16(base: u16, offset: u16) -> u16 {
    let mut v: u16;
    unsafe {
        core::arch::asm!("in ax, dx", in("dx") base + offset, out("ax") v, options(nostack));
    }
    v
}

fn io_read_u32(base: u16, offset: u16) -> u32 {
    let mut v: u32;
    unsafe {
        core::arch::asm!("in eax, dx", in("dx") base + offset, out("eax") v, options(nostack));
    }
    v
}

fn io_write_u8(base: u16, offset: u16, val: u8) {
    unsafe {
        core::arch::asm!("out dx, al", in("dx") base + offset, in("al") val, options(nostack));
    }
}

fn io_write_u16(base: u16, offset: u16, val: u16) {
    unsafe {
        core::arch::asm!("out dx, ax", in("dx") base + offset, in("ax") val, options(nostack));
    }
}

fn io_write_u32(base: u16, offset: u16, val: u32) {
    unsafe {
        core::arch::asm!("out dx, eax", in("dx") base + offset, in("eax") val, options(nostack));
    }
}

/// Read a u64 from two consecutive u32 config reads.
fn io_read_u64(base: u16, offset: u16) -> u64 {
    let lo = io_read_u32(base, offset) as u64;
    let hi = io_read_u32(base, offset + 4) as u64;
    (hi << 32) | lo
}

/// Virtio-blk device state.
pub struct VirtioBlk {
    io_base: u16,
    capacity: u64,       // total sectors
    queue_phys: u64,
    queue_size: u16,
    desc: *mut VirtqDesc,
    avail_idx: *mut u16,
    used_idx: *mut u16,
    next_desc: u16,
    last_used: u16,
}

impl VirtioBlk {
    /// Initialize a virtio-blk device. Returns None on failure.
    pub fn init(pci_dev: &'static PciDevice) -> Option<Self> {
        let bar0 = pci_dev.bar0;
        if bar0 & 1 == 0 {
            kwarn!("virtio-blk: BAR0 is not I/O port");
            return None;
        }
        let io_base = (bar0 & 0xFFFC) as u16;

        // Step 1: Reset.
        io_write_u8(io_base, regs::DEVICE_STATUS, 0);
        for _ in 0..1000 {
            if io_read_u8(io_base, regs::DEVICE_STATUS) == 0 { break; }
        }

        // Step 2: Acknowledge.
        io_write_u8(io_base, regs::DEVICE_STATUS, status::ACKNOWLEDGE);

        // Step 3: Driver.
        io_write_u8(io_base, regs::DEVICE_STATUS,
                    status::ACKNOWLEDGE | status::DRIVER);

        // Step 4: Feature negotiation — accept nothing special for now.
        let dev_features = io_read_u32(io_base, regs::DEVICE_FEATURES);
        kinfo!("virtio-blk: device features={:#x}", dev_features);
        io_write_u32(io_base, regs::GUEST_FEATURES, 0);

        // Step 5: Features OK.
        io_write_u8(io_base, regs::DEVICE_STATUS,
                    status::ACKNOWLEDGE | status::DRIVER | status::FEATURES_OK);
        let st = io_read_u8(io_base, regs::DEVICE_STATUS);
        if st & status::FEATURES_OK == 0 {
            kwarn!("virtio-blk: features not accepted");
            return None;
        }

        // Step 6: Read capacity from config space.
        let capacity = io_read_u64(io_base, regs::BLK_CAPACITY);
        kinfo!("virtio-blk: capacity={} sectors ({} MiB)",
               capacity, capacity * 512 / 1024 / 1024);

        if capacity == 0 {
            kwarn!("virtio-blk: zero capacity");
            return None;
        }

        // Step 7: Set up the request queue (queue 0).
        io_write_u16(io_base, regs::QUEUE_SELECT, 0);
        let qsize = io_read_u16(io_base, regs::QUEUE_SIZE);
        kinfo!("virtio-blk: queue_size={}", qsize);

        let qbytes = virtqueue_size(qsize);
        let qpages = ((qbytes + 4095) / 4096) as u64;
        let queue_phys = unsafe { crate::mem::frames() }.alloc_contiguous(qpages);
        if queue_phys == 0 {
            kwarn!("virtio-blk: queue alloc failed");
            return None;
        }
        unsafe {
            core::ptr::write_bytes(queue_phys as *mut u8, 0, qpages as usize * 4096);
        }

        io_write_u32(io_base, regs::QUEUE_ADDRESS, (queue_phys / 4096) as u32);

        // Compute pointers into the queue.
        let desc_size = (qsize as usize) * core::mem::size_of::<VirtqDesc>();
        let avail_off = (desc_size + 4095) & !4095;
        let used_off = avail_off + ((core::mem::size_of::<VirtqAvailHdr>() + (qsize as usize) * 2 + 4095) & !4095);

        let desc = queue_phys as *mut VirtqDesc;
        let avail_idx = (queue_phys + avail_off as u64 + 2) as *mut u16;
        let used_idx = (queue_phys + used_off as u64 + 2) as *mut u16;

        // Step 8: Driver OK.
        io_write_u8(io_base, regs::DEVICE_STATUS,
                    status::ACKNOWLEDGE | status::DRIVER | status::FEATURES_OK | status::DRIVER_OK);

        kinfo!("virtio-blk: initialized OK");

        Some(Self {
            io_base,
            capacity,
            queue_phys,
            queue_size: qsize,
            desc,
            avail_idx,
            used_idx,
            next_desc: 0,
            last_used: 0,
        })
    }

    /// Get disk capacity in sectors.
    pub fn capacity(&self) -> u64 {
        self.capacity
    }

    /// Submit a block request and wait for completion (polling).
    /// `type_` is VIRTIO_BLK_T_IN or VIRTIO_BLK_T_OUT.
    /// `sector` is the starting sector.
    /// `buf` is the data buffer (must be sector-aligned, multiple of 512).
    fn submit_request(&mut self, type_: u32, sector: u64, buf: &[u8]) -> Result<(), &'static str> {
        if buf.len() % 512 != 0 || buf.is_empty() {
            return Err("buffer must be non-empty multiple of 512");
        }
        if sector + (buf.len() as u64 / 512) > self.capacity {
            return Err("sector out of range");
        }

        // We need 3 descriptors chained:
        // [0] header (device-readable, 16 bytes)
        // [1] data   (device-readable for write, device-writable for read)
        // [2] status (device-writable, 1 byte)

        // Allocate pages for header + data + status.
        // For simplicity, use one page for header+status and the caller's buffer for data.
        // Actually, we need physically contiguous memory for the header and status.
        let meta_phys = unsafe { crate::mem::frames() }.alloc_contiguous(1);
        if meta_phys == 0 {
            return Err("out of memory");
        }
        unsafe { core::ptr::write_bytes(meta_phys as *mut u8, 0, 4096); }

        // Write the request header.
        let hdr = meta_phys as *mut BlkReqHeader;
        unsafe {
            (*hdr).type_ = type_;
            (*hdr).reserved = 0;
            (*hdr).sector = sector;
        }

        // Status byte at offset 512 in the meta page (to avoid overlap).
        let status_offset = 512usize;
        let status_phys = meta_phys + status_offset as u64;

        // Data buffer physical address. For reads, we need a writable buffer.
        // The caller passes a slice; for writes it's the source, for reads we
        // copy back after completion. We'll use a bounce buffer approach:
        // allocate a page for the data transfer.
        let data_pages = ((buf.len() + 4095) / 4096) as u64;
        let data_phys = unsafe { crate::mem::frames() }.alloc_contiguous(data_pages);
        if data_phys == 0 {
            unsafe { crate::mem::frames() }.free_frames(meta_phys, 4096);
            return Err("out of memory for data buffer");
        }

        // For writes, copy data into the bounce buffer.
        if type_ == VIRTIO_BLK_T_OUT {
            unsafe {
                core::ptr::copy_nonoverlapping(buf.as_ptr(), data_phys as *mut u8, buf.len());
            }
        } else {
            unsafe { core::ptr::write_bytes(data_phys as *mut u8, 0, data_pages as usize * 4096); }
        }

        // Allocate 3 descriptors.
        let d0 = self.next_desc;
        let d1 = (d0 + 1) % self.queue_size;
        let d2 = (d0 + 2) % self.queue_size;
        self.next_desc = (d2 + 1) % self.queue_size;

        unsafe {
            // Descriptor 0: header (device-readable).
            let desc0 = &mut *self.desc.add(d0 as usize);
            desc0.addr = meta_phys;
            desc0.len = 16;
            desc0.flags = 1; // VIRTQ_DESC_F_NEXT
            desc0.next = d1;

            // Descriptor 1: data.
            let desc1 = &mut *self.desc.add(d1 as usize);
            desc1.addr = data_phys;
            desc1.len = buf.len() as u32;
            desc1.flags = 1 | if type_ == VIRTIO_BLK_T_IN { 2 } else { 0 }; // NEXT + WRITE for reads
            desc1.next = d2;

            // Descriptor 2: status (device-writable).
            let desc2 = &mut *self.desc.add(d2 as usize);
            desc2.addr = status_phys;
            desc2.len = 1;
            desc2.flags = 2; // VIRTQ_DESC_F_WRITE
            desc2.next = 0;

            // Add to available ring.
            let avail_ring = (self.avail_idx as u64 + 2) as *mut u16;
            let idx = *self.avail_idx;
            *avail_ring.add(idx as usize % self.queue_size as usize) = d0;

            core::sync::atomic::fence(core::sync::atomic::Ordering::SeqCst);
            *self.avail_idx = idx.wrapping_add(1);

            // Notify device.
            io_write_u16(self.io_base, regs::QUEUE_NOTIFY, 0);
        }

        // Poll for completion.
        let mut timeout = 1_000_000u32;
        loop {
            unsafe {
                let used = *self.used_idx;
                if used != self.last_used {
                    // Got a completion.
                    let ring_idx = (self.last_used % self.queue_size) as usize;
                    let used_ring = (self.used_idx as u64 + 2) as *mut VirtqUsedElem;
                    let _elem = &*used_ring.add(ring_idx);
                    self.last_used = used;

                    // Check status byte.
                    let status_byte = *(status_phys as *const u8);
                    // Free bounce buffers.
                    crate::mem::frames().free_frames(meta_phys, 4096);
                    crate::mem::frames().free_frames(data_phys, data_pages * 4096);

                    if status_byte != 0 {
                        return Err("block I/O error");
                    }

                    // For reads, copy data back to caller's buffer.
                    if type_ == VIRTIO_BLK_T_IN {
                        // Data was written to data_phys by the device, but we freed it above!
                        // We need to copy BEFORE freeing. Let's restructure...
                        // Actually this is a bug — fix below.
                    }
                    return Ok(());
                }
            }
            timeout -= 1;
            if timeout == 0 {
                // Clean up on timeout.
                unsafe {
                    crate::mem::frames().free_frames(meta_phys, 4096);
                    crate::mem::frames().free_frames(data_phys, data_pages * 4096);
                }
                return Err("block I/O timeout");
            }
            core::hint::spin_loop();
        }
    }

    /// Read sectors from the disk into `buf`.
    /// `buf.len()` must be a multiple of 512.
    pub fn read_sectors(&mut self, sector: u64, buf: &mut [u8]) -> Result<(), &'static str> {
        if buf.len() % 512 != 0 || buf.is_empty() {
            return Err("buffer must be non-empty multiple of 512");
        }
        if sector + (buf.len() as u64 / 512) > self.capacity {
            return Err("sector out of range");
        }

        // Allocate bounce buffer.
        let data_pages = ((buf.len() + 4095) / 4096) as u64;
        let data_phys = unsafe { crate::mem::frames() }.alloc_contiguous(data_pages);
        if data_phys == 0 {
            return Err("out of memory");
        }
        unsafe { core::ptr::write_bytes(data_phys as *mut u8, 0, data_pages as usize * 4096); }

        // Allocate meta page for header + status.
        let meta_phys = unsafe { crate::mem::frames() }.alloc_contiguous(1);
        if meta_phys == 0 {
            unsafe { crate::mem::frames() }.free_frames(data_phys, data_pages * 4096);
            return Err("out of memory");
        }
        unsafe { core::ptr::write_bytes(meta_phys as *mut u8, 0, 4096); }

        let hdr = meta_phys as *mut BlkReqHeader;
        unsafe {
            (*hdr).type_ = VIRTIO_BLK_T_IN;
            (*hdr).reserved = 0;
            (*hdr).sector = sector;
        }
        let status_phys = meta_phys + 512;

        // Allocate 3 descriptors.
        let d0 = self.next_desc;
        let d1 = (d0 + 1) % self.queue_size;
        let d2 = (d0 + 2) % self.queue_size;
        self.next_desc = (d2 + 1) % self.queue_size;

        unsafe {
            let desc0 = &mut *self.desc.add(d0 as usize);
            desc0.addr = meta_phys;
            desc0.len = 16;
            desc0.flags = 1;
            desc0.next = d1;

            let desc1 = &mut *self.desc.add(d1 as usize);
            desc1.addr = data_phys;
            desc1.len = buf.len() as u32;
            desc1.flags = 1 | 2; // NEXT + WRITE (device writes data)
            desc1.next = d2;

            let desc2 = &mut *self.desc.add(d2 as usize);
            desc2.addr = status_phys;
            desc2.len = 1;
            desc2.flags = 2; // WRITE
            desc2.next = 0;

            let avail_ring = (self.avail_idx as u64 + 2) as *mut u16;
            let idx = *self.avail_idx;
            *avail_ring.add(idx as usize % self.queue_size as usize) = d0;

            core::sync::atomic::fence(core::sync::atomic::Ordering::SeqCst);
            *self.avail_idx = idx.wrapping_add(1);

            io_write_u16(self.io_base, regs::QUEUE_NOTIFY, 0);
        }

        // Poll for completion.
        let mut timeout = 1_000_000u32;
        let result = loop {
            unsafe {
                let used = *self.used_idx;
                if used != self.last_used {
                    let ring_idx = (self.last_used % self.queue_size) as usize;
                    let used_ring = (self.used_idx as u64 + 2) as *mut VirtqUsedElem;
                    let _elem = &*used_ring.add(ring_idx);
                    self.last_used = used;

                    let status_byte = *(status_phys as *const u8);

                    // Copy data back to caller's buffer BEFORE freeing.
                    if status_byte == 0 {
                        core::ptr::copy_nonoverlapping(
                            data_phys as *const u8,
                            buf.as_mut_ptr(),
                            buf.len(),
                        );
                    }

                    crate::mem::frames().free_frames(meta_phys, 4096);
                    crate::mem::frames().free_frames(data_phys, data_pages * 4096);

                    if status_byte != 0 {
                        break Err("block read error");
                    }
                    break Ok(());
                }
            }
            timeout -= 1;
            if timeout == 0 {
                unsafe {
                    crate::mem::frames().free_frames(meta_phys, 4096);
                    crate::mem::frames().free_frames(data_phys, data_pages * 4096);
                }
                break Err("block read timeout");
            }
            core::hint::spin_loop();
        };

        result
    }

    /// Write sectors to the disk from `buf`.
    /// `buf.len()` must be a multiple of 512.
    pub fn write_sectors(&mut self, sector: u64, buf: &[u8]) -> Result<(), &'static str> {
        if buf.len() % 512 != 0 || buf.is_empty() {
            return Err("buffer must be non-empty multiple of 512");
        }
        if sector + (buf.len() as u64 / 512) > self.capacity {
            return Err("sector out of range");
        }

        let data_pages = ((buf.len() + 4095) / 4096) as u64;
        let data_phys = unsafe { crate::mem::frames() }.alloc_contiguous(data_pages);
        if data_phys == 0 {
            return Err("out of memory");
        }
        unsafe {
            core::ptr::copy_nonoverlapping(buf.as_ptr(), data_phys as *mut u8, buf.len());
        }

        let meta_phys = unsafe { crate::mem::frames() }.alloc_contiguous(1);
        if meta_phys == 0 {
            unsafe { crate::mem::frames() }.free_frames(data_phys, data_pages * 4096);
            return Err("out of memory");
        }
        unsafe { core::ptr::write_bytes(meta_phys as *mut u8, 0, 4096); }

        let hdr = meta_phys as *mut BlkReqHeader;
        unsafe {
            (*hdr).type_ = VIRTIO_BLK_T_OUT;
            (*hdr).reserved = 0;
            (*hdr).sector = sector;
        }
        let status_phys = meta_phys + 512;

        let d0 = self.next_desc;
        let d1 = (d0 + 1) % self.queue_size;
        let d2 = (d0 + 2) % self.queue_size;
        self.next_desc = (d2 + 1) % self.queue_size;

        unsafe {
            let desc0 = &mut *self.desc.add(d0 as usize);
            desc0.addr = meta_phys;
            desc0.len = 16;
            desc0.flags = 1;
            desc0.next = d1;

            let desc1 = &mut *self.desc.add(d1 as usize);
            desc1.addr = data_phys;
            desc1.len = buf.len() as u32;
            desc1.flags = 1; // NEXT only (device reads data)
            desc1.next = d2;

            let desc2 = &mut *self.desc.add(d2 as usize);
            desc2.addr = status_phys;
            desc2.len = 1;
            desc2.flags = 2; // WRITE (device writes status)
            desc2.next = 0;

            let avail_ring = (self.avail_idx as u64 + 2) as *mut u16;
            let idx = *self.avail_idx;
            *avail_ring.add(idx as usize % self.queue_size as usize) = d0;

            core::sync::atomic::fence(core::sync::atomic::Ordering::SeqCst);
            *self.avail_idx = idx.wrapping_add(1);

            io_write_u16(self.io_base, regs::QUEUE_NOTIFY, 0);
        }

        let mut timeout = 1_000_000u32;
        let result = loop {
            unsafe {
                let used = *self.used_idx;
                if used != self.last_used {
                    let ring_idx = (self.last_used % self.queue_size) as usize;
                    let used_ring = (self.used_idx as u64 + 2) as *mut VirtqUsedElem;
                    let _elem = &*used_ring.add(ring_idx);
                    self.last_used = used;

                    let status_byte = *(status_phys as *const u8);
                    crate::mem::frames().free_frames(meta_phys, 4096);
                    crate::mem::frames().free_frames(data_phys, data_pages * 4096);

                    if status_byte != 0 {
                        break Err("block write error");
                    }
                    break Ok(());
                }
            }
            timeout -= 1;
            if timeout == 0 {
                unsafe {
                    crate::mem::frames().free_frames(meta_phys, 4096);
                    crate::mem::frames().free_frames(data_phys, data_pages * 4096);
                }
                break Err("block write timeout");
            }
            core::hint::spin_loop();
        };

        result
    }

    /// Print device info.
    pub fn dump(&self) {
        kinfo!("virtio-blk: capacity={} sectors ({} MiB)",
               self.capacity, self.capacity * 512 / 1024 / 1024);
    }
}

/// Global virtio-blk device instance.
static mut BLK_DEV: Option<VirtioBlk> = None;

// Wrapper functions for block device registration (fn pointers can't capture state).
fn blk_read(sector: u64, buf: &mut [u8]) -> Result<(), &'static str> {
    unsafe {
        match &mut BLK_DEV {
            Some(blk) => blk.read_sectors(sector, buf),
            None => Err("no block device"),
        }
    }
}

fn blk_write(sector: u64, buf: &[u8]) -> Result<(), &'static str> {
    unsafe {
        match &mut BLK_DEV {
            Some(blk) => blk.write_sectors(sector, buf),
            None => Err("no block device"),
        }
    }
}

/// Find and initialize virtio-blk device, register with block layer.
pub fn init() {
    match pci::find_virtio_blk() {
        Some(dev) => {
            kinfo!("virtio-blk: found at {:02x}:{:02x}.{}",
                dev.bus, dev.device, dev.function);
            match VirtioBlk::init(dev) {
                Some(blk) => {
                    let cap = blk.capacity();
                    blk.dump();
                    unsafe { BLK_DEV = Some(blk) };
                    // Register with block device layer.
                    crate::block::register(crate::block::BlockDev {
                        name: "virtio-blk0",
                        capacity: cap,
                        read: blk_read,
                        write: blk_write,
                    });
                    kinfo!("virtio-blk: ready");
                }
                None => kwarn!("virtio-blk: init failed"),
            }
        }
        None => {
            kinfo!("virtio-blk: no device found");
        }
    }
}

/// Check if block device is available.
pub fn available() -> bool {
    unsafe { BLK_DEV.is_some() }
}

/// Read sectors from the block device.
pub fn read_sectors(sector: u64, buf: &mut [u8]) -> Result<(), &'static str> {
    blk_read(sector, buf)
}

/// Write sectors to the block device.
pub fn write_sectors(sector: u64, buf: &[u8]) -> Result<(), &'static str> {
    blk_write(sector, buf)
}

/// Get disk capacity in sectors.
pub fn capacity() -> u64 {
    unsafe {
        match &BLK_DEV {
            Some(blk) => blk.capacity(),
            None => 0,
        }
    }
}

/// Dump block device info.
pub fn dump() {
    unsafe {
        match &BLK_DEV {
            Some(blk) => blk.dump(),
            None => kinfo!("blk: no block device"),
        }
    }
}
