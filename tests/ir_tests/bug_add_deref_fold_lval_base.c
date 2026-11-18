/*
 *  TCC Tests - add_deref_fold: an ADD whose base is itself a load
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* `(*ss)[1]` is `T0 = P0; T1 = *T0 + 1; *T1`.  add_deref_fold took the ADD's
 * lval base for the plain register it copies from and emitted
 * LOAD_INDEXED [P0, #1]: a byte of the pointer `*ss`, not of the string
 * (toybox sh read "n-256" as "n--56").  Bases that are loads are left alone. */
#include <stdio.h>

static char *nospace(char **ss)
{
  while (**ss == ' ')
    ++*ss;
  return *ss;
}

/* toybox sh recalculate(): is this a postincrement or postdecrement? */
__attribute__((noinline)) int is_postop(char **ss)
{
  if ((*nospace(ss) == '+' || **ss == '-') && (*ss)[1] == **ss)
    return 1;
  return 0;
}

__attribute__((noinline)) int second_char(char **ss)
{
  return (*ss)[1];
}

__attribute__((noinline)) int second_int(int **pp)
{
  return (*pp)[1];
}

__attribute__((noinline)) int third_short(short **pp)
{
  return (*pp)[2];
}

__attribute__((noinline)) int through_copy(char **ss)
{
  char **q = ss;
  return (*q)[3];
}

/* Still folded: a plain pointer parameter is a register base. */
__attribute__((noinline)) int plain_second(const char *p)
{
  return p[1];
}

int main(void)
{
  char minus[] = "n-256";
  char post[] = "  --";
  char plus[] = "++x";
  char lone[] = "-5";
  int ints[] = {11, 22, 33};
  short shorts[] = {-1, -2, -3};
  char *s;
  int r;
  int *ip = ints;
  short *sp = shorts;

  s = minus + 1;
  printf("n-256: %d\n", is_postop(&s));
  s = post;
  r = is_postop(&s);
  printf("  --: %d at %d\n", r, (int)(s - post));
  s = plus;
  printf("++x: %d\n", is_postop(&s));
  s = lone;
  printf("-5: %d\n", is_postop(&s));

  s = minus;
  printf("second_char: %c\n", second_char(&s));
  printf("second_int: %d\n", second_int(&ip));
  printf("third_short: %d\n", third_short(&sp));
  printf("through_copy: %c\n", through_copy(&s));
  printf("plain_second: %c\n", plain_second(minus));
  return 0;
}
