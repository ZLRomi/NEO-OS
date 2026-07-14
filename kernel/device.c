#include "device.h"
#include "disk.h"
#include "memory.h"
#include "mouse.h"
#include "terminal.h"
#include "serial.h"
#include "printk.h"
#include "tty.h"
#include "gfx_driver.h"
#include <stdint.h>

static device_t device_table[MAX_DEVICES];

static int name_eq(const char* a, const char* b) {
    int i;
    for (i = 0; i < DEV_NAME_LEN; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 0;
}

static void name_cpy(char* dst, const char* src) {
    int i;
    for (i = 0; i < DEV_NAME_LEN - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

int device_register(const char* name, int type,
                    int (*open)(device_t*),
                    int (*close)(device_t*),
                    int (*read)(device_t*, void*, int, uint32_t),
                    int (*write)(device_t*, const void*, int, uint32_t),
                    int (*ioctl)(device_t*, int, void*),
                    void* priv) {
    int i;
    for (i = 0; i < MAX_DEVICES; i++) {
        if (!device_table[i].in_use) {
            name_cpy(device_table[i].name, name);
            device_table[i].type = type;
            device_table[i].open = open;
            device_table[i].close = close;
            device_table[i].read = read;
            device_table[i].write = write;
            device_table[i].ioctl = ioctl;
            device_table[i].priv = priv;
            device_table[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

device_t* device_find(const char* name) {
    int i;
    for (i = 0; i < MAX_DEVICES; i++) {
        if (device_table[i].in_use && name_eq(device_table[i].name, name))
            return &device_table[i];
    }
    return 0;
}

int dev_open(const char* name) {
    device_t* d = device_find(name);
    if (!d) return -1;
    if (d->open) return d->open(d);
    return 0;
}

int dev_close(const char* name) {
    device_t* d = device_find(name);
    if (!d) return -1;
    if (d->close) return d->close(d);
    return 0;
}

int dev_read(const char* name, void* buf, int count, uint32_t offset) {
    device_t* d = device_find(name);
    if (!d || !d->read) return -1;
    return d->read(d, buf, count, offset);
}

int dev_write(const char* name, const void* buf, int count, uint32_t offset) {
    device_t* d = device_find(name);
    if (!d || !d->write) return -1;
    return d->write(d, buf, count, offset);
}

int dev_ioctl(const char* name, int cmd, void* arg) {
    device_t* d = device_find(name);
    if (!d || !d->ioctl) return -1;
    return d->ioctl(d, cmd, arg);
}

void device_list(void) {
    int i;
    printk("device: table dump (%d slots)\n", MAX_DEVICES);
    for (i = 0; i < MAX_DEVICES; i++) {
        if (device_table[i].in_use) {
            printk("  [%d] %s type=%s\n", i,
                   device_table[i].name,
                   device_table[i].type == DEVICE_TYPE_BLOCK ? "BLOCK" : "CHAR");
        }
    }
}

/* ---------- tty0 ---------- */
static int tty_open(device_t* d) { (void)d; return 0; }
static int tty_close(device_t* d) { (void)d; return 0; }
static int dev_tty_read(device_t* d, void* buf, int count, uint32_t off) {
    (void)d; (void)off;
    return tty_read(0, (char*)buf, count);
}
static int dev_tty_write(device_t* d, const void* buf, int count, uint32_t off) {
    (void)d; (void)off;
    return tty_write(0, (const char*)buf, count);
}
static int tty_ioctl(device_t* d, int cmd, void* arg) { (void)d; (void)cmd; (void)arg; return -1; }

/* ---------- disk0 ---------- */
static int disk_open(device_t* d) { (void)d; return 0; }
static int disk_close(device_t* d) { (void)d; return 0; }
static int disk_read_dev(device_t* d, void* buf, int count, uint32_t offset) {
    (void)d;
    if (count % 512 != 0) return -1;
    int sectors = count / 512;
    uint8_t* p = (uint8_t*)buf;
    int i;
    for (i = 0; i < sectors; i++) {
        if (disk_read_sector(offset + i, p) < 0) return -1;
        p += 512;
    }
    return count;
}
static int disk_write_dev(device_t* d, const void* buf, int count, uint32_t offset) {
    (void)d;
    if (count % 512 != 0) return -1;
    int sectors = count / 512;
    const uint8_t* p = (const uint8_t*)buf;
    int i;
    for (i = 0; i < sectors; i++) {
        if (disk_write_sector(offset + i, p) < 0) return -1;
        p += 512;
    }
    return count;
}
static int disk_ioctl(device_t* d, int cmd, void* arg) {
    (void)d; (void)cmd; (void)arg;
    return -1;
}

/* ---------- fb0 ---------- */
static int fb_open(device_t* d) { (void)d; return 0; }
static int fb_close(device_t* d) { (void)d; return 0; }
static int fb_read(device_t* d, void* buf, int count, uint32_t off) {
    (void)d; (void)buf; (void)count; (void)off;
    return -1;
}
static int fb_write(device_t* d, const void* buf, int count, uint32_t offset) {
    (void)d;
    uint32_t fb = gfx_get_fb();
    if (!fb) return -1;
    uint32_t pitch = (uint32_t)gfx_get_pitch();
    uint32_t height = (uint32_t)gfx_get_height();
    uint32_t fbsize = pitch * height;
    if (offset >= fbsize) return -1;
    uint32_t avail = fbsize - offset;
    uint32_t n = (uint32_t)count;
    if (n > avail) n = avail;
    uint8_t* dst = (uint8_t*)(uintptr_t)fb + offset;
    const uint8_t* src = (const uint8_t*)buf;
    uint32_t i;
    for (i = 0; i < n; i++) dst[i] = src[i];
    return (int)n;
}
static int fb_ioctl(device_t* d, int cmd, void* arg) { (void)d; (void)cmd; (void)arg; return -1; }

/* ---------- mouse0 ---------- */
typedef struct {
    int32_t x;
    int32_t y;
    uint8_t buttons;
} mouse_state_t;

static int mouse_open(device_t* d) { (void)d; return 0; }
static int mouse_close(device_t* d) { (void)d; return 0; }
static int mouse_read_dev(device_t* d, void* buf, int count, uint32_t off) {
    (void)d; (void)off;
    if (count < (int)sizeof(mouse_state_t)) return -1;
    mouse_state_t* s = (mouse_state_t*)buf;
    s->x = mouse_x;
    s->y = mouse_y;
    s->buttons = mouse_buttons;
    return (int)sizeof(mouse_state_t);
}
static int mouse_write_dev(device_t* d, const void* buf, int count, uint32_t off) {
    (void)d; (void)buf; (void)count; (void)off;
    return -1;
}
static int mouse_ioctl(device_t* d, int cmd, void* arg) { (void)d; (void)cmd; (void)arg; return -1; }

/* ---------- null ---------- */
static int null_open(device_t* d) { (void)d; return 0; }
static int null_close(device_t* d) { (void)d; return 0; }
static int null_read(device_t* d, void* buf, int count, uint32_t off) {
    (void)d; (void)buf; (void)count; (void)off;
    return 0;
}
static int null_write(device_t* d, const void* buf, int count, uint32_t off) {
    (void)d; (void)buf; (void)count; (void)off;
    return count;
}
static int null_ioctl(device_t* d, int cmd, void* arg) { (void)d; (void)cmd; (void)arg; return -1; }

/* ---------- zero ---------- */
static int zero_open(device_t* d) { (void)d; return 0; }
static int zero_close(device_t* d) { (void)d; return 0; }
static int zero_read(device_t* d, void* buf, int count, uint32_t off) {
    (void)d; (void)off;
    uint8_t* p = (uint8_t*)buf;
    int i;
    for (i = 0; i < count; i++) p[i] = 0;
    return count;
}
static int zero_write(device_t* d, const void* buf, int count, uint32_t off) {
    (void)d; (void)buf; (void)count; (void)off;
    return count;
}
static int zero_ioctl(device_t* d, int cmd, void* arg) { (void)d; (void)cmd; (void)arg; return -1; }

void device_init(void) {
    int i;
    for (i = 0; i < MAX_DEVICES; i++) {
        device_table[i].in_use = 0;
        device_table[i].name[0] = '\0';
    }

    device_register("tty0", DEVICE_TYPE_CHAR,
                    tty_open, tty_close, dev_tty_read, dev_tty_write, tty_ioctl, 0);
    device_register("disk0", DEVICE_TYPE_BLOCK,
                    disk_open, disk_close, disk_read_dev, disk_write_dev, disk_ioctl, 0);
    device_register("fb0", DEVICE_TYPE_CHAR,
                    fb_open, fb_close, fb_read, fb_write, fb_ioctl, 0);
    device_register("mouse0", DEVICE_TYPE_CHAR,
                    mouse_open, mouse_close, mouse_read_dev, mouse_write_dev, mouse_ioctl, 0);
    device_register("null", DEVICE_TYPE_CHAR,
                    null_open, null_close, null_read, null_write, null_ioctl, 0);
    device_register("zero", DEVICE_TYPE_CHAR,
                    zero_open, zero_close, zero_read, zero_write, zero_ioctl, 0);

    printk("device: framework initialized, %d built-in devices registered\n", 6);
    device_list();
}
