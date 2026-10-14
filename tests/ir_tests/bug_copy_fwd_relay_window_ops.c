/*
 *  TCC IR - a relay copy across ops that reach memory unseen
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* ssa:copy_fwd's relay moves `t16.is_null = false; t16.payload = t21` into
 * t9 when `t9 = t16` follows and t16 dies there.  Where t9's address is out,
 * nothing between the first moved store and the copy may reach memory through
 * a pointer.  The window check listed only calls, asm and a few memory ops as
 * reaching memory and let every other op through, so a __builtin_apply whose
 * callee reads t9 through a global pointer saw the new value before the copy
 * ran.  Each walk below puts one such op between the stores and the copy and
 * the callee (or the code) reads or writes t9 through the escaped pointer. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct Hdr
{
  uint64_t id;
  uint32_t w[34];
  uint8_t ft;
  uint8_t len;
};
struct OptHdr
{
  struct Hdr payload;
  bool is_null;
};
struct EHdr
{
  struct Hdr payload;
  uint16_t error;
};

static void fill(struct Hdr *h, int k)
{
  h->id = 0x100000000ull * (unsigned)k + 11u;
  for (int i = 0; i < 34; i++)
    h->w[i] = (uint32_t)(k * 100 + i);
  h->ft = (uint8_t)(k & 3);
  h->len = (uint8_t)(k + 1);
}

__attribute__((noinline)) static struct OptHdr make_opt(int k)
{
  struct OptHdr o;
  memset(&o, 0, sizeof o);
  if (k < 0)
  {
    o.is_null = true;
    return o;
  }
  fill(&o.payload, k);
  return o;
}

__attribute__((noinline)) static struct EHdr make_hdr(int k)
{
  struct EHdr e;
  memset(&e, 0, sizeof e);
  if (k == 7)
  {
    e.error = 3;
    return e;
  }
  fill(&e.payload, k + 20);
  return e;
}

static struct OptHdr *volatile gopt;
static unsigned obs;

/* reads the t9 its caller handed out */
__attribute__((noinline)) void obsfn(void)
{
  struct OptHdr *p = gopt;
  if (p)
    obs = obs * 31 + p->payload.w[0] + (unsigned)p->payload.id + p->is_null;
}

static int apply_args[16];
static void *jb[5];

#define WALK(name, HOOK)                                                                                               \
  __attribute__((noinline)) static unsigned name(int n)                                                                \
  {                                                                                                                    \
    struct OptHdr t9, t16;                                                                                             \
    struct EHdr t22;                                                                                                   \
    struct Hdr t21;                                                                                                    \
    unsigned acc = 0;                                                                                                  \
    t16 = make_opt(n);                                                                                                 \
    t9 = t16;                                                                                                          \
    gopt = &t9;                                                                                                        \
    for (int i = 0;; i++)                                                                                              \
    {                                                                                                                  \
      t16 = t9;                                                                                                        \
      if (t16.is_null)                                                                                                 \
        break;                                                                                                         \
      acc = acc * 7 + t16.payload.w[1];                                                                                \
      if (i == n)                                                                                                      \
      {                                                                                                                \
        t9 = (struct OptHdr){.is_null = true};                                                                         \
        continue;                                                                                                      \
      }                                                                                                                \
      t22 = make_hdr(i);                                                                                               \
      if (t22.error)                                                                                                   \
        break;                                                                                                         \
      t21 = t22.payload;                                                                                               \
      t16.is_null = false;                                                                                             \
      t16.payload = t21;                                                                                               \
      HOOK;                                                                                                            \
      t9 = t16;                                                                                                        \
    }                                                                                                                  \
    gopt = NULL;                                                                                                       \
    return acc;                                                                                                        \
  }

/* __builtin_apply calls obsfn: it reads the old t9 */
WALK(walk_apply, __builtin_apply((void (*)())obsfn, apply_args, 0))
/* __builtin_setjmp sits between the stores and the copy */
WALK(walk_setjmp, (void)__builtin_setjmp(jb); obsfn())
/* a load through the escaped pointer reads the old t9 */
WALK(walk_load, obs = obs * 17 + gopt->payload.w[0])
/* a store through the escaped pointer is overwritten by the copy */
WALK(walk_store, gopt->payload.w[0] = 99; obs = obs * 13 + t9.payload.w[0])

int main(void)
{
  unsigned a;
  obs = 0;
  a = walk_apply(3);
  printf("apply %u obs=%u\n", a, obs);
  obs = 0;
  a = walk_setjmp(4);
  printf("setjmp %u obs=%u\n", a, obs);
  obs = 0;
  a = walk_load(5);
  printf("load %u obs=%u\n", a, obs);
  obs = 0;
  a = walk_store(9);
  printf("store %u obs=%u\n", a, obs);
  return 0;
}
