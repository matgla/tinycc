/*
 *  TCC IR - atomic memory orders and compare-exchange shape
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* Seq_cst loads/stores are LDA/STL on ARMv8-M (as acquire/release are) and a
 * seq_cst read-modify-write is LDAEX/STLEX with no DMB; an inline CAS is
 * `ldaex; cmp; bne; strex; cmp; bne top` with the CLREX on the failure path
 * only.  Every order and width must still load, store, sign-extend and write
 * back the same values; test_atomic_cas_seqcst_codegen.py checks the shapes. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define RELAXED 0
#define CONSUME 1
#define ACQUIRE 2
#define RELEASE 3
#define ACQ_REL 4
#define SEQ_CST 5

uint8_t u8v;
int8_t s8v;
uint16_t u16v;
int16_t s16v;
uint32_t u32v;
int32_t s32v;
uint32_t lock_word;

#define LOADS(T, NAME)                                                                                       \
  __attribute__((noinline)) T NAME##_ld_relaxed(T *p) { return __atomic_load_n(p, RELAXED); }             \
  __attribute__((noinline)) T NAME##_ld_consume(T *p) { return __atomic_load_n(p, CONSUME); }             \
  __attribute__((noinline)) T NAME##_ld_acquire(T *p) { return __atomic_load_n(p, ACQUIRE); }             \
  __attribute__((noinline)) T NAME##_ld_seq_cst(T *p) { return __atomic_load_n(p, SEQ_CST); }             \
  __attribute__((noinline)) T NAME##_ld_acq_rel(T *p) { return __atomic_load_n(p, ACQ_REL); }             \
  __attribute__((noinline)) T NAME##_ld_with(T *p, int o)                                                   \
  {                                                                                                         \
    T r;                                                                                                    \
    __atomic_load(p, &r, o);                                                                                \
    return r;                                                                                               \
  }                                                                                                         \
  __attribute__((noinline)) void NAME##_st_relaxed(T *p, T v) { __atomic_store_n(p, v, RELAXED); }        \
  __attribute__((noinline)) void NAME##_st_release(T *p, T v) { __atomic_store_n(p, v, RELEASE); }        \
  __attribute__((noinline)) void NAME##_st_seq_cst(T *p, T v) { __atomic_store_n(p, v, SEQ_CST); }        \
  __attribute__((noinline)) void NAME##_st_with(T *p, T v, int o) { __atomic_store(p, &v, o); }           \
  __attribute__((noinline)) T NAME##_add_relaxed(T *p, T v) { return __atomic_fetch_add(p, v, RELAXED); } \
  __attribute__((noinline)) T NAME##_add_acquire(T *p, T v) { return __atomic_fetch_add(p, v, ACQUIRE); } \
  __attribute__((noinline)) T NAME##_add_release(T *p, T v) { return __atomic_fetch_add(p, v, RELEASE); } \
  __attribute__((noinline)) T NAME##_add_acq_rel(T *p, T v) { return __atomic_fetch_add(p, v, ACQ_REL); } \
  __attribute__((noinline)) T NAME##_add_seq_cst(T *p, T v) { return __atomic_fetch_add(p, v, SEQ_CST); } \
  __attribute__((noinline)) T NAME##_xchg_seq_cst(T *p, T v) { return __atomic_exchange_n(p, v, SEQ_CST); } \
  __attribute__((noinline)) bool NAME##_cas_strong(T *p, T *e, T d)                                         \
  {                                                                                                         \
    return __atomic_compare_exchange_n(p, e, d, false, SEQ_CST, SEQ_CST);                                   \
  }                                                                                                         \
  __attribute__((noinline)) bool NAME##_cas_weak(T *p, T *e, T d)                                           \
  {                                                                                                         \
    return __atomic_compare_exchange_n(p, e, d, true, ACQUIRE, RELAXED);                                    \
  }                                                                                                         \
  __attribute__((noinline)) bool NAME##_cas_release(T *p, T *e, T d)                                        \
  {                                                                                                         \
    return __atomic_compare_exchange_n(p, e, d, false, RELEASE, RELAXED);                                   \
  }                                                                                                         \
  __attribute__((noinline)) bool NAME##_cas_relaxed(T *p, T *e, T d)                                        \
  {                                                                                                         \
    return __atomic_compare_exchange_n(p, e, d, false, RELAXED, RELAXED);                                   \
  }

LOADS(uint8_t, u8)
LOADS(int8_t, s8)
LOADS(uint16_t, u16)
LOADS(int16_t, s16)
LOADS(uint32_t, u32)
LOADS(int32_t, s32)

/* The kernel's SpinLock.lock: a weak acquire CAS against 0, WFE on failure. */
__attribute__((noinline)) void spin_lock(uint32_t *l, uint32_t token)
{
  for (;;)
  {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(l, &expected, token, true, ACQUIRE, RELAXED))
      return;
    __asm volatile("wfe" ::: "memory");
  }
}

__attribute__((noinline)) void spin_unlock(uint32_t *l) { __atomic_store_n(l, 0, RELEASE); }

/* A CAS whose result is the old value (Zig's cmpxchg optional). */
__attribute__((noinline)) uint32_t cas_old(uint32_t *p, uint32_t e, uint32_t d)
{
  __atomic_compare_exchange_n(p, &e, d, false, SEQ_CST, SEQ_CST);
  return e;
}

/* Expected a constant CMP cannot hold, and one it can. */
__attribute__((noinline)) bool cas_big_const(uint32_t *p, uint32_t d)
{
  uint32_t e = 0x12345678;
  return __atomic_compare_exchange_n(p, &e, d, false, SEQ_CST, RELAXED);
}

__attribute__((noinline)) bool cas_small_const(uint32_t *p, uint32_t d)
{
  uint32_t e = 200;
  return __atomic_compare_exchange_n(p, &e, d, false, ACQ_REL, ACQUIRE);
}

/* Expected 0 (CBNZ out of the loop), failing and succeeding. */
__attribute__((noinline)) uint32_t cas_zero(uint32_t *p, uint32_t d)
{
  uint32_t e = 0;
  if (__atomic_compare_exchange_n(p, &e, d, false, ACQUIRE, RELAXED))
    return 1000;
  return e;
}

/* The result dropped. */
__attribute__((noinline)) void cas_void(uint32_t *p, uint32_t e, uint32_t d)
{
  __atomic_compare_exchange_n(p, &e, d, false, SEQ_CST, SEQ_CST);
}

/* A seqlock reader and writer (the kernel's Seq64). */
struct seq64
{
  uint32_t seq, lo, hi;
};
struct seq64 clock64;

__attribute__((noinline)) uint64_t seq_load(struct seq64 *s)
{
  for (;;)
  {
    uint32_t before = __atomic_load_n(&s->seq, SEQ_CST);
    if (before & 1)
      continue;
    uint32_t lo = __atomic_load_n(&s->lo, RELAXED);
    uint32_t hi = __atomic_load_n(&s->hi, RELAXED);
    if (__atomic_load_n(&s->seq, SEQ_CST) == before)
      return (uint64_t)hi << 32 | lo;
  }
}

__attribute__((noinline)) void seq_store(struct seq64 *s, uint64_t v)
{
  uint32_t start = __atomic_load_n(&s->seq, RELAXED);
  __atomic_store_n(&s->seq, start + 1, SEQ_CST);
  __atomic_store_n(&s->lo, (uint32_t)v, RELAXED);
  __atomic_store_n(&s->hi, (uint32_t)(v >> 32), RELAXED);
  __atomic_store_n(&s->seq, start + 2, SEQ_CST);
}

#define CHECK(T, NAME, VAR, A, B, FMT)                                                                       \
  do                                                                                                        \
  {                                                                                                         \
    T e;                                                                                                    \
    NAME##_st_relaxed(&VAR, (T)(A));                                                                        \
    printf(#NAME " ld: " FMT " " FMT " " FMT " " FMT " " FMT, NAME##_ld_relaxed(&VAR), NAME##_ld_consume(&VAR), \
           NAME##_ld_acquire(&VAR), NAME##_ld_seq_cst(&VAR), NAME##_ld_acq_rel(&VAR));                       \
    printf(" with: " FMT " " FMT " " FMT "\n", NAME##_ld_with(&VAR, SEQ_CST), NAME##_ld_with(&VAR, RELAXED),  \
           NAME##_ld_with(&VAR, 9));                                                                        \
    NAME##_st_release(&VAR, (T)(B));                                                                        \
    printf(#NAME " st: " FMT, VAR);                                                                         \
    NAME##_st_seq_cst(&VAR, (T)(A));                                                                        \
    printf(" " FMT, VAR);                                                                                   \
    NAME##_st_with(&VAR, (T)(B), SEQ_CST);                                                                  \
    printf(" " FMT, VAR);                                                                                   \
    NAME##_st_with(&VAR, (T)(A), 1);                                                                        \
    printf(" " FMT "\n", VAR);                                                                              \
    T r1 = NAME##_add_relaxed(&VAR, 1);                                                                     \
    T r2 = NAME##_add_acquire(&VAR, 2);                                                                     \
    T r3 = NAME##_add_release(&VAR, 3);                                                                     \
    T r4 = NAME##_add_acq_rel(&VAR, 4);                                                                     \
    T r5 = NAME##_add_seq_cst(&VAR, (T)(B));                                                                \
    T r6 = NAME##_xchg_seq_cst(&VAR, (T)(A));                                                               \
    printf(#NAME " rmw: " FMT " " FMT " " FMT " " FMT " " FMT " " FMT " -> " FMT "\n", r1, r2, r3, r4, r5, r6, VAR); \
    e = (T)(A);                                                                                             \
    int k1 = NAME##_cas_strong(&VAR, &e, (T)(B));                                                           \
    printf(#NAME " cas: %d e=" FMT " v=" FMT, k1, e, VAR);                                                  \
    int k2 = NAME##_cas_strong(&VAR, &e, (T)(A));                                                           \
    printf(" | %d e=" FMT " v=" FMT, k2, e, VAR);                                                           \
    int k3 = NAME##_cas_weak(&VAR, &e, (T)(A));                                                             \
    printf(" | %d e=" FMT " v=" FMT, k3, e, VAR);                                                           \
    int k4 = NAME##_cas_weak(&VAR, &e, (T)(B));                                                             \
    printf(" | %d e=" FMT " v=" FMT, k4, e, VAR);                                                           \
    e = (T)(B);                                                                                             \
    int k5 = NAME##_cas_release(&VAR, &e, (T)(A));                                                          \
    int k6 = NAME##_cas_release(&VAR, &e, (T)(B));                                                          \
    printf(" | %d %d e=" FMT " v=" FMT, k5, k6, e, VAR);                                                    \
    e = (T)(A);                                                                                             \
    int k7 = NAME##_cas_relaxed(&VAR, &e, (T)(B));                                                          \
    int k8 = NAME##_cas_relaxed(&VAR, &e, (T)(B));                                                          \
    printf(" | %d %d e=" FMT " v=" FMT "\n", k7, k8, e, VAR);                                               \
  } while (0)

int main(void)
{
  CHECK(uint8_t, u8, u8v, 0xF3, 0x81, "%u");
  CHECK(int8_t, s8, s8v, -13, -127, "%d");
  CHECK(uint16_t, u16, u16v, 0xFFF3, 0x8001, "%u");
  CHECK(int16_t, s16, s16v, -13, -32767, "%d");
  CHECK(uint32_t, u32, u32v, 0xFFFFFFF3u, 0x80000001u, "%u");
  CHECK(int32_t, s32, s32v, -13, -2147483647, "%d");

  /* Inline in a caller: a CAS whose success feeds a branch and a value. */
  uint32_t w = 7, e = 7;
  int hits = 0;
  for (int i = 0; i < 5; i++)
    if (__atomic_compare_exchange_n(&w, &e, w + 1, i & 1, SEQ_CST, RELAXED))
      hits++;
  printf("loop: hits=%d w=%u e=%u\n", hits, w, e);

  spin_lock(&lock_word, 5);
  printf("locked: %u\n", lock_word);
  spin_unlock(&lock_word);
  spin_lock(&lock_word, 6);
  printf("relocked: %u\n", lock_word);
  spin_unlock(&lock_word);

  uint32_t x = 40;
  uint32_t o1 = cas_old(&x, 40, 41);
  uint32_t o2 = cas_old(&x, 40, 42);
  printf("cas_old: %u %u x=%u\n", o1, o2, x);
  x = 0x12345678;
  int b1 = cas_big_const(&x, 1);
  int b2 = cas_big_const(&x, 2);
  printf("big: %d %d x=%u\n", b1, b2, x);
  x = 200;
  int c1 = cas_small_const(&x, 3);
  int c2 = cas_small_const(&x, 4);
  printf("small: %d %d x=%u\n", c1, c2, x);
  x = 5;
  uint32_t z1 = cas_zero(&x, 7);
  uint32_t z2 = x;
  x = 0;
  uint32_t z3 = cas_zero(&x, 7);
  printf("zero: %u %u %u x=%u\n", z1, z2, z3, x);
  x = 3;
  cas_void(&x, 3, 9);
  cas_void(&x, 3, 10);
  printf("void: x=%u\n", x);

  seq_store(&clock64, 0x1122334455667788ull);
  uint64_t t = seq_load(&clock64);
  printf("seq: %08x%08x seq=%u\n", (unsigned)(t >> 32), (unsigned)t, clock64.seq);
  seq_store(&clock64, 0xffffffff00000001ull);
  t = seq_load(&clock64);
  printf("seq: %08x%08x seq=%u\n", (unsigned)(t >> 32), (unsigned)t, clock64.seq);
  return 0;
}
