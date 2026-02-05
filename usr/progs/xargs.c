#include "string.h"
#include "syscall.h"

void main(int argc, char *argv[]) {
  if (argc < 2) {
    _write(2, "xargs: missing command\n", 23);
    _exit();
  }

  char buf[1024];
  int total = 0;
  while (1) {
    int n = _read(0, buf + total, sizeof(buf) - 1 - total);
    if (n <= 0)
      break;
    total += n;
    if (total >= sizeof(buf) - 1)
      break;
  }
  buf[total] = 0;

  char *newargv[32];
  int ac = 0;

  for (int i = 1; i < argc && ac < 31; i++)
    newargv[ac++] = argv[i];

  char *p = buf;
  while (*p && ac < 31) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
      *p = 0;
      p++;
    }
    if (!*p)
      break;

    char *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
      p++;
    if (*p) {
      *p = 0;
      p++;
    }

    newargv[ac++] = start;
  }

  newargv[ac] = 0;

  char cmdpath[256];
  char *cmd = newargv[0];

  if (strchr(cmd, '/') == 0)
    snprintf(cmdpath, sizeof(cmdpath), "c/%s", cmd);
  else
    snprintf(cmdpath, sizeof(cmdpath), "%s", cmd);

  int fd = 3;
  if (_open(fd, cmdpath) < 0) {
    _write(2, "xargs: cannot open\n", 19);
    _exit();
  }

  _exec(fd, ac, newargv);
  
  _exit();
}