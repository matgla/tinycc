/* __builtin_{add,sub,mul}_overflow{,_p} with operands whose sign or width
 * differs from the result: the check must use the infinite-precision value of
 * the operands' own types, not operands already wrapped to the result type.
 *
 * Copyright (c) 2001-2004 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <stdint.h>
#include <stdio.h>

typedef int8_t i8;
typedef uint16_t u16;
typedef int32_t i32;
typedef uint32_t u32;
typedef int64_t i64;
typedef uint64_t u64;

static const i64 vals[] = {0, 1, 2, 3, -1, -2, -3, 7, 0x7f, 0x80, -0x80, 0xff, 0x7fff, 0x8000, -0x8000, 0xffff,
                           0x7fffffffLL, 0x80000000LL, -0x80000000LL, 0xffffffffLL, 0x100000000LL,
                           -0x100000000LL, 0x7fffffffffffffffLL, (i64)(1ULL << 63), (i64)(1ULL << 63) + 1,
                           0x3fffffffffffffffLL, 0x4000000000000000LL, 3037000499LL, 3037000500LL,
                           (i64)0xfffffffffffffff0ULL, 12345, -98765432101LL};
#define NV (int)(sizeof vals / sizeof vals[0])

static uint32_t h;
static void mix(uint32_t x) { h = (h ^ x) * 16777619u; }

#define DEF(N, OP, TA, TB, TR)                                                                                      \
  __attribute__((noinline)) static int N(TA a, TB b, TR *r) { return __builtin_##OP##_overflow(a, b, r); }          \
  __attribute__((noinline)) static int N##_p(TA a, TB b) { return __builtin_##OP##_overflow_p(a, b, (TR)0); }

#define COMBO(S, TA, TB, TR)                                                                                        \
  DEF(add_##S, add, TA, TB, TR)                                                                                     \
  DEF(sub_##S, sub, TA, TB, TR)                                                                                     \
  DEF(mul_##S, mul, TA, TB, TR)

COMBO(i64i64u64, i64, i64, u64)
COMBO(u64u64i64, u64, u64, i64)
COMBO(i32i32u64, i32, i32, u64)
COMBO(u64i32i64, u64, i32, i64)
COMBO(i64u64u64, i64, u64, u64)
COMBO(u64u64i32, u64, u64, i32)
COMBO(i64i64i32, i64, i64, i32)
COMBO(i64i64u32, i64, i64, u32)
COMBO(u32u32i64, u32, u32, i64)
COMBO(i32u32u64, i32, u32, u64)
COMBO(u64u32u64, u64, u32, u64)
COMBO(i64i64i64, i64, i64, i64)
COMBO(u64u64u64, u64, u64, u64)
COMBO(i64i32u16, i64, i32, u16)
COMBO(i8i8u64, i8, i8, u64)
COMBO(u64i64i64, u64, i64, i64)
COMBO(i32i32i32, i32, i32, i32)

#define RUN(S, TA, TB, TR)                                                                                          \
  do                                                                                                                \
  {                                                                                                                 \
    static int (*const fn[3])(TA, TB, TR *) = {add_##S, sub_##S, mul_##S};                                           \
    static int (*const fp[3])(TA, TB) = {add_##S##_p, sub_##S##_p, mul_##S##_p};                                     \
    static const char *const nm[3] = {"add", "sub", "mul"};                                                         \
    for (int o = 0; o < 3; o++)                                                                                     \
    {                                                                                                               \
      unsigned cnt = 0, pbad = 0;                                                                                   \
      h = 2166136261u;                                                                                              \
      for (int i = 0; i < NV; i++)                                                                                  \
        for (int j = 0; j < NV; j++)                                                                                \
        {                                                                                                           \
          TA a = (TA)vals[i];                                                                                       \
          TB b = (TB)vals[j];                                                                                       \
          TR r = 0;                                                                                                 \
          int f = fn[o](a, b, &r);                                                                                  \
          cnt += f;                                                                                                 \
          pbad += fp[o](a, b) != f;                                                                                 \
          mix((uint32_t)f);                                                                                         \
          mix((uint32_t)(u64)r);                                                                                    \
          mix((uint32_t)((u64)r >> 32));                                                                            \
        }                                                                                                           \
      printf("%s %s: %u %08x pbad=%u\n", nm[o], #S, cnt, (unsigned)h, pbad);                                        \
    }                                                                                                               \
  } while (0)

volatile int m1 = -1, zero = 0, two = 2, one = 1;
volatile unsigned long long umax = ~0ULL;
volatile long long llmin = (long long)(1ULL << 63);
volatile unsigned u3 = 3000000000u;

int main(void)
{
  RUN(i64i64u64, i64, i64, u64);
  RUN(u64u64i64, u64, u64, i64);
  RUN(i32i32u64, i32, i32, u64);
  RUN(u64i32i64, u64, i32, i64);
  RUN(i64u64u64, i64, u64, u64);
  RUN(u64u64i32, u64, u64, i32);
  RUN(i64i64i32, i64, i64, i32);
  RUN(i64i64u32, i64, i64, u32);
  RUN(u32u32i64, u32, u32, i64);
  RUN(i32u32u64, i32, u32, u64);
  RUN(u64u32u64, u64, u32, u64);
  RUN(i64i64i64, i64, i64, i64);
  RUN(u64u64u64, u64, u64, u64);
  RUN(i64i32u16, i64, i32, u16);
  RUN(i8i8u64, i8, i8, u64);
  RUN(u64i64i64, u64, i64, i64);
  RUN(i32i32i32, i32, i32, i32);

  /* the report's reproducer */
  unsigned long long ur;
  long long sr;
  unsigned ui;
  int si;
  int o1 = __builtin_add_overflow(m1, zero, &ur);
  int o2 = __builtin_add_overflow(umax, zero, &sr);
  int o3 = __builtin_sub_overflow(zero, two, &ur);
  int o4 = __builtin_mul_overflow(umax, 1, &sr);
  int o5 = __builtin_add_overflow(u3, zero, &si);
  int o6 = __builtin_sub_overflow(llmin, 1, &ur);
  int o7 = __builtin_add_overflow_p(m1, zero, (unsigned long long)0);
  int o8 = __builtin_add_overflow(m1, zero, &ui);
  int o9 = __builtin_mul_overflow(llmin, m1, &ur);
  int a = __builtin_add_overflow(umax, one, &ui);
  int b = __builtin_add_overflow(umax, zero, &si);
  printf("%d %d %d %d %d %d %d %d %d | %d %d\n", o1, o2, o3, o4, o5, o6, o7, o8, o9, a, b);

  /* result stored through a pointer that aliases an operand */
  u64 x = ~0ULL;
  int oa = __builtin_add_overflow(x, (i64)1, &x);
  i64 y = -5;
  int ob = __builtin_sub_overflow(y, (i64)1, (u64 *)&y);
  printf("%d %llu %d %llu\n", oa, (unsigned long long)x, ob, (unsigned long long)y);
  return 0;
}
