//! NEO-OS-3 AHCI (SATA) controller driver framework.
//!
//! Detects AHCI controllers via PCI, maps HBA memory registers,
//! and enumerates SATA ports. This is a detection/enumeration framework;
//! full read/write requires command list setup and FIS handling.
//!
//! AHCI spec: Serial ATA Advanced Host Controller Interface v1.3.1

use crate::kinfo;

/// AHCI PCI class code: Mass Storage (01), SATA (06), AHCI (01).
const AHCI_CLASS: u8 = 0x01;
const AHCI_SUBCLASS: u8 = 0x06;
const AHCI_PROGIF: u8 = 0x01;

/// HBA Memory Register offsets (from ABAR base).
const HBA_CAP: usize = 0x00;       // Host Capabilities (u32)
const HBA_GHC: usize = 0x04;       // Global Host Control (u32)
const HBA_IS: usize = 0x08;        // Interrupt Status (u32)
const HBA_PI: usize = 0x0C;        // Ports Implemented (u32)
const HBA_VS: usize = 0x10;        // AHCI Version (u32)

/// Port Register offsets (per port, stride 0x80).
const PORT_CLB: usize = 0x00;      // Command List Base (u64)
const PORT_FB: usize = 0x08;       // FIS Base (u64)
const PORT_IS: usize = 0x10;       // Interrupt Status (u32)
const PORT_IE: usize = 0x14;       // Interrupt Enable (u32)
const PORT_CMD: usize = 0x18;      // Command and Status (u32)
const PORT_SIG: usize = 0x24;      // Signature (u32)
const PORT_SSTS: usize = 0x28;     // SATA Status (u32)
const PORT_SERR: usize = 0x30;     // SATA Error (u32)
const PORT_SACT: usize = 0x34;     // SATA Active (u32)
const PORT_CI: usize = 0x38;       // Command Issue (u32)

/// Port signature values.
const SIG_ATA: u32 = 0x00000101;   // SATA drive
const SIG_ATAPI: u32 = 0xEB140101; // CD/DVD
const SIG_SEMB: u32 = 0xC33C0101;  // Enclosure management
const SIG_PM: u32 = 0x96690101;    // Port multiplier

/// AHCI controller info.
pub struct AhciController {
    pub pci_bus: u8,
    pub pci_dev: u8,
    pub pci_func: u8,
    pub abar: u64,           // AHCI Base Address Register (MMIO)
    pub num_ports: u8,       // Number of ports from CAP.NP
    pub version: u32,        // AHCI version
    pub ports_implemented: u32, // PI register bitmask
}

static mut AHCI_CTRL: Option<AhciController> = None;

/// Read a 32-bit MMIO register.
fn mmio_read32(addr: u64) -> u32 {
    unsafe { core::ptr::read_volatile(addr as *const u32) }
}

/// Find an AHCI controller on the PCI bus.
fn find_ahci_pci() -> Option<(u8, u8, u8, u32)> {
    let n = crate::pci::count();
    for i in 0..n {
        if let Some(dev) = crate::pci::get(i) {
            if dev.class_code == AHCI_CLASS
                && dev.subclass == AHCI_SUBCLASS
                && dev.prog_if == AHCI_PROGIF
            {
                // BAR5 is the AHCI Base Address Register (ABAR).
                let bar5 = dev.bar5;
                if bar5 & 1 == 0 {
                    // Memory-mapped BAR.
                    let abar = (bar5 & 0xFFFFFFF0) as u64;
                    return Some((dev.bus, dev.device, dev.function, abar as u32));
                }
            }
        }
    }
    None
}

/// Initialize AHCI: detect controller, map registers, enumerate ports.
pub fn init() {
    match find_ahci_pci() {
        Some((bus, dev, func, abar32)) => {
            let abar = abar32 as u64;
            kinfo!("ahci: found controller at {:02x}:{:02x}.{}, ABAR={:#x}",
                   bus, dev, func, abar);

            // Read capabilities.
            let cap = mmio_read32(abar + HBA_CAP as u64);
            let num_ports = ((cap & 0x1F) + 1) as u8; // NP field: bits 0-4
            let version = mmio_read32(abar + HBA_VS as u64);
            let pi = mmio_read32(abar + HBA_PI as u64);

            kinfo!("ahci: v{}.{}, {} ports, PI={:#x}",
                   version >> 16, (version >> 8) & 0xFF, num_ports, pi);

            // Enumerate implemented ports.
            let mut active_ports = 0u32;
            for p in 0..num_ports {
                if pi & (1 << p) == 0 { continue; }
                let port_base = abar + 0x100 + (p as u64) * 0x80;
                let ssts = mmio_read32(port_base + PORT_SSTS as u64);
                let sig = mmio_read32(port_base + PORT_SIG as u64);
                let det = ssts & 0xF; // DET field: 0=no device, 1=present, 3=active

                if det == 3 {
                    let dev_type = match sig {
                        SIG_ATA => "SATA",
                        SIG_ATAPI => "ATAPI",
                        SIG_SEMB => "SEMB",
                        SIG_PM => "PM",
                        _ => "unknown",
                    };
                    kinfo!("ahci: port {}: {} device (sig={:#x})", p, dev_type, sig);
                    active_ports += 1;
                } else if det != 0 {
                    kinfo!("ahci: port {}: present but not active (DET={})", p, det);
                }
            }

            unsafe {
                AHCI_CTRL = Some(AhciController {
                    pci_bus: bus,
                    pci_dev: dev,
                    pci_func: func,
                    abar,
                    num_ports,
                    version,
                    ports_implemented: pi,
                });
            }

            kinfo!("ahci: {} active port(s)", active_ports);
        }
        None => {
            kinfo!("ahci: no AHCI controller found");
        }
    }
}

/// Check if AHCI controller was detected.
pub fn available() -> bool {
    unsafe { AHCI_CTRL.is_some() }
}

/// Dump AHCI controller info.
pub fn dump() {
    unsafe {
        match &AHCI_CTRL {
            Some(ctrl) => {
                kinfo!("ahci: ABAR={:#x} v{}.{} ports={} PI={:#x}",
                       ctrl.abar, ctrl.version >> 16, (ctrl.version >> 8) & 0xFF,
                       ctrl.num_ports, ctrl.ports_implemented);
            }
            None => kinfo!("ahci: not available"),
        }
    }
}
