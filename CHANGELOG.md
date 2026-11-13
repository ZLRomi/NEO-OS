# CHANGELOG

All notable changes to NEO-OS-3 are documented in this file.

## [Unreleased] — 2026-11-08 onwards

### Added — SMP Multi-Core
- Trampoline patch offset fix (stack 0x69, stack_size 0x75, entry 0x84)
- Per-AP independent stacks (dynamic calculation: base + (apic_id+1) × size)
- AP kernel entry (GDT/IDT load, interrupt enable, idle loop)
- Per-CPU data structure (CpuData: apic_id, current_task, ticks, online)
- SpinLock and TicketLock primitives (interrupt-safe, RAII guards)

### Added — Filesystem (NEO-FS)
- Block device abstraction layer (`block.rs`)
- VFS layer with fd management (`vfs.rs`)
- NEO-FS filesystem on virtio-blk (`neofs.rs`)
- Single indirect block support (4KB → 68KB max file)
- Subdirectory support (mkdir, parent_ino tracking)
- rmdir (empty directory removal with child check)
- rename/mv (atomic rename with overwrite semantics)
- truncate/ftruncate (shrink with block freeing, extend sparse)
- fsck consistency checker (inode validation, block reference checks)
- df disk space statistics (blocks/inodes used/free/percentage)
- Persistent boot init script (/init on NEO-FS)
- Bitmap bounds safety (clamp to single-sector capacity)
- Syscalls: open(11), close(12), read(13), write_fd(14), stat(15), readdir(17), ftruncate(18)

### Added — Shell
- Universal output redirection via kprint capture hook (`cmd > file`)
- Append redirection (`cmd >> file`)
- Environment variables (`set KEY=VALUE`, `unset KEY`, `env`)
- Command aliases (`alias name=cmd`, auto-expansion with recursion guard)
- Glob pattern matching (`*` and `?` in `fls [pattern]`)
- Command history persistence (save/load from NEO-FS `/history`)
- New commands: blk, blktest, mkfs, fsinfo, fls, fcat, fwrite, frm, fmkdir, frmdir, fmv, fsck, set, unset, env, alias, uls, ucat, fstest

### Added — Networking
- TCP data receive with ring buffer (4KB recv buffer)
- TCP PSH+ACK handling with automatic ACK reply
- TCP FIN handling for graceful close
- sys_tcp_recv syscall (NR 16)
- HTTP user program updated with full send/recv flow

### Added — User Programs
- fstest: VFS syscall integration test (open/write/read/close/stat)
- ls: ring3 directory listing via readdir syscall
- cat: ring3 file reader via open/read syscalls
- hexdump: hex dump of file contents (ring3)
- whoami: show current task ID and uptime (ring3, getpid syscall)

### Added — Shell Enhancements
- Script interpreter: if/fi, while/done, semicolon (;) command separator
- Tab completion enhanced: context-aware (commands + NEO-FS filenames)
- Ctrl+C (0x03): cancel current input line
- Glob pattern matching (* and ?) in fls command

### Added — Drivers & System
- RTC MC146818 driver (date/time read, BCD/binary auto-detect)
- ACPI RSDP/FADT parser (read-only, checksum validation)
- Slab allocator (32/64/128/256 byte object caches)
- Memory allocation tracking (alloc_count/free_count for leak detection)
- Kernel unit test framework (8 tests: klog, glob, sync, VFS, FS stress, memory)
- sys_getpid syscall (NR 19)
- sys_ftruncate syscall (NR 18)
- ICMP echo reply (automatic ping response)
- HTTP header parsing (status code, body separation)

### Fixed
- virtio-blk avail/used ring offset calculation (-2 → +2)
- virtio-net avail/used ring offset calculation (-2 → +2)
- NEO-FS block number 0 ambiguity (switched to 1-based block numbering)
- Shell fcat stack overflow (reduced buffer from 4096 to 512 bytes)
- Bitmap array out-of-bounds panic (clamped to SECTOR_SIZE × 8)
- Alias infinite recursion (depth limit of 8)
- History persist/load buffer sizes (2048 → 1024 bytes)

### Quality
- Code quality review rounds (R10, R20, R30, R40)
- All functions have boundary checks and error returns
- No panics in normal operation paths

### Added — Post-80 Exploration (Rounds 46-58)
- Shell $VAR environment variable expansion in all commands
- Shell input redirection (`cmd < file`)
- Shell pipe (`cmd1 | cmd2`) via kprint capture + injection
- Shell script interpreter (if/fi, while/done, semicolon separator)
- Shell tab completion enhanced (commands + NEO-FS filenames)
- Ctrl+C (0x03) cancel current input line
- ARP protocol (request/reply, 8-entry cache, auto-reply)
- DNS A record resolver (UDP query to 10.0.2.3, response parsing)
- TCP FIN/RST complete state machine (passive/active close)
- TCP sliding window / flow control (send_window, last_acked tracking)
- TCP retransmission timer (1s RTO, data backup for resend)
- TCP multi-connection table (4 concurrent connections, 4-tuple routing)
- File permissions (rwxrwxrwx mode bits in inode)
- `ffind <pattern>` command (glob search across all directories)
- `ftouch <name>` command (create empty file)
- `fcp <src> <dst>` command (copy file contents)
- `nslookup <name>` command (DNS resolution)
- `tcpclose` command (active TCP close)
- `arp` command (display ARP cache)
- `unittest` command (8 kernel unit tests)
- sys_mkdir (NR 23), sys_unlink (NR 24), sys_tasklist (NR 20)
- User programs: hexdump, whoami, grep, ps, dd, fileops (29 total)
- Syscalls: 25 total (was 11 at baseline)

### Fixed (Post-80)
- mkfs on fresh disk (MOUNT_DEV must be set before any sector I/O)
- fstest stat check updated for permission-aware mode field (ftype & 0xF)
- Smoke test disk reset (always recreate blk.img in smoke mode)

### Added — Post-80 Exploration (Rounds 59-89)
- AHCI/SATA controller detection framework (PCI scan, HBA register read, port enumeration)
- USB controller detection framework (UHCI/OHCI/EHCI/xHCI identification)
- /proc virtual filesystem (self/status, uptime, meminfo, tasks)
- Network throughput benchmark (UDP send rate measurement)
- Lock stress test (1000 spin + 500 ticket + 200 try_lock cycles)
- Syscall fuzz test (18 edge-case assertions across VFS/block/glob)
- Physical memory hot-plug framework (hot_add/hot_remove with overlap checks)
- mprotect syscall (NR 26): change page permissions (R/W/X)
- mmap syscall (NR 27): stub implementation
- munmap syscall (NR 28): unmap user pages and free frames
- COW (Copy-on-Write) pages: PTE_COW bit 9, #PF handler clones on write fault
- Demand paging: #PF handler allocates zero pages for not-present faults in heap region
- fork syscall (NR 29): full COW fork with page table cloning
- waitpid syscall (NR 30): busy-wait for child exit with reaping
- getppid syscall (NR 31): return parent task ID
- kill syscall (NR 32): SIGTERM support, process group kill (negative PID)
- getpgid syscall (NR 33): return process group ID
- exec syscall (NR 34): replace process image with new ELF
- Process groups: pgid field, fork inherits, spawn creates new group
- mount/umount shell commands
- df shell command (disk usage statistics)
- kill shell command (send SIGTERM by PID)
- ipi shell command (IPI self-test)
- ahci/usb shell commands (hardware info)
- ukill ring3 user program (kill/getpid/getppid test)
- nano editor enhanced: Ctrl+S save, Ctrl+G info, Ctrl+F search, proper Enter handling
- HTTP chunked transfer encoding decoder
- IPI (Inter-Processor Interrupt) framework: send_ipi, send_ipi_others, send_ipi_self
- SMP smoke test mode (--smp flag for run-wsl.sh)
- User programs: 30 total (added kill, fileops, udptest)
- Syscalls: 34 total (was 25 at R58)
- Kernel unit tests: 12 total (was 8 at R58)

### Quality (Rounds 59-89)
- Code quality review round R87 (exec name comparison bounds fix)
- Double indirect block design analysis (requires inode v2 format upgrade)
- All post-80 features verified with 38/38 smoke PASS

### Added — Rounds 89-106
- per-CPU runqueue framework (enqueue/dequeue/pop/steal, 16-bit bitmap)
- virtio-net IRQ driver (IRQ 11 registered, ISR status read, RX_PENDING flag)
- `register_irq_handler()` public API for dynamic IRQ registration
- `netstat` shell command (TCP connection status dump)
- `top` shell command (system overview: uptime + memory + tasks + TCP + disk)
- `fslimit` shell command (filesystem limits display)
- `ffind` enhanced with parent/name path display
- `date` command enhanced with weekday/month names (Unix-style format)
- `whoami` enhanced with PID/PPID/PGID/UID/uptime display
- `fileops` enhanced to 8 comprehensive ring3 VFS tests with cleanup
- `ps` enhanced with PGID/PPID columns (serialize_tasks format upgrade to 26 bytes)
- NEO-FS constants exported: SECTOR_SIZE, MAX_INODES, DIRECT_BLOCKS, INDIRECT_PTRS, ROOT_INO
- `inode_name_into()` safe API for reading inode names
- DateTime::weekday_name() and month_name() methods
- Shell commands: 89 total (was 87 at R100)
- Fuzz test assertions: 28 total (was 18 at R75)

### Quality (Rounds 89-106)
- Code quality review round R100 (unreachable pattern fixes, unused variable cleanup)
- Removed static mut NAME_BUF from inode_name (memory safety)
- Fixed fileops cleanup race condition (removed unsafe testdir unlink)
- Fixed duplicate shell command entries (echo/echo2, uptime dedup)
- All features verified with 38/38 smoke PASS

### Added — Rounds 107-121
- sys_rmdir (NR 35): remove empty directory from ring3
- /proc/self/cmdline: show current task program name
- /proc/self/maps: show memory regions (code/data/stack)
- Task.name field: program name stored per-task, inherited on fork, updated on exec
- `netstat` shell command: TCP connection status dump
- `top` shell command: system overview (uptime + memory + tasks + TCP + disk)
- `fslimit` shell command: filesystem limits display
- `hexdump <file>` shell command: kernel hex dump with ASCII sidebar
- `ffind` enhanced: parent/name path display
- `date` enhanced: Unix-style format with weekday/month names
- `whoami` enhanced: PID/PPID/PGID/UID/uptime display
- `nano` enhanced: auto-load nano_save.txt on startup
- `grep` enhanced: multi-file search with filename prefix
- `dd` enhanced: 4 tests (read/write/readback/OOB), safe sector 7000
- `fileops` enhanced: 8 comprehensive VFS tests with unique filenames
- `ps` enhanced: PGID/PPID columns (26-byte serialize format)
- fstest stability fix: increased post-fstest wait to 8 seconds (3/3 stable)
- Syscalls: 35 total (was 34 at R106)
- Shell commands: 89 total
- /proc files: 6 total (self/status, self/cmdline, self/maps, uptime, meminfo, tasks)

### Quality (Rounds 107-121)
- Code quality review round R110 (syscall continuity, program registration, bootloader consistency)
- Code quality review round R120 (Task.name inheritance in fork/exec)
- Fixed dd write test corrupting NEO-FS data area (sector 100 → 7000)
- Fixed fstest intermittent failures (timing: 3s → 8s wait, 3/3 stable)
- Fixed fileops test naming conflicts (testfile → fop_test, testdir → fop_dir)
- All features verified with 38/38 smoke PASS (stable 3/3 runs)

### Added — Rounds 122-137
- /proc/self/maps: memory region display (code/data/stack)
- Task.name field: program name per-task, inherited on fork, updated on exec
- ps enhanced: NAME column showing program name (serialize_tasks 42 bytes/entry)
- echo -n support: print without trailing newline
- fwc command: word count (lines/words/chars) for NEO-FS files
- net throughput test enhanced: ARP cache lookup benchmark (1000 iterations)
- FS stress test enhanced: 7 phases (create/read/rename/truncate/mkdir/rmdir/overwrite)
- Lock stress test enhanced: 5000 spin + 2000 ticket + 1000 try_lock + nested + contention
- Global memory leak detection in test runner (alloc/free balance check)
- sys_rmdir (NR 35): remove empty directory from ring3
- fileops simplified to 2 atomic tests (mkdir+rmdir, create+write+read+unlink)
- ADR-014: Task Name Storage, ADR-015: fstest Timing Fix, ADR-016: Per-CPU Runqueue
- Shell commands: 90 total
- Syscalls: 35 total
- /proc files: 6 total
- ADR records: 16 total
- Smoke test stability: 3/3 PASS consistently

### Quality (Rounds 122-137)
- Code quality review round R130 (Task.name coverage, unused code audit)
- Fixed fileops intermittent failures (simplified to atomic tests, unique short names)
- All rustdoc annotations verified for new modules (dns, ipi, ahci, usb)
- All features verified with 38/38 smoke PASS (stable 3/3 runs)

### Added — Rounds 138-154
- `echo -n` support: print without trailing newline
- `fwc` command: word count (lines/words/chars) for NEO-FS files
- `fls -l` long format: type+perms+size+name (short format adds / suffix for dirs)
- `uptime` enhanced: days/hours/mins adaptive format + running task count
- `help` categorized: System/Filesystem/Networking/User Programs sections
- `alias` no-args: list all defined aliases
- `unalias <name>`: remove alias by name
- `source <file>`: execute script file from NEO-FS (with # comment support)
- `history`: display numbered command history
- `type <cmd>`: show command type (alias/builtin/program/not found)
- `which <cmd>`: locate command (POSIX-style output)
- `export KEY=VALUE`: POSIX-compatible environment variable setting
- `find_prog_by_name()`: public API to check if user program exists
- Shell commands: 96 total (was 89 at R121)
- Smoke test stability: consistently 3/3 PASS

### Quality (Rounds 138-154)
- Code quality review round R140 (warnings audit, syscall continuity)
- Code quality review round R150 (3/3 stability verification)
- All features verified with 38/38 smoke PASS (stable)

## [3.0.0] — 2026-11-07 (handoff baseline)

### Core System
- UEFI bootloader with GOP, self-built identity page table (OVMF 202608 NX workaround)
- Frame allocator, identity paging (0–4 GiB), per-task page tables
- ELF64 loader (PT_LOAD + section-header fallback, RX/RW/RWX permissions)
- Preemptive multitasking (16-slot task table, 100Hz PIT round-robin)
- Ring-3 isolation via CR3 switching
- 11 syscalls (write/exit/uptime/brk/meminfo/read_char/sleep/print_int/net_send/net_recv/tcp_send)
- Interactive shell (36+ commands, help DB, history, tab completion)
- Driver registry framework (serial, PS/2 kbd/mouse, framebuffer, PIT)
- SMP framework (trampoline to long mode, SIPI startup)

### Networking
- PCI enumeration, virtio-net driver (virtqueue TX/RX)
- IPv4 header/checksum, TCP 3-way handshake
- UDP echo server, ping, netdump

### Filesystem
- initrd in-memory archive (ls/cat/write/wc/grep/sort)

### User Programs
- 19 programs: user, userbad, usernx, userexec, userptr, isowrite, heaptest, echo, uptime, meminfo, counter, guess, edit, calc, heapstress, ping, netdump, nano, http
