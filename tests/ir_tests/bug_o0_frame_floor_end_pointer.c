/*
 *  TCC Tests - -O0 frame floor: an array reached only through its end
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* regalloc.c compute_stack_layout raised the frame floor to the lowest
 * offset the IR names; `num + sizeof num` names the byte above num, so the
 * parameter spills went over num.  An offset now keeps the frontend object
 * holding it and the one ending at it (frame.c tcc_ir_frame_object_at). */
#include <stdio.h>

__attribute__((noinline)) void put(const char *s) { fputs(s, stdout); }

/* the end pointer of the only array: the top of the frame */
__attribute__((noinline)) void report(const char *expr, const char *file, unsigned int line, const char *func)
{
  char num[12];
  char *p = num + sizeof(num);
  *--p = '\0';
  do {
    *--p = (char)('0' + line % 10);
    line /= 10;
  } while (line);
  put(file);
  put(":");
  put(p);
  put(": ");
  if (func && *func) {
    put(func);
    put(": ");
  }
  put("`");
  put(expr);
  put("'\n");
}

/* the end of the lower array is the start of the upper one */
__attribute__((noinline)) void two(const char *a, const char *b, unsigned int n, const char *c)
{
  char up[4] = {'u', 'p', '!', 0};
  char low[8];
  char *p = low + sizeof(low);
  *--p = '\0';
  do {
    *--p = (char)('a' + n % 26);
    n /= 26;
  } while (n && p > low);
  put(a);
  put(p);
  put(b);
  put(up);
  put(c);
  put("\n");
}

/* an index inside the array names its middle */
__attribute__((noinline)) void middle(const char *a, const char *b, unsigned int n, const char *c)
{
  char buf[16];
  char *q = &buf[15];
  *q = '\0';
  for (int i = 0; i < 6; i++)
    *--q = (char)('0' + (n + i) % 10);
  put(a);
  put(q);
  put(b);
  put(buf + 9);
  put(c);
  put("\n");
}

int main(void)
{
  report("1 + 1 == 3", "f.c", 72, "fn");
  report("x", "f.c", 7, "");
  two("<", ">", 12345, ".");
  middle("[", "|", 4, "]");
  return 0;
}
