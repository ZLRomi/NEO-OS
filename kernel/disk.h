#ifndef DISK_H
#define DISK_H

#include <stdint.h>

#define IDE_PRIMARY     0x1F0
#define IDE_SECONDARY   0x170

#define ATA_STAT_BSY    0x80
#define ATA_STAT_DRQ    0x08
#define ATA_STAT_ERR    0x01

int disk_read_sector(uint32_t lba, uint8_t* buf);
int disk_write_sector(uint32_t lba, const uint8_t* buf);
void disk_init(void);
uint32_t disk_get_sectors(void);
uint16_t disk_get_base(void);

#endif
