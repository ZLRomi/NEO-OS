#include "idt.h"
#include "io.h"
#include "mouse.h"
#include "gui.h"
#include "sched.h"
#include "fault.h"
#include "syscall.h"
#include "timer.h"
#include "tty.h"

static idt_entry_t idt[IDT_SIZE];
static idt_ptr_t idt_ptr;

volatile uint32_t timer_ticks = 0;

static void pic_init(void) {
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
    outb(PIC1_DATA, 0x20);
    outb(PIC2_DATA, 0x28);
    outb(PIC1_DATA, 0x04);
    outb(PIC2_DATA, 0x02);
    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);
    outb(PIC1_DATA, 0xF8);
    outb(PIC2_DATA, 0xEF);
}

static void pit_init(void) {
    uint32_t freq = 100;
    uint32_t divisor = 1193182 / freq;
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}

static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].selector = sel;
    idt[num].zero = 0;
    idt[num].flags = flags;
}

static void idt_load(void) {
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint32_t)idt;
    __asm__ volatile("lidt (%0)" : : "r"(&idt_ptr));
    __asm__ volatile("sti");
}

void __attribute__((interrupt)) keyboard_isr(void *frame) {
    (void)frame;
    uint8_t sc = inb(0x60);

    static int shift = 0;
    static int ctrl = 0;
    static int alt = 0;

    if (sc & 0x80) {
        if (sc == 0xAA || sc == 0xB6) shift = 0;
        if (sc == 0x9D) ctrl = 0;
        if (sc == 0xB8) alt = 0;
        goto done;
    }

    if (sc == 0x2A || sc == 0x36) {
        shift = 1;
        goto done;
    }
    if (sc == 0x1D) {
        ctrl = 1;
        goto done;
    }
    if (sc == 0x38) {
        alt = 1;
        goto done;
    }

    char ascii = 0;
    switch (sc) {
        case 0x10: ascii = 'q'; break; case 0x11: ascii = 'w'; break;
        case 0x12: ascii = 'e'; break; case 0x13: ascii = 'r'; break;
        case 0x14: ascii = 't'; break; case 0x15: ascii = 'y'; break;
        case 0x16: ascii = 'u'; break; case 0x17: ascii = 'i'; break;
        case 0x18: ascii = 'o'; break; case 0x19: ascii = 'p'; break;
        case 0x1E: ascii = 'a'; break; case 0x1F: ascii = 's'; break;
        case 0x20: ascii = 'd'; break; case 0x21: ascii = 'f'; break;
        case 0x22: ascii = 'g'; break; case 0x23: ascii = 'h'; break;
        case 0x24: ascii = 'j'; break; case 0x25: ascii = 'k'; break;
        case 0x26: ascii = 'l'; break;
        case 0x2C: ascii = 'z'; break; case 0x2D: ascii = 'x'; break;
        case 0x2E: ascii = 'c'; break; case 0x2F: ascii = 'v'; break;
        case 0x30: ascii = 'b'; break; case 0x31: ascii = 'n'; break;
        case 0x32: ascii = 'm'; break;

        case 0x02: ascii = '1'; break; case 0x03: ascii = '2'; break;
        case 0x04: ascii = '3'; break; case 0x05: ascii = '4'; break;
        case 0x06: ascii = '5'; break; case 0x07: ascii = '6'; break;
        case 0x08: ascii = '7'; break; case 0x09: ascii = '8'; break;
        case 0x0A: ascii = '9'; break; case 0x0B: ascii = '0'; break;

        case 0x0C: ascii = '-'; break; case 0x0D: ascii = '='; break;
        case 0x1A: ascii = '['; break; case 0x1B: ascii = ']'; break;
        case 0x27: ascii = ';'; break; case 0x28: ascii = '\''; break;
        case 0x29: ascii = '`'; break; case 0x2B: ascii = '\\'; break;
        case 0x33: ascii = ','; break; case 0x34: ascii = '.'; break;
        case 0x35: ascii = '/'; break;
        case 0x39: ascii = ' '; break;
    }

    if (ascii && shift && ascii >= 'a' && ascii <= 'z') {
        ascii -= 32;
    }

    if (ctrl && sc == 0x2E) {
        tty_input(0, 0x03);
        goto done;
    }

    if (alt && sc >= 0x3B && sc <= 0x41) {
        tty_switch(sc - 0x3B);
        goto done;
    }

    if (gui_active) {
        gui_key_scancode = sc;
        gui_key_char = ascii;
        gui_key_ready = 1;
    } else {
        if (sc == 0x0E) {
            gshell_input_backspace();
            tty_input(0, '\b');
        } else if (sc == 0x1C) {
            gshell_input_submit();
            tty_input(0, '\n');
        } else if (ascii) {
            gshell_input_char(ascii);
            tty_input(0, ascii);
        }
    }

done:
    outb(0x20, 0x20);
}

void __attribute__((interrupt)) timer_isr(void *frame) {
    (void)frame;
    timer_ticks++;
    outb(0x20, 0x20);
    timer_tick();
    schedule();
}

void __attribute__((interrupt)) default_isr(void *frame) {
    (void)frame;
    outb(0x20, 0x20);
}

void init_interrupts(void) {
    for (int i = 0; i < IDT_SIZE; i++) {
        idt_set_gate(i, (uint32_t)default_isr, 0x08, 0x8E);
    }
    idt_set_gate(0x20, (uint32_t)timer_isr, 0x08, 0x8E);
    idt_set_gate(0x21, (uint32_t)keyboard_isr, 0x08, 0x8E);
    idt_set_gate(0x2C, (uint32_t)mouse_isr, 0x08, 0x8E);
    idt_set_gate(6,  (uint32_t)invalid_opcode_isr, 0x08, 0x8E);
    idt_set_gate(8,  (uint32_t)general_protection_isr, 0x08, 0x8E);
    idt_set_gate(14, (uint32_t)page_fault_isr, 0x08, 0x8E);
    idt_set_gate(0x80, (uint32_t)syscall_handler, 0x08, 0xEE);

    pic_init();
    pit_init();
    idt_load();
}
