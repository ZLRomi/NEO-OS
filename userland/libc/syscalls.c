/*
 * NEO libc - 系统调用 C 封装层（errno / strerror / perror）
 *
 * 原始汇编桩（syscall.S）直接返回 eax：负数表示错误，其绝对值即错误码。
 * 本文件提供 errno 全局变量与转换辅助，供 unistd.c 等 POSIX 封装使用。
 */

#include "neolibc.h"
#include <syscall.h>

int neo_errno = 0;

long neo_errno_check(long ret)
{
    if (ret < 0) {
        neo_errno = (int)(-ret);
        return -1;
    }
    neo_errno = 0;
    return ret;
}

const char* neo_strerror(int err)
{
    switch (err) {
    case 0:           return "Success";
    case EPERM:       return "Operation not permitted";
    case ENOENT:      return "No such file or directory";
    case EINTR:       return "Interrupted system call";
    case EIO:         return "I/O error";
    case EBADF:       return "Bad file descriptor";
    case EAGAIN:      return "Resource temporarily unavailable";
    case ENOMEM:      return "Cannot allocate memory";
    case EACCES:      return "Permission denied";
    case EFAULT:      return "Bad address";
    case EBUSY:       return "Device or resource busy";
    case EEXIST:      return "File exists";
    case ENOTDIR:     return "Not a directory";
    case EISDIR:      return "Is a directory";
    case EINVAL:      return "Invalid argument";
    case ENOSPC:      return "No space left on device";
    case EPIPE:       return "Broken pipe";
    case ENOSYS:      return "Function not implemented";
    case ENOTCONN:    return "Transport endpoint is not connected";
    case ETIMEDOUT:  return "Connection timed out";
    case ECONNREFUSED:return "Connection refused";
    default:          return "Unknown error";
    }
}

void neo_perror(const char* s)
{
    int e = neo_errno;
    if (s && *s) {
        neo_write(NEO_STDERR, s, neo_strlen(s));
        neo_write(NEO_STDERR, ": ", 2);
    }
    const char* msg = neo_strerror(e);
    neo_write(NEO_STDERR, msg, neo_strlen(msg));
    neo_write(NEO_STDERR, "\n", 1);
}
