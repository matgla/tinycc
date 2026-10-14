/*
 *  TCC Tests - Folded stack addresses keep a _Complex operand's width
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* lea_fold rewrote `T = &z; PARAM T***DEREF***` into a StackLoc without the
 * operand's complex flag, so a _Complex float member passed by value loaded
 * only its real half.  The address-folding rewrites now copy is_complex. */
#include <complex.h>
#include <stdio.h>

typedef struct { _Complex float z; } Z;
typedef struct { int tag; _Complex double d; } D;
typedef struct { int n; _Complex float a[3]; } A;

#define P(x) ((int)((x) * 10))

__attribute__((noinline)) Z mkz(float r, float i) { Z z; __real__ z.z = r; __imag__ z.z = i; return z; }
__attribute__((noinline)) float via_ptr(Z *p) { return cimagf(p->z); }
__attribute__((noinline)) float im(_Complex float c) { return cimagf(c); }
__attribute__((noinline)) double imd(_Complex double c) { return cimag(c); }
__attribute__((noinline)) _Complex float id(_Complex float c) { return c; }
__attribute__((noinline)) void sink(void *p) { (void)p; }

static void member_by_value(void)
{
  Z z = mkz(1.5f, -2.5f);
  printf("%d %d %d\n", P(crealf(z.z)), P(cimagf(z.z)), P(via_ptr(&z)));
  printf("%d\n", P(im(z.z)));
}

static void double_member(void)
{
  D d;
  d.tag = 7;
  __real__ d.d = 3.25;
  __imag__ d.d = -4.5;
  printf("%d %d %d\n", d.tag, P(creal(d.d)), P(imd(d.d)));
}

static void member_copy(void)
{
  Z z = mkz(2.0f, 6.5f);
  _Complex float w = z.z;
  _Complex float r = id(w);
  printf("%d %d\n", P(crealf(r)), P(cimagf(r)));
}

static void member_store_through_pointer(void)
{
  Z z = mkz(0.0f, 0.0f);
  Z *p = &z;
  p->z = id(1.0f + 9.5f * I);
  printf("%d %d\n", P(crealf(z.z)), P(cimagf(z.z)));
}

static void array_element(void)
{
  A a;
  a.n = 3;
  for (int i = 0; i < 3; i++)
    a.a[i] = id((float)i + (float)(i + 10) * I);
  printf("%d %d %d\n", a.n, P(im(a.a[1])), P(im(a.a[2])));
}

static void compound_assign(void)
{
  Z z = mkz(1.0f, 2.0f);
  z.z += id(0.5f + 0.25f * I);
  z.z *= 2.0f;
  printf("%d %d\n", P(crealf(z.z)), P(cimagf(z.z)));
}

static void escaped(void)
{
  Z z = mkz(-1.5f, 8.0f);
  sink(&z);
  printf("%d %d\n", P(im(z.z)), P(cimagf(id(z.z))));
}

/* Neighbouring scalar members still fold to direct stack reads. */
typedef struct { int a; float f; short s; } S;
__attribute__((noinline)) int sum(int a, float f, short s) { return a + (int)f + s; }
static void scalar_members(void)
{
  S s = {40, 1.5f, -3};
  printf("%d\n", sum(s.a, s.f, s.s));
}

int main(void)
{
  member_by_value();
  double_member();
  member_copy();
  member_store_through_pointer();
  array_element();
  compound_assign();
  escaped();
  scalar_members();
  return 0;
}
