/*
 *  TCC IR - Return-copy elision through a borrowed header initializer
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>

struct Header { unsigned words[40]; };
struct Result { struct Header payload; unsigned short error; };
static struct Header *saved;
struct Borrowed { struct Header *ptr; unsigned count; };

__attribute__((noinline)) static int fill_header(struct Header *p, unsigned seed)
{
  volatile struct Borrowed binding;
  binding.ptr = p;
  for (unsigned i = 0; i < 40; ++i)
    binding.ptr->words[i] = seed + i * 13;
  return seed == 0;
}

__attribute__((noinline)) struct Result make_header(unsigned seed)
{
  struct Header local;
  struct Result result;
  int error = fill_header(&local, seed);
  if (error) {
    result.payload = (struct Header){{0}};
    result.error = 7;
  } else {
    result.payload = local;
    result.error = 0;
  }
  return result;
}

__attribute__((noinline)) static void capture_header(struct Header *p)
{
  volatile struct Borrowed binding;
  binding.ptr = p;
  binding.count = 40;
  saved = binding.ptr;
  p->words[0] = 17;
}

__attribute__((noinline)) struct Result capture_keeps_distinct_storage(unsigned seed)
{
  struct Header local = {{0}};
  capture_header(&local);
  struct Result result;
  result.payload = local;
  result.payload.words[0] = 99;
  result.payload.words[1] = saved->words[0] + seed;
  result.error = 0;
  return result;
}


int main(void)
{
  for (unsigned seed = 0; seed < 10; ++seed) {
    struct Result r = make_header(seed);
    if (r.error != (seed ? 0 : 7))
      return 1;
    for (unsigned i = 0; i < 40; ++i)
      if (r.payload.words[i] != (seed ? seed + i * 13 : 0))
        return 2;
    struct Result captured = capture_keeps_distinct_storage(seed);
    if (captured.payload.words[0] != 99 || captured.payload.words[1] != 17 + seed)
      return 3;
  }
  puts("borrowed header: ok");
  return 0;
}
