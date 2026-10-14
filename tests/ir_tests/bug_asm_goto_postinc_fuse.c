/*
 *  TCC IR - post-RA fusions stop at an asm goto
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* ra:load_postinc/ra:store_postinc folded a later `p += k` into an access
 * across an asm goto, so the label's path saw p already advanced.  The
 * forward window now ends at any instruction that may branch. */
#include <stdio.h>
#define NI __attribute__((noinline))
#define AG_JNZ(a, L) __asm__ goto("cmp %0, #0\n\tbne %l1" ::"r"(a) : "cc" : L)
#define AG_JLT(a, b, L) __asm__ goto("cmp %0, %1\n\tblt %l2" ::"r"(a), "r"(b) : "cc" : L)

int arr[8] = {5, 6, 7, 8, 9, 10, 11, 12};
int arr2[40];
short sh[8];

/* straight line: `*p`, asm goto, `p += 2` */
NI int fwd(int *p, int key)
{
  int v = *p;
  AG_JNZ(v == key, out);
  p += 2;
  return *p * 100 + v;
out:
  return (int)(p - arr) * 1000 + v;
}

/* pointer IV in a loop closed by an asm goto, exit by another asm goto */
NI int ptriv(int n, int key)
{
  int *p = arr2, s = 0, i = 0;
top:
  s += *p;
  AG_JNZ(*p == key, found);
  p += 2;
  i++;
  AG_JLT(i, n, top);
  return -s;
found:
  return (int)(p - arr2) * 1000 + s;
}

/* store form: `*q = v`, asm goto, `q += 1` */
NI int st(short *q, int v, int c)
{
  *q = v;
  AG_JNZ(c, out);
  q += 1;
  *q = v + 1;
  return (int)(q - sh);
out:
  return 100 + (int)(q - sh);
}

/* neighbour: no asm goto between the access and the bump */
NI int plain(int *p, int n)
{
  int s = 0;
  while (n--) {
    s += *p;
    p += 1;
  }
  return s;
}

int main(void)
{
  for (int i = 0; i < 40; i++)
    arr2[i] = (i * 13 + 5) % 17;
  printf("fwd %d %d %d\n", fwd(arr, 5), fwd(arr + 1, 5), fwd(arr + 3, 8));
  printf("loop %d %d %d\n", ptriv(15, 12), ptriv(15, 99), ptriv(1, 5));
  int a = st(sh + 2, 7, 1), b = st(sh + 4, 9, 0);
  printf("st %d %d %d %d %d %d\n", a, b, sh[2], sh[3], sh[4], sh[5]);
  printf("plain %d\n", plain(arr, 8));
  return 0;
}
