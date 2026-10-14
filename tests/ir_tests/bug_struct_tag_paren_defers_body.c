/*
 *  TCC Tests - A struct tag before '(' is not an unprototyped call
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* decl.c body_calls_unprototyped took `struct errunion (*fp)(...)` -- the tag
 * before '(' -- for a call of an undeclared function, so the body was
 * generated at its definition instead of deferred: callees defined further
 * down (Zig's access_ok after _irq_svcall) were never inlined into it. */
#include <stdint.h>
#include <stdio.h>

struct errunion { int32_t payload; uint16_t error; };
union word { uint32_t u; int32_t i; };
enum kind { K_A, K_B };

static int small_check(uintptr_t p, uintptr_t n);
static struct errunion handler_a(void const *arg);
static struct errunion handler_b(void const *arg);

static struct errunion (*const table[2])(void const *) = { handler_a, handler_b };

/* Defined before its callees; every '(' after a tag is a declarator. */
int dispatch(uint32_t nr, void const *arg)
{
  struct errunion (*fp)(void const *);
  union word (*unused_u)(void) = 0;
  enum kind (*unused_e)(void) = 0;
  struct errunion r;
  if (!small_check((uintptr_t)arg, 8))
    return -1;
  if (unused_u || unused_e)
    return -2;
  fp = (struct errunion (*)(void const *))table[nr & 1];
  r = fp(arg);
  return r.error ? -(int)r.error : r.payload;
}

static int small_check(uintptr_t p, uintptr_t n)
{
  return p != 0 && p + n > p;
}

static struct errunion handler_a(void const *arg)
{
  struct errunion r = { *(int32_t const *)arg + 1, 0 };
  return r;
}

static struct errunion handler_b(void const *arg)
{
  struct errunion r = { 0, (uint16_t)(*(int32_t const *)arg & 0xff) };
  return r;
}

int main(void)
{
  int32_t v = 41, w = 7;
  printf("%d\n", dispatch(0, &v));
  printf("%d\n", dispatch(1, &w));
  printf("%d\n", dispatch(0, 0));
  printf("%d\n", dispatch(3, &v));
  return 0;
}
