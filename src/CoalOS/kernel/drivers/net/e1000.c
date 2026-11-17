/**
 * @file e1000.c
 * @brief Intel 82540EM driver: one RX and one TX descriptor ring, interrupt driven receive.
 *
 * Receive is also polled every 10 ms (proto timer), so a missing or misrouted
 * interrupt only costs latency. The stack builds outgoing frames directly in the
 * next free TX buffer (e1000_tx_buffer), so sending copies nothing. Interrupts
 * are throttled: one interrupt hands over every frame that arrived meanwhile.
 */
#define LOG_MODULE "e1000"

#include <kernel/drivers/net/e1000.h>
#include <kernel/drivers/pci.h>
#include <kernel/drivers/pic.h>
#include <kernel/cpu/idt.h>
#include <kernel/core/log.h>
#include <kernel/memory/paging.h>
#include <kernel/memory/frame.h>
#include <kernel/net/net.h>
#include <kernel/cpu/linux_abi.h>
#include <kernel/lib/string.h>

#define VENDOR_INTEL    0x8086
#define DEVICE_82540EM  0x100E

/* Registers (byte offsets into BAR0) */
#define REG_CTRL    0x0000
#define REG_ICR     0x00C0
#define REG_ITR     0x00C4
#define REG_IMS     0x00D0
#define REG_IMC     0x00D8
#define REG_RCTL    0x0100
#define REG_TCTL    0x0400
#define REG_TIPG    0x0410
#define REG_RDBAL   0x2800
#define REG_RDBAH   0x2804
#define REG_RDLEN   0x2808
#define REG_RDH     0x2810
#define REG_RDT     0x2818
#define REG_TDBAL   0x3800
#define REG_TDBAH   0x3804
#define REG_TDLEN   0x3808
#define REG_TDH     0x3810
#define REG_TDT     0x3818
#define REG_MTA     0x5200
#define REG_RAL0    0x5400
#define REG_RAH0    0x5404

#define CTRL_SLU    (1u << 6)
#define CTRL_RST    (1u << 26)
#define RCTL_EN     (1u << 1)
#define RCTL_BAM    (1u << 15)
#define RCTL_SECRC  (1u << 26)
#define TCTL_EN     (1u << 1)
#define TCTL_PSP    (1u << 3)
#define TCTL_CT     (0x0Fu << 4)
#define TCTL_COLD   (0x40u << 12)
#define INT_RXT0    (1u << 7)
#define INT_RXO     (1u << 6)
#define INT_RXDMT0  (1u << 4)
#define DESC_DD     0x01
#define DESC_EOP    0x01
#define TX_CMD      0x0B            /* EOP | IFCS | RS */

#define RX_DESCS    64
#define TX_DESCS    64
#define ITR_INTERVAL 400            /* interrupt throttle, units of 256 ns: about 100 us */
#define BUF_SIZE    2048
#define MMIO_LEN    0x20000         /* BAR0 is 128 KiB */

struct rx_desc { uint64_t addr; uint16_t len, csum; uint8_t status, errors; uint16_t special; };
struct tx_desc { uint64_t addr; uint16_t len; uint8_t cso, cmd, status, css; uint16_t special; };

static volatile uint8_t *mmio;
static bool s_polled;               /* the interrupt line is not wired: net.c polls on a timer */
static volatile struct rx_desc *rx_ring;
static volatile struct tx_desc *tx_ring;
static uint8_t *rx_buf[RX_DESCS], *tx_buf[TX_DESCS];
static unsigned rx_next, tx_tail;

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)(mmio + reg); }
static inline void wr(uint32_t reg, uint32_t v) { *(volatile uint32_t *)(mmio + reg) = v; }

bool e1000_polled(void) {
    return s_polled;
}

void e1000_poll(void) {
    bool any = false;

    while (rx_ring[rx_next].status & DESC_DD) {
        volatile struct rx_desc *d = &rx_ring[rx_next];
        if ((d->status & DESC_EOP) && !d->errors) net_rx_frame(rx_buf[rx_next], d->len);
        d->status = 0;
        rx_next = (rx_next + 1) % RX_DESCS;
        any = true;
    }
    if (any) wr(REG_RDT, (rx_next + RX_DESCS - 1) % RX_DESCS);     /* one register write for the whole batch */
}

uint8_t *e1000_tx_buffer(void) {
    return (tx_ring[tx_tail].status & DESC_DD) ? tx_buf[tx_tail] : NULL;
}

int e1000_send(const void *frame, size_t len) {
    volatile struct tx_desc *d = &tx_ring[tx_tail];

    if (!(d->status & DESC_DD) || len > BUF_SIZE) return -ENOBUFS;
    if (frame != tx_buf[tx_tail]) memcpy(tx_buf[tx_tail], frame, len);
    d->len = (uint16_t)len;
    d->cmd = TX_CMD;
    d->status = 0;
    tx_tail = (tx_tail + 1) % TX_DESCS;
    wr(REG_TDT, tx_tail);
    return 0;
}

static void e1000_irq(isr_frame_t *frame) {
    unsigned irq = (unsigned)(frame->int_no - IRQ0_VECTOR);
    if (rd(REG_ICR)) e1000_poll();          /* reading ICR acknowledges the device */
    pic_send_eoi(irq);
}

/* Returns one zeroed page in the direct map, or NULL. */
static uint8_t *dma_page(uintptr_t *phys) {
    *phys = frame_alloc_zeroed();
    return *phys ? phys_to_virt(*phys) : NULL;
}

static bool map_registers(uintptr_t bar) {
    mmio = ioremap(bar, MMIO_LEN);
    return mmio != NULL;
}

static bool alloc_rings(void) {
    uintptr_t phys, ring_phys;
    uint8_t *ring = dma_page(&ring_phys);

    if (!ring) return false;
    rx_ring = (volatile struct rx_desc *)ring;
    tx_ring = (volatile struct tx_desc *)(ring + RX_DESCS * sizeof(struct rx_desc));
    for (unsigned i = 0; i < RX_DESCS + TX_DESCS; i += 2) {      /* a page holds two buffers */
        uint8_t *page = dma_page(&phys);
        if (!page) return false;
        for (unsigned j = 0; j < 2; j++) {
            if (i < RX_DESCS) {
                rx_buf[i + j] = page + j * BUF_SIZE;
                rx_ring[i + j].addr = phys + j * BUF_SIZE;
            } else {
                unsigned t = i - RX_DESCS + j;
                tx_buf[t] = page + j * BUF_SIZE;
                tx_ring[t].addr = phys + j * BUF_SIZE;
                tx_ring[t].status = DESC_DD;            /* free */
            }
        }
    }
    wr(REG_RDBAL, ring_phys);
    wr(REG_RDBAH, 0);
    wr(REG_RDLEN, RX_DESCS * sizeof(struct rx_desc));
    wr(REG_RDH, 0);
    wr(REG_RDT, RX_DESCS - 1);
    wr(REG_TDBAL, ring_phys + RX_DESCS * sizeof(struct rx_desc));
    wr(REG_TDBAH, 0);
    wr(REG_TDLEN, TX_DESCS * sizeof(struct tx_desc));
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    return true;
}

bool e1000_init(uint8_t mac[6]) {
    pci_dev_t dev;

    if (!pci_find(VENDOR_INTEL, DEVICE_82540EM, &dev)) return false;
    pci_write32(&dev, PCI_COMMAND, pci_read32(&dev, PCI_COMMAND) | PCI_CMD_MEMORY | PCI_CMD_MASTER);
    if (!map_registers(pci_read32(&dev, PCI_BAR0) & ~0xFu)) return false;

    wr(REG_IMC, 0xFFFFFFFFu);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    for (unsigned spin = 0; (rd(REG_CTRL) & CTRL_RST) && spin < 1000000; spin++) {}
    wr(REG_IMC, 0xFFFFFFFFu);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_SLU);

    uint32_t lo = rd(REG_RAL0), hi = rd(REG_RAH0);      /* the MAC the device loaded from its EEPROM */
    for (int i = 0; i < 4; i++) mac[i] = (uint8_t)(lo >> (8 * i));
    mac[4] = (uint8_t)hi;
    mac[5] = (uint8_t)(hi >> 8);
    for (unsigned i = 0; i < 128; i++) wr(REG_MTA + 4 * i, 0);

    if (!alloc_rings()) return false;
    wr(REG_TIPG, 0x0060200A);
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
    wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    unsigned irq = pci_read32(&dev, PCI_IRQ_LINE) & 0xFF;
    s_polled = irq >= 16;
    if (!s_polled) {
        register_int_handler((int)(IRQ0_VECTOR + irq), e1000_irq, NULL);
        pic_unmask(irq);
        wr(REG_ITR, ITR_INTERVAL);
        wr(REG_IMS, INT_RXT0 | INT_RXO | INT_RXDMT0);
    }
    LOG_INFO("irq %u, %s receive", irq, s_polled ? "polled" : "interrupt");
    return true;
}
