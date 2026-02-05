#include "syscall.h"
#include "string.h"
#include "shell.h"

static int make_path(const char *arg, char *out, size_t outsz){ 
    if(arg == NULL || *arg == '\0'){ // reject empty input
        return -1;
    }
    if(arg[0] == '/' && arg[1] == '\0'){ // reject root only
        return -1;
    }

    if(arg[0] == '/'){ //map into c/
        snprintf(out, outsz, "c/%s", arg + 1);
        return 0;
    }

    if(strchr(arg, '/') == NULL){ // simple name assume c
        snprintf(out, outsz, "c/%s", arg);
        return 0;
    }

    snprintf(out, outsz, "%s", arg); // already has subpath
    return 0;
}

static int touch_one_file(const char *argument){ 
    char tmpbuf[256];

    if(make_path(argument, tmpbuf, sizeof(tmpbuf)) < 0){ // validate and build path
        dprintf(CONSOLEOUT, "touch: path invalid");
        return -1;
    }

    int ret = _fscreate((char *)tmpbuf); // create if possible
    if(ret < 0){ // report create failure
        dprintf(CONSOLEOUT, "touch: create not possible %s", argument);
        return -1;
    }

    return 0;
}

void main(int argc, char* argv[]){
    if(argc < 2){ // needs at least one filename
        dprintf(CONSOLEOUT, "touch: missing operand");
        _exit();
    }

    for(int i = 1; i < argc; i++){ // touch each argument
        touch_one_file(argv[i]);
    }
    _exit(); // done
}
