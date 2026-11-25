/*
 *  TCC - yasc slices: what runs, and what traps
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* test_yasc_slices.py: argv[1] names a case; it prints "go <case>", runs,
 * and prints "ok <case> <value>" -- or traps on a slice's check (udf, a
 * HardFault on the QEMU board).  Operands come from volatiles. */
#pragma yasc
#include <stdio.h>
#include <string.h>

volatile int v_zero = 0, v_one = 1, v_two = 2, v_three = 3, v_four = 4, v_five = 5, v_m1 = -1;
static int nums[5] = {1, 2, 3, 4, 5};

static __attribute__((noinline)) int sum([]const int s)
{
  int t = 0;
  for (unsigned i = 0; i < s.len; i++)
    t += s[i];
  return t;
}

static __attribute__((noinline)) int sum_down([]const int s)
{
  int t = 0;
  for (unsigned i = s.len; i > 0; i--)
    t += s[i - 1];
  return t;
}

static __attribute__((noinline)) int at([]const int s, int i) { return s[i]; }
static __attribute__((noinline)) []const int part([]const int s, int a, int b) { return s[a:b]; }
static __attribute__((noinline)) []const int tail([]const int s, int a) { return s[a:]; }
static __attribute__((noinline)) void fill([]int s, int v)
{
  for (unsigned i = 0; i < s.len; i++)
    s[i] = v + (int)i;
}
static __attribute__((noinline)) unsigned slen([:0]const char s) { return s.len; }
static __attribute__((noinline)) int ends_in_zero([:0]const char s) { return s.ptr[s.len] == 0; }
/* [:0]: s[s.len] is its zero, read and written with a zero, never more */
static __attribute__((noinline)) unsigned walk([:0]const char s)
{
  unsigned i = 0;
  while (s[i])
    i++;
  return i;
}
static __attribute__((noinline)) void put([:0]char s, int i, char c) { s[i] = c; }
static __attribute__((noinline)) void cut([:0]char s, int i) { s[i] = '\0'; }
static __attribute__((noinline)) void bump([:0]char s, int i) { s[i]++; }
static __attribute__((noinline)) void mark([:0]char s, int i) { s[i] = 'x'; }
static __attribute__((noinline)) [:0]char terminated(inout char * view p, int n) { return p[0:n:0]; }

int main(int argc, const char * view * view argv)
{
  if (argc < 2)
    return 1;
  const char * view c = argv[1];
  long long r = 0;
  printf("go %s\n", c);
  []const int all = nums;
  if (!strcmp(c, "sum"))
    r = sum(all);
  else if (!strcmp(c, "sum_down"))
    r = sum_down(all);
  else if (!strcmp(c, "at"))
    r = at(all, v_four);
  else if (!strcmp(c, "part"))
    r = sum(part(all, v_one, v_four)) * 10 + part(all, v_one, v_four).len;
  else if (!strcmp(c, "part_empty"))
    r = part(all, v_two, v_two).len;
  else if (!strcmp(c, "tail"))
    r = sum(tail(all, v_three));
  else if (!strcmp(c, "array_slice"))
    r = sum(nums[1:3]) + sum(nums[:2]) * 100 + sum(nums[3:]) * 10000;
  else if (!strcmp(c, "pointer_slice"))
  {
    const int * view p = &nums[1];
    r = sum(p[0:3]);
  }
  else if (!strcmp(c, "fill"))
  {
    int buf[4];
    fill(buf, 10);
    fill(buf[2:], 20);
    r = buf[0] * 1000000 + buf[1] * 10000 + buf[2] * 100 + buf[3];
  }
  else if (!strcmp(c, "string"))
    r = slen("abc") * 10 + ends_in_zero("abcd");
  else if (!strcmp(c, "string_tail"))
  {
    [:0]const char s = "hello";
    [:0]const char t = s[2:];
    r = t.len * 10 + ends_in_zero(t);
  }
  else if (!strcmp(c, "nested"))
  {
    []const int rows_arr[2] = {nums[0:2], nums[2:5]};
    [][]const int rows = rows_arr;
    r = rows.len * 100 + rows[v_one].len * 10 + rows[v_one][v_two];
  }
  else if (!strcmp(c, "vla"))
  {
    int n = v_four, v[n];
    for (int i = 0; i < n; i++)
      v[i] = i + 1;
    r = sum(v[1:n]) * 10 + v[0:n].len;
  }
  else if (!strcmp(c, "sentinel_read"))
  {
    [:0]const char s = "abc";
    r = s[v_three] * 100 + walk(s) * 10 + walk(s[v_one:]);   /* 0, 3, 2 */
  }
  else if (!strcmp(c, "sentinel_cut"))
  {
    char buf[] = "usr/lib";
    [:0]char s = buf[0:7:0];
    cut(s, v_three);                 /* "usr" -- the slice still 7 long */
    cut(s, 7);                       /* its own zero again: a no-op */
    put(s, 7, (char)v_zero);         /* a zero not known here: allowed */
    mark(s, v_one);                  /* below len: anything */
    r = (long long)strlen(buf) * 100 + s.len * 10 + (buf[1] == 'x');
  }
  else if (!strcmp(c, "terminated_pointer"))
  {
    char buf[8] = "ab\0cd";
    char * view p = buf;
    r = terminated(p, v_two).len * 10 + walk(terminated(p + 3, v_two));   /* 2, 2 */
  }
  else if (!strcmp(c, "terminated_array"))
  {
    char b[4] = "ab";
    [:0]const char s = b[0:v_two:0];
    [:0]const char t = s[v_two:v_two:0]; /* s's own zero ends it: empty */
    r = s.len * 10 + t.len;
  }
  /* traps */
  else if (!strcmp(c, "at_len"))
    r = at(all, v_five);
  else if (!strcmp(c, "at_neg"))
    r = at(all, v_m1);
  else if (!strcmp(c, "part_past_end"))
    r = part(all, v_one, v_five + 1).len;
  else if (!strcmp(c, "part_backwards"))
    r = part(all, v_three, v_two).len;
  else if (!strcmp(c, "tail_past_end"))
    r = tail(all, v_five + 1).len;
  else if (!strcmp(c, "store_past_end"))
  {
    int buf[2];
    []int s = buf;
    s[v_two] = 1;
    r = buf[0];
  }
  else if (!strcmp(c, "sentinel_past"))
  {
    [:0]const char s = "abc";
    r = s[v_four];
  }
  else if (!strcmp(c, "sentinel_overwrite"))
  {
    char buf[] = "ab";
    put(buf[0:2:0], v_two, (char)v_one);
    r = buf[0];
  }
  else if (!strcmp(c, "sentinel_overwrite_const"))
  {
    char buf[] = "ab";
    mark(buf[0:2:0], v_two);
    r = buf[0];
  }
  else if (!strcmp(c, "sentinel_bump"))
  {
    char buf[] = "ab";
    bump(buf[0:2:0], v_two);
    r = buf[0];
  }
  else if (!strcmp(c, "terminated_no_zero"))
  {
    char buf[4] = "abc";
    r = terminated(buf, v_two).len;
  }
  else if (!strcmp(c, "terminated_past_array"))
  {
    char b[3] = {'a', 'b', 'c'};
    r = b[0:v_three:0].len;          /* b[3] is past the array */
  }
  else if (!strcmp(c, "strnlen_then_write"))
  {
    /* vrp knows strnlen's zero -- until something writes over it */
    char buf[8] = "ab";
    size_t n = strnlen(buf, sizeof buf);
    if (n != sizeof buf)
    {
      buf[n] = (char)v_one;
      r = buf[0:n:0].len;
    }
  }
  else if (!strcmp(c, "strlen_then_write"))
  {
    /* and strlen's zero, the same */
    char buf[8] = "ab";
    size_t n = strlen(buf);
    buf[n] = (char)v_one;
    r = buf[0:n:0].len;
  }
  else if (!strcmp(c, "terminated_past_slice"))
  {
    char b[4] = "abc";
    []char s = b[0:3];               /* no sentinel: s[3] is not s's */
    r = s[0:v_three:0].len;
  }
  printf("ok %s %lld\n", c, r);
  return 0;
}
