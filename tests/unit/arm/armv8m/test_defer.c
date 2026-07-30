/*
 *  TCC Utilities - Scope-exit defer unit tests
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "utils/defer.h"

#include "ut.h"

static int run_count;
static int order_log[8];
static int order_n;

static void bump(void)
{
  run_count++;
}

static void log_first(void)
{
  order_log[order_n++] = 1;
}

static void log_second(void)
{
  order_log[order_n++] = 2;
}

static void log_third(void)
{
  order_log[order_n++] = 3;
}

static void scope_with_single_defer(void)
{
  defer(bump);
}

static void scope_with_lifo_defers(void)
{
  defer(log_first);
  defer(log_second);
  defer(log_third);
}

static void scope_with_early_return(int take_early_exit)
{
  defer(bump);

  if (take_early_exit) {
    return;
  }
  bump();
}

static void scope_with_null_defer(void)
{
  defer((tcc_defer_callback)0);
}

UT_TEST(test_defer_runs_callback_at_scope_exit)
{
  run_count = 0;

  scope_with_single_defer();

  UT_ASSERT_EQ(run_count, 1);
  return 0;
}

UT_TEST(test_defer_runs_in_lifo_order)
{
  order_n = 0;

  scope_with_lifo_defers();

  UT_ASSERT_EQ(order_n, 3);
  UT_ASSERT_EQ(order_log[0], 3);
  UT_ASSERT_EQ(order_log[1], 2);
  UT_ASSERT_EQ(order_log[2], 1);
  return 0;
}

UT_TEST(test_defer_runs_on_early_return)
{
  run_count = 0;

  scope_with_early_return(1);

  UT_ASSERT_EQ(run_count, 1);
  return 0;
}

UT_TEST(test_defer_null_callback_is_noop)
{
  scope_with_null_defer();

  UT_ASSERT(1);
  return 0;
}
