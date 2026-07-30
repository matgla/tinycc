/*
 *  TCC Memory Utilities - Small sequence unit tests
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "memory/small_sequence.h"

#include "ut.h"

#include <stdlib.h>

static int allocation_count;
static int deallocation_count;

static void *test_allocate(size_t size)
{
  allocation_count++;
  return malloc(size);
}

static void test_deallocate(void *value)
{
  deallocation_count++;
  free(value);
}

TCC_SMALL_SEQUENCE_DEFINE_WITH_ALLOCATOR(TestIntSequence, int, 4, test_allocate, test_deallocate)

static int use_inline_sequence(void)
{
  small_sequence(TestIntSequence) values = {0};

  UT_ASSERT_EQ(TestIntSequence_init(&values, 4), 0);
  UT_ASSERT(TestIntSequence_is_inline(&values));
  UT_ASSERT(TestIntSequence_data(&values) == values.storage.inline_values);
  for (size_t i = 0; i < values.size; ++i) {
    UT_ASSERT_EQ(TestIntSequence_data(&values)[i], 0);
  }
  TestIntSequence_data(&values)[3] = 47;
  UT_ASSERT_EQ(TestIntSequence_const_data(&values)[3], 47);
  return 0;
}

UT_TEST(test_small_sequence_uses_inline_storage_without_allocation)
{
  allocation_count = 0;
  deallocation_count = 0;

  UT_ASSERT_EQ(use_inline_sequence(), 0);
  UT_ASSERT_EQ(allocation_count, 0);
  UT_ASSERT_EQ(deallocation_count, 0);
  return 0;
}

static int use_heap_sequence(void)
{
  small_sequence(TestIntSequence) values = {0};

  UT_ASSERT_EQ(TestIntSequence_init(&values, 5), 0);
  UT_ASSERT(!TestIntSequence_is_inline(&values));
  UT_ASSERT(TestIntSequence_data(&values) == values.storage.heap);
  UT_ASSERT(TestIntSequence_data(&values) != values.storage.inline_values);
  for (size_t i = 0; i < values.size; ++i) {
    UT_ASSERT_EQ(TestIntSequence_data(&values)[i], 0);
  }
  TestIntSequence_data(&values)[4] = 53;
  UT_ASSERT_EQ(TestIntSequence_const_data(&values)[4], 53);
  return 0;
}

UT_TEST(test_small_sequence_uses_and_releases_heap_storage)
{
  allocation_count = 0;
  deallocation_count = 0;

  UT_ASSERT_EQ(use_heap_sequence(), 0);
  UT_ASSERT_EQ(allocation_count, 1);
  UT_ASSERT_EQ(deallocation_count, 1);
  return 0;
}

UT_TEST(test_small_sequence_union_reuses_storage)
{
  TestIntSequence values = {0};

  UT_ASSERT_EQ(sizeof(values.storage), sizeof(values.storage.inline_values));
  UT_ASSERT(sizeof(values.storage) >= sizeof(values.storage.heap));
  return 0;
}

UT_TEST(test_small_sequence_rejects_size_overflow)
{
  small_sequence(TestIntSequence) values = {0};

  allocation_count = 0;
  deallocation_count = 0;
  UT_ASSERT_EQ(TestIntSequence_init(&values, SIZE_MAX), -1);
  UT_ASSERT_EQ(allocation_count, 0);
  UT_ASSERT_EQ(deallocation_count, 0);
  return 0;
}

UT_TEST(test_small_sequence_move_copies_inline_storage)
{
  small_sequence(TestIntSequence) source = {0};
  small_sequence(TestIntSequence) destination = {0};

  UT_ASSERT_EQ(TestIntSequence_init(&source, 4), 0);
  TestIntSequence_data(&source)[3] = 61;
  TestIntSequence_move(&destination, &source);

  UT_ASSERT_EQ(source.size, 0);
  UT_ASSERT(TestIntSequence_is_inline(&destination));
  UT_ASSERT_EQ(TestIntSequence_data(&destination)[3], 61);
  UT_ASSERT(TestIntSequence_data(&destination) == destination.storage.inline_values);
  return 0;
}

UT_TEST(test_small_sequence_move_transfers_heap_storage)
{
  small_sequence(TestIntSequence) source = {0};
  small_sequence(TestIntSequence) destination = {0};
  int *heap;

  allocation_count = 0;
  deallocation_count = 0;
  UT_ASSERT_EQ(TestIntSequence_init(&source, 5), 0);
  heap = TestIntSequence_data(&source);
  TestIntSequence_move(&destination, &source);

  UT_ASSERT_EQ(source.size, 0);
  UT_ASSERT(TestIntSequence_data(&destination) == heap);
  UT_ASSERT_EQ(allocation_count, 1);
  UT_ASSERT_EQ(deallocation_count, 0);
  TestIntSequence_cleanup(&destination);
  UT_ASSERT_EQ(deallocation_count, 1);
  return 0;
}
