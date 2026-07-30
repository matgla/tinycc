/*
 *  TCC Memory Utilities - Inline-first typed sequences
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

#define TCC_SMALL_SEQUENCE_DEFINE_WITH_ALLOCATOR(name, type, inline_capacity, allocate, deallocate)               \
  typedef struct name                                                                                             \
  {                                                                                                               \
    size_t size;                                                                                                  \
    union                                                                                                         \
    {                                                                                                             \
      type *heap;                                                                                                 \
      type inline_values[(inline_capacity)];                                                                      \
    } storage;                                                                                                    \
  } name;                                                                                                         \
                                                                                                                  \
  static inline int name##_is_inline(const name *sequence)                                                        \
  {                                                                                                               \
    return sequence->size <= (inline_capacity);                                                                   \
  }                                                                                                               \
                                                                                                                  \
  static inline type *name##_data(name *sequence)                                                                 \
  {                                                                                                               \
    return name##_is_inline(sequence) ? sequence->storage.inline_values : sequence->storage.heap;                  \
  }                                                                                                               \
                                                                                                                  \
  static inline const type *name##_const_data(const name *sequence)                                               \
  {                                                                                                               \
    return name##_is_inline(sequence) ? sequence->storage.inline_values : sequence->storage.heap;                  \
  }                                                                                                               \
                                                                                                                  \
  static inline int name##_init(name *sequence, size_t size)                                                       \
  {                                                                                                               \
    sequence->size = size;                                                                                        \
    if (size > SIZE_MAX / sizeof(type)) {                                                                         \
      sequence->storage.heap = NULL;                                                                              \
      return -1;                                                                                                  \
    }                                                                                                             \
    if (name##_is_inline(sequence)) {                                                                             \
      memset(sequence->storage.inline_values, 0, size * sizeof(type));                                             \
      return 0;                                                                                                   \
    }                                                                                                             \
    sequence->storage.heap = (type *)allocate(size * sizeof(type));                                               \
    if (sequence->storage.heap == NULL) {                                                                         \
      return -1;                                                                                                  \
    }                                                                                                             \
    memset(sequence->storage.heap, 0, size * sizeof(type));                                                       \
    return 0;                                                                                                     \
  }                                                                                                               \
                                                                                                                  \
  static inline void name##_cleanup(name *sequence)                                                               \
  {                                                                                                               \
    if (!name##_is_inline(sequence) && sequence->storage.heap != NULL) {                                           \
      deallocate(sequence->storage.heap);                                                                         \
    }                                                                                                             \
    sequence->storage.heap = NULL;                                                                                \
    sequence->size = 0;                                                                                           \
  }                                                                                                               \
                                                                                                                  \
  static inline void name##_move(name *destination, name *source)                                                 \
  {                                                                                                               \
    if (destination == source) {                                                                                  \
      return;                                                                                                     \
    }                                                                                                             \
    name##_cleanup(destination);                                                                                  \
    destination->size = source->size;                                                                             \
    if (name##_is_inline(source)) {                                                                               \
      memcpy(destination->storage.inline_values, source->storage.inline_values, source->size * sizeof(type));     \
    } else {                                                                                                      \
      destination->storage.heap = source->storage.heap;                                                           \
    }                                                                                                             \
    source->storage.heap = NULL;                                                                                  \
    source->size = 0;                                                                                             \
  }

#define TCC_SMALL_SEQUENCE_DEFINE(name, type, inline_capacity)                                                     \
  TCC_SMALL_SEQUENCE_DEFINE_WITH_ALLOCATOR(name, type, inline_capacity, tcc_mallocz, tcc_free)

#define small_sequence(type) type __attribute__((cleanup(type##_cleanup)))

