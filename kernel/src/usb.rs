//! NEO-OS-3 USB controller detection framework.
//!
//! Scans PCI bus for USB host controllers (UHCI, OHCI, EHCI, xHCI)
//! and reports their type and base addresses. This is a detection-only
//! framework; actual USB device enumeration requires further implementation.
//!
//! USB controller PCI class codes:
//!   Class 0x0C (Serial Bus), Subclass 0x03 (USB)
//!   ProgIF: 0x00=UHCI, 0x10=OHCI, 0x20=EHCI, 0x30=xHCI, 0x80=Unspecified

use crate::kinfo;

/// USB controller types.
#[derive(Clone, Copy)]
pub enum UsbControllerType {
    Uhci,   // USB 1.x (Intel)
    Ohci,   // USB 1.x (non-Intel)
    Ehci,   // USB 2.0
    Xhci,   // USB 3.0
    Unknown(u8),
}

impl UsbControllerType {
    fn from_prog_if(prog_if: u8) -> Self {
        match prog_if {
            0x00 => UsbControllerType::Uhci,
            0x10 => UsbControllerType::Ohci,
            0x20 => UsbControllerType::Ehci,
            0x30 => UsbControllerType::Xhci,
            other => UsbControllerType::Unknown(other),
        }
    }

    fn name(&self) -> &'static str {
        match self {
            UsbControllerType::Uhci => "UHCI (USB 1.x)",
            UsbControllerType::Ohci => "OHCI (USB 1.x)",
            UsbControllerType::Ehci => "EHCI (USB 2.0)",
            UsbControllerType::Xhci => "xHCI (USB 3.0)",
            UsbControllerType::Unknown(_) => "Unknown",
        }
    }
}

/// Detected USB controller info.
pub struct UsbController {
    pub pci_bus: u8,
    pub pci_dev: u8,
    pub pci_func: u8,
    pub vendor_id: u16,
    pub device_id: u16,
    pub ctrl_type: UsbControllerType,
    pub bar0: u32,
    pub irq_line: u8,
}

const MAX_USB_CONTROLLERS: usize = 4;
static mut USB_CONTROLLERS: [Option<UsbController>; MAX_USB_CONTROLLERS] = {
    const NONE: Option<UsbController> = None;
    [NONE; MAX_USB_CONTROLLERS]
};
static mut NUSB: usize = 0;

/// Scan PCI bus for USB controllers.
pub fn init() {
    let n = crate::pci::count();
    let mut found = 0usize;

    for i in 0..n {
        if let Some(dev) = crate::pci::get(i) {
            // USB: class=0x0C, subclass=0x03
            if dev.class_code == 0x0C && dev.subclass == 0x03 {
                let ctrl_type = UsbControllerType::from_prog_if(dev.prog_if);
                kinfo!("usb: found {} at {:02x}:{:02x}.{} [{:04x}:{:04x}] IRQ={}",
                       ctrl_type.name(), dev.bus, dev.device, dev.function,
                       dev.vendor_id, dev.device_id, dev.irq_line);

                if found < MAX_USB_CONTROLLERS {
                    unsafe {
                        USB_CONTROLLERS[found] = Some(UsbController {
                            pci_bus: dev.bus,
                            pci_dev: dev.device,
                            pci_func: dev.function,
                            vendor_id: dev.vendor_id,
                            device_id: dev.device_id,
                            ctrl_type,
                            bar0: dev.bar0,
                            irq_line: dev.irq_line,
                        });
                    }
                    found += 1;
                }
            }
        }
    }

    unsafe { NUSB = found; }

    if found == 0 {
        kinfo!("usb: no USB controllers found");
    } else {
        kinfo!("usb: {} controller(s) detected", found);
    }
}

/// Get number of detected USB controllers.
pub fn count() -> usize {
    unsafe { NUSB }
}

/// Dump USB controller info.
pub fn dump() {
    let n = count();
    if n == 0 {
        kinfo!("usb: no controllers\n");
        return;
    }
    for i in 0..n {
        unsafe {
            if let Some(ref ctrl) = USB_CONTROLLERS[i] {
                kinfo!("usb[{}]: {} @ {:02x}:{:02x}.{} BAR0={:#x} IRQ={}\n",
                       i, ctrl.ctrl_type.name(),
                       ctrl.pci_bus, ctrl.pci_dev, ctrl.pci_func,
                       ctrl.bar0, ctrl.irq_line);
            }
        }
    }
}
