/*
 *  TCC IR - dead_temp_local must not take a write in another branch as a kill
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * The pass scanned forward through jumps, so the copy into a nested struct
 * ternary's result in one arm died of the copy in the next arm, which never runs
 * after it.  Writes past a jump or a label now say nothing about this store.
 */

#include <stdio.h>

typedef struct { int a; int b; int c; } Plain;
typedef struct __attribute__((packed)) { int a; int b; unsigned char c; } Packed;

#define NONE_OF(T) ((T){.a = -1, .b = 0, .c = 0})

__attribute__((noinline)) Plain plain_of(int v) { Plain p = {v, v * 2, v + 1}; return p; }
__attribute__((noinline)) Packed packed_of(int v) { Packed p = {v, v * 2, (unsigned char)(v + 1)}; return p; }

/* Fills the frame with garbage for a lost store to read. */
__attribute__((noinline)) void scribble(void)
{
  volatile int x[48];
  for (int i = 0; i < 48; i++)
    x[i] = 0x5a5a5a5a;
}

#define PICKS(T, of)                                                                                                   \
  __attribute__((noinline)) int three_arms_##T(int k, int c)                                                           \
  {                                                                                                                    \
    T op = (k == 0) ? (c ? of(10) : NONE_OF(T)) : (k == 1) ? (c ? of(20) : NONE_OF(T)) : NONE_OF(T);                   \
    return op.a * 1000 + op.b * 10 + op.c;                                                                             \
  }                                                                                                                    \
  __attribute__((noinline)) int two_arms_##T(int k, int c)                                                             \
  {                                                                                                                    \
    T op = (k == 0) ? (c ? of(30) : NONE_OF(T)) : NONE_OF(T);                                                          \
    return op.a * 1000 + op.b * 10 + op.c;                                                                             \
  }                                                                                                                    \
  __attribute__((noinline)) int four_arms_##T(int k)                                                                   \
  {                                                                                                                    \
    T op = k == 0 ? of(1) : k == 1 ? NONE_OF(T) : k == 2 ? of(3) : NONE_OF(T);                                         \
    return op.a * 1000 + op.b * 10 + op.c;                                                                             \
  }                                                                                                                    \
  /* Read on the next turn of a loop, above the store in the listing. */                                               \
  __attribute__((noinline)) int in_a_loop_##T(int n)                                                                   \
  {                                                                                                                    \
    int sum = 0;                                                                                                       \
    T last = NONE_OF(T);                                                                                               \
    for (int i = 0; i < n; i++)                                                                                        \
    {                                                                                                                  \
      sum += last.a;                                                                                                   \
      last = (i & 1) ? of(i) : NONE_OF(T);                                                                             \
    }                                                                                                                  \
    return sum;                                                                                                        \
  }

PICKS(Plain, plain_of)
PICKS(Packed, packed_of)

#define RUN(T)                                                                                                         \
  do                                                                                                                   \
  {                                                                                                                    \
    for (int k = 0; k < 3; k++)                                                                                        \
      for (int c = 0; c < 2; c++)                                                                                      \
      {                                                                                                                \
        scribble();                                                                                                    \
        printf(#T " three %d %d = %d\n", k, c, three_arms_##T(k, c));                                                  \
        scribble();                                                                                                    \
        printf(#T " two %d %d = %d\n", k, c, two_arms_##T(k, c));                                                      \
      }                                                                                                                \
    for (int k = 0; k < 4; k++)                                                                                        \
    {                                                                                                                  \
      scribble();                                                                                                      \
      printf(#T " four %d = %d\n", k, four_arms_##T(k));                                                               \
    }                                                                                                                  \
    scribble();                                                                                                        \
    printf(#T " loop = %d\n", in_a_loop_##T(9));                                                                       \
  } while (0)

int main(void)
{
  RUN(Plain);
  RUN(Packed);
  return 0;
}
