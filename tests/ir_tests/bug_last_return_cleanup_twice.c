/*
 *  TCC Tests - the function's last `return` and its scope's cleanups
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* A `return` as the body's last statement emitted no jump, falling into the
 * epilogue -- but the body's cleanups are compiled after it too (a goto may
 * reach them), so a return with no RETURNVALUE (void, a struct or _Complex
 * through memory) ran them twice.  It jumps now when cleanups are pending. */
#include <stdio.h>

struct S
{
  int a[4];
};

static int ran;
static volatile int one = 1;

static void cl(int *p)
{
  ran += *p;
}

__attribute__((noinline)) void v_last(void)
{
  int x __attribute__((cleanup(cl))) = 1;
  return;
}

__attribute__((noinline)) struct S s_last(void)
{
  int x __attribute__((cleanup(cl))) = 1;
  struct S s = {{1, 2, 3, 4}};
  return s;
}

__attribute__((noinline)) double _Complex c_last(void)
{
  int x __attribute__((cleanup(cl))) = 1;
  double _Complex z = 1.5 + 2.5i;
  return z;
}

__attribute__((noinline)) float _Complex f_last(void)
{
  int x __attribute__((cleanup(cl))) = 1;
  float _Complex z = 3.0f + 4.0fi;
  return z;
}

/* a goto into the cleanups after the last return still runs them once */
__attribute__((noinline)) struct S s_goto(int k)
{
  struct S s = {{5, 6, 7, 8}};
  if (k)
    goto out;
  {
    int x __attribute__((cleanup(cl))) = 10;
    return s;
  }
out:
  s.a[0] = 50;
  return s;
}

__attribute__((noinline)) int i_last(void)
{
  int x __attribute__((cleanup(cl))) = 1;
  return 7;
}

int main(void)
{
  v_last();
  printf("void %d\n", ran);
  ran = 0;
  struct S s = s_last();
  printf("struct %d %d\n", ran, s.a[3]);
  ran = 0;
  double _Complex c = c_last();
  printf("complex %d %.1f %.1f\n", ran, __real__ c, __imag__ c);
  ran = 0;
  float _Complex f = f_last();
  printf("float complex %d %.1f %.1f\n", ran, (double)__real__ f, (double)__imag__ f);
  ran = 0;
  s = s_goto(0);
  printf("goto0 %d %d\n", ran, s.a[0]);
  ran = 0;
  s = s_goto(one);
  printf("goto1 %d %d\n", ran, s.a[0]);
  ran = 0;
  int i = i_last();
  printf("int %d %d\n", ran, i);
  return 0;
}
