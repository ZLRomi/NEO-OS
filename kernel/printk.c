#include "printk.h"
#include <stdarg.h>
#include <stdint.h>
#include "terminal.h"
#include "serial.h"

static void print_hex(uint32_t num, int width) {
    char hex[] = "0123456789ABCDEF";
    char buf[9] = {0};
    for (int i = 7; i >= 0; i--) {
        buf[i] = hex[num & 0xF];
        num >>= 4;
    }
    int start = (width > 0 && width < 8) ? 8 - width : 0;
    terminal_write(buf + start);
    serial_write(buf + start);
}

static void print_dec(int num) {
    if (num == 0) {
        terminal_write("0"); serial_write("0");
        return;
    }
    if (num < 0) {
        terminal_write("-"); serial_write("-");
        num = -num;
    }
    char buf[12];
    int i = 0;
    while (num > 0) {
        buf[i++] = '0' + (num % 10);
        num /= 10;
    }
    buf[i] = '\0';
    for (int j = i - 1; j >= 0; j--) {
        terminal_putchar(buf[j]);
        serial_putchar(buf[j]);
    }
}

void printk(const char* fmt, ...) {
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
                    print_dec(val);
                    break;
                }
                case 'x': {
                    uint32_t val = va_arg(args, uint32_t);
                    print_hex(val, width ? width : 8);
                    break;
                }
                case 's': {
                    const char* str = va_arg(args, const char*);
                    terminal_write(str);
                    serial_write(str);
                    break;
                }
                default: {
                    terminal_putchar('%');
                    serial_putchar('%');
                    terminal_putchar(fmt[i]);
                    serial_putchar(fmt[i]);
                }
            }
        } else {
            terminal_putchar(fmt[i]);
            serial_putchar(fmt[i]);
        }
    }
    va_end(args);
}
