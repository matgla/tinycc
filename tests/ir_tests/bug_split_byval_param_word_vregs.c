/*
 *  TCC IR - a by-value struct parameter split between r0-r3 and the stack
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* params.c tcc_ir_params_process_struct used every r3/stack-split struct in
 * place from a `push {r0-r3}` copy, so its fields were memory reads, reloaded
 * in loops.  A split struct with a short stack part now gets one PARAM vreg per
 * register word and a home (like a register struct); long ones stay in place. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct slice_u8 { uint8_t const *ptr; uintptr_t len; };
struct CompIter { struct slice_u8 path; uintptr_t root_len; uintptr_t root_end_index; uintptr_t start_index; uintptr_t end_index; };
struct S20 { signed char c; short h; int a, b, d, e; };
struct S18 { signed char b[18]; };
struct L24 { int a; long long x; long long y; }; /* 8-aligned: r0, r2:r3, stack */
struct Big { int w[16]; };                      /* 64 bytes: stays in place */
struct S8 { int x, y; };

/* Zig-CBE spelling of ComponentIterator.peekNext's separator scan. */
__attribute__((noinline)) uintptr_t skip24(struct CompIter const a0)
{
  struct CompIter t0; struct CompIter const *t1; struct slice_u8 const *t5; struct slice_u8 t11; uintptr_t t16, t4;
  t0 = a0;
  t1 = (struct CompIter const *)&t0;
  t16 = t1->end_index;
zig_loop_69:
  t5 = (struct slice_u8 const *)&t1->path;
  t4 = t5->len;
  if (t16 < t4)
  {
    t5 = (struct slice_u8 const *)&t1->path;
    t11 = (*t5);
    if (t11.ptr[t16] != UINT8_C(47)) { t16 = t16 + 1; goto zig_loop_69; }
  }
  return t16;
}

__attribute__((noinline)) uintptr_t skip24n(struct CompIter const a0)
{
  struct CompIter t0 = a0; struct CompIter const *t1 = &t0; uintptr_t i = t1->end_index;
  while (i < t1->path.len && t1->path.ptr[i] != 47)
    i++;
  return i;
}

__attribute__((noinline)) uintptr_t sum24(struct CompIter a0) { return a0.root_len + a0.end_index * 3 + a0.path.len; }
__attribute__((noinline)) uintptr_t mid(int x, struct CompIter a0) { return a0.root_len * x + a0.end_index; }
__attribute__((noinline)) int s8_r3(int a, int b, int c, struct S8 s) { return a + b * 2 + c * 3 + s.x * 5 + s.y * 7; }
__attribute__((noinline)) int s20_mix(int k, struct S20 s) { return k + s.c + s.h * 3 + s.a * 5 + s.b * 7 + s.d * 11 + s.e * 13; }
__attribute__((noinline)) int s18_sum(int k, struct S18 s)
{
  unsigned t = k;
  for (int i = 0; i < 18; i++)
    t = t * 3 + s.b[i];
  return (int)(t % 100003u);
}
__attribute__((noinline)) long long l24(struct L24 s) { return s.a + s.x * 3 + s.y * 5; }
__attribute__((noinline)) int big_sum(int k, struct Big s)
{
  int t = k;
  for (int i = 0; i < 16; i++)
    t += s.w[i] * (i + 1);
  return t;
}

/* The parameter is a modifiable object: writes, address taken, passed on. */
__attribute__((noinline)) uintptr_t peek(struct CompIter *it) { return it->start_index * 100 + it->end_index; }
__attribute__((noinline)) uintptr_t bump_and_peek(struct CompIter it)
{
  it.start_index += 5;
  it.end_index = it.path.len;
  return peek(&it) + it.root_len;
}
__attribute__((noinline)) uintptr_t take(struct CompIter it) { return it.path.ptr[it.end_index] + it.start_index; }
__attribute__((noinline)) uintptr_t pass_on(struct CompIter it)
{
  it.end_index--;
  return take(it) + take(it) * 1000;
}
__attribute__((noinline)) uintptr_t keep_across_call(struct CompIter it)
{
  uintptr_t r = 0;
  for (uintptr_t i = it.start_index; i < it.end_index; i++)
    r = r * 7 + peek(&(struct CompIter){it.path, 0, 0, i, it.root_len});
  return r + it.path.len;
}

/* Pure forwarders: the body only hands the parameters on. */
__attribute__((noinline)) uintptr_t fwd_target(struct CompIter it) { return sum24(it) + it.start_index; }
__attribute__((noinline)) uintptr_t fwd(struct CompIter it) { return fwd_target(it); }
__attribute__((noinline)) int fwd8_target(int a, int b, int c, struct S8 s) { return s8_r3(a, b, c, s) + 1; }
__attribute__((noinline)) int fwd8(int a, int b, int c, struct S8 s) { return fwd8_target(a, b, c, s); }
__attribute__((noinline)) int fwd18_target(int k, struct S18 s) { return s18_sum(k, s) ^ 0x55; }
__attribute__((noinline)) int fwd18(int k, struct S18 s) { return fwd18_target(k, s); }

/* A stack parameter after the split struct. */
__attribute__((noinline)) int after(int k, struct S8 pad, struct CompIter it, int tail)
{
  return k + pad.x + pad.y * 2 + (int)it.root_len * 3 + (int)it.end_index * 5 + tail * 7;
}

/* The stack word lives in a TEMP loaded from the incoming argument area: not
 * an uninitialised read (ub_only_body_elide emptied this body). */
struct V4 { int w[4]; };
__attribute__((noinline)) void fill(struct V4 *x, struct V4 v)
{
  for (int i = 0; i < 32; ++i)
    x[i] = v;
}

/* A volatile-qualified struct parameter: every field read is a volatile read
 * of the parameter object, re-done each iteration (params.c dropped the
 * qualifier, so the home copy's reads were forwarded and hoisted). */
struct S12 { int x, y, z; };
__attribute__((noinline)) int vol_split12(int a, int b, int c, volatile struct S12 s, int n)
{
  int t = a + b + c;
  for (int i = 0; i < n; i++)
    t += s.x + s.z;
  return t;
}
__attribute__((noinline)) int vol_split8(int a, int b, int c, volatile struct S8 s, int n)
{
  int t = a + b + c;
  for (int i = 0; i < n; i++)
    t += s.y;
  return t;
}
__attribute__((noinline)) int vol_reg8(volatile struct S8 s, int n)
{
  int t = 0;
  for (int i = 0; i < n; i++)
    t += s.y;
  return t;
}

/* sret in r0 pushes the struct one register on. */
__attribute__((noinline)) struct CompIter advance(struct CompIter it)
{
  while (it.end_index < it.path.len && it.path.ptr[it.end_index] != '/')
    it.end_index++;
  it.start_index = it.end_index;
  return it;
}

int main(void)
{
  static const uint8_t p[] = "abc/def/ghij";
  struct CompIter it = {{p, 12}, 1, 2, 3, 0};
  printf("skip24 %u %u\n", (unsigned)skip24(it), (unsigned)skip24((struct CompIter){{p, 12}, 0, 0, 0, 4}));
  printf("skip24n %u %u\n", (unsigned)skip24n(it), (unsigned)skip24n((struct CompIter){{p, 12}, 0, 0, 0, 8}));
  printf("sum24 %u mid %u\n", (unsigned)sum24(it), (unsigned)mid(9, it));
  printf("s8_r3 %d\n", s8_r3(1, 2, 3, (struct S8){4, 5}));
  struct S20 s20 = {-3, -700, 11, 13, 17, 19};
  printf("s20 %d\n", s20_mix(1000, s20));
  struct S18 s18;
  for (int i = 0; i < 18; i++)
    s18.b[i] = (signed char)(i * 7 - 40);
  printf("s18 %d\n", s18_sum(2, s18));
  printf("l24 %lld\n", l24((struct L24){-9, 0x123456789LL, -0x10000000aLL}));
  struct Big big;
  for (int i = 0; i < 16; i++)
    big.w[i] = i * i - 20;
  printf("big %d\n", big_sum(77, big));
  printf("bump %u\n", (unsigned)bump_and_peek(it));
  printf("it %u %u %u %u\n", (unsigned)it.root_len, (unsigned)it.root_end_index, (unsigned)it.start_index,
         (unsigned)it.end_index);
  it.end_index = 6;
  printf("pass_on %u\n", (unsigned)pass_on(it));
  printf("keep %u\n", (unsigned)keep_across_call((struct CompIter){{p, 12}, 4, 0, 2, 6}));
  printf("fwd %u %d %d\n", (unsigned)fwd(it), fwd8(1, 2, 3, (struct S8){4, 5}), fwd18(3, s18));
  printf("after %d\n", after(1, (struct S8){2, 3}, it, 11));
  struct V4 v4[32] = {{{0}}};
  fill(v4, (struct V4){{1, -2, 3, -4}});
  printf("fill %d %d %d\n", v4[0].w[0], v4[13].w[1], v4[31].w[3]);
  printf("vol %d %d %d\n", vol_split12(1, 2, 3, (struct S12){4, 5, 6}, 3), vol_split8(1, 2, 3, (struct S8){4, 5}, 4),
         vol_reg8((struct S8){7, 8}, 5));
  struct CompIter r = advance((struct CompIter){{p, 12}, 0, 0, 0, 4});
  printf("advance %u %u %u\n", (unsigned)r.start_index, (unsigned)r.end_index, (unsigned)r.path.len);
  return 0;
}
