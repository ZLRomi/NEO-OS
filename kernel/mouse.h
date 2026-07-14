#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

extern volatile int mouse_x;
extern volatile int mouse_y;
extern volatile uint8_t mouse_buttons;

void mouse_init(void);
void __attribute__((interrupt)) mouse_isr(void *frame);

#endif
