#include "syscall.h"
#include <stdint.h>

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
  
  // Check rtc0 and rtc
  int fd = _open(-1, "dev/rtc0");

  if(fd < 0){
    
    fd = _open(-1, "dev/rtc");
  }

  if (fd < 0) {
    _write(1, "date: rtc error\n", 16);
    return;
  }

  uint64_t ns;
  if (_read(fd, &ns, sizeof(ns)) != sizeof(ns)) {
    _write(1, "date: read error\n", 17);
    _close(fd);
    return;
  }
  _close(fd);

  uint64_t sec = ns / 1000000000ULL;
  uint64_t days = sec / 86400;
  uint64_t rem = sec % 86400;

  int hour = rem / 3600;
  int min = (rem % 3600) / 60;
  int sec2 = rem % 60;

  int year = 1970;
  for (;;) {
    int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    int d = leap ? 366 : 365;
    if (days >= d) {
      days -= d;
      year++;
    } else
      break;
  }

  int md[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
  if (leap)
    md[1] = 29;

  int month = 0;
  while (days >= md[month]) {
    days -= md[month];
    month++;
  }

  int mday = days + 1;

  const char *names = "JanFebMarAprMayJunJulAugSepOctNovDec";

  putnum(mday);
  _write(1, " ", 1);
  _write(1, names + month * 3, 3);
  _write(1, " ", 1);
  putnum(year);
  _write(1, " ", 1);

  if (hour < 10)
    _write(1, "0", 1);
  putnum(hour);
  _write(1, ":", 1);
  if (min < 10)
    _write(1, "0", 1);
  putnum(min);
  _write(1, ":", 1);
  if (sec2 < 10)
    _write(1, "0", 1);
  putnum(sec2);

  _write(1, "\n", 1);
}