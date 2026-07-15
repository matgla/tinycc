/*
 *  TCC Utilities - Scope-exit defer
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

typedef void (*tcc_defer_callback)(void);

static inline void tcc_defer_invoke(tcc_defer_callback *slot)
{
  if (*slot) {
    (*slot)();
  }
}

#define TCC_DEFER_CONCAT_(prefix, suffix) prefix##suffix
#define TCC_DEFER_CONCAT(prefix, suffix) TCC_DEFER_CONCAT_(prefix, suffix)

/* Runs callback() at the end of the enclosing scope, in reverse (LIFO) order. */
#define defer(callback)                                                          \
  __attribute__((cleanup(tcc_defer_invoke)))                                     \
      tcc_defer_callback TCC_DEFER_CONCAT(tcc_defer_slot_, __COUNTER__) = (callback)
