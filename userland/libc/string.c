/*
 * NEO libc - 字符串与内存函数
 *
 * 弱别名使 POSIX 风格名（strlen/memcpy 等）指向 neo_ 实现，可在应用中覆盖。
 * 编译需 -fno-builtin -fno-tree-loop-distribute-patterns 防止 GCC 将这些
 * 循环再改写为对自身 memcpy/memset 的调用（无限递归）。
 */

#include "neolibc.h"

size_t neo_strlen(const char* s)
{
    const char* p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

char* neo_strcpy(char* dst, const char* src)
{
    char* d = dst;
    while ((*d++ = *src++) != '\0') { }
    return dst;
}

char* neo_strncpy(char* dst, const char* src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i] != '\0'; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char* neo_strcat(char* dst, const char* src)
{
    char* d = dst + neo_strlen(dst);
    while ((*d++ = *src++) != '\0') { }
    return dst;
}

int neo_strcmp(const char* a, const char* b)
{
    while (*a && (*a == *b)) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int neo_strncmp(const char* a, const char* b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0) return 0;
    }
    return 0;
}

char* neo_strchr(const char* s, int c)
{
    char ch = (char)c;
    for (;; s++) {
        if (*s == ch) return (char*)s;
        if (*s == '\0') return (void*)0;
    }
}

void* neo_memcpy(void* dst, const void* src, size_t n)
{
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void* neo_memset(void* dst, int c, size_t n)
{
    unsigned char* d = (unsigned char*)dst;
    unsigned char v = (unsigned char)c;
    for (size_t i = 0; i < n; i++) d[i] = v;
    return dst;
}

int neo_memcmp(const void* a, const void* b, size_t n)
{
    const unsigned char* pa = (const unsigned char*)a;
    const unsigned char* pb = (const unsigned char*)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

void* neo_memmove(void* dst, const void* src, size_t n)
{
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

/* POSIX 风格弱别名：指向同文件中的 neo_ 实现，可在应用中覆盖 */
size_t strlen(const char* s) __attribute__((weak, alias("neo_strlen")));
char*  strcpy(char* dst, const char* src) __attribute__((weak, alias("neo_strcpy")));
char*  strncpy(char* dst, const char* src, size_t n) __attribute__((weak, alias("neo_strncpy")));
char*  strcat(char* dst, const char* src) __attribute__((weak, alias("neo_strcat")));
int    strcmp(const char* a, const char* b) __attribute__((weak, alias("neo_strcmp")));
int    strncmp(const char* a, const char* b, size_t n) __attribute__((weak, alias("neo_strncmp")));
char*  strchr(const char* s, int c) __attribute__((weak, alias("neo_strchr")));
void*  memcpy(void* dst, const void* src, size_t n) __attribute__((weak, alias("neo_memcpy")));
void*  memset(void* dst, int c, size_t n) __attribute__((weak, alias("neo_memset")));
int    memcmp(const void* a, const void* b, size_t n) __attribute__((weak, alias("neo_memcmp")));
void*  memmove(void* dst, const void* src, size_t n) __attribute__((weak, alias("neo_memmove")));
