/*
 *  TCC IR - an inline compare-exchange branching on its own result
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* `if (cas(...))` after an -minline-atomics CAS: the LDREX/STREX loop's exits
 * branch to the `if`'s targets themselves (ir/codegen.c, thumb_inline_atomic_call)
 * instead of testing old == expected again after the loop.  Both outcomes, the
 * failure write-back, CLREX on the failure path only, and a branch target out of
 * the 16-bit range must still behave; test_atomic_cas_seqcst_codegen.py checks
 * the shapes. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define RELAXED 0
#define ACQUIRE 2
#define RELEASE 3
#define SEQ_CST 5

uint32_t word;
volatile unsigned sink;

/* The kernel's SpinLock.lock: free is 0, WFE while taken (never, here). */
__attribute__((noinline)) void br_spin_lock(uint32_t *l, uint32_t token)
{
  for (;;)
  {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(l, &expected, token, true, ACQUIRE, RELAXED))
      return;
    __asm volatile("wfe" ::: "memory");
  }
}

/* A try-lock: the success code on the CAS's fall-through. */
__attribute__((noinline)) int br_trylock(uint32_t *l, uint32_t token)
{
  uint32_t expected = 0;
  if (!__atomic_compare_exchange_n(l, &expected, token, false, ACQUIRE, RELAXED))
    return -(int)expected;
  return 1;
}

/* Expected in a register, both outcomes taken by the caller. */
__attribute__((noinline)) int br_reg(uint32_t *p, uint32_t e, uint32_t d)
{
  if (__atomic_compare_exchange_n(p, &e, d, false, SEQ_CST, SEQ_CST))
    return 100;
  return (int)e;
}

/* An expected CMP's immediate cannot hold. */
__attribute__((noinline)) int br_big(uint32_t *p, uint32_t d)
{
  uint32_t e = 0x12345678;
  if (__atomic_compare_exchange_n(p, &e, d, false, RELEASE, RELAXED))
    return 7;
  return (int)(e & 0xff);
}

/* Zig's atomicrmw-by-CAS: a stale guess first, then the written-back value;
 * the failure branch goes backwards to the loop. */
__attribute__((noinline)) uint32_t br_max(uint32_t *p, uint32_t v, uint32_t guess, int *tries)
{
  uint32_t e = guess, d;
  int n = 0;
  do
  {
    n++;
    d = e > v ? e : v;
  } while (!__atomic_compare_exchange_n(p, &e, d, true, SEQ_CST, RELAXED));
  *tries = n;
  return e;
}

#define PAD sink = sink * 3 + (__COUNTER__ * __COUNTER__ ^ 0x5a);
#define PAD8 PAD PAD PAD PAD PAD PAD PAD PAD
#define PAD64 PAD8 PAD8 PAD8 PAD8 PAD8 PAD8 PAD8 PAD8

/* The failure path between the loop and the success target: past the reach of
 * a 16-bit conditional branch. */
__attribute__((noinline)) int br_far(uint32_t *p, uint32_t e, uint32_t d)
{
  if (__atomic_compare_exchange_n(p, &e, d, false, ACQUIRE, ACQUIRE))
    return 1;
  PAD64
  return (int)e + 2;
}

/* The success path in front: the failure target out of 16-bit reach. */
__attribute__((noinline)) int br_far_fail(uint32_t *p, uint32_t e, uint32_t d)
{
  if (!__atomic_compare_exchange_n(p, &e, d, false, ACQUIRE, ACQUIRE))
    return (int)e + 3;
  PAD64
  return 9;
}

/* The result only tested, the write-back dead: the kernel's shapes. */
__attribute__((noinline)) int br_take(uint32_t *l, uint32_t token)
{
  uint32_t expected = 0;
  if (!__atomic_compare_exchange_n(l, &expected, token, false, ACQUIRE, RELAXED))
    return 0;
  sink = sink + token;
  return 1;
}

/* A constant CMP's immediate holds, and one it cannot. */
__attribute__((noinline)) int br_take5(uint32_t *l, uint32_t token)
{
  uint32_t expected = 5;
  if (__atomic_compare_exchange_n(l, &expected, token, false, SEQ_CST, RELAXED))
    return 1;
  return 2;
}

__attribute__((noinline)) int br_take_big(uint32_t *l, uint32_t token)
{
  uint32_t expected = 0xabcdef01;
  if (!__atomic_compare_exchange_n(l, &expected, token, false, RELEASE, RELAXED))
    return 3;
  return 4;
}

/* Expected in a register that the failure path does not read again. */
__attribute__((noinline)) int br_take_reg(uint32_t *l, uint32_t e, uint32_t token)
{
  if (__atomic_compare_exchange_n(l, &e, token, false, ACQUIRE, ACQUIRE))
    return 10;
  return 11;
}

/* Spin until it is free: the failure branch goes back to the loop. */
__attribute__((noinline)) void br_spin(uint32_t *l, uint32_t token)
{
  uint32_t expected;
  do
    expected = 0;
  while (!__atomic_compare_exchange_n(l, &expected, token, true, ACQUIRE, RELAXED));
}

/* The success code past a 16-bit branch's reach, then the failure code. */
__attribute__((noinline)) int br_take_far(uint32_t *l, uint32_t token)
{
  uint32_t expected = 0;
  if (__atomic_compare_exchange_n(l, &expected, token, false, ACQUIRE, RELAXED))
  {
    PAD64
    return 1;
  }
  return 0;
}

/* The failure code past a 16-bit branch's reach. */
__attribute__((noinline)) int br_take_far_fail(uint32_t *l, uint32_t token)
{
  uint32_t expected = 0;
  if (!__atomic_compare_exchange_n(l, &expected, token, false, ACQUIRE, RELAXED))
  {
    PAD64
    return 0;
  }
  return 1;
}

int main(void)
{
  int tries, r;
  uint32_t m;
  word = 0;
  br_spin_lock(&word, 5);
  printf("lock: %u\n", word);
  r = br_trylock(&word, 6);
  printf("trylock busy: %d word=%u\n", r, word);
  word = 0;
  r = br_trylock(&word, 6);
  printf("trylock free: %d word=%u\n", r, word);
  word = 40;
  r = br_reg(&word, 40, 41);
  printf("reg hit: %d word=%u\n", r, word);
  r = br_reg(&word, 40, 42);
  printf("reg miss: %d word=%u\n", r, word);
  word = 0x12345678;
  r = br_big(&word, 3);
  printf("big hit: %d word=%x\n", r, word);
  r = br_big(&word, 4);
  printf("big miss: %d word=%x\n", r, word);
  word = 10;
  m = br_max(&word, 20, 10, &tries);
  printf("max: %u word=%u tries=%d\n", m, word, tries);
  m = br_max(&word, 15, 3, &tries);
  printf("max stale: %u word=%u tries=%d\n", m, word, tries);
  m = br_max(&word, 25, 0, &tries);
  printf("max stale up: %u word=%u tries=%d\n", m, word, tries);
  word = 50;
  sink = 0;
  r = br_far(&word, 50, 51);
  printf("far hit: %d word=%u sink=%u\n", r, word, sink);
  r = br_far(&word, 50, 52);
  printf("far miss: %d word=%u sink=%u\n", r, word, sink);
  sink = 0;
  r = br_far_fail(&word, 51, 60);
  printf("far fail hit: %d word=%u sink=%u\n", r, word, sink);
  r = br_far_fail(&word, 51, 61);
  printf("far fail miss: %d word=%u sink=%u\n", r, word, sink);
  word = 0;
  sink = 0;
  r = br_take(&word, 7);
  printf("take free: %d word=%u sink=%u\n", r, word, sink);
  r = br_take(&word, 8);
  printf("take busy: %d word=%u sink=%u\n", r, word, sink);
  word = 5;
  r = br_take5(&word, 6);
  printf("take5: %d word=%u", r, word);
  r = br_take5(&word, 7);
  printf(" %d word=%u\n", r, word);
  word = 0xabcdef01;
  r = br_take_big(&word, 1);
  printf("take big: %d word=%x", r, word);
  r = br_take_big(&word, 2);
  printf(" %d word=%x\n", r, word);
  r = br_take_reg(&word, 1, 9);
  printf("take reg: %d word=%u", r, word);
  r = br_take_reg(&word, 1, 10);
  printf(" %d word=%u\n", r, word);
  word = 0;
  br_spin(&word, 12);
  printf("spin: %u\n", word);
  word = 0;
  sink = 0;
  r = br_take_far(&word, 13);
  printf("take far: %d word=%u sink=%u", r, word, sink);
  r = br_take_far(&word, 14);
  printf(" %d word=%u sink=%u\n", r, word, sink);
  sink = 0;
  r = br_take_far_fail(&word, 15);
  printf("take far fail: %d word=%u sink=%u", r, word, sink);
  word = 0;
  r = br_take_far_fail(&word, 16);
  printf(" %d word=%u sink=%u\n", r, word, sink);
  return 0;
}
