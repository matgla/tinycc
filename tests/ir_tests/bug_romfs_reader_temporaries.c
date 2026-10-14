/*
 *  TCC IR - ROMFS reader aggregate temporary regression
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct device
{
  uint32_t words[8];
};
struct reader
{
  uint64_t offset, data_offset;
  struct device device;
};
struct header
{
  struct reader reader;
  struct device device;
};
__attribute__((noinline)) void build_reader(struct header *self, int offset, uint64_t data_offset)
{
  struct reader result, copy;
  struct device d = self->device;
  result.device.words[0] = d.words[0];
  result.device.words[1] = d.words[1];
  result.device.words[2] = d.words[2];
  result.device.words[3] = d.words[3];
  result.device.words[4] = d.words[4];
  result.device.words[5] = d.words[5];
  result.device.words[6] = d.words[6];
  result.device.words[7] = d.words[7];

  result.offset = (int64_t)offset;
  result.data_offset = data_offset;
  copy = result;
  self->reader = copy;
}

struct slice
{
  const unsigned char *ptr;
  unsigned len;
};
__attribute__((noinline)) unsigned mapped_slice(const unsigned char *base, unsigned len, unsigned at)
{
  struct slice image;
  if (base)
  {
    image.ptr = base;
    image.len = len;
  }
  else
  {
    image.ptr = 0;
    image.len = 0xaaaaaaaa;
  }
  if (!image.ptr)
    return 0;
  struct slice img = image;
  if (at + 4 > img.len)
    return 0;
  unsigned word;
  memcpy(&word, img.ptr + at, 4);
  return word;
}

__attribute__((noinline)) int compare_updated_word(unsigned word, unsigned limit)
{
  union
  {
    unsigned word;
    unsigned char bytes[4];
  } value;
  value.word = word;
  value.bytes[0] = 7;
  return value.word == limit;
}

__attribute__((noinline)) int compare_volatile(unsigned word, unsigned limit)
{
  struct
  {
    volatile unsigned word;
    unsigned limit;
  } value;
  value.word = word;
  value.limit = limit;
  return value.word == value.limit && value.word == value.limit;
}

int main(void)
{
  struct header h;
  for (int i = 0; i < 8; i++)
    h.device.words[i] = 0x12340000u + i;
  for (int offset = -3; offset <= 3; offset++)
  {
    uint64_t data = UINT64_C(0x123456789abcdef0) + offset;
    build_reader(&h, offset, data);
    if (h.reader.offset != (uint64_t)(int64_t)offset || h.reader.data_offset != data)
      return 1;
    for (int i = 0; i < 8; i++)
      if (h.reader.device.words[i] != h.device.words[i] || h.device.words[i] != 0x12340000u + i)
        return 2;
  }
  unsigned words[2] = {0x12345678, 0xabcdef01};
  const unsigned char *bytes = (const unsigned char *)words;
  if (mapped_slice(0, 8, 0) != 0 || mapped_slice(bytes, 3, 0) != 0 || mapped_slice(bytes, 8, 5) != 0 ||
      mapped_slice(bytes, 8, 0) != words[0] || mapped_slice(bytes, 8, 4) != words[1])
    return 3;
  if (!compare_updated_word(0x12345678, 0x12345607) || compare_updated_word(0x12345678, 0x12345678))
    return 4;
  if (!compare_volatile(42, 42) || compare_volatile(42, 43))
    return 5;
  puts("reader copies OK");
  return 0;
}
