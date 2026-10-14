/* run_register_coalescing moves a VAR into the register its value arrives in
 * (r0: a call result or a return-value feeder) by swapping registers with the
 * single-register interval that holds r0.  The blocker takes over the VAR's old
 * register for its WHOLE range, which can reach past the VAR's own, and the
 * safety scan only compared the other intervals' LOW register.  A 64-bit
 * interval on r2:r3 inside the blocker's range was not seen when the VAR sat
 * in r3, so the blocker -- here the pointer `s` -- was moved onto r3 and
 * `smull` (long long product) overwrote it with the product's high word.
 *
 * The shape that gets there: the inlined body's return slot V survives
 * allocation (two returns assign it), is the return-value feeder, and is
 * forced onto r3 because j and k hold r1 and r2 where it starts; the pair is
 * dead by then, but `s` (r0) lives from the function entry to V's last
 * definition, `return (unsigned long)s`.
 *
 * The values are chosen so the product's high word is 0: the broken code
 * returned 0 instead of s for every in-range call. */
#include <stdio.h>

typedef struct
{
  unsigned char *tab;
  int len;
} Side;

volatile int sink, sink2, sink3;

__attribute__((always_inline)) static inline unsigned long at8(const Side *s)
{
  sink = (int)(((long long)s->len * s->len) >> 3);
  int j = sink2;
  int k = sink3;
  if (!s->tab)
    return 0;
  if (j < 0 || j >= k)
    return 0;
  return (unsigned long)s;
}

__attribute__((noinline)) unsigned long pick(const Side *s)
{
  return at8(s);
}

int main(void)
{
  unsigned char data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  Side s;

  s.tab = data;
  s.len = 5;

  sink2 = 1;
  sink3 = 3;
  printf("in range: %d\n", pick(&s) == (unsigned long)&s);

  sink3 = 0;
  printf("j >= k: %lu\n", pick(&s));

  s.tab = 0;
  sink3 = 3;
  printf("no tab: %lu\n", pick(&s));

  s.tab = data;
  sink2 = 2;
  sink3 = 4;
  printf("in range again: %d\n", pick(&s) == (unsigned long)&s);
  printf("sink: %d\n", sink);
  return 0;
}
