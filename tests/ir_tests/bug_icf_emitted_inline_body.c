/*
 *  TCC IR - ICF scanning an emitted inline body's freed tokens
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* emit_inline_functions freed a body's tokens once it was compiled but left
 * InlineFunc.func_str pointing at them; the next body's tcc_icf_try_fold
 * scanned them for address uses (heap-use-after-free under ASan, gcc torture
 * pr44043).  Pinned: the address-taken body, the out-of-line one, and two
 * identical out-of-line bodies that may share code. */
#include <stdio.h>

typedef int (*fn_t)(int);

static inline int add_one(int x) { return x + 1; }

static inline int scaled(int x)
{
  __asm__ volatile("" ::: "memory");
  return x * 3;
}

static inline int scaled_too(int x)
{
  __asm__ volatile("" ::: "memory");
  return x * 3;
}

__attribute__((noinline)) int apply(fn_t f, int v) { return f(v); }

int main(void)
{
  printf("add_one %d\n", apply(add_one, 1));
  printf("scaled %d %d\n", scaled(4), scaled_too(5));
  return 0;
}
