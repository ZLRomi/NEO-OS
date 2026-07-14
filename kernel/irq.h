#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

#define IRQ_MAX 16

typedef void (*irq_handler_t)(void* frame);

void irq_init(void);
void register_irq_handler(int irq, irq_handler_t handler);
void irq_dispatch(int irq, void* frame);

#endif
