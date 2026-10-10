/*
 *  TCC Tests - counted-down `for (; n >= K; n -= K)` loops
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License;
 * either version 2 of the License, or (at your option) any later version.
 */

/* ssa:decrement_to_carry rewrites these loops so the guard pre-decrements and
 * the latch CMP reads the body's counter value: every CMP then compares the
 * pre-value against K -- the subtraction the flag-setting SUBS performs -- and
 * the codegen CMP skip elides it (`subs; bcs` latch instead of
 * `subs; cmp; bcs`).  The counter must be read nowhere else: the body sees it
 * one K lower, and a counter read after the loop gets a +K fixup at the common
 * exit (the memset/memcpy counter-feeding-next-loop shape).  Signed `>=`,
 * unsigned `>`, empty bodies, pointer-bumping copy bodies, and the two
 * declination shapes (counter read in the body; counter read after with a
 * second loop on it) all pin the rewrite's iteration counts. */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

__attribute__((noinline)) int count_ge(unsigned n) {
  int hits = 0;
  for (; n >= 4; n -= 4)
    ++hits;
  return hits;
}

__attribute__((noinline)) int count_ge_signed(int n) {
  int hits = 0;
  for (; n >= 4; n -= 4)
    ++hits;
  return hits;
}

__attribute__((noinline)) int count_gt(unsigned n) {
  int hits = 0;
  for (; n > 4; n -= 4)
    ++hits;
  return hits;
}

__attribute__((noinline)) void copy_words_ge(uint32_t *dw, const uint32_t *sw, size_t n) {
  for (; n >= 16; n -= 16, dw += 4, sw += 4) {
    dw[0] = sw[0];
    dw[1] = sw[1];
    dw[2] = sw[2];
    dw[3] = sw[3];
  }
}

__attribute__((noinline)) int count_empty(unsigned n) {
  for (; n >= 4; n -= 4)
    ;
  return 1;
}

/* Counter read after the loop: the +K fixup at the exit must restore it. */
__attribute__((noinline)) int count_exit_value(unsigned n) {
  for (; n >= 4; n -= 4)
    ;
  return (int)n;
}

/* Two loops on one counter: each transformed loop's fixup feeds the next. */
__attribute__((noinline)) int count_two_loops(unsigned n) {
  int hits = 0;
  for (; n >= 8; n -= 8)
    hits += 1;
  for (; n >= 2; n -= 2)
    hits += 2;
  return hits;
}

/* Counter read in the body: the rewrite must decline (the body would see the
 * counter one K lower). */
__attribute__((noinline)) int count_body_reads(unsigned n) {
  int s = 0;
  for (; n >= 4; n -= 4)
    s += (int)n;
  return s;
}

static int ref_ge2(unsigned n);

static int ref_ge(unsigned n) {
  int h = 0;
  while (n >= 4) {
    h++;
    n -= 4;
  }
  return h;
}

static int ref_ge_signed(int n) {
  int h = 0;
  while (n >= 4) {
    h++;
    n -= 4;
  }
  return h;
}

static int ref_gt(unsigned n) {
  int h = 0;
  while (n > 4) {
    h++;
    n -= 4;
  }
  return h;
}

int main(void) {
  int bad = 0;

  for (unsigned n = 0; n <= 40; n++)
    if (count_ge(n) != ref_ge(n)) {
      printf("count_ge(%u)=%d want %d\n", n, count_ge(n), ref_ge(n));
      bad++;
    }
  /* Big enough to cross many K boundaries, small enough to finish at -O0
   * under TCG (the unsigned near-2^32 wrap edges would take ~1G iterations). */
  unsigned edge[] = { 0xFFFFu, 0x10000u, 0x10001u, 65532u, 1000000u };
  for (int i = 0; i < 5; i++)
    if (count_ge(edge[i]) != ref_ge(edge[i])) {
      printf("count_ge(%u) wrong\n", edge[i]);
      bad++;
    }
  for (int n = -8; n <= 24; n++)
    if (count_ge_signed(n) != ref_ge_signed(n)) {
      printf("count_ge_signed(%d)=%d want %d\n", n, count_ge_signed(n), ref_ge_signed(n));
      bad++;
    }
  for (unsigned n = 0; n <= 40; n++)
    if (count_gt(n) != ref_gt(n)) {
      printf("count_gt(%u) wrong\n", n);
      bad++;
    }

  uint32_t src[64], dst[64];
  for (int i = 0; i < 64; i++)
    src[i] = (uint32_t)i * 2654435761u;
  for (unsigned n = 0; n <= 64; n += 3) {
    for (int i = 0; i < 64; i++)
      dst[i] = 0;
    copy_words_ge(dst, src, n);
    unsigned words = (n / 16) * 4;
    for (int i = 0; i < 64; i++) {
      uint32_t want = (unsigned)i < words ? src[i] : 0;
      if (dst[i] != want) {
        printf("copy_words_ge(%u) dst[%d]=%u want %u\n", n, i, dst[i], want);
        bad++;
        break;
      }
    }
  }

  for (unsigned n = 0; n <= 33; n++) {
    if (count_empty(n) != 1) {
      bad++;
      break;
    }
    if (count_exit_value(n) != (int)(n % 4u)) {
      printf("count_exit_value(%u)=%d want %d\n", n, count_exit_value(n), (int)(n % 4u));
      bad++;
    }
    int want2 = ref_ge2(n);
    if (count_two_loops(n) != want2) {
      printf("count_two_loops(%u)=%d want %d\n", n, count_two_loops(n), want2);
      bad++;
    }
    int s = 0;
    for (unsigned m = n; m >= 4; m -= 4)
      s += (int)m;
    if (count_body_reads(n) != s) {
      printf("count_body_reads(%u) wrong\n", n);
      bad++;
    }
  }

  printf(bad ? "FAIL %d\n" : "OK %d\n", bad);
  return bad != 0;
}

static int ref_ge2(unsigned n) {
  int h = 0;
  while (n >= 8) {
    h += 1;
    n -= 8;
  }
  while (n >= 2) {
    h += 2;
    n -= 2;
  }
  return h;
}
