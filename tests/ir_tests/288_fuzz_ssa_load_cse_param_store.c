/*
 * struct_byval / combo fuzz seed 34487 reduction (O1/O2 wrong-code):
 * ssa:load_cse tracked a direct stack store whose value was a TEMP, but a
 * later direct store of a PARAM to the same slot neither replaced nor
 * invalidated that tracked value.  The sret copy of r.a then forwarded the
 * stale initializer instead of the later `r.a = p.a` store.
 * Ground truth (tcc -O0 == gcc -O2): a=01234568 b=7edce645
 * (buggy tcc -O1 emitted a=115d8640).
 */
#include <stdio.h>

struct SB5 {
  unsigned a;
  unsigned char b;
};

struct SB8 {
  unsigned a;
  unsigned b;
};

static struct SB8 sbh5(struct SB5 p, unsigned x)
{
  struct SB8 r = { x ^ (p.a * 3u), 4145173561u };
  r.b = 1892749925u;
  r.a = p.a;
  r.b = (unsigned)((-((unsigned)((p.a & 1u) ? 3357789023u : 1154537915u))) ^
                   (3647142368u << (p.b & 31u)));
  return r;
}

int main(void)
{
  struct SB5 p = { 19088744u, 105u };
  struct SB8 r = sbh5(p, 0x12345678u);
  printf("a=%08x b=%08x\n", r.a, r.b);
  return 0;
}
