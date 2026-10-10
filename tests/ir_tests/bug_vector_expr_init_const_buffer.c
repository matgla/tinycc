/*
 * A small vector/array local initialised from an expression (not a brace
 * list) kept its zero-filled const_init buffer marked valid, so vector
 * constant folding read it as {0,...}.  Only a captured brace list is valid.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>
#include <string.h>

typedef int v4 __attribute__((vector_size(16)));
typedef signed char c16 __attribute__((vector_size(16)));

v4 ga = {10, 20, 30, 40};

__attribute__((noinline)) v4 mk(int k)
{
  v4 r = {k, k + 1, k + 2, k + 3};
  return r;
}

__attribute__((noinline)) int f(v4 a)
{
  v4 t = a;
  v4 u = {1, 2, 3, 4};
  v4 r = t + u;
  return r[0];
}

__attribute__((noinline)) int g(void)
{
  v4 t = ga;
  v4 u = {1, 2, 3, 4};
  v4 r = t + u;
  return r[1];
}

__attribute__((noinline)) int h(v4 *p)
{
  v4 t = *p;
  v4 u = {1, 2, 3, 4};
  v4 r = u * t;
  return r[2];
}

__attribute__((noinline)) int call_init(void)
{
  v4 t = mk(7);
  v4 u = {1, 2, 3, 4};
  v4 r = t - u;
  return r[3];
}

/* Vector elements of a brace-listed array: the element stores are whole
 * vectors, which init_putv does not capture. */
__attribute__((noinline)) int array_of_vectors(v4 a)
{
  v4 arr[2] = {a, a};
  v4 u = {1, 2, 3, 4};
  v4 r = arr[0] + u;
  return r[1];
}

/* A string initialiser copied in bulk from .rodata. */
__attribute__((noinline)) int string_init(void)
{
  char s[16] = "ABCDEFGHIJKLMNO";
  c16 one = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  c16 r = *(c16 *)s + one;
  return r[0];
}

/* Neighbouring behaviour: two brace-list constants still fold correctly. */
__attribute__((noinline)) int both_const(void)
{
  v4 a = {1, 2, 3, 4};
  v4 b = {10, 20, 30, 40};
  v4 c = a * b + a;
  return c[0] + c[1] + c[2] + c[3];
}

/* A brace-list constant copied in its initialiser to another local. */
__attribute__((noinline)) int const_copy(void)
{
  v4 a = {1, 2, 3, 4};
  v4 t = a;
  v4 u = {5, 6, 7, 8};
  v4 r = t + u;
  return r[0] * 1000 + r[3];
}

int main(void)
{
  v4 x = {100, 200, 300, 400};
  printf("f=%d (101)\n", f(x));
  printf("g=%d (22)\n", g());
  printf("h=%d (900)\n", h(&x));
  printf("call_init=%d (6)\n", call_init());
  printf("array_of_vectors=%d (202)\n", array_of_vectors(x));
  printf("string_init=%d (66)\n", string_init());
  printf("both_const=%d (310)\n", both_const());
  printf("const_copy=%d (6012)\n", const_copy());
  return 0;
}
