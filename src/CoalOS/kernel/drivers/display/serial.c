/**
 * @file serial.c
 * @brief 16550 UART driver (see serial.h).
 */
#include <kernel/drivers/display/serial.h>
#include <kernel/drivers/display/terminal.h>
#include <kernel/drivers/pic.h>
#include <kernel/drivers/wait.h>
#include <kernel/cpu/idt.h>
#include <kernel/lib/port_io.h>
#include <kernel/sync/spinlock.h>

#define UART_THR 0 /* data (DLAB=0) / divisor low (DLAB=1) */
#define UART_IER 1
#define UART_FCR 2
#define UART_IIR 2
#define UART_LCR 3
#define UART_MCR 4
#define UART_LSR 5
#define UART_MSR 6
#define UART_SCR 7

#define IER_RX     0x01
#define IER_TX     0x02
#define IER_STATUS 0x04
#define LSR_DR     0x01
#define LSR_THRE   0x20
#define IIR_NONE   0x01
#define IIR_ID     0x0E
#define IIR_THRE   0x02
#define IIR_RX     0x04
#define IIR_RX_TMO 0x0C
#define IIR_LINE   0x06

#define UART_FIFO_DEPTH 16
#define RING_SIZE       2048 /* TX ring, power of two */
#define RX_RING_SIZE    8192 /* RX ring: a task may be busy for a while between reads */

typedef struct {
    uint16_t base;
    uint8_t  irq;
    bool     present;
    bool     irq_mode;
    bool     rx_stalled;        /* ring full: bytes wait in the UART, which holds the sender back */
    uint8_t  rx[RX_RING_SIZE];
    uint8_t  tx[RING_SIZE];
    volatile uint32_t rx_head, rx_tail; /* free-running indices */
    volatile uint32_t tx_head, tx_tail;
    drv_waitq_t rx_wait;
} uart_t;

static uart_t g_uart[2] = {
    [SERIAL_COM1] = { .base = 0x3F8, .irq = 4 },
    [SERIAL_COM2] = { .base = 0x2F8, .irq = 3 },
};

static inline uart_t *uart_get(int port) {
    return (port == SERIAL_COM1 || port == SERIAL_COM2) ? &g_uart[port] : NULL;
}

static inline uint32_t ring_count(uint32_t head, uint32_t tail) { return head - tail; }

void serial_init(int port) {
    uart_t *u = uart_get(port);
    if (!u) return;

    outb(u->base + UART_SCR, 0xAA);
    u->present = inb(u->base + UART_SCR) == 0xAA;
    if (!u->present) return;

    outb(u->base + UART_IER, 0x00);
    outb(u->base + UART_LCR, 0x80);            /* DLAB on */
    outb(u->base + UART_THR, 1);               /* divisor 1 = 115200 baud */
    outb(u->base + 1,        0);
    outb(u->base + UART_LCR, 0x03);            /* 8N1, DLAB off */
    outb(u->base + UART_FCR, 0xC7);            /* FIFO on, clear, 14-byte trigger */
    outb(u->base + UART_MCR, 0x0B);            /* DTR, RTS, OUT2 (gates the IRQ line) */
    (void)inb(u->base + UART_LSR);
    (void)inb(u->base + UART_MSR);
    (void)inb(u->base + UART_THR);
}

/* ---- TX/RX rings; callers run with IRQs off (single CPU) -------------- */

/** Moves queued bytes into an empty TX FIFO. Switches the THRE interrupt on/off. */
static void tx_pump(uart_t *u) {
    if (inb(u->base + UART_LSR) & LSR_THRE) {
        for (int i = 0; i < UART_FIFO_DEPTH && u->tx_head != u->tx_tail; i++) {
            outb(u->base + UART_THR, u->tx[u->tx_tail++ & (RING_SIZE - 1)]);
        }
    }
    if (u->irq_mode) {
        uint8_t ier = (u->rx_stalled ? 0 : IER_RX) | IER_STATUS | (u->tx_head != u->tx_tail ? IER_TX : 0);
        outb(u->base + UART_IER, ier);
    }
}

/* Moves received bytes into the ring. A full ring stops the draining and the RX interrupt (see
 * serial_port_read), so a fast sender is slowed down by the UART's own flow control instead of losing data. */
static void rx_drain(uart_t *u) {
    while (inb(u->base + UART_LSR) & LSR_DR) {
        if (ring_count(u->rx_head, u->rx_tail) == RX_RING_SIZE) {
            u->rx_stalled = true;
            return;
        }
        u->rx[u->rx_head++ & (RX_RING_SIZE - 1)] = inb(u->base + UART_THR);
    }
}

static void uart_irq(isr_frame_t *frame) {
    uart_t *u = (frame->int_no == IRQ4_VECTOR) ? &g_uart[SERIAL_COM1] : &g_uart[SERIAL_COM2];
    pic_send_eoi(u->irq);

    while (!(inb(u->base + UART_IIR) & IIR_NONE)) {
        rx_drain(u);
        (void)inb(u->base + UART_LSR);
        tx_pump(u);
    }
    drv_wake_all(&u->rx_wait);
}

static void uart_start_irq(uart_t *u) {
    uintptr_t flags = local_irq_save();
    bool first = !u->irq_mode;
    u->irq_mode = true;
    if (first) {
        register_int_handler(IRQ0_VECTOR + u->irq, uart_irq, NULL);
        pic_unmask(u->irq);
        rx_drain(u);
        outb(u->base + UART_IER, IER_RX | IER_STATUS);
    }
    local_irq_restore(flags);
}

/* ---- Port API -------------------------------------------------------- */

size_t serial_port_write(int port, const void *buf, size_t len) {
    uart_t *u = uart_get(port);
    if (!u || !u->present || !buf) return 0;
    uart_start_irq(u);

    const uint8_t *p = buf;
    uintptr_t flags = local_irq_save();
    size_t n = 0;
    while (n < len && ring_count(u->tx_head, u->tx_tail) < RING_SIZE) {
        u->tx[u->tx_head++ & (RING_SIZE - 1)] = p[n++];
    }
    tx_pump(u);
    local_irq_restore(flags);
    return n;
}

size_t serial_port_read(int port, void *buf, size_t len) {
    uart_t *u = uart_get(port);
    if (!u || !u->present || !buf || !len) return 0;
    uart_start_irq(u);

    uint8_t *p = buf;
    uintptr_t flags = local_irq_save();
    while (u->rx_head == u->rx_tail) {
        if (drv_wait(&u->rx_wait)) {
            local_irq_restore(flags);
            return 0;                   /* interrupted: the caller sees a short read */
        }
    }
    size_t n = 0;
    while (n < len && u->rx_head != u->rx_tail) {
        p[n++] = u->rx[u->rx_tail++ & (RX_RING_SIZE - 1)];
    }
    if (u->rx_stalled) {
        u->rx_stalled = false;
        rx_drain(u);
        tx_pump(u);                         /* re-enables the RX interrupt */
    }
    local_irq_restore(flags);
    return n;
}

size_t serial_port_rx_available(int port) {
    uart_t *u = uart_get(port);
    if (!u || !u->present) return 0;
    uart_start_irq(u);                  /* poll(2) on an idle port must see its interrupts */
    return ring_count(u->rx_head, u->rx_tail);
}

/* ---- Kernel log on COM1 (synchronous) -------------------------------- */

/** Sends len bytes, waiting for the FIFO as needed, without holding IRQs off while waiting. */
static void log_send(const char *s, size_t len) {
    uart_t *u = &g_uart[SERIAL_COM1];
    size_t i = 0;
    while (i < len) {
        uintptr_t flags = local_irq_save();
        if (u->tx_head != u->tx_tail) {
            tx_pump(u); /* keep order with data queued through the port API */
        } else if (inb(u->base + UART_LSR) & LSR_THRE) {
            for (int k = 0; k < UART_FIFO_DEPTH && i < len; k++) {
                outb(u->base + UART_THR, (uint8_t)s[i++]);
            }
        }
        local_irq_restore(flags);
        if (i < len) asm volatile("pause");
    }
}

void serial_write(const char *str) {
    const char *run = str;
    for (const char *p = str; ; p++) {
        if (*p == '\n' || *p == '\0') {
            log_send(run, (size_t)(p - run));
            if (*p == '\0') return;
            log_send("\r\n", 2);
            run = p + 1;
        }
    }
}
