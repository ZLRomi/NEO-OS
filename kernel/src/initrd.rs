//! Simple initrd archive parser and in-memory editor.
//!
//! Format: [u32 num_entries][entry: u32 name_len, u32 data_len, name..., data...]...
//! All integers are little-endian. Names are NOT null-terminated.
//!
//! The kernel can parse the archive and modify it in-place (in-memory).

use neo_common::InitrdEntry;
use crate::kprint;

/// Maximum entries we can hold in memory.
pub const MAX_ENTRIES: usize = 32;

/// Global in-memory initrd state.
static mut ENTRIES: [InitrdEntry; MAX_ENTRIES] = {
    const EMPTY: InitrdEntry = InitrdEntry {
        name_offset: 0,
        name_len: 0,
        data_offset: 0,
        data_len: 0,
    };
    [EMPTY; MAX_ENTRIES]
};
static mut NENTRIES: usize = 0;
static mut INITRD_DATA: *mut u8 = core::ptr::null_mut();
static mut INITRD_LEN: usize = 0;

/// Parse the initrd archive from a memory buffer.
/// Returns the number of entries parsed.
pub fn parse_initrd(data: &[u8], entries: &mut [InitrdEntry], max_entries: usize) -> usize {
    if data.len() < 4 {
        return 0;
    }
    let num_entries = u32::from_le_bytes([data[0], data[1], data[2], data[3]]) as usize;
    let mut off = 4usize;
    let mut count = 0;

    for _ in 0..num_entries {
        if count >= max_entries { break; }
        if off + 8 > data.len() { break; }
        let name_len = u32::from_le_bytes([data[off], data[off+1], data[off+2], data[off+3]]) as usize;
        let data_len = u32::from_le_bytes([data[off+4], data[off+5], data[off+6], data[off+7]]) as usize;
        off += 8;
        if off + name_len + data_len > data.len() { break; }
        entries[count] = InitrdEntry {
            name_offset: off,
            name_len,
            data_offset: off + name_len,
            data_len,
        };
        off += name_len + data_len;
        count += 1;
    }
    count
}

/// Initialize the in-memory initrd from BootInfo.
pub fn init(base: u64, len: u64) {
    if base == 0 || len == 0 {
        return;
    }
    unsafe {
        INITRD_DATA = base as *mut u8;
        INITRD_LEN = len as usize;
        let data = core::slice::from_raw_parts(INITRD_DATA, INITRD_LEN);
        NENTRIES = parse_initrd(data, &mut ENTRIES, MAX_ENTRIES);
    }
    kprint!("initrd: {} entries loaded\n", unsafe { NENTRIES });
}

/// Get the number of entries.
pub fn count() -> usize {
    unsafe { NENTRIES }
}

/// Get an entry by index.
pub fn get(idx: usize) -> Option<&'static InitrdEntry> {
    unsafe {
        if idx < NENTRIES {
            Some(&ENTRIES[idx])
        } else {
            None
        }
    }
}

/// Find an entry by name. Returns index or None.
pub fn find(name: &[u8]) -> Option<usize> {
    unsafe {
        let data = core::slice::from_raw_parts(INITRD_DATA, INITRD_LEN);
        for i in 0..NENTRIES {
            let e = &ENTRIES[i];
            let ename = &data[e.name_offset..e.name_offset + e.name_len];
            if ename == name {
                return Some(i);
            }
        }
    }
    None
}

/// Read an entry's data by index.
pub fn read(idx: usize) -> Option<&'static [u8]> {
    unsafe {
        if idx >= NENTRIES {
            return None;
        }
        let e = &ENTRIES[idx];
        let data = core::slice::from_raw_parts(INITRD_DATA, INITRD_LEN);
        Some(&data[e.data_offset..e.data_offset + e.data_len])
    }
}

/// Write data to an existing entry (in-memory only).
/// Returns true on success. Data must fit in the existing entry's space.
pub fn write(name: &[u8], new_data: &[u8]) -> bool {
    let idx = match find(name) {
        Some(i) => i,
        None => return false,
    };
    unsafe {
        let e = &mut ENTRIES[idx];
        if new_data.len() > e.data_len {
            return false; // too large for existing slot
        }
        let data = core::slice::from_raw_parts_mut(INITRD_DATA, INITRD_LEN);
        let dst = &mut data[e.data_offset..e.data_offset + e.data_len];
        dst[..new_data.len()].copy_from_slice(new_data);
        // Zero-fill remainder.
        for b in dst[new_data.len()..].iter_mut() {
            *b = 0;
        }
        e.data_len = new_data.len();
    }
    true
}

/// Create a new entry in the initrd (appends to the archive).
/// This requires rebuilding the archive — for simplicity, we don't support
/// this yet. Returns false always.
pub fn create(_name: &[u8], _data: &[u8]) -> bool {
    // TODO: implement archive rebuilding with new entry.
    false
}
