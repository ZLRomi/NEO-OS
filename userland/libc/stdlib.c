/*
 * NEO libc - stdlib：内存管理 / exit / atoi / itoa
 *
 * 简化方案：malloc 直接调用 SYS_ALLOC(5) 向内核要页（内核按 4096 字节向上
 * 取整并映射到用户地址空间，返回基地址）；free 为空操作，由内核在进程
 * 退出时统一回收页表。
 */

#include "neolibc.h"

void* neo_malloc(size_t size)
{
    if (size == 0) size = 1;
    void* p = neo_alloc((int)size);
    return p;
}

void neo_free(void* ptr)
{
    (void)ptr;
}

void* neo_calloc(size_t n, size_t sz)
{
    size_t total = n * sz;
    if (n != 0 && total / n != sz) return (void*)0;
    void* p = neo_malloc(total);
    if (p) neo_memset(p, 0, total);
    return p;
}

void exit(int code)
{
    neo_exit(code);
    for (;;) { }
}

void _exit(int code)
{
    neo_exit(code);
    for (;;) { }
}

int neo_atoi(const char* s)
{
    int sign = 1;
    int v = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v * sign;
}

char* neo_itoa(long value, char* str, int base)
{
    if (base < 2 || base > 36) { str[0] = '\0'; return str; }

    unsigned long uv;
    int negative = 0;
    if (base == 10 && value < 0) {
        negative = 1;
        uv = (unsigned long)(-value);
    } else {
        uv = (unsigned long)value;
    }

    char tmp[32];
    int i = 0;
    if (uv == 0) tmp[i++] = '0';
    while (uv > 0) {
        int d = (int)(uv % (unsigned long)base);
        tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
        uv /= (unsigned long)base;
    }
    if (negative) tmp[i++] = '-';

    int j = 0;
    while (i > 0) str[j++] = tmp[--i];
    str[j] = '\0';
    return str;
}

/* POSIX 风格弱别名：指向同文件中的 neo_ 实现，可在应用中覆盖 */
int    atoi(const char* s) __attribute__((weak, alias("neo_atoi")));
char*  itoa(long value, char* str, int base) __attribute__((weak, alias("neo_itoa")));
void*  malloc(size_t size) __attribute__((weak, alias("neo_malloc")));
void    free(void* ptr) __attribute__((weak, alias("neo_free")));
void*  calloc(size_t n, size_t sz) __attribute__((weak, alias("neo_calloc")));
