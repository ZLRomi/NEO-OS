# NEO-OS-3 User Programming Guide

## Overview

NEO-OS-3 user programs run in ring-3 with isolated address spaces. They communicate with the kernel through `int 0x80` syscalls. Each program is compiled as a separate ELF binary from the `user/` crate using feature flags.

## Syscall Interface

All syscalls use `int 0x80`:
- **RAX**: syscall number (input) / return value (output)
- **RDI**: first argument
- **RSI**: second argument
- **RDX**: third argument

### Assembly Template

```rust
fn sys_call(nr: u64, a: u64, b: u64, c: u64) -> u64 {
    let ret: u64;
    unsafe {
        core::arch::asm!(
            "int 0x80",
            inlateout("rax") nr => ret,
            in("rdi") a,
            in("rsi") b,
            in("rdx") c,
            options(nostack),
        )
    }
    ret
}
```

## Syscall Reference

| NR | Name | Signature | Returns | Description |
|----|------|-----------|---------|-------------|
| 0 | write | `(fd, buf_ptr, len)` | bytes written | Write to console (fd ignored) |
| 1 | exit | `(code, -, -)` | never | Terminate task |
| 2 | uptime | `(-, -, -)` | ticks | PIT tick count (10ms each) |
| 3 | brk | `(new_brk, -, -)` | new brk | Extend heap (0 = query) |
| 4 | meminfo | `(buf, len, -)` | bytes written | Memory stats to buffer |
| 5 | read_char | `(-, -, -)` | char or 0 | Non-blocking keyboard read |
| 6 | sleep | `(ticks, -, -)` | 0 | Busy-wait N ticks |
| 7 | print_int | `(value, -, -)` | 0 | Print signed integer |
| 8 | net_send | `(buf, len, -)` | 0 or !0 | Send raw Ethernet frame |
| 9 | net_recv | `(buf, len, -)` | bytes or 0 | Receive Ethernet frame |
| 10 | tcp_send | `(buf, len, -)` | 0 or !0 | Send TCP data |
| 11 | open | `(name, name_len, flags)` | fd or negative | Open/create file |
| 12 | close | `(fd, -, -)` | 0 or negative | Close file descriptor |
| 13 | read | `(fd, buf, len)` | bytes or negative | Read from file |
| 14 | write_fd | `(fd, buf, len)` | bytes or negative | Write to file |
| 15 | stat | `(name, name_len, stat_buf)` | 0 or negative | Get file metadata |
| 16 | tcp_recv | `(buf, len, -)` | bytes or 0 | Receive TCP data |
| 17 | readdir | `(buf, len, -)` | bytes written | Serialize directory listing |
| 18 | ftruncate | `(name, name_len, size)` | 0 or negative | Truncate file to size |
| 19 | getpid | `(-, -, -)` | task ID | Current task identifier |
| 20 | tasklist | `(buf, len, -)` | bytes written | Serialize task list |
| 21 | block_read | `(sector, buf, len)` | 0 or !0 | Read disk sector |
| 22 | block_write | `(sector, buf, len)` | 0 or !0 | Write disk sector |
| 23 | mkdir | `(name, name_len, -)` | 0 or negative | Create directory |
| 24 | unlink | `(name, name_len, -)` | 0 or negative | Delete file |
| 25 | udp_send | `(buf, len, dst_port)` | 0 or !0 | Send UDP packet |
| 26 | mprotect | `(addr, len, prot)` | 0 or !0 | Change page permissions (1=R,2=W,4=X) |
| 27 | mmap | `(name, name_len, prot)` | addr or !0 | Map file to memory (stub) |
| 28 | munmap | `(addr, len, -)` | 0 or !0 | Unmap pages and free frames |
| 29 | fork | `(-, -, -)` | child PID or !0 | Fork process (COW) |
| 30 | waitpid | `(pid, -, -)` | child ID or 0 | Wait for child exit (pid=0: any) |
| 31 | getppid | `(-, -, -)` | parent ID | Parent task identifier |
| 32 | kill | `(pid, sig, -)` | 0 or negative | Send signal (15=SIGTERM; pid<0: kill group) |
| 33 | getpgid | `(-, -, -)` | pgid | Process group ID |
| 34 | exec | `(name, name_len, -)` | 0 or !0 | Replace process image |
| 35 | rmdir | `(name, name_len, -)` | 0 or negative | Remove empty directory |

### Open Flags
- `0` — read-only
- `1` — write (create/truncate)
- `2` — read-write
- `3` — append (create if needed, seek to end)

### Stat Buffer Format (16 bytes)
```
Offset  Size  Field
0       4     ftype (low 4 bits: 1=file, 2=dir; high 12 bits: rwxrwxrwx)
4       4     size (bytes)
8       4     inode number
12      4     reserved
```

### Readdir Buffer Format
```
[u32 count]
[entry: u8 type, u32 size, u8 name_len, name...]...
```

### Tasklist Buffer Format
```
[u32 count]
[entry: u64 id, u8 state (1=ready, 2=exited), u8 is_user]...
```

## Writing a User Program

### Step 1: Add Feature Flag

In `user/Cargo.toml`:
```toml
prog_myprogram = []
```

### Step 2: Write the Program

In `user/src/main.rs`:
```rust
#[cfg(feature = "prog_myprogram")]
fn user_myprogram() -> ! {
    sys_write(b"Hello from my program!\n");
    sys_exit(0);
}
```

### Step 3: Register in Dispatch Table

Add to the `any(...)` feature list and dispatch match:
```rust
// In the feature list:
feature = "prog_myprogram",

// In the dispatch match:
#[cfg(feature = "prog_myprogram")] user_myprogram();
```

### Step 4: Add to Build System

In `tools/mkesp.sh`:
```bash
build_user_prog "myprogram" "prog_myprogram"
```

Add to the ESP copy loop and bootloader `prog_names` array.

### Step 5: Add Shell Command (optional)

In `kernel/src/shell.rs`, add HELP_DB entry and dispatch case.

## Memory Management

User programs have:
- **Code segment**: Read+Execute at 0x400000–0x7FFFFF
- **Data/Heap**: Read+Write at 0x800000–0x9FFFFF (demand-paged)
- **Stack**: 32KB at top of address space
- **Heap**: Via `sys_brk()` — extends data segment upward

### Process Creation

```rust
// Fork a child process (COW)
let child_pid = sys_call(29, 0, 0, 0); // sys_fork()
if child_pid == 0 {
    // In child: exec a new program
    sys_call(34, b"echo".as_ptr() as u64, 4, 0); // sys_exec("echo")
} else {
    // In parent: wait for child
    sys_call(30, child_pid, 0, 0); // sys_waitpid(child_pid)
}

// Get process info
let my_pid = sys_getpid();           // NR 19
let parent = sys_call(31, 0, 0, 0); // sys_getppid()
let pgid   = sys_call(33, 0, 0, 0); // sys_getpgid()

// Kill a process
sys_call(32, target_pid, 15, 0);     // SIGTERM
sys_call(32, (-pgid as u64), 15, 0); // Kill entire process group
```

### Page Permissions

```rust
// Make a page writable (e.g., for self-modifying code)
sys_call(26, 0x800000, 4096, 3); // mprotect(addr, len, R+W)
// prot bits: 1=READ, 2=WRITE, 4=EXEC
```

### Filesystem Operations

```rust
// Create directory
sys_call(23, b"mydir".as_ptr() as u64, 5, 0);  // sys_mkdir

// Remove empty directory
sys_call(35, b"mydir".as_ptr() as u64, 5, 0);  // sys_rmdir

// Delete file
sys_call(24, b"myfile".as_ptr() as u64, 7, 0); // sys_unlink

// Read /proc files
let fd = sys_open(b"/proc/self/status", 0);
if fd >= 0 {
    let mut buf = [0u8; 256];
    let n = sys_read(fd as u64, &mut buf);
    sys_close(fd as u64);
    // buf now contains "pid: N\nstate: running\n"
}

// Send UDP packet
let payload = b"hello";
sys_call(25, payload.as_ptr() as u64, payload.len() as u64, 7); // to port 7
```

### Heap Allocator

The `user/` crate includes a simple free-list allocator:
```rust
let ptr = malloc(64);   // allocate 64 bytes
free(ptr);              // release
```

## Constraints

- **No standard library** (`#![no_std]`, `#![no_main]`)
- **Max filename**: 16 characters
- **Max file size**: ~68KB (8 direct + 128 indirect blocks × 512B)
- **No floating point** (no FPU save/restore)
- **No threads** (single-threaded per task)
- **Buffer sizes**: Keep stack buffers ≤ 1KB to avoid overflow
- **Programs must call `sys_exit()`** — no implicit return

## Building

```bash
# Build all programs and create ESP image:
./tools/mkesp.sh

# Build single program:
cd user
RUSTFLAGS="-C link-arg=-T/path/to/user.ld -C linker=ld -C relocation-model=static" \
  rustup run nightly cargo build \
  --target x86_64-unknown-none --features prog_myprogram
```

## Running

```bash
# Interactive (SDL display):
powershell -ExecutionPolicy Bypass -File .\run-win.ps1 sdl

# Headless with serial output:
./tools/run-wsl.sh

# Smoke test (automated):
./tools/run-wsl.sh --smoke
```

## Example Programs

See `user/src/main.rs` for complete examples:
- `user_fstest` — VFS operations (open/write/read/close/stat)
- `user_ls` — Directory listing via readdir
- `user_grep` — File search with substring matching
- `user_ps` — Task monitoring via tasklist syscall
- `user_dd` — Raw disk sector access
- `user_http` — TCP send/receive with HTTP parsing
