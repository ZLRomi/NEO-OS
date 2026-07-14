#include "terminal.h"
#include "io.h"
#include "gui.h"

static uint16_t* vga = VGA_ADDR;
static int row = 0;
static int col = 0;

terminal_sink_t terminal_redirect_sink = 0;

void terminal_set_redirect(terminal_sink_t sink) {
    terminal_redirect_sink = sink;
}

static void update_cursor(void) {
    uint16_t pos = row * COLS + col;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void terminal_scroll(void) {
    for (int r = 0; r < ROWS - 1; r++) {
        for (int c = 0; c < COLS; c++) {
            vga[r * COLS + c] = vga[(r + 1) * COLS + c];
        }
    }
    for (int c = 0; c < COLS; c++) {
        vga[(ROWS - 1) * COLS + c] = (uint16_t)((COLOR << 8) | ' ');
    }
}

void terminal_putchar(char ch) {
    if (terminal_redirect_sink) {
        char buf[1] = { ch };
        terminal_redirect_sink(buf, 1);
        return;
    }
    if (gui_active) {
        gshell_putchar(ch);
        return;
    }
    if (ch == '\n') {
        col = 0;
        row++;
        if (row >= ROWS) { terminal_scroll(); row = ROWS - 1; }
        update_cursor();
        return;
    }
    if (ch == '\b') {
        if (col > 0) {
            col--;
            vga[row * COLS + col] = (uint16_t)((COLOR << 8) | ' ');
            update_cursor();
        }
        return;
    }
    vga[row * COLS + col] = (uint16_t)((COLOR << 8) | (uint8_t)ch);
    col++;
    if (col >= COLS) {
        col = 0;
        row++;
        if (row >= ROWS) { terminal_scroll(); row = ROWS - 1; }
    }
    update_cursor();
}

void terminal_write(const char* str) {
    if (terminal_redirect_sink) {
        int len = 0;
        while (str[len]) len++;
        terminal_redirect_sink(str, len);
        return;
    }
    for (int i = 0; str[i] != '\0'; i++) terminal_putchar(str[i]);
}

void terminal_clear(void) {
    if (gui_active) {
        gshell_clear();
        return;
    }
    for (int i = 0; i < COLS * ROWS; i++) {
        vga[i] = (uint16_t)((COLOR << 8) | ' ');
    }
    row = 0;
    col = 0;
    update_cursor();
}
