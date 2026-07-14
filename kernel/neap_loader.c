#include "neap.h"
#include "fat32.h"
#include "heap.h"
#include <stdint.h>

static int neap_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void neap_memcpy(void* dst, const void* src, int n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n-- > 0) *d++ = *s++;
}

/* 检查 [off, off+size) 是否落在 [0, total) 范围内 */
static int neap_range_over(uint32_t off, uint32_t size, uint32_t total) {
    if (off > total) return 1;
    if (size > total - off) return 1;
    return 0;
}

int neap_validate(const neap_header_t* hdr) {
    if (!hdr) return -1;
    if (hdr->magic[0] != NEAP_MAGIC0 ||
        hdr->magic[1] != NEAP_MAGIC1 ||
        hdr->magic[2] != NEAP_MAGIC2 ||
        hdr->magic[3] != NEAP_MAGIC3) {
        return -1;
    }
    if (hdr->version != NEAP_VERSION) return -1;
    return 0;
}

int neap_load(const char* path, neap_header_t* out_hdr,
              void** out_code, void** out_data,
              char* manifest_buf, int manifest_cap) {
    if (!path || !out_hdr || !out_code || !out_data) return -1;

    *out_code = 0;
    *out_data = 0;
    if (manifest_buf && manifest_cap > 0) manifest_buf[0] = '\0';

    /* 第一次扫描：获取文件总长度 */
    int fd = fat32_open(path, 0);
    if (fd < 0) return -1;

    uint32_t total = 0;
    {
        uint8_t tmp[1024];
        int n;
        while ((n = fat32_read(fd, tmp, sizeof(tmp))) > 0) {
            total += (uint32_t)n;
        }
    }
    fat32_close(fd);

    if (total < sizeof(neap_header_t)) return -1;

    /* 分配临时缓冲区，第二次读取完整文件 */
    uint8_t* file = (uint8_t*)kmalloc(total);
    if (!file) return -1;

    fd = fat32_open(path, 0);
    if (fd < 0) { kfree(file); return -1; }

    uint32_t got = 0;
    while (got < total) {
        int n = fat32_read(fd, file + got, (int)(total - got));
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    fat32_close(fd);

    if (got != total) { kfree(file); return -1; }

    /* 拷贝并校验头部 */
    neap_memcpy(out_hdr, file, (int)sizeof(neap_header_t));
    if (neap_validate(out_hdr) != 0) { kfree(file); return -1; }

    neap_header_t* h = out_hdr;

    /* 段范围越界检查 */
    if (neap_range_over(h->code_off, h->code_size, total) ||
        neap_range_over(h->data_off, h->data_size, total) ||
        neap_range_over(h->manifest_off, h->manifest_size, total)) {
        kfree(file);
        return -1;
    }

    /* 代码段 */
    if (h->code_size > 0) {
        void* code = kmalloc(h->code_size);
        if (!code) { kfree(file); return -1; }
        neap_memcpy(code, file + h->code_off, (int)h->code_size);
        *out_code = code;
    }

    /* 数据段 */
    if (h->data_size > 0) {
        void* data = kmalloc(h->data_size);
        if (!data) {
            kfree(*out_code);
            *out_code = 0;
            kfree(file);
            return -1;
        }
        neap_memcpy(data, file + h->data_off, (int)h->data_size);
        *out_data = data;
    }

    /* manifest 拷贝到调用方缓冲区 */
    if (manifest_buf && manifest_cap > 0 && h->manifest_size > 0) {
        int copy = (int)h->manifest_size;
        if (copy >= manifest_cap) copy = manifest_cap - 1;
        neap_memcpy(manifest_buf, file + h->manifest_off, copy);
        manifest_buf[copy] = '\0';
    }

    kfree(file);
    return 0;
}

int neap_get_manifest_field(const char* manifest, const char* key,
                            char* out, int out_cap) {
    if (!manifest || !key || !out || out_cap <= 0) return -1;

    int key_len = neap_strlen(key);
    const char* p = manifest;

    while (*p) {
        const char* line = p;
        const char* eol = line;
        while (*eol && *eol != '\n') eol++;

        /* 行首匹配 key 且紧随 '=' */
        int matched = 0;
        if ((int)(eol - line) >= key_len) {
            int i;
            for (i = 0; i < key_len; i++) {
                if (line[i] != key[i]) break;
            }
            if (i == key_len && line[key_len] == '=') matched = 1;
        }

        if (matched) {
            const char* val = line + key_len + 1;
            int vlen = (int)(eol - val);
            if (vlen < 0) vlen = 0;
            if (vlen >= out_cap) vlen = out_cap - 1;

            int i;
            for (i = 0; i < vlen; i++) out[i] = val[i];
            out[vlen] = '\0';

            /* 去除尾部 \r / 空格 */
            while (vlen > 0 && (out[vlen - 1] == '\r' || out[vlen - 1] == ' ')) {
                vlen--;
                out[vlen] = '\0';
            }
            return vlen;
        }

        /* 跳到下一行 */
        p = eol;
        if (*p == '\n') p++;
    }

    return -1;
}
