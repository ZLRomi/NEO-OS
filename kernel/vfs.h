#ifndef VFS_H
#define VFS_H

#include <stdint.h>

#define VFS_MAX_MOUNTS 8
#define VFS_MAX_FD 32
#define VFS_MAX_PATH 64

typedef struct vfs_filesystem {
    char name[8];
    int (*open)(const char* path, int flags);
    int (*close)(int fd);
    int (*read)(int fd, void* buf, int len);
    int (*write)(int fd, const void* buf, int len);
    int (*readdir)(const char* path, void* buf, int len);
    int (*stat)(const char* path, void* st);
    int (*mkdir)(const char* path);
    int (*unlink)(const char* path);
} vfs_fs_t;

typedef struct {
    uint32_t size;
    uint32_t is_dir;
} vfs_stat_t;

int vfs_init(void);
int vfs_register_fs(const vfs_fs_t* fs);
int vfs_mount(const char* source, const char* target, const char* fstype);
int vfs_umount(const char* target);
int vfs_open(const char* path, int flags);
int vfs_close(int fd);
int vfs_read(int fd, void* buf, int len);
int vfs_write(int fd, const void* buf, int len);
int vfs_readdir(const char* path, void* buf, int len);
int vfs_stat(const char* path, void* st);
int vfs_mkdir(const char* path);
int vfs_unlink(const char* path);

#endif
