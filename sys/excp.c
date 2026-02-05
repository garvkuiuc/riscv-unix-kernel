/*! @file excp.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌‌‌​‌‌‌‌​⁠⁠‌
    @brief Exception handlers
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA
*/
//
#include <stddef.h>

#include "console.h"
#include "intr.h"
#include "memory.h"
#include "misc.h"
#include "riscv.h"
#include "string.h"
#include "thread.h"
#include "trap.h"
#include "process.h"

// EXPORTED FUNCTION DECLARATIONS
//

// The following two functions, defined below, are called to handle an exception
// from trap.s.

extern void handle_smode_exception(unsigned int cause, struct trap_frame* tfr);
extern void handle_umode_exception(unsigned int cause, struct trap_frame* tfr);

// IMPORTED FUNCTION DECLARATIONS
//
/**
 * @brief Imported function definition from syscall.c that handles system calls from user mode.
 * @param tfr Pointer to the traxrame
 * @return None
 */
extern void handle_syscall(struct trap_frame* tfr);  // syscall.c

// INTERNAL GLOBAL VARIABLES
//

/**
 * @brief Array of exception names indexed by their exception code
 */
static const char* const excp_names[] = {
    [RISCV_SCAUSE_INSTR_ADDR_MISALIGNED] = "Misaligned instruction address",
    [RISCV_SCAUSE_INSTR_ACCESS_FAULT] = "Instruction access fault",
    [RISCV_SCAUSE_ILLEGAL_INSTR] = "Illegal instruction",
    [RISCV_SCAUSE_BREAKPOINT] = "Breakpoint",
    [RISCV_SCAUSE_LOAD_ADDR_MISALIGNED] = "Misaligned load address",
    [RISCV_SCAUSE_LOAD_ACCESS_FAULT] = "Load access fault",
    [RISCV_SCAUSE_STORE_ADDR_MISALIGNED] = "Misaligned store address",
    [RISCV_SCAUSE_STORE_ACCESS_FAULT] = "Store access fault",
    [RISCV_SCAUSE_ECALL_FROM_UMODE] = "Environment call from U mode",
    [RISCV_SCAUSE_ECALL_FROM_SMODE] = "Environment call from S mode",
    [RISCV_SCAUSE_INSTR_PAGE_FAULT] = "Instruction page fault",
    [RISCV_SCAUSE_LOAD_PAGE_FAULT] = "Load page fault",
    [RISCV_SCAUSE_STORE_PAGE_FAULT] = "Store page fault"};

// EXPORTED FUNCTION DEFINITIONS
//

/**
 * @brief Handles exceptions from supervisor mode. Ensures that each
 * specfic cause is handled appropriately. Creates a panic.
 * @param cause Exception code
 * @param tfr Pointer to the trap frame
 * @return None
 */
void handle_smode_exception(unsigned int cause, struct trap_frame* tfr) {
  // kprintf("[trap] cause=%lx sepc=%p stval=%p\n", csrr_scause(), csrr_sepc(),
          // csrr_stval());
  const char *name = NULL;
  char msgbuf[80];

  if (0 <= cause && cause < sizeof(excp_names) / sizeof(excp_names[0]))
    name = excp_names[cause];

  if (name != NULL) {
    switch (cause) {
    case RISCV_SCAUSE_LOAD_PAGE_FAULT:
    case RISCV_SCAUSE_STORE_PAGE_FAULT:
    case RISCV_SCAUSE_INSTR_PAGE_FAULT:
    case RISCV_SCAUSE_LOAD_ADDR_MISALIGNED:
    case RISCV_SCAUSE_STORE_ADDR_MISALIGNED:
    case RISCV_SCAUSE_INSTR_ADDR_MISALIGNED:
    case RISCV_SCAUSE_LOAD_ACCESS_FAULT:
    case RISCV_SCAUSE_STORE_ACCESS_FAULT:
    case RISCV_SCAUSE_INSTR_ACCESS_FAULT:
      snprintf(msgbuf, sizeof(msgbuf), "%s at %p for %p in S mode", name,
               (void *)tfr->sepc, (void *)csrr_stval());
      break;
    default:
      snprintf(msgbuf, sizeof(msgbuf), "%s at %p in S mode", name,
               (void *)tfr->sepc);
    }
    } else {
        snprintf(msgbuf, sizeof(msgbuf), "Exception %d at %p in S mode", cause, (void*)tfr->sepc);
    }

    panic(msgbuf);
}

/**
 * @brief Handles exceptions from user mode to ensure proper system functionality. The handler
 * redirects certain exceptions to support lazy allocation and system calls from user mode.
 * Otherwise, the handler exits the current process after providing information on where the
 * exception occurred.
 * @param cause Exception code
 * @param tfr Trap frame pointer
 * @return None
 */
void handle_umode_exception(unsigned int cause, struct trap_frame* tfr) {
    //kprintf("[UMODE] cause=%u sepc=%p a0=%d a7=%d\n", cause, tfr->sepc, tfr->a0, tfr->a7);
    uintptr_t bad_vaddr = csrr_stval();
    if (cause == RISCV_SCAUSE_ECALL_FROM_UMODE) {
        handle_syscall(tfr); // hand off to syscall path
        return; // done
    }

    int x = (cause == RISCV_SCAUSE_INSTR_PAGE_FAULT) || (cause == RISCV_SCAUSE_LOAD_PAGE_FAULT)  || (cause == RISCV_SCAUSE_STORE_PAGE_FAULT); // page-fault?

    if(x) { 
        int y = handle_umode_page_fault(tfr, bad_vaddr); // attempt resolve
        if(y == 0) return; 
    }

    const char *name = NULL; // choose a label for the cause
    char buf[80]; // message buffer

    unsigned int max_name_limit = (unsigned int)(sizeof(excp_names) / sizeof(excp_names[0])); // bounds check
    if(cause < max_name_limit) name = excp_names[cause]; // lookup label

    int addy = 0; // decide whether to print bad_vaddr
    if(x){
        addy = 1; // PFs show VA
    } 
    else if(cause == RISCV_SCAUSE_LOAD_ADDR_MISALIGNED  || cause == RISCV_SCAUSE_STORE_ADDR_MISALIGNED || cause == RISCV_SCAUSE_INSTR_ADDR_MISALIGNED ||
               cause == RISCV_SCAUSE_LOAD_ACCESS_FAULT ||
               cause == RISCV_SCAUSE_STORE_ACCESS_FAULT ||
               cause == RISCV_SCAUSE_INSTR_ACCESS_FAULT) {
        addy = 1; // address faults show VA
    }

    if(name) { // named exception
        if(addy){
            snprintf(buf, sizeof(buf), "%s at %p for %p in U mode", name, (void*)tfr->sepc, (void*)bad_vaddr); // name + PC + VA
        } 
        else{
            snprintf(buf, sizeof(buf), "%s at %p in U mode", name, (void*)tfr->sepc); // name + PC
        }
    } 
    else{ // unknown exception
        if(addy){
            snprintf(buf, sizeof(buf), "Exception %u at %p for %p in U mode",
                     cause, (void*)tfr->sepc, (void*)bad_vaddr); // code + PC + VA
        } 
        else{
            snprintf(buf, sizeof(buf), "Exception %u at %p in U mode", cause, (void*)tfr->sepc); // code + PC
        }
    }

    kprintf("%s\n", buf); // log and exit
    process_exit(); // kill the process
}
