/* ssa:licm hoisted `-u & m` out of a loop although the loop's call to a
 * nested function rewrote u every iteration: a VAR source with no def inside
 * the loop counted as invariant, and a local captured by a nested function is
 * written through the static chain, never by a def (or an `&u`) the pass sees.
 * Found by the gen_c.py "hazard" fuzz profile (FUZZ_HZ_KINDS=hz_nested seed 26,
 * -O2/-Os). */
#include <stdio.h>

__attribute__((noinline)) unsigned id(unsigned x) { return x; }

int main(void)
{
  unsigned u = 5u, m = id(0xffu), cs = 0u;
  for (unsigned k = 0u; k < 4u; k++) {
    unsigned bump(unsigned x) { u = u * 3u + x; return u; }
    cs = cs * 31u + bump(-u & m);
  }
  printf("%u %u\n", cs, u);
  return 0;
}
