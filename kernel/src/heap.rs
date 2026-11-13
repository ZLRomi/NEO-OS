//! M3 kernel heap: intrusive linked-list allocator over frames from
//! [`crate::mem`], exposed as the global allocator.
//!
//! Single CPU + IRQ-safe: every lock disables interrupts (cli/sti pairing),
//! so timer/keyboard handlers can't preempt a half-split list. IRQ handlers
//! still must never allocate (they don't); the cli makes a main-context
//! lock unpreemptible instead of deadlocking on re-entry.

use core::alloc::{GlobalAlloc, Layout};
use core::ptr::null_mut;
use spin::Mutex;

use alloc::{boxed::Box, string::String, vec, vec::Vec};

use crate::cpu::{cli, interrupts_enabled, sti};
use crate::mem::FRAME;
use crate::{kerror, kinfo, kwarn};

/// 1 MiB heap = 256 frames, carved once at boot.
const HEAP_FRAMES: u64 = 256;

struct Hole {
    size: usize,
    next: *mut Hole,
}

struct Heap {
    head: *mut Hole,
}

// Single CPU kernel: the heap is only touched with interrupts disabled
// (boot) or under a rule that IRQ handlers never allocate.
unsafe impl Send for Heap {}

impl Heap {
    const fn empty() -> Heap {
        Heap { head: null_mut() }
    }

    /// # Safety
    ///
    /// `base`/`size` must be a fresh, mapped, unused range owned by us.
    unsafe fn init(&mut self, base: u64, size: usize) {
        let hole = base as *mut Hole;
        unsafe {
            hole.write(Hole {
                size,
                next: null_mut(),
            });
        }
        self.head = hole;
    }

    fn alloc(&mut self, layout: Layout) -> *mut u8 {
        // Header scheme (correct for any align):
        //   hole [start, start+hole_size) -> allocated [start, start+need)
        //   + optional tail. `aligned` leaves 8 bytes for `start` so free()
        //   can recompute `need` from the same layout. All holes stay
        //   8-aligned; `user_size >= 8` guarantees `need >= 16 == sizeof(Hole)`
        //   so every freed block can be re-linked. Tiny remainders (<16)
        //   are skipped, not leaked: the hole stays for a smaller request.
        let align = layout.align().max(8);
        // Round up: tail = aligned+user_size must stay 8-aligned for `Hole`.
        let user_size = layout.size().max(8).next_multiple_of(8);
        let mut prev: *mut *mut Hole = &mut self.head;
        unsafe {
            while !(*prev).is_null() {
                let hole = *prev;
                let start = hole as usize;
                let hole_size = (*hole).size;
                let aligned = (start + 8).next_multiple_of(align);
                let need = (aligned + user_size) - start;
                if hole_size < need {
                    prev = &mut (*hole).next;
                    continue;
                }
                let rest = hole_size - need;
                if rest != 0 && rest < size_of::<Hole>() {
                    // Would orphan `rest` bytes (unfreeable pad). Keep the
                    // hole for a tighter fit instead of leaking.
                    prev = &mut (*hole).next;
                    continue;
                }
                // Commit: stash `start` just before the user pointer.
                ((aligned - 8) as *mut usize).write(start);
                if rest >= size_of::<Hole>() {
                    let tail = (start + need) as *mut Hole;
                    tail.write(Hole {
                        size: rest,
                        next: (*hole).next,
                    });
                    *prev = tail;
                } else {
                    // Exact fit.
                    *prev = (*hole).next;
                }
                return aligned as *mut u8;
            }
        }
        null_mut()
    }

    /// # Safety
    ///
    /// `ptr`/`layout` must be a live allocation from this heap.
    unsafe fn dealloc(&mut self, ptr: *mut u8, layout: Layout) {
        let align = layout.align().max(8);
        // Round up: tail = aligned+user_size must stay 8-aligned for `Hole`.
        let user_size = layout.size().max(8).next_multiple_of(8);
        unsafe {
            let aligned = ptr as usize;
            let start = *((aligned - 8) as *const usize);
            // Recompute the identical `need` from alloc time. `start` is
            // trusted (we wrote it); sanity-check it before linking.
            debug_assert!(start + 8 <= aligned);
            debug_assert!((aligned - (start + 8)) % align == 0 || align == 8);
            let need = (aligned + user_size) - start;
            debug_assert!(need >= size_of::<Hole>());
            let hole = start as *mut Hole;
            hole.write(Hole {
                size: need,
                next: self.head,
            });
            self.head = hole;
        }
        self.coalesce();
    }

    /// Merge adjacent holes (single linear pass; heap is tiny).
    fn coalesce(&mut self) {
        unsafe {
            let mut cur = self.head;
            while !cur.is_null() {
                let mut nxt = (*cur).next;
                // Find a hole starting exactly where `cur` ends.
                let mut prev_next: *mut *mut Hole = &mut (*cur).next;
                while !nxt.is_null() {
                    let cur_end = (cur as usize) + (*cur).size;
                    if nxt as usize == cur_end {
                        (*cur).size += (*nxt).size;
                        *prev_next = (*nxt).next;
                        nxt = *prev_next;
                    } else {
                        prev_next = &mut (*nxt).next;
                        nxt = *prev_next;
                    }
                }
                cur = (*cur).next;
            }
        }
    }

    /// Walk the free list: (free_bytes, largest_hole, hole_count).
    /// Caller must hold the lock (see [`stats`]).
    fn stats_locked(&self) -> (usize, usize, usize) {
        let mut free = 0usize;
        let mut largest = 0usize;
        let mut n = 0usize;
        unsafe {
            let mut cur = self.head;
            while !cur.is_null() {
                let s = (*cur).size;
                free += s;
                if s > largest {
                    largest = s;
                }
                n += 1;
                cur = (*cur).next;
            }
        }
        (free, largest, n)
    }
}

struct LockedHeap(Mutex<Heap>);

impl LockedHeap {
    const fn empty() -> LockedHeap {
        LockedHeap(Mutex::new(Heap::empty()))
    }
}

unsafe impl GlobalAlloc for LockedHeap {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        // IRQ-safe: cli around the list surgery so a timer/keyboard IRQ
        // can't preempt a half-split list. IRQ handlers must still never
        // allocate (they'd spin with IF=0 if main holds the lock).
        let was = interrupts_enabled();
        cli();
        let p = self.0.lock().alloc(layout);
        if was {
            sti();
        }
        p
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        let was = interrupts_enabled();
        cli();
        self.0.lock().dealloc(ptr, layout);
        if was {
            sti();
        }
    }
}

#[global_allocator]
static ALLOCATOR: LockedHeap = LockedHeap::empty();

/// Shell `heap` command: (heap_bytes, free_bytes, largest_hole, holes).
pub fn stats() -> (usize, usize, usize, usize) {
    let total = (HEAP_FRAMES * FRAME) as usize;
    let was = interrupts_enabled();
    cli();
    let (free, largest, n) = ALLOCATOR.0.lock().stats_locked();
    if was {
        sti();
    }
    (total, free, largest, n)
}

/// OOM is fatal and now visible: previously no handler existed, so an
/// exhausted heap would fail to link / trap obscurely. Prints layout over
/// serial+screen (no allocation) then halts with a red frame.
#[alloc_error_handler]
fn oom(layout: Layout) -> ! {
    kerror!("heap OOM: size={} align={}", layout.size(), layout.align());
    crate::console::frame(0x00C00000);
    loop {
        crate::cpu::hlt();
    }
}

/// Carve the heap out of fresh frames and run smoke tests.
pub fn init() {
    let base = unsafe { crate::mem::frames() }.alloc_contiguous(HEAP_FRAMES);
    let size = (HEAP_FRAMES * FRAME) as usize;
    unsafe { ALLOCATOR.0.lock().init(base, size) };
    kinfo!("heap: {} KiB @ {:#x}", size / 1024, base);

    // Smoke test the whole path: Box, Vec growth, String, free+reuse.
    let b = Box::new(0x4E45_4F33u32);
    kinfo!("heap: box={:#x}", *b);
    drop(b);

    let mut v: Vec<u64> = Vec::new();
    for i in 0..1000u64 {
        v.push(i * i);
    }
    let sum: u64 = v.iter().sum();
    kinfo!("heap: vec len={} sum={} (expect 332833500)", v.len(), sum);
    assert!(sum == 332833500);
    drop(v);

    let s = String::from("neo-heap-ok");
    kinfo!("heap: str={}", s);

    let big = vec![7u8; 65536];
    kinfo!("heap: big len={} first={}", big.len(), big[0]);
    if crate::mem::translate(big.as_ptr() as u64) != Some(big.as_ptr() as u64) {
        kwarn!("heap: translate mismatch (expected identity)");
    } else {
        kinfo!("heap: translate self-test ok");
    }
    drop(big);

    // Over-aligned alloc regression: old pad-leak scheme lost `pad` bytes
    // here and fragmented the heap. Header scheme must round-trip exact.
    #[repr(align(32))]
    struct Aligned32([u8; 32]);
    let a = Box::new(Aligned32([0xA5; 32]));
    assert!((a.as_ref() as *const Aligned32 as usize) % 32 == 0);
    assert!(a.0[0] == 0xA5 && a.0[31] == 0xA5);
    kinfo!("heap: align32 ok @ {:#x}", a.as_ref() as *const _ as u64);
    drop(a);

    // Free+reuse: freed 64K + align32 must coalesce back for a new big vec.
    let reuse = vec![9u8; 65536];
    kinfo!("heap: reuse len={} first={}", reuse.len(), reuse[0]);
    assert!(reuse[65535] == 9);
}
