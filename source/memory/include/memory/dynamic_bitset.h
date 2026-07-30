/*
 *  TCC Memory Utilities - Inline-first dynamic bitsets
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

#define TCC_DYNAMIC_BITSET_WORD_BITS 64

#define TCC_DYNAMIC_BITSET_DEFINE_WITH_ALLOCATOR(name, inline_bit_capacity, allocate, deallocate)                \
  typedef struct name                                                                                             \
  {                                                                                                               \
    size_t size;                                                                                                  \
    union                                                                                                         \
    {                                                                                                             \
      uint64_t *heap;                                                                                             \
      uint64_t inline_values[((inline_bit_capacity) + TCC_DYNAMIC_BITSET_WORD_BITS - 1) /                         \
                             TCC_DYNAMIC_BITSET_WORD_BITS];                                                       \
    } storage;                                                                                                    \
  } name;                                                                                                         \
                                                                                                                  \
  static inline size_t name##_word_count(const name *bitset)                                                      \
  {                                                                                                               \
    return bitset->size / TCC_DYNAMIC_BITSET_WORD_BITS +                                                         \
           (bitset->size % TCC_DYNAMIC_BITSET_WORD_BITS != 0);                                                    \
  }                                                                                                               \
                                                                                                                  \
  static inline int name##_is_inline(const name *bitset)                                                          \
  {                                                                                                               \
    return name##_word_count(bitset) <=                                                                           \
           ((inline_bit_capacity) + TCC_DYNAMIC_BITSET_WORD_BITS - 1) / TCC_DYNAMIC_BITSET_WORD_BITS;             \
  }                                                                                                               \
                                                                                                                  \
  static inline uint64_t *name##_data(name *bitset)                                                               \
  {                                                                                                               \
    return name##_is_inline(bitset) ? bitset->storage.inline_values : bitset->storage.heap;                        \
  }                                                                                                               \
                                                                                                                  \
  static inline const uint64_t *name##_const_data(const name *bitset)                                             \
  {                                                                                                               \
    return name##_is_inline(bitset) ? bitset->storage.inline_values : bitset->storage.heap;                        \
  }                                                                                                               \
                                                                                                                  \
  static inline int name##_init(name *bitset, size_t size)                                                        \
  {                                                                                                               \
    size_t word_count;                                                                                            \
                                                                                                                  \
    bitset->size = size;                                                                                          \
    word_count = name##_word_count(bitset);                                                                       \
    if (name##_is_inline(bitset)) {                                                                               \
      memset(bitset->storage.inline_values, 0, word_count * sizeof(uint64_t));                                     \
      return 0;                                                                                                   \
    }                                                                                                             \
    if (word_count > SIZE_MAX / sizeof(uint64_t)) {                                                               \
      bitset->storage.heap = NULL;                                                                                \
      bitset->size = 0;                                                                                           \
      return -1;                                                                                                  \
    }                                                                                                             \
    bitset->storage.heap = (uint64_t *)allocate(word_count * sizeof(uint64_t));                                    \
    if (bitset->storage.heap == NULL) {                                                                           \
      bitset->size = 0;                                                                                           \
      return -1;                                                                                                  \
    }                                                                                                             \
    memset(bitset->storage.heap, 0, word_count * sizeof(uint64_t));                                                \
    return 0;                                                                                                     \
  }                                                                                                               \
                                                                                                                  \
  static inline void name##_cleanup(name *bitset)                                                                 \
  {                                                                                                               \
    if (!name##_is_inline(bitset) && bitset->storage.heap != NULL) {                                              \
      deallocate(bitset->storage.heap);                                                                           \
    }                                                                                                             \
    bitset->storage.heap = NULL;                                                                                  \
    bitset->size = 0;                                                                                             \
  }                                                                                                               \
                                                                                                                  \
  static inline void name##_clear(name *bitset)                                                                   \
  {                                                                                                               \
    memset(name##_data(bitset), 0, name##_word_count(bitset) * sizeof(uint64_t));                                 \
  }                                                                                                               \
                                                                                                                  \
  static inline int name##_test(const name *bitset, size_t position)                                               \
  {                                                                                                               \
    if (position >= bitset->size) {                                                                               \
      return 0;                                                                                                   \
    }                                                                                                             \
    return (name##_const_data(bitset)[position / TCC_DYNAMIC_BITSET_WORD_BITS] >>                                 \
            (position % TCC_DYNAMIC_BITSET_WORD_BITS)) & 1;                                                       \
  }                                                                                                               \
                                                                                                                  \
  static inline void name##_set(name *bitset, size_t position)                                                    \
  {                                                                                                               \
    if (position < bitset->size) {                                                                                \
      name##_data(bitset)[position / TCC_DYNAMIC_BITSET_WORD_BITS] |=                                             \
          (uint64_t)1 << (position % TCC_DYNAMIC_BITSET_WORD_BITS);                                               \
    }                                                                                                             \
  }                                                                                                               \
                                                                                                                  \
  static inline void name##_reset(name *bitset, size_t position)                                                  \
  {                                                                                                               \
    if (position < bitset->size) {                                                                                \
      name##_data(bitset)[position / TCC_DYNAMIC_BITSET_WORD_BITS] &=                                             \
          ~((uint64_t)1 << (position % TCC_DYNAMIC_BITSET_WORD_BITS));                                            \
    }                                                                                                             \
  }                                                                                                               \
                                                                                                                  \
  static inline void name##_move(name *destination, name *source)                                                 \
  {                                                                                                               \
    size_t word_count;                                                                                            \
                                                                                                                  \
    if (destination == source) {                                                                                  \
      return;                                                                                                     \
    }                                                                                                             \
    name##_cleanup(destination);                                                                                  \
    destination->size = source->size;                                                                             \
    word_count = name##_word_count(source);                                                                       \
    if (name##_is_inline(source)) {                                                                               \
      memcpy(destination->storage.inline_values, source->storage.inline_values, word_count * sizeof(uint64_t));   \
    } else {                                                                                                      \
      destination->storage.heap = source->storage.heap;                                                           \
    }                                                                                                             \
    source->storage.heap = NULL;                                                                                  \
    source->size = 0;                                                                                             \
  }

#define TCC_DYNAMIC_BITSET_DEFINE(name, inline_bit_capacity)                                                      \
  TCC_DYNAMIC_BITSET_DEFINE_WITH_ALLOCATOR(name, inline_bit_capacity, tcc_mallocz, tcc_free)

#define dynamic_bitset(type) type __attribute__((cleanup(type##_cleanup)))

