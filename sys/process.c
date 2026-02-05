/*! @file process.c
    @brief user process
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

/*!
 * @brief Enables trace messages for process.c
 */
#include "console.h"
#include "intr.h"
#include <stdint.h>
// #include <sys/errno.h>
#ifdef PROCESS_TRACE
#define TRACE
#endif

/*!
 * @brief Enables debug messages for process.c
 */
#ifdef PROCESS_DEBUG
#define DEBUG
#endif

#include "process.h"

#include "conf.h"
#include "elf.h"
#include "error.h"
#include "filesys.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "riscv.h"
#include "string.h"
#include "thread.h"
#include "trap.h"
#include "uio.h"
#include "conf.h"

// COMPILE-TIME PARAMETERS
//

/*!
 * @brief Maximum number of processes
 */
#ifndef NPROC
#define NPROC 16
#endif

// INTERNAL FUNCTION DECLARATIONS
//

static int build_stack(void *stack, int argc, char **argv);

static void fork_func(struct condition *forked, struct trap_frame *tfr);

// INTERNAL GLOBAL VARIABLES
//

/*!
 * @brief The main user process struct
 */
static struct process main_proc;

static struct process *proctab[NPROC] = {&main_proc};

// EXPORTED GLOBAL VARIABLES
//

char procmgr_initialized = 0;

// EXPORTED FUNCTION DEFINITIONS
//

void procmgr_init(void) {
  assert(memory_initialized && heap_initialized);
  assert(!procmgr_initialized);

  main_proc.tid = running_thread();
  main_proc.mtag = active_mspace();
  thread_set_process(main_proc.tid, &main_proc);
  procmgr_initialized = 1;
}

int process_exec(struct uio *exefile, int argc, char **argv) {
  struct trap_frame tfr;
  void (*entry)(void) = NULL;
  void *stack_page;
  int i;

  kprintf("process_exec: starting\n");
  
  if (!procmgr_initialized || exefile == NULL || argc < 0)
    return -EINVAL;
  
  /* Step 1: Copy argv strings to kernel heap */
  char **kargv = kmalloc((argc + 1) * sizeof(char *));
  if (!kargv)
    return -ENOMEM;

  int from_kernel = ((uintptr_t)argv >= RAM_START_PMA);

  for (i = 0; i < argc; i++) {
    if (!from_kernel) {
      if (validate_vptr(&argv[i], sizeof(char *), PTE_U | PTE_R) != 0 ||
          validate_vstr(argv[i], PTE_U | PTE_R) != 0) {
        while (--i >= 0)
          kfree(kargv[i]);
        kfree(kargv);
        return -EINVAL;
      }
    }
      size_t len = strlen(argv[i]) + 1;
      kargv[i] = kmalloc(len);
      if (!kargv[i]) {
        while (--i >= 0)
          kfree(kargv[i]);
        kfree(kargv);
        return -ENOMEM;
      }
      memcpy(kargv[i], argv[i], len);
    }
  kargv[argc] = NULL;

  kprintf("process_exec: copied %d args to kernel\n", argc);

  /* --- STEP 2: Unmap memory space of previous processes ---  */

  reset_active_mspace(); // (a) v mem of other processes are unmapped

  kprintf("process_exec: reset memory space, loading ELF...\n");

  /* --- STEP 3: Load ELF ---  */

  int rc = elf_load(exefile, &entry); // (b) exefile loaded from uio and provided to mapped pages

  kprintf("process_exec: elf_load returned %d, entry=%p, scause=%lx\n", rc,
          entry, csrr_scause());

  uio_close(exefile); // close file

  if (rc != 0) {
    kprintf("process_exec: elf_load FAILED with %d\n", rc);
    for (i = 0; i < argc; i++)
      kfree(kargv[i]);
    kfree(kargv);
    return rc;
  }

  /* --- STEP 4: Build stack using helper function ---  */

  stack_page = alloc_phys_page();
  if (!stack_page) {
    kprintf("process_exec: stack alloc failed\n");
    for (i = 0; i < argc; i++) 
      kfree(kargv[i]);
    kfree(kargv);
    return -ENOMEM;
  }

  int stksz = build_stack(stack_page, argc, kargv);
  if (stksz < 0) {
    kprintf("process_exec: build_stack returned %d\n", stksz);
    free_phys_page(stack_page);
    for (i = 0; i < argc; i++)
      kfree(kargv[i]);
    kfree(kargv);
    return stksz;
  }
  /* --- STEP 5: Map stack page ---  */

  uintptr_t stack_vaddr = UMEM_END_VMA - PAGE_SIZE;
  if (!map_page(stack_vaddr, stack_page, PTE_R | PTE_W | PTE_U)) {
    kprintf("process_exec: map_page failed\n");
    free_phys_page(stack_page);
    for (i = 0; i < argc; i++)
      kfree(kargv[i]);
    kfree(kargv);
    return -ENOMEM;
  }

  /* --- STEP 6: Free kernel copies ---  */
  for (i = 0; i < argc; i++)
    kfree(kargv[i]);
  kfree(kargv);

  /* Step 7: Set up trap frame bits + jump to user mode */
  uintptr_t sp = stack_vaddr + PAGE_SIZE - stksz;

  long pre = disable_interrupts();

  memset(&tfr, 0, sizeof(tfr));
  tfr.sepc = (void *)entry;
  tfr.sp = (void *)sp;
  tfr.a0 = (uintptr_t)argc;
  tfr.a1 = sp;
  tfr.sstatus = RISCV_SSTATUS_SPIE;

  kprintf("process_exec: jumping to user mode, entry=%p sp=%p\n", entry,
          (void *)sp);

  /* --- STEP 6: Jump to user space ---  */
  void *sscratch = (char *)running_thread_stack_base() - sizeof(tfr); // - sizeof(stack_page);

  //kprintf("About to jump: sepc=%p sstatus=%lx\n", tfr.sepc, tfr.sstatus);
  kprintf("process_exec: sscratch=%p stack_base=%p\n", sscratch,
          running_thread_stack_base());
  kprintf("process_exec: tfr.sepc=%p tfr.sp=%p tfr.sstatus=%lx\n", tfr.sepc,
          tfr.sp, tfr.sstatus);

  restore_interrupts(pre);

  struct process *proc = current_process();
  //kprintf("process_exec: before jump - uiotab[0]=%p [1]=%p [2]=%p\n",
         //proc->uiotab[0], proc->uiotab[1], proc->uiotab[2]);
  (void) proc;

  trap_frame_jump(&tfr, sscratch);

  panic("process_exec: trap_frame_jump returned");
  return -EINVAL;
}



int process_fork(const struct trap_frame *parent_tfr) {

  if (!procmgr_initialized || parent_tfr == NULL){
    return -EINVAL;
  }

  // Find free process slot
  int pid = -1;
  for (int i = 0; i < NPROC; i++) {
    if (proctab[i] == NULL) {
      pid = i;
      break;
    }
  }
  if (pid < 0){

    return -ENOMEM;
  }

  // Allocate new process struct
  struct process *child = kmalloc(sizeof(struct process));
  if (!child){

    return -ENOMEM;
  }

  memset(child, 0, sizeof(*child));

  // Clone parent's memory space 
  mtag_t newtag = clone_active_mspace();
  if (!newtag) {
    kfree(child);
    return -ENOMEM;
  }
  child->mtag = newtag;

  // Create a condition variable so parent waits until child copies trap frame
  struct condition done;
  condition_init(&done, NULL);

  // Allocate memory to store a copy of trap frame for child to use
  struct trap_frame *kid_tfr = kmalloc(sizeof(struct trap_frame));
  if (!kid_tfr) {
    mtag_t saved = switch_mspace(newtag);
    discard_active_mspace();
    switch_mspace(saved);
    kfree(child);
    return -ENOMEM;
  }
  memcpy(kid_tfr, parent_tfr, sizeof(struct trap_frame));

  // In the child, fork must return 0
  kid_tfr->a0 = 0;

  // The child is not going through handle syscalln so manually increment sepc
  kid_tfr->sepc = (void *)((uintptr_t)kid_tfr->sepc + 4);

  // Spawn kernel thread for child
  child->tid = spawn_thread("forked_child", (void *)fork_func, (uint64_t)&done, (uint64_t)kid_tfr);

  if (child->tid < 0) {
    kfree(kid_tfr);
    mtag_t saved = switch_mspace(newtag);
    discard_active_mspace();
    switch_mspace(saved);
    kfree(child);
    return -ENOMEM;
  }


  // When you fork a child it must inherit all open file descriptors so thats what this function helps with
  // Get the kernels parent process that called _fork and we want to duplicate this fd table
  struct process *parent = running_thread_process();


  // Check to make sure that parent is no null
  if(parent != NULL){

    // Iterate through all the possible fd slots
    for (int i = 0; i < PROCESS_UIOMAX; i++) {

      // Check that the file descriptors are open
      if (parent->uiotab[i] != NULL) {

        // Give the child process the same open fd as the parent as a shallow copy
        child->uiotab[i] = parent->uiotab[i];
        
        // Incremeent the reference count
        uio_addref(child->uiotab[i]);  
      }
    }
  }
  


  // Register process struct
  proctab[pid] = child;
  thread_set_process(child->tid, child);

  //int child_pid = pid;
  int child_tid = child->tid;

  // Wait until child consumes trap frame (child signals us)
  condition_wait(&done);

  return child_tid;
}

/** \brief
 *
 *
 *  Discard memory space, close your associated uio, free the memory you're
 * supposed to free.
 *
 *
 */
void process_exit(void) {
  struct process *proc = running_thread_process(); // Note: different function

  if (proc == NULL) {
    running_thread_exit();
  }

  int tid = proc->tid;

  // Step 1: close all UIO interfaces before memory is gone
  for (int i = 0; i < PROCESS_UIOMAX; i++) {
    if (proc->uiotab[i] != NULL) {
      uio_close(proc->uiotab[i]);
      proc->uiotab[i] = NULL;
    }
  }

  // Step 2: discard memory space
  discard_active_mspace();

  // Step 3: remove from proctab and free struct (but not main_proc)
  if (proc != &main_proc) {
    for (int i = 0; i < NPROC; i++) {
      if (proctab[i] == proc) {
        proctab[i] = NULL;
        break;
      }
    }
    kfree(proc);
  }

  thread_set_process(tid, NULL);

  running_thread_exit();

  }

// INTERNAL FUNCTION DEFINITIONS
//

/**
 * \brief Builds the initial user stack for a new process.
 *
 * Builds the stack for a new process, including the argument vector (\p argv)
 * and the strings it points to. Note that \p argv must contain \p argc + 1
 * elements (the last one is a NULL pointer).
 *
 * Remember to round the final stack size up to a multiple of 16 bytes
 * (RISC-V ABI requirement).
 *
 * \param[in,out] stack  Pointer to the stack page (destination buffer).
 * \param[in]     argc   Number of arguments in \p argv.
 * \param[in]     argv   Array of argument pointers; length is \p argc+1 and
 *                       \p argv[argc] must be NULL.
 *
 * \return Size of the stack page on success; negative error code on failure.
 */
int build_stack(void *stack, int argc, char **argv) {
  size_t stksz, argsz;
  uintptr_t *newargv;
  char *p;
  int i;

  // We need to be able to fit argv[] on the initial stack page, so _argc_
  // cannot be too large. Note that argv[] contains argc+1 elements (last one
  // is a NULL pointer).

  if (PAGE_SIZE / sizeof(char *) - 1 < argc)
    return -ENOMEM;

  stksz = (argc + 1) * sizeof(char *);

  // Add the sizes of the null-terminated strings that argv[] points to.

  for (i = 0; i < argc; i++) {
    argsz = strlen(argv[i]) + 1;
    if (PAGE_SIZE - stksz < argsz)
      return -ENOMEM;
    stksz += argsz;
  }

  // Round up stksz to a multiple of 16 (RISC-V ABI requirement).

  stksz = ROUND_UP(stksz, 16);
  assert(stksz <= PAGE_SIZE);

  // Set _newargv_ to point to the location of the argument vector on the new
  // stack and set _p_ to point to the stack space after it to which we will
  // copy the strings. Note that the string pointers we write to the new
  // argument vector must point to where the user process will see the stack.
  // The user stack will be at the highest page in user memory, the address of
  // which is `(UMEM_END_VMA - PAGE_SIZE)`. The offset of the _p_ within the
  // stack is given by `p - newargv'.

  newargv = stack + PAGE_SIZE - stksz;
  p = (char *)(newargv + argc + 1);

  for (i = 0; i < argc; i++) {
    newargv[i] = (UMEM_END_VMA - PAGE_SIZE) + ((void *)p - (void *)stack);
    argsz = strlen(argv[i]) + 1;
    memcpy(p, argv[i], argsz);
    p += argsz;
  }

  newargv[argc] = 0;
  return stksz;
}

/**
 * \brief Function to be executed by the child process after fork.
 * This is a very beautiful function.
 * Tell the parent process that it is done with the trap frame, then jumps to
 * user space (hint: which function should we use?)
 *
 * \param[in] done  Pointer to a condition variable to signal parent
 * \param[in] tfr   Pointer to a trap frame
 *
 * \return NONE (very important, this is a hint)
 */
void fork_func(struct condition *done, struct trap_frame *tfr) {

  // signal parent that trap frame is copied
  condition_broadcast(done);

  //switch to child's mspace
  switch_mspace(running_thread_process()->mtag);

  void *sscratch = running_thread_stack_base() - sizeof(*tfr);
  // switch to U mode
  trap_frame_jump(tfr, sscratch);

  panic("fork_func: trap_frame_jump returned");
} 