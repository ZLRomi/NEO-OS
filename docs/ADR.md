# Architecture Decision Records (ADR)

Key design decisions for NEO-OS-3, with context and rationale.

---

## ADR-001: Layered Filesystem Architecture

**Date**: 2026-11-08  
**Status**: Accepted

**Context**: The original system had only an in-memory initrd archive. We needed persistent storage on virtio-blk.

**Decision**: Three-layer architecture:
1. `block.rs` — Hardware-independent block device abstraction (register/read/write)
2. `vfs.rs` — Virtual filesystem layer (fd table, open/close/read/write/stat)
3. `neofs.rs` — NEO-FS implementation (inode/bitmap/superblock on block device)

**Rationale**: Separation of concerns allows swapping filesystem implementations without changing VFS or drivers. Block abstraction enables future AHCI/USB storage support.

**Consequences**: Slight overhead from indirection layers. All file operations go through VFS even for simple reads.

---

## ADR-002: Virtqueue Ring Offset Convention

**Date**: 2026-11-08  
**Status**: Accepted (bug fix)

**Context**: Original virtio-net driver used `-2` offset from avail/used idx pointers to reach ring arrays. This was incorrect per the virtio spec.

**Decision**: Avail/used ring arrays start at `idx_ptr + 2` (after the 4-byte header: flags[2] + idx[2]). Changed all `-2` to `+2`.

**Rationale**: Per virtio 0.9.5 spec, `struct virtq_avail { le16 flags; le16 idx; le16 ring[N]; }`. The `idx` field is at offset +2, and `ring[]` starts at offset +4. Since our pointer points to `idx`, ring starts at pointer + 2.

**Consequences**: Fixed both virtio-net and virtio-blk. Previous code worked by accident due to QEMU's lenient implementation.

---

## ADR-003: NEO-FS Inode Layout (64 bytes)

**Date**: 2026-11-08  
**Status**: Accepted

**Context**: Need a compact on-disk inode that fits 8 per sector (512/64).

**Decision**:
```
Offset  Size  Field
0       4     mode (low 4 bits = type, high 12 bits = rwxrwxrwx)
4       4     size (bytes)
8       32    blocks[8] (1-based direct block pointers, 0=unused)
40      16    name (null-padded, max 16 chars)
56      4     parent_ino (directory hierarchy)
60      4     indirect_block (1-based, 0=none)
```

**Rationale**: 64 bytes gives good density (8 inodes/sector). Mode encoding combines type + permissions in one field. 1-based block numbers allow 0 as "unused" sentinel. Parent inode enables directory trees without separate directory entries.

**Consequences**: Max filename 16 chars. Max file ~68KB (8 direct + 128 indirect × 512B). No extended attributes. Single-sector bitmap limits to 4096 data blocks (~2MB usable).

---

## ADR-004: Syscall Interface (int 0x80)

**Date**: 2026-11-08  
**Status**: Accepted

**Context**: Need a mechanism for ring-3 programs to request kernel services.

**Decision**: Use `int 0x80` with RAX=syscall number, RDI/RDX/RSI=args, return in RAX. IDT gate has DPL=3.

**Rationale**: Simplest x86-64 syscall mechanism. No SYSCALL/SYSRET MSR setup needed. Compatible with all x86-64 CPUs. The existing IDT infrastructure already supports it.

**Consequences**: Slower than SYSCALL instruction (~100ns vs ~50ns). Limited to 3 arguments. Acceptable for this OS scale.

---

## ADR-005: Shell Script Interpreter Design

**Date**: 2026-11-08  
**Status**: Accepted

**Context**: Need basic scripting support (if/while/;) without a full POSIX shell.

**Decision**: Line-collecting approach: `if`/`while` starts collection mode, lines stored in global `SCRIPT_LINES[32]`, `fi`/`done` triggers execution. Condition evaluated by running command and checking output for error keywords.

**Rationale**: Avoids complex parser/tokenizer. Global buffer avoids stack allocation issues (kernel stack is limited). Error-keyword detection is simpler than exit codes (our tasks don't have proper exit status propagation).

**Consequences**: Max 32 lines per block. No nested if/while. Condition evaluation is heuristic (string matching). Not POSIX compatible.

---

## ADR-006: kprint Redirect Mechanism

**Date**: 2026-11-08  
**Status**: Accepted

**Context**: Shell output redirection (`cmd > file`) needs to capture kprint output.

**Decision**: Global atomic flag + static buffer. `redirect_start()` sets flag, `kprint!` macro calls `redirect_print()` which appends to buffer when active. `redirect_stop()` returns captured bytes.

**Rationale**: Minimal invasiveness — only changes kprint macro, not every command. Uses `format_args!` which can be consumed multiple times. AtomicBool ensures visibility across contexts.

**Consequences**: 2KB buffer limit per redirect. Not thread-safe for concurrent kprint from multiple cores (acceptable since redirect only used from BSP shell). Format args consumed 3× (serial + console + redirect).

---

## ADR-007: SMP Trampoline Patch Strategy

**Date**: 2026-11-08  
**Status**: Accepted

**Context**: AP trampoline runs at fixed address 0x8000. BSP must patch runtime addresses (CR3, stack, entry point) into the trampoline binary.

**Decision**: Embed `movabs rax, 0xDEADBEEFCAFEBABE` placeholder instructions at known offsets. BSP overwrites the 8-byte immediate values. Stack calculation done in trampoline: `rsp = base + (apic_id + 1) * stack_size`.

**Rationale**: Avoids position-independent code complexity. Fixed offsets are verified against assembled binary. Per-AP stack calculation in trampoline avoids needing separate trampolines per core.

**Consequences**: Offsets must match exactly between ASM source and Rust patch code. Changes to trampoline ASM require updating patch constants. Verified via hex dump of assembled binary.

---

## ADR-008: Smoke Test Disk Reset Policy

**Date**: 2026-11-08  
**Status**: Accepted

**Context**: Intermittent fstest failures caused by stale NEO-FS state on reused disk image.

**Decision**: In smoke test mode, always recreate `/tmp/neo3-blk.img` with zeros before each run. Non-smoke runs preserve the disk for interactive testing.

**Rationale**: Ensures deterministic test environment. mkfs creates fresh filesystem each run. Eliminates cross-run state pollution.

**Consequences**: Slightly slower smoke test startup (~50ms for dd). Interactive debugging requires manual disk management.

---

## ADR-009: COW Fork Implementation

**Date**: 2026-11-09  
**Status**: Accepted

**Context**: fork() requires duplicating a process's address space. Eagerly copying all pages is expensive. Copy-on-Write (COW) defers the copy until a write occurs.

**Decision**: Use PTE bit 9 (software-available) as COW marker. On fork:
1. Clone page table tree (share physical pages)
2. Mark data pages (0x800000–0x9FFFFF) as COW in both parent and child (remove W, set bit 9)
3. On #PF with write+present+COW: allocate new page, copy content, remap as RW

**Rationale**: Bit 9 is unused by x86-64 hardware. Avoids adding metadata structures. Single #PF handler handles both COW and demand paging.

**Consequences**: First write to shared page costs one page allocation + memcpy. Memory usage doubles only for modified pages.

---

## ADR-010: Demand Paging via #PF Handler

**Date**: 2026-11-09  
**Status**: Accepted

**Context**: User programs may access heap memory beyond pre-mapped regions. Pre-allocating all possible heap pages wastes memory.

**Decision**: Handle not-present faults in user heap region (0x800000–0x9FFFFF) by allocating a zero page on demand. Only handle not-present faults (code bit 0 = 0), not permission violations.

**Rationale**: Lazy allocation reduces initial memory footprint. Simple implementation — no VMA tracking needed since user address space layout is fixed.

**Consequences**: First access to unmapped heap page triggers #PF + allocation. Permission faults still kill the task (security boundary preserved).

---

## ADR-011: TCP Multi-Connection Table

**Date**: 2026-11-09  
**Status**: Accepted

**Context**: Original TCP stack used a single global connection. Multiple concurrent connections (e.g., HTTP + DNS) require per-connection state.

**Decision**: Replace `TCP_CONN` singleton with `TCP_CONNS[4]` array. Route incoming segments by 4-tuple (src_ip, src_port, dst_ip, dst_port). Public API functions operate on first active connection for backward compatibility.

**Rationale**: Fixed-size table avoids dynamic allocation. 4 connections sufficient for embedded use. Backward-compatible API preserves existing code.

**Consequences**: Maximum 4 concurrent TCP connections. Linear scan for connection lookup (acceptable for small N).

---

## ADR-012: /proc Virtual Filesystem

**Date**: 2026-11-09  
**Status**: Accepted

**Context**: User programs need runtime system information (process list, memory stats, uptime) without dedicated syscalls for each.

**Decision**: Intercept `/proc/` paths in `vfs_open()`. Generate content dynamically on read. Supported files: `self/status`, `uptime`, `meminfo`, `tasks`.

**Rationale**: Follows Linux convention. No disk I/O. Content generated at read time ensures freshness. Extensible by adding new proc types.

**Consequences**: Adds ~200 lines to vfs.rs. Read-only (writes return error). Buffer limited to 256 bytes per file.

---

## ADR-013: Double Indirect Block Design (Deferred)

**Date**: 2026-11-09  
**Status**: Deferred

**Context**: Current NEO-FS max file size is ~68KB (8 direct + 128 indirect blocks). Larger files require double indirect blocks.

**Decision**: Deferred to next inode format version. Adding double indirect requires either:
- Option A: Repurpose indirect block index 0 as double-indirect pointer (breaks backward compatibility)
- Option B: Add `double_indirect_block: u32` field to inode (requires inode v2 format, mkfs upgrade)

**Rationale**: Option A breaks existing filesystems. Option B requires coordinated format upgrade. Neither is safe without a migration path.

**Consequences**: Max file size remains ~68KB until inode format upgrade is implemented. The double indirect read/write logic was prototyped and verified but reverted for safety.

---

## ADR-014: Task Name Storage for /proc

**Date**: 2026-11-09  
**Status**: Accepted

**Context**: /proc/self/cmdline needs to return the program name, but tasks had no name field.

**Decision**: Add `name: [u8; 16]` field to Task struct. Set on spawn (from program name), inherited on fork, updated on exec. Kernel tasks get zeroed name.

**Rationale**: Fixed-size array avoids heap allocation. 16 bytes matches MAX_NAME_LEN. Null-terminated for C compatibility.

**Consequences**: Task struct grows by 16 bytes. All spawn paths must initialize name. Fork inherits parent name (correct for fork-before-exec pattern).

---

## ADR-015: fstest Timing Fix

**Date**: 2026-11-09  
**Status**: Accepted

**Context**: fstest intermittently failed in smoke test (~33% failure rate) with "FAIL write" or "FAIL stat" errors. Root cause: ring3 task still running when shell sent next command, causing VFS race conditions.

**Decision**: Increase post-fstest wait from 3s to 8s in smoke test script. Also use unique filenames in fileops (fop_test/fop_dir) to avoid cross-test interference.

**Rationale**: Ring3 tasks run asynchronously. Shell returns prompt immediately after spawn. Longer wait ensures task completes before next command. Unique names prevent stale state.

**Consequences**: Smoke test takes ~5s longer. Stability improved from ~67% to 100% (3/3 runs pass).

---

## ADR-016: Per-CPU Runqueue Framework (Not Yet Active)

**Date**: 2026-11-09  
**Status**: Proposed

**Context**: Current scheduler uses global TASKS array with single CURRENT index. Multi-core requires per-CPU scheduling.

**Decision**: Implemented per-CPU runqueue as 16-bit bitmap in CpuData. API: enqueue/dequeue/pop/steal. NOT yet integrated into spawn or scheduler — attempting integration caused fstest timing regressions.

**Rationale**: Bitmap is lock-free for single-writer (each CPU writes only its own queue). Work stealing enables load balancing. Integration deferred until scheduler is refactored for per-CPU current task.

**Consequences**: Framework code exists but is unused. Activation requires: (1) per-CPU CURRENT variable, (2) scheduler reads from local runqueue, (3) spawn distributes tasks across CPUs.
