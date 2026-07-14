#include "klog.h"
#include "printk.h"
#include "serial.h"
#include <stdarg.h>
#include <stdint.h>

static char klog_buf[KLOG_SIZE];
static int klog_head = 0;
static int klog_tail = 0;
static int klog_count = 0;
static int klog_inited = 0;
static int g_klog_level = KLOG_INFO;

void klog_init(void) {
    klog_head = 0;
    klog_tail = 0;
    klog_count = 0;
    klog_inited = 1;
    for (int i = 0; i < KLOG_SIZE; i++) klog_buf[i] = 0;
    printk("klog: ring buffer initialized (%d bytes)\n", KLOG_SIZE);
}

static void klog_putc(char c) {
    klog_buf[klog_head] = c;
    klog_head = (klog_head + 1) % KLOG_SIZE;
    if (klog_count < KLOG_SIZE) {
        klog_count++;
    } else {
        klog_tail = (klog_tail + 1) % KLOG_SIZE;
    }
}

static int klog_dec(int v, char* out) {
    char tmp[12];
    int i = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) tmp[i++] = '0';
    while (v > 0) { tmp[i++] = '0' + (v % 10); v /= 10; }
    int j = 0;
    if (neg) out[j++] = '-';
    while (i > 0) out[j++] = tmp[--i];
    out[j] = '\0';
    return j;
}

static int klog_hex(uint32_t v, char* out) {
    const char* hex = "0123456789abcdef";
    char tmp[9];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v > 0) { tmp[i++] = hex[v & 0xF]; v >>= 4; }
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = '\0';
    return j;
}

void klog_write_str(const char* s) {
    if (!klog_inited || !s) return;
    while (*s) {
        klog_putc(*s++);
    }
}

static const char* klog_level_prefix(int level) {
    switch (level) {
        case KLOG_DEBUG: return "[DEBUG] ";
        case KLOG_INFO:  return "[INFO] ";
        case KLOG_WARN:  return "[WARN] ";
        case KLOG_ERROR: return "[ERROR] ";
        case KLOG_FATAL: return "[FATAL] ";
        default:         return "[INFO] ";
    }
}

static void klog_format_into(char* out, int max, const char* fmt, va_list args) {
    int pos = 0;
    char numbuf[16];
    for (int i = 0; fmt[i] != '\0' && pos < max - 1; i++) {
        if (fmt[i] == '%') {
            i++;
            if (fmt[i] == 'd') {
                int val = va_arg(args, int);
                klog_dec(val, numbuf);
                for (int j = 0; numbuf[j] && pos < max - 1; j++) out[pos++] = numbuf[j];
            } else if (fmt[i] == 'x') {
                uint32_t val = va_arg(args, uint32_t);
                klog_hex(val, numbuf);
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

void klog_write_level(int level, const char* fmt, ...) {
    if (!klog_inited || !fmt) return;
    if (level < g_klog_level) return;

    const char* prefix = klog_level_prefix(level);
    char entry[KLOG_ENTRY_MAX];
    int pos = 0;
    for (int i = 0; prefix[i] && pos < KLOG_ENTRY_MAX - 1; i++) {
        entry[pos++] = prefix[i];
    }

    va_list args;
    va_start(args, fmt);
    klog_format_into(entry + pos, KLOG_ENTRY_MAX - pos, fmt, args);
    va_end(args);

    klog_write_str(entry);
    serial_puts(entry);
}

void klog_set_level(int level) {
    g_klog_level = level;
}

int klog_get_level(void) {
    return g_klog_level;
}

int klog_read(char* buf, int max) {
    if (!klog_inited || !buf || max <= 0) return 0;
    int n = klog_count;
    if (n > max - 1) n = max - 1;
    int pos = klog_tail;
    for (int i = 0; i < n; i++) {
        buf[i] = klog_buf[pos];
        pos = (pos + 1) % KLOG_SIZE;
    }
    buf[n] = '\0';
    return n;
}

int klog_read_recent(char* buf, int max, int recent_bytes) {
    if (!klog_inited || !buf || max <= 0 || recent_bytes <= 0) return 0;
    int n = recent_bytes;
    if (n > klog_count) n = klog_count;
    if (n > max - 1) n = max - 1;
    int pos = klog_head - n;
    if (pos < 0) pos += KLOG_SIZE;
    for (int i = 0; i < n; i++) {
        buf[i] = klog_buf[pos];
        pos = (pos + 1) % KLOG_SIZE;
    }
    buf[n] = '\0';
    return n;
}
