/*
 *  TCC Memory Utilities - Typed dynamic arrays
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

void tcc_vector_cleanup_storage(void *data_owner);
int tcc_vector_reserve_storage(void *data_owner, size_t *capacity, size_t element_size, size_t requested_capacity);
int tcc_vector_resize_storage(void *data_owner, size_t *size, size_t *capacity, size_t element_size,
                              size_t requested_size);
int tcc_vector_push_back_storage(void *data_owner, size_t *size, size_t *capacity, size_t element_size,
                                 const void *value);
int tcc_vector_pop_back_storage(const void *data_owner, size_t *size, size_t element_size, void *value);
int tcc_vector_insert_storage(void *data_owner, size_t *size, size_t *capacity, size_t element_size, size_t index,
                              const void *value);
int tcc_vector_erase_storage(void *data_owner, size_t *size, size_t element_size, size_t index);
int tcc_vector_shrink_to_fit_storage(void *data_owner, size_t size, size_t *capacity, size_t element_size);

#define TCC_VECTOR_DEFINE_WITH_ALLOCATOR(name, type, reallocate, deallocate)                                       \
  typedef struct name                                                                                             \
  {                                                                                                               \
    type *data;                                                                                                   \
    size_t size;                                                                                                  \
    size_t capacity;                                                                                              \
  } name;                                                                                                         \
                                                                                                                  \
  static inline void name##_init(name *vector)                                                                    \
  {                                                                                                               \
    vector->data = NULL;                                                                                          \
    vector->size = 0;                                                                                             \
    vector->capacity = 0;                                                                                         \
  }                                                                                                               \
                                                                                                                  \
  static inline void name##_cleanup(name *vector)                                                                 \
  {                                                                                                               \
    if (vector->data != NULL) {                                                                                   \
      deallocate(vector->data);                                                                                   \
    }                                                                                                             \
    name##_init(vector);                                                                                          \
  }                                                                                                               \
                                                                                                                  \
  static inline int name##_empty(const name *vector)                                                              \
  {                                                                                                               \
    return vector->size == 0;                                                                                     \
  }                                                                                                               \
                                                                                                                  \
  static inline void name##_clear(name *vector)                                                                   \
  {                                                                                                               \
    vector->size = 0;                                                                                             \
  }                                                                                                               \
                                                                                                                  \
  static inline int name##_reserve(name *vector, size_t capacity)                                                 \
  {                                                                                                               \
    type *data;                                                                                                   \
                                                                                                                  \
    if (capacity <= vector->capacity) {                                                                           \
      return 0;                                                                                                   \
    }                                                                                                             \
    if (capacity > SIZE_MAX / sizeof(type)) {                                                                     \
      return -1;                                                                                                  \
    }                                                                                                             \
    data = (type *)reallocate(vector->data, capacity * sizeof(type));                                              \
    if (data == NULL) {                                                                                           \
      return -1;                                                                                                  \
    }                                                                                                             \
    vector->data = data;                                                                                          \
    vector->capacity = capacity;                                                                                  \
    return 0;                                                                                                     \
  }                                                                                                               \
                                                                                                                  \
  static inline int name##_grow(name *vector, size_t minimum_capacity)                                            \
  {                                                                                                               \
    size_t capacity;                                                                                              \
                                                                                                                  \
    if (minimum_capacity <= vector->capacity) {                                                                   \
      return 0;                                                                                                   \
    }                                                                                                             \
    capacity = vector->capacity == 0 ? 4 : vector->capacity;                                                      \
    while (capacity < minimum_capacity) {                                                                         \
      if (capacity > SIZE_MAX / 2) {                                                                              \
        capacity = minimum_capacity;                                                                              \
        break;                                                                                                    \
      }                                                                                                           \
      capacity *= 2;                                                                                              \
    }                                                                                                             \
    return name##_reserve(vector, capacity);                                                                      \
  }                                                                                                               \
                                                                                                                  \
  static inline int name##_resize(name *vector, size_t size)                                                      \
  {                                                                                                               \
    size_t old_size = vector->size;                                                                               \
                                                                                                                  \
    if (size > old_size) {                                                                                        \
      if (name##_grow(vector, size) != 0) {                                                                       \
        return -1;                                                                                                \
      }                                                                                                           \
      memset(vector->data + old_size, 0, (size - old_size) * sizeof(type));                                       \
    }                                                                                                             \
    vector->size = size;                                                                                          \
    return 0;                                                                                                     \
  }                                                                                                               \
                                                                                                                  \
  static inline int name##_push_back(name *vector, type value)                                                    \
  {                                                                                                               \
    if (vector->size == SIZE_MAX || name##_grow(vector, vector->size + 1) != 0) {                                 \
      return -1;                                                                                                  \
    }                                                                                                             \
    vector->data[vector->size++] = value;                                                                         \
    return 0;                                                                                                     \
  }                                                                                                               \
                                                                                                                  \
  static inline int name##_pop_back(name *vector, type *value)                                                    \
  {                                                                                                               \
    if (name##_empty(vector)) {                                                                                   \
      return -1;                                                                                                  \
    }                                                                                                             \
    vector->size--;                                                                                               \
    if (value != NULL) {                                                                                          \
      *value = vector->data[vector->size];                                                                        \
    }                                                                                                             \
    return 0;                                                                                                     \
  }                                                                                                               \
                                                                                                                  \
  static inline int name##_insert(name *vector, size_t index, type value)                                         \
  {                                                                                                               \
    if (index > vector->size || vector->size == SIZE_MAX ||                                                       \
        name##_grow(vector, vector->size + 1) != 0) {                                                             \
      return -1;                                                                                                  \
    }                                                                                                             \
    memmove(vector->data + index + 1, vector->data + index, (vector->size - index) * sizeof(type));                \
    vector->data[index] = value;                                                                                  \
    vector->size++;                                                                                               \
    return 0;                                                                                                     \
  }                                                                                                               \
                                                                                                                  \
  static inline int name##_erase(name *vector, size_t index)                                                      \
  {                                                                                                               \
    if (index >= vector->size) {                                                                                  \
      return -1;                                                                                                  \
    }                                                                                                             \
    memmove(vector->data + index, vector->data + index + 1, (vector->size - index - 1) * sizeof(type));            \
    vector->size--;                                                                                               \
    return 0;                                                                                                     \
  }                                                                                                               \
                                                                                                                  \
  static inline int name##_shrink_to_fit(name *vector)                                                           \
  {                                                                                                               \
    type *data;                                                                                                   \
                                                                                                                  \
    if (vector->size == vector->capacity) {                                                                       \
      return 0;                                                                                                   \
    }                                                                                                             \
    if (vector->size == 0) {                                                                                      \
      name##_cleanup(vector);                                                                                     \
      return 0;                                                                                                   \
    }                                                                                                             \
    data = (type *)reallocate(vector->data, vector->size * sizeof(type));                                          \
    if (data == NULL) {                                                                                           \
      return -1;                                                                                                  \
    }                                                                                                             \
    vector->data = data;                                                                                          \
    vector->capacity = vector->size;                                                                              \
    return 0;                                                                                                     \
  }

#define TCC_VECTOR_DEFINE(name, type) TCC_VECTOR_DEFINE_WITH_ALLOCATOR(name, type, tcc_realloc, tcc_free)

#define vector(type)                                                                                              \
  struct                                                                                                          \
  {                                                                                                               \
    type *data;                                                                                                   \
    size_t size;                                                                                                  \
    size_t capacity;                                                                                              \
  }

#define scoped_vector(type) __attribute__((cleanup(tcc_vector_cleanup_storage))) vector(type)
#define scoped_named_vector(name) name __attribute__((cleanup(name##_cleanup)))

#define vector_init(owner)                                                                                        \
  do {                                                                                                            \
    (owner)->data = NULL;                                                                                         \
    (owner)->size = 0;                                                                                            \
    (owner)->capacity = 0;                                                                                        \
  } while (0)

#define vector_cleanup(owner)                                                                                     \
  do {                                                                                                            \
    tcc_vector_cleanup_storage(&(owner)->data);                                                                   \
    vector_init(owner);                                                                                           \
  } while (0)

#define vector_data(owner) ((owner)->data)
#define vector_empty(owner) ((owner)->size == 0)
#define vector_clear(owner) ((owner)->size = 0)
#define vector_reserve(owner, requested_capacity)                                                                 \
  tcc_vector_reserve_storage(&(owner)->data, &(owner)->capacity, sizeof(*(owner)->data), (requested_capacity))
#define vector_resize(owner, requested_size)                                                                      \
  tcc_vector_resize_storage(&(owner)->data, &(owner)->size, &(owner)->capacity, sizeof(*(owner)->data),            \
                            (requested_size))
#define vector_pop_back(owner, value)                                                                             \
  tcc_vector_pop_back_storage(&(owner)->data, &(owner)->size, sizeof(*(owner)->data), (value))
#define vector_erase(owner, index)                                                                                \
  tcc_vector_erase_storage(&(owner)->data, &(owner)->size, sizeof(*(owner)->data), (index))
#define vector_shrink_to_fit(owner)                                                                               \
  tcc_vector_shrink_to_fit_storage(&(owner)->data, (owner)->size, &(owner)->capacity, sizeof(*(owner)->data))

#define vector_push_back(owner, value)                                                                            \
  __extension__({                                                                                                 \
    __typeof__(owner) _tcc_vector_owner = (owner);                                                                \
    __typeof__(*(_tcc_vector_owner)->data) _tcc_vector_value = (value);                                            \
    tcc_vector_push_back_storage(&(_tcc_vector_owner)->data, &(_tcc_vector_owner)->size,                           \
                                 &(_tcc_vector_owner)->capacity, sizeof(*(_tcc_vector_owner)->data),               \
                                 &_tcc_vector_value);                                                              \
  })

#define vector_insert(owner, index, value)                                                                        \
  __extension__({                                                                                                 \
    __typeof__(owner) _tcc_vector_owner = (owner);                                                                \
    __typeof__(*(_tcc_vector_owner)->data) _tcc_vector_value = (value);                                            \
    tcc_vector_insert_storage(&(_tcc_vector_owner)->data, &(_tcc_vector_owner)->size,                              \
                              &(_tcc_vector_owner)->capacity, sizeof(*(_tcc_vector_owner)->data), (index),         \
                              &_tcc_vector_value);                                                                 \
  })

