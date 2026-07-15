/*
 *  TCC Memory Utilities - Single-allocation 2-D bit matrix
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
#include <string.h>

#include "memory/bitspan.h"

/* A dense rows x columns bit matrix stored as one contiguous
 * rows*words_per_row uint64_t allocation. Rows are addressable as bitspans and
 * fed to the tcc_bitspan_* word kernels; per-row bits use the _set/_test/_reset
 * helpers. Instantiate a named type with TCC_BIT_MATRIX_DEFINE(name) after
 * including a header that declares tcc_mallocz/tcc_free, or _WITH_ALLOCATOR to
 * supply custom (allocate, deallocate) — the allocator need not zero, _init
 * clears the storage itself. */

#define TCC_BIT_MATRIX_DEFINE_WITH_ALLOCATOR(name, allocate, deallocate)                                            \
  typedef struct name                                                                                             \
  {                                                                                                               \
    uint64_t *words;                                                                                             \
    int rows;                                                                                                    \
    int words_per_row;                                                                                           \
  } name;                                                                                                         \
                                                                                                                  \
  static inline void name##_init_empty(name *matrix)                                                              \
  {                                                                                                               \
    matrix->words = NULL;                                                                                        \
    matrix->rows = 0;                                                                                            \
    matrix->words_per_row = 0;                                                                                   \
  }                                                                                                               \
                                                                                                                  \
  static inline int name##_init(name *matrix, int rows, int columns)                                              \
  {                                                                                                               \
    int words_per_row = (columns + 63) / 64;                                                                     \
    size_t total = (size_t)rows * (size_t)words_per_row;                                                         \
                                                                                                                  \
    name##_init_empty(matrix);                                                                                    \
    if (rows < 0 || columns < 0) {                                                                               \
      return -1;                                                                                                 \
    }                                                                                                            \
    if (words_per_row != 0 && total / (size_t)words_per_row != (size_t)rows) {                                    \
      return -1;                                                                                                 \
    }                                                                                                            \
    if (total > SIZE_MAX / sizeof(uint64_t)) {                                                                    \
      return -1;                                                                                                 \
    }                                                                                                            \
    if (total != 0) {                                                                                            \
      matrix->words = (uint64_t *)allocate(total * sizeof(uint64_t));                                             \
      if (matrix->words == NULL) {                                                                               \
        return -1;                                                                                               \
      }                                                                                                          \
      memset(matrix->words, 0, total * sizeof(uint64_t));                                                        \
    }                                                                                                            \
    matrix->rows = rows;                                                                                         \
    matrix->words_per_row = words_per_row;                                                                       \
    return 0;                                                                                                    \
  }                                                                                                              \
                                                                                                                  \
  static inline void name##_cleanup(name *matrix)                                                                 \
  {                                                                                                               \
    if (matrix->words != NULL) {                                                                                 \
      deallocate(matrix->words);                                                                                 \
    }                                                                                                            \
    name##_init_empty(matrix);                                                                                    \
  }                                                                                                              \
                                                                                                                  \
  static inline uint64_t *name##_row(name *matrix, int row)                                                       \
  {                                                                                                              \
    return matrix->words + (size_t)row * (size_t)matrix->words_per_row;                                          \
  }                                                                                                              \
                                                                                                                  \
  static inline const uint64_t *name##_const_row(const name *matrix, int row)                                     \
  {                                                                                                              \
    return matrix->words + (size_t)row * (size_t)matrix->words_per_row;                                          \
  }                                                                                                              \
                                                                                                                  \
  static inline void name##_set(name *matrix, int row, int bit)                                                   \
  {                                                                                                              \
    tcc_bitspan_set(name##_row(matrix, row), (size_t)bit);                                                       \
  }                                                                                                              \
                                                                                                                  \
  static inline void name##_reset(name *matrix, int row, int bit)                                                 \
  {                                                                                                              \
    tcc_bitspan_reset(name##_row(matrix, row), (size_t)bit);                                                     \
  }                                                                                                              \
                                                                                                                  \
  static inline int name##_test(const name *matrix, int row, int bit)                                             \
  {                                                                                                              \
    return tcc_bitspan_test(name##_const_row(matrix, row), (size_t)bit);                                         \
  }

#define TCC_BIT_MATRIX_DEFINE(name) TCC_BIT_MATRIX_DEFINE_WITH_ALLOCATOR(name, tcc_mallocz, tcc_free)

#define bit_matrix(name) name __attribute__((cleanup(name##_cleanup)))
