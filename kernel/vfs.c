#include "vfs.h"
#include "fat32.h"
#include "printk.h"
#include <stdint.h>

static int kstrlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int kstrcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int kstrncmp(const char* a, const char* b, int n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (int)(unsigned char)*a - (int)(unsigned char)*b : 0;
}

static void kmemcpy(void* dst, const void* src, int n) {
    char* d = (char*)dst;
    const char* s = (const char*)src;
    while (n--) *d++ = *s++;
}

static void name_cpy(char* dst, const char* src, int max) {
    int i;
    for (i = 0; i < max - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

typedef struct {
    int in_use;
    char target[VFS_MAX_PATH];
    const vfs_fs_t* fs;
} vfs_mount_t;

typedef struct {
    int in_use;
    const vfs_fs_t* fs;
    int fs_fd;
} vfs_fd_t;

static vfs_mount_t mount_table[VFS_MAX_MOUNTS];
static vfs_fd_t fd_table[VFS_MAX_FD];
static const vfs_fs_t* fs_registry[8];
static int fs_reg_count;

static int register_fs(const vfs_fs_t* fs) {
    if (!fs || fs_reg_count >= 8) return -1;
    fs_registry[fs_reg_count++] = fs;
    return 0;
}

int vfs_register_fs(const vfs_fs_t* fs) {
    return register_fs(fs);
}

static const vfs_fs_t* find_fs(const char* name) {
    int i;
    for (i = 0; i < fs_reg_count; i++)
        if (kstrcmp(fs_registry[i]->name, name) == 0) return fs_registry[i];
    return 0;
}

static vfs_mount_t* find_mount(const char* path) {
    int i, best = -1, best_len = 0;
    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mount_table[i].in_use) continue;
        int tl = kstrlen(mount_table[i].target);
        if (kstrncmp(path, mount_table[i].target, tl) != 0) continue;
        if (!(tl == 1 && mount_table[i].target[0] == '/')) {
            char c = path[tl];
            if (c != '/' && c != '\0') continue;
        }
        if (tl > best_len) { best_len = tl; best = i; }
    }
    return best >= 0 ? &mount_table[best] : 0;
}

static const char* strip_prefix(const char* path, const char* target) {
    int tl = kstrlen(target);
    if (tl == 1 && target[0] == '/') return path;
    if (path[tl] == '\0') return "/";
    return path + tl;
}

/* ---------- FAT32 wrappers ---------- */
static int fat32_vfs_open(const char* path, int flags) {
    return fat32_open(path, flags);
}

static int fat32_vfs_close(int fd) {
    fat32_close(fd);
    return 0;
}

static int fat32_vfs_read(int fd, void* buf, int len) {
    return fat32_read(fd, buf, len);
}

static int fat32_vfs_write(int fd, const void* buf, int len) {
    return fat32_write(fd, buf, len);
}

static int fat32_vfs_mkdir(const char* path) {
    return fat32_mkdir(path);
}

static int fat32_vfs_unlink(const char* path) {
    return fat32_remove(path);
}

static int fat32_vfs_readdir(const char* path, void* buf, int len) {
    if (!buf || len <= 0) return -1;
    int dh = fat32_opendir(path);
    if (dh < 0) return -1;
    char name[260];
    int isdir;
    uint32_t size;
    int total = 0;
    char* out = (char*)buf;
    while (total < len) {
        if (!fat32_readdir(dh, name, &isdir, &size)) break;
        int nl = 0;
        while (name[nl]) nl++;
        if (total + nl + 1 > len) break;
        kmemcpy(out + total, name, nl);
        out[total + nl] = '\0';
        total += nl + 1;
    }
    fat32_close(dh);
    return total;
}

static int fat32_vfs_stat(const char* path, void* st) {
    vfs_stat_t* s = (vfs_stat_t*)st;
    if (!s) return -1;
    int dh = fat32_opendir(path);
    if (dh >= 0) {
        s->size = 0;
        s->is_dir = 1;
        fat32_close(dh);
        return 0;
    }
    int fd = fat32_open(path, 0);
    if (fd >= 0) {
        s->size = 0;
        s->is_dir = 0;
        fat32_close(fd);
        return 0;
    }
    return -1;
}

static const vfs_fs_t fat32_fs = {
    "fat32",
    fat32_vfs_open, fat32_vfs_close, fat32_vfs_read, fat32_vfs_write,
    fat32_vfs_readdir, fat32_vfs_stat, fat32_vfs_mkdir, fat32_vfs_unlink
};

static const vfs_fs_t ext2_fs = { "ext2", 0, 0, 0, 0, 0, 0, 0, 0 };
static const vfs_fs_t ntfs_fs = { "ntfs", 0, 0, 0, 0, 0, 0, 0, 0 };

int vfs_mount(const char* source, const char* target, const char* fstype) {
    (void)source;
    if (!target || !fstype) return -1;
    const vfs_fs_t* fs = find_fs(fstype);
    if (!fs) return -1;
    int i;
    for (i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mount_table[i].in_use && kstrcmp(mount_table[i].target, target) == 0) return -1;
    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mount_table[i].in_use) {
            mount_table[i].in_use = 1;
            name_cpy(mount_table[i].target, target, VFS_MAX_PATH);
            mount_table[i].fs = fs;
            return 0;
        }
    }
    return -1;
}

int vfs_umount(const char* target) {
    if (!target) return -1;
    int i;
    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mount_table[i].in_use && kstrcmp(mount_table[i].target, target) == 0) {
            mount_table[i].in_use = 0;
            mount_table[i].target[0] = '\0';
            mount_table[i].fs = 0;
            return 0;
        }
    }
    return -1;
}

int vfs_open(const char* path, int flags) {
    if (!path) return -1;
    vfs_mount_t* m = find_mount(path);
    if (!m || !m->fs || !m->fs->open) return -1;
    const char* sub = strip_prefix(path, m->target);
    int fs_fd = m->fs->open(sub, flags);
    if (fs_fd < 0) return -1;
    int i;
    for (i = 0; i < VFS_MAX_FD; i++) {
        if (!fd_table[i].in_use) {
            fd_table[i].in_use = 1;
            fd_table[i].fs = m->fs;
            fd_table[i].fs_fd = fs_fd;
            return i;
        }
    }
    if (m->fs->close) m->fs->close(fs_fd);
    return -1;
}

int vfs_close(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FD || !fd_table[fd].in_use) return -1;
    if (fd_table[fd].fs->close) fd_table[fd].fs->close(fd_table[fd].fs_fd);
    fd_table[fd].in_use = 0;
    return 0;
}

int vfs_read(int fd, void* buf, int len) {
    if (fd < 0 || fd >= VFS_MAX_FD || !fd_table[fd].in_use) return -1;
    if (!fd_table[fd].fs->read) return -1;
    return fd_table[fd].fs->read(fd_table[fd].fs_fd, buf, len);
}

int vfs_write(int fd, const void* buf, int len) {
    if (fd < 0 || fd >= VFS_MAX_FD || !fd_table[fd].in_use) return -1;
    if (!fd_table[fd].fs->write) return -1;
    return fd_table[fd].fs->write(fd_table[fd].fs_fd, buf, len);
}

int vfs_readdir(const char* path, void* buf, int len) {
    if (!path) return -1;
    vfs_mount_t* m = find_mount(path);
    if (!m || !m->fs || !m->fs->readdir) return -1;
    return m->fs->readdir(strip_prefix(path, m->target), buf, len);
}

int vfs_stat(const char* path, void* st) {
    if (!path) return -1;
    vfs_mount_t* m = find_mount(path);
    if (!m || !m->fs || !m->fs->stat) return -1;
    return m->fs->stat(strip_prefix(path, m->target), st);
}

int vfs_mkdir(const char* path) {
    if (!path) return -1;
    vfs_mount_t* m = find_mount(path);
    if (!m || !m->fs || !m->fs->mkdir) return -1;
    return m->fs->mkdir(strip_prefix(path, m->target));
}

int vfs_unlink(const char* path) {
    if (!path) return -1;
    vfs_mount_t* m = find_mount(path);
    if (!m || !m->fs || !m->fs->unlink) return -1;
    return m->fs->unlink(strip_prefix(path, m->target));
}

int vfs_init(void) {
    int i;
    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        mount_table[i].in_use = 0;
        mount_table[i].target[0] = '\0';
        mount_table[i].fs = 0;
    }
    for (i = 0; i < VFS_MAX_FD; i++) {
        fd_table[i].in_use = 0;
        fd_table[i].fs = 0;
        fd_table[i].fs_fd = -1;
    }
    fs_reg_count = 0;
    register_fs(&fat32_fs);
    register_fs(&ext2_fs);
    register_fs(&ntfs_fs);

    if (fat32_is_mounted()) {
        if (vfs_mount("disk0", "/", "fat32") == 0)
            printk("vfs: fat32 mounted at /\n");
        else
            printk("vfs: mount fat32 failed\n");
    } else {
        printk("vfs: fat32 not mounted, /\n");
    }
    return 0;
}
