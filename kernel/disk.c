#include "disk.h"
#include "io.h"
#include "serial.h"
#include "printk.h"

static uint16_t disk_base = 0;
static uint32_t disk_sectors = 0;

static int ata_wait_bsy(uint16_t base) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(base + 7) & ATA_STAT_BSY)) return 0;
    }
    return -1;
}

static void ata_delay(uint16_t base) {
    for (int i = 0; i < 4; i++) {
        inb(base + 7);
    }
}

static int ata_wait_drq(uint16_t base) {
    ata_delay(base);
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(base + 7);
        if (s & ATA_STAT_ERR) return -1;
        if (s & ATA_STAT_DRQ) return 0;
    }
    return -1;
}

static int ata_identify(uint16_t base, uint16_t* id) {
    outb(base + 6, 0xA0);
    outb(base + 2, 0);
    outb(base + 3, 0);
    outb(base + 4, 0);
    outb(base + 5, 0);
    outb(base + 7, 0xEC);

    uint8_t s = inb(base + 7);
    if (s == 0xFF || s == 0x00) return -1;

    if (ata_wait_bsy(base) < 0) return -1;

    s = inb(base + 7);
    if (s & ATA_STAT_ERR) return -1;
    if ((s & (ATA_STAT_DRQ | ATA_STAT_BSY)) == 0) return -1;

    if (ata_wait_drq(base) < 0) return -1;

    for (int i = 0; i < 256; i++) {
        id[i] = inw(base);
    }
    return 0;
}

void disk_init(void) {
    uint16_t id[256];
    uint16_t bases[2] = {IDE_PRIMARY, IDE_SECONDARY};

    for (int b = 0; b < 2; b++) {
        uint16_t base = bases[b];
        if (ata_identify(base, id) == 0) {
            disk_base = base;
            disk_sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
            if (disk_sectors == 0) {
                disk_sectors = (uint32_t)id[100] | ((uint32_t)id[101] << 16);
            }
            printk("disk: ATA %s master, sectors=%d\n",
                   base == IDE_PRIMARY ? "primary" : "secondary",
                   disk_sectors);
            return;
        }
    }

    disk_base = 0;
    disk_sectors = 0;
    serial_write("disk: no ATA device detected\n");
}

uint32_t disk_get_sectors(void) {
    return disk_sectors;
}

uint16_t disk_get_base(void) {
    return disk_base;
}

int disk_read_sector(uint32_t lba, uint8_t* buf) {
    if (!disk_base) return -1;
    uint16_t base = disk_base;

    if (ata_wait_bsy(base) < 0) return -1;

    outb(base + 6, 0xE0 | ((lba >> 24) & 0x0F));
    outb(base + 2, 1);
    outb(base + 3, lba & 0xFF);
    outb(base + 4, (lba >> 8) & 0xFF);
    outb(base + 5, (lba >> 16) & 0xFF);
    outb(base + 7, 0x20);

    if (ata_wait_drq(base) < 0) return -1;

    for (int i = 0; i < 256; i++) {
        uint16_t w = inw(base);
        buf[i * 2] = w & 0xFF;
        buf[i * 2 + 1] = (w >> 8) & 0xFF;
    }
    return 0;
}

int disk_write_sector(uint32_t lba, const uint8_t* buf) {
    if (!disk_base) return -1;
    uint16_t base = disk_base;

    if (ata_wait_bsy(base) < 0) return -1;

    outb(base + 6, 0xE0 | ((lba >> 24) & 0x0F));
    outb(base + 2, 1);
    outb(base + 3, lba & 0xFF);
    outb(base + 4, (lba >> 8) & 0xFF);
    outb(base + 5, (lba >> 16) & 0xFF);
    outb(base + 7, 0x30);

    if (ata_wait_drq(base) < 0) return -1;

    for (int i = 0; i < 256; i++) {
        uint16_t w = buf[i * 2] | ((uint16_t)buf[i * 2 + 1] << 8);
        outw(base, w);
    }

    outb(base + 7, 0xE7);
    ata_wait_bsy(base);
    return 0;
}
