/*
 *  TCC IR - signed narrow reload of a zero-extended stored value
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* sl_forward forwarded a stored value into a same-width narrow load as it
 * stands when the value fit the width unsigned: a struct copy reads the i16
 * field as a u16 and stores that, so the signed reload of the copy's field
 * came back zero-extended (-12345 * 3 printed 159573).  A signed reload now
 * forwards as it stands only a value below the sign bit, else as an SBFX. */

#include <stdint.h>
#include <stdio.h>

struct pair { int16_t lo; int8_t b; };

__attribute__((noinline)) int32_t field_copy(int16_t v)
{
  struct pair a, b;
  a.lo = v;
  b = a;
  return (int32_t)b.lo * 3;
}

struct bytes { int8_t x; int8_t y; uint8_t z; };

__attribute__((noinline)) int32_t byte_copy(int8_t x, int8_t y, uint8_t z)
{
  struct bytes a, b;
  a.x = x;
  a.y = y;
  a.z = z;
  b = a;
  return (int32_t)b.x * 1000 + (int32_t)b.y * 10 + (int32_t)b.z;
}

/* A masked value below the sign bit reads back the same either way. */
__attribute__((noinline)) int32_t masked_store(uint32_t v)
{
  struct pair a;
  a.lo = (int16_t)(v & 0x7fff);
  a.b = (int8_t)(v & 0x7f);
  return (int32_t)a.lo + (int32_t)a.b;
}

/* ... and one with the sign bit set does not. */
__attribute__((noinline)) int32_t masked_sign(uint32_t v)
{
  struct pair a;
  a.lo = (int16_t)(v & 0xffff);
  a.b = (int8_t)(v & 0xff);
  return (int32_t)a.lo + (int32_t)a.b;
}

int main(void)
{
  printf("field_copy %d %d %d %d\n", (int)field_copy(-12345), (int)field_copy(32767), (int)field_copy(-32768),
         (int)field_copy(-1));
  printf("byte_copy %d %d\n", (int)byte_copy(-128, -1, 255), (int)byte_copy(127, -100, 7));
  printf("masked_store %d %d\n", (int)masked_store(0xffffffffu), (int)masked_store(0x12345u));
  printf("masked_sign %d %d\n", (int)masked_sign(0xffffffffu), (int)masked_sign(0x18085u));
  return 0;
}
