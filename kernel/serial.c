#include "serial.h"
#include "io.h"
#include <stdarg.h>
#include <stdint.h>

#define COM1 0x3F8

int serial_available = 0;

void serial_init(void) {
    serial_available = 0;

    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);

    outb(COM1 + 7, 0xAE);
    uint8_t scratch = inb(COM1 + 7);
    if (scratch != 0xAE) {
        return;
    }

    serial_available = 1;
}

void serial_putchar(char ch) {
    if (!serial_available) return;
    while ((inb(COM1 + 5) & 0x20) == 0);
    outb(COM1, ch);
}

void serial_write(const char* str) {
    if (!serial_available || !str) return;
    for (int i = 0; str[i]; i++) serial_putchar(str[i]);
}

void serial_puts(const char* str) {
    serial_write(str);
}

void serial_write_hex(const uint8_t* data, int len) {
    static const char hex[] = "0123456789ABCDEF";
    if (!serial_available || !data || len <= 0) return;
    for (int i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (b >= 0x20 && b < 0x7F) {
            serial_putchar((char)b);
        } else {
            serial_putchar('<');
            serial_putchar(hex[b >> 4]);
            serial_putchar(hex[b & 0xF]);
            serial_putchar('>');
        }
    }
}

static void serial_print_hex(uint32_t num, int width) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[9];
    int i;
    for (i = 0; i < 8; i++) {
        buf[7 - i] = hex[num & 0xF];
        num >>= 4;
    }
    buf[8] = '\0';
    int start = 0;
    if (width > 0 && width < 8) {
        start = 8 - width;
    }
    serial_write(buf + start);
}

static void serial_print_dec(int num) {
    if (num == 0) {
        serial_putchar('0');
        return;
    }
    if (num < 0) {
        serial_putchar('-');
        num = -num;
    }
    char buf[12];
    int i = 0;
    while (num > 0) {
        buf[i++] = '0' + (num % 10);
        num /= 10;
    }
    while (i > 0) {
        serial_putchar(buf[--i]);
    }
}

void serial_printf(const char* fmt, ...) {
    if (!serial_available || !fmt) return;
    va_list args;
    va_start(args, fmt);

    for (int i = 0; fmt[i] != '\0'; i++) {
        if (fmt[i] == '%') {
            i++;
            int width = 0;
            while (fmt[i] >= '0' && fmt[i] <= '9') {
                width = width * 10 + (fmt[i] - '0');
                i++;
            }
            switch (fmt[i]) {
                case 'd': {
                    int val = va_arg(args, int);
                    serial_print_dec(val);
                    break;
                }
                case 'x': {
                    uint32_t val = va_arg(args, uint32_t);
                    if (width == 0) width = 8;
                    serial_print_hex(val, width);
                    break;
                }
                case 's': {
                    const char* str = va_arg(args, const char*);
                    serial_write(str ? str : "(null)");
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    serial_putchar(c);
                    break;
                }
                case '%': {
                    serial_putchar('%');
                    break;
                }
                default: {
                    serial_putchar('%');
                    serial_putchar(fmt[i]);
                    break;
                }
            }
        } else {
            serial_putchar(fmt[i]);
        }
    }
    va_end(args);
}
