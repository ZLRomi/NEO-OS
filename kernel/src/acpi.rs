//! NEO-OS-3 ACPI table parser.
//!
//! Searches for the RSDP (Root System Description Pointer) in standard
//! memory regions, then parses the FADT (Fixed ACPI Description Table)
//! for system configuration information.
//!
//! This is a read-only parser — it does not enable ACPI or modify tables.

use crate::kinfo;

/// RSDP signature: "RSD PTR " (8 bytes).
const RSDP_SIGNATURE: &[u8; 8] = b"RSD PTR ";

/// RSDP v1 structure (20 bytes).
#[repr(C, packed)]
struct RsdpV1 {
    signature: [u8; 8],
    checksum: u8,
    oem_id: [u8; 6],
    revision: u8,
    rsdt_address: u32,
}

/// RSDP v2 structure (36 bytes, extends v1).
#[repr(C, packed)]
struct RsdpV2 {
    v1: RsdpV1,
    length: u32,
    xsdt_address: u64,
    extended_checksum: u8,
    reserved: [u8; 3],
}

/// Standard ACPI table header (36 bytes).
#[repr(C, packed)]
struct AcpiHeader {
    signature: [u8; 4],
    length: u32,
    revision: u8,
    checksum: u8,
    oem_id: [u8; 6],
    oem_table_id: [u8; 8],
    oem_revision: u32,
    creator_id: u32,
    creator_revision: u32,
}

/// Parsed ACPI information.
pub struct AcpiInfo {
    pub rsdp_found: bool,
    pub acpi_version: u8,     // 1 or 2
    pub oem_id: [u8; 6],
    pub rsdt_address: u32,
    pub xsdt_address: u64,
    pub fadt_found: bool,
    pub pm_timer_port: u32,
    pub sci_interrupt: u16,
}

static mut ACPI_INFO: AcpiInfo = AcpiInfo {
    rsdp_found: false,
    acpi_version: 0,
    oem_id: [0; 6],
    rsdt_address: 0,
    xsdt_address: 0,
    fadt_found: false,
    pm_timer_port: 0,
    sci_interrupt: 0,
};

/// Validate an ACPI table checksum (sum of all bytes must be 0 mod 256).
fn validate_checksum(ptr: *const u8, len: usize) -> bool {
    let mut sum: u8 = 0;
    for i in 0..len {
        sum = sum.wrapping_add(unsafe { *ptr.add(i) });
    }
    sum == 0
}

/// Search a memory region for the RSDP signature (must be on 16-byte boundary).
fn search_rsdp(start: u64, end: u64) -> Option<u64> {
    let mut addr = start;
    while addr < end {
        unsafe {
            let sig = core::slice::from_raw_parts(addr as *const u8, 8);
            if sig == RSDP_SIGNATURE {
                return Some(addr);
            }
        }
        addr += 16; // RSDP must be on 16-byte boundary
    }
    None
}

/// Read and parse the RSDP from a given address.
fn parse_rsdp(addr: u64) {
    unsafe {
        let rsdp = &*(addr as *const RsdpV2);

        // Validate v1 checksum (first 20 bytes).
        if !validate_checksum(addr as *const u8, 20) {
            kinfo!("acpi: RSDP v1 checksum invalid");
            return;
        }

        ACPI_INFO.rsdp_found = true;
        ACPI_INFO.acpi_version = rsdp.v1.revision + 1; // 0=v1, 1+=v2+
        ACPI_INFO.oem_id = rsdp.v1.oem_id;
        ACPI_INFO.rsdt_address = rsdp.v1.rsdt_address;

        if rsdp.v1.revision >= 2 {
            // Validate extended checksum (full structure).
            let full_len = rsdp.length as usize;
            if full_len >= 36 && validate_checksum(addr as *const u8, full_len) {
                ACPI_INFO.xsdt_address = rsdp.xsdt_address;
            }
        }

        kinfo!("acpi: RSDP found at {:#x}, version {}, OEM={}",
               addr,
               ACPI_INFO.acpi_version,
               core::str::from_utf8(&ACPI_INFO.oem_id).unwrap_or("???"));
    }
}

/// Parse the FADT from the RSDT to find PM timer port and SCI interrupt.
fn parse_fadt_from_rsdt() {
    unsafe {
        if !ACPI_INFO.rsdp_found || ACPI_INFO.rsdt_address == 0 {
            return;
        }

        let rsdt_hdr = &*(ACPI_INFO.rsdt_address as *const AcpiHeader);
        if rsdt_hdr.signature != *b"RSDT" {
            kinfo!("acpi: RSDT signature mismatch");
            return;
        }

        let rsdt_len = rsdt_hdr.length as usize;
        let num_entries = (rsdt_len - 36) / 4;
        let entries_ptr = (ACPI_INFO.rsdt_address + 36) as *const u32;

        for i in 0..num_entries {
            let table_addr = *entries_ptr.add(i) as u64;
            if table_addr == 0 { continue; }

            let hdr = &*(table_addr as *const AcpiHeader);
            if hdr.signature == *b"FACP" {
                // Found FADT. Parse relevant fields.
                // FADT layout varies by revision; we read common fields.
                let fadt = table_addr as *const u8;
                // SCI interrupt at offset 46 (u16 LE).
                ACPI_INFO.sci_interrupt = u16::from_le_bytes([*fadt.add(46), *fadt.add(47)]);
                // PM timer port at offset 76 (u32 LE) for FADT v2+.
                if rsdt_hdr.length >= 80 {
                    ACPI_INFO.pm_timer_port = u32::from_le_bytes([
                        *fadt.add(76), *fadt.add(77), *fadt.add(78), *fadt.add(79),
                    ]);
                }
                ACPI_INFO.fadt_found = true;
                kinfo!("acpi: FADT found, SCI={}, PM_TMR={:#x}",
                       ACPI_INFO.sci_interrupt, ACPI_INFO.pm_timer_port);
                return;
            }
        }
        kinfo!("acpi: FADT not found in RSDT");
    }
}

/// Initialize ACPI: find RSDP and parse FADT.
pub fn init() {
    // Search EBDA region (first 1KB of EBDA, typically at 0x80000).
    // Also search 0xE0000-0xFFFFF (BIOS ROM area).
    if let Some(addr) = search_rsdp(0x80000, 0xA0000) {
        parse_rsdp(addr);
    } else if let Some(addr) = search_rsdp(0xE0000, 0x100000) {
        parse_rsdp(addr);
    } else {
        kinfo!("acpi: RSDP not found");
        return;
    }

    parse_fadt_from_rsdt();
}

/// Get parsed ACPI info.
pub fn info() -> &'static AcpiInfo {
    unsafe { &ACPI_INFO }
}
