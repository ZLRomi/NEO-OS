#ifndef PANIC_H
#define PANIC_H

#include <stdint.h>

typedef struct {
    uint32_t eip;
    uint32_t esp;
    uint32_t eflags;
    uint32_t cr0;
    uint32_t cr2;
    uint32_t cr3;
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    uint32_t esi;
    uint32_t edi;
    uint32_t ebp;
    uint32_t error_code;
    uint32_t exception_vector;
} panic_context_t;

void panic(const char* fmt, ...);
void panic_from_exception(const panic_context_t* ctx, const char* fmt, ...);

#define kernel_assert(cond) \
    do { if (!(cond)) panic("ASSERTION FAILED: %s at %s:%d", #cond, __FILE__, __LINE__); } while (0)

#endif
