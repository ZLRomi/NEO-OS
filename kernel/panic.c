#include "panic.h"
#include "serial.h"
#include "klog.h"
#include "vfs.h"
#include "rtc.h"
#include <stdarg.h>
#include <stdint.h>

#define PANIC_MSG_MAX     256
#define PANIC_NUMBUF_MAX  16
#define PANIC_MAX_FRAMES  8
#define PANIC_KLOG_BYTES  1024
#define PANIC_KERNEL_START 0xC0000000
#define PANIC_SUMMARY_SIZE 4096
#define PANIC_CRASH_DIR    "/var/crash"
#define PANIC_CRASH_LOG    "/var/crash/last_panic.log"

static char panic_summary_buf[PANIC_SUMMARY_SIZE];
static int  panic_summary_len = 0;

static void panic_format_dec(int v, char* out) {
    char tmp[12];
    int i = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) tmp[i++] = '0';
    while (v > 0) { tmp[i++] = '0' + (v % 10); v /= 10; }
    int j = 0;
    if (neg) out[j++] = '-';
    while (i > 0) out[j++] = tmp[--i];
    out[j] = '\0';
}

static void panic_format_hex(uint32_t v, char* out) {
    const char* hex = "0123456789abcdef";
    char tmp[9];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v > 0) { tmp[i++] = hex[v & 0xF]; v >>= 4; }
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = '\0';
}

static void panic_format(char* out, int max, const char* fmt, va_list args) {
    int pos = 0;
    char numbuf[PANIC_NUMBUF_MAX];
    for (int i = 0; fmt[i] != '\0' && pos < max - 1; i++) {
        if (fmt[i] == '%') {
            i++;
            if (fmt[i] == 'd') {
                int val = va_arg(args, int);
                panic_format_dec(val, numbuf);
                for (int j = 0; numbuf[j] && pos < max - 1; j++) out[pos++] = numbuf[j];
            } else if (fmt[i] == 'x') {
                uint32_t val = va_arg(args, uint32_t);
                panic_format_hex(val, numbuf);
                for (int j = 0; numbuf[j] && pos < max - 1; j++) out[pos++] = numbuf[j];
            } else if (fmt[i] == 's') {
                const char* str = va_arg(args, const char*);
                if (!str) str = "(null)";
                for (int j = 0; str[j] && pos < max - 1; j++) out[pos++] = str[j];
            } else if (fmt[i] == 'c') {
                char c = (char)va_arg(args, int);
                if (pos < max - 1) out[pos++] = c;
            } else {
                if (pos < max - 1) out[pos++] = '%';
                if (fmt[i] && pos < max - 1) out[pos++] = fmt[i];
            }
        } else {
            if (pos < max - 1) out[pos++] = fmt[i];
        }
    }
    out[pos] = '\0';
}

static void summary_reset(void) {
    panic_summary_len = 0;
    panic_summary_buf[0] = '\0';
}

static void summary_putc(char c) {
    if (panic_summary_len < PANIC_SUMMARY_SIZE - 1) {
        panic_summary_buf[panic_summary_len++] = c;
        panic_summary_buf[panic_summary_len] = '\0';
    }
}

static void summary_puts(const char* s) {
    if (!s) s = "(null)";
    while (*s && panic_summary_len < PANIC_SUMMARY_SIZE - 1) {
        panic_summary_buf[panic_summary_len++] = *s++;
    }
    panic_summary_buf[panic_summary_len] = '\0';
}

static void summary_printf(const char* fmt, ...) {
    char tmp[PANIC_MSG_MAX];
    va_list args;
    va_start(args, fmt);
    panic_format(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    summary_puts(tmp);
}

static void panic_append_timestamp(void) {
    rtc_time_t t;
    if (rtc_get_time(&t) == 0) {
        summary_puts("Timestamp: ");
        summary_printf("%d", t.year);
        summary_putc('-');
        if (t.month < 10) summary_putc('0');
        summary_printf("%d", t.month);
        summary_putc('-');
        if (t.day < 10) summary_putc('0');
        summary_printf("%d", t.day);
        summary_putc(' ');
        if (t.hour < 10) summary_putc('0');
        summary_printf("%d", t.hour);
        summary_putc(':');
        if (t.minute < 10) summary_putc('0');
        summary_printf("%d", t.minute);
        summary_putc(':');
        if (t.second < 10) summary_putc('0');
        summary_printf("%d", t.second);
        summary_putc('\n');
    } else {
        summary_puts("Timestamp: unknown\n");
    }
}

static void panic_append_registers(const panic_context_t* ctx) {
    summary_puts("Register dump:\n");
    summary_printf("  EIP=0x%08x ESP=0x%08x EBP=0x%08x\n",
                   ctx->eip, ctx->esp, ctx->ebp);
    summary_printf("  EAX=0x%08x EBX=0x%08x ECX=0x%08x EDX=0x%08x\n",
                   ctx->eax, ctx->ebx, ctx->ecx, ctx->edx);
    summary_printf("  ESI=0x%08x EDI=0x%08x EFLAGS=0x%08x\n",
                   ctx->esi, ctx->edi, ctx->eflags);
    summary_printf("  CR0=0x%08x CR2=0x%08x CR3=0x%08x\n",
                   ctx->cr0, ctx->cr2, ctx->cr3);
    summary_printf("  vector=0x%08x error_code=0x%08x\n",
                   ctx->exception_vector, ctx->error_code);
}

static void panic_append_klog(void) {
    char kbuf[PANIC_KLOG_BYTES + 1];
    int n = klog_read_recent(kbuf, sizeof(kbuf), PANIC_KLOG_BYTES);
    if (n <= 0) return;
    summary_puts("--- recent klog ---\n");
    summary_puts(kbuf);
    summary_puts("\n--- end klog ---\n");
}

void panic_write_crash_log(const char* summary) {
    if (!summary) return;

    vfs_stat_t st;
    if (vfs_stat("/", &st) != 0) return;
    if (vfs_stat(PANIC_CRASH_DIR, &st) != 0) {
        if (vfs_mkdir(PANIC_CRASH_DIR) != 0) return;
    }

    int fd = vfs_open(PANIC_CRASH_LOG, 1);
    if (fd < 0) return;

    int len = 0;
    while (summary[len]) len++;
    if (len > 0) {
        (void)vfs_write(fd, summary, len);
    }
    vfs_close(fd);
}

static void panic_print_klog(void) {
    char kbuf[PANIC_KLOG_BYTES + 1];
    int n = klog_read_recent(kbuf, sizeof(kbuf), PANIC_KLOG_BYTES);
    if (n <= 0) return;
    serial_puts("--- recent klog ---\n");
    serial_puts(kbuf);
    serial_puts("\n--- end klog ---\n");
}

static int panic_frame_valid(uint32_t addr) {
    if (addr == 0) return 0;
    if (addr < PANIC_KERNEL_START) return 0;
    if ((addr & 0x3) != 0) return 0;
    return 1;
}

static void panic_dump_registers(const panic_context_t* ctx) {
    serial_printf("Register dump:\n");
    serial_printf("  EIP=0x%08x ESP=0x%08x EBP=0x%08x\n",
                  ctx->eip, ctx->esp, ctx->ebp);
    serial_printf("  EAX=0x%08x EBX=0x%08x ECX=0x%08x EDX=0x%08x\n",
                  ctx->eax, ctx->ebx, ctx->ecx, ctx->edx);
    serial_printf("  ESI=0x%08x EDI=0x%08x EFLAGS=0x%08x\n",
                  ctx->esi, ctx->edi, ctx->eflags);
    serial_printf("  CR0=0x%08x CR2=0x%08x CR3=0x%08x\n",
                  ctx->cr0, ctx->cr2, ctx->cr3);
    serial_printf("  vector=0x%08x error_code=0x%08x\n",
                  ctx->exception_vector, ctx->error_code);
}

static void panic_stack_trace(uint32_t ebp, uint32_t eip) {
    serial_printf("Stack trace:\n");
    serial_printf("  #0 0x%08x\n", eip);

    uint32_t* frame = (uint32_t*)ebp;
    for (int i = 1; i < PANIC_MAX_FRAMES; i++) {
        if (!panic_frame_valid((uint32_t)frame)) break;
        uint32_t next_ebp = frame[0];
        uint32_t ret = frame[1];
        if (ret == 0) break;
        serial_printf("  #%d 0x%08x\n", i, ret);
        if (next_ebp == 0) break;
        frame = (uint32_t*)next_ebp;
    }
}

void panic(const char* fmt, ...) {
    __asm__ volatile("cli");

    char msg[PANIC_MSG_MAX];
    va_list args;
    va_start(args, fmt);
    panic_format(msg, sizeof(msg), fmt, args);
    va_end(args);

    serial_printf("*** PANIC: %s\n", msg);

    panic_context_t ctx;
    ctx.error_code = 0;
    ctx.exception_vector = 0;

    uint32_t cr0, cr2, cr3, esp, eflags, ebp;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%esp, %0" : "=r"(esp));
    __asm__ volatile("pushfl; pop %0" : "=r"(eflags));
    __asm__ volatile("mov %%ebp, %0" : "=r"(ebp));

    ctx.cr0 = cr0;
    ctx.cr2 = cr2;
    ctx.cr3 = cr3;
    ctx.esp = esp;
    ctx.eflags = eflags;
    ctx.ebp = ebp;

    uint32_t eax, ebx, ecx, edx, esi, edi;
    __asm__ volatile(
        "movl %%eax, %0\n"
        "movl %%ebx, %1\n"
        "movl %%ecx, %2\n"
        "movl %%edx, %3\n"
        "movl %%esi, %4\n"
        "movl %%edi, %5\n"
        : "=r"(eax), "=r"(ebx), "=r"(ecx),
          "=r"(edx), "=r"(esi), "=r"(edi)
        :
        : "memory"
    );

    ctx.eax = eax;
    ctx.ebx = ebx;
    ctx.ecx = ecx;
    ctx.edx = edx;
    ctx.esi = esi;
    ctx.edi = edi;

    uint32_t* frame = (uint32_t*)ebp;
    if (panic_frame_valid((uint32_t)frame)) {
        ctx.eip = frame[1];
    } else {
        ctx.eip = 0;
    }

    panic_dump_registers(&ctx);
    panic_stack_trace(ctx.ebp, ctx.eip);
    panic_print_klog();

    summary_reset();
    panic_append_timestamp();
    summary_puts("Panic: ");
    summary_puts(msg);
    summary_putc('\n');
    panic_append_registers(&ctx);
    panic_append_klog();
    panic_write_crash_log(panic_summary_buf);

    while (1) __asm__ volatile("hlt");
}

void panic_from_exception(const panic_context_t* ctx, const char* fmt, ...) {
    __asm__ volatile("cli");

    char msg[PANIC_MSG_MAX];
    va_list args;
    va_start(args, fmt);
    panic_format(msg, sizeof(msg), fmt, args);
    va_end(args);

    serial_printf("*** PANIC: %s (vector=0x%x error_code=0x%x cr2=0x%x)\n",
                  msg, ctx->exception_vector, ctx->error_code, ctx->cr2);

    panic_dump_registers(ctx);
    panic_stack_trace(ctx->ebp, ctx->eip);
    panic_print_klog();

    summary_reset();
    panic_append_timestamp();
    summary_puts("Panic: ");
    summary_puts(msg);
    summary_putc('\n');
    panic_append_registers(ctx);
    panic_append_klog();
    panic_write_crash_log(panic_summary_buf);

    while (1) __asm__ volatile("hlt");
}
