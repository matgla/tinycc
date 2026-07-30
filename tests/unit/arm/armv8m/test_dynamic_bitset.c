/*
 *  TCC Memory Utilities - Dynamic bitset unit tests
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "memory/dynamic_bitset.h"

#include "ut.h"

#include <stdlib.h>

static int bitset_allocation_count;
static int bitset_deallocation_count;

static void *bitset_allocate(size_t size)
{
  bitset_allocation_count++;
  return malloc(size);
}

static void bitset_deallocate(void *value)
{
  bitset_deallocation_count++;
  free(value);
}

TCC_DYNAMIC_BITSET_DEFINE_WITH_ALLOCATOR(TestDynamicBitset, 128, bitset_allocate, bitset_deallocate)

static int use_inline_bitset(void)
{
  dynamic_bitset(TestDynamicBitset) bits = {0};

  UT_ASSERT_EQ(TestDynamicBitset_init(&bits, 128), 0);
  UT_ASSERT(TestDynamicBitset_is_inline(&bits));
  UT_ASSERT(TestDynamicBitset_data(&bits) == bits.storage.inline_values);
  TestDynamicBitset_set(&bits, 0);
  TestDynamicBitset_set(&bits, 63);
  TestDynamicBitset_set(&bits, 127);
  UT_ASSERT(TestDynamicBitset_test(&bits, 0));
  UT_ASSERT(TestDynamicBitset_test(&bits, 63));
  UT_ASSERT(TestDynamicBitset_test(&bits, 127));
  return 0;
}

UT_TEST(test_dynamic_bitset_uses_inline_storage_without_allocation)
{
  bitset_allocation_count = 0;
  bitset_deallocation_count = 0;

  UT_ASSERT_EQ(use_inline_bitset(), 0);
  UT_ASSERT_EQ(bitset_allocation_count, 0);
  UT_ASSERT_EQ(bitset_deallocation_count, 0);
  return 0;
}

static int use_heap_bitset(void)
{
  dynamic_bitset(TestDynamicBitset) bits = {0};

  UT_ASSERT_EQ(TestDynamicBitset_init(&bits, 129), 0);
  UT_ASSERT(!TestDynamicBitset_is_inline(&bits));
  UT_ASSERT(TestDynamicBitset_data(&bits) == bits.storage.heap);
  TestDynamicBitset_set(&bits, 128);
  UT_ASSERT(TestDynamicBitset_test(&bits, 128));
  return 0;
}

UT_TEST(test_dynamic_bitset_uses_and_releases_heap_storage)
{
  bitset_allocation_count = 0;
  bitset_deallocation_count = 0;

  UT_ASSERT_EQ(use_heap_bitset(), 0);
  UT_ASSERT_EQ(bitset_allocation_count, 1);
  UT_ASSERT_EQ(bitset_deallocation_count, 1);
  return 0;
}

UT_TEST(test_dynamic_bitset_clear_and_reset_update_underlying_words)
{
  dynamic_bitset(TestDynamicBitset) bits = {0};

  UT_ASSERT_EQ(TestDynamicBitset_init(&bits, 129), 0);
  UT_ASSERT_EQ(TestDynamicBitset_word_count(&bits), 3);
  TestDynamicBitset_set(&bits, 64);
  TestDynamicBitset_set(&bits, 128);
  TestDynamicBitset_reset(&bits, 64);
  UT_ASSERT(!TestDynamicBitset_test(&bits, 64));
  UT_ASSERT(TestDynamicBitset_test(&bits, 128));
  TestDynamicBitset_clear(&bits);
  for (size_t i = 0; i < bits.size; i++) {
    UT_ASSERT(!TestDynamicBitset_test(&bits, i));
  }
  return 0;
}

UT_TEST(test_dynamic_bitset_ignores_out_of_range_positions)
{
  dynamic_bitset(TestDynamicBitset) bits = {0};

  UT_ASSERT_EQ(TestDynamicBitset_init(&bits, 64), 0);
  TestDynamicBitset_set(&bits, 64);
  TestDynamicBitset_reset(&bits, 64);
  UT_ASSERT(!TestDynamicBitset_test(&bits, 64));
  UT_ASSERT_EQ(TestDynamicBitset_const_data(&bits)[0], 0);
  return 0;
}

UT_TEST(test_dynamic_bitset_move_preserves_inline_and_heap_values)
{
  dynamic_bitset(TestDynamicBitset) inline_source = {0};
  dynamic_bitset(TestDynamicBitset) inline_destination = {0};
  dynamic_bitset(TestDynamicBitset) heap_source = {0};
  dynamic_bitset(TestDynamicBitset) heap_destination = {0};

  UT_ASSERT_EQ(TestDynamicBitset_init(&inline_source, 128), 0);
  UT_ASSERT_EQ(TestDynamicBitset_init(&heap_source, 129), 0);
  TestDynamicBitset_set(&inline_source, 127);
  TestDynamicBitset_set(&heap_source, 128);
  TestDynamicBitset_move(&inline_destination, &inline_source);
  TestDynamicBitset_move(&heap_destination, &heap_source);
  UT_ASSERT_EQ(inline_source.size, 0);
  UT_ASSERT_EQ(heap_source.size, 0);
  UT_ASSERT(TestDynamicBitset_is_inline(&inline_destination));
  UT_ASSERT(!TestDynamicBitset_is_inline(&heap_destination));
  UT_ASSERT(TestDynamicBitset_test(&inline_destination, 127));
  UT_ASSERT(TestDynamicBitset_test(&heap_destination, 128));
  return 0;
}
