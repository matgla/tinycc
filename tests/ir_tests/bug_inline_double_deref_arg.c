/*
 *  TCC Tests - Inlined double parameter bound from a memory argument
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* An inline expansion binds each parameter with a STORE of the argument; a
 * 64-bit lvalue argument (`*p`, `s->d`) must be LOADed into a temp first, since
 * the 64-bit STORE cannot split a DEREF source (mach_make_hi_half ICE at -O1+).
 * call.c did that for long long only: double and long double now get it too. */
#include <stdio.h>

struct value
{
  int type;
  double number;
  long double wide;
};

static long long seen_bits;
static int seen_calls;

__attribute__((noinline)) void sink_double(double d)
{
  seen_calls++;
  seen_bits = (long long)(d * 1000.0);
}

__attribute__((noinline)) void sink_two(int k, double d)
{
  seen_calls++;
  seen_bits = k * 1000000LL + (long long)(d * 10.0);
}

/* each helper is static with exactly one call: -finline-functions-called-once */
static void from_ptr(double n) { sink_double(n); }
static void from_member(double n) { sink_double(n + 1.0); }
static void from_index(double n) { sink_double(n); }
static void from_wide(long double n) { sink_double((double)n); }
static void mixed(int k, double n) { sink_two(k, n); }
static double modified(double n)
{
  n = n * 2.0;
  return n;
}
static void from_llong(long long n) { sink_double((double)n); }
static void from_float(float n) { sink_double(n); }
static void by_value(double n) { sink_double(n); }

static void write_number(const struct value *v)
{
  if (v->number == (double)(long)v->number)
    printf("int %ld\n", (long)v->number);
  else
    printf("frac %ld\n", (long)(v->number * 100.0));
}

static void write_value(const struct value *v)
{
  switch (v->type)
  {
  case 3:
    write_number(v);
    break;
  default:
    printf("other\n");
    break;
  }
}

int main(void)
{
  volatile double storage[3] = {1.5, -2.25, 12345.125};
  double *p = (double *)storage;
  struct value v = {3, 41.5, 7.75L};
  struct value whole = {3, 42.0, 0.0L};
  long long ll = -9;
  float f = 0.5f;
  double direct = 3.0;
  int i = 2;

  from_ptr(*p);
  printf("ptr %lld\n", seen_bits);
  from_member(v.number);
  printf("member %lld\n", seen_bits);
  from_index(p[i]);
  printf("index %lld\n", seen_bits);
  from_wide(v.wide);
  printf("wide %lld\n", seen_bits);
  mixed(7, p[1]);
  printf("mixed %lld\n", seen_bits);
  double twice = modified(p[0]);
  printf("modified %ld source %ld\n", (long)(twice * 100.0), (long)(p[0] * 100.0));
  from_llong(ll);
  printf("llong %lld\n", seen_bits);
  from_float(f);
  printf("float %lld\n", seen_bits);
  by_value(direct);
  printf("value %lld\n", seen_bits);
  write_value(&v);
  write_value(&whole);
  printf("calls %d\n", seen_calls);
  return 0;
}
