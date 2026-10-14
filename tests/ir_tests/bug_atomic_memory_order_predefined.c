/*
 *  TCC Tests - The __ATOMIC_* memory orders are predefined
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* gcc and clang predefine the six __ATOMIC_* memory orders; tcc defined them
 * only in <stdatomic.h>, so __atomic_* calls without that header failed to
 * compile.  tccdefs.h predefines them now, with the gcc values. */
#include <stdio.h>

#if !defined __ATOMIC_RELAXED || !defined __ATOMIC_CONSUME || !defined __ATOMIC_ACQUIRE || \
    !defined __ATOMIC_RELEASE || !defined __ATOMIC_ACQ_REL || !defined __ATOMIC_SEQ_CST
#error memory-order macros not predefined
#endif
#if __ATOMIC_RELAXED != 0 || __ATOMIC_CONSUME != 1 || __ATOMIC_ACQUIRE != 2 || __ATOMIC_RELEASE != 3 || \
    __ATOMIC_ACQ_REL != 4 || __ATOMIC_SEQ_CST != 5
#error memory-order macros have the wrong values
#endif

static int counter;

__attribute__((noinline)) int load_relaxed(int *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }

int main(void)
{
  int v = 5;
  __atomic_store_n(&counter, 40, __ATOMIC_RELEASE);
  int old = __atomic_fetch_add(&counter, 2, __ATOMIC_ACQ_REL);
  int expected = 42;
  int ok = __atomic_compare_exchange_n(&counter, &expected, 7, 0, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE);
  printf("%d %d %d %d\n", load_relaxed(&v), old, ok, __atomic_load_n(&counter, __ATOMIC_CONSUME));
  return 0;
}

