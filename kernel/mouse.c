#include "mouse.h"
#include "io.h"
#include "memory.h"
#include "serial.h"
#include <stdint.h>

#define MOUSE_DATA_PORT   0x60
#define MOUSE_CMD_PORT    0x64

volatile int mouse_x = 0;
volatile int mouse_y = 0;
volatile uint8_t mouse_buttons = 0;

static volatile uint8_t mouse_buffer[3];
static volatile int mouse_buffer_index = 0;
static volatile int mouse_initialized = 0;

static void mouse_wait(uint8_t type) {
    uint32_t timeout = 100000;
    if (type == 0) {
        while (!(inb(MOUSE_CMD_PORT) & 0x01)) {
            if (--timeout == 0) break;
        }
    } else {
        while (inb(MOUSE_CMD_PORT) & 0x02) {
            if (--timeout == 0) break;
        }
    }
}

void mouse_init(void) {
    mouse_initialized = 0;
    mouse_buffer_index = 0;
    mouse_buttons = 0;

    uint32_t flags;
    __asm__ volatile("pushf\ncli\npop %0" : "=r"(flags));

    while (inb(MOUSE_CMD_PORT) & 0x01) {
        inb(MOUSE_DATA_PORT);
    }

    mouse_wait(1);
    outb(MOUSE_CMD_PORT, 0xA8);

    mouse_wait(1);
    outb(MOUSE_CMD_PORT, 0x20);
    mouse_wait(0);
    uint8_t status = inb(MOUSE_DATA_PORT);
    status |= 0x02;
    status &= ~0x20;
    mouse_wait(1);
    outb(MOUSE_CMD_PORT, 0x60);
    mouse_wait(1);
    outb(MOUSE_DATA_PORT, status);

    mouse_wait(1);
    outb(MOUSE_CMD_PORT, 0xD4);
    mouse_wait(1);
    outb(MOUSE_DATA_PORT, 0xF4);

    if (flags & 0x200) {
        __asm__ volatile("sti");
    }

    uint32_t w = vbe_get_width();
    uint32_t h = vbe_get_height();
    if (w == 0) w = 1024;
    if (h == 0) h = 768;
    mouse_x = (int)w / 2;
    mouse_y = (int)h / 2;

    mouse_initialized = 1;
}

void __attribute__((interrupt)) mouse_isr(void *frame) {
    (void)frame;
    uint8_t data = inb(MOUSE_DATA_PORT);

    if (!mouse_initialized) {
        outb(0xA0, 0x20);
        outb(0x20, 0x20);
        return;
    }

    if (mouse_buffer_index == 0 && (data & 0x08) == 0) {
        outb(0xA0, 0x20);
        outb(0x20, 0x20);
        return;
    }

    mouse_buffer[mouse_buffer_index++] = data;

    if (mouse_buffer_index >= 3) {
        mouse_buffer_index = 0;
        uint8_t flags = mouse_buffer[0];
        if (flags & 0xC0) {
            outb(0xA0, 0x20);
            outb(0x20, 0x20);
            return;
        }

        int dx = mouse_buffer[1];
        if (flags & 0x10) dx |= 0xFFFFFF00;
        int dy = mouse_buffer[2];
        if (flags & 0x20) dy |= 0xFFFFFF00;

        mouse_x += dx;
        mouse_y -= dy;

        uint32_t w = vbe_get_width();
        uint32_t h = vbe_get_height();
        if (w == 0) w = 1024;
        if (h == 0) h = 768;

        if (mouse_x < 0) mouse_x = 0;
        if (mouse_x >= (int)w) mouse_x = (int)w - 1;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_y >= (int)h) mouse_y = (int)h - 1;

        mouse_buttons = flags & 0x07;
    }

    outb(0xA0, 0x20);
    outb(0x20, 0x20);
}
