//! NEO-OS-3 Virtual File System (VFS) layer.
//!
//! Provides a unified file interface across different filesystems.
//! Manages file descriptors per-task and dispatches operations to
//! the appropriate filesystem driver.
//!
//! Architecture:
//!   User program → syscall → VFS → FileSystem → BlockDevice
//!
//! Currently supports one mounted filesystem (NEO-FS on block device 0).

use crate::kinfo;

/// Maximum open files system-wide.
const MAX_OPEN_FILES: usize = 32;
/// Maximum filename length.
pub const MAX_NAME_LEN: usize = 16;

/// File types.
#[derive(Clone, Copy, PartialEq)]
pub enum FileType {
    Regular,
    Directory,
}

/// File stat information.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Stat {
    pub ftype: u32,     // 1=file, 2=dir
    pub size: u32,
    pub inode: u32,
    pub _pad: u32,
}

/// Open file descriptor entry.
struct OpenFile {
    /// Inode number on the filesystem (or proc type ID for virtual files).
    ino: usize,
    /// Current read/write position (bytes).
    pos: u32,
    /// Owner task ID (for per-task fd mapping).
    task_id: u8,
    /// File descriptor number within the task.
    fd: u8,
    /// Whether this slot is in use.
    active: bool,
    /// Whether this is a /proc virtual file.
    is_proc: bool,
}

static mut OPEN_FILES: [OpenFile; MAX_OPEN_FILES] = {
    const EMPTY: OpenFile = OpenFile {
        ino: 0, pos: 0, task_id: 0, fd: 0, active: false, is_proc: false,
    };
    [EMPTY; MAX_OPEN_FILES]
};

// ---- Public VFS API (called by syscall handler) ----

/// Open a file by name.
///
/// Supports `"dir/file"` path syntax. Returns fd >= 0 on success, or negative error code:
/// - `-1`: invalid name or path resolution failure
/// - `-2`: file not found (read-only open)
/// - `-3`: cannot create file (no space/inodes)
/// - `-4`: too many open files
///
/// # Flags
/// - `0` — read-only
/// - `1` — write (create/truncate)
/// - `2` — read-write
/// - `3` — append (create if needed, position at end)
pub fn vfs_open(name: &[u8], flags: u32, task_id: u8) -> i32 {
    if name.is_empty() || name.len() > 32 {
        return -1;
    }

    // Check for /proc/ virtual files.
    if name.starts_with(b"/proc/") {
        if flags != 0 { return -1; } // proc is read-only
        let proc_path = &name[6..]; // skip "/proc/"
        return proc_open(proc_path, task_id);
    }

    // Resolve path to get parent directory and filename.
    let (parent_ino, filename) = match crate::neofs::resolve_path(name) {
        Some(p) => p,
        None => return -1,
    };
    if filename.is_empty() || filename.len() > MAX_NAME_LEN {
        return -1;
    }

    // Find existing file or create new one.
    let ino = match crate::neofs::find_inode(filename) {
        Some(i) => {
            if flags == 1 {
                let _ = crate::neofs::truncate_inode(i);
            }
            i
        }
        None => {
            if flags == 0 {
                return -2;
            }
            match crate::neofs::create_file_in_dir(filename, parent_ino) {
                Some(i) => i,
                None => return -3,
            }
        }
    };

    // For append mode, determine initial position (end of file).
    let initial_pos = if flags == 3 {
        match crate::neofs::stat_inode(ino) {
            Some(s) => s.size,
            None => 0,
        }
    } else {
        0
    };

    // Allocate an open file slot.
    unsafe {
        for i in 0..MAX_OPEN_FILES {
            if !OPEN_FILES[i].active {
                let mut fd: u8 = 3;
                for j in 0..MAX_OPEN_FILES {
                    if OPEN_FILES[j].active && OPEN_FILES[j].task_id == task_id && OPEN_FILES[j].fd == fd {
                        fd += 1;
                    }
                }
                OPEN_FILES[i] = OpenFile {
                    ino,
                    pos: initial_pos,
                    task_id,
                    fd,
                    active: true,
                    is_proc: false,
                };
                return fd as i32;
            }
        }
    }
    -4 // too many open files
}

/// Close a file descriptor.
pub fn vfs_close(fd: u32, task_id: u8) -> i32 {
    unsafe {
        for i in 0..MAX_OPEN_FILES {
            if OPEN_FILES[i].active && OPEN_FILES[i].task_id == task_id && OPEN_FILES[i].fd == fd as u8 {
                OPEN_FILES[i].active = false;
                return 0;
            }
        }
    }
    -1 // bad fd
}

/// Read from a file descriptor into buf. Returns bytes read or negative error.
pub fn vfs_read(fd: u32, buf: &mut [u8], task_id: u8) -> i32 {
    let (ino, pos, is_proc) = unsafe {
        let mut found = None;
        for i in 0..MAX_OPEN_FILES {
            if OPEN_FILES[i].active && OPEN_FILES[i].task_id == task_id && OPEN_FILES[i].fd == fd as u8 {
                found = Some((OPEN_FILES[i].ino, OPEN_FILES[i].pos, OPEN_FILES[i].is_proc));
                break;
            }
        }
        match found {
            Some(f) => f,
            None => return -1,
        }
    };

    // Handle /proc virtual files.
    if is_proc {
        let n = proc_read(ino, task_id, pos, buf);
        if n > 0 {
            unsafe {
                for i in 0..MAX_OPEN_FILES {
                    if OPEN_FILES[i].active && OPEN_FILES[i].task_id == task_id && OPEN_FILES[i].fd == fd as u8 {
                        OPEN_FILES[i].pos += n as u32;
                        break;
                    }
                }
            }
        }
        return n as i32;
    }

    // Read file data starting at pos.
    let bytes_read = crate::neofs::read_file_data(ino, pos as usize, buf);
    match bytes_read {
        Ok(n) => {
            // Advance position.
            unsafe {
                for i in 0..MAX_OPEN_FILES {
                    if OPEN_FILES[i].active && OPEN_FILES[i].task_id == task_id && OPEN_FILES[i].fd == fd as u8 {
                        OPEN_FILES[i].pos += n as u32;
                        break;
                    }
                }
            }
            n as i32
        }
        Err(_) => -2,
    }
}

/// Write to a file descriptor from buf. Returns bytes written or negative error.
pub fn vfs_write(fd: u32, buf: &[u8], task_id: u8) -> i32 {
    let (ino, pos) = unsafe {
        let mut found = None;
        for i in 0..MAX_OPEN_FILES {
            if OPEN_FILES[i].active && OPEN_FILES[i].task_id == task_id && OPEN_FILES[i].fd == fd as u8 {
                found = Some((OPEN_FILES[i].ino, OPEN_FILES[i].pos));
                break;
            }
        }
        match found {
            Some(f) => f,
            None => return -1,
        }
    };

    let bytes_written = crate::neofs::write_file_data(ino, pos as usize, buf);
    match bytes_written {
        Ok(n) => {
            unsafe {
                for i in 0..MAX_OPEN_FILES {
                    if OPEN_FILES[i].active && OPEN_FILES[i].task_id == task_id && OPEN_FILES[i].fd == fd as u8 {
                        OPEN_FILES[i].pos += n as u32;
                        break;
                    }
                }
            }
            n as i32
        }
        Err(_) => -2,
    }
}

/// Stat a file by name. Returns 0 on success, negative on error.
pub fn vfs_stat(name: &[u8], stat: &mut Stat) -> i32 {
    match crate::neofs::find_inode(name) {
        Some(ino) => {
            match crate::neofs::stat_inode(ino) {
                Some(s) => {
                    *stat = s;
                    0
                }
                None => -2,
            }
        }
        None => -1,
    }
}

/// List directory entries. Calls callback for each entry.
/// Returns number of entries listed.
pub fn vfs_readdir(callback: &mut dyn FnMut(&[u8], u32, u32)) -> usize {
    crate::neofs::list_root(callback)
}

/// Serialize directory listing into a buffer for syscall return.
/// Format: [u32 count][entry: u8 type, u32 size, u8 name_len, name...]...
/// Returns bytes written.
pub fn vfs_readdir_serialize(buf: &mut [u8]) -> usize {
    if buf.len() < 4 { return 0; }
    let mut pos = 0usize;
    let mut count = 0u32;

    // Reserve space for count.
    pos += 4;

    crate::neofs::list_root(&mut |name, mode, size| {
        let name_len = name.len();
        let entry_size = 1 + 4 + 1 + name_len; // type + size + name_len + name
        if pos + entry_size > buf.len() { return; }

        buf[pos] = mode as u8;
        buf[pos + 1..pos + 5].copy_from_slice(&size.to_le_bytes());
        buf[pos + 5] = name_len as u8;
        buf[pos + 6..pos + 6 + name_len].copy_from_slice(name);
        pos += entry_size;
        count += 1;
    });

    buf[0..4].copy_from_slice(&count.to_le_bytes());
    pos
}

/// Initialize the VFS layer.
pub fn init() {
    kinfo!("vfs: initialized (max {} open files)", MAX_OPEN_FILES);
}

// ---- /proc virtual filesystem ----

/// Proc file type IDs (stored in OpenFile.ino for proc files).
const PROC_SELF_STATUS: usize = 1;
const PROC_UPTIME: usize = 2;
const PROC_MEMINFO: usize = 3;
const PROC_TASKS: usize = 4;
const PROC_SELF_CMDLINE: usize = 5;
const PROC_SELF_MAPS: usize = 6;

/// Open a /proc virtual file. Returns fd or negative error.
fn proc_open(path: &[u8], task_id: u8) -> i32 {
    let proc_type = match path {
        b"self/status" => PROC_SELF_STATUS,
        b"uptime" => PROC_UPTIME,
        b"meminfo" => PROC_MEMINFO,
        b"tasks" => PROC_TASKS,
        b"self/cmdline" => PROC_SELF_CMDLINE,
        b"self/maps" => PROC_SELF_MAPS,
        _ => return -2, // not found
    };

    unsafe {
        for i in 0..MAX_OPEN_FILES {
            if !OPEN_FILES[i].active {
                let mut fd: u8 = 3;
                for j in 0..MAX_OPEN_FILES {
                    if OPEN_FILES[j].active && OPEN_FILES[j].task_id == task_id && OPEN_FILES[j].fd == fd {
                        fd += 1;
                    }
                }
                OPEN_FILES[i] = OpenFile {
                    ino: proc_type,
                    pos: 0,
                    task_id,
                    fd,
                    active: true,
                    is_proc: true,
                };
                return fd as i32;
            }
        }
    }
    -4
}

/// Generate content for a /proc virtual file.
fn proc_generate(proc_type: usize, task_id: u8) -> [u8; 256] {
    let mut buf = [0u8; 256];
    let mut pos = 0usize;

    macro_rules! write_str {
        ($s:expr) => {
            for &b in ($s as &[u8]) {
                if pos < 255 { buf[pos] = b; pos += 1; }
            }
        };
    }

    macro_rules! write_u64 {
        ($v:expr) => {
            let mut digits = [0u8; 20];
            let mut d = 0;
            let mut v = $v;
            if v == 0 { digits[0] = b'0'; d = 1; }
            else { while v > 0 { digits[d] = (v % 10) as u8 + b'0'; d += 1; v /= 10; } }
            for i in (0..d).rev() { if pos < 255 { buf[pos] = digits[i]; pos += 1; } }
        };
    }

    match proc_type {
        PROC_SELF_STATUS => {
            write_str!(b"pid:\t");
            write_u64!(task_id as u64);
            write_str!(b"\nstate:\trunning\n");
        }
        PROC_UPTIME => {
            let ticks = crate::interrupts::ticks();
            write_u64!(ticks / 100);
            write_str!(b".");
            write_u64!((ticks % 100) / 10);
            write_str!(b" seconds\n");
        }
        PROC_MEMINFO => {
            let (_, total, used, free) = crate::mem::stats();
            write_str!(b"total:\t");
            write_u64!(total / 1024);
            write_str!(b" KiB\nused:\t");
            write_u64!(used * 4); // frames to KiB (4KB per frame)
            write_str!(b" KiB\nfree:\t");
            write_u64!(free / 1024);
            write_str!(b" KiB\n");
        }
        PROC_TASKS => {
            // List all active tasks: PID STATE ROOT
            for i in 0..crate::task::MAX_TASKS {
                if let Some(info) = crate::task::get_task_info(i) {
                    write_str!(b"pid=");
                    write_u64!(info.id);
                    write_str!(b" state=");
                    match info.state {
                        0 => write_str!(b"empty"),
                        1 => write_str!(b"ready"),
                        2 => write_str!(b"exited"),
                        _ => write_str!(b"?"),
                    }
                    write_str!(b" pgid=");
                    write_u64!(info.pgid);
                    write_str!(b"\n");
                }
            }
        }
        PROC_SELF_CMDLINE => {
            let name = crate::task::current_name();
            let name_len = name.iter().position(|&b| b == 0).unwrap_or(16);
            for &b in &name[..name_len] {
                if pos < 255 { buf[pos] = b; pos += 1; }
            }
            if pos < 255 { buf[pos] = b'\n'; pos += 1; }
        }
        PROC_SELF_MAPS => {
            // Show memory regions for current task.
            write_str!(b"00400000-007fffff r-x  [code]\n");
            write_str!(b"00800000-009fffff rw-  [data/heap]\n");
            // Stack region (top of user address space).
            write_str!(b"00a00000-00a07fff rw-  [stack]\n");
        }
        _ => {}
    }
    buf
}

/// Read from a /proc virtual file.
fn proc_read(proc_type: usize, task_id: u8, offset: u32, buf: &mut [u8]) -> usize {
    let content = proc_generate(proc_type, task_id);
    let len = content.iter().position(|&b| b == 0).unwrap_or(256);
    let start = offset as usize;
    if start >= len { return 0; }
    let avail = len - start;
    let to_copy = core::cmp::min(avail, buf.len());
    buf[..to_copy].copy_from_slice(&content[start..start + to_copy]);
    to_copy
}
