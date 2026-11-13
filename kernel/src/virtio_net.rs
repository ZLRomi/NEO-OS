//! NEO-OS-3 virtio-net driver.
//!
//! Implements the virtio-net device initialization sequence per Virtio 1.0 spec:
//! reset → acknowledge → driver → features → features_ok → queues → driver_ok.

use crate::pci::{self, PciDevice};
use crate::{kinfo, kwarn};
use core::sync::atomic::{AtomicBool, Ordering};

/// Flag: ISR signaled new RX packets available.
static RX_PENDING: AtomicBool = AtomicBool::new(false);

/// Called from IRQ handler when virtio-net interrupt fires.
pub fn irq_handler() {
    // Read ISR status register to acknowledge interrupt.
    unsafe {
        if let Some(ref net) = NET_DEV {
            let isr = core::ptr::read_volatile((net.io_base + 0x13) as *const u8);
            if isr & 0x01 != 0 {
                // Used ring updated (RX or TX completion).
                RX_PENDING.store(true, Ordering::Release);
            }
            // Write back to clear interrupt.
            core::ptr::write_volatile((net.io_base + 0x13) as *mut u8, isr);
        }
    }
}

/// Check and clear the RX pending flag.
pub fn rx_pending() -> bool {
    RX_PENDING.swap(false, Ordering::AcqRel)
}

/// Virtio legacy device I/O registers (BAR0).
mod regs {
    pub const DEVICE_FEATURES: u16 = 0x00;      // u32 (read)
    pub const GUEST_FEATURES: u16 = 0x04;      // u32 (write)
    pub const QUEUE_ADDRESS: u16 = 0x08;       // u32 (write)
    pub const QUEUE_SIZE: u16 = 0x0C;          // u16 (read)
    pub const QUEUE_SELECT: u16 = 0x0E;        // u16 (write)
    pub const QUEUE_NOTIFY: u16 = 0x10;        // u16 (write)
    pub const DEVICE_STATUS: u16 = 0x12;       // u8 (read/write)
    pub const ISR_STATUS: u16 = 0x13;          // u8 (read)
    pub const MAC: u16 = 0x14;                 // 6 bytes MAC address
    pub const STATUS: u16 = 0x1A;              // u16 (link status)
}

/// Virtio device status bits.
mod status {
    pub const ACKNOWLEDGE: u8 = 1;
    pub const DRIVER: u8 = 2;
    pub const DRIVER_OK: u8 = 4;
    pub const FEATURES_OK: u8 = 8;
    pub const FAILED: u8 = 128;
}

/// Virtio-net feature bits we negotiate.
const FEATURE_MAC: u32 = 1 << 5;
const FEATURE_STATUS: u32 = 1 << 16;

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

/// Virtqueue available ring header (fixed part).
#[repr(C)]
struct VirtqAvailHdr {
    flags: u16,
    idx: u16,
    // ring[] follows
}

/// Virtqueue used ring header (fixed part).
#[repr(C)]
struct VirtqUsedHdr {
    flags: u16,
    idx: u16,
    // ring[] follows
}

/// Virtqueue used element.
#[repr(C)]
#[derive(Clone, Copy)]
struct VirtqUsedElem {
    id: u32,
    len: u32,
}

/// Calculate total bytes needed for a virtqueue of the given size.
/// Layout: desc[] + avail + used (each page-aligned).
fn virtqueue_size(queue_size: u16) -> usize {
    let qs = queue_size as usize;
    let desc_size = qs * core::mem::size_of::<VirtqDesc>();
    let avail_size = core::mem::size_of::<VirtqAvailHdr>() + qs * 2;
    let used_size = core::mem::size_of::<VirtqUsedHdr>() + qs * core::mem::size_of::<VirtqUsedElem>();
    // Align each section to page boundary.
    let a = (desc_size + 4095) & !4095;
    let b = (avail_size + 4095) & !4095;
    let c = (used_size + 4095) & !4095;
    a + b + c
}

/// Read from device I/O port.
fn io_read_u16(base: u16, offset: u16) -> u16 {
    let mut v: u16;
    unsafe {
        core::arch::asm!(
            "in ax, dx",
            in("dx") base + offset,
            out("ax") v,
            options(nostack),
        );
    }
    v
}

fn io_read_u8(base: u16, offset: u16) -> u8 {
    let mut v: u8;
    unsafe {
        core::arch::asm!(
            "in al, dx",
            in("dx") base + offset,
            out("al") v,
            options(nostack),
        );
    }
    v
}

fn io_read_u32(base: u16, offset: u16) -> u32 {
    let mut v: u32;
    unsafe {
        core::arch::asm!(
            "in eax, dx",
            in("dx") base + offset,
            out("eax") v,
            options(nostack),
        );
    }
    v
}

fn io_write_u16(base: u16, offset: u16, val: u16) {
    unsafe {
        core::arch::asm!(
            "out dx, ax",
            in("dx") base + offset,
            in("ax") val,
            options(nostack),
        );
    }
}

fn io_write_u8(base: u16, offset: u16, val: u8) {
    unsafe {
        core::arch::asm!(
            "out dx, al",
            in("dx") base + offset,
            in("al") val,
            options(nostack),
        );
    }
}

fn io_write_u32(base: u16, offset: u16, val: u32) {
    unsafe {
        core::arch::asm!(
            "out dx, eax",
            in("dx") base + offset,
            in("eax") val,
            options(nostack),
        );
    }
}

/// Virtio-net device state.
pub struct VirtioNet {
    io_base: u16,
    mac: [u8; 6],
    rx_queue_phys: u64,
    tx_queue_phys: u64,
    rx_queue_size: u16,
    tx_queue_size: u16,
    rx_desc: *mut VirtqDesc,
    rx_avail_idx: *mut u16,
    rx_used_idx: *mut u16,
    tx_desc: *mut VirtqDesc,
    tx_avail_idx: *mut u16,
    tx_used_idx: *mut u16,
    tx_free_idx: u16,
    rx_last_used: u16,
}

impl VirtioNet {
    /// Initialize a virtio-net device. Returns None on failure.
    pub fn init(pci_dev: &'static PciDevice) -> Option<Self> {
        let bar0 = pci_dev.bar0;
        if bar0 & 1 == 0 {
            kwarn!("virtio-net: BAR0 is not I/O port");
            return None;
        }
        let io_base = (bar0 & 0xFFFC) as u16;

        // Step 1: Reset the device.
        io_write_u8(io_base, regs::DEVICE_STATUS, 0);
        // Wait for reset to complete (status becomes 0).
        for _ in 0..1000 {
            if io_read_u8(io_base, regs::DEVICE_STATUS) == 0 {
                break;
            }
        }

        // Step 2: Acknowledge — we noticed the device.
        io_write_u8(io_base, regs::DEVICE_STATUS, status::ACKNOWLEDGE);

        // Step 3: Driver — we know how to drive it.
        io_write_u8(io_base, regs::DEVICE_STATUS,
                    status::ACKNOWLEDGE | status::DRIVER);

        // Step 4: Read device features and negotiate.
        let dev_features = io_read_u32(io_base, regs::DEVICE_FEATURES);
        kinfo!("virtio-net: device features={:#x}", dev_features);

        // We accept MAC and STATUS features.
        let guest_features = dev_features & (FEATURE_MAC | FEATURE_STATUS);
        io_write_u32(io_base, regs::GUEST_FEATURES, guest_features);

        // Step 5: Features OK.
        io_write_u8(io_base, regs::DEVICE_STATUS,
                    status::ACKNOWLEDGE | status::DRIVER | status::FEATURES_OK);

        // Verify device accepted our features.
        let status_val = io_read_u8(io_base, regs::DEVICE_STATUS);
        if status_val & status::FEATURES_OK == 0 {
            kwarn!("virtio-net: features not accepted");
            return None;
        }

        // Step 6: Read MAC address.
        let mut mac = [0u8; 6];
        for i in 0..6 {
            mac[i] = io_read_u8(io_base, regs::MAC + i as u16);
        }

        // Step 7: Allocate and initialize virtqueues.
        io_write_u16(io_base, regs::QUEUE_SELECT, 0); // RX queue
        let rx_size = io_read_u16(io_base, regs::QUEUE_SIZE);
        io_write_u16(io_base, regs::QUEUE_SELECT, 1); // TX queue
        let tx_size = io_read_u16(io_base, regs::QUEUE_SIZE);
        kinfo!("virtio-net: rx_queue_size={} tx_queue_size={}", rx_size, tx_size);

        // Allocate RX queue memory.
        let rx_bytes = virtqueue_size(rx_size);
        let rx_pages = ((rx_bytes + 4095) / 4096) as u64;
        let rx_queue_phys = unsafe { crate::mem::frames() }.alloc_contiguous(rx_pages);
        if rx_queue_phys == 0 {
            kwarn!("virtio-net: RX queue alloc failed");
            return None;
        }
        unsafe {
            core::ptr::write_bytes(rx_queue_phys as *mut u8, 0, rx_pages as usize * 4096);
        }
        // Program the queue address (physical frame number).
        io_write_u16(io_base, regs::QUEUE_SELECT, 0);
        io_write_u32(io_base, regs::QUEUE_ADDRESS, (rx_queue_phys / 4096) as u32);

        // Allocate TX queue memory.
        let tx_bytes = virtqueue_size(tx_size);
        let tx_pages = ((tx_bytes + 4095) / 4096) as u64;
        let tx_queue_phys = unsafe { crate::mem::frames() }.alloc_contiguous(tx_pages);
        if tx_queue_phys == 0 {
            kwarn!("virtio-net: TX queue alloc failed");
            unsafe { crate::mem::frames() }.free_frames(rx_queue_phys, rx_pages * 4096);
            return None;
        }
        unsafe {
            core::ptr::write_bytes(tx_queue_phys as *mut u8, 0, tx_pages as usize * 4096);
        }
        io_write_u16(io_base, regs::QUEUE_SELECT, 1);
        io_write_u32(io_base, regs::QUEUE_ADDRESS, (tx_queue_phys / 4096) as u32);

        // Set up pointers to queue structures.
        let rx_desc_size = (rx_size as usize) * core::mem::size_of::<VirtqDesc>();
        let rx_avail_off = (rx_desc_size + 4095) & !4095;
        let rx_used_off = rx_avail_off + ((core::mem::size_of::<VirtqAvailHdr>() + (rx_size as usize) * 2 + 4095) & !4095);

        let tx_desc_size = (tx_size as usize) * core::mem::size_of::<VirtqDesc>();
        let tx_avail_off = (tx_desc_size + 4095) & !4095;
        let tx_used_off = tx_avail_off + ((core::mem::size_of::<VirtqAvailHdr>() + (tx_size as usize) * 2 + 4095) & !4095);

        let rx_desc = rx_queue_phys as *mut VirtqDesc;
        let rx_avail_idx = (rx_queue_phys + rx_avail_off as u64 + 2) as *mut u16;
        let rx_used_idx = (rx_queue_phys + rx_used_off as u64 + 2) as *mut u16;

        let tx_desc = tx_queue_phys as *mut VirtqDesc;
        let tx_avail_idx = (tx_queue_phys + tx_avail_off as u64 + 2) as *mut u16;
        let tx_used_idx = (tx_queue_phys + tx_used_off as u64 + 2) as *mut u16;

        // Pre-fill RX queue with buffers (up to QUEUE_SIZE entries).
        let rx_fill = core::cmp::min(rx_size, QUEUE_SIZE);
        for i in 0..rx_fill as usize {
            let buf = unsafe { crate::mem::frames() }.alloc_contiguous(1);
            if buf == 0 { break; }
            unsafe {
                core::ptr::write_bytes(buf as *mut u8, 0, 4096);
                // Write virtio-net header (10 bytes) + buffer.
                let hdr = buf as *mut u8;
                // Header: flags=0, gso_type=0, hdr_len=0, gso_size=0, csum_start=0, csum_offset=0
                core::ptr::write_bytes(hdr, 0, 10);

                let desc = &mut *rx_desc.add(i);
                desc.addr = buf;
                desc.len = 4096 - 10; // buffer size minus header
                desc.flags = 2; // VIRTQ_DESC_F_WRITE (device writes to buffer)
                desc.next = 0;

                // Add to available ring.
                let avail_ring = (rx_avail_idx as u64 + 2) as *mut u16;
                *avail_ring.add(i) = i as u16;
            }
        }
        // Set avail idx to indicate we filled slots.
        unsafe { *rx_avail_idx = rx_fill; }
        // Notify device that RX buffers are available.
        io_write_u16(io_base, regs::QUEUE_NOTIFY, 0); // RX queue

        // Step 8: Driver OK.
        io_write_u8(io_base, regs::DEVICE_STATUS,
                    status::ACKNOWLEDGE | status::DRIVER | status::FEATURES_OK | status::DRIVER_OK);

        kinfo!(
            "virtio-net: MAC {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
        );

        Some(Self {
            io_base,
            mac,
            rx_queue_phys,
            tx_queue_phys,
            rx_queue_size: rx_size,
            tx_queue_size: tx_size,
            rx_desc,
            rx_avail_idx,
            rx_used_idx,
            tx_desc,
            tx_avail_idx,
            tx_used_idx,
            tx_free_idx: 0,
            rx_last_used: 0,
        })
    }

    /// Get the MAC address.
    pub fn mac(&self) -> [u8; 6] {
        self.mac
    }

    /// Send a raw Ethernet frame via the TX virtqueue.
    pub fn send_raw(&mut self, data: &[u8]) -> Result<(), &'static str> {
        if data.len() > 1514 {
            return Err("packet too large");
        }

        // Allocate a buffer for the packet + virtio-net header (10 bytes).
        let total_len = data.len() + 10;
        let buf_phys = unsafe { crate::mem::frames() }.alloc_contiguous(1);
        if buf_phys == 0 {
            return Err("out of memory");
        }

        // Zero the virtio header (10 bytes) and copy data.
        unsafe {
            core::ptr::write_bytes(buf_phys as *mut u8, 0, 4096);
            core::ptr::copy_nonoverlapping(data.as_ptr(), (buf_phys + 10) as *mut u8, data.len());
        }

        // Find a free descriptor in the TX queue.
        let idx = self.tx_free_idx;
        if idx >= self.tx_queue_size {
            unsafe { crate::mem::frames() }.free_frames(buf_phys, 4096);
            return Err("tx queue full");
        }

        // Fill the descriptor.
        unsafe {
            let desc = &mut *self.tx_desc.add(idx as usize);
            desc.addr = buf_phys;
            desc.len = total_len as u32;
            desc.flags = 0; // device-readable
            desc.next = 0;

            // Add to available ring.
            let avail_idx = *self.tx_avail_idx;
            let avail_ring = (self.tx_avail_idx as u64 + 2) as *mut u16; // ring[] follows hdr
            *avail_ring.add(avail_idx as usize % self.tx_queue_size as usize) = idx;

            // Memory barrier to ensure descriptor is visible before index update.
            core::sync::atomic::fence(core::sync::atomic::Ordering::SeqCst);
            *self.tx_avail_idx = avail_idx.wrapping_add(1);

            // Notify the device.
            io_write_u16(self.io_base, regs::QUEUE_NOTIFY, 1); // TX queue
        }

        self.tx_free_idx += 1;
        Ok(())
    }

    /// Poll the RX queue for received packets. Returns packet length or 0.
    /// Caller must recycle the buffer after processing.
    pub fn poll_rx(&mut self) -> Option<(&'static [u8], u16)> {
        unsafe {
            let used_idx = *self.rx_used_idx;
            let last_processed = self.rx_last_used;
            if used_idx == last_processed {
                return None; // no new packets
            }
            let ring_idx = (last_processed % self.rx_queue_size) as usize;
            let used_ring = (self.rx_used_idx as u64 + 2) as *mut VirtqUsedElem;
            let used = &*used_ring.add(ring_idx);
            let desc_idx = used.id as usize;
            let len = used.len;

            // Get the descriptor and its buffer.
            let desc = &*self.rx_desc.add(desc_idx);
            let buf = desc.addr as *const u8;
            // Packet data starts after the 10-byte virtio-net header.
            let pkt_data = core::slice::from_raw_parts(buf.add(10), (len as usize).saturating_sub(10));

            // Recycle the descriptor: put it back in the available ring.
            let avail_idx = *self.rx_avail_idx;
            let avail_ring = (self.rx_avail_idx as u64 + 2) as *mut u16;
            *avail_ring.add((avail_idx % self.rx_queue_size) as usize) = desc_idx as u16;
            core::sync::atomic::fence(core::sync::atomic::Ordering::SeqCst);
            *self.rx_avail_idx = avail_idx.wrapping_add(1);
            io_write_u16(self.io_base, regs::QUEUE_NOTIFY, 0); // RX queue

            self.rx_last_used = used_idx;
            Some((pkt_data, desc_idx as u16))
        }
    }

    /// Print device status.
    pub fn dump(&self) {
        kinfo!("virtio-net: MAC={:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
            self.mac[0], self.mac[1], self.mac[2], self.mac[3], self.mac[4], self.mac[5]);
    }
}

/// Global virtio-net device instance.
static mut NET_DEV: Option<VirtioNet> = None;
/// Flag: UDP echo server enabled.
static ECHO_ENABLED: core::sync::atomic::AtomicBool = core::sync::atomic::AtomicBool::new(false);

/// Initialize the network subsystem: find and init virtio-net.
pub fn init() {
    match pci::find_virtio_net() {
        Some(dev) => {
            kinfo!("virtio-net: found at {:02x}:{:02x}.{} IRQ={}",
                dev.bus, dev.device, dev.function, dev.irq_line);
            let irq = dev.irq_line;
            match VirtioNet::init(dev) {
                Some(net) => {
                    net.dump();
                    unsafe { NET_DEV = Some(net) };
                    // Register IRQ handler (IRQ line + PIC offset 32).
                    if irq > 0 && irq < 16 {
                        crate::interrupts::register_irq_handler(irq, virtio_net_irq_entry);
                        kinfo!("virtio-net: IRQ {} registered", irq);
                    }
                    kinfo!("virtio-net: initialized");
                }
                None => kwarn!("virtio-net: init failed"),
            }
        }
        None => {
            kinfo!("virtio-net: no device found");
        }
    }
}

/// IRQ entry point for virtio-net.
extern "x86-interrupt" fn virtio_net_irq_entry(_frame: crate::interrupts::Frame) {
    irq_handler();
    // Send EOI to PIC.
    unsafe {
        let irq = 11u8; // typical virtio-net IRQ on q35
        if irq >= 8 {
            core::ptr::write_volatile(0xA0 as *mut u8, 0x20); // slave PIC EOI
        }
        core::ptr::write_volatile(0x20 as *mut u8, 0x20); // master PIC EOI
    }
}

/// Print network status.
pub fn dump() {
    unsafe {
        match &NET_DEV {
            Some(net) => net.dump(),
            None => kinfo!("net: no network device"),
        }
    }
}

/// Send a packet through the network device.
pub fn send(data: &[u8]) -> Result<(), &'static str> {
    unsafe {
        match &mut NET_DEV {
            Some(net) => net.send_raw(data),
            None => Err("no network device"),
        }
    }
}

/// Poll for received packets. Returns (packet_data, buffer_id) or None.
/// The caller should process the packet and call recycle_rx_buf to return
/// the buffer to the queue.
pub fn poll() -> Option<(&'static [u8], u16)> {
    unsafe {
        match &mut NET_DEV {
            Some(net) => net.poll_rx(),
            None => None,
        }
    }
}

/// Handle ICMP echo request: build and send reply.
fn handle_icmp_echo(pkt: &[u8], ip_hdr_len: usize, icmp_start: usize) {
    let src_ip = [pkt[26], pkt[27], pkt[28], pkt[29]];
    let dst_ip = [pkt[30], pkt[31], pkt[32], pkt[33]];
    let icmp_len = pkt.len() - icmp_start;

    // Build reply packet: Eth(14) + IP(20) + ICMP(icmp_len).
    let total_len = 14 + 20 + icmp_len;
    if total_len > 1514 { return; }

    let mut reply = [0u8; 1514];

    // Ethernet: swap src/dst MAC.
    reply[0..6].copy_from_slice(&pkt[6..12]);   // dst = original src
    reply[6..12].copy_from_slice(&pkt[0..6]);     // src = original dst
    reply[12] = 0x08; reply[13] = 0x00;           // IPv4

    // IP header.
    let ip_total_len = (20 + icmp_len) as u16;
    crate::ip::build_ipv4_header(
        &mut reply[14..],
        ip_total_len,
        1,      // ICMP
        dst_ip, // our IP as source
        src_ip, // requester as destination
    );

    // ICMP reply: type=0 (echo reply), code=0, same id/seq/data.
    let reply_icmp = 14 + 20;
    reply[reply_icmp] = 0;     // type = echo reply
    reply[reply_icmp + 1] = 0; // code = 0
    reply[reply_icmp + 2] = 0; // checksum (filled below)
    reply[reply_icmp + 3] = 0;
    // Copy identifier + sequence + data from request.
    if icmp_len > 4 {
        reply[reply_icmp + 4..reply_icmp + icmp_len]
            .copy_from_slice(&pkt[icmp_start + 4..icmp_start + icmp_len]);
    }

    // Calculate ICMP checksum.
    let csum = crate::ip::ipv4_checksum(&reply[reply_icmp..reply_icmp + icmp_len]);
    reply[reply_icmp + 2] = (csum >> 8) as u8;
    reply[reply_icmp + 3] = csum as u8;

    let _ = send(&reply[..total_len]);
}

/// Process a received packet: check if it's a TCP packet and handle it.
pub fn process_packet(pkt: &[u8]) {
    if pkt.len() < 54 {
        return; // too short for Eth+IP+TCP
    }
    // Check IP protocol = TCP (6).
    if pkt[23] != 6 {
        return;
    }
    // Parse TCP header.
    let ip_hdr_len = (pkt[14] & 0x0F) as usize * 4;
    let tcp_start = 14 + ip_hdr_len;
    if pkt.len() < tcp_start + 20 {
        return;
    }
    let src_port = ((pkt[tcp_start] as u16) << 8) | pkt[tcp_start + 1] as u16;
    let dst_port = ((pkt[tcp_start + 2] as u16) << 8) | pkt[tcp_start + 3] as u16;
    let seq = u32::from_be_bytes([pkt[tcp_start+4], pkt[tcp_start+5], pkt[tcp_start+6], pkt[tcp_start+7]]);
    let ack = u32::from_be_bytes([pkt[tcp_start+8], pkt[tcp_start+9], pkt[tcp_start+10], pkt[tcp_start+11]]);
    let flags = pkt[tcp_start + 13];
    let window = u16::from_be_bytes([pkt[tcp_start + 14], pkt[tcp_start + 15]]);
    let src_ip = [pkt[26], pkt[27], pkt[28], pkt[29]];
    let tcp_hdr_len = ((pkt[tcp_start + 12] >> 4) & 0xF) as usize * 4;
    let payload_start = tcp_start + tcp_hdr_len;
    let payload = if payload_start < pkt.len() { &pkt[payload_start..] } else { &[] };

    // Handle TCP segment (including payload for data transfer).
    let state_changed = crate::tcp::handle_segment(src_ip, src_port, dst_port, flags, seq, ack, window, payload);
    if state_changed {
        crate::sinfo!("tcp: state={}", crate::tcp::state_name());
    }
}

/// Enable/disable the UDP echo server.
pub fn set_echo(enabled: bool) {
    ECHO_ENABLED.store(enabled, core::sync::atomic::Ordering::SeqCst);
}

/// Check if echo server is enabled.
pub fn echo_enabled() -> bool {
    ECHO_ENABLED.load(core::sync::atomic::Ordering::SeqCst)
}

/// UDP echo server task: poll RX queue and echo back UDP packets.
/// This runs as a kernel task.
pub fn echo_task() {
    crate::sinfo!("udpecho: started");
    loop {
        if !echo_enabled() {
            crate::interrupts::sleep_ms(100);
            continue;
        }
        if let Some((pkt, _buf_id)) = poll() {
            // Handle ARP packets.
            if pkt.len() >= 42 && pkt[12] == 0x08 && pkt[13] == 0x06 {
                crate::arp::handle_packet(pkt);
                continue;
            }

            // Process TCP packets for handshake.
            process_packet(pkt);

            // Handle ICMP echo requests (ping).
            if pkt.len() >= 34 && pkt[23] == 1 {
                // IP protocol = ICMP (1).
                let ip_hdr_len = (pkt[14] & 0x0F) as usize * 4;
                let icmp_start = 14 + ip_hdr_len;
                if pkt.len() >= icmp_start + 8 && pkt[icmp_start] == 8 && pkt[icmp_start + 1] == 0 {
                    // ICMP type=8 (echo request), code=0.
                    handle_icmp_echo(pkt, ip_hdr_len, icmp_start);
                }
            }

            // Parse Ethernet + IP + UDP headers.
            if pkt.len() < 42 {
                continue; // too short
            }
            // Ethernet: 14 bytes, IP: 20 bytes (min), UDP: 8 bytes.
            // Check IP protocol = UDP (17).
            if pkt[23] != 17 {
                continue; // not UDP
            }
            let udp_start = 14 + 20; // eth + ip
            let _dst_port = ((pkt[udp_start + 2] as u16) << 8) | pkt[udp_start + 3] as u16;
            let udp_len = ((pkt[udp_start + 4] as u16) << 8) | pkt[udp_start + 5] as u16;
            if udp_len < 8 {
                continue;
            }
            let payload = &pkt[udp_start + 8..];
            if payload.is_empty() {
                continue;
            }
            // Echo back: send the same payload to the source.
            // Build a simple reply packet.
            let mut reply = [0u8; 64];
            let reply_len = core::cmp::min(payload.len(), 42);
            reply[..reply_len].copy_from_slice(&payload[..reply_len]);
            // Send via net (the driver will add headers).
            let _ = send(&reply[..reply_len]);
            crate::sinfo!("udpecho: echoed {} bytes", reply_len);
        }
        // Check for TCP retransmission timeouts.
        crate::tcp::check_retransmit();
        crate::interrupts::sleep_ms(10);
    }
}
