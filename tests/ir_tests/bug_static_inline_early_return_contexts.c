/*
 *  TCC - a static inline with several returns, expanded wherever a call can stand
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Registered by token length alone, these stayed calls; registered by what they
 * compile to, they expand here, and every return must land at the call's value. */
#include <stdio.h>

typedef struct Pair
{
  int a;
  int b;
} Pair;

/* Over the token limit, with returns at three depths. */
static inline int classify(int w)
{
  const unsigned type = (unsigned)w >> 29, position = (unsigned)w & 0x7FFFu;
  if (type == 0)
    return -1;
  if (type == 7)
  {
    if (w == -1)
      return -2;
    if ((position & 0x7FF0u) == 0x7FF0u)
      return -(int)((position & 0xFu) + 3);
  }
  return (int)((type << 8) | (position & 0xFFu));
}

static inline Pair make_pair(int x, int flag)
{
  if (flag == 0)
    return (Pair){.a = (int)-1, .b = (int)0};
  if (flag < 0)
    return (Pair){.a = (int)x, .b = (int)-x};
  Pair p = {(int)(x + flag), (int)(x - flag)};
  if (p.a > 100)
    p.b = (int)(p.a * 2);
  return p;
}

/* Reads through its pointer after a return that skips the read. */
static inline int first_or(const int *p, int n, int fallback)
{
  if (!p)
    return fallback;
  if (n <= 0)
    return (int)(fallback + 1);
  return p[0] + (int)(n > 1 ? p[1] : 0);
}

static int calls;
static int counted(int v)
{
  calls++;
  return v;
}

int main(void)
{
  static const int words[] = {0, 5, 0x20000012, 0x40000203, -1, (int)0xE0007FF4u, (int)0xE0000001u};
  static const int data[] = {7, 11};
  const int nwords = sizeof(words) / sizeof(words[0]);

  for (int i = 0; i < nwords; i++)
    printf("classify %d\n", classify(words[i]));

  /* As a condition, an operand, an argument, a nested argument, in a loop header. */
  if (classify(0) < 0 && classify(0x20000012) > 0)
    printf("condition ok\n");
  printf("sum %d\n", classify(0x20000012) + classify(0x40000203) * classify(-1));
  printf("ternary %d\n", classify(5) == -1 ? classify(0x20000012) : classify(-1));
  printf("nested %d\n", classify(classify(0x20000012) << 21));
  int n = 0;
  for (int i = 0; classify(words[i]) != -2; i++)
    n++;
  printf("loop %d\n", n);

  /* The argument is evaluated once, whichever return is taken. */
  calls = 0;
  printf("once %d", classify(counted(-1)));
  printf(" %d", classify(counted(0)));
  printf(" %d", classify(counted(0x20000012)));
  printf(" calls %d\n", calls);

  Pair a = make_pair(10, 0), b = make_pair(10, -1), c = make_pair(10, 3), d = make_pair(99, 5);
  printf("pair %d %d %d %d %d %d %d %d\n", a.a, a.b, b.a, b.b, c.a, c.b, d.a, d.b);
  printf("member %d %d\n", make_pair(4, -1).b, make_pair(4, 2).a + make_pair(4, 0).a);

  printf("first %d %d %d %d\n", first_or(0, 2, 40), first_or(data, 0, 50), first_or(data, 1, 60), first_or(data, 2, 70));
  return 0;
}
