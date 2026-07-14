#include "terminal.h"
#include "printk.h"
#include "serial.h"
#include "panic.h"
#include "idt.h"
#include "memory.h"
#include "heap.h"
#include "gui.h"
#include "wallpaper.h"
#include "pci.h"
#include "e1000.h"
#include "net.h"
#include "http.h"
#include "tls.h"
#include "process.h"
#include "sched.h"
#include "disk.h"
#include "disk_sched.h"
#include "fat32.h"
#include "vfs.h"
#include "procfs.h"
#include "buffer.h"
#include "device.h"
#include "auth.h"
#include "userdb.h"
#include "klog.h"
#include "irq.h"
#include "gfx_driver.h"
#include "gfx_3d_pipeline.h"
#include "audio.h"
#include "systray.h"
#include "notify.h"
#include "app_store.h"
#include "power.h"
#include "hal.h"
#include "syscall.h"
#include "rtc.h"
#include "timer.h"
#include "spool.h"
#include "tty.h"
#include "event.h"

extern uint32_t mb_info_ptr;

// 演示进程：无限循环计数，验证抢占式调度工作
static volatile int demo_counter = 0;
static void demo_process(void) {
    while (1) {
        demo_counter++;
        if ((demo_counter & 0xFFFFFF) == 0) {
            serial_write("demo: process running\n");
        }
    }
}

static void boot_selftest(void) {
    if (!memory_is_initialized()) {
        panic("boot selftest: memory manager not initialized");
    }
    if (!heap_is_initialized()) {
        panic("boot selftest: heap not initialized");
    }

    if (sizeof(process_t) == 0 || (sizeof(process_t) & 3) != 0) {
        panic("boot selftest: process_t size or alignment invalid");
    }

    if (sizeof(vfs_fs_t) == 0) {
        panic("boot selftest: vfs_fs_t size invalid");
    }

    klog_write("boot selftest: passed\n");
}

void kernel_main(void) {
    terminal_clear();
    serial_init();
    init_interrupts();
    klog_init();
    irq_init();
    timer_init();
    rtc_init();

    memory_init(mb_info_ptr);
    kernel_assert(memory_is_initialized());

    terminal_write("NEO-OS Boot Success!\n");
    printk("Kernel loaded at physical address 0x100000\n");
    printk("Multiboot info at 0x%x\n", mb_info_ptr);
    print_memory_info();
    heap_init();
    kernel_assert(heap_is_initialized());
    boot_selftest();

    // 进程与调度器初始化（机制就绪，调度默认禁用，保持单任务）
    process_init();
    sched_init();
    signal_init();

    // 创建演示进程并启用抢占式调度（在网络测试前启用，让多任务与网络并行）
    process_create((uint32_t)(uintptr_t)demo_process, 1, "demo");
    sched_enable();
    printk("sched: enabled, demo process created\n");

    disk_init();
    disk_sched_init();
    hal_init();
    if (fat32_init() < 0) {
        // 无盘或文件系统无效时仅记录警告；系统可在没有磁盘的环境中继续启动
        klog_write_level(KLOG_WARN, "fs: FAT32 mount failed (no disk or invalid FS)\n");
    } else {
        serial_write("fs: FAT32 mounted\n");
    }

    vfs_init();
    procfs_init();

    wallpaper_init();

    auth_init();
    userdb_init();

    syscall_init();

    buffer_init();
    tty_init();
    event_init();
    device_init();
    spool_init();
    audio_init();
    systray_init();
    notify_init();
    app_store_init();
    power_init();

    if (e1000_init()) {
        net_init();
        ntp_sync_time(300);
        if (ping(net_parse_ip("10.0.2.2"), 200)) {
            serial_write("Ping gateway OK\n");
        } else {
            serial_write("Ping gateway FAIL\n");
        }
        static char page[2048];
        if (http_get("example.com", "/", page, sizeof(page), 1) > 0) {
            serial_write("HTTP test OK:\n");
            serial_write(page);
            serial_write("\n");
        } else {
            serial_write("HTTP test FAIL\n");
        }
        static char https_page[4096];
        if (http_fetch("https://neocn.top/", https_page, sizeof(https_page), NULL, 0) > 0) {
            serial_write("HTTPS test OK:\n");
            serial_write(https_page);
            serial_write("\n");
        } else {
            serial_write("HTTPS test FAIL\n");
        }
    }

    while (1) {
        gfx_init();
        if (!gfx_set_mode(1920, 1080, 32)) {
            serial_write("gfx: 1920x1080 failed, trying 1024x768\n");
            if (!gfx_set_mode(1024, 768, 32)) {
                panic("gfx: failed to set any graphics mode");
            }
        }
        gfx_3d_pipeline_init();
        gui_run();
        __asm__("hlt");
    }
}
