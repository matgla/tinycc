/*
 *  TCC Tests - Range-check boolean latch and edge returns
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
#include <stdint.h>
struct region { uint32_t start, end; };
/* spelled the way Zig's C backend emits it: every comparison materialized
 * into a bool temp first, loop condition tested through the temp */
__attribute__((noinline)) int range_ok(const struct region *regions, uint32_t len, uint32_t addr, uint32_t size)
{
    if (size == 0) return 1;
    uint64_t end = (uint64_t)addr + (uint64_t)size;
    uint8_t ovf = 5;                      /* CBE undefined poison */
    if (end < (uint64_t)addr) { ovf = 1; } else { ovf = 0; }
    if (ovf == 0) goto overflow_ok;
    return 0;
overflow_ok:
    { uint32_t i = 0;
    for (;;) {
        uint8_t more = (uint32_t)(i < len);
        if (more == 0) goto done;
        uint8_t lo = 0;
        if (addr >= regions[i].start) { lo = 1; } else { lo = 0; }
        if (lo != 0) {
            uint8_t hi = 0;
            if (end <= (uint64_t)regions[i].end) { hi = 1; } else { hi = 0; }
            if (hi != 0) return 1;
        }
        i += 1;
    } }
done:
    return 0;
}

struct region_slice { const struct region *ptr; uint32_t len; };
struct checked_sum { uint32_t payload; uint16_t error; };

static struct checked_sum add_checked(uint32_t a, uint32_t b)
{
    struct checked_sum sum = {0xaaaaaaaa, 5};
    uint32_t end = a + b;
    if (end < a) sum.error = 1;
    else { sum.payload = end; sum.error = 0; }
    return sum;
}

static uint8_t inline_range_ok(struct region_slice const a0, uint32_t const a1, uint32_t const a2) {
 uint32_t t0;
 uint32_t t7;
 uint32_t t2;
 uint32_t t4;
 uint32_t t6;
 uint32_t t12;
 uint32_t t5;
 struct checked_sum t3;
 struct region t8;
 struct region t9;
 struct region const *t10;
 uint32_t const *t11;
 uint8_t t1;
 uint8_t t13;
 t0 = (uint32_t)a2;
 t1 = t0 == UINT32_C(0);
 if (t1) {
  return 1;
 }
 goto zig_block_0;
zig_block_0:;
 t3 = add_checked(a1, a2);
 t1 = t3.error == UINT16_C(0);
 if (t1) {
  t4 = t3.payload;
  t2 = t4;
  goto zig_block_1;
 }
 return 0;
zig_block_1:;
 t5 = (uint32_t)0ul;
 t4 = a0.len;
 zig_loop_30:
 t6 = t5;
 t0 = (uint32_t)t6;
 t7 = (uint32_t)t4;
 t1 = t0 < t7;
 if (t1) {
  t8 = a0.ptr[t6];
  t9 = t8;
  t10 = (struct region const *)&t9;
  t11 = (uint32_t const *)&t10->start;
  t12 = (*t11);
  t7 = (uint32_t)a1;
  t0 = (uint32_t)t12;
  t1 = t7 >= t0;
  if (t1) {
   t11 = (uint32_t const *)&t10->end;
   t12 = (*t11);
   t0 = (uint32_t)t2;
   t7 = (uint32_t)t12;
   t1 = t0 <= t7;
   t13 = t1;
   goto zig_block_5;
  }
  t13 = 0;
  goto zig_block_5;
zig_block_5:;
  if (t13) {
   return 1;
  }
  goto zig_block_4;
zig_block_4:;
  (void)0;
  goto zig_block_3;
 }
 goto zig_block_2;
zig_block_3:;
 t6 = t6 + (uint32_t)1ul;
 t5 = t6;
 goto zig_loop_30;
zig_block_2:;
 return 0;
}

int range_caller(const struct region *r, uint32_t n, uint32_t a, uint32_t s)
{
    struct region_slice regions = {r, n};
    return inline_range_ok(regions, a, s);
}

__attribute__((noinline)) int shared_exit(unsigned a, unsigned b, unsigned bypass)
{
    unsigned value = a < b;
    if (bypass) goto out;
    if (value == 0) goto out;
    return 7;
out:
    return value;
}

__attribute__((noinline)) int fallthrough_exit(unsigned a, unsigned b)
{
    unsigned value = a < b;
    if (value != 0) return 7;
    return value;
}

__attribute__((noinline)) int value_read(unsigned a, unsigned b)
{
    unsigned value = a < b;
    if (value == 0) return value;
    return value + 9;
}

int main(void)
{
    struct region r[2] = {{0,100},{200,300}};
    uint32_t addresses[] = {0, 50, 99, 100, 150, 200, 299, 300, UINT32_MAX};
    uint32_t sizes[] = {0, 1, 10, 100, UINT32_MAX};
    for (unsigned n = 0; n <= 2; n++)
        for (unsigned i = 0; i < sizeof(addresses)/sizeof(*addresses); i++)
            for (unsigned j = 0; j < sizeof(sizes)/sizeof(*sizes); j++) {
                uint32_t a = addresses[i], s = sizes[j];
                uint64_t end = (uint64_t)a + s;
                int expected = s == 0;
                for (unsigned k = 0; k < n; k++)
                    if (a >= r[k].start && end <= r[k].end) expected = 1;
                if (range_ok(r,n,a,s) != expected || range_caller(r,n,a,s) != expected) return 1;
            }
    for (unsigned a = 0; a < 3; a++)
        for (unsigned b = 0; b < 3; b++) {
            unsigned v = a < b;
            if (shared_exit(a,b,1) != v || shared_exit(a,b,0) != (v ? 7 : 0)) return 2;
            if (fallthrough_exit(a,b) != (v ? 7 : 0)) return 3;
            if (value_read(a,b) != (v ? 10 : 0)) return 4;
        }
    return 0;
}
