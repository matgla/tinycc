/*
 *  TCC - -ffortify on QEMU (builtin/fortify.c)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* test_compiler_fortify.py: argv[1] names a case.  Each prints "go CASE",
 * then either reaches "ok CASE" or ends: a string with no NUL in its buffer
 * in __chk_fail ("chk_fail", exit 3), a size over its buffer in the UDF
 * (HardFault). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void __chk_fail(void)
{
  puts("chk_fail");
  exit(3);
}

/* the callees: noinline, so the checks are the caller's */
__attribute__((noinline, null_terminated_string_arg(1))) size_t my_len(const char *s)
{
  return strlen(s);
}

__attribute__((noinline, access(write_only, 1, 2))) void my_fill(char *d, size_t n)
{
  memset(d, 'x', n);
}

__attribute__((noinline, access(read_write, 1, 2))) void my_ints(int *d, int n)
{
  for (int i = 0; i < n; i++)
    d[i]++;
}

/* the size first */
__attribute__((noinline, access(write_only, 2, 1))) void my_fill_rev(size_t n, char *d)
{
  memset(d, 'y', n);
}

volatile size_t vn; /* sizes the compiler does not know */
volatile int vi;

struct S
{
  char a[4];
  char rest[12];
};

static int run(const char *c)
{
  printf("go %s\n", c);
  if (!strcmp(c, "str_nonul"))
  {
    char b[8];
    memset(b, 'a', sizeof b);
    vn = my_len(b);
  }
  else if (!strcmp(c, "str_ok"))
  {
    char b[8];
    memset(b, 'a', sizeof b);
    b[7] = 0;
    vn = my_len(b);
  }
  else if (!strcmp(c, "str_member"))
  {
    /* the closest member: a[4] has no NUL, rest does */
    struct S s;
    memset(&s, 'a', sizeof s);
    s.rest[11] = 0;
    vn = my_len(s.a);
  }
  else if (!strcmp(c, "str_heap"))
  {
    char *p = malloc(8);
    memset(p, 'a', 8);
    vn = my_len(p);
  }
  else if (!strcmp(c, "str_null_ok"))
  {
    /* a null pointer is the callee's business: not called here */
    char *volatile p = NULL;
    if (vn == 12345)
      vn = my_len(p);
  }
  else if (!strcmp(c, "fill"))
  {
    char b[8];
    vn = 9;
    my_fill(b, vn);
  }
  else if (!strcmp(c, "fill_ok"))
  {
    char b[8];
    vn = 8;
    my_fill(b, vn);
  }
  else if (!strcmp(c, "ints"))
  {
    int b[4] = {0};
    vi = 5;
    my_ints(b, vi);
  }
  else if (!strcmp(c, "ints_neg"))
  {
    int b[4] = {0};
    vi = -1;
    my_ints(b, vi);
  }
  else if (!strcmp(c, "ints_ok"))
  {
    int b[4] = {0};
    vi = 4;
    my_ints(b, vi);
  }
  else if (!strcmp(c, "vla"))
  {
    vn = 6;
    char b[vn];
    vn = 7;
    my_fill(b, vn);
  }
  else if (!strcmp(c, "vla_ok"))
  {
    vn = 6;
    char b[vn];
    my_fill(b, 6);
  }
  else if (!strcmp(c, "rev"))
  {
    char b[4];
    my_fill_rev(5, b);
  }
  else if (!strcmp(c, "rev_rt"))
  {
    char b[4];
    vn = 5;
    my_fill_rev(vn, b);
  }
  else if (!strcmp(c, "rev_ok"))
  {
    char b[4];
    my_fill_rev(4, b);
  }
  else
    return 2;
  printf("ok %s %u\n", c, (unsigned)vn);
  return 0;
}

int main(int argc, char **argv)
{
  return argc > 1 ? run(argv[1]) : 2;
}
