/*
 *  TCC Tests - Inlined boolean predicate results: constant-only return slots
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* An inlined `return true;` / `return false;` callee leaves a return slot
 * written only by constant STOREs; SSA kept it a memory VAR, so nothing
 * threaded the constant edges into the caller's test of it (mov; uxtb; cbz;
 * movs #1; b; movs #0; uxtb before every return).  Such slots are promoted
 * now, and a loop predicate whose returns are then all 0/1 inlines. */
#include <stdint.h>
#include <stdio.h>

typedef _Bool bool;
#define true 1
#define false 0

struct Region { uintptr_t start; uintptr_t end; };
struct slice_Region { struct Region const *ptr; uintptr_t len; };
struct errunion_usize { uintptr_t payload; uint16_t error; };
struct tuple_warn { uintptr_t f1; uintptr_t f2; };

uintptr_t writable_count;
struct Region writable[4];
int warned;

/* spelled the way Zig's C backend emits uaccess.zig */
static struct errunion_usize math_add(uintptr_t const a0, uintptr_t const a1)
{
 uintptr_t sum;
 struct errunion_usize t8;
 if (__builtin_add_overflow(a0, a1, &sum)) {
  return (struct errunion_usize){ .error = 5, .payload = (uintptr_t)0xaaaaaaaaul };
 }
 t8.payload = sum;
 t8.error = 0;
 return t8;
}

static bool range_ok(struct slice_Region const a0, uintptr_t const a1, uintptr_t const a2)
{
 uintptr_t t2, t4, t5, t6, t12;
 uint32_t t0, t7;
 struct errunion_usize t3;
 struct Region t8;
 struct Region const *t10;
 bool t1, t13;
 t0 = a2;
 t1 = t0 == 0;
 if (t1) {
  return true;
 }
 goto zig_block_0;
zig_block_0:;
 t3 = math_add(a1, a2);
 t1 = t3.error == 0;
 if (t1) {
  t4 = t3.payload;
  t2 = t4;
  goto zig_block_1;
 }
 return false;
zig_block_1:;
 t5 = 0;
 t4 = a0.len;
zig_loop_30:
 t6 = t5;
 t0 = t6;
 t7 = t4;
 t1 = t0 < t7;
 if (t1) {
  t8 = a0.ptr[t6];
  t10 = &t8;
  t12 = t10->start;
  t7 = a1;
  t0 = t12;
  t1 = t7 >= t0;
  if (t1) {
   t12 = t10->end;
   t0 = t2;
   t7 = t12;
   t1 = t0 <= t7;
   t13 = t1;
   goto zig_block_5;
  }
  t13 = false;
  goto zig_block_5;
zig_block_5:;
  if (t13) {
   return true;
  }
  goto zig_block_3;
 }
 goto zig_block_2;
zig_block_3:;
 t6 = t6 + 1;
 t5 = t6;
 goto zig_loop_30;
zig_block_2:;
 return false;
}

static bool permitted(uintptr_t const a0, uintptr_t const a1)
{
 struct slice_Region t3;
 bool t0;
 t3.ptr = writable;
 t3.len = writable_count;
 t0 = range_ok(t3, a0, a1);
 if (t0) {
  return true;
 }
 goto zig_block_0;
zig_block_0:;
 return false;
}

/* log.scoped(.uaccess).warn: compiled out */
static void log_warn(struct tuple_warn const a0)
{
 (void)a0;
}

static bool access_ok(uintptr_t const a0, uintptr_t const a1)
{
 struct tuple_warn t2;
 bool t0, t1;
 t0 = permitted(a0, a1);
 t1 = !t0;
 if (t1) {
  t2.f1 = a1;
  t2.f2 = a0;
  log_warn(t2);
  goto zig_block_0;
 }
 goto zig_block_0;
zig_block_0:;
 return t0;
}

/* An out-of-line copy (address taken): its returns come straight off the
 * loop's tests. */
bool (*volatile access_ok_ptr)(uintptr_t, uintptr_t) = access_ok;

/* _irq_svcall's first check */
__attribute__((noinline)) bool caller_is_untrusted(uint32_t a0)
{
 return a0 != 3;
}

intptr_t dispatch(uint32_t const a0, uintptr_t const a2)
{
 bool t0, t2, t4;
 t0 = caller_is_untrusted(a0);
 if (t0) {
  t4 = access_ok(a2, 8);
  t4 = !t4;
  t2 = t4;
  goto zig_block_2;
 }
 t2 = false;
 goto zig_block_2;
zig_block_2:;
 if (t2) {
  return -1;
 }
 return (intptr_t)a0;
}

/* uaccess.check */
uint16_t check(uintptr_t const a0, uintptr_t const a1)
{
 bool t0;
 t0 = access_ok(a0, a1);
 t0 = !t0;
 if (t0) {
  return 23;
 }
 return 0;
}

/* A slot written by constants AND a computed value keeps both. */
__attribute__((noinline)) static bool mixed_slot(uint32_t a, uint32_t b)
{
 if (a == 0)
  return true;
 if (a == 1)
  return false;
 return a < b;
}

static int reference(uintptr_t a, uintptr_t s)
{
 uint64_t end = (uint64_t)a + s;
 if (s == 0)
  return 1;
 if (end > 0xffffffffu)
  return 0;
 for (uintptr_t k = 0; k < writable_count; k++)
  if (a >= writable[k].start && end <= writable[k].end)
   return 1;
 return 0;
}

int main(void)
{
 static const uintptr_t addrs[] = {0, 50, 99, 100, 150, 200, 292, 299, 300, 0x20000000u, 0xffffffffu - 4, 0xffffffffu};
 static const uintptr_t sizes[] = {0, 1, 8, 100, 0xffffffffu};
 writable[0] = (struct Region){0, 100};
 writable[1] = (struct Region){200, 300};
 writable[2] = (struct Region){0x20000000u, 0x20000100u};
 writable[3] = (struct Region){0xffffffffu - 8, 0xffffffffu};
 int ok = 0, bad = 0, denied = 0;
 for (unsigned n = 0; n <= 4; n++) {
  writable_count = n;
  for (unsigned i = 0; i < sizeof(addrs) / sizeof(*addrs); i++) {
   for (unsigned j = 0; j < sizeof(sizes) / sizeof(*sizes); j++) {
    uintptr_t a = addrs[i], s = sizes[j];
    int want = reference(a, s);
    if (access_ok_ptr(a, s) != want || access_ok(a, s) != want || check(a, s) != (want ? 0 : 23))
     bad++;
    else
     ok++;
    denied += !want;
   }
   intptr_t want_d = reference(addrs[i], 8) ? 5 : -1;
   if (dispatch(5, addrs[i]) != want_d || dispatch(3, addrs[i]) != 3)
    bad++;
  }
 }
 printf("checked %d bad %d denied %d\n", ok, bad, denied);
 printf("mixed %d %d %d %d\n", mixed_slot(0, 0), mixed_slot(1, 9), mixed_slot(2, 9), mixed_slot(9, 2));
 return bad != 0;
}
