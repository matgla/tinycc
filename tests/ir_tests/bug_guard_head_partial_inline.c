/*
 *  TCC IR - guard-head partial inlining regression
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Zig's FileHeader.deinit: `if (heap.ptr) { free; reset }`, too large for the
 * wrapper class, was a call at every site even when the test fails.  A void
 * static body of effect-free local assignments and one `if` now expands its
 * test at a site that keeps it out of line, and calls it only when the test
 * holds.  The adv_* bodies are shapes the guard must not take. */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

struct slice_u8 { uint8_t *ptr; uintptr_t len; };
struct Allocator { void *ptr; void const *vtable; };
struct Hdr {
  uint32_t head[21];
  struct Allocator _allocator;
  uint32_t mid[2];
  struct slice_u8 _name_heap;
  uint32_t tail[13];
};

static int frees, log_n;
static int log_v[64];

__attribute__((noinline)) void allocator_free(struct Allocator a, struct slice_u8 s)
{
  frees += (int)s.len + (a.ptr != NULL);
}

__attribute__((noinline)) void record(int v)
{
  if (log_n < 64)
    log_v[log_n++] = v;
}

/* FileHeader.deinit as kernel.c spells it. */
static void deinit(struct Hdr *const a0) {
 struct Hdr *const *t1;
 struct Hdr *t2;
 struct Hdr *t0;
 struct slice_u8 *t3;
 struct slice_u8 t4;
 struct slice_u8 t6;
 struct Allocator *t7;
 struct Allocator t8;
 _Bool t5;
 t0 = a0;
 t1 = (struct Hdr *const *)&t0;
 t2 = (*t1);
 t3 = (struct slice_u8 *)&t2->_name_heap;
 t4 = (*t3);
 t5 = t4.ptr != NULL;
 if (t5) {
  t6 = t4;
  t2 = (*t1);
  t7 = (struct Allocator *)&t2->_allocator;
  t8 = (*t7);
  allocator_free(t8, t6);
  t2 = (*t1);
  t3 = (struct slice_u8 *)&t2->_name_heap;
  (*t3) = (struct slice_u8){NULL,(uintptr_t)0xaaaaaaaaul};
  goto zig_block_0;
 }
 goto zig_block_0;

zig_block_0:;
 return;
}

struct Q { int n; int hits; struct Q *next; };

/* Accepted: a declaration with an initializer, and a short-circuit test. */
static void adv_init(struct Q *q) { int n = q->n; if (n != 0) { record(n); record(-n); record(0); q->n = 0; } }
static void adv_and(struct Q *q) { if (q && q->n > 1) { record(100 + q->n); record(q->hits); record(0); q->n = 1; } }

/* The test reads a global the guarded block writes: the callee tests again. */
static int budget;
static void adv_global(void) { int b; b = budget; if (b > 0) { record(200 + b); record(b); record(0); budget = b - 1; } }

/* Rejected: the prefix writes a parameter (the call would see the new value). */
static void adv_param(int *p) { p = p + 1; if (*p) { record(300 + *p); record(*p); record(0); *p = 0; } }
/* Rejected: work after the `if` runs on both paths. */
static void adv_tail(struct Q *q) { if (q->n) { record(400 + q->n); record(q->n); record(0); q->n = 0; } q->hits++; }
/* Rejected: an else arm. */
static void adv_else(struct Q *q) { if (q->n) { record(500 + q->n); record(q->n); record(0); } else { q->hits += 2; } }
/* Rejected: recursion (the body names itself). */
static void adv_rec(struct Q *q) { if (q) { adv_rec(q->next); record(600 + q->n); record(q->n); record(0); } }
/* Rejected: a call in the test. */
__attribute__((noinline)) int is_live(struct Q *q) { return q->n > 0; }
static void adv_call(struct Q *q) { if (is_live(q)) { record(700 + q->n); record(q->n); record(0); q->n = 0; } }

/* deinit runs once per entry stepped over. */
int walk(struct Hdr *h, int n)
{
  int found = 0;
  for (int i = 0; i < n; i++)
  {
    deinit(h);
    found += h->_name_heap.ptr != NULL ? 10 : 1;
    if (i == 1)
      h->_name_heap = (struct slice_u8){(uint8_t *)h, 3};
  }
  return found;
}

void drop(struct Hdr *h) { deinit(h); }
/* Expression contexts: a comma operand, both arms of ?:, a for increment. */
int drop_expr(struct Hdr *a, struct Hdr *b, int c)
{
  int n = (deinit(a), 5);
  c ? deinit(a) : deinit(b);
  for (int i = 0; i < 2; deinit(b), i++)
    n += i;
  if (0)
    deinit(a);
  return n;
}
static void drop_pair(struct Hdr *a, struct Hdr *b) { deinit(a); deinit(b); }
void drop_both(struct Hdr *a, struct Hdr *b) { drop_pair(a, b); }

void use_param(int *p) { adv_param(p); }
void use_tail(struct Q *q) { adv_tail(q); }
void use_else(struct Q *q) { adv_else(q); }
void use_rec(struct Q *q) { adv_rec(q); }
void use_call(struct Q *q) { adv_call(q); }
void use_init(struct Q *q) { adv_init(q); }
void use_and(struct Q *q) { adv_and(q); }
void use_global(void) { adv_global(); }
/* A second site each, so none is a called-once body. */
void use_all(struct Q *q, int *p)
{
  adv_param(p), adv_tail(q), adv_else(q), adv_rec(q), adv_call(q), adv_init(q), adv_and(q), adv_global();
}

static void (*volatile keep)(struct Hdr *) = deinit;

int main(void)
{
  struct Hdr a = {0}, b = {0};
  drop(&a);
  printf("null drop: frees=%d len=%u\n", frees, (unsigned)a._name_heap.len);
  a._name_heap = (struct slice_u8){(uint8_t *)&a, 5};
  a._allocator.ptr = &b;
  drop(&a);
  printf("live drop: frees=%d ptr=%d len=%x\n", frees, a._name_heap.ptr != NULL, (unsigned)a._name_heap.len);
  b._name_heap = (struct slice_u8){(uint8_t *)&b, 7};
  drop_both(&a, &b);
  printf("pair: frees=%d a=%d b=%d\n", frees, a._name_heap.ptr != NULL, b._name_heap.ptr != NULL);
  a._name_heap = (struct slice_u8){NULL, 9};
  int w = walk(&a, 5);
  printf("walk: %d frees=%d\n", w, frees);
  b._name_heap = (struct slice_u8){(uint8_t *)&a, 2};
  keep(&b);
  printf("pointer: frees=%d\n", frees);
  a._name_heap = (struct slice_u8){(uint8_t *)&a, 20};
  b._name_heap = (struct slice_u8){(uint8_t *)&b, 40};
  int e = drop_expr(&a, &b, 0);
  printf("expr: %d frees=%d\n", e, frees);

  struct Q q2 = {0, 5, NULL}, q1 = {4, 1, &q2}, q0 = {3, 2, &q1};
  int arr[2] = {11, 6};
  use_init(&q0);
  use_init(&q0);
  use_and(NULL);
  use_and(&q1);
  use_and(&q1);
  budget = 2;
  for (int i = 0; i < 4; i++)
    use_global();
  use_param(arr);
  use_param(arr);
  q2.n = 8;
  use_tail(&q2);
  use_tail(&q2);
  use_else(&q2);
  q1.n = 4;
  use_else(&q1);
  use_rec(&q0);
  q0.n = 2;
  use_call(&q0);
  use_call(&q0);
  printf("arr=%d,%d q0=%d/%d q1=%d/%d q2=%d/%d budget=%d\n", arr[0], arr[1], q0.n, q0.hits, q1.n, q1.hits, q2.n,
         q2.hits, budget);
  for (int i = 0; i < log_n; i++)
    printf("%d%c", log_v[i], i + 1 < log_n ? ' ' : '\n');
  return 0;
}
