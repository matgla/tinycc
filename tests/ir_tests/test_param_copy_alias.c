/* A local filled by a whole copy of a by-value struct parameter becomes the
 * parameter's own memory (param_copy_alias).  Covers reads and writes through
 * the local, a parameter split across r1-r3 and the stack, one wholly on the
 * stack, one in registers (copied from its home slot), a large one copied by
 * memmove, and the cases that must keep the copy: the parameter still read
 * after the local changed, and a copy not in the entry block. */
#include <stdio.h>
#include <string.h>

struct M { unsigned a[15]; };
struct B { unsigned a[40]; };
struct S { unsigned lo, hi; };

__attribute__((noinline)) static unsigned sum_m(struct M m)
{
  unsigned r = 0;
  for (int i = 0; i < 15; i++)
    r = r * 3 + m.a[i];
  return r;
}

/* Zig C backend shape: copy, take the address, work through the pointer. */
__attribute__((noinline)) static unsigned read_through(unsigned k, struct M const a1)
{
  struct M t0;
  struct M const *t1;
  t0 = a1;
  t1 = (struct M const *)&t0;
  return t1->a[9] * k + t1->a[10];
}

__attribute__((noinline)) static unsigned write_through(int x, struct M a1)
{
  struct M t0;
  struct M *t1;
  t0 = a1;
  t1 = &t0;
  t1->a[0] += (unsigned)x;
  t1->a[14] = t1->a[13] * 2;
  return sum_m(t0);
}

__attribute__((noinline)) static unsigned on_stack(int a, int b, int c, int d, struct M a1)
{
  struct M t0;
  t0 = a1;
  t0.a[3] = (unsigned)(a + b + c + d);
  return sum_m(t0) ^ t0.a[5];
}

__attribute__((noinline)) static unsigned in_regs(struct S a0, unsigned k)
{
  struct S t0;
  struct S *t1;
  t0 = a0;
  t1 = &t0;
  t1->lo += k;
  return t1->lo * 7 + t1->hi;
}

__attribute__((noinline)) static unsigned big(struct B a1, unsigned k)
{
  struct B t0;
  struct B *t1;
  t0 = a1;
  t1 = &t0;
  for (int i = 0; i < 40; i++)
    t1->a[i] += k * (unsigned)i;
  unsigned r = 0;
  for (int i = 0; i < 40; i++)
    r = r * 5 + t1->a[i];
  return r;
}

/* The parameter is read after the local changed: the copy must stay. */
__attribute__((noinline)) static unsigned param_after(struct M a1)
{
  struct M t0;
  t0 = a1;
  t0.a[2] = 1000;
  return t0.a[2] + a1.a[2];
}

/* The copy is not in the entry block. */
__attribute__((noinline)) static unsigned late_copy(int flag, struct M a1)
{
  struct M t0;
  memset(&t0, 0, sizeof(t0));
  if (flag)
    t0 = a1;
  return sum_m(t0);
}

int main(void)
{
  struct M m;
  struct B b;
  struct S s = {5, 9};
  for (int i = 0; i < 15; i++)
    m.a[i] = (unsigned)(i * i + 1);
  for (int i = 0; i < 40; i++)
    b.a[i] = (unsigned)(i * 3 + 2);
  printf("%u\n", read_through(3, m));
  printf("%u %u\n", write_through(4, m), m.a[0]);
  printf("%u\n", on_stack(1, 2, 3, 4, m));
  printf("%u %u\n", in_regs(s, 6), s.lo);
  printf("%u %u\n", big(b, 3), b.a[39]);
  printf("%u\n", param_after(m));
  printf("%u %u\n", late_copy(0, m), late_copy(1, m));
  return 0;
}
