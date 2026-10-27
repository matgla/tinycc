/*
 *  TCC Tests - const_local_table: a direct read at a join of two constant stores
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* Zig's Parse.eatToken, inlined into a loop latch, leaves `is_null` written
 * by a constant store on each path and read where they join.  The rerun of
 * const_local_table after an unrolled table loop (ssa:unroll_cascade) folded
 * that read to the textually last store: ir_xform_same_block ignores joins.
 * A direct read now folds only when no jump target lies between. */
#include <stdint.h>
#include <stdio.h>

typedef _Bool bool;

struct opt_u32 { uint32_t payload; bool is_null; };
struct arr_3_usize { uintptr_t array[3]; };
struct arr_3_ptr { uint8_t *array[3]; };
struct parser { const char *tags; uint32_t tok_i; uint32_t fields; uint8_t *bytes; };

static uint32_t next_token(struct parser *const a0)
{
  uint32_t t4 = a0->tok_i;
  a0->tok_i = t4 + 1;
  return t4;
}

/* The CBE shape: the match path copies a local result, the other a literal. */
static struct opt_u32 eat_token(struct parser *const a0, char const a1)
{
  struct opt_u32 t2, t9;
  if (a0->tags[a0->tok_i] == a1)
  {
    uint32_t t6 = next_token(a0);
    t9.is_null = 0;
    t9.payload = t6;
    t2 = t9;
    goto out;
  }
  t2 = (struct opt_u32){.is_null = 1, .payload = 0};
  goto out;
out:
  return t2;
}

/* The same with the null path first: its store is the one in between. */
static struct opt_u32 eat_token_rev(struct parser *const a0, char const a1)
{
  struct opt_u32 t2, t9;
  if (a0->tags[a0->tok_i] != a1)
  {
    t2 = (struct opt_u32){.is_null = 1, .payload = 0};
    goto out;
  }
  uint32_t t6 = next_token(a0);
  t9.is_null = 0;
  t9.payload = t6;
  t2 = t9;
  goto out;
out:
  return t2;
}

/* MultiArrayList.slice: a loop over constant local tables, which the
 * unroller expands and the propagation group then revisits. */
static void slice(struct parser *p, uintptr_t cap)
{
  struct arr_3_ptr ptrs;
  uint8_t *ptr = p->bytes;
  for (uintptr_t i = 0; i < 3; i++)
  {
    uintptr_t field = (struct arr_3_usize){{1, 0, 2}}.array[i];
    ptrs.array[field] = ptr;
    ptr += (struct arr_3_usize){{4, 2, 1}}.array[i] * cap;
  }
  p->fields = (uint32_t)(ptrs.array[0] - p->bytes) * 100 + (uint32_t)(ptrs.array[2] - p->bytes);
}

/* `a, b, c` -- items separated by commas; at most 8. */
__attribute__((noinline)) int count_list(struct parser *p, uintptr_t cap)
{
  struct opt_u32 t24;
  int n = 0;
loop:
  p->tok_i++;
  if (++n == 8)
    return -1;
  t24 = eat_token(p, ',');
  if (!t24.is_null)
    goto loop;
  slice(p, cap);
  return n;
}

__attribute__((noinline)) int count_list_rev(struct parser *p, uintptr_t cap)
{
  struct opt_u32 t24;
  int n = 0;
loop:
  p->tok_i++;
  if (++n == 8)
    return -1;
  t24 = eat_token_rev(p, ',');
  if (!t24.is_null)
    goto loop;
  slice(p, cap);
  return n;
}

/* The payload of the eaten token: both fields of the joined result. */
__attribute__((noinline)) int eat_payload(struct parser *p, uintptr_t cap)
{
  struct opt_u32 t24 = eat_token(p, '=');
  slice(p, cap);
  return t24.is_null ? -1 : (int)t24.payload;
}

static uint8_t buf[64];

static void run(const char *name, int (*f)(struct parser *, uintptr_t), const char *src, uintptr_t cap)
{
  struct parser p = {src, 0, 0, buf};
  int n = f(&p, cap);
  printf("%s(\"%s\"): n=%d tok_i=%u fields=%u\n", name, src, n, p.tok_i, p.fields);
}

int main(void)
{
  run("count_list", count_list, "a,a,a;", 1);
  run("count_list", count_list, "a,a;", 2);
  run("count_list", count_list, "a;", 2);
  run("count_list_rev", count_list_rev, "a,a,a;", 1);
  run("count_list_rev", count_list_rev, "a;", 3);
  run("eat_payload", eat_payload, "=", 1);
  run("eat_payload", eat_payload, ";", 1);
  return 0;
}
