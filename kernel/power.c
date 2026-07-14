#include "power.h"
#include "io.h"
#include "serial.h"
#include "printk.h"
#include <stdint.h>

void power_init(void) {
    printk("power: initialized\n");
    serial_write("power: initialized\n");
}

void power_off(void) {
    serial_write("power: ACPI shutdown\n");
    __asm__ volatile("cli;");
    outw(0x604, 0x2000);
    while (1) __asm__ volatile("hlt");
}

void power_reboot(void) {
    serial_write("power: reboot\n");
    __asm__ volatile("cli;");
    uint8_t good = 0x02;
    while (good & 0x02) good = inb(0x64);
    outb(0x64, 0xFE);
    outb(0x70, 0x64);
    outb(0x71, 0x00);
    while (1) __asm__ volatile("hlt");
}

int power_sleep(void) {
    return -1;
}

int power_hibernate(void) {
    return -1;
}
