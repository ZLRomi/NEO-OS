#include "spool.h"
#include "fat32.h"
#include "serial.h"
#include "heap.h"
#include "auth.h"
#include "printk.h"
#include <stdint.h>

static print_job_t jobs[MAX_SPOOL_JOBS];
static int next_job_id = 1;
static int spool_ready = 0;

static void spool_strncpy(char* dst, const char* src, int n) {
    int i = 0;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void spool_memcpy(void* dst, const void* src, unsigned int n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
}

static int spool_itoa(int val, char* out) {
    if (val == 0) { out[0] = '0'; out[1] = 0; return 1; }
    char tmp[12];
    int n = 0;
    int neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    while (val > 0) { tmp[n++] = '0' + (val % 10); val /= 10; }
    int j = 0;
    if (neg) out[j++] = '-';
    while (n > 0) out[j++] = tmp[--n];
    out[j] = 0;
    return j;
}

static void build_spool_path(int job_id, char* out) {
    const char* prefix = "/var/spool/print_";
    int i = 0;
    while (prefix[i]) { out[i] = prefix[i]; i++; }
    i += spool_itoa(job_id, out + i);
    const char* suffix = ".dat";
    int k = 0;
    while (suffix[k]) { out[i++] = suffix[k++]; }
    out[i] = 0;
}

static void ensure_spool_dir(void) {
    if (!fat32_is_mounted()) return;
    int dh = fat32_opendir("/var");
    if (dh >= 0) {
        fat32_close(dh);
    } else {
        fat32_mkdir("/var");
    }
    dh = fat32_opendir("/var/spool");
    if (dh >= 0) {
        fat32_close(dh);
    } else {
        fat32_mkdir("/var/spool");
    }
}

void spool_init(void) {
    for (int i = 0; i < MAX_SPOOL_JOBS; i++) {
        jobs[i].job_id = 0;
        jobs[i].status = SPOOL_STATUS_DONE;
        jobs[i].name[0] = 0;
        jobs[i].owner[0] = 0;
        jobs[i].size = 0;
    }
    next_job_id = 1;
    spool_ready = 1;
    ensure_spool_dir();
    serial_write("spool: initialized\n");
}

static int find_free_slot(void) {
    for (int i = 0; i < MAX_SPOOL_JOBS; i++) {
        if (jobs[i].job_id == 0 ||
            jobs[i].status == SPOOL_STATUS_DONE ||
            jobs[i].status == SPOOL_STATUS_CANCELLED) {
            return i;
        }
    }
    return -1;
}

int spool_submit(const char* name, const void* data, uint32_t size) {
    if (!spool_ready) return -1;
    if (!fat32_is_mounted()) return -1;
    if (!data || size == 0) return -1;

    int slot = find_free_slot();
    if (slot < 0) return -1;

    int id = next_job_id++;
    char path[64];
    build_spool_path(id, path);

    fat32_remove(path);
    int fd = fat32_open(path, 1);
    if (fd < 0) return -1;

    int written = fat32_write(fd, data, (int)size);
    fat32_close(fd);
    if (written < 0) return -1;

    jobs[slot].job_id = id;
    spool_strncpy(jobs[slot].name, name ? name : "unnamed", 32);
    spool_strncpy(jobs[slot].owner, auth_get_current_username(), 32);
    jobs[slot].size = size;
    jobs[slot].status = SPOOL_STATUS_PENDING;

    printk("spool: job %d queued (%d bytes, owner=%s)\n",
           id, (int)size, jobs[slot].owner);
    return id;
}

int spool_cancel(int job_id) {
    if (!spool_ready || job_id <= 0) return -1;
    for (int i = 0; i < MAX_SPOOL_JOBS; i++) {
        if (jobs[i].job_id == job_id) {
            if (jobs[i].status != SPOOL_STATUS_PENDING) return -1;
            jobs[i].status = SPOOL_STATUS_CANCELLED;
            char path[64];
            build_spool_path(job_id, path);
            fat32_remove(path);
            printk("spool: job %d cancelled\n", job_id);
            return 0;
        }
    }
    return -1;
}

int spool_list(print_job_t* out, int max_count) {
    if (!spool_ready || !out || max_count <= 0) return 0;
    int n = 0;
    for (int i = 0; i < MAX_SPOOL_JOBS && n < max_count; i++) {
        if (jobs[i].job_id != 0) {
            spool_memcpy(&out[n], &jobs[i], sizeof(print_job_t));
            n++;
        }
    }
    return n;
}

int spool_count(void) {
    if (!spool_ready) return 0;
    int n = 0;
    for (int i = 0; i < MAX_SPOOL_JOBS; i++) {
        if (jobs[i].job_id != 0 && jobs[i].status == SPOOL_STATUS_PENDING) n++;
    }
    return n;
}

int spool_process_next(void) {
    if (!spool_ready) return 0;
    if (!fat32_is_mounted()) return 0;

    int target = -1;
    int target_id = 0x7FFFFFFF;
    for (int i = 0; i < MAX_SPOOL_JOBS; i++) {
        if (jobs[i].status == SPOOL_STATUS_PENDING &&
            jobs[i].job_id != 0 && jobs[i].job_id < target_id) {
            target_id = jobs[i].job_id;
            target = i;
        }
    }
    if (target < 0) return 0;

    jobs[target].status = SPOOL_STATUS_PRINTING;

    char path[64];
    build_spool_path(jobs[target].job_id, path);

    int fd = fat32_open(path, 0);
    if (fd < 0) {
        printk("spool: job %d data missing\n", jobs[target].job_id);
        jobs[target].status = SPOOL_STATUS_DONE;
        return jobs[target].job_id;
    }

    char* buf = (char*)kmalloc(1024);
    if (!buf) {
        fat32_close(fd);
        jobs[target].status = SPOOL_STATUS_PENDING;
        return -1;
    }

    serial_write("\n===== SPOOL PRINT START =====\n");
    serial_write("Job: ");
    serial_write(jobs[target].name);
    serial_write(" (id=");
    char num[12];
    spool_itoa(jobs[target].job_id, num);
    serial_write(num);
    serial_write(") Owner: ");
    serial_write(jobs[target].owner);
    serial_write("\n----- content -----\n");

    int n;
    while ((n = fat32_read(fd, buf, 1023)) > 0) {
        buf[n] = 0;
        serial_write(buf);
    }

    serial_write("\n----- end content -----\n");
    serial_write("===== SPOOL PRINT END =====\n");

    fat32_close(fd);
    kfree(buf);

    jobs[target].status = SPOOL_STATUS_DONE;
    printk("spool: job %d printed\n", jobs[target].job_id);
    fat32_remove(path);
    return jobs[target].job_id;
}
