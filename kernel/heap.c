#include "heap.h"
#include "memory.h"
#include "printk.h"
#include <stddef.h>
#include <stdint.h>

// 内存块头部（8 字节）
typedef struct block_header {
    size_t size;                 // 数据区大小（不含头部）
    struct block_header* next;   // 下一个空闲块
    uint32_t magic;              // 魔术数 0xDEADBEEF 用于校验
} block_header_t;

#define MAGIC_NUMBER 0xDEADBEEF
#define HEAP_START   0xC0000000   // 堆起始虚拟地址（必须已经映射）
#define PAGE_SIZE    4096

static block_header_t* free_list = NULL;
static uint32_t heap_brk = HEAP_START;   // 当前堆顶虚拟地址
static int s_heap_initialized = 0;

// 分配一个新物理页并映射到 heap_brk，返回虚拟地址
static void* heap_alloc_page(void) {
    uint32_t phys = pmm_alloc_page();
    if (!phys) {
        printk("heap: pmm_alloc_page failed\n");
        return NULL;
    }
    if (!map_page(heap_brk, phys, PAGE_PRESENT | PAGE_WRITE)) {
        printk("heap: map_page failed for 0x%x\n", heap_brk);
        // 释放已分配的物理页，避免内存泄漏
        pmm_free_page(phys);
        return NULL;
    }
    void* virt = (void*)heap_brk;
    heap_brk += PAGE_SIZE;
    return virt;
}

// 合并相邻空闲块
static void merge_blocks(block_header_t* block) {
    while (block->next && (uint8_t*)block + sizeof(block_header_t) + block->size == (uint8_t*)block->next) {
        block->size += sizeof(block_header_t) + block->next->size;
        block->next = block->next->next;
    }
}

// 初始化堆（分配第一页）
void heap_init(void) {
    void* first_page = heap_alloc_page();
    if (!first_page) {
        printk("heap_init: allocation failed, heap disabled\n");
        return;
    }
    block_header_t* block = (block_header_t*)first_page;
    block->size = PAGE_SIZE - sizeof(block_header_t);
    block->next = NULL;
    block->magic = MAGIC_NUMBER;
    free_list = block;
    s_heap_initialized = 1;
    printk("Heap initialized at 0x%x\n", (uint32_t)first_page);
}

// 分配内存（首次适应）
void* kmalloc(size_t size) {
    if (size == 0) return NULL;
    size = (size + 7) & ~7;  // 8 字节对齐

    block_header_t* prev = NULL;
    block_header_t* curr = free_list;

    while (curr) {
        if (curr->magic != MAGIC_NUMBER) {
            // 损坏检测，跳过
            curr = curr->next;
            continue;
        }
        if (curr->size >= size) {
            // 找到合适块
            if (curr->size >= size + sizeof(block_header_t) + 8) {
                // 分裂：curr 分配给调用者，剩余部分 new_block 替换 curr 在空闲链中的位置
                block_header_t* new_block = (block_header_t*)((uint8_t*)curr + sizeof(block_header_t) + size);
                new_block->size = curr->size - size - sizeof(block_header_t);
                new_block->next = curr->next;
                new_block->magic = MAGIC_NUMBER;
                curr->size = size;
                if (prev) prev->next = new_block;
                else free_list = new_block;
            } else {
                // 整块分配，从空闲链中移除
                if (prev) prev->next = curr->next;
                else free_list = curr->next;
            }
            curr->magic = MAGIC_NUMBER;
            return (void*)((uint8_t*)curr + sizeof(block_header_t));
        }
        prev = curr;
        curr = curr->next;
    }

    // 无空闲块，按需分配连续物理页
    size_t total_needed = size + sizeof(block_header_t);
    uint32_t pages_needed = (uint32_t)((total_needed + PAGE_SIZE - 1) / PAGE_SIZE);
    if (pages_needed < 1) pages_needed = 1;

    void* first_page = heap_alloc_page();
    if (!first_page) return NULL;
    for (uint32_t i = 1; i < pages_needed; i++) {
        if (!heap_alloc_page()) {
            // 分配失败时无法优雅回滚，记录并返回
            printk("heap: failed to allocate %u pages\n", pages_needed);
            return NULL;
        }
    }

    block_header_t* new_block = (block_header_t*)first_page;
    new_block->size = (size_t)pages_needed * PAGE_SIZE - sizeof(block_header_t);
    new_block->next = NULL;
    new_block->magic = MAGIC_NUMBER;

    // 插入空闲链表末尾
    if (free_list) {
        block_header_t* last = free_list;
        while (last->next) last = last->next;
        last->next = new_block;
    } else {
        free_list = new_block;
    }

    // 递归调用分配（新块足够大，最多分裂一次）
    return kmalloc(size);
}

// 释放内存
void kfree(void* ptr) {
    if (!ptr) return;
    block_header_t* block = (block_header_t*)((uint8_t*)ptr - sizeof(block_header_t));
    if (block->magic != MAGIC_NUMBER) {
        printk("kfree: invalid magic at 0x%x\n", (uint32_t)block);
        return;
    }
    // 插入空闲链表头部
    block->next = free_list;
    free_list = block;
    // 合并相邻空闲块
    block_header_t* curr = free_list;
    while (curr) {
        merge_blocks(curr);
        curr = curr->next;
    }
}

int heap_is_initialized(void) {
    return s_heap_initialized;
}
