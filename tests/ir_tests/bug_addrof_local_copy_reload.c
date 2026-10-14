/*
 *  TCC IR - A locally built u64 read back through its address
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* The Zig C backend spells tuple destructuring as
 *   t8 = a; t11 = (uint64_t const *)&t8; x = *t11;
 * (IntegerContext(u16).hash in the kernel, docs/bugs/
 * kernel-hash-map-u16-key-mix-emulation.md): the key pair was built in
 * registers, took a `strd r,r,[sp,#N]` + `add r,sp,#N` + `ldrd r,r,[r]`
 * stack round-trip before the mix, because ptr_local_fwd only resolved
 * pointer copies spelled as slot reads (`T <-- P` lval), never the value
 * copy `T <-- P`, and had no 64-bit template for the pointee anyway.
 *
 * Shapes that must keep the slot: the address reaches a call (the callee may
 * write through it), and a pointer that is also read as an ordinary value. */

#include <stdint.h>
#include <stdio.h>

#define zig_noinline __attribute__((noinline))

static const uint64_t s0 = UINT64_C(0xa0761d6478bd642f);
static const uint64_t s1 = UINT64_C(0xe7037ed1a0b428db);

/* 64x64 -> 128 multiply the way the IR lowers it (four 32-bit products).
 * Kept out of line: once ssa:fold drops its zero cross products it is a small
 * leaf, and two inlined copies spill for register pressure, which is not the
 * key pair's round-trip this test is about. */
zig_noinline static uint64_t mix64(uint64_t a, uint64_t b)
{
  uint64_t lo = (uint64_t)(uint32_t)a * (uint32_t)b;
  uint64_t mid1 = ((a >> 32) * (uint32_t)b) + (lo >> 32);
  uint64_t mid2 = (uint32_t)a * (b >> 32);
  uint64_t hi = ((a >> 32) * (b >> 32)) + (mid1 >> 32) + (mid2 >> 32);
  return (lo ^ hi ^ s0) + (mid1 ^ mid2 ^ s1);
}

/* The defect site: the u64 key is built in a local and read back through
 * its address, the value-copy pointer hand-off the fix resolves. */
zig_noinline uint64_t hash_key(uint64_t key, uint64_t seed)
{
  uint64_t t8 = key;
  uint64_t const *t11 = &t8;
  uint64_t a = (*t11) ^ s1;
  uint64_t b = seed ^ s0;
  return mix64(a, b) ^ mix64(b, a);
}

/* Must stay a memory round-trip: the address reaches a call. */
zig_noinline static uint64_t through_ptr(const uint64_t *p)
{
  return *p + UINT64_C(3);
}

zig_noinline uint64_t hash_escapes(uint64_t key, uint64_t seed)
{
  uint64_t t8 = key;
  uint64_t const *t11 = &t8;
  uint64_t x = mix64(*t11, seed);
  return x + through_ptr(t11);
}

/* The pointer itself read as a value: no deref rewrite may apply to it. */
zig_noinline uint64_t pointer_value(uint64_t key)
{
  uint64_t t8 = key;
  uint64_t const *t11 = &t8;
  uintptr_t bits = (uintptr_t)t11;
  return (*t11) + (uint64_t)(bits & 1);
}

int main(void)
{
  printf("%llu %llu %llu %llu\n",
         (unsigned long long)hash_key(UINT64_C(0x123456789abcdef0), UINT64_C(42)),
         (unsigned long long)hash_key(UINT64_C(0), UINT64_C(0)),
         (unsigned long long)hash_escapes(UINT64_C(99), UINT64_C(7)),
         (unsigned long long)pointer_value(UINT64_C(0xdeadbeefcafebabe)));
  return 0;
}
