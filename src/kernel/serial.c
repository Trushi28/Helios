/**
 * @file serial.c
 * @brief Polled COM1 UART debug output (115200 baud, 8N1).
 *
 * This is the primary debug channel for the Helios kernel.
 * It remains active throughout the kernel's lifetime.
 */

#include <helios/types.h>

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  COM1 I/O ports                                                            */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define COM1_BASE           0x3F8
#define COM1_DATA           (COM1_BASE + 0)   /* Data register (R/W)         */
#define COM1_IER            (COM1_BASE + 1)   /* Interrupt Enable Register   */
#define COM1_FCR            (COM1_BASE + 2)   /* FIFO Control Register       */
#define COM1_LCR            (COM1_BASE + 3)   /* Line Control Register       */
#define COM1_MCR            (COM1_BASE + 4)   /* Modem Control Register      */
#define COM1_LSR            (COM1_BASE + 5)   /* Line Status Register        */

/* Divisor latch registers (accessible when DLAB=1 in LCR) */
#define COM1_DLL            (COM1_BASE + 0)   /* Divisor Latch Low           */
#define COM1_DLH            (COM1_BASE + 1)   /* Divisor Latch High          */

/* LSR bits */
#define LSR_TX_EMPTY        (1 << 5)          /* Transmit Holding Register Empty */

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Forward declarations                                                      */
/* ═══════════════════════════════════════════════════════════════════════════ */

extern int vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap);

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Serial initialization                                                     */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Initialize COM1 serial port: 115200 baud, 8N1, no interrupts.
 */
void serial_init(void) {
    /* Disable all interrupts */
    outb(COM1_IER, 0x00);

    /* Enable DLAB (set baud rate divisor) */
    outb(COM1_LCR, 0x80);

    /* Set divisor to 1 (115200 baud) */
    outb(COM1_DLL, 0x01);    /* Low byte */
    outb(COM1_DLH, 0x00);    /* High byte */

    /* 8 bits, no parity, one stop bit (8N1), disable DLAB */
    outb(COM1_LCR, 0x03);

    /* Enable FIFO, clear TX/RX, 14-byte threshold */
    outb(COM1_FCR, 0xC7);

    /* Set RTS/DSR, enable aux output 2 (required for interrupts, but we poll) */
    outb(COM1_MCR, 0x0B);

    /* Disable interrupts again (we use polled I/O) */
    outb(COM1_IER, 0x00);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Serial output                                                             */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Wait for the transmit buffer to be empty, then send one byte.
 */
void serial_putc(char c) {
    /* Wait until TX holding register is empty */
    while (!(inb(COM1_LSR) & LSR_TX_EMPTY)) {
        cpu_pause();
    }
    outb(COM1_DATA, (uint8_t)c);
}

/**
 * @brief Write a null-terminated string to serial.
 */
void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') {
            serial_putc('\r');
        }
        serial_putc(*s);
        s++;
    }
}

/**
 * @brief Minimal printf to serial. Supports %s %d %u %x %lx %lu %p %%.
 */
void serial_printf(const char *fmt, ...) {
    char buf[1024];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    serial_puts(buf);
}
