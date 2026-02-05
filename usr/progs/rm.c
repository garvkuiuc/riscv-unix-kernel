#include "syscall.h"
#include "string.h"
#include "shell.h"
#include <stddef.h>

static int fix_path(const char *arg, char *tmp, size_t tmpsz){
    if(arg == NULL || *arg == '\0'){ // reject empty input
        return -1;
    }

    if(arg[0] == '/' && arg[1] == '\0'){ // reject root only
        return -1;
    }

    if(arg[0] == '/'){ //map into c/
        snprintf(tmp, tmpsz, "c/%s", arg + 1);
        return 0;
    }

    if(strchr(arg, '/') == NULL){ // simple name assume c/
        snprintf(tmp, tmpsz, "c/%s", arg);
        return 0;
    }

    snprintf(tmp, tmpsz, "%s", arg); // already has subpath
    return 0;
}

static int delete_one_file(const char *argument){ // remove one file
    char BUF[256];

    if(fix_path(argument, BUF, sizeof(BUF)) < 0){ //build path
        dprintf(CONSOLEOUT, "rm: invalid path");
        return -1;
    }

    int ret = _fsdelete((char *)BUF); // attempt delete
    if(ret < 0){ // report delete failure
        dprintf(CONSOLEOUT, "rm: cannot remove %s", argument);
        return -1;
    }

    return 0;
}

void main(int argc, char* argv[]){
    if(argc < 2){ // needs at least one filename
        dprintf(CONSOLEOUT, "rm: missing file operand");
        _exit();
    }

    for(int i = 1; i < argc; i++){ // remove each argument
        delete_one_file(argv[i]);
    }

    _exit(); // done
}
