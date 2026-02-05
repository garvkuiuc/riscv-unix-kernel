#include "syscall.h"
#include "string.h"
#include "shell.h"

static void print_args(int argc, char* argv[]){ 
    int i = 1;
    if(argc <= 1){ // nothing to print
        dputc(STDOUT, '\n');
        return;
    }
    while(i < argc){ // walk through argv
        if(i > 1){ // separate with spaces
            dputc(STDOUT, ' ');
        }
        dprintf(STDOUT, "%s", argv[i]); // output one arg
        i++;
    }
    dputc(STDOUT, '\n'); // end with newline
}

void main(int argc, char* argv[]){
    print_args(argc, argv); // handle printing
    _exit(); // finish cleanly
}
