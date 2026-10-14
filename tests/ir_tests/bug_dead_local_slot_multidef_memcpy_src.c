/*
 *  TCC Tests - dead_local_slot: memcpy source through a two-def address temp
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* `x = c ? b : a` copies through one TEMP defined twice (&a home, &b home).
 * dls_vreg_frame_off resolved it by its last def only, so the read of a's
 * home was never recorded and the parameter stores to it were deleted: the
 * copy read stale stack.  A chain step must now be a single-def temp. */
#include <stdint.h>
#include <stdio.h>

typedef struct __attribute__((packed)) Op
{
  union
  {
    int32_t vr;
    struct
    {
      uint32_t position : 17;
      uint32_t is_complex : 1;
      uint32_t tag : 3;
      uint32_t is_lval : 1;
      uint32_t is_llocal : 1;
      uint32_t is_local : 1;
      uint32_t is_const : 1;
      uint32_t btype : 3;
      uint32_t vreg_type : 4;
    };
  };
  union
  {
    int32_t imm32;
    uint32_t pool_idx;
  } u;
  uint8_t is_unsigned : 1;
  uint8_t is_static : 1;
  uint8_t is_sym : 1;
  uint8_t is_param : 1;
  uint8_t aux : 4;
} Op;

static inline int is_imm32(Op op) { return op.tag == 2 && !op.is_lval; }
__attribute__((noinline)) int32_t get_vreg_w(int32_t w) { return w & 0x1ffff; }
static inline int32_t get_vreg(const Op op) { return get_vreg_w(op.vr); }

__attribute__((noinline)) static int inner_matches(Op xs1, Op xs2, Op a)
{
  int s1_neg1 = is_imm32(xs1) && xs1.u.imm32 == -1;
  int s2_neg1 = is_imm32(xs2) && xs2.u.imm32 == -1;
  if (!s1_neg1 && !s2_neg1)
    return 0;
  Op x_inner = s1_neg1 ? xs2 : xs1;
  return !x_inner.is_lval && x_inner.tag == 1 && get_vreg(x_inner) == get_vreg(a);
}

/* Single-def address: the copy source resolves exactly and must stay live. */
__attribute__((noinline)) static int pick_one(Op xs1, int k)
{
  Op c = xs1;
  return c.position + k;
}

__attribute__((noinline)) int dirty(int k)
{
  volatile uint32_t buf[32];
  for (int i = 0; i < 32; i++)
    buf[i] = (1u << 18) | (uint32_t)(77 + k);
  return buf[k & 31];
}

__attribute__((noinline)) Op mk(int tag, int pos, int imm)
{
  Op o = {0};
  o.tag = tag;
  o.position = pos;
  o.u.imm32 = imm;
  return o;
}

int main(void)
{
  for (int k = 0; k < 3; k++)
  {
    dirty(k);
    int miss = inner_matches(mk(1, 5, 0), mk(2, 0, -1), mk(1, 77 + k, 0));
    dirty(k);
    int hit1 = inner_matches(mk(1, 9, 0), mk(2, 0, -1), mk(1, 9, 0));
    dirty(k);
    int hit2 = inner_matches(mk(2, 0, -1), mk(1, 9, 0), mk(1, 9, 0));
    printf("k=%d miss=%d hit1=%d hit2=%d one=%d\n", k, miss, hit1, hit2, pick_one(mk(1, 40, 0), k));
  }
  return 0;
}
