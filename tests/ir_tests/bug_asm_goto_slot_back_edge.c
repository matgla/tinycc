/*
 *  TCC IR - frame slots carried around an asm goto back edge
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* An asm goto to a label above it is the loop's only back edge.  Flat and
 * SSA passes that find loops from JUMP/JUMPIF back edges alone must still see
 * the slot writes in the loop: opt:dead_local_slot deleted the store of a
 * struct returned in r0/r1 (`carried_struct`), known_bits and ssa:sccp
 * folded the loop test to the value stored before the loop (`carried_array`,
 * `bytes`, `copied`). */

#include <stdio.h>
#include <string.h>
#define NI __attribute__((noinline))
struct pair { unsigned v; unsigned char done; };

NI static struct pair step(unsigned x)
{
  struct pair r;
  r.v = x + 1;
  r.done = x + 1 >= 4;
  return r;
}

NI unsigned bump(unsigned x) { return x + 1; }

/* A struct returned in r0/r1 is stored into the loop-carried local; the asm
 * goto is the loop's only back edge. */
NI int carried_struct(void)
{
  int n = 0;
  struct pair b = step(0);
top:
  if (b.done || n > 50)
    return n;
  n++;
  b = step(b.v);
  __asm__ goto("b %l0" : : : : top);
  return 1000;
}

/* The same through an address-taken array local. */
NI int carried_array(void)
{
  unsigned a[2];
  int n = 0;
  a[0] = 0;
  a[1] = 0;
  unsigned *volatile keep = a;
  (void)keep;
top:
  if (a[1] >= 4 || n > 50)
    return n * 10 + (int)a[0];
  n++;
  a[1] = bump(a[1]);
  a[0] = a[1] + 1;
  __asm__ goto("b %l0" : : : : top);
  return 1000;
}

/* A struct field written before a conditional backward asm goto. */
NI int carried_field(int lim)
{
  struct pair p;
  p.v = 0;
  p.done = 0;
again:
  p.v = bump(p.v);
  __asm__ goto("cmp %0, %1\n\tblt %l2" : : "r"(p.v), "r"(lim) : "cc" : again);
  return (int)p.v;
}
NI void touch(unsigned *p) { (void)p; }
int g;
struct big { unsigned a[6]; };

/* global carried by an asm-goto back edge */
NI int glob(void)
{
  int n = 0;
  g = 0;
top:
  if (g >= 4 || n > 50)
    return n * 10 + g;
  n++;
  g = g + 1;
  __asm__ goto("b %l0" : : : : top);
  return 1000;
}

/* address-taken scalar whose address escaped before the loop */
NI int escaped(void)
{
  unsigned x = 0;
  int n = 0;
  touch(&x);
top:
  if (x >= 4 || n > 50)
    return n * 10 + (int)x;
  n++;
  x = bump(x);
  __asm__ goto("b %l0" : : : : top);
  return 1000;
}

/* byte array updated with memset inside the loop */
NI int bytes(void)
{
  unsigned char b[8];
  int n = 0;
  memset(b, 0, sizeof b);
top:
  if (b[3] >= 4 || n > 50)
    return n * 10 + b[7];
  n++;
  memset(b, b[3] + 1, sizeof b);
  __asm__ goto("b %l0" : : : : top);
  return 1000;
}

/* struct copied whole inside the loop */
NI int copied(void)
{
  struct big s, t;
  int n = 0;
  memset(&s, 0, sizeof s);
top:
  if (s.a[5] >= 4 || n > 50)
    return n * 10 + (int)s.a[0];
  n++;
  t = s;
  t.a[5] = bump(t.a[5]);
  t.a[0] = t.a[5] * 2;
  s = t;
  __asm__ goto("b %l0" : : : : top);
  return 1000;
}

/* overwritten store: x = 2 is read only after the back edge */
NI int overwrite(int lim)
{
  unsigned x = 1, sum = 0;
  int n = 0;
  unsigned *volatile k = &x;
  (void)k;
top:
  sum += x;
  x = 2;
  n++;
  __asm__ goto("cmp %0, %1\n\tblt %l2" : : "r"(n), "r"(lim) : "cc" : top);
  return (int)sum;
}

int main(void)
{
  printf("struct %d array %d field %d\n", carried_struct(), carried_array(), carried_field(5));
  printf("glob %d escaped %d bytes %d copied %d overwrite %d\n", glob(), escaped(), bytes(), copied(), overwrite(4));
  return 0;
}
