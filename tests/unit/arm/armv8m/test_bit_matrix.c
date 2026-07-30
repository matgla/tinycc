/*
 *  TCC Memory Utilities - Bit matrix unit tests
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "memory/bit_matrix.h"

#include "ut.h"

#include <stdlib.h>

static int matrix_allocation_count;
static int matrix_deallocation_count;

static void *matrix_allocate(size_t size)
{
  matrix_allocation_count++;
  return malloc(size); /* deliberately non-zeroing: _init must clear itself */
}

static void matrix_deallocate(void *value)
{
  matrix_deallocation_count++;
  free(value);
}

TCC_BIT_MATRIX_DEFINE_WITH_ALLOCATOR(TestBitMatrix, matrix_allocate, matrix_deallocate)

static void reset_counters(void)
{
  matrix_allocation_count = 0;
  matrix_deallocation_count = 0;
}

UT_TEST(test_bit_matrix_init_allocates_once_and_zeroes)
{
  TestBitMatrix matrix;

  reset_counters();
  UT_ASSERT_EQ(TestBitMatrix_init(&matrix, 4, 100), 0);
  UT_ASSERT_EQ(matrix_allocation_count, 1);
  UT_ASSERT(matrix.words != NULL);
  UT_ASSERT_EQ(matrix.rows, 4);
  UT_ASSERT_EQ(matrix.words_per_row, 2); /* ceil(100/64) */
  for (int r = 0; r < matrix.rows; r++) {
    for (int w = 0; w < matrix.words_per_row; w++) {
      UT_ASSERT_EQ(TestBitMatrix_row(&matrix, r)[w], 0);
    }
  }

  TestBitMatrix_cleanup(&matrix);
  UT_ASSERT_EQ(matrix_deallocation_count, 1);
  UT_ASSERT(matrix.words == NULL);
  UT_ASSERT_EQ(matrix.rows, 0);
  UT_ASSERT_EQ(matrix.words_per_row, 0);
  return 0;
}

UT_TEST(test_bit_matrix_words_per_row_rounds_up)
{
  TestBitMatrix matrix;
  const int cases[][2] = {{1, 1}, {64, 1}, {65, 2}, {128, 2}, {129, 3}};

  for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
    UT_ASSERT_EQ(TestBitMatrix_init(&matrix, 1, cases[i][0]), 0);
    UT_ASSERT_EQ(matrix.words_per_row, cases[i][1]);
    TestBitMatrix_cleanup(&matrix);
  }
  return 0;
}

UT_TEST(test_bit_matrix_empty_dimensions_do_not_allocate)
{
  TestBitMatrix zero_rows;
  TestBitMatrix zero_columns;

  reset_counters();
  UT_ASSERT_EQ(TestBitMatrix_init(&zero_rows, 0, 128), 0);
  UT_ASSERT_EQ(TestBitMatrix_init(&zero_columns, 8, 0), 0);
  UT_ASSERT_EQ(matrix_allocation_count, 0);
  UT_ASSERT(zero_rows.words == NULL);
  UT_ASSERT(zero_columns.words == NULL);

  TestBitMatrix_cleanup(&zero_rows);
  TestBitMatrix_cleanup(&zero_columns);
  UT_ASSERT_EQ(matrix_deallocation_count, 0);
  return 0;
}

UT_TEST(test_bit_matrix_rows_are_independent)
{
  TestBitMatrix matrix;

  UT_ASSERT_EQ(TestBitMatrix_init(&matrix, 3, 80), 0);
  TestBitMatrix_set(&matrix, 0, 5);
  TestBitMatrix_set(&matrix, 1, 5);
  TestBitMatrix_set(&matrix, 1, 70);
  TestBitMatrix_set(&matrix, 2, 79);

  UT_ASSERT(TestBitMatrix_test(&matrix, 0, 5));
  UT_ASSERT(TestBitMatrix_test(&matrix, 1, 5));
  UT_ASSERT(TestBitMatrix_test(&matrix, 1, 70));
  UT_ASSERT(TestBitMatrix_test(&matrix, 2, 79));

  /* Setting row 1 must not leak into rows 0 or 2. */
  UT_ASSERT(!TestBitMatrix_test(&matrix, 0, 70));
  UT_ASSERT(!TestBitMatrix_test(&matrix, 2, 5));
  UT_ASSERT(!TestBitMatrix_test(&matrix, 0, 79));

  TestBitMatrix_reset(&matrix, 1, 5);
  UT_ASSERT(!TestBitMatrix_test(&matrix, 1, 5));
  UT_ASSERT(TestBitMatrix_test(&matrix, 0, 5)); /* other row unchanged */

  TestBitMatrix_cleanup(&matrix);
  return 0;
}

UT_TEST(test_bit_matrix_rows_are_contiguous)
{
  TestBitMatrix matrix;

  UT_ASSERT_EQ(TestBitMatrix_init(&matrix, 4, 64), 0);
  /* Row r begins words_per_row words after row r-1 in the single allocation. */
  for (int r = 0; r < matrix.rows; r++) {
    ptrdiff_t offset = TestBitMatrix_row(&matrix, r) - matrix.words;
    UT_ASSERT_EQ((long long)offset, (long long)r * matrix.words_per_row);
  }
  TestBitMatrix_cleanup(&matrix);
  return 0;
}

static void scoped_matrix_body(void)
{
  bit_matrix(TestBitMatrix) matrix = {0};

  TestBitMatrix_init(&matrix, 2, 64);
  TestBitMatrix_set(&matrix, 0, 1);
  /* leaving this scope must invoke TestBitMatrix_cleanup automatically */
}

UT_TEST(test_bit_matrix_scoped_cleanup_frees_on_scope_exit)
{
  reset_counters();
  scoped_matrix_body();
  UT_ASSERT_EQ(matrix_allocation_count, 1);
  UT_ASSERT_EQ(matrix_deallocation_count, 1);
  return 0;
}

UT_TEST(test_bit_matrix_rejects_negative_dimensions)
{
  TestBitMatrix matrix;

  reset_counters();
  UT_ASSERT_EQ(TestBitMatrix_init(&matrix, -1, 64), -1);
  UT_ASSERT(matrix.words == NULL);
  UT_ASSERT_EQ(TestBitMatrix_init(&matrix, 4, -1), -1);
  UT_ASSERT(matrix.words == NULL);
  UT_ASSERT_EQ(matrix_allocation_count, 0);
  return 0;
}
