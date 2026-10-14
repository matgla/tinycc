/*
 *  TCC regression - zig.h full-width signed truncate is the identity
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* `(x & 1<<(N-1)) != 0 ? x | minInt(N) : x & maxInt(N)` at N == width kept the
 * sign test and both arms.  ssa:vrp now bounds x on the branch edge and folds a
 * mask/extract that is a no-op over that range to x; phi_simplify sees the phi
 * of two copies of x; ssa:branch drops the branch whose edges then converge.
 * The narrower truncates below must keep their real sign extension. */
#include <stdint.h>
#include <stdio.h>

#define TRUNC(sw, uw, arg, bits)                                                        \
  ((((uw)(arg) & (uw)((uw)1 << ((bits) - 1))) != 0)                                     \
       ? (sw)((sw)(arg) | (sw)((sw)((uw)1 << (sizeof(sw) * 8 - 1)) >> (sizeof(sw) * 8 - (bits)))) \
       : (sw)((sw)(arg) & (sw)((sw)(((uw)1 << (sizeof(sw) * 8 - 1)) - 1) >> (sizeof(sw) * 8 - (bits)))))

#define trunc_i32(a, bits) TRUNC(int32_t, uint32_t, a, bits)
#define trunc_i16(a, bits) TRUNC(int16_t, uint16_t, a, bits)
#define trunc_i8(a, bits) TRUNC(int8_t, uint8_t, a, bits)
#define trunc_i64(a, bits) TRUNC(int64_t, uint64_t, a, bits)

__attribute__((noinline)) int32_t id32(int32_t a) { return trunc_i32(a, 32); }
__attribute__((noinline)) int16_t id16(int16_t a) { return trunc_i16(a, 16); }
__attribute__((noinline)) int8_t id8(int8_t a) { return trunc_i8(a, 8); }
__attribute__((noinline)) int64_t id64(int64_t a) { return trunc_i64(a, 64); }
/* Narrow values loaded from memory arrive already extended. */
__attribute__((noinline)) int16_t ld16_id(const int16_t *p) { int16_t a = *p; return trunc_i16(a, 16); }
__attribute__((noinline)) int8_t ld8_id(const int8_t *p) { int8_t a = *p; return trunc_i8(a, 8); }

/* Not the identity: the sign bit sits below the register width. */
__attribute__((noinline)) int32_t t32_31(int32_t a) { return trunc_i32(a, 31); }
__attribute__((noinline)) int32_t t32_16(int32_t a) { return trunc_i32(a, 16); }
__attribute__((noinline)) int32_t t32_8(int32_t a) { return trunc_i32(a, 8); }
__attribute__((noinline)) int32_t t32_1(int32_t a) { return trunc_i32(a, 1); }
__attribute__((noinline)) int16_t t16_8(int16_t a) { return trunc_i16(a, 8); }
__attribute__((noinline)) int8_t t8_4(int8_t a) { return trunc_i8(a, 4); }
__attribute__((noinline)) int64_t t64_32(int64_t a) { return trunc_i64(a, 32); }
__attribute__((noinline)) int64_t t64_63(int64_t a) { return trunc_i64(a, 63); }

/* Same sign test, but an arm mask that is not a no-op over its half. */
__attribute__((noinline)) int32_t wide_or(int32_t a)
{
  return ((uint32_t)a & 0x80000000u) ? (a | (int32_t)0xC0000000u) : (a & 0x7fffffff);
}
__attribute__((noinline)) int32_t narrow_and(int32_t a)
{
  return ((uint32_t)a & 0x80000000u) ? (a | (int32_t)0x80000000u) : (a & 0x3fffffff);
}
/* A sign-extended 16-bit value tested on bit 15: full width for its range. */
__attribute__((noinline)) int32_t sext16_id(int32_t a)
{
  int32_t s = (int16_t)a;
  return (s & 0x8000) ? (s | (int32_t)0xffff8000u) : (s & 0x7fff);
}
/* ... but bit 14 of that value is not its sign. */
__attribute__((noinline)) int32_t sext16_bit14(int32_t a)
{
  int32_t s = (int16_t)a;
  return (s & 0x4000) ? (s | (int32_t)0xffffc000u) : (s & 0x3fff);
}

struct result { int32_t result; int32_t err; };
struct errunion { int32_t payload; uint16_t error; };

/* interrupts.system_call.write_result, as the Zig C backend emits it. */
__attribute__((noinline)) intptr_t write_result(void volatile *p, struct errunion e)
{
  struct result volatile *r = (struct result volatile *)p;
  if (e.error != 0)
  {
    r->err = e.error;
    r->result = -1;
    return -1;
  }
  r->result = trunc_i32(e.payload, 32);
  r->err = -1;
  return trunc_i32(e.payload, 32);
}

static volatile int32_t v32[] = { 0, 1, -1, 0x7fffffff, (int32_t)0x80000000u, 0x7fff, 0x8000, -32768,
                                  0x12345678, (int32_t)0x87654321u, 0x80, 0x7f, -128, 0x4000, 0xc000,
                                  (int32_t)0x40000001u, (int32_t)0xbfffffffu };
static volatile int64_t v64[] = { 0, 1, -1, 0x7fffffffffffffffLL, (int64_t)0x8000000000000000ULL,
                                  0x80000000LL, 0x7fffffffLL, 0x123456789abcdefLL,
                                  (int64_t)0xfedcba9876543210ULL, (int64_t)0x4000000080000000ULL };

int main(void)
{
  unsigned n32 = sizeof(v32) / sizeof(v32[0]);
  for (unsigned i = 0; i < n32; i++)
  {
    int32_t a = v32[i];
    printf("%08x: id32 %08x id16 %08x id8 %08x | 31 %08x 16 %08x 8 %08x 1 %08x\n", (unsigned)a,
           (unsigned)id32(a), (unsigned)(int32_t)id16((int16_t)a), (unsigned)(int32_t)id8((int8_t)a),
           (unsigned)t32_31(a), (unsigned)t32_16(a), (unsigned)t32_8(a), (unsigned)t32_1(a));
    printf("  t16_8 %08x t8_4 %08x wide_or %08x narrow_and %08x sext16 %08x bit14 %08x\n",
           (unsigned)(int32_t)t16_8((int16_t)a), (unsigned)(int32_t)t8_4((int8_t)a),
           (unsigned)wide_or(a), (unsigned)narrow_and(a), (unsigned)sext16_id(a),
           (unsigned)sext16_bit14(a));
    int16_t h = (int16_t)a;
    int8_t b = (int8_t)a;
    printf("  ld16 %08x ld8 %08x\n", (unsigned)(int32_t)ld16_id(&h), (unsigned)(int32_t)ld8_id(&b));
  }
  unsigned n64 = sizeof(v64) / sizeof(v64[0]);
  for (unsigned i = 0; i < n64; i++)
  {
    int64_t a = v64[i];
    printf("%016llx: id64 %016llx 32 %016llx 63 %016llx\n", (unsigned long long)a,
           (unsigned long long)id64(a), (unsigned long long)t64_32(a), (unsigned long long)t64_63(a));
  }
  struct result r = { 0, 0 };
  struct errunion ok = { (int32_t)0x80000005u, 0 }, bad = { 7, 3 };
  intptr_t ret = write_result(&r, ok);
  printf("ok: ret %08x result %08x err %d\n", (unsigned)ret, (unsigned)r.result, (int)r.err);
  ret = write_result(&r, bad);
  printf("bad: ret %d result %d err %d\n", (int)ret, (int)r.result, (int)r.err);
  ok.payload = 0x7ffffff0;
  ret = write_result(&r, ok);
  printf("pos: ret %08x result %08x err %d\n", (unsigned)ret, (unsigned)r.result, (int)r.err);
  return 0;
}
