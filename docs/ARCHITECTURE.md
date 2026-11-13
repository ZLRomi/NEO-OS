# NEO-OS-3 Architecture

This document describes the **operating system** architecture, not just the
kernel. It is the contract the code is expected to honour; when code and this
document disagree, the document is the target and the code is the bug.

## 1. Why this document exists

NEO-OS-3 started as a kernel: a single ring-0 program that happened to load
user code. A kernel becomes an *operating system* when it provides a stable
boundary with the following properties:

1. **A process abstraction with real semantics.** `fork` returns twice, `exec`
   replaces an image, `wait` blocks, `exit` leaves a corpse to be reaped.
2. **A scheduler that knows about waiting.** A process that cannot make
   progress must not consume CPU; the scheduler must own the notion of
   "blocked until event E".
3. **A user-space runtime.** Programs must not hand-roll `int 0x80` sequences
   or private allocators. There is one ABI, one runtime.
4. **System services outside the kernel.** The shell and init run at ring 3, so
   a bug there is a process crash, not a kernel panic.
5. **A verifiable build.** Anyone can clone, build and boot it on CI.

Items 1-4 are what this architecture lays out.

## 2. Layers

```text
+----------------------------------------------------------------------+
| Ring 3 |  applications              - src in user/src/*                 |
|        +--------------------------------------------------------------+
|        |  ulib  (user runtime: syscall wrappers, malloc, formatting)   |
+--------+--------------------------------------------------------------+
| ABI    |  neo_common::syscall - numbers, errno, structs (shared)       |
+--------+--------------------------------------------------------------+
| Ring 0 |  syscall dispatcher -> process, vfs, net, mem subsystems      |
|        |  drivers (virtio-blk, virtio-net, PS/2, PIT, framebuffer)     |
|        |  core (paging, frame allocator, GDT/IDT, scheduler)           |
+--------+--------------------------------------------------------------+
```

The arrow only points one way: ring 3 calls ring 0 through the syscall gate.
Nothing in ring 0 touches a user address space except through the validated
`user_range_ok` path.

## 3. Process model

### 3.1 States

```text
              spawn
                |
                v
   +--------> Ready <----------+
   |            |               |
   |       scheduled       wake (timer / explicit)
   |            v               |
   |         Running -----------+
   |            |
   |         blocks
   |            v
   |        Sleeping -----------+
   |            |
   |      exit()|
   |            v
   +-------  ExitPend --(parent waits)--> Empty (reaped)
```

`ExitPend` is the zombie state: the task has finished but its exit status must
stay observable until the parent calls `wait`. `Empty` is the only state a new
task may reuse.

### 3.2 Blocking contract

`__switch_to` saves only `rsp`; every other register lives on the task's own
kernel stack. Consequently **a task blocks on its own stack** and the timer
preempts it. There is no separate idle context to switch to.

`sleep_current(ticks)` therefore:

1. records an absolute deadline in the sleep table,
2. sets the task's state to `Sleeping`,
3. re-enables interrupts and `hlt`s until the timer promotes it back to
   `Ready` and the scheduler switches it in,
4. re-disables interrupts so `do_syscall`'s `iretq` sees a consistent state.

This fixes a real deadlock class: `int 0x80` is an **interrupt gate**, so IF is
clear on entry. Any syscall that spins waiting for a tick counter can never
observe that counter advance.

### 3.3 Address-space layout

| Region | Virtual range | Protection |
|---|---|---|
| program image | `0x0080_0000` upward | RX (text) / RW+NX (data, bss) |
| user stack | `USER_STACK_BASE..USER_STACK_TOP` (`0x7FFF_0000`-`0x8000_0000`) | RW+NX |
| kernel | wherever the identity map puts it | supervisor only |

The stack has a **fixed virtual address**, independent of the physical frames
backing it. That is what makes `fork` meaningful: parent and child share a
layout but not storage.

## 4. System call ABI

Defined once in `common/src/syscall.rs` and consumed by both sides.

* `RAX` = number in, result out.
* `RDI`, `RSI`, `RDX` = arguments.
* A result in `[-4095, -1]` is `-errno`.
* Every pointer argument is a `(ptr, len)` pair validated against the caller's
  page tables before the kernel touches it.

Adding a syscall:

1. Add the number and any errno to `common/src/syscall.rs`.
2. Implement a case in `do_syscall`, validating *all* user pointers first.
3. Add a wrapper in `user/src/ulib.rs`.
4. Add a smoke assertion that exercises it end to end.

## 5. Build and verification

```bash
./tools/mkesp.sh --ci      # build everything, assemble esp.img
./tools/run.sh --smoke     # boot under QEMU and assert boot markers
```

Build-time tools beyond a Rust toolchain: `nasm` (assembles the AP
trampoline), `mtools` and `dosfstools` (pack `esp.img`), `objcopy`,
`qemu-system-x86_64` and OVMF firmware (boot test). CI installs all of them.

CI (`.github/workflows/build.yml`) runs both on every push. The smoke test is a
**behavioural** gate: it asserts that ring-3 programs report their own verdicts
(for example `userptr: invalid buffers rejected`), not merely that the kernel
printed a banner.

## 6. Known gaps

Tracked so they are not mistaken for finished work:

* ~~`fork` does not return 0 in the child~~ — fixed. The child now resumes from
  the parent's duplicated syscall frame with RAX=0, and `waitpid` reaps the
  zombie. Verified by the `forktest` program (child sees 0, parent sees the pid,
  COW isolation holds).
* `mmap` is a stub returning `ENOSYS`.
* SMP: APs are detected but never scheduled; the run queue is single-CPU.
* The shell still runs in ring 0 and must move to a ring-3 service.
* No user/group identity, so permission bits are stored but unenforced.
* `exec` has no behavioural test yet; `forktest` covers fork/wait/exit only.
* The file descriptor table is a single global array of 32 entries keyed by
  task id, not a per-process table with reference counts. It is cleaned up on
  exit and on exec, but sharing a file between processes is not modelled.
