//! NEO-OS-3 Slab Allocator.
//!
//! Provides efficient fixed-size object allocation from pre-allocated slabs.
//! Each slab cache manages objects of a single size class, backed by the
//! frame allocator for slab pages.
//!
//! Usage:
//!   let cache = SlabCache::new(64);  // 64-byte objects
//!   let ptr = cache.alloc();          // get one object
//!   cache.free(ptr);                  // return it

use crate::mem;

/// Maximum number of objects per slab page (4096 / obj_size).
const SLAB_PAGE_SIZE: usize = 4096;

/// A single slab cache for one object size.
pub struct SlabCache {
    /// Object size in bytes (must be >= 8 and <= SLAB_PAGE_SIZE/2).
    obj_size: usize,
    /// Free list head (linked through first 8 bytes of each free object).
    free_list: *mut u8,
    /// Total allocated objects (for stats).
    total_allocs: usize,
    /// Currently free objects.
    free_count: usize,
}

impl SlabCache {
    /// Create a new slab cache for objects of the given size.
    /// Size is rounded up to minimum 8 bytes and aligned to 8.
    pub fn new(size: usize) -> Self {
        let obj_size = core::cmp::max(size, 8);
        let obj_size = (obj_size + 7) & !7; // align to 8
        Self {
            obj_size,
            free_list: core::ptr::null_mut(),
            total_allocs: 0,
            free_count: 0,
        }
    }

    /// Allocate one object from the cache. Returns null on failure.
    pub fn alloc(&mut self) -> *mut u8 {
        // Try free list first.
        if !self.free_list.is_null() {
            let obj = self.free_list;
            unsafe {
                self.free_list = *(obj as *mut *mut u8);
            }
            self.free_count -= 1;
            self.total_allocs += 1;
            // Zero the object for safety.
            unsafe {
                core::ptr::write_bytes(obj, 0, self.obj_size);
            }
            return obj;
        }

        // Grow: allocate a new slab page from the frame allocator.
        let page = unsafe { mem::frames() }.alloc_contiguous(1);
        if page == 0 {
            return core::ptr::null_mut();
        }

        // Carve the page into objects and link them onto the free list.
        let objs_per_page = SLAB_PAGE_SIZE / self.obj_size;
        // Skip first object — we'll return it directly.
        for i in (1..objs_per_page).rev() {
            let obj = (page + (i * self.obj_size) as u64) as *mut u8;
            unsafe {
                *(obj as *mut *mut u8) = self.free_list;
            }
            self.free_list = obj;
            self.free_count += 1;
        }

        self.total_allocs += 1;
        // Return first object (already zeroed by alloc_contiguous).
        page as *mut u8
    }

    /// Free an object back to the cache.
    ///
    /// # Safety
    /// `ptr` must have been returned by `alloc()` on this cache.
    pub fn free(&mut self, ptr: *mut u8) {
        if ptr.is_null() { return; }
        unsafe {
            *(ptr as *mut *mut u8) = self.free_list;
        }
        self.free_list = ptr;
        self.free_count += 1;
    }

    /// Get cache statistics.
    pub fn stats(&self) -> (usize, usize, usize) {
        (self.obj_size, self.total_allocs, self.free_count)
    }
}

// ---- Pre-defined slab caches for common sizes ----

static mut SLAB_32: Option<SlabCache> = None;
static mut SLAB_64: Option<SlabCache> = None;
static mut SLAB_128: Option<SlabCache> = None;
static mut SLAB_256: Option<SlabCache> = None;

/// Initialize the slab allocator subsystem.
pub fn init() {
    unsafe {
        SLAB_32 = Some(SlabCache::new(32));
        SLAB_64 = Some(SlabCache::new(64));
        SLAB_128 = Some(SlabCache::new(128));
        SLAB_256 = Some(SlabCache::new(256));
    }
    crate::kinfo!("slab: initialized (32/64/128/256 byte caches)");
}

/// Allocate from the best-fit slab cache. Falls back to frame allocator for large sizes.
pub fn slab_alloc(size: usize) -> *mut u8 {
    unsafe {
        if size <= 32 {
            if let Some(ref mut c) = SLAB_32 { return c.alloc(); }
        } else if size <= 64 {
            if let Some(ref mut c) = SLAB_64 { return c.alloc(); }
        } else if size <= 128 {
            if let Some(ref mut c) = SLAB_128 { return c.alloc(); }
        } else if size <= 256 {
            if let Some(ref mut c) = SLAB_256 { return c.alloc(); }
        }
        // Fallback: direct frame allocation for larger sizes.
        let pages = (size + SLAB_PAGE_SIZE - 1) / SLAB_PAGE_SIZE;
        let addr = mem::frames().alloc_contiguous(pages as u64);
        if addr != 0 {
            core::ptr::write_bytes(addr as *mut u8, 0, pages * SLAB_PAGE_SIZE);
        }
        addr as *mut u8
    }
}

/// Free to the appropriate slab cache. For large allocations, caller must use frame free.
///
/// # Safety
/// `ptr` must have been returned by `slab_alloc` with the same or smaller `size`.
pub fn slab_free(ptr: *mut u8, size: usize) {
    if ptr.is_null() { return; }
    unsafe {
        if size <= 32 {
            if let Some(ref mut c) = SLAB_32 { c.free(ptr); return; }
        } else if size <= 64 {
            if let Some(ref mut c) = SLAB_64 { c.free(ptr); return; }
        } else if size <= 128 {
            if let Some(ref mut c) = SLAB_128 { c.free(ptr); return; }
        } else if size <= 256 {
            if let Some(ref mut c) = SLAB_256 { c.free(ptr); return; }
        }
        // Large allocation: free frames directly.
        let pages = (size + SLAB_PAGE_SIZE - 1) / SLAB_PAGE_SIZE;
        mem::frames().free_frames(ptr as u64, (pages * SLAB_PAGE_SIZE) as u64);
    }
}
