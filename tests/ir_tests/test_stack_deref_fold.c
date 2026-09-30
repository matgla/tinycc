/* Loads and stores through a pointer temporary holding a constant frame address
 * become direct stack accesses (ssa:stack_deref_fold).  The shapes the Zig C
 * backend emits: a pointer to each sub-object, set in every arm of a switch;
 * narrow fields of either signedness; indexed element accesses; a pointer that
 * is also passed on; and one chosen by a merge, which must not be folded. */
#include <stdio.h>
#include <stdint.h>

struct sl
{
  const uint8_t *ptr;
  uintptr_t len;
};
struct ver
{
  uintptr_t major, minor, patch;
  struct sl pre, build;
};
struct range
{
  struct ver min, max;
  int8_t tag;
  uint16_t flags;
};

__attribute__((noinline)) static unsigned keep(const struct range *r)
{
  return (unsigned)(r->min.major * 1000 + r->max.minor * 100 + r->tag + r->flags);
}

__attribute__((noinline)) static unsigned select_range(int k)
{
  struct range t0;
  struct ver *t2;
  uintptr_t *t3;
  struct sl *t4;
  int8_t *tag;
  uint16_t *fl;
  switch (k)
  {
  case 1:
    t2 = &t0.min;
    t3 = &t2->major;
    *t3 = 5;
    t3 = &t2->minor;
    *t3 = 0;
    t4 = &t2->pre;
    *t4 = (struct sl){NULL, (uintptr_t)0xaaaaaaaau};
    t2 = &t0.max;
    t3 = &t2->major;
    *t3 = 5;
    t3 = &t2->minor;
    *t3 = 1;
    tag = &t0.tag;
    *tag = -3;
    fl = &t0.flags;
    *fl = 0xfffe;
    break;
  default:
    t2 = &t0.min;
    t3 = &t2->major;
    *t3 = 27;
    t2 = &t0.max;
    t3 = &t2->minor;
    *t3 = 30;
    tag = &t0.tag;
    *tag = 7;
    fl = &t0.flags;
    *fl = 1;
    break;
  }
  /* Read back through fresh pointers, then pass the whole thing on. */
  int8_t *rt = &t0.tag;
  uint16_t *rf = &t0.flags;
  return keep(&t0) + (unsigned)(*rt * 10) + *rf;
}

/* Indexed accesses off a pointer into a local array. */
__attribute__((noinline)) static int indexed(int k)
{
  int16_t a[8];
  int16_t *p = &a[2];
  p[0] = (int16_t)-k;
  p[1] = 300;
  p[3] = (int16_t)(k * 2);
  a[0] = 1;
  a[1] = 2;
  return a[0] + a[1] + a[2] * 10 + a[3] + a[5] * 100 + p[3];
}

/* The pointer comes from a merge: it names one of two objects. */
__attribute__((noinline)) static int merged(int k)
{
  int x[2] = {1, 2}, y[2] = {30, 40};
  int *p;
  if (k)
    p = &x[1];
  else
    p = &y[0];
  *p += 100;
  return x[0] + x[1] + y[0] + y[1] + *p;
}

int main(void)
{
  printf("%u %u\n", select_range(1), select_range(2));
  printf("%d %d\n", indexed(3), indexed(-4));
  printf("%d %d\n", merged(1), merged(0));
  return 0;
}
