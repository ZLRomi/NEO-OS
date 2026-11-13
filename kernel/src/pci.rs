//! NEO-OS-3 PCI bus enumeration.
//!
//! Uses port I/O (CF8/CFD) to scan the PCI bus and find devices.
//! This is the legacy PCI configuration mechanism — works on all x86.

use crate::kinfo;

/// PCI configuration port addresses.
const PCI_CONFIG_ADDR: u16 = 0xCF8;
const PCI_CONFIG_DATA: u16 = 0xCFC;

/// PCI vendor/device IDs for known devices.
const VIRTIO_VENDOR: u16 = 0x1AF4;
const VIRTIO_NET_DEVICE: u16 = 0x1000; // legacy virtio-net
const VIRTIO_NET_MODERN: u16 = 0x1041; // modern virtio-net (transitional)
const VIRTIO_BLK_DEVICE: u16 = 0x1001; // legacy virtio-blk
const VIRTIO_BLK_MODERN: u16 = 0x1042; // modern virtio-blk (transitional)

/// A found PCI device.
#[derive(Clone, Copy)]
pub struct PciDevice {
    pub bus: u8,
    pub device: u8,
    pub function: u8,
    pub vendor_id: u16,
    pub device_id: u16,
    pub class_code: u8,
    pub subclass: u8,
    pub prog_if: u8,
    pub bar0: u32,
    pub bar1: u32,
    pub bar2: u32,
    pub bar3: u32,
    pub bar4: u32,
    pub bar5: u32,
    pub irq_line: u8,
}

/// Maximum number of PCI devices to track.
const MAX_PCI_DEVICES: usize = 32;
static mut PCI_DEVICES: [Option<PciDevice>; MAX_PCI_DEVICES] = {
    const NONE: Option<PciDevice> = None;
    [NONE; MAX_PCI_DEVICES]
};
static mut NPCI_DEVICES: usize = 0;

/// Read a 32-bit value from PCI config space.
fn pci_read32(bus: u8, device: u8, func: u8, offset: u8) -> u32 {
    let addr: u32 = 0x80000000
        | ((bus as u32) << 16)
        | ((device as u32) << 11)
        | ((func as u32) << 8)
        | ((offset as u32) & 0xFC);
    unsafe {
        core::arch::asm!(
            "out dx, eax",
            in("dx") PCI_CONFIG_ADDR,
            in("eax") addr,
            options(nostack),
        );
        let mut val: u32;
        core::arch::asm!(
            "in eax, dx",
            in("dx") PCI_CONFIG_DATA,
            out("eax") val,
            options(nostack),
        );
        val
    }
}

/// Write a 32-bit value to PCI config space.
fn pci_write32(bus: u8, device: u8, func: u8, offset: u8, val: u32) {
    let addr: u32 = 0x80000000
        | ((bus as u32) << 16)
        | ((device as u32) << 11)
        | ((func as u32) << 8)
        | ((offset as u32) & 0xFC);
    unsafe {
        core::arch::asm!(
            "out dx, eax",
            in("dx") PCI_CONFIG_ADDR,
            in("eax") addr,
            options(nostack),
        );
        core::arch::asm!(
            "out dx, eax",
            in("dx") PCI_CONFIG_DATA,
            in("eax") val,
            options(nostack),
        );
    }
}

/// Scan the PCI bus for devices. Returns number of devices found.
pub fn scan() -> usize {
    let mut count = 0;
    for bus in 0..256u16 {
        for device in 0..32u8 {
            for func in 0..8u8 {
                let vendor = pci_read32(bus as u8, device, func, 0x00) as u16;
                if vendor == 0xFFFF || vendor == 0x0000 {
                    continue; // no device
                }
                let device_id = (pci_read32(bus as u8, device, func, 0x00) >> 16) as u16;
                let class_sub = pci_read32(bus as u8, device, func, 0x08);
                let class_code = (class_sub >> 24) as u8;
                let subclass = ((class_sub >> 16) & 0xFF) as u8;
                let prog_if = ((class_sub >> 8) & 0xFF) as u8;

                // Read BARs.
                let bar0 = pci_read32(bus as u8, device, func, 0x10);
                let bar1 = pci_read32(bus as u8, device, func, 0x14);
                let bar2 = pci_read32(bus as u8, device, func, 0x18);
                let bar3 = pci_read32(bus as u8, device, func, 0x1C);
                let bar4 = pci_read32(bus as u8, device, func, 0x20);
                let bar5 = pci_read32(bus as u8, device, func, 0x24);

                // Read interrupt line.
                let irq_line = pci_read32(bus as u8, device, func, 0x3C) as u8;

                if count < MAX_PCI_DEVICES {
                    unsafe {
                        PCI_DEVICES[count] = Some(PciDevice {
                            bus: bus as u8,
                            device,
                            function: func,
                            vendor_id: vendor,
                            device_id,
                            class_code,
                            subclass,
                            prog_if,
                            bar0, bar1, bar2, bar3, bar4, bar5,
                            irq_line,
                        });
                    }
                    count += 1;
                }
            }
        }
    }
    unsafe { NPCI_DEVICES = count; }
    count
}

/// Get the number of PCI devices found.
pub fn count() -> usize {
    unsafe { NPCI_DEVICES }
}

/// Get a PCI device by index.
pub fn get(idx: usize) -> Option<&'static PciDevice> {
    unsafe {
        if idx < NPCI_DEVICES {
            PCI_DEVICES[idx].as_ref()
        } else {
            None
        }
    }
}

/// Find a device by vendor and device ID.
pub fn find(vendor: u16, device: u16) -> Option<&'static PciDevice> {
    for i in 0..count() {
        if let Some(d) = get(i) {
            if d.vendor_id == vendor && d.device_id == device {
                return Some(d);
            }
        }
    }
    None
}

/// Dump all PCI devices to the console.
pub fn dump() {
    let n = count();
    kinfo!("pci: {} devices found", n);
    for i in 0..n {
        if let Some(d) = get(i) {
            kinfo!(
                "  [{:02x}:{:02x}.{}] {:04x}:{:04x} class={:02x}:{:02x}:{:02x} irq={}",
                d.bus, d.device, d.function,
                d.vendor_id, d.device_id,
                d.class_code, d.subclass, d.prog_if,
                d.irq_line
            );
        }
    }
}

/// Find a virtio-net device (legacy or modern).
pub fn find_virtio_net() -> Option<&'static PciDevice> {
    find(VIRTIO_VENDOR, VIRTIO_NET_DEVICE)
        .or_else(|| find(VIRTIO_VENDOR, VIRTIO_NET_MODERN))
}

/// Find a virtio-blk device (legacy or modern).
pub fn find_virtio_blk() -> Option<&'static PciDevice> {
    find(VIRTIO_VENDOR, VIRTIO_BLK_DEVICE)
        .or_else(|| find(VIRTIO_VENDOR, VIRTIO_BLK_MODERN))
}
