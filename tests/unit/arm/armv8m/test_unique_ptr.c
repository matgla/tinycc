/*
 *  TCC Memory Utilities - Unique pointer unit tests
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "memory/unique_ptr.h"

#include "ut.h"

#include <stdlib.h>

typedef struct Payload
{
  int value;
} Payload;

static int free_call_count;

void __real_tcc_free(void *pointer);

void __wrap_tcc_free(void *pointer)
{
  free_call_count++;
  __real_tcc_free(pointer);
}

static Payload *payload_create(int value)
{
  Payload *payload = malloc(sizeof(*payload));

  if (payload != NULL) {
    payload->value = value;
  }
  return payload;
}

static int create_scoped_owner(void)
{
  unique_ptr(Payload) owner = payload_create(7);

  UT_ASSERT(owner != NULL);
  UT_ASSERT_EQ(owner->value, 7);
  return 0;
}

UT_TEST(test_unique_ptr_cleanup_runs_at_scope_exit)
{
  free_call_count = 0;

  UT_ASSERT_EQ(create_scoped_owner(), 0);

  UT_ASSERT_EQ(free_call_count, 1);
  return 0;
}

UT_TEST(test_unique_ptr_cleanup_clears_and_frees_owner)
{
  Payload *owner = payload_create(11);

  UT_ASSERT(owner != NULL);
  free_call_count = 0;

  tcc_unique_ptr_cleanup(&owner);

  UT_ASSERT(owner == NULL);
  UT_ASSERT_EQ(free_call_count, 1);
  return 0;
}

UT_TEST(test_unique_ptr_release_transfers_without_freeing)
{
  unique_ptr(Payload) owner = payload_create(13);
  Payload *released;

  UT_ASSERT(owner != NULL);
  free_call_count = 0;

  released = unique_ptr_release(owner);

  UT_ASSERT(owner == NULL);
  UT_ASSERT(released != NULL);
  UT_ASSERT_EQ(released->value, 13);
  UT_ASSERT_EQ(free_call_count, 0);

  free(released);
  return 0;
}

UT_TEST(test_unique_ptr_reset_replaces_and_frees_owner)
{
  unique_ptr(Payload) owner = payload_create(17);
  Payload *replacement = payload_create(19);

  UT_ASSERT(owner != NULL);
  UT_ASSERT(replacement != NULL);
  free_call_count = 0;

  unique_ptr_reset(owner, replacement);

  UT_ASSERT(owner == replacement);
  UT_ASSERT_EQ(owner->value, 19);
  UT_ASSERT_EQ(free_call_count, 1);
  return 0;
}

UT_TEST(test_unique_ptr_reset_same_pointer_is_noop)
{
  unique_ptr(Payload) owner = payload_create(23);

  UT_ASSERT(owner != NULL);
  free_call_count = 0;

  unique_ptr_reset(owner, owner);

  UT_ASSERT_EQ(owner->value, 23);
  UT_ASSERT_EQ(free_call_count, 0);
  return 0;
}

UT_TEST(test_unique_ptr_reset_null_clears_owner)
{
  unique_ptr(Payload) owner = payload_create(29);

  UT_ASSERT(owner != NULL);
  free_call_count = 0;

  unique_ptr_reset(owner, NULL);

  UT_ASSERT(owner == NULL);
  UT_ASSERT_EQ(free_call_count, 1);
  return 0;
}

UT_TEST(test_unique_ptr_move_transfers_and_frees_destination)
{
  unique_ptr(Payload) source = payload_create(31);
  unique_ptr(Payload) destination = payload_create(37);
  Payload *source_value = source;

  UT_ASSERT(source != NULL);
  UT_ASSERT(destination != NULL);
  free_call_count = 0;

  unique_ptr_move(destination, source);

  UT_ASSERT(source == NULL);
  UT_ASSERT(destination == source_value);
  UT_ASSERT_EQ(destination->value, 31);
  UT_ASSERT_EQ(free_call_count, 1);
  return 0;
}

UT_TEST(test_unique_ptr_self_move_is_noop)
{
  unique_ptr(Payload) owner = payload_create(41);
  Payload *value = owner;

  UT_ASSERT(owner != NULL);
  free_call_count = 0;

  unique_ptr_move(owner, owner);

  UT_ASSERT(owner == value);
  UT_ASSERT_EQ(owner->value, 41);
  UT_ASSERT_EQ(free_call_count, 0);
  return 0;
}

UT_TEST(test_unique_ptr_supports_arrays)
{
  unique_ptr(int) values = calloc(4, sizeof(*values));

  UT_ASSERT(values != NULL);
  values[3] = 43;
  UT_ASSERT_EQ(values[3], 43);
  return 0;
}
