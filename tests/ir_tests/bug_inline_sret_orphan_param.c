/*
 *  TCC IR - inlined struct-return call leaves no sret parameter behind
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Inlining a struct-returning call NOPed the call's argument parameters but
 * not the sret pointer emitted before them (frontend builtin/call.c), so the
 * buffer's address stayed passed to a call that no longer existed and its
 * frame slot survived: an 8-byte `sub sp` in a body that never touches it
 * (the Zig C backend's interface vcall thunks). */

#include <stdint.h>
#include <stdio.h>

struct slice { const uint8_t *ptr; uintptr_t len; };
struct ramfs { int id; };
struct tuple { const struct ramfs *f0; };
struct big { uint32_t w[5]; };

static const uint8_t name_bytes[5] = {'r', 'a', 'm', 'f', 's'};

static struct slice ramfs_name(const struct ramfs *const self)
{
  (void)self;
  return (struct slice){name_bytes, 5};
}

/* The CBE's gen_vcall thunk: the inlined callee's result copied out twice. */
__attribute__((noinline)) struct slice vcall_name(const void *const a0)
{
  struct slice t0, t3;
  const struct ramfs *t1;
  struct tuple t2;
  t1 = (const struct ramfs *)a0;
  t2.f0 = t1;
  t1 = t2.f0;
  t3 = ramfs_name(t1);
  t0 = t3;
  return t0;
}

static struct big make_big(uint32_t seed)
{
  struct big b;
  for (int i = 0; i < 5; i++)
    b.w[i] = seed * (uint32_t)(i + 1);
  return b;
}

/* The inlined result is read back field by field: the buffer must still be
 * filled even with the sret parameter gone. */
__attribute__((noinline)) uint32_t big_sum(uint32_t seed)
{
  struct big b = make_big(seed);
  return b.w[0] + b.w[1] + b.w[2] + b.w[3] + b.w[4];
}

__attribute__((noinline)) struct big big_copy(uint32_t seed)
{
  return make_big(seed);
}

int main(void)
{
  struct ramfs fs = {7};
  struct slice s = vcall_name(&fs);
  printf("%.*s %u\n", (int)s.len, (const char *)s.ptr, (unsigned)s.len);
  printf("sum %u\n", (unsigned)big_sum(3));
  struct big b = big_copy(11);
  printf("copy %u %u %u %u %u\n", (unsigned)b.w[0], (unsigned)b.w[1], (unsigned)b.w[2], (unsigned)b.w[3],
         (unsigned)b.w[4]);
  return 0;
}
