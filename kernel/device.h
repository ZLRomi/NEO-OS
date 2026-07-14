#ifndef DEVICE_H
#define DEVICE_H
#include <stdint.h>

#define DEVICE_TYPE_CHAR  1
#define DEVICE_TYPE_BLOCK 2

#define MAX_DEVICES 32

#define DEV_NAME_LEN 16

typedef struct device {
    char name[DEV_NAME_LEN];
    int type;
    int in_use;
    int  (*open)(struct device* dev);
    int  (*close)(struct device* dev);
    int  (*read)(struct device* dev, void* buf, int count, uint32_t offset);
    int  (*write)(struct device* dev, const void* buf, int count, uint32_t offset);
    int  (*ioctl)(struct device* dev, int cmd, void* arg);
    void* priv;
} device_t;

int device_register(const char* name, int type,
                    int (*open)(device_t*),
                    int (*close)(device_t*),
                    int (*read)(device_t*, void*, int, uint32_t),
                    int (*write)(device_t*, const void*, int, uint32_t),
                    int (*ioctl)(device_t*, int, void*),
                    void* priv);

device_t* device_find(const char* name);

int dev_open(const char* name);
int dev_close(const char* name);
int dev_read(const char* name, void* buf, int count, uint32_t offset);
int dev_write(const char* name, const void* buf, int count, uint32_t offset);
int dev_ioctl(const char* name, int cmd, void* arg);

void device_init(void);

void device_list(void);

#endif
