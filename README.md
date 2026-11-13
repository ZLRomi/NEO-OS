# NEO-OS-3

A bare-metal x86-64 operating system written in Rust, featuring UEFI boot, preemptive multitasking, ring-3 user tasks with per-process address spaces, ELF loading, a layered filesystem with persistent block storage, TCP networking with full send/receive, and more.

## Features

- **UEFI Boot**: Custom bootloader with GOP display setup, self-built identity page table (OVMF 202608+ NX-bit workaround), ExitBootServices handoff
- **Memory Management**: Frame allocator with alloc/free tracking, identity paging (0–4 GiB), per-task page tables, slab allocator (32/64/128/256 byte caches)
- **ELF64 Loader**: Full parser supporting PT_LOAD program headers + section-header fallback, ET_EXEC/ET_DYN, permission mapping (RX/RW/RWX), BSS zero-fill
- **Preemptive Multitasking**: 16-slot task table, timer-driven round-robin (100 Hz PIT), ring-3 isolation via CR3 switching
- **User Programs**: Independent `user/` crate with 30 programs compiled as separate ELF binaries via feature flags
- **Syscalls**: 35 syscalls via `int 0x80` (see table below)
- **Networking**: virtio-net driver with TX/RX virtqueues, IPv4, TCP full send/receive with ring buffer + retransmission + flow control + FIN/RST state machine + multi-connection table, ICMP echo reply (ping), UDP echo server + user API, ARP protocol, DNS resolver, HTTP client with header parsing + chunked encoding
- **Filesystem**: Layered architecture — block device abstraction → VFS → NEO-FS on virtio-blk, with subdirectories, file permissions (rwxrwxrwx), indirect blocks (up to 68KB), rename, truncate, fsck, df, /proc virtual FS (6 files: self/status, self/cmdline, self/maps, uptime, meminfo, tasks)
- **Shell**: 60+ commands with help database, command history (↑/↓) with persistence, tab completion (commands + filenames), output redirection (`>` `>>` `<`), pipes (`|`), environment variables, aliases, glob patterns (`*` `?`), script interpreter (if/fi, while/done, `;`), Ctrl+C cancel
- **Process Management**: fork (COW page copy), exec (replace image), waitpid, getpid/getppid, kill/signal (SIGTERM), process groups
- **Memory**: COW pages, demand paging, mprotect, mmap/munmap (stub), slab allocator, physical memory hot-plug framework
- **Drivers**: Registry framework (serial, PS/2 keyboard, PS/2 mouse, framebuffer, PIT, virtio-net, virtio-blk, RTC MC146818, ACPI RSDP/FADT parser, AHCI/SATA detection, USB controller detection)
- **SMP**: AP startup framework (SIPI trampoline to long mode, per-AP stacks, per-CPU data, SpinLock/TicketLock primitives, IPI support)
- **Testing**: In-kernel unit test framework (12 tests covering klog, glob, sync, VFS, FS stress (7 phases), memory tracking, net throughput, lock stress (8000+ ops), syscall fuzz (28 assertions), mem hotplug, global leak detection)

## Architecture

```
┌─────────────────────────────────────────────────┐
│              User Programs (ring 3)              │
│  uls · ucat · fstest · http · nano · calc · ... │
├─────────────────────────────────────────────────┤
│            Syscall Interface (int 0x80)          │
│         34 syscalls: write, open, read, ...      │
├──────────┬──────────┬───────────┬───────────────┤
│   VFS    │   TCP    │  Network  │    Task       │
│  fd mgmt │  recv tx │  virtio   │  scheduler    │
├──────────┴──────────┤   -net    ├───────────────┤
│     NEO-FS          ├───────────┤               │
│  inode/bitmap/sb    │           │               │
├─────────────────────┴───────────┤               │
│        Block Device Layer       │               │
│     register / read / write     │               │
├─────────────────────────────────┤               │
│  virtio-blk    virtio-net       │               │
│  (PCI+virtq)   (PCI+virtq)     │               │
└─────────────────────────────────┴───────────────┘
```

### Directory Structure

| Path | Description |
|------|-------------|
| `bootloader/` | UEFI bootloader (GOP, kernel loading, page table, initrd staging) |
| `kernel/` | Main kernel source |
| `kernel/src/main.rs` | Entry point (`_start`), subsystem init sequence |
| `kernel/src/mem.rs` | Frame allocator, identity map, per-task page tables |
| `kernel/src/task.rs` | Task struct, spawn, schedule, exit, ELF dispatch |
| `kernel/src/elf.rs` | ELF64 loader, permission mapping |
| `kernel/src/interrupts.rs` | IDT, PIC, PIT, syscall dispatcher (`do_syscall`) |
| `kernel/src/block.rs` | Block device abstraction layer |
| `kernel/src/vfs.rs` | Virtual File System (fd table, open/close/read/write/stat/readdir) |
| `kernel/src/neofs.rs` | NEO-FS filesystem (inode, bitmap, superblock on block device) |
| `kernel/src/virtio_blk.rs` | virtio-blk PCI driver (virtqueue sector I/O) |
| `kernel/src/virtio_net.rs` | virtio-net PCI driver (TX/RX virtqueues) |
| `kernel/src/tcp.rs` | TCP state machine (SYN/ACK/data/FIN, recv ring buffer) |
| `kernel/src/ip.rs` | IPv4 header, checksums, packet builders |
| `kernel/src/shell.rs` | Interactive shell (50+ commands, history, tab completion, scripts) |
| `kernel/src/initrd.rs` | In-memory initrd archive (legacy, still used for /bin/) |
| `kernel/src/smp.rs` | SMP topology detection, AP trampoline, per-CPU data |
| `kernel/src/sync.rs` | SpinLock and TicketLock primitives (interrupt-safe, RAII) |
| `kernel/src/slab.rs` | Slab allocator (32/64/128/256 byte object caches) |
| `kernel/src/test.rs` | Kernel unit test framework (12 tests) |
| `kernel/src/rtc.rs` | MC146818 RTC driver (date/time read) |
| `kernel/src/acpi.rs` | ACPI RSDP/FADT parser (read-only) |
| `kernel/src/klog.rs` | kprint! macro with redirect capture support |
| `kernel/src/pci.rs` | PCI bus enumeration (CF8/CFD port I/O) |
| `kernel/src/driver.rs` | Driver registry framework |
| `kernel/src/gdt.rs` | GDT + TSS setup |
| `kernel/src/heap.rs` | Kernel heap allocator |
| `user/` | User-space programs (independent crate, separate ELFs) |
| `common/` | Shared types (BootInfo, InitrdEntry) |
| `tools/mkesp.sh` | Build script (bootloader + kernel + user → FAT32 ESP image) |
| `tools/run-wsl.sh` | QEMU runner (headless + smoke test) |
| `tools/run-win.ps1` | Windows native QEMU runner (SDL display) |

## Syscall Table

All syscalls use `int 0x80`. Arguments: RAX=nr, RDI=a, RSI=b, RDX=c. Return in RAX.

| NR | Name | Signature | Description |
|----|------|-----------|-------------|
| 0 | write | `(fd, buf, len)` | Write to console (fd ignored) |
| 1 | exit | `(code)` | Terminate current task |
| 2 | uptime | `()` | Return PIT tick count |
| 3 | brk | `(new_brk)` | Extend program break (heap) |
| 4 | meminfo | `(buf, len)` | Write memory stats to buffer |
| 5 | read_char | `()` | Read one keyboard char (non-blocking) |
| 6 | sleep | `(ticks)` | Busy-wait N ticks (10ms each) |
| 7 | print_int | `(val)` | Print signed integer |
| 8 | net_send | `(buf, len)` | Send raw Ethernet frame |
| 9 | net_recv | `(buf, len)` | Receive Ethernet frame |
| 10 | tcp_send | `(buf, len)` | Send TCP data (established connection) |
| 11 | open | `(name, name_len, flags)` | Open/create file → fd |
| 12 | close | `(fd)` | Close file descriptor |
| 13 | read | `(fd, buf, len)` | Read from file |
| 14 | write_fd | `(fd, buf, len)` | Write to file |
| 15 | stat | `(name, name_len, stat_buf)` | Get file metadata |
| 16 | tcp_recv | `(buf, len)` | Receive TCP data |
| 17 | readdir | `(buf, len)` | Serialize directory listing |
| 18 | ftruncate | `(name, name_len, size)` | Truncate file to size |
| 19 | getpid | `()` | Return current task ID |
| 20 | tasklist | `(buf, len)` | Serialize task list |
| 21 | block_read | `(sector, buf, len)` | Read raw disk sector |
| 22 | block_write | `(sector, buf, len)` | Write raw disk sector |
| 23 | mkdir | `(name, name_len)` | Create directory |
| 24 | unlink | `(name, name_len)` | Delete file |
| 25 | udp_send | `(buf, len, dst_port)` | Send UDP packet |
| 26 | mprotect | `(addr, len, prot)` | Change page permissions (R/W/X) |
| 27 | mmap | `(name, name_len, prot)` | Map file (stub) |
| 28 | munmap | `(addr, len)` | Unmap pages |
| 29 | fork | `()` | Fork process (COW) → child PID |
| 30 | waitpid | `(pid)` | Wait for child exit |
| 31 | getppid | `()` | Return parent task ID |
| 32 | kill | `(pid, sig)` | Send signal (SIGTERM=15) |
| 33 | getpgid | `()` | Return process group ID |
| 34 | exec | `(name, name_len)` | Replace process image |
| 35 | rmdir | `(name, name_len)` | Remove empty directory |

Flags for `open`: 0=read-only, 1=write (create/truncate), 2=read-write, 3=append.

## User Programs

| Program | Description |
|---------|-------------|
| user | Demo ring3 program (hello + wait) |
| userbad | Negative test: read kernel memory (faults) |
| usernx | Negative test: execute stack (faults) |
| userexec | Negative test: execute kernel text (faults) |
| userptr | Syscall pointer validation test |
| isowrite | Write isolation between tasks |
| heaptest | Heap allocation test |
| echo | Echo from heap buffer |
| uptime | Show system uptime |
| meminfo | Show memory stats via syscall |
| counter | Counter test with heap |
| guess | Number guessing game (interactive) |
| edit | Simple line editor |
| calc | Arithmetic calculator |
| heapstress | Heap alloc/free stress test |
| ping | ICMP echo request sender |
| netdump | Network packet receiver |
| time | Show formatted time |
| nano | Screen editor with cursor movement |
| http | HTTP GET via TCP send/recv with header parsing |
| fstest | VFS syscall integration test (open/write/read/close/stat) |
| ls | List files on NEO-FS (ring3, via readdir syscall) |
| cat | Read and print file from NEO-FS (ring3, via open/read) |
| hexdump | Hex dump of file contents (ring3) |
| whoami | Show current task ID and uptime (ring3, via getpid syscall) |
| grep | Pattern search in files (ring3) |
| ps | Process list (ring3, via tasklist syscall) |
| dd | Block-level disk read/write (ring3) |
| kill | Send SIGTERM to task (ring3, via kill syscall) |
| fileops | Test mkdir/rm/touch via syscalls (ring3) |
| udptest | Send UDP packet via syscall (ring3) |

## Shell Commands

| Command | Description |
|---------|-------------|
| `help [cmd]` | Show all commands or detailed help |
| `clear` | Clear screen |
| `mem` | Memory stats |
| `heap` | Heap usage |
| `ticks` | PIT tick count |
| `uptime` | System uptime |
| `echo <text>` | Print text |
| `sleep <ms>` | Sleep N milliseconds |
| `tasks` / `ps` | List tasks |
| `drivers` | List registered drivers |
| `smp` | CPU topology info |
| `progs` | List loaded user programs |
| `ls` | List initrd files (/bin/) |
| `cat <name>` | Show initrd file contents |
| `write <name> <text>` | Write to initrd file |
| `wc <name>` | Word/line/char count |
| `grep <pat> <file>` | Search pattern in file |
| `sort <name>` | Sort file lines |
| `blk` | Block device status |
| `blktest` | Block device read/write test |
| `mkfs` | Format disk with NEO-FS |
| `fsinfo` | NEO-FS filesystem info |
| `fls` | List files on NEO-FS |
| `fcat <name>` | Read file from NEO-FS |
| `fwrite <name> <text>` | Write text to NEO-FS file |
| `frm <name>` | Remove file from NEO-FS |
| `fmkdir <name>` | Create directory on NEO-FS |
| `uls` | List files (ring3 user program) |
| `ucat` | Cat file (ring3 user program) |
| `net` | Network device status |
| `ipconfig` | IP configuration |
| `tcp` | TCP connection test |
| `udpecho` | UDP echo server toggle |
| `exec <name>` | Execute program by name |
| `user*` | Spawn ring3 test programs |
| `fstest` | VFS syscall test (ring3) |
| `uls`, `ucat`, `hexdump`, `whoami` | Ring3 user programs |
| `set KEY=VALUE` | Set environment variable |
| `unset KEY` | Unset environment variable |
| `env` | List environment variables |
| `alias name=cmd` | Define command alias |
| `date` | Show RTC date/time |
| `acpi` | Show ACPI table info |
| `guess`, `edit`, `calc`, `heapstress`, `ping`, `netdump`, `nano`, `http` | User programs |
| `gui` | Graphical mode (mouse) |
| `mouse` | Mouse position/buttons |
| `kbddebug` | Keyboard scancode trace toggle |
| `sysinfo` | Full system information |
| `test` | One-shot task test |
| `off` | Power off (ACPI) |
| `reboot` | Reboot system |

## NEO-FS Filesystem

NEO-FS is a simple inode-based filesystem stored on a virtio-blk block device.

### On-Disk Layout (512-byte sectors)

| Sector(s) | Content |
|-----------|---------|
| 0 | Superblock (magic `NEO1`, version, inode/data counts) |
| 1 | Inode bitmap (64 inodes) |
| 2 | Data block bitmap |
| 3–18 | Inode table (16 sectors × 8 inodes = 128 slots, 64 used) |
| 19+ | Data blocks |

### Inode Format (64 bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | mode (0=free, 1=file, 2=dir) |
| 4 | 4 | size (bytes) |
| 8 | 32 | blocks[8] (1-based direct block pointers) |
| 40 | 16 | name (null-padded) |
| 56 | 4 | parent_ino |
| 60 | 4 | reserved |

Max file size: 8 × 512 = 4096 bytes. Max filename: 16 characters. Subdirectories supported via `parent_ino`.

## Building

```bash
# Requires: Rust nightly, QEMU, mtools, OVMF (edk2-ovmf)
./tools/mkesp.sh
```

Produces `esp.img` (64 MiB FAT32 EFI System Partition).

## Running

```bash
# Headless with serial output
./tools/run-wsl.sh

# Smoke test (37 assertions)
./tools/run-wsl.sh --smoke

# Windows native (SDL window)
powershell -ExecutionPolicy Bypass -File .\run-win.ps1 sdl
```

QEMU config: `-machine q35 -cpu qemu64 -m 512 -smp 4`, virtio-net + virtio-blk (4 MiB test disk).

## Known Limitations

- **SMP**: APs reach long mode with per-CPU data but task scheduling on APs not yet implemented
- **TCP**: Single connection only, no retransmission or flow control
- **NEO-FS**: Max 64 inodes, max 68 KB per file (indirect blocks), single-sector bitmap (4096 data blocks max)
- **initrd**: Still used for `/bin/` program staging; not persistent
- **No interrupts for virtio**: Polling-based I/O only
- **ACPI**: RSDP search limited to legacy BIOS memory regions; UEFI systems use EFI configuration table
- **Alias**: Max 8 nesting levels, no expansion history
- **Shell redirect**: Capture buffer limited to 2 KB per command

## License

MIT
