/* A local pointer whose every definition is `P = &V` for one V holds &V at
 * every use, so derefs through it become direct accesses of V
 * (ptr_local_fwd).  The Zig C backend emits `t0 = call(); t1 = &t0; (*t1)->f`
 * and sets t1 again in every block.  Pointers that may name two objects, are
 * copied from another pointer, or that a nested function redirects must keep
 * their deref. */
#include <stdio.h>
#include <stdint.h>

struct S
{
  uint32_t a, b;
};

static struct S pool[4] = {{1, 2}, {30, 40}, {500, 600}, {7000, 8000}};

__attribute__((noinline)) static struct S *mk(uint32_t k) { return &pool[k & 3]; }
__attribute__((noinline)) static uint32_t use(uint32_t x, uint32_t y) { return x * 3 + y; }
__attribute__((noinline)) static signed char mkc(int k) { return (signed char)(k - 3); }

/* V set only by a call; P set in the entry block. */
__attribute__((noinline)) static uint32_t call_only(uint32_t k)
{
  struct S *t0;
  struct S *const *t1;
  t0 = mk(k);
  t1 = (struct S *const *)&t0;
  uint32_t t2 = (*t1)->b;
  return use(t2, (*t1)->a);
}

/* The same V re-pointed in every case, as Zig emits it. */
__attribute__((noinline)) static uint32_t every_block(int k)
{
  struct S *t0;
  struct S *const *t1;
  uint32_t r;
  switch (k)
  {
  case 0:
    t0 = mk(1);
    t1 = (struct S *const *)&t0;
    r = (*t1)->a;
    break;
  case 1:
    t0 = mk(2);
    t1 = (struct S *const *)&t0;
    r = (*t1)->b + 1;
    break;
  default:
    t0 = mk((uint32_t)k);
    t1 = (struct S *const *)&t0;
    t0 = mk(3); /* through t1 this must be seen */
    r = (*t1)->a + (*t1)->b;
    break;
  }
  return r;
}

/* Stores through P land in V. */
__attribute__((noinline)) static uint32_t store_through(uint32_t k)
{
  uint32_t v;
  uint32_t *p;
  v = use(k, 1);
  p = &v;
  *p += 10;
  if (k & 1)
  {
    p = &v;
    *p *= 2;
  }
  return v + *p;
}

/* P names V on one path, W on the other: no forwarding. */
__attribute__((noinline)) static uint32_t two_targets(int k)
{
  uint32_t v, w;
  uint32_t *p;
  v = use(1, 2);
  w = use(3, 4);
  if (k)
    p = &v;
  else
    p = &w;
  *p += 100;
  return v * 1000 + w + *p;
}

/* P copied from another pointer: no forwarding. */
__attribute__((noinline)) static uint32_t copied(int k)
{
  uint32_t v, w;
  uint32_t *p, *q;
  v = use(5, 0);
  w = use(6, 0);
  q = k ? &w : &v;
  p = &v;
  *p += 1;
  p = q;
  *p += 7;
  return v * 1000 + w;
}

/* A nested function redirects the captured P: no forwarding. */
__attribute__((noinline)) static uint32_t nested(int k)
{
  uint32_t v, w;
  uint32_t *p;
  void redirect(void) { p = &w; }
  v = use(2, 0);
  w = use(9, 0);
  p = &v;
  if (k)
    redirect();
  *p += 50;
  return v * 1000 + w;
}

/* A narrow V only set by a call. */
__attribute__((noinline)) static int narrow(int k)
{
  signed char c;
  signed char *p;
  c = mkc(k);
  p = &c;
  return *p * 10 + c;
}

int main(void)
{
  printf("%u %u\n", call_only(0), call_only(2));
  printf("%u %u %u\n", every_block(0), every_block(1), every_block(2));
  printf("%u %u\n", store_through(2), store_through(3));
  printf("%u %u\n", two_targets(1), two_targets(0));
  printf("%u %u\n", copied(1), copied(0));
  printf("%u %u\n", nested(1), nested(0));
  printf("%d %d\n", narrow(1), narrow(10));
  return 0;
}
