/*
 *  TCC IR - A derived IV whose base is a load of memory the loop writes
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* derived_iv took `V <-- i*3 ADD g***DEREF***` as base + IV*k and hoisted
 * the load of g into the preheader although the loop stores g, so later
 * iterations read g's value from before the loop.  A base that reads memory
 * is now hoisted only out of a loop that writes no memory and calls nothing. */
#include <stdio.h>

int g, cnt, hits;
#define WORK { hits++; printf("c%d d%d\n", g + 3, cnt + 4); printf("e%d f%d\n", g * 5, cnt * 6); }
static void f_glob(int x) { if (g == x) WORK }
static void f_shadow(int a) { int g2; g2 = a + g; if (g2 > 5) WORK }
static void f_bool2(_Bool b) { if (b) WORK }
static void f_short(short s) { if (s == -1) WORK }

int *gp;
int tab[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

/* The loop writes only a local: the base load of gp may stay hoisted. */
__attribute__((noinline)) int sum_ro(int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
  {
    int *p = gp + i * 3;
    s += (int)(p - tab);
  }
  return s;
}

/* The loop rewrites gp itself: every iteration must reread it. */
__attribute__((noinline)) int sum_rw(int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
  {
    int *p = gp + i * 3;
    s += (int)(p - tab);
    gp = gp + 1;
  }
  return s;
}

int main(void)
{
  int i;
  for (i = 0; i < 3; i++)
  {
    f_glob(g++);
    f_glob(++g);
    f_shadow(i * 3);
    f_bool2(0.5 * i);
    f_short(65535 + i);
  }
  printf("hits %d\n", hits);
  gp = tab;
  printf("%d\n", sum_ro(4));
  printf("%d\n", sum_rw(4));
  return 0;
}
