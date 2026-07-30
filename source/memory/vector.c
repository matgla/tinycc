/*
 *  TCC Memory Utilities - Dynamic array storage
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "tcc.h"

#include "memory/vector.h"

static void *tcc_vector_load_data(const void *owner)
{
  void *data;

  memcpy(&data, owner, sizeof(data));
  return data;
}

static void tcc_vector_store_data(void *owner, void *data)
{
  memcpy(owner, &data, sizeof(data));
}

void tcc_vector_cleanup_storage(void *data_owner)
{
  tcc_free(tcc_vector_load_data(data_owner));
}

int tcc_vector_reserve_storage(void *data_owner, size_t *capacity, size_t element_size, size_t requested_capacity)
{
  void *data;

  if (requested_capacity <= *capacity) {
    return 0;
  }
  if (requested_capacity > SIZE_MAX / element_size) {
    return -1;
  }
  data = tcc_realloc(tcc_vector_load_data(data_owner), requested_capacity * element_size);
  if (data == NULL) {
    return -1;
  }
  tcc_vector_store_data(data_owner, data);
  *capacity = requested_capacity;
  return 0;
}

static int tcc_vector_grow_storage(void *data_owner, size_t *capacity, size_t element_size,
                                   size_t minimum_capacity)
{
  size_t new_capacity;

  if (minimum_capacity <= *capacity) {
    return 0;
  }
  new_capacity = *capacity == 0 ? 4 : *capacity;
  while (new_capacity < minimum_capacity) {
    if (new_capacity > SIZE_MAX / 2) {
      new_capacity = minimum_capacity;
      break;
    }
    new_capacity *= 2;
  }
  return tcc_vector_reserve_storage(data_owner, capacity, element_size, new_capacity);
}

int tcc_vector_resize_storage(void *data_owner, size_t *size, size_t *capacity, size_t element_size,
                              size_t requested_size)
{
  size_t old_size = *size;

  if (requested_size > old_size) {
    if (tcc_vector_grow_storage(data_owner, capacity, element_size, requested_size) != 0) {
      return -1;
    }
    memset((unsigned char *)tcc_vector_load_data(data_owner) + old_size * element_size, 0,
           (requested_size - old_size) * element_size);
  }
  *size = requested_size;
  return 0;
}

int tcc_vector_push_back_storage(void *data_owner, size_t *size, size_t *capacity, size_t element_size,
                                 const void *value)
{
  void *data;

  if (*size == SIZE_MAX || tcc_vector_grow_storage(data_owner, capacity, element_size, *size + 1) != 0) {
    return -1;
  }
  data = tcc_vector_load_data(data_owner);
  memcpy((unsigned char *)data + *size * element_size, value, element_size);
  (*size)++;
  return 0;
}

int tcc_vector_pop_back_storage(const void *data_owner, size_t *size, size_t element_size, void *value)
{
  if (*size == 0) {
    return -1;
  }
  (*size)--;
  if (value != NULL) {
    memcpy(value, (const unsigned char *)tcc_vector_load_data(data_owner) + *size * element_size, element_size);
  }
  return 0;
}

int tcc_vector_insert_storage(void *data_owner, size_t *size, size_t *capacity, size_t element_size, size_t index,
                              const void *value)
{
  unsigned char *data;

  if (index > *size || *size == SIZE_MAX ||
      tcc_vector_grow_storage(data_owner, capacity, element_size, *size + 1) != 0) {
    return -1;
  }
  data = (unsigned char *)tcc_vector_load_data(data_owner);
  memmove(data + (index + 1) * element_size, data + index * element_size, (*size - index) * element_size);
  memcpy(data + index * element_size, value, element_size);
  (*size)++;
  return 0;
}

int tcc_vector_erase_storage(void *data_owner, size_t *size, size_t element_size, size_t index)
{
  unsigned char *data;

  if (index >= *size) {
    return -1;
  }
  data = (unsigned char *)tcc_vector_load_data(data_owner);
  memmove(data + index * element_size, data + (index + 1) * element_size, (*size - index - 1) * element_size);
  (*size)--;
  return 0;
}

int tcc_vector_shrink_to_fit_storage(void *data_owner, size_t size, size_t *capacity, size_t element_size)
{
  void *data;

  if (size == *capacity) {
    return 0;
  }
  if (size == 0) {
    tcc_vector_cleanup_storage(data_owner);
    tcc_vector_store_data(data_owner, NULL);
    *capacity = 0;
    return 0;
  }
  data = tcc_realloc(tcc_vector_load_data(data_owner), size * element_size);
  if (data == NULL) {
    return -1;
  }
  tcc_vector_store_data(data_owner, data);
  *capacity = size;
  return 0;
}
