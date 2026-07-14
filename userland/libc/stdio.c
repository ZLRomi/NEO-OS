/*
 * NEO libc - stdio：printf / puts / putchar
 *
 * printf 支持转换：d i u o x X p s c %
 * 支持：标志 '-' 左对齐、'0' 零填充、'+'/' ' 正负号、'# 进制前缀；
 *       宽度、精度（%s 截断、数值最小位数）；
 *       长度修饰 h hh l ll z（数值按 long/unsigned long 取参）。
 * 不支持浮点（无 FPU）。
 */

#include "neolibc.h"

static void emit_pad(int fd, char c, int n)
{
    char buf[64];
    if (n <= 0) return;
    for (int i = 0; i < 64; i++) buf[i] = c;
    while (n >= 64) { neo_write(fd, buf, 64); n -= 64; }
    if (n > 0) neo_write(fd, buf, (size_t)n);
}

static int u2s(unsigned long v, unsigned base, int upper, char* buf)
{
    int i = 0;
    if (v == 0) { buf[i++] = '0'; return i; }
    while (v > 0) {
        unsigned d = (unsigned)(v % base);
        buf[i++] = (char)(d < 10 ? '0' + d : (upper ? 'A' + d - 10 : 'a' + d - 10));
        v /= base;
    }
    for (int a = 0, b = i - 1; a < b; a++, b--) {
        char t = buf[a]; buf[a] = buf[b]; buf[b] = t;
    }
    return i;
}

int neo_vfprintf(int fd, const char* fmt, va_list ap)
{
    int total = 0;
    const char* p = fmt;

    while (*p) {
        if (*p != '%') {
            const char* start = p;
            while (*p && *p != '%') p++;
            total += (int)neo_write(fd, start, (size_t)(p - start));
            continue;
        }

        p++;
        if (*p == '%') { total += (int)neo_write(fd, "%", 1); p++; continue; }

        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;;) {
            char f = *p;
            if (f == '-') left = 1;
            else if (f == '0') zero = 1;
            else if (f == '+') plus = 1;
            else if (f == ' ') space = 1;
            else if (f == '#') alt = 1;
            else break;
            p++;
        }

        int width = 0;
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }

        int precision = -1;
        if (*p == '.') {
            p++;
            precision = 0;
            while (*p >= '0' && *p <= '9') { precision = precision * 10 + (*p - '0'); p++; }
        }

        int is_long = 0;
        if (*p == 'l') { is_long = 1; p++; if (*p == 'l') { is_long = 2; p++; } }
        else if (*p == 'h') { p++; if (*p == 'h') p++; }
        else if (*p == 'z' || *p == 'j' || *p == 't') { is_long = 1; p++; }

        char conv = *p;
        if (conv == 0) break;
        p++;

        char numbuf[32];
        int nlen = 0;
        const char* sstr = (const char*)0;
        int slen = 0;
        char sign = 0;
        const char* prefix = (const char*)0;
        int prefix_len = 0;

        switch (conv) {
        case 'd':
        case 'i': {
            long sv = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
            unsigned long uv;
            if (sv < 0) { sign = '-'; uv = (unsigned long)(-sv); }
            else { if (plus) sign = '+'; else if (space) sign = ' '; uv = (unsigned long)sv; }
            nlen = u2s(uv, 10, 0, numbuf);
            break;
        }
        case 'u': {
            unsigned long uv = is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            nlen = u2s(uv, 10, 0, numbuf);
            break;
        }
        case 'o': {
            unsigned long uv = is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            nlen = u2s(uv, 8, 0, numbuf);
            if (alt && nlen > 0 && numbuf[0] != '0') { prefix = "0"; prefix_len = 1; }
            break;
        }
        case 'x':
        case 'X': {
            unsigned long uv = is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            nlen = u2s(uv, 16, conv == 'X', numbuf);
            if (alt && nlen > 0) { prefix = (conv == 'X') ? "0X" : "0x"; prefix_len = 2; }
            break;
        }
        case 'p': {
            unsigned long uv = (unsigned long)(unsigned long)va_arg(ap, void*);
            nlen = u2s(uv, 16, 0, numbuf);
            prefix = "0x"; prefix_len = 2;
            break;
        }
        case 's': {
            sstr = va_arg(ap, const char*);
            if (!sstr) sstr = "(null)";
            slen = (int)neo_strlen(sstr);
            if (precision >= 0 && precision < slen) slen = precision;
            break;
        }
        case 'c': {
            int ch = va_arg(ap, int);
            numbuf[0] = (char)ch;
            nlen = 1;
            conv = 'c';
            break;
        }
        default:
            numbuf[0] = '%'; numbuf[1] = conv; nlen = 2;
            break;
        }

        if (sstr != (const char*)0) {
            int pad = width - slen;
            if (pad < 0) pad = 0;
            if (!left) emit_pad(fd, ' ', pad);
            total += (int)neo_write(fd, sstr, (size_t)slen);
            if (left) emit_pad(fd, ' ', pad);
            total += pad;
            continue;
        }

        int prec_zeros = 0;
        if (precision >= 0 && conv != 'c') {
            if (nlen < precision) prec_zeros = precision - nlen;
        }
        int content_len = nlen + prec_zeros;
        int total_len = content_len + (sign ? 1 : 0) + prefix_len;
        int pad = width - total_len;
        if (pad < 0) pad = 0;

        if (!left && !(zero && precision < 0)) emit_pad(fd, ' ', pad);

        if (sign) { total += (int)neo_write(fd, &sign, 1); }
        if (prefix_len) total += (int)neo_write(fd, prefix, (size_t)prefix_len);

        if (zero && precision < 0 && !left) {
            emit_pad(fd, '0', pad);
        }
        emit_pad(fd, '0', prec_zeros);
        total += (int)neo_write(fd, numbuf, (size_t)nlen);

        if (left) emit_pad(fd, ' ', pad);
        total += pad;
    }
    return total;
}

int neo_vprintf(const char* fmt, va_list ap)
{
    return neo_vfprintf(NEO_STDOUT, fmt, ap);
}

int neo_printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = neo_vfprintf(NEO_STDOUT, fmt, ap);
    va_end(ap);
    return n;
}

int vprintf(const char* fmt, va_list ap)
{
    return neo_vfprintf(NEO_STDOUT, fmt, ap);
}

int printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = neo_vfprintf(NEO_STDOUT, fmt, ap);
    va_end(ap);
    return n;
}

int neo_fprintf(int fd, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = neo_vfprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

int fprintf(int fd, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = neo_vfprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

int neo_putchar(int c)
{
    char ch = (char)c;
    return (int)neo_write(NEO_STDOUT, &ch, 1);
}

int putchar(int c)
{
    char ch = (char)c;
    return (int)neo_write(NEO_STDOUT, &ch, 1);
}

int neo_puts(const char* s)
{
    int n = (int)neo_write(NEO_STDOUT, s, neo_strlen(s));
    n += (int)neo_write(NEO_STDOUT, "\n", 1);
    return n;
}

int puts(const char* s)
{
    int n = (int)neo_write(NEO_STDOUT, s, neo_strlen(s));
    n += (int)neo_write(NEO_STDOUT, "\n", 1);
    return n;
}
