/*
 *  TCC IR - Address-taken byte and halfword probe locals
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

__attribute__((noinline)) bool metadata_used(uint8_t a0)
{
  uint8_t t0;
  memset(&t0, 0, sizeof(t0));
  t0 = a0;
  const uint8_t *t1 = &t0;
  const uint8_t *t2 = t1;
  const uint8_t *t3 = t2;
  uint8_t t4 = *t3;
  return (t4 >> 7) == 1;
}

__attribute__((noinline)) int signed_byte(int a)
{
  int8_t v = (int8_t)a;
  const int8_t *p = &v;
  return *p;
}

__attribute__((noinline)) int signed_half(int a)
{
  int16_t v = (int16_t)a;
  const int16_t *p = &v;
  return *p;
}

__attribute__((noinline)) unsigned half_store(unsigned a)
{
  uint16_t v = (uint16_t)a;
  uint16_t *p = &v;
  *p += 19;
  return v + *p;
}

__attribute__((noinline)) unsigned volatile_byte(uint8_t a)
{
  volatile uint8_t v = a;
  const volatile uint8_t *p = &v;
  return *p + *p;
}

int main(void)
{
  for (int i = 0; i < 256; i++)
    if (metadata_used((uint8_t)i) != (i >= 128) || signed_byte(i) != (i < 128 ? i : i - 256) ||
        volatile_byte((uint8_t)i) != (unsigned)(2 * i))
      return 1;
  for (int i = -70000; i < 70000; i += 137)
    if (signed_half(i) != (int16_t)i || half_store((unsigned)i) != 2u * (uint16_t)(i + 19))
      return 2;
  puts("narrow locals: ok");
  return 0;
}
