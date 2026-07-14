#include "fat32.h"
#include "disk.h"
#include "heap.h"
#include "auth.h"
#include "serial.h"
#include "printk.h"
#include <stdint.h>
#include <stddef.h>

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME    0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LFN       0x0F

#define EOF_CLUSTER    0x0FFFFFF8
#define BAD_CLUSTER    0x0FFFFFF7

static struct {
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint32_t sectors_per_fat;
    uint32_t root_cluster;
    uint32_t fat_start;
    uint32_t data_start;
    uint32_t cluster_size;
    int mounted;
} fs;

typedef struct {
    int used;
    uint32_t cluster;
    uint32_t size;
    uint32_t pos;
    uint32_t start_cluster;
    int write;
} fd_t;

typedef struct {
    int used;
    uint32_t cluster;
    uint32_t pos_in_cluster;
    uint32_t cluster_idx;
} dh_t;

static fd_t fd_table[MAX_FD];
static dh_t dh_table[MAX_FD];

static char cwd_buf[MAX_PATH] = "/";

static uint8_t sector_buf[512];
static uint8_t fat_cache_sector[512];
static uint32_t fat_cached_sector = 0xFFFFFFFF;
static uint8_t dir_cache_buf[512];
static uint32_t dir_cached_sec = 0xFFFFFFFF;

static uint32_t fat_get_next(uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t sec = fs.fat_start + (fat_offset / fs.bytes_per_sector);
    uint32_t off = fat_offset % fs.bytes_per_sector;

    if (sec != fat_cached_sector) {
        if (disk_read_sector(sec, fat_cache_sector) < 0) return 0xFFFFFFFF;
        fat_cached_sector = sec;
    }
    uint32_t val = *(uint32_t*)(fat_cache_sector + off);
    return val & 0x0FFFFFFF;
}

static int fat_set_next(uint32_t cluster, uint32_t value) {
    uint32_t fat_offset = cluster * 4;
    uint32_t sec = fs.fat_start + (fat_offset / fs.bytes_per_sector);
    uint32_t off = fat_offset % fs.bytes_per_sector;

    if (sec != fat_cached_sector) {
        if (disk_read_sector(sec, fat_cache_sector) < 0) return -1;
        fat_cached_sector = sec;
    }
    uint32_t cur = *(uint32_t*)(fat_cache_sector + off);
    *(uint32_t*)(fat_cache_sector + off) = (cur & 0xF0000000) | (value & 0x0FFFFFFF);
    if (disk_write_sector(sec, fat_cache_sector) < 0) return -1;
    if (fs.num_fats > 1) {
        for (uint8_t f = 1; f < fs.num_fats; f++) {
            uint32_t sec_f = fs.fat_start + f * fs.sectors_per_fat + (fat_offset / fs.bytes_per_sector);
            if (disk_read_sector(sec_f, sector_buf) < 0) return -1;
            uint32_t* p = (uint32_t*)(sector_buf + off);
            *p = (cur & 0xF0000000) | (value & 0x0FFFFFFF);
            if (disk_write_sector(sec_f, sector_buf) < 0) return -1;
        }
    }
    return 0;
}

static uint32_t alloc_cluster(void) {
    uint32_t entries_per_sec = fs.bytes_per_sector / 4;
    uint32_t total_entries = fs.sectors_per_fat * entries_per_sec;
    for (uint32_t i = 0; i < fs.sectors_per_fat; i++) {
        uint32_t sec = fs.fat_start + i;
        if (disk_read_sector(sec, sector_buf) < 0) return 0;
        for (uint32_t j = 0; j < entries_per_sec; j++) {
            uint32_t c = i * entries_per_sec + j;
            if (c < 2 || c >= total_entries) continue;
            uint32_t* p = (uint32_t*)(sector_buf + j * 4);
            if ((*p & 0x0FFFFFFF) != 0) continue;
            *p = (*p & 0xF0000000) | (EOF_CLUSTER & 0x0FFFFFFF);
            if (disk_write_sector(sec, sector_buf) < 0) return 0;
            if (fat_cached_sector == sec) {
                *(uint32_t*)(fat_cache_sector + j * 4) = *p;
            }
            for (uint8_t f = 1; f < fs.num_fats; f++) {
                if (disk_write_sector(fs.fat_start + f * fs.sectors_per_fat + i, sector_buf) < 0) return 0;
            }
            return c;
        }
    }
    return 0;
}

static uint32_t cluster_to_sector(uint32_t cluster) {
    return fs.data_start + (cluster - 2) * fs.sectors_per_cluster;
}

static int name_to_83(const char* src, uint8_t* dst) {
    int i = 0, j = 0;
    while (src[i] && src[i] != '.' && j < 8) {
        char c = src[i++];
        if (c >= 'a' && c <= 'z') c -= 32;
        dst[j++] = c;
    }
    while (j < 8) dst[j++] = ' ';
    while (src[i] && src[i] != '.') i++;
    if (src[i] == '.') i++;
    while (j < 11) {
        char c = src[i++];
        if (!c) break;
        if (c >= 'a' && c <= 'z') c -= 32;
        dst[j++] = c;
    }
    while (j < 11) dst[j++] = ' ';
    return 0;
}

static void name_from_83(const uint8_t* src, char* dst) {
    int j = 0;
    for (int i = 0; i < 8; i++) {
        if (src[i] != ' ') dst[j++] = src[i];
    }
    if (src[8] != ' ') {
        dst[j++] = '.';
        for (int i = 8; i < 11; i++) {
            if (src[i] != ' ') dst[j++] = src[i];
        }
    }
    dst[j] = 0;
}

static int find_dir_entry(uint32_t dir_cluster, const char* name, uint8_t* entry_out) {
    uint8_t name83[11];
    name_to_83(name, name83);

    uint32_t cluster = dir_cluster;
    while (cluster < EOF_CLUSTER && cluster != 0) {
        for (uint32_t s = 0; s < fs.sectors_per_cluster; s++) {
            uint32_t sec = cluster_to_sector(cluster) + s;
            if (disk_read_sector(sec, sector_buf) < 0) return -1;
            for (int e = 0; e < 16; e++) {
                uint8_t* entry = sector_buf + e * 32;
                if (entry[0] == 0) return 0;
                if (entry[0] == 0xE5) continue;
                if ((entry[11] & ATTR_LFN) == ATTR_LFN) continue;
                int match = 1;
                for (int i = 0; i < 11; i++) {
                    if (entry[i] != name83[i]) { match = 0; break; }
                }
                if (match) {
                    for (int i = 0; i < 32; i++) entry_out[i] = entry[i];
                    return 1;
                }
            }
        }
        cluster = fat_get_next(cluster);
    }
    return 0;
}

static void resolve_path(const char* path, char* abs) {
    if (path[0] == '/') {
        int i = 0;
        while (path[i]) { abs[i] = path[i]; i++; }
        abs[i] = 0;
    } else {
        const char* c = cwd_buf;
        int i = 0;
        while (c[i]) { abs[i] = c[i]; i++; }
        if (i > 0 && abs[i - 1] != '/') abs[i++] = '/';
        int j = 0;
        while (path[j]) abs[i++] = path[j++];
        abs[i] = 0;
    }
}

static uint32_t path_to_cluster(const char* path, uint8_t* entry_out) {
    char abs[MAX_PATH];
    resolve_path(path, abs);

    uint32_t cluster = fs.root_cluster;
    if (entry_out) {
        for (int i = 0; i < 32; i++) entry_out[i] = 0;
        entry_out[11] = ATTR_DIRECTORY;
    }

    const char* p = abs;
    while (*p == '/') p++;

    while (*p) {
        char name[32];
        int i = 0;
        while (*p && *p != '/' && i < 31) name[i++] = *p++;
        name[i] = 0;
        while (*p == '/') p++;

        if (i == 0) continue;

        uint8_t entry[32];
        int r = find_dir_entry(cluster, name, entry);
        if (r != 1) return 0;

        uint32_t hi = *(uint16_t*)(entry + 0x14);
        uint32_t lo = *(uint16_t*)(entry + 0x1A);
        cluster = (hi << 16) | lo;

        if (entry_out) {
            for (int k = 0; k < 32; k++) entry_out[k] = entry[k];
        }
    }
    return cluster;
}

int fat32_init(void) {
    if (disk_get_sectors() == 0) {
        serial_write("fat32: no disk available\n");
        return -1;
    }

    if (disk_read_sector(0, sector_buf) < 0) {
        serial_write("fat32: read boot sector failed\n");
        return -1;
    }

    fs.bytes_per_sector = *(uint16_t*)(sector_buf + 0x0B);
    fs.sectors_per_cluster = sector_buf[0x0D];
    fs.reserved_sectors = *(uint16_t*)(sector_buf + 0x0E);
    fs.num_fats = sector_buf[0x10];
    fs.sectors_per_fat = *(uint32_t*)(sector_buf + 0x24);
    fs.root_cluster = *(uint32_t*)(sector_buf + 0x2C);
    fs.cluster_size = fs.sectors_per_cluster * fs.bytes_per_sector;

    if (fs.bytes_per_sector == 0 || fs.sectors_per_cluster == 0 ||
        fs.reserved_sectors == 0 || fs.sectors_per_fat == 0) {
        serial_write("fat32: invalid BPB\n");
        return -1;
    }

    if (sector_buf[510] != 0x55 || sector_buf[511] != 0xAA) {
        serial_write("fat32: invalid boot signature\n");
        return -1;
    }

    fs.fat_start = fs.reserved_sectors;
    fs.data_start = fs.reserved_sectors + fs.num_fats * fs.sectors_per_fat;
    fs.mounted = 1;
    fat_cached_sector = 0xFFFFFFFF;

    for (int i = 0; i < MAX_FD; i++) {
        fd_table[i].used = 0;
        dh_table[i].used = 0;
    }

    printk("fat32: mounted, bps=%d spc=%d root=%d data=%d\n",
           fs.bytes_per_sector, fs.sectors_per_cluster, fs.root_cluster, fs.data_start);
    return 0;
}

int fat32_is_mounted(void) {
    return fs.mounted;
}

static int path_starts_with(const char* abs, const char* prefix) {
    int i = 0;
    while (prefix[i]) {
        if (abs[i] != prefix[i]) return 0;
        i++;
    }
    return abs[i] == '/' || abs[i] == 0;
}

static int check_write_perm(const char* path) {
    if (auth_get_current_uid() == 0) return 0;
    char abs[MAX_PATH];
    resolve_path(path, abs);
    if (path_starts_with(abs, "/system")) {
        printk("fat32: write denied uid=%d path=%s\n", auth_get_current_uid(), abs);
        return -1;
    }
    return 0;
}

int fat32_open(const char* path, int flags) {
    if (!fs.mounted) return -1;

    if ((flags & 1) && check_write_perm(path) < 0) return -1;

    uint8_t entry[32];
    uint32_t cluster = path_to_cluster(path, entry);
    if (cluster == 0) {
        if (flags & 1) {
            if (fat32_create(path) < 0) return -1;
            uint8_t e2[32];
            cluster = path_to_cluster(path, e2);
            if (cluster == 0) return -1;
            for (int i = 0; i < 32; i++) entry[i] = e2[i];
        } else {
            return -1;
        }
    }
    if (entry[11] & ATTR_DIRECTORY) return -1;

    for (int i = 0; i < MAX_FD; i++) {
        if (!fd_table[i].used) {
            fd_table[i].used = 1;
            fd_table[i].start_cluster = cluster;
            fd_table[i].cluster = cluster;
            fd_table[i].size = *(uint32_t*)(entry + 0x1C);
            fd_table[i].pos = 0;
            fd_table[i].write = (flags & 1) ? 1 : 0;
            return i;
        }
    }
    return -1;
}

int fat32_read(int fd, void* buf, int count) {
    if (!fs.mounted || fd < 0 || fd >= MAX_FD || !fd_table[fd].used) return -1;
    fd_t* f = &fd_table[fd];

    if (f->pos >= f->size) return 0;
    int remain = f->size - f->pos;
    if (count > remain) count = remain;

    uint8_t* out = (uint8_t*)buf;
    int total = 0;

    while (count > 0) {
        uint32_t sec_in_cluster = (f->pos / fs.bytes_per_sector) % fs.sectors_per_cluster;
        uint32_t off_in_sector = f->pos % fs.bytes_per_sector;
        uint32_t sec = cluster_to_sector(f->cluster) + sec_in_cluster;

        if (disk_read_sector(sec, sector_buf) < 0) return total;

        int chunk = fs.bytes_per_sector - off_in_sector;
        if (chunk > count) chunk = count;
        for (int i = 0; i < chunk; i++) out[total + i] = sector_buf[off_in_sector + i];
        total += chunk;
        f->pos += chunk;
        count -= chunk;

        if (count > 0) {
            uint32_t next = fat_get_next(f->cluster);
            if (next >= EOF_CLUSTER || next == 0) break;
            f->cluster = next;
        }
    }
    return total;
}

int fat32_write(int fd, const void* buf, int count) {
    if (!fs.mounted || fd < 0 || fd >= MAX_FD || !fd_table[fd].used) return -1;
    fd_t* f = &fd_table[fd];
    if (!f->write) return -1;

    const uint8_t* in = (const uint8_t*)buf;
    int total = 0;

    while (count > 0) {
        uint32_t sec_in_cluster = (f->pos / fs.bytes_per_sector) % fs.sectors_per_cluster;
        uint32_t off_in_sector = f->pos % fs.bytes_per_sector;
        uint32_t sec = cluster_to_sector(f->cluster) + sec_in_cluster;

        if (off_in_sector != 0 || count < fs.bytes_per_sector) {
            if (disk_read_sector(sec, sector_buf) < 0) return total;
        }

        int chunk = fs.bytes_per_sector - off_in_sector;
        if (chunk > count) chunk = count;
        for (int i = 0; i < chunk; i++) sector_buf[off_in_sector + i] = in[total + i];
        if (disk_write_sector(sec, sector_buf) < 0) return total;

        total += chunk;
        f->pos += chunk;
        count -= chunk;

        if (count > 0) {
            uint32_t next = fat_get_next(f->cluster);
            if (next >= EOF_CLUSTER) {
                next = alloc_cluster();
                if (next == 0) break;
                fat_set_next(f->cluster, next);
                fat_set_next(next, EOF_CLUSTER);
            }
            f->cluster = next;
        }
    }

    if (f->pos > f->size) f->size = f->pos;
    return total;
}

void fat32_close(int fd) {
    if (fd < 0 || fd >= MAX_FD) return;
    fd_table[fd].used = 0;
    dh_table[fd].used = 0;
}

int fat32_opendir(const char* path) {
    if (!fs.mounted) return -1;
    uint32_t cluster;
    if (path[0] == 0 || (path[0] == '/' && path[1] == 0)) {
        cluster = fs.root_cluster;
    } else {
        cluster = path_to_cluster(path, NULL);
        if (cluster == 0) return -1;
    }
    for (int i = 0; i < MAX_FD; i++) {
        if (!dh_table[i].used) {
            dh_table[i].used = 1;
            dh_table[i].cluster = cluster;
            dh_table[i].pos_in_cluster = 0;
            dh_table[i].cluster_idx = cluster;
            dir_cached_sec = 0xFFFFFFFF;
            return i;
        }
    }
    return -1;
}

int fat32_readdir(int dh, char* name, int* isdir, uint32_t* size) {
    if (!fs.mounted || dh < 0 || dh >= MAX_FD || !dh_table[dh].used) return 0;
    dh_t* d = &dh_table[dh];
    uint32_t entries_per_sec = fs.bytes_per_sector / 32;
    uint32_t entries_per_cluster = fs.cluster_size / 32;

    for (;;) {
        if (d->cluster_idx >= EOF_CLUSTER || d->cluster_idx == 0) return 0;

        uint32_t sec_in_cluster = (d->pos_in_cluster / entries_per_sec) % fs.sectors_per_cluster;
        uint32_t sec = cluster_to_sector(d->cluster_idx) + sec_in_cluster;

        if (sec != dir_cached_sec) {
            if (disk_read_sector(sec, dir_cache_buf) < 0) return 0;
            dir_cached_sec = sec;
        }
        uint32_t entry_in_sec = d->pos_in_cluster % entries_per_sec;
        uint8_t* entry = dir_cache_buf + entry_in_sec * 32;

        uint8_t e0 = entry[0];
        uint8_t attr = entry[11];

        d->pos_in_cluster++;

        if (e0 == 0) {
            dir_cached_sec = 0xFFFFFFFF;
            return 0;
        }

        if (e0 != 0xE5 && (attr & ATTR_LFN) != ATTR_LFN && !(attr & ATTR_VOLUME)) {
            name_from_83(entry, name);
            if (isdir) *isdir = (attr & ATTR_DIRECTORY) ? 1 : 0;
            if (size) *size = *(uint32_t*)(entry + 0x1C);
            if (d->pos_in_cluster >= entries_per_cluster) {
                d->pos_in_cluster = 0;
                d->cluster_idx = fat_get_next(d->cluster_idx);
                dir_cached_sec = 0xFFFFFFFF;
            }
            return 1;
        }

        if (d->pos_in_cluster >= entries_per_cluster) {
            d->pos_in_cluster = 0;
            d->cluster_idx = fat_get_next(d->cluster_idx);
            dir_cached_sec = 0xFFFFFFFF;
        }
    }
}

static int dir_add_entry(uint32_t dir_cluster, uint8_t* new_entry) {
    uint32_t cluster = dir_cluster;
    while (cluster < EOF_CLUSTER && cluster != 0) {
        for (uint32_t s = 0; s < fs.sectors_per_cluster; s++) {
            uint32_t sec = cluster_to_sector(cluster) + s;
            if (disk_read_sector(sec, sector_buf) < 0) return -1;
            for (int e = 0; e < 16; e++) {
                uint8_t* entry = sector_buf + e * 32;
                uint8_t orig = entry[0];
                if (orig == 0 || orig == 0xE5) {
                    for (int i = 0; i < 32; i++) entry[i] = new_entry[i];
                    if (orig == 0 && e + 1 < 16) {
                        uint8_t* next = sector_buf + (e + 1) * 32;
                        next[0] = 0;
                    }
                    if (disk_write_sector(sec, sector_buf) < 0) return -1;
                    return 0;
                }
            }
        }
        uint32_t next = fat_get_next(cluster);
        if (next >= EOF_CLUSTER) {
            next = alloc_cluster();
            if (next == 0) return -1;
            fat_set_next(cluster, next);
            fat_set_next(next, EOF_CLUSTER);
            uint8_t zero[512];
            for (int i = 0; i < 512; i++) zero[i] = 0;
            for (uint32_t s = 0; s < fs.sectors_per_cluster; s++) {
                disk_write_sector(cluster_to_sector(next) + s, zero);
            }
        }
        cluster = next;
    }
    return -1;
}

static int make_entry(const char* name, int isdir, uint32_t parent_cluster, uint8_t* entry_out) {
    uint8_t name83[11];
    name_to_83(name, name83);

    uint32_t cluster = 0;
    if (isdir) {
        cluster = alloc_cluster();
        if (cluster == 0) return -1;
        fat_set_next(cluster, EOF_CLUSTER);

        uint8_t zero[512];
        for (int i = 0; i < 512; i++) zero[i] = 0;
        for (uint32_t s = 0; s < fs.sectors_per_cluster; s++) {
            disk_write_sector(cluster_to_sector(cluster) + s, zero);
        }

        uint8_t dot[32];
        for (int i = 0; i < 32; i++) dot[i] = ' ';
        dot[0] = '.';
        dot[11] = ATTR_DIRECTORY;
        *(uint16_t*)(dot + 0x14) = (cluster >> 16) & 0xFFFF;
        *(uint16_t*)(dot + 0x1A) = cluster & 0xFFFF;
        *(uint32_t*)(dot + 0x1C) = 0;
        disk_read_sector(cluster_to_sector(cluster), sector_buf);
        for (int i = 0; i < 32; i++) sector_buf[i] = dot[i];
        for (int i = 32; i < 64; i++) sector_buf[i] = ' ';
        sector_buf[32] = '.';
        sector_buf[33] = '.';
        sector_buf[32 + 11] = ATTR_DIRECTORY;
        *(uint16_t*)(sector_buf + 32 + 0x14) = (parent_cluster >> 16) & 0xFFFF;
        *(uint16_t*)(sector_buf + 32 + 0x1A) = parent_cluster & 0xFFFF;
        *(uint32_t*)(sector_buf + 32 + 0x1C) = 0;
        sector_buf[64] = 0;
        disk_write_sector(cluster_to_sector(cluster), sector_buf);
    }

    for (int i = 0; i < 11; i++) entry_out[i] = name83[i];
    entry_out[11] = isdir ? ATTR_DIRECTORY : ATTR_ARCHIVE;
    for (int i = 12; i < 32; i++) entry_out[i] = 0;
    *(uint16_t*)(entry_out + 0x14) = (cluster >> 16) & 0xFFFF;
    *(uint16_t*)(entry_out + 0x1A) = cluster & 0xFFFF;
    *(uint32_t*)(entry_out + 0x1C) = 0;
    return 0;
}

int fat32_create(const char* path) {
    if (!fs.mounted) return -1;
    if (check_write_perm(path) < 0) return -1;
    char abs[MAX_PATH];
    resolve_path(path, abs);

    const char* p = abs;
    while (*p == '/') p++;
    const char* last_slash = NULL;
    const char* q = p;
    while (*q) {
        if (*q == '/') last_slash = q;
        q++;
    }
    if (last_slash) {
        char dir_path[MAX_PATH];
        int i = 0;
        const char* r = abs;
        while (r != last_slash) dir_path[i++] = *r++;
        dir_path[i] = 0;
        uint32_t dir = path_to_cluster(dir_path, NULL);
        if (dir == 0) return -1;
        uint8_t entry[32];
        if (make_entry(last_slash + 1, 0, dir, entry) < 0) return -1;
        return dir_add_entry(dir, entry);
    } else {
        uint8_t entry[32];
        if (make_entry(p, 0, fs.root_cluster, entry) < 0) return -1;
        return dir_add_entry(fs.root_cluster, entry);
    }
}

int fat32_mkdir(const char* path) {
    if (!fs.mounted) return -1;
    if (check_write_perm(path) < 0) return -1;
    char abs[MAX_PATH];
    resolve_path(path, abs);

    const char* p = abs;
    while (*p == '/') p++;
    const char* last_slash = NULL;
    const char* q = p;
    while (*q) {
        if (*q == '/') last_slash = q;
        q++;
    }
    uint32_t dir;
    char name[32];
    if (last_slash) {
        char dir_path[MAX_PATH];
        int i = 0;
        const char* r = abs;
        while (r != last_slash) dir_path[i++] = *r++;
        dir_path[i] = 0;
        dir = path_to_cluster(dir_path, NULL);
        if (dir == 0) return -1;
        int j = 0;
        const char* t = last_slash + 1;
        while (*t && j < 31) name[j++] = *t++;
        name[j] = 0;
    } else {
        dir = fs.root_cluster;
        int j = 0;
        while (p[j] && j < 31) name[j] = p[j], j++;
        name[j] = 0;
    }
    uint8_t entry[32];
    if (make_entry(name, 1, dir, entry) < 0) return -1;
    return dir_add_entry(dir, entry);
}

int fat32_remove(const char* path) {
    if (!fs.mounted) return -1;
    if (check_write_perm(path) < 0) return -1;
    char abs[MAX_PATH];
    resolve_path(path, abs);

    const char* p = abs;
    while (*p == '/') p++;
    const char* last_slash = NULL;
    const char* q = p;
    while (*q) {
        if (*q == '/') last_slash = q;
        q++;
    }
    uint32_t dir;
    char name[32];
    if (last_slash) {
        char dir_path[MAX_PATH];
        int i = 0;
        const char* r = abs;
        while (r != last_slash) dir_path[i++] = *r++;
        dir_path[i] = 0;
        dir = path_to_cluster(dir_path, NULL);
        if (dir == 0) return -1;
        int j = 0;
        const char* t = last_slash + 1;
        while (*t && j < 31) name[j++] = *t++;
        name[j] = 0;
    } else {
        dir = fs.root_cluster;
        int j = 0;
        while (p[j] && j < 31) name[j] = p[j], j++;
        name[j] = 0;
    }

    uint8_t name83[11];
    name_to_83(name, name83);

    uint32_t cluster = dir;
    while (cluster < EOF_CLUSTER && cluster != 0) {
        for (uint32_t s = 0; s < fs.sectors_per_cluster; s++) {
            uint32_t sec = cluster_to_sector(cluster) + s;
            if (disk_read_sector(sec, sector_buf) < 0) return -1;
            for (int e = 0; e < 16; e++) {
                uint8_t* entry = sector_buf + e * 32;
                if (entry[0] == 0) return -1;
                if (entry[0] == 0xE5) continue;
                if ((entry[11] & ATTR_LFN) == ATTR_LFN) continue;
                int match = 1;
                for (int i = 0; i < 11; i++) {
                    if (entry[i] != name83[i]) { match = 0; break; }
                }
                if (match) {
                    entry[0] = 0xE5;
                    if (disk_write_sector(sec, sector_buf) < 0) return -1;
                    return 0;
                }
            }
        }
        cluster = fat_get_next(cluster);
    }
    return -1;
}

void fat32_setcwd(const char* path) {
    if (!fs.mounted || !path) return;

    char abs[MAX_PATH];
    resolve_path(path, abs);

    uint32_t cluster = path_to_cluster(abs, NULL);
    if (cluster == 0) return;

    int i = 0;
    while (abs[i] && i < MAX_PATH - 1) {
        cwd_buf[i] = abs[i];
        i++;
    }
    if (i > 1 && cwd_buf[i - 1] == '/') i--;
    cwd_buf[i] = 0;
    if (i == 0) { cwd_buf[0] = '/'; cwd_buf[1] = 0; }
}

const char* fat32_getcwd(void) {
    return cwd_buf;
}

int fat32_dup(int oldfd) {
    if (!fs.mounted) return -1;
    if (oldfd < 0 || oldfd >= MAX_FD || !fd_table[oldfd].used) return -1;
    int i;
    for (i = 0; i < MAX_FD; i++) {
        if (!fd_table[i].used) {
            fd_table[i] = fd_table[oldfd];
            return i;
        }
    }
    return -1;
}

int fat32_dup2(int oldfd, int newfd) {
    if (!fs.mounted) return -1;
    if (oldfd < 0 || oldfd >= MAX_FD || !fd_table[oldfd].used) return -1;
    if (newfd < 0 || newfd >= MAX_FD) return -1;
    if (oldfd == newfd) return newfd;
    fd_table[newfd] = fd_table[oldfd];
    return newfd;
}
