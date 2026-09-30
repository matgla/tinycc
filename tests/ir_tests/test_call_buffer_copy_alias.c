/* The Zig C backend copies a call's struct result into a second local before
 * reading it: `t3 = f(); t4 = t3; p = &t4;`.  When the call's own buffer is
 * written by nothing else and the second local by nothing but the copy, the
 * two hold the same bytes from the copy onwards whatever the control flow, so
 * the copy goes and the local is the buffer -- anywhere in the function, not
 * just the entry block.  What must keep its copy: a source read after the
 * copy, a destination written by anything else, a copy not next to its call,
 * and two locals copied from one buffer. */
#include <stdint.h>
#include <stdio.h>

struct big
{
  uint32_t a[8];
};

__attribute__((noinline)) static struct big make(int x, int y)
{
  struct big b;
  for (int i = 0; i < 8; i++)
    b.a[i] = (uint32_t)(x * (i + 1) + y);
  return b;
}

__attribute__((noinline)) static void bump(struct big *p, uint32_t k)
{
  p->a[0] += k;
}

/* the plain shape, outside the entry block */
__attribute__((noinline)) static uint32_t in_branch(int x, int sel)
{
  struct big t3;
  struct big t4;
  const struct big *t5;
  uint32_t r = 0;
  if (sel & 1)
  {
    r += 7;
    t3 = make(x, 1);
    t4 = t3;
    t5 = (const struct big *)&t4;
    r += t5->a[3] + t5->a[7];
  }
  else
  {
    t3 = make(x, 2);
    t4 = t3;
    r += t4.a[1];
  }
  return r;
}

/* in a loop, with the destination read before the copy runs again */
__attribute__((noinline)) static uint32_t in_loop(int x, int n)
{
  struct big s;
  struct big d;
  uint32_t r = 0;
  for (int i = 0; i < n; i++)
  {
    if (i)
      r += d.a[2]; /* the previous iteration's value */
    s = make(x + i, 3);
    d = s;
    r = r * 3 + d.a[0];
  }
  return r;
}

/* the destination is written through a pointer: the writes must stick */
__attribute__((noinline)) static uint32_t written_through_pointer(int x)
{
  struct big s;
  struct big d;
  s = make(x, 4);
  d = s;
  bump(&d, 100);
  bump(&d, 1000);
  return d.a[0] + d.a[5];
}

/* the source is read after the copy: both must keep their own bytes */
__attribute__((noinline)) static uint32_t source_read_after(int x)
{
  struct big s;
  struct big d;
  s = make(x, 5);
  d = s;
  bump(&d, 10000);
  return d.a[0] * 3 + s.a[0];
}

/* two destinations from one buffer */
__attribute__((noinline)) static uint32_t two_copies(int x)
{
  struct big s;
  struct big d1;
  struct big d2;
  s = make(x, 6);
  d1 = s;
  d2 = s;
  bump(&d1, 5);
  return d1.a[0] * 2 + d2.a[0];
}

/* something between the call and the copy */
__attribute__((noinline)) static uint32_t not_adjacent(int x, uint32_t *out)
{
  struct big s;
  struct big d;
  s = make(x, 7);
  *out = s.a[4];
  d = s;
  bump(&d, 3);
  return d.a[0] + *out;
}

int main(void)
{
  unsigned r = 0;
  for (int x = 1; x < 6; x++)
    r = r * 31 + in_branch(x, x);
  printf("%u\n", r);
  printf("%u %u\n", in_loop(2, 4), in_loop(3, 1));
  printf("%u %u\n", written_through_pointer(2), source_read_after(3));
  uint32_t out = 0;
  printf("%u %u %u\n", two_copies(4), not_adjacent(5, &out), out);
  return 0;
}
