#include "irq.h"
#include "klog.h"
#include <stdint.h>

static irq_handler_t irq_handlers[IRQ_MAX];
static int irq_inited = 0;

void irq_init(void) {
    for (int i = 0; i < IRQ_MAX; i++) irq_handlers[i] = 0;
    irq_inited = 1;
    klog_write_str("irq: dispatcher initialized\n");
}

void register_irq_handler(int irq, irq_handler_t handler) {
    if (!irq_inited) return;
    if (irq < 0 || irq >= IRQ_MAX) return;
    irq_handlers[irq] = handler;
    klog_write_level(KLOG_INFO, "irq: registered handler for irq=%d\n", irq);
}

void irq_dispatch(int irq, void* frame) {
    if (!irq_inited) return;
    if (irq < 0 || irq >= IRQ_MAX) return;
    if (irq_handlers[irq]) {
        irq_handlers[irq](frame);
    }
}
