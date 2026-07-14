#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdint.h>

#define VGA_ADDR  ((uint16_t*)0xB8000)
#define COLS      80
#define ROWS      25
#define COLOR     0x0A

void terminal_scroll(void);
void terminal_putchar(char ch);
void terminal_write(const char* str);
void terminal_clear(void);

typedef void (*terminal_sink_t)(const char* s, int len);
extern terminal_sink_t terminal_redirect_sink;
void terminal_set_redirect(terminal_sink_t sink);

#endif
