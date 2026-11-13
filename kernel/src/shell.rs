//! M5 interactive shell: line editing over the keyboard queue, tiny commands.
//!
//! No allocation on the input path (fixed 256B line buffer); IRQ handlers
//! never touch this module -- the [`crate::keyboard`] SPSC queue is the only
//! handoff. All printing is IF-safe via `kprint!`.

use crate::cpu::{hlt, outb, outw};
use crate::{heap, interrupts, kprint, mem};

const MAX_LINE: usize = 256;

static mut LINE: [u8; MAX_LINE] = [0; MAX_LINE];
static mut LEN: usize = 0;

// ---- Environment variables ----

const MAX_ENV_VARS: usize = 16;
const MAX_ENV_LEN: usize = 64; // max length of "KEY=VALUE"

static mut ENV_VARS: [[u8; MAX_ENV_LEN]; MAX_ENV_VARS] = [[0; MAX_ENV_LEN]; MAX_ENV_VARS];
static mut ENV_COUNT: usize = 0;

/// Find an environment variable by key. Returns index or None.
fn env_find(key: &[u8]) -> Option<usize> {
    unsafe {
        for i in 0..ENV_COUNT {
            let entry = &ENV_VARS[i];
            // Find '=' separator.
            if let Some(eq_pos) = entry.iter().position(|&b| b == b'=') {
                if &entry[..eq_pos] == key {
                    return Some(i);
                }
            }
        }
    }
    None
}

/// Set an environment variable (create or update).
fn env_set(key: &[u8], value: &[u8]) -> bool {
    let total_len = key.len() + 1 + value.len(); // KEY=VALUE
    if total_len >= MAX_ENV_LEN { return false; }

    // Try to update existing.
    if let Some(idx) = env_find(key) {
        unsafe {
            let entry = &mut ENV_VARS[idx];
            entry[..key.len()].copy_from_slice(key);
            entry[key.len()] = b'=';
            entry[key.len() + 1..key.len() + 1 + value.len()].copy_from_slice(value);
            entry[total_len] = 0;
        }
        return true;
    }

    // Create new.
    unsafe {
        if ENV_COUNT >= MAX_ENV_VARS { return false; }
        let entry = &mut ENV_VARS[ENV_COUNT];
        entry[..key.len()].copy_from_slice(key);
        entry[key.len()] = b'=';
        entry[key.len() + 1..key.len() + 1 + value.len()].copy_from_slice(value);
        entry[total_len] = 0;
        ENV_COUNT += 1;
    }
    true
}

/// Unset an environment variable.
fn env_unset(key: &[u8]) -> bool {
    if let Some(idx) = env_find(key) {
        unsafe {
            ENV_COUNT -= 1;
            if idx < ENV_COUNT {
                ENV_VARS[idx] = ENV_VARS[ENV_COUNT];
            }
            ENV_VARS[ENV_COUNT] = [0; MAX_ENV_LEN];
        }
        true
    } else {
        false
    }
}

// ---- Command aliases ----

const MAX_ALIASES: usize = 16;
const MAX_ALIAS_LEN: usize = 64; // max length of "name=command"

static mut ALIASES: [[u8; MAX_ALIAS_LEN]; MAX_ALIASES] = [[0; MAX_ALIAS_LEN]; MAX_ALIASES];
static mut ALIAS_COUNT: usize = 0;

/// Find an alias by name. Returns the expansion (command string) or None.
fn alias_find(name: &[u8]) -> Option<&'static [u8]> {
    unsafe {
        for i in 0..ALIAS_COUNT {
            let entry = &ALIASES[i];
            if let Some(eq_pos) = entry.iter().position(|&b| b == b'=') {
                if &entry[..eq_pos] == name {
                    let val_start = eq_pos + 1;
                    let val_end = entry[val_start..].iter().position(|&b| b == 0).unwrap_or(MAX_ALIAS_LEN - val_start) + val_start;
                    return Some(&entry[val_start..val_end]);
                }
            }
        }
    }
    None
}

/// Set an alias (create or update).
fn alias_set(name: &[u8], expansion: &[u8]) -> bool {
    let total_len = name.len() + 1 + expansion.len();
    if total_len >= MAX_ALIAS_LEN { return false; }

    // Try to update existing.
    unsafe {
        for i in 0..ALIAS_COUNT {
            let entry = &ALIASES[i];
            if let Some(eq_pos) = entry.iter().position(|&b| b == b'=') {
                if &entry[..eq_pos] == name {
                    let e = &mut ALIASES[i];
                    e[..name.len()].copy_from_slice(name);
                    e[name.len()] = b'=';
                    e[name.len() + 1..total_len].copy_from_slice(expansion);
                    e[total_len] = 0;
                    return true;
                }
            }
        }
        if ALIAS_COUNT >= MAX_ALIASES { return false; }
        let e = &mut ALIASES[ALIAS_COUNT];
        e[..name.len()].copy_from_slice(name);
        e[name.len()] = b'=';
        e[name.len() + 1..total_len].copy_from_slice(expansion);
        e[total_len] = 0;
        ALIAS_COUNT += 1;
    }
    true
}

/// List all aliases.
fn alias_list() {
    unsafe {
        if ALIAS_COUNT == 0 {
            kprint!("(no aliases defined)\n");
            return;
        }
        for i in 0..ALIAS_COUNT {
            let entry = &ALIASES[i];
            let end = entry.iter().position(|&b| b == 0).unwrap_or(MAX_ALIAS_LEN);
            let s = core::str::from_utf8(&entry[..end]).unwrap_or("?");
            kprint!("{}\n", s);
        }
    }
}

/// Remove an alias by name. Returns true if found and removed.
fn alias_remove(name: &[u8]) -> bool {
    unsafe {
        for i in 0..ALIAS_COUNT {
            let entry = &ALIASES[i];
            if let Some(eq_pos) = entry.iter().position(|&b| b == b'=') {
                if &entry[..eq_pos] == name {
                    for j in i..ALIAS_COUNT - 1 {
                        ALIASES[j] = ALIASES[j + 1];
                    }
                    ALIASES[ALIAS_COUNT - 1] = [0; MAX_ALIAS_LEN];
                    ALIAS_COUNT -= 1;
                    return true;
                }
            }
        }
    }
    false
}

/// Command help entries: (name, usage, description).
const HELP_DB: &[(&[u8], &str, &str)] = &[
    (b"help", "help [cmd]", "Show all commands or detailed help for one command"),
    (b"clear", "clear", "Clear the screen"),
    (b"mem", "mem", "Show physical memory map and frame allocator stats"),
    (b"heap", "heap", "Show kernel heap usage"),
    (b"ticks", "ticks", "Show PIT tick count"),
    (b"uptime", "uptime", "Show system uptime in seconds"),
    (b"echo", "echo <text>", "Print text to the console"),
    (b"sleep", "sleep <ms>", "Sleep for N milliseconds"),
    (b"tasks", "tasks", "List all tasks (alias: ps)"),
    (b"drivers", "drivers", "List registered hardware drivers"),
    (b"smp", "smp", "Show SMP/CPU topology info"),
    (b"progs", "progs", "List loaded user programs"),
    (b"ls", "ls", "List files in /bin/ (initrd)"),
    (b"cat", "cat <name>", "Show file contents (hex dump)"),
    (b"write", "write <name> <text>", "Write text to an initrd file"),
    (b"wc", "wc <name>", "Count lines, words, chars in a file"),
    (b"grep", "grep <pattern> <file>", "Search for pattern in a file"),
    (b"net", "net", "Show network device status"),
    (b"exec", "exec <name>", "Execute a user program by name"),
    (b"user", "user", "Spawn demo ring3 task (hello)"),
    (b"userbad", "userbad", "Test ring3 kernel memory read (should fault)"),
    (b"usernx", "usernx", "Test ring3 stack execution (should fault)"),
    (b"userexec", "userexec", "Test ring3 kernel text execution (should fault)"),
    (b"userptr", "userptr", "Test syscall pointer validation"),
    (b"isowrite", "isowrite", "Test write isolation between tasks"),
    (b"heaptest", "heaptest", "Test user-space heap allocation"),
    (b"echo2", "echo2", "Echo from heap buffer (user program)"),
    (b"uptime2", "uptime2", "Show uptime (user program)"),
    (b"meminfo", "meminfo", "Show memory stats (user program)"),
    (b"counter", "counter", "Counter test (user program)"),
    (b"guess", "guess", "Number guessing game (interactive)"),
    (b"edit", "edit", "Simple line editor (interactive)"),
    (b"calc", "calc", "Simple calculator (user program)"),
    (b"heapstress", "heapstress", "Heap alloc/free stress test"),
    (b"ping", "ping", "Send ICMP echo request"),
    (b"netdump", "netdump", "Dump received network packets"),
    (b"gui", "gui", "Enter graphical mode (mouse)"),
    (b"mouse", "mouse", "Show mouse position and buttons"),
    (b"kbddebug", "kbddebug", "Toggle keyboard scancode tracing"),
    (b"test", "test", "Run one-shot task test"),
    (b"blk", "blk", "Show block device (virtio-blk) status"),
    (b"blktest", "blktest", "Test block device read/write"),
    (b"mkfs", "mkfs", "Format disk with NEO-FS"),
    (b"fsinfo", "fsinfo", "Show NEO-FS filesystem info"),
    (b"fls", "fls [pattern]", "List files on NEO-FS (supports * and ?)"),
    (b"fcat", "fcat <name>", "Read file from NEO-FS"),
    (b"fwrite", "fwrite <name> <text>", "Write text to NEO-FS file"),
    (b"frm", "frm <name>", "Remove file from NEO-FS"),
    (b"fstest", "fstest", "Test VFS syscalls from ring3"),
    (b"forktest", "forktest", "Test fork/wait process semantics from ring3"),
    (b"exectest", "exectest", "Test exec image replacement semantics from ring3"),
    (b"fmkdir", "fmkdir <name>", "Create directory on NEO-FS"),
    (b"frmdir", "frmdir <name>", "Remove empty directory on NEO-FS"),
    (b"fmv", "fmv <old> <new>", "Rename file/directory on NEO-FS"),
    (b"fsck", "fsck", "Check NEO-FS filesystem consistency"),
    (b"set", "set KEY=VALUE", "Set environment variable"),
    (b"export", "export KEY=VALUE", "Set environment variable (POSIX alias for set)"),
    (b"unset", "unset KEY", "Unset environment variable"),
    (b"env", "env", "List all environment variables"),
    (b"alias", "alias [name=cmd]", "Define or list command aliases"),
    (b"unalias", "unalias <name>", "Remove a command alias"),
    (b"history", "history", "Show command history"),
    (b"source", "source <file>", "Execute script file from NEO-FS"),
    (b"type", "type <cmd>", "Show command type (builtin/alias/program)"),
    (b"which", "which <cmd>", "Locate a command"),
    (b"uls", "uls", "List files (ring3 user program)"),
    (b"ucat", "ucat", "Cat file (ring3 user program)"),
    (b"hexdump", "hexdump", "Hex dump of file (ring3)"),
    (b"whoami", "whoami", "Show current task ID and uptime (ring3)"),
    (b"ugrep", "ugrep", "Search file for pattern (ring3)"),
    (b"ups", "ups", "Show running tasks (ring3 ps)"),
    (b"udd", "udd", "Read disk sector 0 hex dump (ring3 dd)"),
    (b"ukill", "ukill", "Test kill/getpid syscalls (ring3)"),
    (b"kill", "kill <pid>", "Send SIGTERM to a task"),
    (b"mount", "mount [dev]", "Mount NEO-FS on block device (default 0)"),
    (b"umount", "umount", "Unmount NEO-FS"),
    (b"df", "df", "Show disk usage statistics"),
    (b"fslimit", "fslimit", "Show filesystem limits"),
    (b"fwc", "fwc <name>", "Count lines/words/chars in NEO-FS file"),
    (b"uexec", "uexec <name>", "Exec program (replace current task)"),
    (b"ahci", "ahci", "Show AHCI/SATA controller info"),
    (b"usb", "usb", "Show USB controller info"),
    (b"ipi", "ipi", "Send IPI test to self"),
    (b"ufileops", "ufileops", "Test mkdir/rm/touch from ring3"),
    (b"date", "date", "Show current date/time from RTC"),
    (b"acpi", "acpi", "Show ACPI table information"),
    (b"arp", "arp", "Show ARP cache / send ARP request"),
    (b"nslookup", "nslookup <name>", "DNS lookup via UDP"),
    (b"tcpclose", "tcpclose", "Close active TCP connection"),
    (b"netstat", "netstat", "Show TCP connection status"),
    (b"top", "top", "Show system overview (tasks + memory + uptime)"),
    (b"ffind", "ffind <pattern>", "Find files matching glob pattern"),
    (b"ftouch", "ftouch <name>", "Create empty file on NEO-FS"),
    (b"fcp", "fcp <src> <dst>", "Copy file on NEO-FS"),
    (b"unittest", "unittest", "Run kernel unit tests"),
    (b"off", "off", "Power off (QEMU ACPI shutdown)"),
    (b"reboot", "reboot", "Reboot the system"),
];

fn line() -> &'static [u8] {
    unsafe { &LINE[..LEN] }
}

fn push_byte(b: u8) {
    unsafe {
        if LEN < MAX_LINE {
            LINE[LEN] = b;
            LEN += 1;
        }
    }
}

fn pop_byte() {
    unsafe {
        if LEN > 0 {
            LEN -= 1;
        }
    }
}

fn clear_line() {
    unsafe {
        LEN = 0;
    }
}

/// Command history: store last N commands.
const MAX_HISTORY: usize = 8;
static mut HISTORY: [[u8; MAX_LINE]; MAX_HISTORY] = [[0; MAX_LINE]; MAX_HISTORY];
static mut HISTORY_LENS: [usize; MAX_HISTORY] = [0; MAX_HISTORY];
static mut NHISTORY: usize = 0;
static mut HISTORY_POS: usize = 0; // 0 = current, 1..N = history entries

/// Save current line to history and reset history position.
fn save_to_history() {
    unsafe {
        if LEN == 0 {
            HISTORY_POS = 0;
            return;
        }
        // Shift history down.
        for i in (1..MAX_HISTORY).rev() {
            HISTORY[i] = HISTORY[i - 1];
            HISTORY_LENS[i] = HISTORY_LENS[i - 1];
        }
        HISTORY[0] = LINE;
        HISTORY_LENS[0] = LEN;
        if NHISTORY < MAX_HISTORY {
            NHISTORY += 1;
        }
        HISTORY_POS = 0;
    }
    // Persist history to NEO-FS.
    persist_history();
}

/// Save history to /history file on NEO-FS.
fn persist_history() {
    if !crate::neofs::is_mounted() { return; }
    let mut buf = [0u8; 1024];
    let mut pos = 0usize;
    unsafe {
        // Write newest first (index 0 = most recent).
        for i in 0..NHISTORY {
            let len = HISTORY_LENS[i];
            if len == 0 || pos + len + 1 > buf.len() { break; }
            buf[pos..pos + len].copy_from_slice(&HISTORY[i][..len]);
            pos += len;
            buf[pos] = b'\n';
            pos += 1;
        }
    }
    if pos > 0 {
        let fd = crate::vfs::vfs_open(b"history", 1, 0);
        if fd >= 0 {
            crate::vfs::vfs_write(fd as u32, &buf[..pos], 0);
            crate::vfs::vfs_close(fd as u32, 0);
        }
    }
}

/// Load history from /history file on NEO-FS at boot.
pub fn load_history() {
    if !crate::neofs::is_mounted() { return; }
    let fd = crate::vfs::vfs_open(b"history", 0, 0);
    if fd < 0 { return; }
    let mut buf = [0u8; 1024];
    let n = crate::vfs::vfs_read(fd as u32, &mut buf, 0);
    crate::vfs::vfs_close(fd as u32, 0);
    if n <= 0 { return; }

    // Parse lines into history (newest first in file).
    let data = &buf[..n as usize];
    let mut count = 0usize;
    let mut line_start = 0;
    for i in 0..data.len() {
        if data[i] == b'\n' {
            let line_len = i - line_start;
            if line_len > 0 && line_len <= MAX_LINE && count < MAX_HISTORY {
                unsafe {
                    HISTORY[count][..line_len].copy_from_slice(&data[line_start..i]);
                    HISTORY_LENS[count] = line_len;
                }
                count += 1;
            }
            line_start = i + 1;
        }
    }
    unsafe { NHISTORY = count; }
    if count > 0 {
        crate::sinfo!("shell: loaded {} history entries", count);
    }
}

/// Navigate history: dir=1 for up (older), dir=-1 for down (newer).
/// Returns true if a history entry was loaded.
fn navigate_history(dir: i32) -> bool {
    unsafe {
        if NHISTORY == 0 {
            return false;
        }
        if dir > 0 && HISTORY_POS < NHISTORY {
            if HISTORY_POS == 0 {
                // Save current line as scratch.
                HISTORY[0] = LINE;
                HISTORY_LENS[0] = LEN;
            }
            HISTORY_POS += 1;
            if HISTORY_POS > NHISTORY {
                HISTORY_POS = NHISTORY;
            }
        } else if dir < 0 && HISTORY_POS > 0 {
            HISTORY_POS -= 1;
        } else {
            return false;
        }
        // Load the history entry.
        let src = &HISTORY[HISTORY_POS];
        let src_len = HISTORY_LENS[HISTORY_POS];
        for i in 0..src_len {
            LINE[i] = src[i];
        }
        LEN = src_len;
        true
    }
}

/// Tab completion: complete the current word against commands or filenames.
fn tab_complete() {
    unsafe {
        if LEN == 0 {
            return;
        }
        // Find the start of the current word (last space + 1).
        let mut word_start = 0;
        for i in (0..LEN).rev() {
            if LINE[i] == b' ' || LINE[i] == b'\t' {
                word_start = i + 1;
                break;
            }
        }
        let prefix = &LINE[word_start..LEN];
        if prefix.is_empty() {
            return;
        }

        // Collect candidates.
        let mut candidates: [&[u8]; 64] = [&[]; 64];
        let mut ncandidates = 0usize;

        if word_start == 0 {
            // First word: complete command names from HELP_DB.
            for entry in HELP_DB.iter() {
                if entry.0.len() >= prefix.len() && &entry.0[..prefix.len()] == prefix {
                    if ncandidates < 64 {
                        candidates[ncandidates] = entry.0;
                        ncandidates += 1;
                    }
                }
            }
        } else {
            // Subsequent word: complete NEO-FS filenames.
            if crate::neofs::is_mounted() {
                // Use a static buffer for filename candidates (avoid stack overflow).
                static mut FILE_CANDIDATES: [[u8; 16]; 64] = [[0; 16]; 64];
                crate::vfs::vfs_readdir(&mut |name, _mode, _size| {
                    if name.len() >= prefix.len() && &name[..prefix.len()] == prefix {
                        if ncandidates < 64 {
                            let len = core::cmp::min(name.len(), 16);
                            FILE_CANDIDATES[ncandidates][..len].copy_from_slice(&name[..len]);
                            candidates[ncandidates] = &FILE_CANDIDATES[ncandidates][..len];
                            ncandidates += 1;
                        }
                    }
                });
            }
        }

        if ncandidates == 0 {
            return;
        }
        if ncandidates == 1 {
            // Single candidate: complete it.
            let cand = candidates[0];
            // Erase current word.
            for _ in word_start..LEN {
                kprint!("\x08");
            }
            // Write completed word.
            for i in 0..cand.len() {
                let c = cand[i];
                if word_start + i < MAX_LINE {
                    LINE[word_start + i] = c;
                    kprint!("{}", c as char);
                }
            }
            LEN = word_start + cand.len();
            // Add a trailing space.
            if LEN < MAX_LINE {
                LINE[LEN] = b' ';
                LEN += 1;
                kprint!(" ");
            }
        } else {
            // Multiple candidates: print them.
            kprint!("\n");
            for i in 0..ncandidates {
                kprint!("{} ", core::str::from_utf8(candidates[i]).unwrap_or("?"));
            }
            kprint!("\n");
            // Redraw prompt + current line.
            prompt();
            for i in 0..LEN {
                kprint!("{}", LINE[i] as char);
            }
        }
    }
}

pub fn prompt() {
    kprint!("neo> ");
}

/// Drain the keyboard queue; returns bytes consumed (0 => caller may hlt).
pub fn pump() -> usize {
    let mut n = 0usize;
    while let Some(b) = crate::keyboard::pop() {
        n += 1;
        on_byte(b);
    }
    n
}

fn on_byte(b: u8) {
    match b {
        b'\n' | b'\r' => {
            kprint!("\n");
            let l = line();
            // If collecting a script block, route to script handler.
            if script_active() {
                if script_handle_line(l) {
                    save_to_history();
                    clear_line();
                    prompt();
                    return;
                }
            }
            exec(l);
            save_to_history();
            clear_line();
            prompt();
        }
        0x08 => {
            // Backspace: console erases the cell; drop from buffer.
            if unsafe { LEN } > 0 {
                pop_byte();
                kprint!("\x08");
            }
        }
        0x01 => {
            // Up arrow: navigate to older command in history.
            if navigate_history(1) {
                // Clear current line on screen and rewrite with history entry.
                // Simple approach: clear to end of line and rewrite.
                let len = unsafe { LEN };
                for _ in 0..len {
                    kprint!("\x08"); // backspace
                }
                let hist_len = unsafe { LEN };
                for i in 0..hist_len {
                    let c = unsafe { LINE[i] };
                    kprint!("{}", c as char);
                }
            }
        }
        0x02 => {
            // Down arrow: navigate to newer command in history.
            if navigate_history(-1) {
                let len = unsafe { LEN };
                for _ in 0..len {
                    kprint!("\x08");
                }
                let hist_len = unsafe { LEN };
                for i in 0..hist_len {
                    let c = unsafe { LINE[i] };
                    kprint!("{}", c as char);
                }
            }
        }
        b'\t' => {
            // Tab: command completion.
            tab_complete();
        }
        0x03 => {
            // Ctrl+C: cancel current line or kill foreground task.
            kprint!("^C\n");
            clear_line();
            prompt();
        }
        0x1B => {}   // escape: ignore
        c if c == b' ' || c.is_ascii_graphic() => {
            if unsafe { LEN } < MAX_LINE {
                push_byte(c);
                kprint!("{}", c as char);
            }
        }
        _ => {} // other control bytes: ignore
    }
}

fn parse_u64(s: &[u8]) -> Option<u64> {
    if s.is_empty() {
        return None;
    }
    let mut v: u64 = 0;
    for &c in s {
        if !c.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add((c - b'0') as u64)?;
    }
    Some(v)
}

/// Trim leading and trailing whitespace from a byte slice.
fn trim_bytes(s: &[u8]) -> &[u8] {
    let start = s.iter().position(|&b| b != b' ' && b != b'\t').unwrap_or(s.len());
    let end = s.iter().rposition(|&b| b != b' ' && b != b'\t').map(|p| p + 1).unwrap_or(start);
    &s[start..end]
}

/// Find position of ">>" in a byte slice. Returns index of first '>'.
fn find_double_gt(line: &[u8]) -> Option<usize> {
    for i in 0..line.len().saturating_sub(1) {
        if line[i] == b'>' && line[i + 1] == b'>' {
            return Some(i);
        }
    }
    None
}

/// Simple glob pattern matching. Supports '*' (any sequence) and '?' (one char).
/// Returns true if `name` matches `pattern`.
pub fn glob_match(pattern: &[u8], name: &[u8]) -> bool {
    let mut pi = 0usize; // pattern index
    let mut ni = 0usize; // name index
    let mut star_pi = usize::MAX; // position after last '*'
    let mut star_ni = 0usize;     // name position when last '*' was seen

    while ni < name.len() || pi < pattern.len() {
        if pi < pattern.len() && pattern[pi] == b'*' {
            // Record star position and try matching zero chars.
            star_pi = pi + 1;
            star_ni = ni;
            pi += 1;
            continue;
        }
        if ni < name.len() && pi < pattern.len() && (pattern[pi] == b'?' || pattern[pi] == name[ni]) {
            pi += 1;
            ni += 1;
            continue;
        }
        // Mismatch: backtrack to last star if possible.
        if star_pi != usize::MAX {
            pi = star_pi;
            star_ni += 1;
            ni = star_ni;
            continue;
        }
        return false;
    }
    true
}

/// First whitespace-delimited word, lowercased (commands are lowercase).
/// Fills `out` in place (arrays are Copy: by-value params would be a no-op,
/// which is exactly the M5 bug that made every command "unknown").
fn cmd_word(line: &[u8], out: &mut [u8; 16]) -> (usize, usize) {
    let mut i = 0usize;
    while i < line.len() && line[i] == b' ' {
        i += 1;
    }
    let start = i;
    let mut n = 0usize;
    while i < line.len() && line[i] != b' ' && n < out.len() {
        let c = line[i];
        out[n] = c.to_ascii_lowercase();
        n += 1;
        i += 1;
    }
    // If the word is longer than `out`, still consume it for arg offset.
    while i < line.len() && line[i] != b' ' {
        i += 1;
    }
    let _ = start;
    (n, i)
}

fn rest_of(line: &[u8], mut i: usize) -> &[u8] {
    while i < line.len() && line[i] == b' ' {
        i += 1;
    }
    &line[i.min(line.len())..]
}

fn print_bytes(s: &[u8]) {
    kprint!(
        "{}",
        core::str::from_utf8(s).unwrap_or("<non-utf8>")
    );
}

/// Expand $VAR references in a line, writing result to `out`.
/// Returns the number of bytes written to `out`.
fn expand_vars(line: &[u8], out: &mut [u8; MAX_LINE]) -> usize {
    let mut oi = 0usize;
    let mut i = 0usize;
    while i < line.len() && oi < MAX_LINE - 1 {
        if line[i] == b'$' && i + 1 < line.len() {
            // Extract variable name (alphanumeric + underscore).
            let name_start = i + 1;
            let mut name_end = name_start;
            while name_end < line.len() && (line[name_end].is_ascii_alphanumeric() || line[name_end] == b'_') {
                name_end += 1;
            }
            if name_end > name_start {
                let var_name = &line[name_start..name_end];
                // Look up in environment.
                if let Some(idx) = env_find(var_name) {
                    unsafe {
                        let entry = &ENV_VARS[idx];
                        // Find '=' to get value part.
                        if let Some(eq) = entry.iter().position(|&b| b == b'=') {
                            let val_start = eq + 1;
                            let val_end = entry[val_start..].iter().position(|&b| b == 0).unwrap_or(MAX_ENV_LEN - val_start) + val_start;
                            for &b in &entry[val_start..val_end] {
                                if oi < MAX_LINE - 1 {
                                    out[oi] = b;
                                    oi += 1;
                                }
                            }
                        }
                    }
                }
                // If not found, $VAR expands to empty string.
                i = name_end;
            } else {
                // Lone $ at end or followed by non-alnum: keep as-is.
                out[oi] = line[i];
                oi += 1;
                i += 1;
            }
        } else {
            out[oi] = line[i];
            oi += 1;
            i += 1;
        }
    }
    oi
}

fn shutdown() -> ! {
    kprint!("powering off (close the window if QEMU stays)...\n");
    // q35 PM + Bochs APM, in that order. If neither is wired, we hang below.
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    loop {
        hlt();
    }
}

fn reboot() -> ! {
    kprint!("rebooting...\n");
    // 8042 reset pulse; QEMU honors it on q35.
    outb(0x64, 0xFE);
    // Fast-reset fallback (port 0x92 bit 0).
    outb(0x92, 0x01);
    loop {
        hlt();
    }
}

/// Execute a single command line programmatically (for init scripts).
pub fn run_line(line: &[u8]) {
    let trimmed = trim_bytes(line);
    if trimmed.is_empty() { return; }

    // Handle script constructs.
    if trimmed.contains(&b';') {
        run_script_line(trimmed);
        return;
    }

    // Check for if/while keywords.
    let mut wbuf = [0u8; 16];
    let (wn, _) = cmd_word(trimmed, &mut wbuf);
    let w = &wbuf[..wn];

    match w {
        b"if" => {
            script_start_block(trimmed, true);
        }
        b"while" => {
            script_start_block(trimmed, false);
        }
        b"fi" | b"done" | b"then" | b"do" => {
            // These are handled by the block collector.
            kprint!("script: unexpected '{}'\n", core::str::from_utf8(w).unwrap_or("?"));
        }
        _ => {
            exec(trimmed);
        }
    }
}

/// Execute a script line with semicolon-separated commands.
fn run_script_line(line: &[u8]) {
    let mut start = 0usize;
    let mut i = 0usize;
    while i <= line.len() {
        if i == line.len() || line[i] == b';' {
            let cmd = trim_bytes(&line[start..i]);
            if !cmd.is_empty() {
                exec(cmd);
            }
            start = i + 1;
        }
        i += 1;
    }
}

// ---- Script block state ----

const MAX_SCRIPT_LINES: usize = 32;
static mut SCRIPT_LINES: [[u8; MAX_LINE]; MAX_SCRIPT_LINES] = [[0; MAX_LINE]; MAX_SCRIPT_LINES];
static mut SCRIPT_COUNT: usize = 0;
static mut SCRIPT_CONDITION: [u8; MAX_LINE] = [0; MAX_LINE];
static mut SCRIPT_IS_IF: bool = true; // true=if, false=while
static SCRIPT_ACTIVE: core::sync::atomic::AtomicBool = core::sync::atomic::AtomicBool::new(false);

/// Start collecting a script block (if/while).
fn script_start_block(line: &[u8], is_if: bool) {
    // Extract condition: "if <cmd>" or "while <cmd>".
    let mut wbuf = [0u8; 16];
    let (_, wend) = cmd_word(line, &mut wbuf);
    let cond = trim_bytes(&line[wend..]);
    unsafe {
        if cond.len() < MAX_LINE {
            SCRIPT_CONDITION[..cond.len()].copy_from_slice(cond);
            SCRIPT_CONDITION[cond.len()] = 0;
        }
        SCRIPT_COUNT = 0;
        SCRIPT_IS_IF = is_if;
        SCRIPT_ACTIVE.store(true, core::sync::atomic::Ordering::SeqCst);
    }
}

/// Add a line to the current script block.
fn script_add_line(line: &[u8]) {
    unsafe {
        if SCRIPT_COUNT >= MAX_SCRIPT_LINES { return; }
        let len = core::cmp::min(line.len(), MAX_LINE - 1);
        SCRIPT_LINES[SCRIPT_COUNT][..len].copy_from_slice(&line[..len]);
        SCRIPT_LINES[SCRIPT_COUNT][len] = 0;
        SCRIPT_COUNT += 1;
    }
}

/// Execute the collected script block.
fn script_execute() {
    unsafe {
        SCRIPT_ACTIVE.store(false, core::sync::atomic::Ordering::SeqCst);
    }

    if unsafe { SCRIPT_IS_IF } {
        // if: evaluate condition, execute body if true.
        let cond = unsafe {
            let len = SCRIPT_CONDITION.iter().position(|&b| b == 0).unwrap_or(0);
            &SCRIPT_CONDITION[..len]
        };
        // Run condition command and check output for error indicators.
        crate::klog::redirect_start();
        exec(cond);
        let output = crate::klog::redirect_stop();
        let success = !contains_error(output);
        if success {
            for i in 0..unsafe { SCRIPT_COUNT } {
                let line = unsafe {
                    let l = &SCRIPT_LINES[i];
                    let len = l.iter().position(|&b| b == 0).unwrap_or(MAX_LINE);
                    &l[..len]
                };
                exec(trim_bytes(line));
            }
        }
    } else {
        // while: loop until condition fails.
        let cond = unsafe {
            let len = SCRIPT_CONDITION.iter().position(|&b| b == 0).unwrap_or(0);
            &SCRIPT_CONDITION[..len]
        };
        let mut iterations = 0;
        loop {
            if iterations >= 100 { break; } // safety limit
            crate::klog::redirect_start();
            exec(cond);
            let output = crate::klog::redirect_stop();
            if contains_error(output) { break; }
            for i in 0..unsafe { SCRIPT_COUNT } {
                let line = unsafe {
                    let l = &SCRIPT_LINES[i];
                    let len = l.iter().position(|&b| b == 0).unwrap_or(MAX_LINE);
                    &l[..len]
                };
                exec(trim_bytes(line));
            }
            iterations += 1;
        }
    }
}

/// Check if captured output contains error indicators.
fn contains_error(output: &[u8]) -> bool {
    let s = core::str::from_utf8(output).unwrap_or("");
    s.contains("error") || s.contains("failed") || s.contains("unknown")
        || s.contains("not found") || s.contains("FAIL")
}

/// Check if we're currently collecting a script block.
pub fn script_active() -> bool {
    SCRIPT_ACTIVE.load(core::sync::atomic::Ordering::SeqCst)
}

/// Handle a line that might be part of a script block.
/// Returns true if the line was consumed by the script system.
pub fn script_handle_line(line: &[u8]) -> bool {
    if !script_active() { return false; }

    let trimmed = trim_bytes(line);
    let mut wbuf = [0u8; 16];
    let (wn, _) = cmd_word(trimmed, &mut wbuf);
    let w = &wbuf[..wn];

    match w {
        b"fi" => {
            if unsafe { SCRIPT_IS_IF } {
                script_execute();
            }
            true
        }
        b"done" => {
            if !unsafe { SCRIPT_IS_IF } {
                script_execute();
            }
            true
        }
        b"then" | b"do" => {
            // Block body starts; just skip the keyword.
            true
        }
        _ => {
            script_add_line(trimmed);
            true
        }
    }
}

fn exec(line: &[u8]) {
    // Expand $VAR references before processing.
    let mut expanded = [0u8; MAX_LINE];
    let elen = expand_vars(line, &mut expanded);
    let line = &expanded[..elen];

    // Check for pipe: "cmd1 | cmd2"
    // Pipe has highest redirection priority. Find '|' that is not inside quotes.
    if let Some(pipe_pos) = line.iter().position(|&b| b == b'|') {
        let left = trim_bytes(&line[..pipe_pos]);
        let right = trim_bytes(&line[pipe_pos + 1..]);
        if !left.is_empty() && !right.is_empty() {
            // Capture left command's output.
            crate::klog::redirect_start();
            exec(left);
            let captured = crate::klog::redirect_stop();

            if !captured.is_empty() {
                // Feed captured output as input to right command.
                // Build: right_cmd + " " + first_line_of_captured
                let mut combined = [0u8; MAX_LINE];
                let mut ci = 0usize;
                for &b in right {
                    if ci < MAX_LINE - 1 { combined[ci] = b; ci += 1; }
                }
                if ci < MAX_LINE - 1 { combined[ci] = b' '; ci += 1; }
                // Append first line of captured output.
                for &b in captured.iter() {
                    if b == b'\n' || b == b'\r' { break; }
                    if ci < MAX_LINE - 1 { combined[ci] = b; ci += 1; }
                }
                exec(&combined[..ci]);
            } else {
                // No output from left — just run right with no extra args.
                exec(right);
            }
            return;
        }
    }

    // Check for input redirection: "cmd < filename"
    if let Some(lt_pos) = line.iter().position(|&b| b == b'<') {
        let cmd_part = trim_bytes(&line[..lt_pos]);
        let file_part = trim_bytes(&line[lt_pos + 1..]);
        if !file_part.is_empty() && !cmd_part.is_empty() {
            // Read file content.
            let fd = crate::vfs::vfs_open(file_part, 0, 0);
            if fd >= 0 {
                let mut fbuf = [0u8; 512];
                let n = crate::vfs::vfs_read(fd as u32, &mut fbuf, 0);
                crate::vfs::vfs_close(fd as u32, 0);
                if n > 0 {
                    // Build new command: cmd + file_content
                    let mut combined = [0u8; MAX_LINE];
                    let mut ci = 0usize;
                    for &b in cmd_part {
                        if ci < MAX_LINE - 1 { combined[ci] = b; ci += 1; }
                    }
                    if ci < MAX_LINE - 1 { combined[ci] = b' '; ci += 1; }
                    let content_end = core::cmp::min(n as usize, MAX_LINE - ci - 1);
                    for j in 0..content_end {
                        let b = fbuf[j];
                        if b == b'\n' || b == b'\r' { break; } // first line only
                        combined[ci] = b;
                        ci += 1;
                    }
                    exec(&combined[..ci]);
                    return;
                }
            } else {
                kprint!("< : cannot open '");
                print_bytes(file_part);
                kprint!("'\n");
                return;
            }
        }
    }

    // Check for output redirection: "cmd >> filename" (append) or "cmd > filename" (truncate).
    // Must check >> before > to avoid matching the first > of >>.
    let redirect = if let Some(pos) = find_double_gt(line) {
        // >> append mode
        Some((pos, 3u32)) // flags=3 = append
    } else if let Some(pos) = line.iter().position(|&b| b == b'>') {
        // > truncate mode
        Some((pos, 1u32)) // flags=1 = write/truncate
    } else {
        None
    };

    if let Some((gt_pos, open_flags)) = redirect {
        let cmd_part = &line[..gt_pos];
        // For >>, skip 2 chars; for >, skip 1 char.
        let skip = if open_flags == 3 { 2 } else { 1 };
        let file_part = &line[gt_pos + skip..];
        let cmd_trimmed = trim_bytes(cmd_part);
        let file_trimmed = trim_bytes(file_part);
        if !file_trimmed.is_empty() && !cmd_trimmed.is_empty() {
            crate::klog::redirect_start();
            exec(cmd_trimmed);
            let captured = crate::klog::redirect_stop();

            let fd = crate::vfs::vfs_open(file_trimmed, open_flags, 0);
            if fd >= 0 {
                if !captured.is_empty() {
                    crate::vfs::vfs_write(fd as u32, captured, 0);
                }
                crate::vfs::vfs_close(fd as u32, 0);
            } else {
                kprint!("redirect: cannot open '");
                print_bytes(file_trimmed);
                kprint!("'\n");
            }
            return;
        }
    }

    let mut wbuf = [0u8; 16];
    let (wn, wend) = cmd_word(line, &mut wbuf);
    let w = &wbuf[..wn];

    // Check for alias expansion (with recursion depth limit).
    static mut ALIAS_DEPTH: usize = 0;
    if let Some(expansion) = alias_find(w) {
        unsafe {
            if ALIAS_DEPTH >= 8 {
                kprint!("alias: too many expansions (circular?)\n");
                return;
            }
            ALIAS_DEPTH += 1;
        }
        // Build expanded line: expansion + rest_of(line).
        let args = rest_of(line, wend);
        let mut expanded = [0u8; MAX_LINE];
        let mut pos = 0;
        for &b in expansion {
            if pos < MAX_LINE { expanded[pos] = b; pos += 1; }
        }
        if !args.is_empty() && pos < MAX_LINE {
            expanded[pos] = b' '; pos += 1;
            for &b in args {
                if pos < MAX_LINE { expanded[pos] = b; pos += 1; }
            }
        }
        exec(&expanded[..pos]);
        unsafe { ALIAS_DEPTH -= 1; }
        return;
    }
    let args = rest_of(line, wend);

    match w {
        [] => {}
        b"help" => {
            if args.is_empty() {
                // List commands grouped by category.
                kprint!("=== System ===\n");
                for entry in HELP_DB {
                    let n = entry.0;
                    if n == b"help" || n == b"clear" || n == b"mem" || n == b"heap"
                        || n == b"ticks" || n == b"uptime" || n == b"sleep"
                        || n == b"tasks" || n == b"ps" || n == b"drivers" || n == b"smp"
                        || n == b"progs" || n == b"test" || n == b"off" || n == b"reboot"
                        || n == b"top" || n == b"date" || n == b"acpi" || n == b"ahci"
                        || n == b"usb" || n == b"ipi" || n == b"unittest"
                        || n == b"echo" || n == b"set" || n == b"unset" || n == b"env"
                        || n == b"alias" || n == b"df" || n == b"fslimit" {
                        kprint!("  {:12} {}\n", core::str::from_utf8(n).unwrap_or("?"), entry.2);
                    }
                }
                kprint!("=== Filesystem ===\n");
                for entry in HELP_DB {
                    let n = entry.0;
                    if n.starts_with(b"f") && n != b"fslimit" || n == b"mkfs" || n == b"fsinfo" || n == b"fsck" || n == b"mount" || n == b"umount" {
                        kprint!("  {:12} {}\n", core::str::from_utf8(n).unwrap_or("?"), entry.2);
                    }
                }
                kprint!("=== Networking ===\n");
                for entry in HELP_DB {
                    let n = entry.0;
                    if n == b"net" || n == b"ping" || n == b"netdump" || n == b"arp"
                        || n == b"nslookup" || n == b"tcpclose" || n == b"netstat"
                        || n == b"http" {
                        kprint!("  {:12} {}\n", core::str::from_utf8(n).unwrap_or("?"), entry.2);
                    }
                }
                kprint!("=== User Programs (u*) ===\n");
                for entry in HELP_DB {
                    let n = entry.0;
                    if n.starts_with(b"u") || n == b"exec" || n == b"kill" || n == b"hexdump"
                        || n == b"whoami" || n == b"calc" || n == b"guess" || n == b"edit"
                        || n == b"nano" || n == b"counter" || n == b"heapstress"
                        || n == b"heaptest" || n == b"isowrite" {
                        kprint!("  {:12} {}\n", core::str::from_utf8(n).unwrap_or("?"), entry.2);
                    }
                }
                kprint!("Use 'help <cmd>' for details.\n");
            } else {
                // Show help for a specific command.
                // Trim leading/trailing whitespace from args.
                let mut start = 0;
                let mut end = args.len();
                while start < end && (args[start] == b' ' || args[start] == b'\t') {
                    start += 1;
                }
                while end > start && (args[end-1] == b' ' || args[end-1] == b'\t') {
                    end -= 1;
                }
                let arg = &args[start..end];
                let arg_str = core::str::from_utf8(arg).unwrap_or("?");
                let mut found = false;
                for entry in HELP_DB {
                    if entry.0 == arg {
                        kprint!("{}: {}\n  {}\n",
                            core::str::from_utf8(entry.0).unwrap_or("?"),
                            entry.1, entry.2);
                        found = true;
                        break;
                    }
                }
                if !found {
                    kprint!("help: unknown command '{}'\n", arg_str);
                }
            }
        }
        b"clear" => {
            crate::console::clear();
        }
        b"mem" => {
            let (n, total, used, free) = mem::stats();
            kprint!(
                "mem: {} regions {} MiB usable, {} frames used, {} KiB recycled\n",
                n,
                total / (1 << 20),
                used,
                free / 1024
            );
        }
        b"heap" => {
            let (total, free, largest, holes) = heap::stats();
            kprint!(
                "heap: {} KiB total, {} KiB free, largest {} B in {} holes\n",
                total / 1024,
                free / 1024,
                largest,
                holes
            );
        }
        b"ticks" => {
            kprint!("ticks: {}\n", interrupts::ticks());
        }
        b"uptime" => {
            let ticks = interrupts::ticks();
            let total_secs = ticks / 100;
            let days = total_secs / 86400;
            let hours = (total_secs % 86400) / 3600;
            let mins = (total_secs % 3600) / 60;
            let secs = total_secs % 60;
            if days > 0 {
                kprint!("up {}d {}h {}m {}s ({} ticks)\n", days, hours, mins, secs, ticks);
            } else if hours > 0 {
                kprint!("up {}h {}m {}s ({} ticks)\n", hours, mins, secs, ticks);
            } else {
                kprint!("up {}m {}s ({} ticks)\n", mins, secs, ticks);
            }
            // Show load info.
            let mut ready = 0u32;
            for i in 0..crate::task::MAX_TASKS {
                if let Some(info) = crate::task::get_task_info(i) {
                    if info.state == 1 { ready += 1; }
                }
            }
            kprint!("tasks: {} running\n", ready);
        }
        b"tasks" | b"ps" => {
            crate::task::dump();
        }
        b"drivers" => {
            crate::driver::dump();
        }
        b"net" => {
            crate::virtio_net::dump();
        }
        b"sysinfo" => {
            kprint!("=== NEO-OS-3 System Info ===\n");
            kprint!("Kernel: v3.0.0\n");
            let (n, total, used, free) = mem::stats();
            kprint!("Memory: {} MiB usable, {} frames used\n", total / (1 << 20), used);
            kprint!("Uptime: {} ticks\n", interrupts::ticks());
            kprint!("Tasks: ");
            crate::task::dump();
            kprint!("Drivers: {} registered\n", crate::driver::count());
            kprint!("SMP: {} AP(s) online\n", crate::smp::ap_count());
            kprint!("Initrd: {} entries\n", crate::initrd::count());
            crate::virtio_net::dump();
        }
        b"ipconfig" => {
            // Show network configuration.
            let src_ip = [10, 0, 2, 15]; // QEMU default
            let dst_ip = [10, 0, 2, 2];  // QEMU gateway
            kprint!("ipconfig:\n");
            kprint!("  local IP: {}.{}.{}.{}\n", src_ip[0], src_ip[1], src_ip[2], src_ip[3]);
            kprint!("  gateway:  {}.{}.{}.{}\n", dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3]);
            // Test checksum calculation.
            let mut hdr = [0u8; 20];
            let csum = crate::ip::ipv4_checksum(&hdr);
            kprint!("  ip checksum test: {:#x}\n", csum);
        }
        b"udpecho" => {
            let cur = crate::virtio_net::echo_enabled();
            crate::virtio_net::set_echo(!cur);
            kprint!("udpecho: {}\n", if !cur { "enabled" } else { "disabled" });
        }
        b"tcp" => {
            if crate::tcp::is_established() {
                kprint!("tcp: connection established\n");
            } else {
                // Try to connect to QEMU gateway (10.0.2.2) port 80.
                let dst_ip = [10, 0, 2, 2];
                let dst_port = 80;
                let src_port = 12345;
                kprint!("tcp: connecting to {}.{}.{}.{}:{}...\n",
                    dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3], dst_port);
                if crate::tcp::connect(dst_ip, dst_port, src_port) {
                    kprint!("tcp: SYN sent\n");
                } else {
                    kprint!("tcp: connect failed\n");
                }
            }
        }
        _ if line.starts_with(b"write ") => {
            // write <name> <content>: write text to an initrd file.
            let rest = &line[6..];
            let mut parts = rest.split(|&b| b == b' ' || b == b'\t');
            let name = parts.next().unwrap_or(b"");
            let content_start = rest.iter().position(|&b| b == b' ' || b == b'\t')
                .map(|p| p + 1).unwrap_or(rest.len());
            let content = &rest[content_start..];
            if name.is_empty() || content.is_empty() {
                kprint!("usage: write <name> <content>\n");
            } else {
                if crate::initrd::write(name, content) {
                    kprint!("write: {} updated ({} bytes)\n", core::str::from_utf8(name).unwrap_or("?"), content.len());
                } else {
                    kprint!("write: {} failed (not found or too large)\n", core::str::from_utf8(name).unwrap_or("?"));
                }
            }
        }
        b"wc" => {
            // wc <name>: count lines, words, chars in an initrd file.
            // wc <name>: count lines, words, chars in an initrd file.
            // Trim leading/trailing whitespace from args.
            let mut start = 0;
            let mut end = args.len();
            while start < end && (args[start] == b' ' || args[start] == b'\t') {
                start += 1;
            }
            while end > start && (args[end-1] == b' ' || args[end-1] == b'\t') {
                end -= 1;
            }
            let name = &args[start..end];
            if name.is_empty() {
                kprint!("usage: wc <name>\n");
            } else {
                match crate::initrd::find(name) {
                    Some(idx) => {
                        if let Some(data) = crate::initrd::read(idx) {
                            let mut lines = 0;
                            let mut words = 0;
                            let mut chars = 0;
                            let mut in_word = false;
                            for &b in data {
                                chars += 1;
                                if b == b'\n' {
                                    lines += 1;
                                }
                                if b == b' ' || b == b'\t' || b == b'\n' || b == b'\r' {
                                    if in_word {
                                        words += 1;
                                        in_word = false;
                                    }
                                } else {
                                    in_word = true;
                                }
                            }
                            if in_word {
                                words += 1;
                            }
                            kprint!("wc: {} lines {} words {} chars\n", lines, words, chars);
                        }
                    }
                    None => kprint!("wc: {} not found\n", core::str::from_utf8(name).unwrap_or("?")),
                }
            }
        }
        b"grep" => {
            // grep <pattern> <file>: search for pattern in initrd file.
            // grep <pattern> <file>: search for pattern in initrd file.
            let mut parts = args.split(|&b| b == b' ' || b == b'\t');
            let pattern = parts.next().unwrap_or(b"");
            let file = parts.next().unwrap_or(b"");
            if pattern.is_empty() || file.is_empty() {
                kprint!("usage: grep <pattern> <file>\n");
            } else {
                match crate::initrd::find(file) {
                    Some(idx) => {
                        if let Some(data) = crate::initrd::read(idx) {
                            let mut found = false;
                            for (i, &b) in data.iter().enumerate() {
                                if b == pattern[0] {
                                    // Check if pattern matches at this position.
                                    let mut matches = true;
                                    for (j, &pb) in pattern.iter().enumerate() {
                                        if i + j >= data.len() || data[i + j] != pb {
                                            matches = false;
                                            break;
                                        }
                                    }
                                    if matches {
                                        found = true;
                                        // Print the line containing the match.
                                        let mut line_start = i;
                                        while line_start > 0 && data[line_start - 1] != b'\n' {
                                            line_start -= 1;
                                        }
                                        let mut line_end = i;
                                        while line_end < data.len() && data[line_end] != b'\n' {
                                            line_end += 1;
                                        }
                                        let line = &data[line_start..line_end];
                                        kprint!("grep: ");
                                        for &c in line {
                                            kprint!("{}", c as char);
                                        }
                                        kprint!("\n");
                                        break;
                                    }
                                }
                            }
                            if !found {
                                kprint!("grep: pattern not found\n");
                            }
                        }
                    }
                    None => kprint!("grep: {} not found\n", core::str::from_utf8(file).unwrap_or("?")),
                }
            }
        }
        b"sort" => {
            // sort <name>: sort lines in an initrd file alphabetically.
            let mut start = 0;
            let mut end = args.len();
            while start < end && (args[start] == b' ' || args[start] == b'\t') {
                start += 1;
            }
            while end > start && (args[end-1] == b' ' || args[end-1] == b'\t') {
                end -= 1;
            }
            let name = &args[start..end];
            if name.is_empty() {
                kprint!("usage: sort <name>\n");
            } else {
                match crate::initrd::find(name) {
                    Some(idx) => {
                        if let Some(data) = crate::initrd::read(idx) {
                            // Split into lines.
                            let mut lines: [Option<&[u8]>; 32] = [None; 32];
                            let mut nlines = 0;
                            let mut start = 0;
                            for (i, &b) in data.iter().enumerate() {
                                if b == b'\n' || i == data.len() - 1 {
                                    let end = if b == b'\n' { i } else { i + 1 };
                                    if nlines < 32 && end > start {
                                        lines[nlines] = Some(&data[start..end]);
                                        nlines += 1;
                                    }
                                    start = i + 1;
                                }
                            }
                            // Simple bubble sort (small file, few lines).
                            for i in 0..nlines {
                                for j in (i + 1)..nlines {
                                    if let (Some(a), Some(b)) = (lines[i], lines[j]) {
                                        if a > b {
                                            lines.swap(i, j);
                                        }
                                    }
                                }
                            }
                            // Print sorted lines.
                            for i in 0..nlines {
                                if let Some(line) = lines[i] {
                                    for &c in line {
                                        kprint!("{}", c as char);
                                    }
                                    kprint!("\n");
                                }
                            }
                        }
                    }
                    None => kprint!("sort: {} not found\n", core::str::from_utf8(name).unwrap_or("?")),
                }
            }
        }
        b"smp" => {
            let n = crate::smp::ap_count();
            kprint!("smp: {} AP(s) online, ready={}\n", n, crate::smp::is_ready());
        }
        b"user" => match crate::task::spawn_user(0) {
            Some(id) => kprint!("spawned ring3 task {}\n", id),
            None => kprint!("task table full\n"),
        },
        b"userbad" => match crate::task::spawn_user(1) {
            Some(id) => kprint!("spawned ring3 task {} (reads kernel mem)\n", id),
            None => kprint!("task table full\n"),
        },
        b"usernx" => match crate::task::spawn_user(2) {
            Some(id) => kprint!("spawned ring3 task {} (executes stack)\n", id),
            None => kprint!("task table full\n"),
        },
        b"userexec" => match crate::task::spawn_user(3) {
            Some(id) => kprint!("spawned ring3 task {} (executes kernel text)\n", id),
            None => kprint!("task table full\n"),
        },
        b"userptr" => match crate::task::spawn_user(4) {
            Some(id) => kprint!("spawned ring3 task {} (syscall pointer checks)\n", id),
            None => kprint!("task table full\n"),
        },
        b"isowrite" => match crate::task::spawn_user(5) {
            Some(id) => kprint!("spawned ring3 task {} (write isolation)\n", id),
            None => kprint!("task table full\n"),
        },
        b"heaptest" => match crate::task::spawn_user(6) {
            Some(id) => kprint!("spawned ring3 task {} (heap test)\n", id),
            None => kprint!("task table full\n"),
        },
        b"echo2" => match crate::task::spawn_user(7) {
            Some(id) => kprint!("spawned ring3 task {} (echo)\n", id),
            None => kprint!("task table full\n"),
        },
        b"meminfo" => match crate::task::spawn_user(9) {
            Some(id) => kprint!("spawned ring3 task {} (meminfo)\n", id),
            None => kprint!("task table full\n"),
        },
        b"counter" => match crate::task::spawn_user(10) {
            Some(id) => kprint!("spawned ring3 task {} (counter)\n", id),
            None => kprint!("task table full\n"),
        },
        b"guess" => match crate::task::spawn_user(11) {
            Some(id) => kprint!("spawned ring3 task {} (guess game, press 1-9/q)\n", id),
            None => kprint!("task table full\n"),
        },
        b"edit" => match crate::task::spawn_user(12) {
            Some(id) => kprint!("spawned ring3 task {} (line editor, Esc=quit)\n", id),
            None => kprint!("task table full\n"),
        },
        b"calc" => match crate::task::spawn_user(13) {
            Some(id) => kprint!("spawned ring3 task {} (calculator)\n", id),
            None => kprint!("task table full\n"),
        },
        b"heapstress" => match crate::task::spawn_user(14) {
            Some(id) => kprint!("spawned ring3 task {} (heap stress)\n", id),
            None => kprint!("task table full\n"),
        },
        b"ping" => match crate::task::spawn_user(15) {
            Some(id) => kprint!("spawned ring3 task {} (ping)\n", id),
            None => kprint!("task table full\n"),
        },
        b"netdump" => match crate::task::spawn_user(16) {
            Some(id) => kprint!("spawned ring3 task {} (netdump, any key=quit)\n", id),
            None => kprint!("task table full\n"),
        },
        b"time" => match crate::task::spawn_user(17) {
            Some(id) => kprint!("spawned ring3 task {} (time)\n", id),
            None => kprint!("task table full\n"),
        },
        b"nano" => match crate::task::spawn_user(18) {
            Some(id) => kprint!("spawned ring3 task {} (nano editor, Esc=quit)\n", id),
            None => kprint!("task table full\n"),
        },
        b"http" => match crate::task::spawn_user(19) {
            Some(id) => kprint!("spawned ring3 task {} (http demo)\n", id),
            None => kprint!("task table full\n"),
        },
        b"progs" => {
            let bi = unsafe { &crate::task::bi_ref() };
            if bi.nuser_progs == 0 {
                kprint!("no user programs loaded\n");
            } else {
                kprint!("loaded programs:\n");
                for i in 0..bi.nuser_progs as usize {
                    let p = &bi.user_progs[i];
                    let name_end = p.name.iter().position(|&b| b == 0).unwrap_or(16);
                    let name = core::str::from_utf8(&p.name[..name_end]).unwrap_or("?");
                    kprint!("  {} ({} bytes @ {:#x})\n", name, p.elf_len, p.elf_base);
                }
            }
        }
        b"ls" => {
            let n = crate::initrd::count();
            if n == 0 {
                kprint!("no initrd loaded\n");
            } else {
                kprint!("/bin/ ({} files):\n", n);
                for i in 0..n {
                    if let Some(e) = crate::initrd::get(i) {
                        let data = crate::initrd::read(i).unwrap_or(&[]);
                        let name_data = unsafe {
                            core::slice::from_raw_parts(
                                (crate::task::bi_ref().initrd_base + e.name_offset as u64) as *const u8,
                                e.name_len
                            )
                        };
                        let name = core::str::from_utf8(name_data).unwrap_or("?");
                        kprint!("  {:16} {} bytes\n", name, data.len());
                    }
                }
            }
        }
        _ if line.starts_with(b"cat ") => {
            let name = &line[4..];
            let name = name.split(|&b| b == b' ' || b == b'\t').next().unwrap_or(b"");
            match crate::initrd::find(name) {
                Some(idx) => {
                    if let Some(data) = crate::initrd::read(idx) {
                        // Print first 512 bytes as hex dump.
                        let dump_len = core::cmp::min(data.len(), 512);
                        kprint!("{}: {} bytes (showing {})\n",
                            core::str::from_utf8(name).unwrap_or("?"), data.len(), dump_len);
                        for off in (0..dump_len).step_by(16) {
                            kprint!("{:04x}: ", off);
                            for j in 0..16 {
                                if off + j < dump_len {
                                    kprint!("{:02x} ", data[off + j]);
                                } else {
                                    kprint!("   ");
                                }
                            }
                            kprint!("\n");
                        }
                    }
                }
                None => kprint!("cat: {} not found\n", core::str::from_utf8(name).unwrap_or("?")),
            }
        }
        b"gui" => {
            crate::gui::enter();
        }
        _ if line.starts_with(b"exec ") => {
            let name = &line[5..];
            // Trim trailing whitespace.
            let name = name.split(|&b| b == b' ' || b == b'\t').next().unwrap_or(b"");
            if name.is_empty() {
                kprint!("usage: exec <program>\n");
            } else {
                match crate::task::spawn_user_by_name(name) {
                    Some(id) => kprint!("exec: spawned task {} ({})\n", id, core::str::from_utf8(name).unwrap_or("?")),
                    None => kprint!("exec: program not found\n"),
                }
            }
        }
        b"mouse" => {
            let (x, y, btn, seq) = crate::mouse::sample();
            kprint!("mouse: x={} y={} btn={:#05b} seq={}\n", x, y, btn, seq);
        }
        b"kbddebug" => {
            // Toggles the raw-scancode serial trace (on by default).
            let on = !crate::keyboard::tracing();
            crate::keyboard::set_trace(on);
            kprint!("kbd trace {}\n", if on { "on" } else { "off" });
        }
        b"echo" => {
            // echo [-n] [-e] <text> — print to console
            let mut no_newline = false;
            let mut escape = false;
            let mut text = args;
            // Parse flags.
            loop {
                if text.starts_with(b"-n ") { no_newline = true; text = &text[3..]; }
                else if text.starts_with(b"-e ") { escape = true; text = &text[3..]; }
                else if text == b"-n" { no_newline = true; text = b""; break; }
                else if text == b"-e" { escape = true; text = b""; break; }
                else { break; }
            }
            if escape {
                // Process escape sequences: \n, \t, \\, \0.
                let mut i = 0;
                while i < text.len() {
                    if text[i] == b'\\' && i + 1 < text.len() {
                        match text[i + 1] {
                            b'n' => kprint!("\n"),
                            b't' => kprint!("\t"),
                            b'\\' => kprint!("\\"),
                            b'0' => kprint!("\0"),
                            other => { kprint!("\\{}", other as char); }
                        }
                        i += 2;
                    } else {
                        kprint!("{}", text[i] as char);
                        i += 1;
                    }
                }
            } else {
                print_bytes(text);
            }
            if !no_newline {
                kprint!("\n");
            }
        }
        b"sleep" => match parse_u64(args) {
            Some(ms) => {
                interrupts::sleep_ms(ms);
                kprint!("slept {} ms ({} ticks)\n", ms, interrupts::ticks());
            }
            None => kprint!("usage: sleep <ms>\n"),
        },
        b"test" => {
            // Non-fatal self-tests only (faults halt by design, not here).
            let b = alloc::boxed::Box::new(0x54455354u32);
            kprint!("test: box={:#x}\n", *b);
            drop(b);
            match crate::task::spawn(crate::task::demo_once) {
                Some(id) => kprint!("test: spawned one-shot task {}\n", id),
                None => kprint!("test: task table full\n"),
            }
            kprint!("test: breakpoint (EXC line below = ok)\n");
            unsafe { core::arch::asm!("int3") };
            kprint!("test: pass\n");
        }
        b"blk" => {
            crate::virtio_blk::dump();
        }
        b"blktest" => {
            if !crate::virtio_blk::available() {
                kprint!("blktest: no block device\n");
            } else {
                let cap = crate::virtio_blk::capacity();
                kprint!("blktest: disk capacity={} sectors\n", cap);
                // Write a test pattern to sector 0.
                let mut buf = [0u8; 512];
                for i in 0..512 { buf[i] = (i & 0xFF) as u8; }
                match crate::virtio_blk::write_sectors(0, &buf) {
                    Ok(()) => kprint!("blktest: write sector 0 OK\n"),
                    Err(e) => { kprint!("blktest: write failed: {}\n", e); }
                }
                // Read it back.
                let mut rbuf = [0u8; 512];
                match crate::virtio_blk::read_sectors(0, &mut rbuf) {
                    Ok(()) => {
                        if rbuf == buf {
                            kprint!("blktest: read+verify OK\n");
                        } else {
                            kprint!("blktest: MISMATCH!\n");
                        }
                    }
                    Err(e) => { kprint!("blktest: read failed: {}\n", e); }
                }
            }
        }
        b"mkfs" => {
            // mkfs requires unmounting first, then formatting, then remounting.
            match crate::neofs::mkfs() {
                Ok(()) => {
                    // Re-mount after format.
                    let _ = crate::neofs::mount(0);
                    kprint!("mkfs: NEO-FS created and mounted\n");
                }
                Err(e) => kprint!("mkfs: {}\n", e),
            }
        }
        b"fsinfo" => {
            crate::neofs::fs_info();
        }
        b"fls" => {
            if !crate::neofs::is_mounted() {
                kprint!("fls: filesystem not mounted\n");
            } else {
                let pattern = trim_bytes(args);
                let long_format = pattern.starts_with(b"-l");
                let glob_pat = if long_format { trim_bytes(&pattern[2..]) } else { pattern };
                kprint!("/\n");
                crate::vfs::vfs_readdir(&mut |name, mode, size| {
                    if !glob_pat.is_empty() && !glob_match(glob_pat, name) {
                        return;
                    }
                    let type_char = if (mode & 0xF) == 2 { b'd' } else { b'-' };
                    let mut perms = [b'-'; 9];
                    crate::neofs::format_perms_pub(mode, &mut perms);
                    let name_str = core::str::from_utf8(name).unwrap_or("???");
                    if long_format {
                        // Long format: type+perms size name
                        kprint!("{}{} {:>6} {}\n", type_char as char,
                                core::str::from_utf8(&perms).unwrap_or("---------"),
                                size, name_str);
                    } else {
                        // Short format: just name with type indicator
                        let suffix = if (mode & 0xF) == 2 { "/" } else { "" };
                        kprint!("  {}{}\n", name_str, suffix);
                    }
                });
            }
        }
        _ if line.starts_with(b"fcat ") => {
            let name = trim_bytes(&line[5..]);
            if !crate::neofs::is_mounted() {
                kprint!("fcat: filesystem not mounted\n");
            } else {
                // Use VFS open/read/close.
                let fd = crate::vfs::vfs_open(name, 0, 0); // read-only, task_id=0 (shell)
                if fd < 0 {
                    kprint!("fcat: cannot open '");
                    print_bytes(name);
                    kprint!("'\n");
                } else {
                    let mut buf = [0u8; 512];
                    let n = crate::vfs::vfs_read(fd as u32, &mut buf, 0);
                    if n > 0 {
                        for i in 0..n as usize {
                            let b = buf[i];
                            if b >= 0x20 && b < 0x7f {
                                let ch = [b];
                                kprint!("{}", core::str::from_utf8(&ch).unwrap_or("?"));
                            } else if b == b'\n' {
                                kprint!("\n");
                            }
                        }
                        kprint!("\n");
                    }
                    crate::vfs::vfs_close(fd as u32, 0);
                }
            }
        }
        _ if line.starts_with(b"fwrite ") => {
            let rest = &line[7..];
            let mut split = rest.splitn(2, |&b| b == b' ');
            let name = split.next().unwrap_or(b"");
            let content = split.next().unwrap_or(b"");
            if name.is_empty() {
                kprint!("fwrite: usage: fwrite <name> <text>\n");
            } else if !crate::neofs::is_mounted() {
                kprint!("fwrite: filesystem not mounted\n");
            } else {
                // Use VFS open/write/close.
                let fd = crate::vfs::vfs_open(name, 1, 0); // write (create/trunc)
                if fd < 0 {
                    kprint!("fwrite: cannot create '");
                    print_bytes(name);
                    kprint!("'\n");
                } else {
                    let n = crate::vfs::vfs_write(fd as u32, content, 0);
                    if n >= 0 {
                        kprint!("fwrite: wrote {} bytes to '", n);
                        print_bytes(name);
                        kprint!("'\n");
                    } else {
                        kprint!("fwrite: write error\n");
                    }
                    crate::vfs::vfs_close(fd as u32, 0);
                }
            }
        }
        _ if line.starts_with(b"frm ") => {
            let name = trim_bytes(&line[4..]);
            match crate::neofs::remove_file(name) {
                Ok(()) => { kprint!("frm: removed '"); print_bytes(name); kprint!("'\n"); }
                Err(e) => kprint!("frm: {}\n", e),
            }
        }
        _ if line.starts_with(b"fmkdir ") => {
            let name = trim_bytes(&line[7..]);
            if name.is_empty() {
                kprint!("fmkdir: usage: fmkdir <name>\n");
            } else {
                match crate::neofs::create_dir(name, 0) {
                    Some(_) => { kprint!("fmkdir: created '"); print_bytes(name); kprint!("'\n"); }
                    None => kprint!("fmkdir: failed (exists or no space)\n"),
                }
            }
        }
        _ if line.starts_with(b"frmdir ") => {
            let name = trim_bytes(&line[7..]);
            if name.is_empty() {
                kprint!("frmdir: usage: frmdir <name>\n");
            } else {
                match crate::neofs::remove_dir(name) {
                    Ok(()) => { kprint!("frmdir: removed '"); print_bytes(name); kprint!("'\n"); }
                    Err(e) => kprint!("frmdir: {}\n", e),
                }
            }
        }
        _ if line.starts_with(b"fmv ") => {
            let rest = &line[4..];
            let mut split = rest.splitn(2, |&b| b == b' ');
            let old_name = trim_bytes(split.next().unwrap_or(b""));
            let new_name = trim_bytes(split.next().unwrap_or(b""));
            if old_name.is_empty() || new_name.is_empty() {
                kprint!("fmv: usage: fmv <old> <new>\n");
            } else {
                match crate::neofs::rename(old_name, new_name) {
                    Ok(()) => {
                        kprint!("fmv: '");
                        print_bytes(old_name);
                        kprint!("' -> '");
                        print_bytes(new_name);
                        kprint!("'\n");
                    }
                    Err(e) => kprint!("fmv: {}\n", e),
                }
            }
        }
        b"fsck" => {
            crate::neofs::fsck();
        }
        b"unittest" => {
            let (p, f) = crate::test::run_all();
            if f > 0 {
                kprint!("unittest: {} FAILED\n", f);
            } else {
                kprint!("unittest: all {} passed\n", p);
            }
        }
        _ if line.starts_with(b"set ") || line.starts_with(b"export ") => {
            let prefix_len = if line.starts_with(b"export ") { 7 } else { 4 };
            let rest = trim_bytes(&line[prefix_len..]);
            // Parse KEY=VALUE.
            if let Some(eq_pos) = rest.iter().position(|&b| b == b'=') {
                let key = &rest[..eq_pos];
                let value = &rest[eq_pos + 1..];
                if key.is_empty() {
                    kprint!("set: empty key\n");
                } else if env_set(key, value) {
                    kprint!("set: ");
                    print_bytes(rest);
                    kprint!("\n");
                } else {
                    kprint!("set: too many variables or value too long\n");
                }
            } else {
                kprint!("set: usage: set KEY=VALUE\n");
            }
        }
        _ if line.starts_with(b"unset ") => {
            let key = trim_bytes(&line[6..]);
            if key.is_empty() {
                kprint!("unset: usage: unset KEY\n");
            } else if env_unset(key) {
                kprint!("unset: ");
                print_bytes(key);
                kprint!("\n");
            } else {
                kprint!("unset: '");
                print_bytes(key);
                kprint!("' not found\n");
            }
        }
        b"env" => {
            unsafe {
                if ENV_COUNT == 0 {
                    kprint!("(no environment variables)\n");
                } else {
                    for i in 0..ENV_COUNT {
                        let entry = &ENV_VARS[i];
                        let len = entry.iter().position(|&b| b == 0).unwrap_or(MAX_ENV_LEN);
                        for &b in &entry[..len] {
                            kprint!("{}", b as char);
                        }
                        kprint!("\n");
                    }
                }
            }
        }
        _ if line.starts_with(b"alias ") => {
            let rest = trim_bytes(&line[6..]);
            if rest.is_empty() {
                // List all aliases.
                alias_list();
            } else if let Some(eq_pos) = rest.iter().position(|&b| b == b'=') {
                let name = &rest[..eq_pos];
                let expansion = &rest[eq_pos + 1..];
                if name.is_empty() {
                    kprint!("alias: empty name\n");
                } else if alias_set(name, expansion) {
                    kprint!("alias: ");
                    print_bytes(rest);
                    kprint!("\n");
                } else {
                    kprint!("alias: too many aliases or too long\n");
                }
            } else {
                // No '=' — show existing alias.
                if let Some(exp) = alias_find(rest) {
                    print_bytes(rest);
                    kprint!("='");
                    for &b in exp { kprint!("{}", b as char); }
                    kprint!("'\n");
                } else {
                    kprint!("alias: '");
                    print_bytes(rest);
                    kprint!("' not found\n");
                }
            }
        }
        b"alias" => {
            // List all aliases.
            alias_list();
        }
        _ if line.starts_with(b"unalias ") => {
            let name = trim_bytes(&line[8..]);
            if name.is_empty() {
                kprint!("unalias: usage: unalias <name>\n");
            } else if alias_remove(name) {
                kprint!("unalias: removed '");
                print_bytes(name);
                kprint!("'\n");
            } else {
                kprint!("unalias: '");
                print_bytes(name);
                kprint!("' not found\n");
            }
        }
        b"history" => {
            unsafe {
                if NHISTORY == 0 {
                    kprint!("(no history)\n");
                } else {
                    // Show oldest first (NHISTORY-1 is oldest, 0 is newest).
                    for i in (0..NHISTORY).rev() {
                        let num = NHISTORY - i;
                        let len = HISTORY_LENS[i];
                        kprint!("  {:>3}  ", num);
                        for j in 0..len {
                            let b = HISTORY[i][j];
                            if b >= 0x20 && b < 0x7f {
                                kprint!("{}", b as char);
                            }
                        }
                        kprint!("\n");
                    }
                }
            }
        }
        _ if line.starts_with(b"source ") => {
            let fname = trim_bytes(&line[7..]);
            if fname.is_empty() {
                kprint!("source: usage: source <file>\n");
            } else {
                let fd = crate::vfs::vfs_open(fname, 0, 0);
                if fd < 0 {
                    kprint!("source: cannot open '");
                    print_bytes(fname);
                    kprint!("'\n");
                } else {
                    let mut buf = [0u8; 512];
                    let n = crate::vfs::vfs_read(fd as u32, &mut buf, 0);
                    crate::vfs::vfs_close(fd as u32, 0);
                    if n <= 0 {
                        kprint!("source: empty file\n");
                    } else {
                        // Execute each line.
                        let data = &buf[..n as usize];
                        let mut line_start = 0;
                        for i in 0..data.len() {
                            if data[i] == b'\n' || i == data.len() - 1 {
                                let end = if data[i] == b'\n' { i } else { i + 1 };
                                let script_line = trim_bytes(&data[line_start..end]);
                                if !script_line.is_empty() && script_line[0] != b'#' {
                                    run_script_line(script_line);
                                }
                                line_start = i + 1;
                            }
                        }
                        kprint!("source: executed ");
                        print_bytes(fname);
                        kprint!("\n");
                    }
                }
            }
        }
        _ if line.starts_with(b"type ") => {
            let cmd = trim_bytes(&line[5..]);
            if cmd.is_empty() {
                kprint!("type: usage: type <command>\n");
            } else {
                // Check alias first.
                if alias_find(cmd).is_some() {
                    print_bytes(cmd);
                    kprint!(" is an alias\n");
                } else {
                    // Check if it's a builtin (in HELP_DB).
                    let mut is_builtin = false;
                    for entry in HELP_DB {
                        if entry.0 == cmd {
                            is_builtin = true;
                            break;
                        }
                    }
                    if is_builtin {
                        print_bytes(cmd);
                        kprint!(" is a shell builtin\n");
                    } else if crate::task::find_prog_by_name(cmd) {
                        print_bytes(cmd);
                        kprint!(" is /bin/");
                        print_bytes(cmd);
                        kprint!("\n");
                    } else {
                        kprint!("type: ");
                        print_bytes(cmd);
                        kprint!(" not found\n");
                    }
                }
            }
        }
        _ if line.starts_with(b"which ") => {
            let cmd = trim_bytes(&line[6..]);
            if cmd.is_empty() {
                kprint!("which: usage: which <command>\n");
            } else if alias_find(cmd).is_some() {
                kprint!("alias: ");
                print_bytes(cmd);
                kprint!("\n");
            } else {
                let mut is_builtin = false;
                for entry in HELP_DB {
                    if entry.0 == cmd { is_builtin = true; break; }
                }
                if is_builtin {
                    print_bytes(cmd);
                    kprint!(": shell builtin\n");
                } else if crate::task::find_prog_by_name(cmd) {
                    kprint!("/bin/");
                    print_bytes(cmd);
                    kprint!("\n");
                } else {
                    kprint!("which: no ");
                    print_bytes(cmd);
                    kprint!(" in (/bin)\n");
                }
            }
        }
        b"exectest" => match crate::task::spawn_user_by_name(b"exectest") {
            Some(_) => { kprint!("spawned ring3 task (exec test)\n"); }
            None => kprint!("exectest: not found\n"),
        },
        b"forktest" => match crate::task::spawn_user_by_name(b"forktest") {
            Some(_) => { kprint!("spawned ring3 task (fork test)\n"); }
            None => kprint!("forktest: not found\n"),
        },
        b"fstest" => match crate::task::spawn_user_by_name(b"fstest") {
            Some(id) => kprint!("spawned ring3 task {} (VFS test)\n", id),
            None => kprint!("fstest: program not found\n"),
        },
        b"uls" => match crate::task::spawn_user_by_name(b"ls") {
            Some(id) => kprint!("spawned ring3 task {} (ls)\n", id),
            None => kprint!("uls: program not found\n"),
        },
        b"ucat" => match crate::task::spawn_user_by_name(b"cat") {
            Some(id) => kprint!("spawned ring3 task {} (cat)\n", id),
            None => kprint!("ucat: program not found\n"),
        },
        _ if line.starts_with(b"hexdump ") => {
            let fname = trim_bytes(&line[8..]);
            if fname.is_empty() {
                kprint!("hexdump: usage: hexdump <filename>\n");
            } else {
                let fd = crate::vfs::vfs_open(fname, 0, 0);
                if fd < 0 {
                    kprint!("hexdump: cannot open '");
                    print_bytes(fname);
                    kprint!("'\n");
                } else {
                    let mut buf = [0u8; 256];
                    let n = crate::vfs::vfs_read(fd as u32, &mut buf, 0);
                    crate::vfs::vfs_close(fd as u32, 0);
                    if n <= 0 {
                        kprint!("hexdump: empty file\n");
                    } else {
                        let len = n as usize;
                        let mut off = 0usize;
                        while off < len {
                            kprint!("{:04x}  ", off);
                            let end = core::cmp::min(off + 16, len);
                            for i in off..off + 16 {
                                if i < end {
                                    kprint!("{:02x}", buf[i]);
                                } else {
                                    kprint!("  ");
                                }
                                if i % 2 == 1 { kprint!(" "); }
                            }
                            kprint!(" |");
                            for i in off..end {
                                let b = buf[i];
                                if b >= 0x20 && b < 0x7f {
                                    kprint!("{}", b as char);
                                } else {
                                    kprint!(".");
                                }
                            }
                            kprint!("|\n");
                            off += 16;
                        }
                        kprint!("{} bytes\n", len);
                    }
                }
            }
        }
        b"hexdump" => match crate::task::spawn_user_by_name(b"hexdump") {
            Some(id) => kprint!("spawned ring3 task {} (hexdump)\n", id),
            None => kprint!("hexdump: program not found\n"),
        },
        b"whoami" => match crate::task::spawn_user_by_name(b"whoami") {
            Some(id) => kprint!("spawned ring3 task {} (whoami)\n", id),
            None => kprint!("whoami: program not found\n"),
        },
        b"ugrep" => match crate::task::spawn_user_by_name(b"grep") {
            Some(id) => kprint!("spawned ring3 task {} (grep)\n", id),
            None => kprint!("ugrep: program not found\n"),
        },
        b"ups" => match crate::task::spawn_user_by_name(b"ps") {
            Some(id) => kprint!("spawned ring3 task {} (ps)\n", id),
            None => kprint!("ups: program not found\n"),
        },
        b"udd" => match crate::task::spawn_user_by_name(b"dd") {
            Some(id) => kprint!("spawned ring3 task {} (dd)\n", id),
            None => kprint!("udd: program not found\n"),
        },
        _ if line.starts_with(b"kill ") => {
            let rest = trim_bytes(&line[5..]);
            // Parse PID as decimal.
            let mut pid: u64 = 0;
            let mut valid = false;
            for &b in rest.iter() {
                if b >= b'0' && b <= b'9' {
                    pid = pid * 10 + (b - b'0') as u64;
                    valid = true;
                } else { break; }
            }
            if !valid {
                kprint!("kill: usage: kill <pid>\n");
            } else {
                let ret = crate::task::kill_task(pid, 15);
                match ret {
                    0 => kprint!("kill: sent SIGTERM to {}\n", pid),
                    -1 => kprint!("kill: task {} not found\n", pid),
                    _ => kprint!("kill: unsupported signal\n"),
                }
            }
        }
        b"mount" => {
            if crate::neofs::is_mounted() {
                kprint!("mount: already mounted\n");
            } else if crate::block::count() == 0 {
                kprint!("mount: no block devices\n");
            } else {
                match crate::neofs::mount(0) {
                    Ok(()) => kprint!("mount: NEO-FS mounted on dev 0\n"),
                    Err(e) => kprint!("mount: {}\n", e),
                }
            }
        }
        b"umount" => {
            if !crate::neofs::is_mounted() {
                kprint!("umount: not mounted\n");
            } else {
                // Clear mount device to unmount.
                crate::neofs::unmount();
                kprint!("umount: NEO-FS unmounted\n");
            }
        }
        b"df" => {
            match crate::neofs::disk_stats() {
                Some(stats) => {
                    kprint!("Filesystem  Blocks  Used  Free  Inodes  IUsed  IFree\n");
                    kprint!("NEO-FS      {:>6}  {:>4}  {:>4}  {:>6}  {:>5}  {:>5}\n",
                           stats.total_blocks, stats.used_blocks, stats.free_blocks,
                           stats.total_inodes, stats.used_inodes, stats.free_inodes);
                    let used_pct = if stats.total_blocks > 0 {
                        (stats.used_blocks as u64 * 100) / stats.total_blocks as u64
                    } else { 0 };
                    kprint!("Block usage: {}%\n", used_pct);
                }
                None => kprint!("df: filesystem not mounted\n"),
            }
        }
        b"fslimit" => {
            kprint!("NEO-FS Limits:\n");
            kprint!("  Max file size:   {} bytes ({} KB)\n",
                    crate::neofs::MAX_FILE_SIZE, crate::neofs::MAX_FILE_SIZE / 1024);
            kprint!("  Direct blocks:   {} ({} bytes)\n",
                    crate::neofs::DIRECT_BLOCKS, crate::neofs::DIRECT_BLOCKS * 512);
            kprint!("  Indirect ptrs:   {} ({} bytes)\n",
                    crate::neofs::INDIRECT_PTRS, crate::neofs::INDIRECT_PTRS * 512);
            kprint!("  Max inodes:      {}\n", crate::neofs::MAX_INODES);
            kprint!("  Max name len:    {} chars\n", crate::neofs::MAX_NAME_LEN);
            kprint!("  Sector size:     {} bytes\n", crate::neofs::SECTOR_SIZE);
        }
        _ if line.starts_with(b"fwc ") => {
            let name = trim_bytes(&line[4..]);
            if name.is_empty() {
                kprint!("fwc: usage: fwc <filename>\n");
            } else {
                let fd = crate::vfs::vfs_open(name, 0, 0);
                if fd < 0 {
                    kprint!("fwc: cannot open '");
                    print_bytes(name);
                    kprint!("'\n");
                } else {
                    let mut buf = [0u8; 512];
                    let n = crate::vfs::vfs_read(fd as u32, &mut buf, 0);
                    crate::vfs::vfs_close(fd as u32, 0);
                    if n <= 0 {
                        kprint!("0 0 0 ");
                        print_bytes(name);
                        kprint!("\n");
                    } else {
                        let data = &buf[..n as usize];
                        let mut lines = 1u32;
                        let mut words = 0u32;
                        let mut in_word = false;
                        for &b in data.iter() {
                            if b == b'\n' { lines += 1; in_word = false; }
                            else if b == b' ' || b == b'\t' || b == b'\r' { in_word = false; }
                            else if !in_word { words += 1; in_word = true; }
                        }
                        kprint!("{} {} {} ", lines, words, n);
                        print_bytes(name);
                        kprint!("\n");
                    }
                }
            }
        }
        _ if line.starts_with(b"uexec ") => {
            let name = trim_bytes(&line[6..]);
            if name.is_empty() {
                kprint!("uexec: usage: uexec <name>\n");
            } else {
                kprint!("uexec: replacing image with '");
                print_bytes(name);
                kprint!("'...\n");
                if !crate::task::exec_current(name) {
                    kprint!("uexec: failed (not found or load error)\n");
                }
                // If exec succeeded, we won't reach here (new image replaces us).
                kprint!("uexec: WARNING: exec returned but should have replaced image\n");
            }
        }
        b"ukill" => match crate::task::spawn_user_by_name(b"kill") {
            Some(id) => kprint!("spawned ring3 task {} (kill)\n", id),
            None => kprint!("ukill: program not found\n"),
        },
        b"ufileops" => match crate::task::spawn_user_by_name(b"fileops") {
            Some(id) => kprint!("spawned ring3 task {} (fileops)\n", id),
            None => kprint!("ufileops: program not found\n"),
        },
        b"ahci" => {
            crate::ahci::dump();
        },
        b"usb" => {
            crate::usb::dump();
        },
        b"ipi" => {
            let id = crate::ipi::current_apic_id();
            kprint!("ipi: sending self-IPI (vector 0xFE) to APIC {}\n", id);
            crate::ipi::send_ipi_self(0xFE, crate::ipi::ICR_FIXED);
            kprint!("ipi: sent\n");
        },
        b"date" => {
            let dt = crate::rtc::read_rtc();
            let mut buf = [0u8; 20];
            dt.format(&mut buf);
            kprint!("{} {} {:>2} ", dt.weekday_name(), dt.month_name(), dt.day);
            for &b in &buf[11..19] { kprint!("{}", b as char); }
            kprint!(" UTC {}\n", dt.year);
        },
        b"acpi" => {
            let info = crate::acpi::info();
            if !info.rsdp_found {
                kprint!("acpi: RSDP not found\n");
            } else {
                kprint!("ACPI v{}\n", info.acpi_version);
                kprint!("  OEM: {}\n", core::str::from_utf8(&info.oem_id).unwrap_or("???"));
                kprint!("  RSDT: {:#x}\n", info.rsdt_address);
                if info.acpi_version >= 2 {
                    kprint!("  XSDT: {:#x}\n", info.xsdt_address);
                }
                if info.fadt_found {
                    kprint!("  FADT: SCI={} PM_TMR={:#x}\n", info.sci_interrupt, info.pm_timer_port);
                } else {
                    kprint!("  FADT: not found\n");
                }
            }
        },
        b"arp" => {
            crate::arp::dump();
        },
        _ if line.starts_with(b"nslookup ") => {
            let name = trim_bytes(&line[9..]);
            if name.is_empty() {
                kprint!("nslookup: usage: nslookup <hostname>\n");
            } else {
                match crate::dns::resolve(name) {
                    Some(ip) => {
                        print_bytes(name);
                        kprint!(" -> {}.{}.{}.{}\n", ip[0], ip[1], ip[2], ip[3]);
                    }
                    None => {
                        kprint!("nslookup: failed to resolve '");
                        print_bytes(name);
                        kprint!("'\n");
                    }
                }
            }
        }
        b"tcpclose" => {
            if crate::tcp::is_established() {
                crate::tcp::close();
                kprint!("tcpclose: FIN sent\n");
            } else {
                kprint!("tcpclose: no active connection\n");
            }
        }
        b"netstat" => {
            crate::tcp::dump_connections();
        }
        b"top" => {
            // System overview: uptime + memory + tasks.
            let ticks = crate::interrupts::ticks();
            let secs = ticks / 100;
            let mins = secs / 60;
            let hrs = mins / 60;
            kprint!("=== NEO-OS-3 System Overview ===\n");
            kprint!("Uptime: {}h {}m {}s ({} ticks)\n", hrs, mins % 60, secs % 60, ticks);

            // Memory.
            let (_, total, used_frames, free) = crate::mem::stats();
            let used_kb = used_frames * 4;
            let total_kb = total / 1024;
            let free_kb = free / 1024;
            kprint!("Memory: {} KiB used / {} KiB total ({} KiB free)\n", used_kb, total_kb, free_kb);

            // Tasks.
            let mut ready = 0u32;
            let mut exited = 0u32;
            for i in 0..crate::task::MAX_TASKS {
                if let Some(info) = crate::task::get_task_info(i) {
                    match info.state {
                        1 => ready += 1,
                        2 => exited += 1,
                        _ => {}
                    }
                }
            }
            kprint!("Tasks: {} ready, {} exited\n", ready, exited);

            // Network.
            kprint!("TCP: {}\n", crate::tcp::state_name());

            // Filesystem.
            if crate::neofs::is_mounted() {
                if let Some(stats) = crate::neofs::disk_stats() {
                    let pct = if stats.total_blocks > 0 {
                        (stats.used_blocks as u64 * 100) / stats.total_blocks as u64
                    } else { 0 };
                    kprint!("Disk: {}/{} blocks used ({}%)\n", stats.used_blocks, stats.total_blocks, pct);
                }
            } else {
                kprint!("Disk: not mounted\n");
            }
            kprint!("================================\n");
        }
        _ if line.starts_with(b"ffind ") => {
            let pattern = trim_bytes(&line[6..]);
            if pattern.is_empty() {
                kprint!("ffind: usage: ffind <pattern>\n");
            } else {
                let mut found = 0usize;
                crate::neofs::find_files(pattern, &mut |name, mode, size, parent_ino| {
                    let type_char = if (mode & 0xF) == 2 { 'd' } else { '-' };
                    // Build path: parent/name.
                    if parent_ino != 0 && parent_ino != crate::neofs::ROOT_INO as u32 {
                        let mut pname_buf = [0u8; 16];
                        let plen = crate::neofs::inode_name_into(parent_ino as usize, &mut pname_buf);
                        if plen > 0 {
                            let pn = core::str::from_utf8(&pname_buf[..plen]).unwrap_or("?");
                            let nn = core::str::from_utf8(name).unwrap_or("?");
                            kprint!("{} {:>6} {}/{}\n", type_char, size, pn, nn);
                        } else {
                            let name_str = core::str::from_utf8(name).unwrap_or("???");
                            kprint!("{} {:>6} {}\n", type_char, size, name_str);
                        }
                    } else {
                        let name_str = core::str::from_utf8(name).unwrap_or("???");
                        kprint!("{} {:>6} /{}\n", type_char, size, name_str);
                    }
                    found += 1;
                });
                if found == 0 {
                    kprint!("ffind: no matches\n");
                }
            }
        }
        _ if line.starts_with(b"ftouch ") => {
            let name = trim_bytes(&line[7..]);
            if name.is_empty() {
                kprint!("ftouch: usage: ftouch <name>\n");
            } else if crate::neofs::find_inode(name).is_some() {
                kprint!("ftouch: '");
                print_bytes(name);
                kprint!("' already exists\n");
            } else {
                match crate::neofs::create_file(name) {
                    Some(_) => { kprint!("ftouch: created '"); print_bytes(name); kprint!("'\n"); }
                    None => kprint!("ftouch: failed (no space or name too long)\n"),
                }
            }
        }
        _ if line.starts_with(b"fcp ") => {
            let rest = &line[4..];
            let mut split = rest.splitn(2, |&b| b == b' ');
            let src_name = trim_bytes(split.next().unwrap_or(b""));
            let dst_name = trim_bytes(split.next().unwrap_or(b""));
            if src_name.is_empty() || dst_name.is_empty() {
                kprint!("fcp: usage: fcp <src> <dst>\n");
            } else {
                // Read source file.
                let fd = crate::vfs::vfs_open(src_name, 0, 0);
                if fd < 0 {
                    kprint!("fcp: cannot open source '");
                    print_bytes(src_name);
                    kprint!("'\n");
                } else {
                    let mut buf = [0u8; 512];
                    let n = crate::vfs::vfs_read(fd as u32, &mut buf, 0);
                    crate::vfs::vfs_close(fd as u32, 0);
                    if n < 0 {
                        kprint!("fcp: read error\n");
                    } else {
                        // Write to destination (create/truncate).
                        let fd2 = crate::vfs::vfs_open(dst_name, 1, 0);
                        if fd2 < 0 {
                            kprint!("fcp: cannot create '");
                            print_bytes(dst_name);
                            kprint!("'\n");
                        } else {
                            if n > 0 {
                                crate::vfs::vfs_write(fd2 as u32, &buf[..n as usize], 0);
                            }
                            crate::vfs::vfs_close(fd2 as u32, 0);
                            kprint!("fcp: copied ");
                            print_bytes(src_name);
                            kprint!(" -> ");
                            print_bytes(dst_name);
                            kprint!(" ({} bytes)\n", n);
                        }
                    }
                }
            }
        }
        b"off" | b"shutdown" | b"poweroff" | b"exit" => shutdown(),
        b"reboot" | b"reset" => reboot(),
        _ => {
            print_bytes(w);
            kprint!(": unknown command (try help)\n");
        }
    }
}
