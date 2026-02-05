/*! @file main.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌‌‌​‌‌‌‌​⁠⁠‌
 *  @brief main function of the kernel (called from start.s)
 *  @copyright Copyright (c) 2024-2025 University of Illinois
 */

#include "cache.h"
#include "conf.h"
#include "console.h"
#include "dev/rtc.h"
#include "dev/uart.h"
#include "dev/virtio.h"
#include "device.h"
#include "error.h"
#include "filesys.h"
// #include "heap.h"
#include "intr.h"
#include "string.h"
#include "thread.h"
#include "timer.h"
#include "elf.h"
#include "uio.h"
#include "memory.h"
#include "process.h"

#define INITEXE   "shell"  // initial user program
#define CMNTNAME  "c"
#define DEVMNTNAME "dev"
#define CDEVNAME  "vioblk"
#define CDEVINST  0

#ifndef NUART  // number of UARTs
#define NUART 2
#endif

#ifndef NVIODEV  // number of VirtIO devices
#define NVIODEV 8
#endif

static void attach_devices(void);
static void mount_cdrive(void);  // mount primary storage device ("C drive")
static void run_init(void);

void main(void)
{
    // extern char _kimg_end[];  // provided by kernel.ld

    console_init();

    intrmgr_init();
    devmgr_init();
    thrmgr_init();
    timer_init(); // added for preemption
    memory_init();
    procmgr_init();

    attach_devices();

    enable_interrupts();

    mount_cdrive();
    run_init();
}

void attach_devices(void)
{
    int i;
    int result;

    rtc_attach((void *)RTC_MMIO_BASE);

    for (i = 0; i < NUART; i++) {
        attach_uart((void *)UART_MMIO_BASE(i), UART0_INTR_SRCNO + i);
    }

    for (i = 0; i < NVIODEV; i++) {
        attach_virtio((void *)VIRTIO_MMIO_BASE(i), VIRTIO0_INTR_SRCNO + i);
    }

    result = mount_devfs(DEVMNTNAME);
    if (result != 0) {
        kprintf("mount_devfs(%s) failed: %s\n", CDEVNAME, error_name(result));
        halt_failure();
    }
}

void mount_cdrive(void)
{
    struct storage *hd;
    struct cache   *cache;
    int             result;

    hd = find_storage(CDEVNAME, CDEVINST);
    if (hd == NULL) {
        kprintf("Storage device %s%d not found\n", CDEVNAME, CDEVINST);
        halt_failure();
    }

    result = storage_open(hd);
    if (result != 0) {
        kprintf("storage_open failed on %s%d: %s\n",
                CDEVNAME, CDEVINST, error_name(result));
        halt_failure();
    }

    result = create_cache(hd, &cache);
    if (result != 0) {
        kprintf("create_cache(%s%d) failed: %s\n",
                CDEVNAME, CDEVINST, error_name(result));
        halt_failure();
    }

    result = mount_ktfs(CMNTNAME, cache);
    if (result != 0) {
        kprintf("mount_ktfs(%s, cache(%s%d)) failed: %s\n",
                CMNTNAME, CDEVNAME, CDEVINST, error_name(result));
        halt_failure();
    }
}

/*
// Old run_init variant kept for reference
void run_init(void)
{
    struct uio *initexe_uio;
    int         result;
    char       *argv[] = { INITEXE, NULL };
    int         argc   = 1;

    // 2. Open the initial executable file
    result = open_file(CMNTNAME, INITEXE, &initexe_uio);
    if (result != 0) {
        kprintf("main: Could not open %s/%s: %s; terminating\n",
                CMNTNAME, INITEXE, error_name(result));
        halt_failure();
    }

    // 3. Hand off to process manager for user-mode execution
    kprintf("main: Running %s (via process_exec)\n", INITEXE);
    result = process_exec(initexe_uio, argc, argv);

    // 4. Should never return
    kprintf("[ERROR] process_exec(" INITEXE ") returned unexpectedly! %s\n",
            error_name(result));
    halt_failure();
}
*/

void run_init(void)
{
    struct uio *initexe_uio;
    struct uio *console_uio;
    int result;
    char *argv[] = { INITEXE, NULL };
    int argc = 1;

    // Set up standard I/O file descriptors BEFORE exec
    struct process *proc = current_process();  // or thread_main_process()

    // Open the console for stdin/stdout/stderr
    result = open_file("dev", "uart0", &console_uio);
    if(result != 0){

        kprintf("main: Could not open dev/uart0 for stdio: %s\n",
                error_name(result));
        halt_failure();
    }

    // fd 0 (stdin), 1 (stdout), 2 (stderr)
    proc->uiotab[0] = console_uio;   // stdin
    uio_addref(console_uio);

    proc->uiotab[1] = console_uio;   // stdout
    uio_addref(console_uio);

    proc->uiotab[2] = console_uio;   // stderr (shares same uio)

    // Open the initial executable file
    result = open_file(CMNTNAME, INITEXE, &initexe_uio);
    if(result != 0){

        kprintf("main: Could not open %s/%s: %s; terminating\n",
                CMNTNAME, INITEXE, error_name(result));
        halt_failure();
    }

    // Hand off to process manager for user-mode execution
    kprintf("main: Running %s (via process_exec)\n", INITEXE);
    result = process_exec(initexe_uio, argc, argv);

    // Should never return
    kprintf("[ERROR] process_exec(" INITEXE ") returned unexpectedly! %s\n",
            error_name(result));
    halt_failure();
}
