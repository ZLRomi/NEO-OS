#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>

// ----- 物理内存管理（PMM） -----
void pmm_init(void);
uint32_t pmm_alloc_page(void);
void pmm_free_page(uint32_t phys_addr);
uint32_t pmm_get_total_pages(void);
uint32_t pmm_get_free_pages(void);

// ----- 分页常量 -----
#define PAGE_PRESENT 0x1
#define PAGE_WRITE   0x2
#define PAGE_USER    0x4
#define PAGE_PWT     0x8
#define PAGE_PCD     0x10

// ----- 分页管理（递归映射） -----
void paging_init(void);
int map_page(uint32_t virt, uint32_t phys, uint32_t flags);
uint32_t vmm_alloc_page(void);
uint32_t get_kernel_pd_phys(void);

// 为设备 DMA 分配一页物理内存，并返回虚拟地址（phys_out 可选回物理地址）
uint32_t dma_alloc_page(uint32_t* phys_out);

// 将 MMIO 物理区域映射到虚拟地址空间，返回基址
uint32_t mmio_map(uint32_t phys, uint32_t pages);

// ----- 初始化入口和显示 -----
void memory_init(uint32_t mb_info_addr);
int memory_is_initialized(void);
void print_memory_info(void);
uint32_t memory_get_total_kb(void);

// ----- VESA 图形模式 -----
void vesa_init(void);
uint32_t vesa_get_fb_addr(void);    // 返回帧缓冲区的虚拟地址
uint32_t vesa_get_width(void);
uint32_t vesa_get_height(void);
uint32_t vesa_get_pitch(void);      // 每行字节数（Scanline）

// ----- VBE 高分辨率图形（Bochs VBE） -----
int vbe_init(uint32_t width, uint32_t height, uint32_t bpp);
void vbe_putpixel(int x, int y, uint32_t color);
void vbe_fillrect(int x, int y, int w, int h, uint32_t color);
uint32_t vbe_get_fb(void);
uint32_t vbe_get_width(void);
uint32_t vbe_get_height(void);
uint32_t vbe_get_pitch(void);
void vbe_disable(void);

#endif
