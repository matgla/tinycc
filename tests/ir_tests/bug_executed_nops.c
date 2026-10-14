/* Executed NOPs: jumps over code-free phi copies and fall-through alignment pads.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* codegen: a JUMP whose span holds only ops the rehearsal laid out as zero
 * bytes is dropped instead of being patched to a NOP, and a word-alignment pad
 * reached by fall-through widens the 16-bit instruction before it. */
#include <stdint.h>
#include <stdio.h>

__attribute__((noinline)) int32_t sel(int32_t x)
{
  return (x & 0x80000000u) ? (int32_t)(x | 0x80000000u) : (x & 0x7fffffff);
}

__attribute__((noinline)) int32_t sel2(int32_t c, int32_t x)
{
  return c ? (x | 0x100) : (x & 0xff);
}

/* The join is not a return: the merged value feeds more code. */
__attribute__((noinline)) int32_t sel3(int32_t c, int32_t x, int32_t y)
{
  int32_t v = c > 3 ? (x ^ 0x55) : (x + y);
  return v * 3 + y;
}

/* Loop head right after a 16-bit counter init (uaccess_access_ok). */
__attribute__((noinline)) int count_le(const uint32_t *p, int n, uint32_t lim)
{
  int k = 0;
  for (int i = 0; i < n; i++)
    if (p[i] <= lim)
      k++;
  return k;
}

__attribute__((noinline)) uint32_t sum_bytes(const uint8_t *p, int n)
{
  uint32_t s = 0;
  for (int i = 0; i < n; i++)
    s += p[i];
  return s;
}

__attribute__((noinline)) void copy_bytes(uint8_t *d, const uint8_t *s, unsigned n)
{
  while (n--)
    *d++ = *s++;
}

__attribute__((noinline)) void fill_words(uint32_t *d, uint32_t v, int n)
{
  for (int i = 0; i < n; i++)
    d[i] = v + (uint32_t)i;
}

/* An in-loop join (TmpMemoryPool.next_bit). */
__attribute__((noinline)) int next_bit(const uint32_t *words, int nbits, int from)
{
  for (int b = from; b < nbits; b++)
  {
    uint32_t w = words[b >> 5];
    int set = (w >> (b & 31)) & 1;
    if (set)
      return b;
  }
  return -1;
}

__attribute__((noinline)) int mixed(const int16_t *p, int n, int bias)
{
  int acc = bias;
  for (int i = 0; i < n; i++)
  {
    int v = p[i];
    if (v < 0)
      v = -v;
    else
      v = v >> 1;
    acc += v;
  }
  return acc;
}

__attribute__((noinline)) uint32_t trim(const uint8_t *s, uint32_t len)
{
  while (len > 0 && s[len - 1] == ' ')
    len--;
  return len;
}

int main(void)
{
  static const uint32_t words[] = {0, 0x100, 0, 0x80000000u};
  static const uint32_t vals[] = {1, 9, 3, 7, 5, 11};
  static const uint8_t bytes[] = {1, 2, 3, 250, 5};
  static const int16_t shorts[] = {-4, 6, -8, 9, 0, -1};
  uint8_t dst[5] = {0};
  uint32_t filled[4];

  printf("sel %ld %ld %ld\n", (long)sel(5), (long)sel((int32_t)0x80000003u), (long)sel(-1));
  printf("sel2 %ld %ld\n", (long)sel2(0, 0x1234), (long)sel2(1, 0x1234));
  printf("sel3 %ld %ld\n", (long)sel3(4, 7, 2), (long)sel3(1, 7, 2));
  printf("count_le %d %d %d\n", count_le(vals, 6, 5), count_le(vals, 0, 5), count_le(vals, 6, 100));
  printf("sum_bytes %lu %lu\n", (unsigned long)sum_bytes(bytes, 5), (unsigned long)sum_bytes(bytes, 0));
  copy_bytes(dst, bytes, 5);
  printf("copy_bytes %u %u %u\n", dst[0], dst[3], dst[4]);
  fill_words(filled, 10, 4);
  printf("fill_words %lu %lu\n", (unsigned long)filled[0], (unsigned long)filled[3]);
  printf("next_bit %d %d %d\n", next_bit(words, 128, 0), next_bit(words, 128, 41), next_bit(words, 100, 41));
  printf("mixed %d %d\n", mixed(shorts, 6, 1), mixed(shorts, 0, 7));
  printf("trim %lu %lu\n", (unsigned long)trim((const uint8_t *)"ab  ", 4), (unsigned long)trim((const uint8_t *)"   ", 3));
  return 0;
}
