/*
 *  TCC Memory Utilities - Typed vector unit tests
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "memory/vector.h"

#include "ut.h"

#include <stdlib.h>

typedef struct TestPoint
{
  int x;
  int y;
} TestPoint;

static int reallocation_count;
static int deallocation_count;
static int reject_reallocation;

static void *test_reallocate(void *value, size_t size)
{
  reallocation_count++;
  if (reject_reallocation) {
    return NULL;
  }
  return realloc(value, size);
}

static void test_deallocate(void *value)
{
  deallocation_count++;
  free(value);
}

void *tcc_realloc(void *value, unsigned long size)
{
  return test_reallocate(value, (size_t)size);
}

/* vector.c's tcc_realloc calls come through tcc.h's allocation-attribution
   wrapper, so that is the name it references; route it to the counting
   allocator above, which the assertions below are written against. */
void *tcc_realloc_at(void *value, unsigned long size, const char *file, int line)
{
  (void)file;
  (void)line;
  return tcc_realloc(value, size);
}

void tcc_free(void *value)
{
  test_deallocate(value);
}

TCC_VECTOR_DEFINE_WITH_ALLOCATOR(TestIntVector, int, test_reallocate, test_deallocate)
TCC_VECTOR_DEFINE_WITH_ALLOCATOR(TestPointVector, TestPoint, test_reallocate, test_deallocate)

static void reset_allocator(void)
{
  reallocation_count = 0;
  deallocation_count = 0;
  reject_reallocation = 0;
}

UT_TEST(test_vector_initializes_empty_and_cleans_up)
{
  TestIntVector values;

  reset_allocator();
  TestIntVector_init(&values);

  UT_ASSERT(TestIntVector_empty(&values));
  UT_ASSERT(values.data == NULL);
  UT_ASSERT_EQ(values.size, 0);
  UT_ASSERT_EQ(values.capacity, 0);

  TestIntVector_cleanup(&values);
  UT_ASSERT_EQ(deallocation_count, 0);
  return 0;
}

UT_TEST(test_vector_pushes_typed_values_and_grows_geometrically)
{
  TestPointVector points = {0};

  reset_allocator();
  for (int i = 0; i < 9; ++i) {
    TestPoint point = {i, i * 3};

    UT_ASSERT_EQ(TestPointVector_push_back(&points, point), 0);
  }

  UT_ASSERT_EQ(points.size, 9);
  UT_ASSERT_EQ(points.capacity, 16);
  UT_ASSERT_EQ(reallocation_count, 3);
  for (int i = 0; i < 9; ++i) {
    UT_ASSERT_EQ(points.data[i].x, i);
    UT_ASSERT_EQ(points.data[i].y, i * 3);
  }

  TestPointVector_cleanup(&points);
  UT_ASSERT_EQ(deallocation_count, 1);
  return 0;
}

UT_TEST(test_vector_reserve_resize_and_clear_preserve_capacity)
{
  TestIntVector values = {0};

  reset_allocator();
  UT_ASSERT_EQ(TestIntVector_reserve(&values, 6), 0);
  UT_ASSERT_EQ(values.capacity, 6);
  UT_ASSERT_EQ(TestIntVector_resize(&values, 4), 0);
  for (size_t i = 0; i < values.size; ++i) {
    UT_ASSERT_EQ(values.data[i], 0);
  }

  values.data[0] = 17;
  UT_ASSERT_EQ(TestIntVector_resize(&values, 2), 0);
  UT_ASSERT_EQ(values.data[0], 17);
  UT_ASSERT_EQ(values.capacity, 6);

  TestIntVector_clear(&values);
  UT_ASSERT(TestIntVector_empty(&values));
  UT_ASSERT_EQ(values.capacity, 6);
  TestIntVector_cleanup(&values);
  return 0;
}

UT_TEST(test_vector_insert_erase_and_pop_keep_order)
{
  TestIntVector values = {0};
  int value = 0;

  reset_allocator();
  UT_ASSERT_EQ(TestIntVector_push_back(&values, 11), 0);
  UT_ASSERT_EQ(TestIntVector_push_back(&values, 33), 0);
  UT_ASSERT_EQ(TestIntVector_insert(&values, 1, 22), 0);
  UT_ASSERT_EQ(TestIntVector_insert(&values, 3, 44), 0);
  UT_ASSERT_EQ(values.size, 4);
  UT_ASSERT_EQ(values.data[0], 11);
  UT_ASSERT_EQ(values.data[1], 22);
  UT_ASSERT_EQ(values.data[2], 33);
  UT_ASSERT_EQ(values.data[3], 44);

  UT_ASSERT_EQ(TestIntVector_erase(&values, 1), 0);
  UT_ASSERT_EQ(values.data[1], 33);
  UT_ASSERT_EQ(TestIntVector_pop_back(&values, &value), 0);
  UT_ASSERT_EQ(value, 44);
  UT_ASSERT_EQ(TestIntVector_pop_back(&values, NULL), 0);
  UT_ASSERT_EQ(TestIntVector_pop_back(&values, NULL), 0);
  UT_ASSERT_EQ(TestIntVector_pop_back(&values, NULL), -1);
  UT_ASSERT_EQ(TestIntVector_insert(&values, 1, 55), -1);
  UT_ASSERT_EQ(TestIntVector_erase(&values, 0), -1);

  TestIntVector_cleanup(&values);
  return 0;
}

UT_TEST(test_vector_failed_growth_leaves_contents_unchanged)
{
  TestIntVector values = {0};
  int *data;

  reset_allocator();
  for (int i = 0; i < 4; ++i) {
    UT_ASSERT_EQ(TestIntVector_push_back(&values, i + 1), 0);
  }
  data = values.data;
  reject_reallocation = 1;

  UT_ASSERT_EQ(TestIntVector_push_back(&values, 5), -1);
  UT_ASSERT(values.data == data);
  UT_ASSERT_EQ(values.size, 4);
  UT_ASSERT_EQ(values.capacity, 4);
  for (int i = 0; i < 4; ++i) {
    UT_ASSERT_EQ(values.data[i], i + 1);
  }

  reject_reallocation = 0;
  TestIntVector_cleanup(&values);
  return 0;
}

UT_TEST(test_vector_rejects_capacity_overflow)
{
  TestIntVector values = {0};
  size_t too_large = SIZE_MAX / sizeof(*values.data) + 1;

  reset_allocator();
  UT_ASSERT_EQ(TestIntVector_reserve(&values, too_large), -1);
  UT_ASSERT_EQ(reallocation_count, 0);
  UT_ASSERT(values.data == NULL);
  UT_ASSERT_EQ(values.size, 0);
  UT_ASSERT_EQ(values.capacity, 0);
  return 0;
}

UT_TEST(test_vector_shrink_to_fit_releases_unused_storage)
{
  TestIntVector values = {0};

  reset_allocator();
  UT_ASSERT_EQ(TestIntVector_reserve(&values, 20), 0);
  UT_ASSERT_EQ(TestIntVector_push_back(&values, 71), 0);
  UT_ASSERT_EQ(TestIntVector_push_back(&values, 73), 0);
  UT_ASSERT_EQ(TestIntVector_shrink_to_fit(&values), 0);
  UT_ASSERT_EQ(values.capacity, 2);
  UT_ASSERT_EQ(values.data[0], 71);
  UT_ASSERT_EQ(values.data[1], 73);

  TestIntVector_clear(&values);
  UT_ASSERT_EQ(TestIntVector_shrink_to_fit(&values), 0);
  UT_ASSERT(values.data == NULL);
  UT_ASSERT_EQ(values.capacity, 0);
  UT_ASSERT_EQ(deallocation_count, 1);
  return 0;
}

static int use_manual_vector(void)
{
  vector(int) values = {0};

  UT_ASSERT_EQ(vector_push_back(&values, 79), 0);
  UT_ASSERT_EQ(vector_data(&values)[0], 79);
  UT_ASSERT_EQ(deallocation_count, 0);
  vector_cleanup(&values);
  return 0;
}

UT_TEST(test_vector_owner_can_be_cleaned_up_manually)
{
  reset_allocator();

  UT_ASSERT_EQ(use_manual_vector(), 0);
  UT_ASSERT_EQ(deallocation_count, 1);
  return 0;
}

static int use_scoped_vector(void)
{
  scoped_vector(int) values = {0};

  UT_ASSERT_EQ(vector_push_back(&values, 83), 0);
  UT_ASSERT_EQ(vector_data(&values)[0], 83);
  return 0;
}

UT_TEST(test_vector_scope_owner_cleans_up_automatically)
{
  reset_allocator();

  UT_ASSERT_EQ(use_scoped_vector(), 0);
  UT_ASSERT_EQ(deallocation_count, 1);
  return 0;
}

UT_TEST(test_unnamed_vector_supports_all_operations)
{
  vector(TestPoint) points = {0};
  TestPoint point = {3, 5};
  TestPoint popped = {0};

  reset_allocator();
  UT_ASSERT(vector_empty(&points));
  UT_ASSERT_EQ(vector_reserve(&points, 6), 0);
  UT_ASSERT_EQ(points.capacity, 6);
  UT_ASSERT_EQ(vector_resize(&points, 2), 0);
  UT_ASSERT_EQ(points.data[0].x, 0);
  UT_ASSERT_EQ(points.data[1].y, 0);
  UT_ASSERT_EQ(vector_insert(&points, 1, point), 0);
  UT_ASSERT_EQ(vector_data(&points)[1].x, 3);
  UT_ASSERT_EQ(vector_data(&points)[1].y, 5);
  UT_ASSERT_EQ(vector_erase(&points, 0), 0);
  UT_ASSERT_EQ(vector_pop_back(&points, &popped), 0);
  UT_ASSERT_EQ(popped.x, 0);
  UT_ASSERT_EQ(popped.y, 0);
  UT_ASSERT_EQ(vector_pop_back(&points, NULL), 0);
  UT_ASSERT(vector_empty(&points));
  UT_ASSERT_EQ(vector_shrink_to_fit(&points), 0);
  UT_ASSERT(points.data == NULL);
  UT_ASSERT_EQ(points.capacity, 0);
  UT_ASSERT_EQ(deallocation_count, 1);
  return 0;
}
