#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>

#define MAX_PATH 256
#define MAX_FD 16

int fat32_init(void);
int fat32_open(const char* path, int flags);
int fat32_read(int fd, void* buf, int count);
int fat32_write(int fd, const void* buf, int count);
void fat32_close(int fd);

int fat32_opendir(const char* path);
int fat32_readdir(int dh, char* name, int* isdir, uint32_t* size);

int fat32_create(const char* path);
int fat32_mkdir(const char* path);
int fat32_remove(const char* path);

void fat32_setcwd(const char* path);
const char* fat32_getcwd(void);

int fat32_is_mounted(void);

int fat32_dup(int oldfd);
int fat32_dup2(int oldfd, int newfd);

#endif
