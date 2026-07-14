#include "fault.h"
#include "memory.h"
#include "panic.h"
#include "process.h"
#include "printk.h"
#include "serial.h"
#include "sched.h"
#include "klog.h"
#include <stdint.h>

#define USER_SPACE_START    0x08000000
#define USER_SPACE_END      0x40000000
#define KERNEL_SPACE_START  0xC0000000

static void panic_context_fill(panic_context_t* ctx, uint32_t vector, uint32_t error_code) {
    ctx->exception_vector = vector;
    ctx->error_code = error_code;

    __asm__ volatile("mov %%cr0, %0" : "=r"(ctx->cr0));
    __asm__ volatile("mov %%cr2, %0" : "=r"(ctx->cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(ctx->cr3));
    __asm__ volatile("mov %%esp, %0" : "=r"(ctx->esp));
    __asm__ volatile("pushfl; pop %0" : "=r"(ctx->eflags));
    __asm__ volatile("mov %%ebp, %0" : "=r"(ctx->ebp));

    __asm__ volatile(
        "movl %%eax, %0\n"
        "movl %%ebx, %1\n"
        "movl %%ecx, %2\n"
        "movl %%edx, %3\n"
        "movl %%esi, %4\n"
        "movl %%edi, %5\n"
        : "=r"(ctx->eax), "=r"(ctx->ebx), "=r"(ctx->ecx),
          "=r"(ctx->edx), "=r"(ctx->esi), "=r"(ctx->edi)
        :
        : "memory"
    );

    uint32_t ebp = ctx->ebp;
    if (ebp >= KERNEL_SPACE_START && (ebp & 0x3) == 0) {
        ctx->eip = ((uint32_t*)ebp)[1];
    } else {
        ctx->eip = 0;
    }
}

static void terminate_current(void) {
    process_t* cur = process_get_current();
    if (cur) {
        cur->state = PROCESS_TERMINATED;
        process_ready_remove(cur);
    }
    schedule();
}

void handle_user_page_fault(uint32_t addr, uint32_t error_code) {
    if (addr < USER_SPACE_START || addr >= USER_SPACE_END) {
        printk("fault: illegal user addr=0x%x code=0x%x\n", addr, error_code);
        klog_write_level(KLOG_WARN, "pagefault: illegal addr=0x%x code=0x%x\n", addr, error_code);
        process_t* cur = process_get_current();
        if (cur) {
            signal_send(cur->pid, SIGSEGV);
            signal_deliver(SIGSEGV);
            return;
        }
        terminate_current();
        return;
    }

    // bit0=0: page not present -> demand allocate
    if ((error_code & 0x1) == 0) {
        uint32_t phys = pmm_alloc_page();
        if (!phys) {
            printk("fault: pmm_alloc_page failed\n");
            terminate_current();
            return;
        }
        if (!map_page(addr & 0xFFFFF000, phys,
                       PAGE_PRESENT | PAGE_WRITE | PAGE_USER)) {
            printk("fault: map_page failed for 0x%x\n", addr);
            pmm_free_page(phys);
            terminate_current();
            return;
        }
        {
            process_t* cur = process_get_current();
            klog_write_level(KLOG_INFO, "pagefault: demand alloc addr=0x%x pid=%d\n", addr, cur ? cur->pid : -1);
        }
        return;  // CPU retries faulting instruction
    }

    // protection violation or other: send SIGSEGV
    printk("fault: cannot handle user fault addr=0x%x code=0x%x\n", addr, error_code);
    klog_write_level(KLOG_WARN, "pagefault: unhandled addr=0x%x code=0x%x\n", addr, error_code);
    {
        process_t* cur = process_get_current();
        if (cur) {
            signal_send(cur->pid, SIGSEGV);
            signal_deliver(SIGSEGV);
            return;
        }
        terminate_current();
    }
}

void __attribute__((interrupt)) page_fault_isr(void* frame, uint32_t error_code) {
    (void)frame;
    panic_context_t ctx;
    panic_context_fill(&ctx, 14, error_code);

    process_t* cur = process_get_current();
    int pid = cur ? cur->pid : -1;

    printk("page fault: addr=0x%x code=0x%x pid=%d\n", ctx.cr2, error_code, pid);

    if (ctx.cr2 < KERNEL_SPACE_START) {
        handle_user_page_fault(ctx.cr2, error_code);
        return;
    }

    // kernel space fault: panic
    klog_write_level(KLOG_FATAL, "kernel page fault: addr=0x%x code=0x%x pid=%d\n",
                     ctx.cr2, error_code, pid);
    panic_from_exception(&ctx, "kernel page fault");
}

void __attribute__((interrupt)) general_protection_isr(void* frame, uint32_t error_code) {
    (void)frame;
    panic_context_t ctx;
    panic_context_fill(&ctx, 13, error_code);
    klog_write_level(KLOG_FATAL, "general protection fault: code=0x%x\n", error_code);
    panic_from_exception(&ctx, "general protection fault");
}

void __attribute__((interrupt)) invalid_opcode_isr(void* frame) {
    (void)frame;
    panic_context_t ctx;
    panic_context_fill(&ctx, 6, 0);
    klog_write_level(KLOG_FATAL, "invalid opcode\n");
    panic_from_exception(&ctx, "invalid opcode");
}
