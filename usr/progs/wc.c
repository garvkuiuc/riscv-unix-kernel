#include "syscall.h"

static void putnum(long v) {
  char buf[32];
  int i = 0;
  if (v == 0) {
    _write(1, "0", 1);
    return;
  }
  while (v > 0) {
    buf[i++] = '0' + (v % 10);
    v /= 10;
  }
  while (--i >= 0)
    _write(1, &buf[i], 1);
}

void main(int argc, char *argv[]) {
  int fd = 0;

  if (argc > 1) {
    fd = _open(-1, argv[1]); 
    if (fd < 0) {
      _write(1, "wc: cannot open\n", 16);
      return;
    }
  }

  long lines = 0, words = 0, bytes = 0;
  char buf[512];
  int n;
  int inword = 0;

  while ((n = _read(fd, buf, sizeof(buf))) > 0) {
    bytes += n;
    for (int i = 0; i < n; i++) {
      char c = buf[i];
      if (c == '\n')
        lines++;

      if (c == ' ' || c == '\n' || c == '\t') {
        if (inword) {
          words++;
          inword = 0;
        }
      } else {
        inword = 1;
      }
    }
  }

  if (inword)
    words++;

  putnum(lines);
  _write(1, "\t", 1);
  putnum(words);
  _write(1, "\t", 1);
  putnum(bytes);
  _write(1, "\n", 1);

  if (argc > 1)
    _close(fd);
}