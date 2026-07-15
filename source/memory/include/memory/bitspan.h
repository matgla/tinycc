/*
 *  TCC Memory Utilities - Word-level bitset span operations
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* Bulk SWAR operations over a run of 64-bit bitset words. The caller owns the
 * storage (a plain uint64_t[], a scoped_vector, or a bit_matrix row); these
 * helpers only touch the words. `n` is a word count, `bit` is a bit index. */

static inline void tcc_bitspan_zero(uint64_t *words, size_t n)
{
  for (size_t i = 0; i < n; i++) {
    words[i] = 0;
  }
}

static inline void tcc_bitspan_copy(uint64_t *destination, const uint64_t *source, size_t n)
{
  for (size_t i = 0; i < n; i++) {
    destination[i] = source[i];
  }
}

/* destination |= source */
static inline void tcc_bitspan_or(uint64_t *destination, const uint64_t *source, size_t n)
{
  for (size_t i = 0; i < n; i++) {
    destination[i] |= source[i];
  }
}

/* Dataflow transfer step: destination = gen | (in & ~kill). Returns 1 if any
 * word of destination changed (the fixpoint "changed" flag), 0 otherwise. */
static inline int tcc_bitspan_or_andnot(uint64_t *destination, const uint64_t *gen, const uint64_t *in,
                                        const uint64_t *kill, size_t n)
{
  int changed = 0;

  for (size_t i = 0; i < n; i++) {
    uint64_t value = gen[i] | (in[i] & ~kill[i]);
    if (value != destination[i]) {
      destination[i] = value;
      changed = 1;
    }
  }
  return changed;
}

/* popcount(a & b) across the span. */
static inline int tcc_bitspan_and_popcount(const uint64_t *a, const uint64_t *b, size_t n)
{
  int count = 0;

  for (size_t i = 0; i < n; i++) {
    count += __builtin_popcountll(a[i] & b[i]);
  }
  return count;
}

static inline void tcc_bitspan_set(uint64_t *words, size_t bit)
{
  words[bit >> 6] |= (uint64_t)1 << (bit & 63);
}

static inline void tcc_bitspan_reset(uint64_t *words, size_t bit)
{
  words[bit >> 6] &= ~((uint64_t)1 << (bit & 63));
}

static inline int tcc_bitspan_test(const uint64_t *words, size_t bit)
{
  return (int)((words[bit >> 6] >> (bit & 63)) & 1);
}

/* Iterate the set-bit indices of `words[0..n)` in ascending order, binding each
 * to `idx` for the following statement/block. Indices >= `limit` are skipped
 * (indices are ascending, so this bounds the scan to the logical bit count).
 * Not nestable within the same scope (fixed loop-variable names). */
#define tcc_bitspan_for_each_set(words, n, limit, idx)                                                              \
  for (size_t tcc_bitspan_word_ = 0; tcc_bitspan_word_ < (size_t)(n); tcc_bitspan_word_++)                          \
    for (uint64_t tcc_bitspan_bits_ = (words)[tcc_bitspan_word_]; tcc_bitspan_bits_;                                \
         tcc_bitspan_bits_ &= tcc_bitspan_bits_ - 1)                                                                \
      for (int idx = (int)(tcc_bitspan_word_ << 6) + __builtin_ctzll(tcc_bitspan_bits_),                            \
               tcc_bitspan_go_ = (idx) < (int)(limit);                                                              \
           tcc_bitspan_go_; tcc_bitspan_go_ = 0)
