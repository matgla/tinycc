/*
 *  TCC - libtcc1's __chk_fail, the one a program without its own gets
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* test_compiler_fortify.py: argv[1] names a case.  Each prints "go CASE",
 * then a failed check ends it in libtcc1's __chk_fail (the message on
 * stderr, abort) before "ok CASE". */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* vsprintf into this function's own 6 bytes */
static __attribute__((noinline)) int fmt_local(const char *fmt, ...)
{
  char d[6];
  va_list ap;
  va_start(ap, fmt);
  vsprintf(d, fmt, ap);
  va_end(ap);
  return d[0];
}

__attribute__((noinline, null_terminated_string_arg(1))) size_t my_len(const char *s)
{
  return strlen(s);
}

int main(int argc, char **argv)
{
  volatile int n = 4;
  if (argc < 2)
    return 2;
  printf("go %s\n", argv[1]);
  fflush(stdout);
  if (!strcmp(argv[1], "str"))
  {
    char b[4];
    memset(b, 'x', sizeof b);
    printf("ok str %d\n", (int)my_len(b));
  }
  else if (!strcmp(argv[1], "strcpy"))
  {
    char d[4];
    char s[8] = "abcdefg";
    s[n] = 0; /* 4 characters and the NUL: one more than d holds */
    __builtin___strcpy_chk(d, s, __builtin_object_size(d, 1));
    printf("ok strcpy %c\n", d[0]);
  }
  else if (!strcmp(argv[1], "plain_strcpy"))
  {
    char d[4];
    char s[8] = "abcdefg";
    s[n] = 0;
    strcpy(d, s); /* the C library's: -ffortify checks it by name */
    printf("ok plain_strcpy %c\n", d[0]);
  }
  else if (!strcmp(argv[1], "plain_strcat"))
  {
    char d[6] = "ab";
    char s[8] = "cdefg";
    s[n] = 0;
    strcat(d, s); /* "ab" + "cdef" + NUL: 7 bytes */
    printf("ok plain_strcat %c\n", d[0]);
  }
  else if (!strcmp(argv[1], "plain_sprintf"))
  {
    char d[6];
    sprintf(d, "%d-%d", n * 1000, n); /* "4000-4" and the NUL: 7 bytes */
    printf("ok plain_sprintf %c\n", d[0]);
  }
  else if (!strcmp(argv[1], "plain_vsprintf"))
  {
    printf("ok plain_vsprintf %c\n", fmt_local("%d%d", n * 1000, n * 10)); /* "400040" */
  }
  else if (!strcmp(argv[1], "fits"))
  {
    char d[8], e[8] = "ab", f[16];
    char s[8] = "abcdefg";
    s[n] = 0;
    strcpy(d, s);
    strcat(e, s);
    sprintf(f, "%d-%s", n, s);
    printf("ok fits %s %s %s\n", d, e, f);
  }
  return 0;
}
