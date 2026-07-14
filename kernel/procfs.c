#include "procfs.h"
#include "vfs.h"
#include "process.h"
#include "memory.h"
#include "printk.h"
#include <stdint.h>

#define PROCFS_MAX_FD 8
#define PROCFS_BUF 256

enum {
    PROC_KIND_MEMINFO = 0,
    PROC_KIND_CPUINFO = 1,
    PROC_KIND_STATUS = 2,
    PROC_KIND_MEM = 3,
};

typedef struct {
    int in_use;
    int kind;
    int pid;
    int offset;
} procfs_fd_t;

static procfs_fd_t proc_fd_table[PROCFS_MAX_FD];

static int p_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int p_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int p_strncmp(const char* a, const char* b, int n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (int)(unsigned char)*a - (int)(unsigned char)*b : 0;
}

static void p_memcpy(void* dst, const void* src, int n) {
    char* d = (char*)dst;
    const char* s = (const char*)src;
    while (n--) *d++ = *s++;
}

static int p_atoi(const char* s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

static int is_all_digits(const char* s) {
    if (!*s) return 0;
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        s++;
    }
    return 1;
}

static void append_str(char* buf, int* pos, int max, const char* s) {
    while (*s && *pos < max - 1) buf[(*pos)++] = *s++;
    buf[*pos] = '\0';
}

static void append_int(char* buf, int* pos, int max, int val) {
    char tmp[16];
    int len = 0;
    unsigned int u;
    if (val < 0) {
        if (*pos < max - 1) buf[(*pos)++] = '-';
        u = (unsigned int)(-val);
    } else {
        u = (unsigned int)val;
    }
    if (u == 0) tmp[len++] = '0';
    while (u > 0 && len < 15) { tmp[len++] = '0' + (u % 10); u /= 10; }
    while (len > 0 && *pos < max - 1) buf[(*pos)++] = tmp[--len];
    buf[*pos] = '\0';
}

static void cpuid_call(uint32_t leaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile("cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf));
}

static void cpu_get_info(char* vendor, uint32_t* family, uint32_t* model) {
    uint32_t a, b, c, d;
    cpuid_call(0, &a, &b, &c, &d);
    uint32_t regs[3];
    regs[0] = b; regs[1] = d; regs[2] = c;
    int i, j;
    for (i = 0; i < 3; i++) {
        uint32_t r = regs[i];
        for (j = 0; j < 4; j++) {
            vendor[i*4 + j] = (char)(r & 0xFF);
            r >>= 8;
        }
    }
    vendor[12] = '\0';

    cpuid_call(1, &a, &b, &c, &d);
    uint32_t base_family = (a >> 8) & 0xF;
    uint32_t base_model = (a >> 4) & 0xF;
    uint32_t ext_family = (a >> 20) & 0xFF;
    uint32_t ext_model = (a >> 16) & 0xF;
    uint32_t fam, mdl;
    if (base_family == 0xF) {
        fam = base_family + ext_family;
        mdl = (ext_model << 4) | base_model;
    } else {
        fam = base_family;
        mdl = base_model;
    }
    *family = fam;
    *model = mdl;
}

static int procfs_generate(const char* subpath, char* buf, int max) {
    if (!buf || max <= 0) return -1;
    buf[0] = '\0';
    int pos = 0;
    const char* p = subpath;
    while (*p == '/') p++;
    if (*p == '\0') return -1;

    if (p_strcmp(p, "meminfo") == 0) {
        uint32_t total = pmm_get_total_pages();
        uint32_t freep = pmm_get_free_pages();
        uint32_t used = (total > freep) ? total - freep : 0;
        append_str(buf, &pos, max, "total:");
        append_int(buf, &pos, max, (int)(total * 4));
        append_str(buf, &pos, max, "\nused:");
        append_int(buf, &pos, max, (int)(used * 4));
        append_str(buf, &pos, max, "\nfree:");
        append_int(buf, &pos, max, (int)(freep * 4));
        append_str(buf, &pos, max, "\n");
        return pos;
    }

    if (p_strcmp(p, "cpuinfo") == 0) {
        char vendor[13];
        uint32_t family = 0, model = 0;
        cpu_get_info(vendor, &family, &model);
        append_str(buf, &pos, max, "vendor:");
        append_str(buf, &pos, max, vendor);
        append_str(buf, &pos, max, "\nfamily:");
        append_int(buf, &pos, max, (int)family);
        append_str(buf, &pos, max, "\nmodel:");
        append_int(buf, &pos, max, (int)model);
        append_str(buf, &pos, max, "\n");
        return pos;
    }

    const char* slash = p;
    while (*slash && *slash != '/') slash++;
    int comp_len = slash - p;
    char comp[16];
    if (comp_len >= 16) return -1;
    p_memcpy(comp, p, comp_len);
    comp[comp_len] = '\0';
    if (!is_all_digits(comp)) return -1;
    int pid = p_atoi(comp);
    process_t* proc = process_get_by_pid(pid);
    if (!proc) return -1;
    const char* rest = slash;
    while (*rest == '/') rest++;
    if (*rest == '\0') return -1;

    if (p_strcmp(rest, "status") == 0) {
        append_str(buf, &pos, max, "pid:");
        append_int(buf, &pos, max, proc->pid);
        append_str(buf, &pos, max, "\nname:");
        append_str(buf, &pos, max, proc->name);
        append_str(buf, &pos, max, "\nstate:");
        append_int(buf, &pos, max, proc->state);
        append_str(buf, &pos, max, "\npriority:");
        append_int(buf, &pos, max, proc->priority);
        append_str(buf, &pos, max, "\nmem:");
        append_int(buf, &pos, max, (int)proc->page_dir);
        append_str(buf, &pos, max, "\n");
        return pos;
    }
    if (p_strcmp(rest, "mem") == 0) {
        append_str(buf, &pos, max, "mem:reserved\n");
        return pos;
    }
    return -1;
}

static int emit_name(char* buf, int pos, int max, const char* name) {
    int n = p_strlen(name);
    if (pos + n + 1 > max) return 0;
    p_memcpy(buf + pos, name, n);
    buf[pos + n] = '\0';
    return n + 1;
}

static int procfs_list(const char* subpath, char* buf, int max) {
    if (!buf || max <= 0) return -1;
    int pos = 0;
    const char* p = subpath;
    while (*p == '/') p++;

    if (*p == '\0') {
        pos += emit_name(buf, pos, max, "meminfo");
        pos += emit_name(buf, pos, max, "cpuinfo");
        process_t* list[MAX_PROCESSES];
        int n = process_get_all(list, MAX_PROCESSES);
        int i;
        for (i = 0; i < n; i++) {
            if (list[i]->state == PROCESS_TERMINATED) continue;
            char pidname[12];
            int plen = 0;
            int pid = list[i]->pid;
            char rev[12];
            int rl = 0;
            if (pid == 0) rev[rl++] = '0';
            while (pid > 0) { rev[rl++] = '0' + (pid % 10); pid /= 10; }
            while (rl > 0) pidname[plen++] = rev[--rl];
            pidname[plen] = '\0';
            pos += emit_name(buf, pos, max, pidname);
        }
        return pos;
    }

    if (is_all_digits(p)) {
        pos += emit_name(buf, pos, max, "status");
        pos += emit_name(buf, pos, max, "mem");
        return pos;
    }
    return -1;
}

static int procfs_resolve(const char* subpath, int* kind, int* pid) {
    const char* p = subpath;
    while (*p == '/') p++;
    if (*p == '\0') return -1;
    if (p_strcmp(p, "meminfo") == 0) { *kind = PROC_KIND_MEMINFO; *pid = 0; return 0; }
    if (p_strcmp(p, "cpuinfo") == 0) { *kind = PROC_KIND_CPUINFO; *pid = 0; return 0; }

    const char* slash = p;
    while (*slash && *slash != '/') slash++;
    int comp_len = slash - p;
    char comp[16];
    if (comp_len >= 16) return -1;
    p_memcpy(comp, p, comp_len);
    comp[comp_len] = '\0';
    if (!is_all_digits(comp)) return -1;
    int v = p_atoi(comp);
    const char* rest = slash;
    while (*rest == '/') rest++;
    if (*rest == '\0') return -1;
    if (p_strcmp(rest, "status") == 0) { *kind = PROC_KIND_STATUS; *pid = v; return 0; }
    if (p_strcmp(rest, "mem") == 0) { *kind = PROC_KIND_MEM; *pid = v; return 0; }
    return -1;
}

static int procfs_vfs_open(const char* path, int flags) {
    (void)flags;
    int kind = 0, pid = 0;
    if (procfs_resolve(path, &kind, &pid) < 0) return -1;
    int i;
    for (i = 0; i < PROCFS_MAX_FD; i++) {
        if (!proc_fd_table[i].in_use) {
            proc_fd_table[i].in_use = 1;
            proc_fd_table[i].kind = kind;
            proc_fd_table[i].pid = pid;
            proc_fd_table[i].offset = 0;
            return i;
        }
    }
    return -1;
}

static int procfs_vfs_close(int fd) {
    if (fd < 0 || fd >= PROCFS_MAX_FD) return -1;
    proc_fd_table[fd].in_use = 0;
    return 0;
}

static int procfs_vfs_read(int fd, void* buf, int len) {
    if (fd < 0 || fd >= PROCFS_MAX_FD || !proc_fd_table[fd].in_use) return -1;
    procfs_fd_t* f = &proc_fd_table[fd];
    char subpath[32];
    int pos = 0;
    subpath[pos++] = '/';
    const char* name;
    if (f->kind == PROC_KIND_MEMINFO) {
        name = "meminfo";
    } else if (f->kind == PROC_KIND_CPUINFO) {
        name = "cpuinfo";
    } else {
        char rev[12];
        int rl = 0;
        int v = f->pid;
        if (v == 0) rev[rl++] = '0';
        while (v > 0 && rl < 11) { rev[rl++] = '0' + (v % 10); v /= 10; }
        while (rl > 0 && pos < 30) subpath[pos++] = rev[--rl];
        subpath[pos++] = '/';
        name = (f->kind == PROC_KIND_STATUS) ? "status" : "mem";
    }
    int i = 0;
    while (name[i] && pos < 31) subpath[pos++] = name[i++];
    subpath[pos] = '\0';

    char content[PROCFS_BUF];
    int total = procfs_generate(subpath, content, PROCFS_BUF);
    if (total < 0) return -1;
    if (f->offset >= total) return 0;
    int avail = total - f->offset;
    int n = (avail < len) ? avail : len;
    p_memcpy(buf, content + f->offset, n);
    f->offset += n;
    return n;
}

static int procfs_vfs_readdir(const char* path, void* buf, int len) {
    return procfs_list(path, (char*)buf, len);
}

static int procfs_vfs_stat(const char* path, void* st) {
    vfs_stat_t* s = (vfs_stat_t*)st;
    if (!s) return -1;
    const char* p = path;
    while (*p == '/') p++;
    if (*p == '\0') { s->size = 0; s->is_dir = 1; return 0; }
    const char* slash = p;
    while (*slash && *slash != '/') slash++;
    if (*slash == '\0') {
        if (p_strcmp(p, "meminfo") == 0 || p_strcmp(p, "cpuinfo") == 0) {
            s->size = 0; s->is_dir = 0; return 0;
        }
        s->size = 0; s->is_dir = 1; return 0;
    }
    s->size = 0; s->is_dir = 0; return 0;
}

static const vfs_fs_t procfs_fs = {
    "procfs",
    procfs_vfs_open, procfs_vfs_close, procfs_vfs_read, 0,
    procfs_vfs_readdir, procfs_vfs_stat, 0, 0
};

int procfs_read(const char* path, char* buf, int max_len) {
    if (!path || !buf || max_len <= 0) return -1;
    const char* sub;
    if (p_strncmp(path, "/proc", 5) == 0 && (path[5] == '/' || path[5] == '\0')) {
        sub = path + 5;
        if (*sub == '\0') return -1;
    } else {
        sub = path;
    }
    return procfs_generate(sub, buf, max_len);
}

int procfs_readdir(const char* path, char* buf, int max_len) {
    if (!path || !buf || max_len <= 0) return -1;
    const char* sub;
    if (p_strncmp(path, "/proc", 5) == 0 && (path[5] == '/' || path[5] == '\0')) {
        sub = path + 5;
    } else {
        sub = path;
    }
    return procfs_list(sub, buf, max_len);
}

int procfs_init(void) {
    int i;
    for (i = 0; i < PROCFS_MAX_FD; i++) proc_fd_table[i].in_use = 0;
    if (vfs_register_fs(&procfs_fs) < 0) {
        printk("procfs: register failed\n");
        return -1;
    }
    if (vfs_mount("proc", "/proc", "procfs") < 0) {
        printk("procfs: mount failed\n");
        return -1;
    }
    printk("procfs: mounted at /proc\n");
    return 0;
}
