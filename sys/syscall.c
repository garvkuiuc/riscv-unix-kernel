/*! @file syscall.c
    @brief system call handlers
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA
*/

#ifdef SYSCALL_TRACE
#define TRACE
#endif

#ifdef SYSCALL_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "console.h"
#include "device.h"
#include "error.h"
#include "filesys.h"
#include "heap.h"
#include "intr.h"
#include "memory.h"
#include "misc.h"
#include "process.h"
#include "scnum.h"
#include "string.h"
#include "thread.h"
#include "timer.h"
#include "uio.h"

// EXPORTED FUNCTION DECLARATIONS
//

extern void handle_syscall(struct trap_frame *tfr);  // called from excp.c

// INTERNAL FUNCTION DECLARATIONS
//

static int64_t syscall(const struct trap_frame *tfr);

static int sysexit(void);
static int sysexec(int fd, int argc, char **argv);
static int sysfork(const struct trap_frame *tfr);
static int syswait(int tid);
static int sysprint(const char *msg);
static int sysusleep(unsigned long us);

static int sysfsdelete(const char *path);
static int sysfscreate(const char *path);

static int sysopen(int fd, const char *path);
static int sysclose(int fd);
static long sysread(int fd, void *buf, size_t bufsz);
static long syswrite(int fd, const void *buf, size_t len);
static int sysfcntl(int fd, int cmd, void *arg);
static int syspipe(int *wfdptr, int *rfdptr);
static int sysuiodup(int oldfd, int newfd);

// EXPORTED FUNCTION DEFINITIONS
//

/**
 * @brief Initiates syscall present in trap frame struct and stores the return address into the sepc
 * @details sepc will be used to return back to program execution after interrupt is handled and
 * sret is called
 * @param tfr pointer to trap frame struct
 * @return void
 */

void handle_syscall(struct trap_frame *tfr) {
    long ret; // syscall return value
    ret = syscall(tfr); // run the syscall
    tfr->a0 = ret; // give result back to user code
    tfr->sepc = (void *)((uintptr_t)tfr->sepc + 4); 
}

// INTERNAL FUNCTION DEFINITIONS
//

/**
 * @brief Calls specified syscall and passes arguments
 * @details Function uses register a7 to determine syscall number and arguments are passed in from
 * a0-a5 depending on the function
 * @param tfr pointer to trap frame struct
 * @return result of syscall
 */

int64_t syscall(const struct trap_frame *tfr) {
    if(tfr->a7 == SYSCALL_EXIT){
        return sysexit(); // handle process exit
    }
    if(tfr->a7 == SYSCALL_EXEC){
        return sysexec((int)tfr->a0, (int)tfr->a1, (char **)tfr->a2); // execute new program
    }

    if(tfr->a7 == SYSCALL_FORK){
        return sysfork(tfr); // create child process
    }
    if(tfr->a7 == SYSCALL_WAIT){
        return syswait((int)tfr->a0); // wait for child
    }

    if(tfr->a7 == SYSCALL_PRINT){
        return sysprint((const char *)tfr->a0); // print string to console
    }
    if(tfr->a7 == SYSCALL_USLEEP){
        return sysusleep((unsigned long)tfr->a0); // sleep for microseconds
    }

    if(tfr->a7 == SYSCALL_FSCREATE){
        return sysfscreate((const char *)tfr->a0); // create filesystem object
    }
    if(tfr->a7 == SYSCALL_FSDELETE){
        return sysfsdelete((const char *)tfr->a0); // delete filesystem object
    }

    if(tfr->a7 == SYSCALL_OPEN){
        return sysopen((int)tfr->a0, (const char *)tfr->a1); // open file/device
    }
    if(tfr->a7 == SYSCALL_CLOSE){
        return sysclose((int)tfr->a0); // close descriptor
    }
    if(tfr->a7 == SYSCALL_READ){
        return sysread((int)tfr->a0, (void *)tfr->a1, (size_t)tfr->a2); // read from descriptor
    }

    if(tfr->a7 == SYSCALL_WRITE){
        return syswrite((int)tfr->a0, (const void *)tfr->a1, (size_t)tfr->a2); // write to descriptor
    }

    if(tfr->a7 == SYSCALL_FCNTL){
        return sysfcntl((int)tfr->a0, (int)tfr->a1, (void *)tfr->a2); // control operation on descriptor
    }

    if(tfr->a7 == SYSCALL_PIPE){
        return syspipe((int *)tfr->a0, (int *)tfr->a1); // create pipe endpoints
    }

    if(tfr->a7 == SYSCALL_UIODUP){
        return sysuiodup((int)tfr->a0, (int)tfr->a1); // duplicate descriptor
    }

    return -ENOTSUP; // unknown syscall number    

}

/**
 * @brief Calls process exit
 * @return void
 */

int sysexit(void) { 
    process_exit(); // end the current process 
    return 0;
}

/**
 * @brief Executes new process given a executable and arguments
 * @details Valid fd checks, get current process struct, close fd being executed, finally calls
 * process_exec with arguments and executable io "file"
 * @param fd file descripter idx
 * @param argc number of arguments in argv
 * @param argv array of arguments for multiple args
 * @return result of process_exec, else -EBADFD on invalid file descriptors
 */

int sysexec(int fd, int argc, char **argv) { 
    struct process *p; // current process
    struct uio *x; // executable handle
    int ret; // temp for error codes

    if((unsigned)fd >= 16){
        return -EBADFD; // fd out of range
    }

    if(argc < 0){
        return -EINVAL; // invalid argument count
    }
    p = current_process(); // get current process
    x = p->uiotab[fd]; // lookup uio for fd
    if(x == NULL){
        return -EBADFD; // fd not open
    }

    if(argc > 0){
        ret = validate_vptr(argv, (argc + 1) * sizeof(char *), PTE_U | PTE_R); // check argv array
        if(ret < 0){
            return ret;
        }
        for(int i = 0; i < argc; i++){
            ret = validate_vstr(argv[i], PTE_U | PTE_R); // check each argument string
            if(ret < 0){
                return ret;
            }
        }
    }

    uio_addref(x); // keep uio alive for new program
    sysclose(fd); // close caller's fd
    return process_exec(x, argc, argv); 
}

/**
 * @brief Forks a new child process using process_fork
 * @param tfr pointer to the trap frame
 * @return result of process_fork
 */

int sysfork(const struct trap_frame *tfr) {
    return process_fork(tfr);
}

/**
 * @brief Sleeps till a specified child process completes
 * @details Calls thread_join with the thread id the process wishes to wait for
 * @param tid thread_id
 * @return result of thread_join else invalid on invalid thread id
 */

int syswait(int tid) { 
    int ret = thread_join(tid); // block until tid exits

    return ret+1-1; // return the join result
}

/**
 * @brief Prints to console via kprintf
 * @details Validates that msg string is valid via validate_vstr and pages are mapped, calls kprintf
 * on current running process
 * @param msg string msg in userspace
 * @return 0 on sucess else error from validate_vstr
 */

int sysprint(const char *msg) {
    int ret= validate_vstr(msg, PTE_U | PTE_R); // check that msg is a valid user string
    if(ret < 0){
        return ret; // return error if invalid
    }

    kprintf("%s", msg); // print the string to console

    return 0;
}

/**
 * @brief Sleeps process till specificed amount of time has passed
 * @details Creates alarm struct, inits struct with name usleep, which sets the current time via the
 * rd_time() function, taking values from the csr, makes frequency calcuation to determine us has
 * passed before waking process
 * @param us time in us for process to sleep
 * @return 0
 */

int sysusleep(unsigned long us) { 
    sleep_us(us); // sleep for given microseconds
    return 0*0; // always return 0
}

/**
 * @brief Creates a new file in the filesystem specified by the path.
 * @details Validates and parses the user provided path for mountpoint name, file name and calls
 * create_file.
 * @param path User provided path string.
 * @return 0 on success, negative error code if error on error.
 */

int sysfscreate(const char *path) {
    int ret;
    char buf[256];
    char *mnt;
    char *fname;

    ret = validate_vstr(path, PTE_U | PTE_R); // ensure path string is a valid user pointer
    if(ret < 0){
        return ret;
    }

    strncpy(buf, path, sizeof(buf)); // copy path into kernel buffer
    buf[sizeof(buf)-1] = '\0'; // guarantee NUL termination

    ret = parse_path(buf, &mnt, &fname); // split into mount point and filename
    if(ret < 0){
        return ret;
    }

    return create_file(mnt, fname); // create the file in the filesystem
}

/**
 * @brief Deletes a file in the filesystem specified by the path.
 * @details Validates and parses the user provided path for mountpoint name, file name and calls
 * delete_file.
 * @param path User provided path string.
 * @return 0 on success, negative error code if error on error.
 */

int sysfsdelete(const char *path) {
    int ret;
    char buf[256];
    char *mount;
    char *l;

    ret = validate_vstr(path, PTE_U | PTE_R); // check that path is a valid user string
    if(ret < 0){
        return ret;


    }

    strncpy(buf, path, sizeof(buf)); // copy path into kernel buffer
    buf[sizeof(buf)-1] = '\0'; // ensure NUL termination

    ret = parse_path(buf, &mount, &l); // split into mount and leaf name
    if(ret < 0){

        return ret;
    }

    ret = delete_file(mount, l); // delete the target file

    return ret; // return status to caller
}

/**
 * @brief Opens a file or device of specified fd for given process
 * @details gets current process, allocates file descriptor (if fd = -1) or uses valid file
 * descriptor given, validates and parses user provided path, calls open_file
 * @param fd file descriptor number
 * @param path User provided path string
 * @return fd number if sucessful else return error that occured -EMFILE or -EBADFD
 */

int sysopen(int fd, const char *path) {
    int ret = 0;
    char buf[256];
    char *mount = 0;
    char *name = 0;
    struct process *proc = 0;
    struct uio *handle = 0;

    ret = validate_vstr(path, PTE_U | PTE_R); // verify user path string
    if(ret < 0){
        return ret;
    }

    strncpy(buf, path, sizeof(buf)); // copy path into kernel buffer
    buf[sizeof(buf)-1] = 0; // ensure NUL termination

    ret = parse_path(buf, &mount, &name); // split into mount and name
    if(ret < 0){
        return ret;
    }

    proc = current_process(); // get current process

    if(fd >= 0){
        if(fd >= 16 || proc->uiotab[fd] != NULL){
            return -EBADFD; // invalid or already used fd
        }
    } 
    else {
        int i = 0;
        while(i < 16 && proc->uiotab[i] != NULL) { // find free fd slot
            i++;
        }
        if(i == 16) {

            return -EMFILE; // no descriptors available
        }
        fd = i; // use found free descriptor
    }

    ret = open_file(mount, name, &handle); // open the backing object
    if(ret < 0){
        return ret;
    }

    proc->uiotab[fd] = handle; 
    return fd; // return descriptor number to user

}

/**
 * @brief Closes file or device of specified fd for given process
 * @details gets current process, calls close function of the io, deallocates the file descriptor
 * @param fd file descriptor
 * @return 0 on success, error on invalid file descriptor or empty file descriptor
 */

int sysclose(int fd) {
    int ret = 0;
    struct process *proc = 0;
    struct uio *io = 0;

    proc = current_process(); // get current process

    if((unsigned)fd >= 16){
        return -EBADFD; // fd out of range
    }

    io = proc->uiotab[fd]; // lookup uio from fd

    if(io == NULL){
        return -EBADFD; // fd not in use
    }

    uio_close(io); // close underlying object
    proc->uiotab[fd] = NULL; // mark fd as free

    return ret;
}

/**
 * @brief Calls read function of file io on given buffer
 * @details get current process, valid file descriptor checks, find io struct via file descriptor,
 * validate buffer, call ioread with given buffer
 * @param fd file descriptor number
 * @param buf pointer to buffer
 * @param bufsz number of bytes to be read
 * @return number of bytes read
 */

long sysread(int fd, void *buf, size_t bufsz) {
    long ret = 0;
    struct process *proc = 0;
    struct uio *x = 0;

    if(bufsz == 0){
        return 0; // nothing to read
    }


    proc = current_process(); // get current process

    if((unsigned)fd >= 16){
        return -EBADFD; // fd out of range
    }

    x = proc->uiotab[fd]; // lookup uio for this fd

    if(!x){ 
        return -EBADFD; // fd not open
    }
    
    ret = validate_vptr(buf, bufsz, PTE_U | PTE_W); // verify user buffer is writable
    if(ret < 0){
        return ret; // return validation error
    }

    return uio_read(x, buf, (unsigned long)bufsz); // perform the actual read
}

/**
 * @brief Calls write function of file io on given buffer
 * @details get current process, valid file descriptor checks, find io struct via file descriptor,
 * validate buffer, call iowrite with given buffer
 * @param fd file descriptor number
 * @param buf pointer to buffer
 * @param len number of bytes to be written
 * @return number of bytes written
 */

long syswrite(int fd, const void *buf, size_t len) {
    long ret = 0;
    struct process *proc = 0;
    struct uio *x = 0;
    proc = current_process(); // get current process

    if(fd < 0 || fd >= 16 || (x = proc->uiotab[fd]) == NULL) { // validate fd and lookup uio
        ret = -EBADFD; // bad file descriptor
    }

    if(ret >= 0){
        if(len > 0){
            ret = validate_vptr(buf, len, PTE_U | PTE_R); // check user buffer is readable
            if(ret >= 0){
                ret = uio_write(x, buf, (unsigned long)len); // perform write to device/file
            }
        } 
        else{
            ret = 0; // zero-length write succeeds
        }
    }

    return ret; // return bytes written or error
}

/**
 * @brief Calls device input output commands for a given device instance
 * @details get current process, valid file descriptor checks, find io struct via file descriptor,
 * ensure that fcntl type exists, validate argument pointer, issue fcntl
 * @param fd file descriptor number
 * @param cmd selection of fcntl
 * @param arg pointer to arguments
 * @return number of bytes written
 */

int sysfcntl(int fd, int cmd, void *arg) {
    int ret = 0;
    struct process *p = 0;
    struct uio *x = 0;
    p = current_process(); // get current process

    if(fd < 0 || fd >= 16 || (x = p->uiotab[fd]) == NULL){ // validate fd and find uio

        ret = -EBADFD; // invalid or unused descriptor

    }


    if(ret >= 0 && arg != NULL){
        ret = validate_vptr(arg, sizeof(unsigned long long), PTE_U | PTE_R | PTE_W); // check arg buffer
    }

    if(ret >= 0){
        ret = uio_cntl(x, cmd, arg); // issue device/filesys control
    }

    return ret; // return result or error
}

/**
 * @brief Creates a pipe for the current process
 * @details The function retrieves the current process. If either the write or read descriptor
 * pointer stores a negative value, an unused descriptor is assigned. If both file descriptors are
 * unused and valid, the function connects them via create_pipe function.
 * @param wfdptr pointer to write file descriptor
 * @param rfdptr pointer to read file descriptor
 * @return 0 on success. Else, negative error code on invalid file descriptor, or if a file
 * descriptor is already in use, or if no descriptors are found available.
 */
int syspipe(int *wfdptr, int *rfdptr) {
    int ret = 0;
    struct process *p = 0;
    struct uio *wio = 0;
    struct uio *rio = 0;
    int wfd_req;
    int rfd_req;
    int wfd = -1;
    int rfd = -1;
    int i;

    ret = validate_vptr(wfdptr, sizeof(int), PTE_U | PTE_R | PTE_W); // validate write-fd pointer
    if(ret < 0){
        return ret;
    }

    ret = validate_vptr(rfdptr, sizeof(int), PTE_U | PTE_R | PTE_W); // validate read-fd pointer
    if(ret < 0){
        return ret;
    }

    p = current_process(); // get current process

    wfd_req = *wfdptr; // requested write fd
    rfd_req = *rfdptr; // requested read fd

    if(wfd_req >= 0){
        if(wfd_req >= 16 || p->uiotab[wfd_req] != NULL){
            ret = -EBADFD; // invalid or in-use write fd
            return ret;
        }
        wfd = wfd_req;
    }

    if(rfd_req >= 0){
        if(rfd_req >= 16 || p->uiotab[rfd_req] != NULL || rfd_req == wfd){
            ret = -EBADFD; // invalid, in-use, or same as write fd
            return ret;
        }
        rfd = rfd_req;
    }

    if(wfd < 0 || rfd < 0){
        for(i = 0; i < 16; i++){
            if(p->uiotab[i] != NULL){
                continue;
            }

            if(wfd < 0){
                if(rfd >= 0 && i == rfd){
                    continue;
                }
                wfd = i; // choose free write fd
                continue;
            }

            if(rfd < 0){
                if(wfd >= 0 && i == wfd){
                    continue;
                }
                rfd = i; // choose free read fd
                continue;
            }
        }
    }

    if(wfd < 0 || rfd < 0){
        ret = -EMFILE; // no free descriptors
        return ret;
    }

    create_pipe(&wio, &rio); // create connected pipe endpoints

    if(wio == NULL || rio == NULL){
        if(wio != NULL){
            uio_close(wio); // cleanup partial allocation
        }
        if(rio != NULL){
            uio_close(rio);
        }
        ret = -ENOMEM; // out of memory
        return ret;
    }

    p->uiotab[wfd] = wio; // install write end
    p->uiotab[rfd] = rio; // install read end

    *wfdptr = wfd; // return write fd to user
    *rfdptr = rfd; // return read fd to user

    return 0; // success
}


/**
 * @brief Duplicates a file description
 * @details Allocates a new file descriptor that refers to the same open _uio_ as the descriptor
 * _oldfd_. Increments the _refcnt_ if successful.
 * @param oldfd old file descriptor number
 * @param newfd new file descriptor number
 * @return fd number if sucessful else return error on invalid file descriptor or empty file
 * descriptor
 */

int sysuiodup(int oldfd, int newfd){
    int ret = 0;
    struct process *p = 0;
    struct uio *x = 0;

    p = current_process(); // get current process

    if(oldfd < 0 || oldfd >= 16) {
        ret = -EBADFD; // invalid source fd
    }

    if(ret >= 0) {
        x = p->uiotab[oldfd];
        if(x == NULL) {
            ret = -EBADFD; // source fd not open
        }
    }

    if(ret >= 0) {
        if(newfd >= 0){
            if(newfd >= 16 || p->uiotab[newfd] != NULL){
                ret = -EBADFD; // invalid or busy target fd
            }
        } 
        else{
            int j = 0;
            while(j < 16 && p->uiotab[j] != NULL) {
                j++; // search for free fd
            }
            if(j == 16){
                ret = -EMFILE; // no descriptors available

            } 
            else{
                
                newfd = j; // use free fd
            }
        }
    }

    if(ret >= 0) {
        ret = newfd; // return new descriptor number
        uio_addref(x); // bump reference count
        p->uiotab[ret] = x; // point new fd at same uio
    }

    return ret;
}
