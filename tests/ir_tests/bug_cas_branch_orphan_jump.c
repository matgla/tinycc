/*
 *  TCC IR - an inline CAS loop's branch lands on a jump nothing else reaches
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* `if (cas(...)) { r = i; goto done; }` under -minline-atomics: the success
 * block is a coalesced (empty) phi copy and a JUMP to `done`, after code that
 * never falls into it.  The LDREX/STREX loop branches to that block's IR
 * target as it is, unthreaded (ir_codegen_offer_cas_branch), so the JUMP is
 * reached; ir_codegen_mark_orphaned_jumps took it for orphaned (every other
 * branch to it is threaded) and dropped it, and the success branch ran on into
 * the failure path.  main() takes every exit of each function. */
#include <stdint.h>
#include <stdio.h>

#define RELAXED 0
#define ACQUIRE 2
#define SEQ_CST 5

volatile unsigned sink;

/* The success block between the `return -1` and the second exit's code. */
__attribute__((noinline)) int cas_goto_done(uint32_t *p, const uint32_t *q, int n)
{
  int i = 0, r = 0;
  for (;;)
  {
    uint32_t e = 0;
    if (__atomic_compare_exchange_n(p, &e, 1u, 1, ACQUIRE, RELAXED))
    {
      r = i;
      goto done;
    }
    if (++i > n)
      return -1;
    if (q[i] == 7)
    {
      r = i + 100;
      goto done;
    }
  }
done:
  sink = r;
  return r * 3;
}

/* Strong CAS, seq_cst, the expected value in a register; the jump block
 * leaves an inner loop for the outer one's next round. */
__attribute__((noinline)) int cas_next_round(uint32_t *p, uint32_t want, const uint32_t *q, int rounds, int n)
{
  int total = 0;
  for (int k = 0; k < rounds; k++)
  {
    int i = 0;
    for (;;)
    {
      uint32_t e = want;
      if (__atomic_compare_exchange_n(p, &e, want + 1u, 0, SEQ_CST, SEQ_CST))
        goto next;
      if (++i > n)
        return -1 - total;
      if (q[i] == 9)
      {
        total += 1000;
        goto next;
      }
    }
  next:
    total += i;
    sink = (unsigned)total;
  }
  return total;
}

int main(void)
{
  uint32_t q[8] = {0, 3, 7, 1, 1, 1, 1, 1};
  uint32_t w;

  w = 0;
  int a = cas_goto_done(&w, q, 5);
  printf("goto free: %d w=%u sink=%u\n", a, w, sink);
  w = 1;
  a = cas_goto_done(&w, q, 5);
  printf("goto found: %d w=%u sink=%u\n", a, w, sink);
  w = 1;
  a = cas_goto_done(&w, q, 1);
  printf("goto none: %d w=%u\n", a, w);

  w = 4;
  int b = cas_next_round(&w, 4, q, 3, 6);
  printf("round free: %d w=%u sink=%u\n", b, w, sink);
  uint32_t r9[8] = {0, 2, 9, 2, 2, 2, 2, 2};
  w = 0;
  b = cas_next_round(&w, 5, r9, 2, 6);
  printf("round found: %d w=%u sink=%u\n", b, w, sink);
  w = 0;
  b = cas_next_round(&w, 5, q, 2, 1);
  printf("round none: %d w=%u\n", b, w);
  w = 6;
  b = cas_next_round(&w, 6, r9, 2, 6);
  printf("round mixed: %d w=%u sink=%u\n", b, w, sink);
  return 0;
}
