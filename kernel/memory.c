#include "memory.h"
#include "printk.h"
#include "serial.h"
#include <stdint.h>

// ----- Bochs VBE I/O 端口 -----
#define VBE_DISPI_IOPORT_INDEX  0x01CE
#define VBE_DISPI_IOPORT_DATA   0x01CF

#define VBE_DISPI_INDEX_ID              0
#define VBE_DISPI_INDEX_XRES            1
#define VBE_DISPI_INDEX_YRES            2
#define VBE_DISPI_INDEX_BPP             3
#define VBE_DISPI_INDEX_ENABLE          4
#define VBE_DISPI_INDEX_BANK            5
#define VBE_DISPI_INDEX_VIRT_WIDTH      6
#define VBE_DISPI_INDEX_VIRT_HEIGHT     7
#define VBE_DISPI_INDEX_X_OFFSET        8
#define VBE_DISPI_INDEX_Y_OFFSET        9
#define VBE_DISPI_INDEX_COUNT           10

#define VBE_DISPI_ENABLED               0x01
#define VBE_DISPI_NOCLEARMEM            0x02
#define VBE_DISPI_LFB_ENABLED           0x04

// 默认显存物理地址（Bochs VBE 固定为 0xE0000000，QEMU 通常也使用）
#define VBE_DISPI_LFB_PHYSICAL          0xE0000000

static uint32_t vbe_fb_virt = 0;
static uint32_t vbe_width = 0;
static uint32_t vbe_height = 0;
static uint32_t vbe_bpp = 0;
static uint32_t vbe_pitch = 0;

// ----- VBE 模式信息结构（从 GRUB 传递） -----
typedef struct {
    uint16_t attributes;
    uint8_t winA, winB;
    uint16_t granularity;
    uint16_t winsize;
    uint16_t segmentA, segmentB;
    uint32_t realFctPtr;
    uint16_t pitch;                 // 每行字节数
    uint16_t width, height;
    uint8_t wchar[4], charsize, planes, bpp, banks;
    uint8_t memory_model, bank_size, image_pages;
    uint8_t reserved0;
    uint8_t red_mask, red_position;
    uint8_t green_mask, green_position;
    uint8_t blue_mask, blue_position;
    uint8_t rsvd_mask, rsvd_position;
    uint8_t directcolor_attributes;
    uint32_t phys_base;             // 帧缓冲区物理地址
    uint32_t reserved1;
    uint16_t reserved2;
} __attribute__((packed)) vbe_mode_info_t;

// ========== Multiboot 信息结构 ==========
typedef struct {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    // ----- VBE 相关字段 -----
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
} __attribute__((packed)) multiboot_info_t;

typedef struct {
    uint32_t size;
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
} __attribute__((packed)) mmap_entry_t;

static multiboot_info_t* mb_info = 0;

// ========== PMM（物理内存管理） ==========
#define PAGE_SIZE 4096
#define BITMAP_SIZE (1024 * 1024 / 8)
static uint8_t pmm_bitmap[BITMAP_SIZE];
static uint32_t total_pages = 0;

void pmm_init(void) {
    if (!mb_info) return;
    for (int i = 0; i < BITMAP_SIZE; i++) pmm_bitmap[i] = 0xFF;

    uint64_t total_mem_kb = mb_info->mem_upper + 1024;
    uint64_t total_mem_bytes = total_mem_kb * 1024;
    if (total_mem_bytes > 0xFFFFFFFF) total_mem_bytes = 0xFFFFFFFF;

    uint32_t start_addr = 0x100000;
    uint32_t end_addr = (uint32_t)total_mem_bytes;
    if (end_addr > start_addr) {
        uint32_t start_page = start_addr / PAGE_SIZE;
        uint32_t page_count = (end_addr - start_addr) / PAGE_SIZE;
        uint32_t max_bit = BITMAP_SIZE * 8;
        for (uint32_t i = 0; i < page_count && (start_page + i) < max_bit; i++) {
            uint32_t bit = start_page + i;
            pmm_bitmap[bit / 8] &= ~(1 << (bit % 8));
        }
    }

    // 保护整个内核镜像区域（代码+数据+BSS），防止 pmm 分配内核占用的物理页
    extern uint32_t _end;
    uint32_t kernel_end = (uint32_t)&_end;
    kernel_end = (kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint32_t addr = 0x100000; addr < kernel_end; addr += PAGE_SIZE) {
        uint32_t bit = addr / PAGE_SIZE;
        if (bit < BITMAP_SIZE * 8)
            pmm_bitmap[bit / 8] |= (1 << (bit % 8));
    }

    total_pages = 0;
    for (int i = 0; i < BITMAP_SIZE; i++)
        for (int b = 0; b < 8; b++)
            if ((pmm_bitmap[i] & (1 << b)) == 0) total_pages++;
}

uint32_t pmm_alloc_page(void) {
    for (int i = 0; i < BITMAP_SIZE; i++) {
        if (pmm_bitmap[i] != 0xFF) {
            for (int b = 0; b < 8; b++) {
                if ((pmm_bitmap[i] & (1 << b)) == 0) {
                    pmm_bitmap[i] |= (1 << b);
                    return (i * 8 + b) * PAGE_SIZE;
                }
            }
        }
    }
    return 0;
}

void pmm_free_page(uint32_t phys_addr) {
    if (phys_addr % PAGE_SIZE) return;
    uint32_t bit = phys_addr / PAGE_SIZE;
    if (bit >= BITMAP_SIZE * 8) return;
    pmm_bitmap[bit / 8] &= ~(1 << (bit % 8));
}

uint32_t pmm_get_total_pages(void) {
    return total_pages;
}

uint32_t pmm_get_free_pages(void) {
    uint32_t free = 0;
    for (int i = 0; i < BITMAP_SIZE; i++) {
        if (pmm_bitmap[i] == 0) { free += 8; continue; }
        if (pmm_bitmap[i] == 0xFF) continue;
        for (int b = 0; b < 8; b++) {
            if ((pmm_bitmap[i] & (1 << b)) == 0) free++;
        }
    }
    return free;
}

// ========== 分页管理（递归映射） ==========
static uint32_t pd_phys = 0;

static uint32_t* get_pt_entry(uint32_t virt) {
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;
    uint32_t* pt = (uint32_t*)(0xFFC00000 + (pd_idx << 12));
    return &pt[pt_idx];
}

int map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t pd_idx = virt >> 22;
    uint32_t* pd = (uint32_t*)0xFFFFF000;

    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        uint32_t pt_phys = pmm_alloc_page();
        if (!pt_phys) return 0;   // 分配失败
        pd[pd_idx] = (pt_phys & 0xFFFFF000) | PAGE_PRESENT | PAGE_WRITE | (flags & PAGE_USER);
        uint32_t* new_pt = (uint32_t*)(0xFFC00000 + (pd_idx << 12));
        for (int i = 0; i < 1024; i++) new_pt[i] = 0;
    }

    uint32_t* pte = get_pt_entry(virt);
    *pte = (phys & 0xFFFFF000) | flags | PAGE_PRESENT | PAGE_WRITE;

    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    return 1;   // 成功
}

uint32_t vmm_alloc_page(void) {
    static uint32_t next_virt = 0x40000000;
    uint32_t phys = pmm_alloc_page();
    if (!phys) return 0;
    uint32_t virt = next_virt;
    map_page(virt, phys, PAGE_PRESENT | PAGE_WRITE);
    next_virt += PAGE_SIZE;
    return virt;
}

uint32_t dma_alloc_page(uint32_t* phys_out) {
    uint32_t phys = pmm_alloc_page();
    if (!phys) return 0;
    static uint32_t next_virt = 0x50000000;
    uint32_t virt = next_virt;
    if (!map_page(virt, phys, PAGE_PRESENT | PAGE_WRITE)) return 0;
    next_virt += PAGE_SIZE;
    if (phys_out) *phys_out = phys;
    return virt;
}

uint32_t mmio_map(uint32_t phys, uint32_t pages) {
    static uint32_t next_virt = 0xD0000000;
    uint32_t virt = next_virt;
    for (uint32_t i = 0; i < pages; i++) {
        if (!map_page(virt + i * PAGE_SIZE, phys + i * PAGE_SIZE,
                      PAGE_PRESENT | PAGE_WRITE | PAGE_PCD)) {
            return 0;
        }
    }
    next_virt += pages * PAGE_SIZE;
    return virt;
}

void paging_init(void) {
    pd_phys = pmm_alloc_page();
    if (!pd_phys) {
        printk("Failed to allocate page directory!\n");
        return;
    }
    uint32_t* pd = (uint32_t*)pd_phys;
    for (int i = 0; i < 1024; i++) pd[i] = 0;

    for (uint32_t addr = 0; addr < 0x100000; addr += PAGE_SIZE) {
        uint32_t pd_idx = addr >> 22;
        uint32_t pt_idx = (addr >> 12) & 0x3FF;
        if (!(pd[pd_idx] & PAGE_PRESENT)) {
            uint32_t pt_phys = pmm_alloc_page();
            if (!pt_phys) {
                printk("Failed to allocate page table for low memory!\n");
                return;
            }
            uint32_t* pt = (uint32_t*)pt_phys;
            for (int i = 0; i < 1024; i++) pt[i] = 0;
            pd[pd_idx] = (pt_phys & 0xFFFFF000) | PAGE_PRESENT | PAGE_WRITE;
        }
        uint32_t pt_phys = pd[pd_idx] & 0xFFFFF000;
        uint32_t* pt = (uint32_t*)pt_phys;
        pt[pt_idx] = (addr & 0xFFFFF000) | PAGE_PRESENT | PAGE_WRITE;
    }

    for (uint32_t addr = 0x100000; addr < 0x1000000; addr += PAGE_SIZE) {
        uint32_t pd_idx = addr >> 22;
        uint32_t pt_idx = (addr >> 12) & 0x3FF;
        if (!(pd[pd_idx] & PAGE_PRESENT)) {
            uint32_t pt_phys = pmm_alloc_page();
            if (!pt_phys) {
                printk("Failed to allocate page table for kernel identity map!\n");
                return;
            }
            uint32_t* pt = (uint32_t*)pt_phys;
            for (int i = 0; i < 1024; i++) pt[i] = 0;
            pd[pd_idx] = (pt_phys & 0xFFFFF000) | PAGE_PRESENT | PAGE_WRITE;
        }
        uint32_t pt_phys = pd[pd_idx] & 0xFFFFF000;
        uint32_t* pt = (uint32_t*)pt_phys;
        pt[pt_idx] = (addr & 0xFFFFF000) | PAGE_PRESENT | PAGE_WRITE;
    }

    for (uint32_t addr = 0xB8000; addr < 0xC0000; addr += PAGE_SIZE) {
        uint32_t pd_idx = addr >> 22;
        uint32_t pt_idx = (addr >> 12) & 0x3FF;
        if (!(pd[pd_idx] & PAGE_PRESENT)) {
            uint32_t pt_phys = pmm_alloc_page();
            if (!pt_phys) {
                printk("Failed to allocate page table for VGA identity map!\n");
                return;
            }
            uint32_t* pt = (uint32_t*)pt_phys;
            for (int i = 0; i < 1024; i++) pt[i] = 0;
            pd[pd_idx] = (pt_phys & 0xFFFFF000) | PAGE_PRESENT | PAGE_WRITE;
        }
        uint32_t pt_phys = pd[pd_idx] & 0xFFFFF000;
        uint32_t* pt = (uint32_t*)pt_phys;
        pt[pt_idx] = (addr & 0xFFFFF000) | PAGE_PRESENT | PAGE_WRITE;
    }

    pd[1023] = pd_phys | PAGE_PRESENT | PAGE_WRITE;

    __asm__ volatile("mov %0, %%cr3" : : "r"(pd_phys));

    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

    serial_write("Paging enabled successfully.\n");
}

uint32_t get_kernel_pd_phys(void) {
    return pd_phys;
}

int memory_is_initialized(void) {
    return total_pages > 0 && pd_phys != 0;
}

// ========== 对外接口 ==========
void memory_init(uint32_t mb_info_addr) {
    mb_info = (multiboot_info_t*)mb_info_addr;
    pmm_init();
    paging_init();
    vesa_init();   // 初始化 VESA 帧缓冲
}

void print_memory_info(void) {
    if (!mb_info) {
        printk("No multiboot info.\n");
        return;
    }
    if (!(mb_info->flags & (1 << 0))) {
        printk("Memory info not provided by bootloader.\n");
        return;
    }

    printk("Conventional memory: %d KB\n", mb_info->mem_lower);
    printk("Extended memory: %d KB\n", mb_info->mem_upper);

    if (mb_info->flags & (1 << 6)) {
        printk("Memory map:\n");
        uint32_t addr = mb_info->mmap_addr;
        uint32_t end = addr + mb_info->mmap_length;
        while (addr < end) {
            mmap_entry_t* entry = (mmap_entry_t*)addr;
            printk("  Base: 0x%08x%08x, Length: 0x%08x%08x, Type: %d\n",
                   (uint32_t)(entry->base_addr >> 32),
                   (uint32_t)entry->base_addr,
                   (uint32_t)(entry->length >> 32),
                   (uint32_t)entry->length,
                   entry->type);
            addr += entry->size + 4;
        }
    } else {
        printk("No memory map provided.\n");
    }
}

uint32_t memory_get_total_kb(void) {
    if (!mb_info) return 0;
    if (!(mb_info->flags & (1 << 0))) return 0;
    return mb_info->mem_upper + 1024;
}

// ----- VESA 图形模式实现 -----
static uint32_t fb_virt = 0;
static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint32_t fb_pitch = 0;

// 将帧缓冲区映射到虚拟地址 0xE0000000（高位，避开堆）
#define FB_VIRT_BASE 0xE0000000

void vesa_init(void) {
    if (!mb_info) return;
    if (!(mb_info->flags & (1 << 11))) {  // bit 11 = 视频模式信息可用
        printk("VESA: No video mode info provided by GRUB.\n");
        return;
    }

    // 获取 VBE 模式信息指针
    vbe_mode_info_t* vbe = (vbe_mode_info_t*)mb_info->vbe_mode_info;
    if (!vbe) return;

    fb_width = vbe->width;
    fb_height = vbe->height;
    fb_pitch = vbe->pitch;
    uint32_t fb_phys = vbe->phys_base;

    if (fb_phys == 0 || fb_width == 0 || fb_height == 0) {
        printk("VESA: Invalid mode info (phys=0x%x, %dx%d)\n", fb_phys, fb_width, fb_height);
        return;
    }

    printk("VESA: %dx%d, %d bpp, pitch=%d, phys=0x%x\n",
           fb_width, fb_height, vbe->bpp, fb_pitch, fb_phys);

    // 将帧缓冲区物理地址映射到虚拟地址
    uint32_t virt = FB_VIRT_BASE;
    uint32_t size = fb_height * fb_pitch;
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint32_t i = 0; i < pages; i++) {
        uint32_t phys = fb_phys + i * PAGE_SIZE;
        if (!map_page(virt + i * PAGE_SIZE, phys, PAGE_PRESENT | PAGE_WRITE)) {
            printk("VESA: Failed to map page at 0x%x\n", phys);
            return;
        }
    }

    fb_virt = virt;
    printk("VESA: Framebuffer mapped to virtual address 0x%x\n", fb_virt);
}

uint32_t vesa_get_fb_addr(void) { return fb_virt; }
uint32_t vesa_get_width(void)   { return fb_width; }
uint32_t vesa_get_height(void)  { return fb_height; }
uint32_t vesa_get_pitch(void)   { return fb_pitch; }

// ========== Bochs VBE 高分辨率图形 ==========

// 向 VBE 索引端口写 16 位值
static void vbe_write_reg(uint16_t index, uint16_t value) {
    __asm__ volatile(
        "outw %0, %w1"
        : : "a"(index), "d"((uint16_t)VBE_DISPI_IOPORT_INDEX)
    );
    __asm__ volatile(
        "outw %0, %w1"
        : : "a"(value), "d"((uint16_t)VBE_DISPI_IOPORT_DATA)
    );
}

// 从 VBE 索引端口读 16 位值
static uint16_t vbe_read_reg(uint16_t index) {
    uint16_t value;
    __asm__ volatile(
        "outw %0, %w1"
        : : "a"(index), "d"((uint16_t)VBE_DISPI_IOPORT_INDEX)
    );
    __asm__ volatile(
        "inw %w1, %0"
        : "=a"(value) : "d"((uint16_t)VBE_DISPI_IOPORT_DATA)
    );
    return value;
}

static void serial_hex(uint32_t num) {
    const char* hex = "0123456789ABCDEF";
    char buf[9];
    for (int i = 7; i >= 0; i--) {
        buf[i] = hex[num & 0xF];
        num >>= 4;
    }
    buf[8] = '\0';
    serial_write(buf);
}

static uint32_t pci_read_cfg(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg) {
    uint32_t addr = (uint32_t)0x80000000 | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (reg & 0xFC);
    __asm__ volatile("outl %0, %w1" : : "a"(addr), "d"((uint16_t)0xCF8));
    uint32_t val;
    __asm__ volatile("inl %w1, %0" : "=a"(val) : "d"((uint16_t)0xCFC));
    return val;
}

static void pci_find_vga(uint32_t* bar0) {
    *bar0 = 0;
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t vendor = pci_read_cfg((uint8_t)bus, dev, 0, 0);
            if ((vendor & 0xFFFF) == 0xFFFF) continue;
            uint32_t classrev = pci_read_cfg((uint8_t)bus, dev, 0, 8);
            uint32_t classcode = (classrev >> 16) & 0xFFFF;
            if (classcode == 0x0300) {
                uint32_t bar = pci_read_cfg((uint8_t)bus, dev, 0, 0x10);
                serial_write("VBE: VGA found bus=");
                serial_hex(bus);
                serial_write(" dev=");
                serial_hex(dev);
                serial_write(" class=");
                serial_hex(classcode);
                serial_write(" BAR0=");
                serial_hex(bar);
                serial_write("\n");
                if ((bar & 0xFFFFFFF0) != 0) {
                    *bar0 = bar & 0xFFFFFFF0;
                    return;
                }
            }
        }
    }
}

// 初始化 VBE 图形模式
int vbe_init(uint32_t width, uint32_t height, uint32_t bpp) {
    if (vbe_fb_virt) {
        // 已经初始化过，直接复用当前帧缓冲
        return 1;
    }

    // 检查 VBE ID
    uint16_t id = vbe_read_reg(VBE_DISPI_INDEX_ID);
    if (id < 0xB0C0 || id > 0xB0C5) {
        printk("VBE: Not supported (ID=0x%x)\n", id);
        return 0;
    }

    // 设置模式：先禁用，再写入参数，最后启用 LFB
    vbe_write_reg(VBE_DISPI_INDEX_ENABLE, 0);
    vbe_write_reg(VBE_DISPI_INDEX_XRES, width);
    vbe_write_reg(VBE_DISPI_INDEX_YRES, height);
    vbe_write_reg(VBE_DISPI_INDEX_BPP, bpp);
    vbe_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    // 读取实际参数
    vbe_width = vbe_read_reg(VBE_DISPI_INDEX_XRES);
    vbe_height = vbe_read_reg(VBE_DISPI_INDEX_YRES);
    vbe_bpp = vbe_read_reg(VBE_DISPI_INDEX_BPP);
    vbe_pitch = (vbe_width * vbe_bpp + 7) / 8;

    if (vbe_width == 0 || vbe_height == 0) {
        printk("VBE: Failed to set mode\n");
        return 0;
    }

    // 通过 PCI 获取 VGA 帧缓冲区物理地址
    uint32_t phys_base = 0;
    pci_find_vga(&phys_base);
    if (phys_base == 0) {
        phys_base = VBE_DISPI_LFB_PHYSICAL;
    }

    // 映射到高位虚拟地址，避开内核和堆
    uint32_t virt_base = 0xE0000000;
    uint32_t fb_size = vbe_height * vbe_pitch;
    uint32_t pages = (fb_size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint32_t i = 0; i < pages; i++) {
        if (!map_page(virt_base + i * PAGE_SIZE, phys_base + i * PAGE_SIZE,
                      PAGE_PRESENT | PAGE_WRITE | PAGE_PWT | PAGE_PCD)) {
            printk("VBE: Failed to map framebuffer at 0x%x\n", phys_base + i * PAGE_SIZE);
            return 0;
        }
    }

    vbe_fb_virt = virt_base;
    printk("VBE: %dx%dx%d, pitch=%d, fb_virt=0x%x, phys=0x%x\n",
           vbe_width, vbe_height, vbe_bpp, vbe_pitch, vbe_fb_virt, phys_base);

    return 1;
}

uint32_t vbe_get_fb(void)      { return vbe_fb_virt; }
uint32_t vbe_get_width(void)   { return vbe_width; }
uint32_t vbe_get_height(void)  { return vbe_height; }
uint32_t vbe_get_pitch(void)   { return vbe_pitch; }

void vbe_disable(void) {
    vbe_write_reg(VBE_DISPI_INDEX_ENABLE, 0);
    vbe_fb_virt = 0;
}

// 画像素（32 位色）
void vbe_putpixel(int x, int y, uint32_t color) {
    if (!vbe_fb_virt || x < 0 || (uint32_t)x >= vbe_width || y < 0 || (uint32_t)y >= vbe_height) return;
    uint32_t* fb = (uint32_t*)vbe_fb_virt;
    fb[y * (vbe_pitch / 4) + x] = color;
}

// 填充矩形
void vbe_fillrect(int x, int y, int w, int h, uint32_t color) {
    if (!vbe_fb_virt) return;
    uint32_t* fb = (uint32_t*)vbe_fb_virt;
    int pitch_words = (int)(vbe_pitch / 4);
    for (int row = y; row < y + h && (uint32_t)row < vbe_height; row++) {
        for (int col = x; col < x + w && (uint32_t)col < vbe_width; col++) {
            if (col >= 0 && row >= 0)
                fb[row * pitch_words + col] = color;
        }
    }
}
