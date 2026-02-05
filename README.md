### **RISC-V Unix-Style Kernel (LUMON OS)**

A preemptive, multi-threaded Unix-style operating system kernel built for the RISC-V (Sv39) architecture. This project implements core OS abstractions from the hardware up, including demand-paged virtual memory, an asynchronous VirtIO storage stack, and a custom shell supporting I/O redirection and pipelines.

__Device Drivers & Hardware Abstraction Layer__


_VirtIO Block Device Driver (sys/dev/vioblk.c)_

- Developed an asynchronous storage stack to interface with virtual disks.
- Virtqueue Protocol: Managed the shared-memory "Virtqueue" transport layer. Implemented a 3-part descriptor chain (Header, Data Payload, Status) to facilitate non-blocking I/O.
- Descriptor Management: Built a custom free-list stack to manage available descriptors. Handled the technical challenge of "descriptor reclamation" within the ISR to ensure the driver can support continuous, high-throughput requests.
- Synchronization: Utilized __sync_synchronize() memory barriers to prevent CPU reordering of MMIO writes, ensuring the device sees a consistent view of the available ring before notification.

NS8250 UART Driver (sys/dev/uart.c)_
- Implemented an interrupt-driven serial driver for system console and peripheral communication.
- Interrupt Service Routine (ISR): Configured the Line Status Register (LSR) to handle both Receive Data Ready (DR) and Transmitter Holding Register Empty (THRE) interrupts.
- Concurrency: Used Condition Variables (rxbnotempty, txbnotfull) to allow threads to block efficiently while waiting for slow serial I/O, rather than busy-waiting.
- Polled Fallback: Developed a "polled" mode for early-boot diagnostics before the interrupt controller (PLIC) is initialized.



__Kernel Subsystems__

_Memory Management & Paging (sys/memory.c)_
- Implemented the Sv39 paging scheme, providing 39-bit virtual address spaces with process isolation.
- Three-Level Page Tables: Authored recursive page table walkers for mapping, unmapping, and permission adjustments (ptab_insert, ptab_remove).
- Demand Paging: Built a page fault handler (handle_umode_page_fault) that lazily maps physical frames only when a user process accesses them, significantly optimizing physical memory utilization.
- Identity Mapping: Configured early-boot identity maps for the kernel text/data and MMIO regions to prevent "Identity Crisis" crashes when enabling the MMU.

_Process Lifecycle & Multitasking (sys/process.c & sys/timer.c)_
- Context Switching: Implemented preemptive multitasking by triggering a scheduler tick every 10ms via the RISC-V stcmp timer.
- Fork & Exec: Developed a full fork() implementation that clones the parent’s address space (clone_active_mspace) and exec() which loads and parses ELF binaries from disk into memory.
- System Call Layer: Built the trap handler that transitions the CPU from User Mode to Supervisor Mode, passing arguments through the a0-a7 registers.

_Keegan Teal Filesystem (KTFS) (sys/ktfs.c)_
- A custom, robust read-only filesystem designed for high reliability and structured storage.
- Inode Addressing: Supports direct, indirect, and double-indirect block addressing, allowing the kernel to handle files spanning up to 16MB+ within the block device.
- Directory Management: Implemented directory traversal and filename-to-inode resolution, supporting nested file paths and metadata retrieval.
- Block Caching: Integrated a buffer cache layer to minimize expensive disk I/O during metadata lookups.

_User-Land Shell (usr/shell.c)_
- The interface for interacting with LUMON OS, demonstrating the kernel's ability to handle complex system calls.
- Pipeline Implementation: Supports command piping (e.g., cat /file | wc) by leveraging the _pipe and _uiodup syscalls to bridge the stdout of one process to the stdin of another.
- I/O Redirection: Parses and handles < (input) and > (output) operators by manipulating file descriptors before program execution.
- Signal & Wait: Utilizes _wait to allow the shell to block until foreground child processes terminate, maintaining clean process hierarchy.

__Technical Tech Stack__

Language: C, RISC-V Assembly (RV64GC)

Build System: Complex Makefile architecture managing inter-subsystem dependencies.

Environment: QEMU (Virt machine), GDB, RISC-V Toolchain.

__Engineering Challenge: The Fork-Pipe Race__

The most difficult technical challenge was coordinating the Process-Pipe-Redirection sequence. In run_pipeline, the shell must create a pipe, fork twice, and ensure each child correctly reassigns its standard file descriptors using uiodup before the exec call. Any failure in this sequence results in orphaned processes or "zombie" pipes. I solved this by implementing a strict FD-closing policy in the parent immediately after the fork to prevent the pipe from staying open indefinitely.
