#include "syscall.h"
#include "shell.h"
#include "string.h"
#include <stddef.h>

static int buildPath(const char *input, char *outbuf, size_t outcap){ 
    int ok = 0;

    if(input == NULL){ // null input
        ok = -1;
    }

    else if(*input == '\0'){ // empty input
        ok = -1;
    }

    else if(input[0] == '/' && input[1] == '\0'){ // reject root only
        ok = -1;
    }

    else{

        int is_abs = 0;
        int saw_slash = 0;

        if(input[0] == '/'){ // absolute path
            is_abs = 1;
        }
        else{
            const char *p = input;

            while(*p != '\0'){ // scan for slash
                if(*p == '/'){
                    saw_slash = 1;
                    break;
                }
                ++p;
            }
        }

        if(is_abs){ // map into c
            snprintf(outbuf, outcap, "c/%s", input + 1);
        }
        else if(saw_slash == 0){ // no slash assume c
            snprintf(outbuf, outcap, "c/%s", input);
        }
        else{ // keep provided relative path

            snprintf(outbuf, outcap, "%s", input);
        }
    }

    return ok; // success or failure
}

void main(int argc, char* argv[]){
    char io_chunk[128];
    long bytes_read;

    int in_fd = STDIN;

    if(argc > 1){ 
        char resolved[128];

        if(buildPath(argv[1], resolved, sizeof(resolved)) < 0){ // validate path
            dprintf(CONSOLEOUT, "cat: invalid path\n");
            _exit();
        }

        in_fd = _open(-1, resolved); // open file
        if(in_fd < 0){ // open failed
            dprintf(CONSOLEOUT, "cat: open failed\n");
            _exit();
        }
    }

    bytes_read = _read(in_fd, io_chunk, 128); // initial read
    while(bytes_read > 0){ // read/write loop
        long remaining = bytes_read;
        char *cursor = io_chunk;

        while(remaining > 0){ // write until this chunk is done
            long just_sent = _write(STDOUT, cursor, (unsigned long)remaining); // write remaining bytes
            if(just_sent <= 0){ // write error
                dprintf(CONSOLEOUT, "cat: write failed\n"); // report error
                if(in_fd != STDIN){ // close file if needed
                    
                    _close(in_fd);
                }
                _exit(); // abort on failure
            }
            cursor += just_sent; // advance buffer pointer
            remaining -= just_sent; // track bytes left
        }

        bytes_read = _read(in_fd, io_chunk, 128); // next read
    }

    if(in_fd != STDIN){ // close file if used
        _close(in_fd);
    }

    _exit(); // done
}
