/* sl_forward_load_redefines_var.c — store-load forwarding must not forward a
 * variable's value across a LOAD that redefines that variable.
 *
 * `u3` is written both directly and through `**pp` (pp == &p, p == &u3).
 * ptr_local_fwd rewrites the pointer accesses into direct `u3` accesses, and
 * sl_forward then tracks `u3 <- T` and forwards T into later reads of u3.
 * Between them sits `u3 = m[g][2]`, which lowers to `V1 <-- T***DEREF***
 * [LOAD]` — a genuine redefinition of u3.
 *
 * sl_forward's redefinition scan handles exactly that shape, but the LOAD
 * branch reached it only when it had something to forward INTO the load: an
 * unresolvable address (here `T25 + 8`, not in the LEA map), a volatile read
 * or an address-taken base all did `continue`, skipping the rest of the loop
 * body — including the scan.  The entry from the earlier store stayed live and
 * the second read forwarded a stale value.  Those three now `goto
 * skip_load_fwd` so the destination's bookkeeping still runs.
 *
 * Found by tests/fuzz agg_deep seed 3292 (tcc -O2/-Os wrong, -O0/-O1 and both
 * gcc levels right), reduced to this.
 */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}

int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u3 = 140438262u;
  unsigned u4 = 2784757854u;
  unsigned m28[4][4] = { {1,2,3,4}, {5,6,7,8}, {9,10,11,12}, {13,14,15,16} };
  unsigned *pa29 = &u3;
  unsigned **ppa210 = &pa29;

  for (unsigned g = 0u; g < 4u; g++) {
    **ppa210 = m28[(u4 & 3u)][(u3 & 3u)];
    cs = csmix(cs, *pa29);
    u3 = m28[(g & 3u)][2] & 0xffffffffu;   /* redefines u3 via a LOAD */
    cs = csmix(cs, **ppa210);              /* must see the redefinition */
  }
  printf("checksum=%08x\n", cs);
  return 0;
}
