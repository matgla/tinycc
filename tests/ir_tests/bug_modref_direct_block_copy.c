/* A direct memmove into a global writes that global.
 *
 * tcc_ir_call_may_write skipped block-copy helpers without a summary, which
 * is right only for one reached through a callee (its effect is in that
 * callee's summary).  For a direct call it said `g = make()` -- a memmove into
 * g -- does not write g, so copy-source load forwarding answered a read of an
 * earlier copy of g with g's NEW value (-O1/-O2). */
#include <stdio.h>
#include <stdint.h>

/* Returned in r0, then stored into g with a memmove: 4 single-byte fields
 * are more chunks than the inline small-aggregate copy takes. */
struct quad
{
  uint8_t a, b, c, d;
};

struct quad g = {0x34, 2, 3, 4};

__attribute__((noinline)) static struct quad make(uint8_t v)
{
  struct quad t = {v, v, v, v};
  return t;
}

int main(void)
{
  unsigned sum = 0;

  struct quad copy;
  copy = g;      /* copy the global */
  g = make(9);   /* then overwrite it through a memmove */
  sum += copy.a + g.a;

  printf("%u\n", sum); /* 0x34 + 9 = 61 */
  return 0;
}
