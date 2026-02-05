// uart.c -  NS8250-compatible serial port
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef UART_TRACE
#define TRACE
#endif

#ifdef UART_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "misc.h"
#include "uart.h"
#include "devimpl.h"
#include "intr.h"
#include "heap.h"
#include "thread.h"
#include "console.h"

#include "error.h"

#include <stdint.h>

// COMPILE-TIME CONSTANT DEFINITIONS
//

#ifndef UART_RBUFSZ
#define UART_RBUFSZ 64
#endif

#ifndef UART_INTR_PRIO
#define UART_INTR_PRIO 1
#endif

#ifndef UART_DEVNAME
#define UART_DEVNAME "uart"
#endif


// INTERNAL TYPE DEFINITIONS
// 

struct uart_regs {
    union {
        char rbr; // DLAB=0 read
        char thr; // DLAB=0 write
        uint8_t dll; // DLAB=1
    };
    
    union {
        uint8_t ier; // DLAB=0
        uint8_t dlm; // DLAB=1
    };
    
    union {
        uint8_t iir; // read
        uint8_t fcr; // write
    };

    uint8_t lcr;
    uint8_t mcr;
    uint8_t lsr;
    uint8_t msr;
    uint8_t scr;
};

#define LCR_DLAB (1 << 7)
#define LSR_OE (1 << 1)
#define LSR_DR (1 << 0)
#define LSR_THRE (1 << 5)
#define IER_DRIE (1 << 0)
#define IER_THREIE (1 << 1)

// Simple fixed-size ring buffer

struct ringbuf {
    unsigned int hpos; // head of queue (from where elements are removed)
    unsigned int tpos; // tail of queue (where elements are inserted)
    char data[UART_RBUFSZ];
};

// UART device structure

struct uart_serial {
    struct serial base;
    volatile struct uart_regs * regs;
    int irqno;
    char opened;

    unsigned long rxovrcnt; ///< number of times OE was set
    
    struct condition rxbnotempty; ///< signalled when rxbuf becomes not empty
    struct condition txbnotfull;  ///< signalled when txbuf becomes not full

    struct ringbuf rxbuf;
    struct ringbuf txbuf;
};

// INTERNAL FUNCTION DEFINITIONS
//

static int uart_serial_open(struct serial * ser);
static void uart_serial_close(struct serial * ser);
static int uart_serial_recv(struct serial * ser, void * buf, unsigned int bufsz);
static int uart_serial_send(struct serial * ser, const void * buf, unsigned int bufsz);

static void uart_isr(int srcno, void * aux);

// Ring buffer (struct rbuf) functions

static void rbuf_init(struct ringbuf * rbuf);
static int rbuf_empty(const struct ringbuf * rbuf);
static int rbuf_full(const struct ringbuf * rbuf);
static void rbuf_putc(struct ringbuf * rbuf, char c);
static char rbuf_getc(struct ringbuf * rbuf);

// INTERNAL GLOBAL VARIABLES
//

static const struct serial_intf uart_serial_intf = {
    .blksz = 1,
    .open = &uart_serial_open,
    .close = &uart_serial_close,
    .recv = &uart_serial_recv,
    .send = &uart_serial_send
};

// EXPORTED FUNCTION DEFINITIONS
// 


void attach_uart(void * mmio_base, int irqno) {
    struct uart_serial * uart;

    trace("%s(%p,%d)", __func__, mmio_base, irqno);
    
    // UART0 is used for the console and should not be attached as a normal
    // device. It should already be initialized by console_init(). We still
    // register the device (to reserve the name uart0), but pass a NULL device
    // pointer, so that find_serial("uart", 0) returns NULL.

    //if (mmio_base == (void*)UART0_MMIO_BASE) {
        //register_device(UART_DEVNAME, DEV_SERIAL, NULL);
        //return;
    //}
    
    uart = kcalloc(1, sizeof(struct uart_serial));

    uart->regs = mmio_base;
    uart->irqno = irqno;
    uart->opened = 0;

    // Initialize condition variables. The ISR is registered when our interrupt
    // source is enabled in uart_serial_open().

    condition_init(&uart->rxbnotempty, "uart.rxnotempty");
    condition_init(&uart->txbnotfull, "uart.txnotfull");


    // Initialize hardware

    uart->regs->ier = 0;
    uart->regs->lcr = LCR_DLAB;
    // fence o,o ?
    uart->regs->dll = 0x01;
    uart->regs->dlm = 0x00;
    // fence o,o ?
    uart->regs->lcr = 0; // DLAB=0

    serial_init(&uart->base, &uart_serial_intf);
    register_device(UART_DEVNAME, DEV_SERIAL, uart);
}

/* Function Interface:
    int uart_serial_open(struct serial * ser)
    Inputs: struct serial * ser - pointer to the serial device structure associated with the UART
    Outputs: Returns 0 on success, or -EBUSY if the UART is already opened.
    Description: Initializes the UART device for use by resetting buffers, flushing stale data,
                 enabling interrupts, and marking the device as opened.
    Side Effects: - Modifies UART hardware registers (IER)
                  - Enables UART interrupt source
                  - Resets software receive/transmit buffers
*/

int uart_serial_open(struct serial * ser) {
    struct uart_serial * const uart =
        (void*)ser - offsetof(struct uart_serial, base);

    trace("%s()", __func__);

    if (uart->opened)
        return -EBUSY;
    
    // Reset receive and transmit buffers
    
    rbuf_init(&uart->rxbuf);
    rbuf_init(&uart->txbuf);

    // Read receive buffer register to flush any stale data in hardware buffer

    uart->regs->rbr; // forces a read because uart->regs is volatile

    // Enable interrupts when data ready (DR) status asserted

    // Enable "data ready" interrupt
    uart->regs->ier = IER_DRIE;

    // Register and enable UART interrupt handler
    enable_intr_source(uart->irqno, UART_INTR_PRIO, &uart_isr, uart);

    uart->opened = 1; // mark as opened

    return 0;
}

/* Function Interface:
    void uart_serial_close(struct serial * ser)
    Inputs: struct serial * ser - pointer to the serial device structure associated with the UART
    Outputs: None
    Description: Closes the UART device by disabling interrupts and marking it as closed.
    Side Effects: - Disables UART interrupt source
                  - Clears hardware interrupt enable register
*/
void uart_serial_close(struct serial *ser)
{
    struct uart_serial *const uart =
        (void *)ser - offsetof(struct uart_serial, base);

    trace("%s()", __func__);

    if (!uart->opened)
        return; // already closed, nothing to do

    // Disable all UART interrupts
    uart->regs->ier = 0;

    // Unregister/disable UART interrupt handler
    disable_intr_source(uart->irqno);

    uart->opened = 0; // mark as closed
}

/* Function Interface:
    int uart_serial_recv(struct serial * ser, void * buf, unsigned int bufsz)
    Inputs: struct serial * ser - pointer to the UART device structure
            void * buf - buffer to store received characters
            unsigned int bufsz - maximum number of bytes to read
    Outputs: Returns the number of bytes read into buf, or -EINVAL if UART is not opened.
    Description: Retrieves data from the UART receive buffer into the provided buffer.
    Side Effects: - Reads from software-managed receive buffer
*/
int uart_serial_recv(struct serial *ser, void *buf, unsigned int bufsz)
{
    struct uart_serial *const uart =
        (void *)ser - offsetof(struct uart_serial, base);

    if (!uart->opened)
    {
        return -EINVAL; // UART not opened
    }
    if (bufsz == 0)
    {
        return 0; // nothing to read
    }

    char *bit = buf;
    unsigned int n = 0;

    // Spin until there is data available in RX buffer
    while (rbuf_empty(&uart->rxbuf)) {
        condition_wait(&uart->rxbnotempty);
    }

    // Read as much as possible, up to bufsz
    while ((n < bufsz) && !(rbuf_empty(&uart->rxbuf)))
    {
        bit[n++] = rbuf_getc(&uart->rxbuf); // put byte in buf
    }

    return n; // number of bytes read
}

/* Function Interface:
    int uart_serial_send(struct serial * ser, const void * buf, unsigned int bufsz)
    Inputs: struct serial * ser - pointer to the UART device structure
            const void * buf - buffer containing data to send
            unsigned int bufsz - number of bytes to send
    Outputs: Returns bufsz on success, or -EINVAL if UART is not opened.
    Description: Writes data into the UART transmit buffer for sending.
    Side Effects: - Writes characters into software-managed transmit buffer
                  - May trigger UART transmit interrupts
*/

int uart_serial_send(struct serial *ser, const void *buf, unsigned int bufsz)
{
    struct uart_serial *const uart =
        (void *)ser - offsetof(struct uart_serial, base);

    if (!uart->opened)
        return -EINVAL;
    if (bufsz == 0)
        return 0;

    char *in = (char *)buf;
    unsigned int n = 0;

    // FIX: Enable TX interrupt BEFORE filling buffer
    // This prevents race condition when buffer fills up
    uart->regs->ier |= IER_THREIE;
    
    // Early kickstart: if THR is empty and buffer has data, start transmission
    uint8_t lsr = uart->regs->lsr;

    // Push characters into TX buffer, waiting if full
    while (n < bufsz) {
        while (rbuf_full(&uart->txbuf)) {
            condition_wait(&uart->txbnotfull);
        }
        rbuf_putc(&uart->txbuf, in[n]);
        n++;
        uart->regs->ier |= IER_THREIE;  // Always enable
    }

     if ((lsr & LSR_THRE) && !rbuf_empty(&uart->txbuf)) { // send char loop
        char c = rbuf_getc(&uart->txbuf);
        uart->regs->thr = c;
    }

    return (int)bufsz;
}

/* Function Interface:
    void uart_isr(int srcno, void * aux)
    Inputs: int srcno - interrupt source number
            void * aux - pointer to UART device (struct uart_serial *)
    Outputs: None
    Description: Interrupt service routine for UART. Handles both receive and transmit events
                 by moving data between hardware registers and software buffers.
    Side Effects: - Reads/writes UART hardware registers
                  - Modifies software-managed RX/TX buffers
                  - Enables/disables UART interrupts based on buffer state
*/
void uart_isr(int srcno, void *aux)
{
    struct uart_serial *const uart = aux;

    // uint8_t iir = uart->regs->iir;
    uint8_t lsr = uart->regs->lsr; // line status register snapshot

    while (lsr & LSR_DR) {
        if (!rbuf_full(&uart->rxbuf)) { // check buf full
            char c = uart->regs->rbr;
            rbuf_putc(&uart->rxbuf, c);
        } else {
            // Buffer full - read and discard to clear interrupt
            uart->regs->rbr;
            break;
        }
        lsr = uart->regs->lsr;
    }
    // Handle received data
    
    //CP3: new conditions 
    if (!rbuf_empty(&uart->rxbuf)) {
        condition_broadcast(&uart->rxbnotempty);
    }

   while ((lsr & LSR_THRE) && !rbuf_empty(&uart->txbuf)) { // loop for THRE bit
        char c = rbuf_getc(&uart->txbuf);
        uart->regs->thr = c; // send char
        lsr = uart->regs->lsr;
    }
   
    // CP3: new conditions
    if (!rbuf_full(&uart->txbuf)) {
        condition_broadcast(&uart->txbnotfull);
    }
    
    //  If TX buffer is empty, disable TX interrupt
    if (rbuf_empty(&uart->txbuf)) {
        uart->regs->ier &= ~IER_THREIE;
    }
}

void rbuf_init(struct ringbuf * rbuf) {
    rbuf->hpos = 0;
    rbuf->tpos = 0;
}



int rbuf_empty(const struct ringbuf * rbuf) {
    return (rbuf->hpos == rbuf->tpos);
}


int rbuf_full(const struct ringbuf * rbuf) {
    return (rbuf->tpos - rbuf->hpos == UART_RBUFSZ);
}


void rbuf_putc(struct ringbuf * rbuf, char c) {
    uint_fast16_t tpos;

    tpos = rbuf->tpos;
    rbuf->data[tpos % UART_RBUFSZ] = c;
    asm volatile ("" ::: "memory");
    rbuf->tpos = tpos + 1;
}

char rbuf_getc(struct ringbuf * rbuf) {
    uint_fast16_t hpos;
    char c;

    hpos = rbuf->hpos;
    c = rbuf->data[hpos % UART_RBUFSZ];
    asm volatile ("" ::: "memory");
    rbuf->hpos = hpos + 1;
    return c;
}

// The functions below provide polled uart input and output for the console.

#define UART0 (*(volatile struct uart_regs*)UART0_MMIO_BASE)

void console_device_init(void) {
    UART0.ier = 0x00;

    // Configure UART0. We set the baud rate divisor to 1, the lowest value,
    // for the fastest baud rate. In a physical system, the actual baud rate
    // depends on the attached oscillator frequency. In a virtualized system,
    // it doesn't matter.
    
    UART0.lcr = LCR_DLAB;
    UART0.dll = 0x01;
    UART0.dlm = 0x00;

    // The com0_putc and com0_getc functions assume DLAB=0.

    UART0.lcr = 0;
}

void console_device_putc(char c) {
    // Spin until THR is empty
    while (!(UART0.lsr & LSR_THRE))
        continue;

    UART0.thr = c;
}

char console_device_getc(void) {
    // Spin until RBR contains a byte
    while (!(UART0.lsr & LSR_DR))
        continue;
    
    return UART0.rbr;
}
