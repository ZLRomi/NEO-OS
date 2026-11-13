//! NEO-OS-3 driver framework.
//!
//! Provides a simple registry for hardware drivers. Each driver has a name,
//! type, and status. The framework is intentionally minimal — it's a registry
//! and lifecycle manager, not a full bus/device model.

use core::fmt;

/// Driver types.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum DriverType {
    Keyboard,
    Mouse,
    Serial,
    Display,
    Block,
    Other,
}

impl fmt::Display for DriverType {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            DriverType::Keyboard => write!(f, "kbd"),
            DriverType::Mouse => write!(f, "mouse"),
            DriverType::Serial => write!(f, "serial"),
            DriverType::Display => write!(f, "display"),
            DriverType::Block => write!(f, "block"),
            DriverType::Other => write!(f, "other"),
        }
    }
}

/// Driver status.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum DriverStatus {
    Uninitialized,
    Ready,
    Error,
}

impl fmt::Display for DriverStatus {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            DriverStatus::Uninitialized => write!(f, "uninit"),
            DriverStatus::Ready => write!(f, "ready"),
            DriverStatus::Error => write!(f, "error"),
        }
    }
}

/// Maximum number of registered drivers.
pub const MAX_DRIVERS: usize = 16;

/// One registered driver entry.
pub struct DriverEntry {
    pub name: &'static str,
    pub dtype: DriverType,
    pub status: DriverStatus,
}

/// Global driver registry.
static mut DRIVERS: [Option<DriverEntry>; MAX_DRIVERS] = {
    // const initializer for Option<DriverEntry>
    const NONE: Option<DriverEntry> = None;
    [NONE; MAX_DRIVERS]
};
static mut NDRIVERS: usize = 0;

/// Register a new driver. Returns the driver index or None if full.
pub fn register(name: &'static str, dtype: DriverType) -> Option<usize> {
    unsafe {
        if NDRIVERS >= MAX_DRIVERS {
            return None;
        }
        let idx = NDRIVERS;
        DRIVERS[idx] = Some(DriverEntry {
            name,
            dtype,
            status: DriverStatus::Uninitialized,
        });
        NDRIVERS += 1;
        Some(idx)
    }
}

/// Mark a driver as ready.
pub fn set_ready(idx: usize) {
    unsafe {
        if idx < NDRIVERS {
            if let Some(ref mut d) = DRIVERS[idx] {
                d.status = DriverStatus::Ready;
            }
        }
    }
}

/// Mark a driver as error.
pub fn set_error(idx: usize) {
    unsafe {
        if idx < NDRIVERS {
            if let Some(ref mut d) = DRIVERS[idx] {
                d.status = DriverStatus::Error;
            }
        }
    }
}

/// Get the number of registered drivers.
pub fn count() -> usize {
    unsafe { NDRIVERS }
}

/// Get a driver entry by index.
pub fn get(idx: usize) -> Option<&'static DriverEntry> {
    unsafe {
        if idx < NDRIVERS {
            DRIVERS[idx].as_ref()
        } else {
            None
        }
    }
}

/// Dump all registered drivers to the console.
pub fn dump() {
    let n = count();
    crate::kprint!("drivers ({} registered):\n", n);
    for i in 0..n {
        if let Some(d) = get(i) {
            crate::kprint!("  [{:2}] {:8} {:7} {}\n", i, d.name, d.dtype, d.status);
        }
    }
}
