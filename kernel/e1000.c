#include "e1000.h"
#include "pci.h"
#include "memory.h"
#include "printk.h"
#include "serial.h"
#include "idt.h"
#include <stdint.h>

#define E1000_VENDOR 0x8086
#define E1000_DEVICE 0x100E

#define E1000_CTRL     0x00000
#define E1000_STATUS   0x00008
#define E1000_EECD     0x00010
#define E1000_EERD     0x00014
#define E1000_IMASK    0x000D0
#define E1000_IMC      0x000D8
#define E1000_RCTL     0x00100
#define E1000_TCTL     0x00400
#define E1000_TIPG     0x00410
#define E1000_RDBAL    0x02800
#define E1000_RDBAH    0x02804
#define E1000_RDLEN    0x02808
#define E1000_RDH      0x02810
#define E1000_RDT      0x02818
#define E1000_TDBAL    0x03800
#define E1000_TDBAH    0x03804
#define E1000_TDLEN    0x03808
#define E1000_TDH      0x03810
#define E1000_TDT      0x03818
#define E1000_MTA      0x05200
#define E1000_RAL      0x05400
#define E1000_RAH      0x05404

#define CTRL_RST       (1u << 26)
#define CTRL_SLU       (1u << 6)

#define TCTL_EN        (1u << 1)
#define TCTL_PSP       (1u << 3)
#define TCTL_CT_SHIFT  4
#define TCTL_COLD_SHIFT 12

#define RCTL_EN        (1u << 1)
#define RCTL_SBP       (1u << 2)
#define RCTL_UPE       (1u << 3)
#define RCTL_MPE       (1u << 4)
#define RCTL_LPE       (1u << 5)
#define RCTL_BAM       (1u << 15)
#define RCTL_SECRC     (1u << 26)

#define TXCMD_EOP      (1u << 0)
#define TXCMD_IFCS     (1u << 1)
#define TXCMD_RS       (1u << 3)
#define TXSTS_DD       (1u << 0)

#define RXSTS_DD       (1u << 0)
#define RXSTS_EOP      (1u << 1)

#define TX_COUNT 8
#define RX_COUNT 8

struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

static volatile uint32_t* regs = 0;
static uint8_t mac_addr[6];

static struct e1000_tx_desc* tx_descs;
static uint32_t tx_desc_phys;
static uint8_t* tx_bufs[TX_COUNT];
static uint32_t tx_buf_phys[TX_COUNT];
static int tx_cur = 0;

static struct e1000_rx_desc* rx_descs;
static uint32_t rx_desc_phys;
static uint8_t* rx_bufs[RX_COUNT];
static uint32_t rx_buf_phys[RX_COUNT];
static int rx_cur = 0;

static uint32_t e1000_read(uint32_t reg) {
    return regs[reg / 4];
}

static void e1000_write(uint32_t reg, uint32_t val) {
    regs[reg / 4] = val;
}

static void e1000_read_mac(void) {
    uint32_t ral = e1000_read(E1000_RAL);
    uint32_t rah = e1000_read(E1000_RAH);
    mac_addr[0] = ral & 0xFF;
    mac_addr[1] = (ral >> 8) & 0xFF;
    mac_addr[2] = (ral >> 16) & 0xFF;
    mac_addr[3] = (ral >> 24) & 0xFF;
    mac_addr[4] = rah & 0xFF;
    mac_addr[5] = (rah >> 8) & 0xFF;
    e1000_write(E1000_RAH, rah | (1u << 31));
}

static void e1000_clear_mta(void) {
    for (int i = 0; i < 128; i++) {
        e1000_write(E1000_MTA + i * 4, 0);
    }
}

static int e1000_wait_reset(void) {
    for (int i = 0; i < 1000; i++) {
        if (!(e1000_read(E1000_CTRL) & CTRL_RST)) return 1;
    }
    return 0;
}

static void e1000_reset(void) {
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | CTRL_RST);
    e1000_wait_reset();
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | CTRL_SLU);
}

static void e1000_disable_interrupts(void) {
    e1000_write(E1000_IMC, 0xFFFFFFFF);
    e1000_read(E1000_STATUS);
}

static int e1000_init_tx(void) {
    tx_descs = (struct e1000_tx_desc*)dma_alloc_page(&tx_desc_phys);
    if (!tx_descs) return 0;
    for (int i = 0; i < TX_COUNT; i++) {
        tx_descs[i].addr = 0;
        tx_descs[i].length = 0;
        tx_descs[i].cmd = 0;
        tx_descs[i].status = TXSTS_DD;
        tx_bufs[i] = (uint8_t*)dma_alloc_page(&tx_buf_phys[i]);
        if (!tx_bufs[i]) return 0;
    }

    e1000_write(E1000_TDBAL, tx_desc_phys);
    e1000_write(E1000_TDBAH, 0);
    e1000_write(E1000_TDLEN, TX_COUNT * sizeof(struct e1000_tx_desc));
    e1000_write(E1000_TDH, 0);
    e1000_write(E1000_TDT, 0);

    uint32_t tctl = TCTL_EN | TCTL_PSP
                  | (0x10 << TCTL_CT_SHIFT)
                  | (0x40 << TCTL_COLD_SHIFT);
    e1000_write(E1000_TCTL, tctl);
    e1000_write(E1000_TIPG, 0x0060200A);
    return 1;
}

static int e1000_init_rx(void) {
    rx_descs = (struct e1000_rx_desc*)dma_alloc_page(&rx_desc_phys);
    if (!rx_descs) return 0;
    for (int i = 0; i < RX_COUNT; i++) {
        rx_bufs[i] = (uint8_t*)dma_alloc_page(&rx_buf_phys[i]);
        if (!rx_bufs[i]) return 0;
        rx_descs[i].addr = rx_buf_phys[i];
        rx_descs[i].length = 0;
        rx_descs[i].status = 0;
    }

    e1000_write(E1000_RDBAL, rx_desc_phys);
    e1000_write(E1000_RDBAH, 0);
    e1000_write(E1000_RDLEN, RX_COUNT * sizeof(struct e1000_rx_desc));
    e1000_write(E1000_RDH, 0);
    e1000_write(E1000_RDT, RX_COUNT - 1);

    uint32_t rctl = RCTL_EN | RCTL_BAM | RCTL_SECRC;
    e1000_write(E1000_RCTL, rctl);
    return 1;
}

int e1000_init(void) {
    pci_dev_t dev;
    if (!pci_find_device(E1000_VENDOR, E1000_DEVICE, &dev)) {
        printk("E1000: device not found\n");
        return 0;
    }

    pci_enable_bus_master(&dev);

    uint32_t bar0 = dev.bar[0] & 0xFFFFFFF0;
    if (!bar0) {
        printk("E1000: BAR0 not configured\n");
        return 0;
    }

    uint32_t mmio = mmio_map(bar0, 32);
    if (!mmio) {
        printk("E1000: failed to map MMIO\n");
        return 0;
    }
    regs = (volatile uint32_t*)mmio;

    e1000_reset();
    e1000_disable_interrupts();
    e1000_clear_mta();
    e1000_read_mac();

    printk("E1000: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac_addr[0], mac_addr[1], mac_addr[2],
           mac_addr[3], mac_addr[4], mac_addr[5]);

    if (!e1000_init_tx() || !e1000_init_rx()) {
        printk("E1000: descriptor allocation failed\n");
        return 0;
    }

    printk("E1000: initialized\n");
    return 1;
}

int e1000_send(const void* data, uint16_t len) {
    if (!regs || len > 2048) return 0;

    int i = tx_cur;
    struct e1000_tx_desc* desc = &tx_descs[i];

    int wait = 0;
    while (!(desc->status & TXSTS_DD) && wait < 1000) wait++;
    if (!(desc->status & TXSTS_DD)) return 0;

    const uint8_t* src = (const uint8_t*)data;
    uint8_t* dst = tx_bufs[i];
    for (uint16_t j = 0; j < len; j++) dst[j] = src[j];

    desc->addr = tx_buf_phys[i];
    desc->length = len;
    desc->cso = 0;
    desc->cmd = TXCMD_EOP | TXCMD_IFCS | TXCMD_RS;
    desc->status = 0;
    desc->css = 0;
    desc->special = 0;

    tx_cur = (tx_cur + 1) % TX_COUNT;
    e1000_write(E1000_TDT, tx_cur);
    return len;
}

int e1000_has_packet(void) {
    if (!regs) return 0;
    return (rx_descs[rx_cur].status & RXSTS_DD) ? 1 : 0;
}

int e1000_recv(void* buf, uint16_t max_len) {
    if (!regs || !e1000_has_packet()) return 0;

    int i = rx_cur;
    struct e1000_rx_desc* desc = &rx_descs[i];
    uint16_t len = desc->length;
    if (len > max_len) len = max_len;

    uint8_t* src = rx_bufs[i];
    uint8_t* dst = (uint8_t*)buf;
    for (uint16_t j = 0; j < len; j++) dst[j] = src[j];

    desc->status = 0;
    e1000_write(E1000_RDT, i);

    rx_cur = (rx_cur + 1) % RX_COUNT;
    return len;
}

void e1000_get_mac(uint8_t* out) {
    for (int i = 0; i < 6; i++) out[i] = mac_addr[i];
}
