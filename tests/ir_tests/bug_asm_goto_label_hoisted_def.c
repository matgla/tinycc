/*
 *  TCC IR - an asm goto label keeps the address def hoisted in front of it
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* ssa:local_addr_cse inserts `T <- &sym` at the first instruction of a label's
 * block and moved the IR branches onto it, but the asm goto label stayed on
 * the old first instruction and skipped the def.  A label now follows an
 * anchor instruction that such an insertion moves onto the def, and its
 * symbol and asm goto CFG edges both resolve through it (k7-k9: labels whose
 * instruction a pass moves, switch_head and loop rotation). */
#include <stdio.h>
#define NI __attribute__((noinline))
#define AG_JNZ(a, L) __asm__ goto("cmp %0, #0\n\tbne %l1" ::"r"(a) : "cc" : L)

unsigned sink;
struct G
{
  int a, b, c;
} g;

/* L is entered only by the asm goto */
NI int k1(int x)
{
  __asm__ goto("cmp %0, #3\n\tbeq %l[L]" : : "r"(x) : "cc" : L);
  return 1;
L:
  sink += 1;
  return 2;
}

/* L is also a plain goto target */
NI int k2(int x)
{
  if (x == 9)
    goto L;
  __asm__ goto("cmp %0, #3\n\tbeq %l[L]" : : "r"(x) : "cc" : L);
  return 1;
L:
  sink += 1;
  return 2;
}

/* two labels, both starting with a read-modify-write of a global struct */
NI int k3(int x)
{
  __asm__ goto("cmp %0, #1\n\tbeq %l[A]\n\tcmp %0, #2\n\tbeq %l[B]" : : "r"(x) : "cc" : A, B);
  return 0;
A:
  g.a += x;
  g.b += 2 * x;
  return 10;
B:
  g.c += x;
  g.b -= x;
  return 20;
}

/* the label heads a loop closed by the asm goto itself */
NI int k4(int n)
{
  int i = 0;
top:
  sink += 3;
  g.a ^= i;
  i++;
  __asm__ goto("cmp %0, %1\n\tblt %l[top]" : : "r"(i), "r"(n) : "cc" : top);
  return i;
}

/* neighbour: a computed goto's label (no asm goto) */
NI int k5(int x)
{
  static void *tab[] = {&&N, &&Y};
  goto *tab[x == 3];
N:
  return 1;
Y:
  sink += 5;
  return 2;
}

/* neighbour: the hoisted address with no label at all */
NI int k6(int x)
{
  sink += x;
  g.a += sink;
  return g.a;
}

/* the switch's dispatch moves to the top: c2 moves with its case */
NI int k7(int x, int y)
{
  int r = 0;
  switch (x) {
  case 0: r = 1; AG_JNZ(y, c2); break;
  case 1: r = 2; break;
  case 2:
  c2:
    r += 30; break;
  case 3: r = 4; break;
  default: r = 9;
  }
  return r * 10 + y;
}

int arr[16] = {3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9, 3};

/* a label in the latch of a rotated loop */
NI int k8(int n)
{
  int s = 0;
  for (int i = 0; i < n; i++) {
    s += arr[i];
    AG_JNZ(arr[i] & 1, lat);
    s *= 2;
  lat:
    s -= 1;
  }
  return s;
}

/* a label in a rotated inner loop */
NI int k9(int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
    for (int j = 0; j < 4; j++) {
      AG_JNZ((i ^ j) == 2, sk);
      s += i * j;
      continue;
    sk:
      s += 1000;
      sink += s;
    }
  return s;
}

int main(void)
{
  int a = k1(2), b = k1(3);
  printf("k1 %d %d sink=%u\n", a, b, sink);
  int c = k2(2), d = k2(3), e = k2(9);
  printf("k2 %d %d %d sink=%u\n", c, d, e, sink);
  int f = k3(0), h = k3(1), i = k3(2), j = k3(1);
  printf("k3 %d %d %d %d g=%d,%d,%d\n", f, h, i, j, g.a, g.b, g.c);
  int k = k4(5), l = k4(1);
  printf("k4 %d %d sink=%u g.a=%d\n", k, l, sink, g.a);
  int m = k5(2), o = k5(3);
  printf("k5 %d %d sink=%u\n", m, o, sink);
  int p = k6(4);
  printf("k6 %d sink=%u\n", p, sink);
  printf("k7 %d %d %d %d %d %d\n", k7(0, 0), k7(0, 1), k7(1, 1), k7(2, 0), k7(3, 3), k7(7, 2));
  printf("k8 %d %d\n", k8(16), k8(1));
  sink = 0;
  int q = k9(5);
  printf("k9 %d sink=%u\n", q, sink);
  return 0;
}
