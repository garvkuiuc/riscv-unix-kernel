// rtc.c - Goldfish RTC driver
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef RTC_TRACE
#define TRACE
#endif

#ifdef RTC_DEBUG
#define DEBUG
#endif

#include "rtc.h"
#include "conf.h"
#include "misc.h"
#include "devimpl.h"
#include "console.h"
#include "string.h"
#include "heap.h"

#include "error.h"

#include <stdint.h>

// INTERNAL TYPE DEFINITIONS
// 

struct rtc_regs {
    uint32_t time_low;  // read first, latches time_high
    uint32_t time_high; //
};

struct rtc_device {
    struct serial base; // must be first
    volatile struct rtc_regs * regs;
};

// INTERNAL FUNCTION DEFINITIONS
//

static int rtc_open(struct serial * ser);
static void rtc_close(struct serial * ser);
static int rtc_recv(struct serial * ser, void * buf, unsigned int bufsz);

static uint64_t read_real_time(volatile struct rtc_regs * regs);

// INTERNAL GLOBAL VARIABLES AND CONSTANTS
//

static const struct serial_intf rtc_serial_intf = {
    .blksz = 8,
    .open = &rtc_open,
    .close = &rtc_close,
    .recv = &rtc_recv
};

// EXPORTED FUNCTION DEFINITIONS
//

/* Function Interface:
    void rtc_attach(void * mmio_base)
    Inputs: void * mmio_base - base address of the memory-mapped RTC hardware registers (must be valid and accessible)
    Outputs: None
    Description: Allocates and initializes an rtc_device structure, associates it with
    the RTC hardware registers at mmio_base, initializes the serial interface, and registers
    the device with the system's device manager.
    Side Effects: - Allocates dynamic memory for the rtc_device structure
                  - Modifies global device registry/state by registering a new device
                  - May perform hardware initialization via serial_init()
*/

void rtc_attach(void * mmio_base) {
    struct rtc_device * rtc;
    rtc = kcalloc(1, sizeof(struct rtc_device)); // kcalloc memory for new device
    rtc->regs = mmio_base; // point the device to mmio_base to communicate through memory
    serial_init(&rtc->base, &rtc_serial_intf); // initialze the serial communication
    register_device("rtc", DEV_SERIAL, rtc); // registers the device
}

int rtc_open(struct serial * ser) {
    trace("%s()", __func__);
    return 0;
}

void rtc_close(struct serial * ser) {
    trace("%s()", __func__);
}

/* Function Interface:
    int rtc_recv(struct serial * ser, void * buf, unsigned int bufsz)
    Inputs: struct serial * ser - pointer to the serial device structure associated with the RTC
            void * buf - buffer to store received data (time value)
            unsigned int bufsz - size of the buffer in bytes
    Outputs: Returns the number of bytes written to buf: size of uint64_t if success or 0 if bufsz == 0.
    Description: Reads the current real-time clock value from the RTC device registers,
                 copies the 64-bit timestamp into the provided buffer, and returns the number of bytes transferred.
    Side Effects: - Reads hardware registers of the RTC (may change internal timing state)
                  - Writes to the provided buffer
*/

int rtc_recv(struct serial * ser, void * buf, unsigned int bufsz) {
    struct rtc_device * const rtc =
        (void*)ser - offsetof(struct rtc_device, base); // finds device struct to point to 
    uint64_t time_now;
    trace("%s(bufsz=%ld)", __func__, bufsz);
    if (bufsz == 0) { return 0; }
    time_now = read_real_time(rtc->regs); // reads time from hardware regs
    memcpy(buf, &time_now, sizeof(uint64_t));
    return sizeof(uint64_t); // returns the number of bytes after reading
}

/* Function Interface:
    uint64_t read_real_time(volatile struct rtc_regs * regs)
    Inputs: volatile struct rtc_regs * regs - pointer to the RTC hardware register block
    Outputs: Returns a 64-bit value representing the current real-time clock value
    Description: Reads the low and high parts of the hardware clock registers and combines them
                 into a single 64-bit timestamp representing the current time.
    Side Effects: - Reads hardware registers (accesses RTC MMIO region)
*/
uint64_t read_real_time(volatile struct rtc_regs * regs) {
    uint32_t lo, hi;
    lo = regs->time_low;
    hi = regs->time_high; // finds the hi and lo timestamps to stack them in return
    return ((uint64_t)hi << 32) | lo; // bitshifts hi over lo to complete real-time with accuracy
} 