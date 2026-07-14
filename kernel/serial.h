#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

extern int serial_available;

void serial_init(void);
void serial_putchar(char ch);
void serial_write(const char* str);
void serial_puts(const char* str);
void serial_printf(const char* fmt, ...);
void serial_write_hex(const uint8_t* data, int len);

#endif
