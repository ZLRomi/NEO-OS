#ifndef NEAP_H
#define NEAP_H

#include <stdint.h>

#define NEAP_VERSION       1u
#define NEAP_RESERVED_SIZE 32

/* 魔数 "NEAP"，按字节顺序为 0x4E 0x45 0x41 0x50 */
#define NEAP_MAGIC0        'N'
#define NEAP_MAGIC1        'E'
#define NEAP_MAGIC2        'A'
#define NEAP_MAGIC3        'P'

typedef struct neap_header {
    uint8_t  magic[4];        /* 'N' 'E' 'A' 'P' */
    uint32_t version;         /* = 1 */
    uint32_t entry_offset;    /* 入口点，相对代码段 */
    uint32_t code_off;        /* 代码段在文件内偏移 */
    uint32_t code_size;       /* 代码段字节数 */
    uint32_t data_off;        /* 数据段在文件内偏移 */
    uint32_t data_size;       /* 数据段字节数 */
    uint32_t bss_size;        /* BSS 字节数（文件中不占空间） */
    uint32_t manifest_off;    /* manifest 在文件内偏移 */
    uint32_t manifest_size;   /* manifest 字节数 */
    uint8_t  reserved[NEAP_RESERVED_SIZE];
} neap_header_t;

/* 从 FAT32 读取 .neap，校验并分配堆内存存放代码段/数据段/manifest。
   成功返回 0，失败返回 -1。out_code/out_data 在 size 为 0 时为 NULL。 */
int neap_load(const char* path, neap_header_t* out_hdr,
              void** out_code, void** out_data,
              char* manifest_buf, int manifest_cap);

/* 校验魔数与版本，成功返回 0，失败返回 -1 */
int neap_validate(const neap_header_t* hdr);

/* 从 manifest 文本中解析指定 key=value，写入 out（含 '\0'）。
   成功返回写入长度，未找到返回 -1。 */
int neap_get_manifest_field(const char* manifest, const char* key,
                            char* out, int out_cap);

#endif
