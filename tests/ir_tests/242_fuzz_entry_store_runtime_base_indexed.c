/* Regression: entry-store propagation forwarded a stack array element's
 * initializer across a STORE_INDEXED whose base was a RUNTIME array pointer but
 * whose index displacement was a constant — so the store was mis-classified as
 * a fully-constant address and never invalidated the array's other elements.
 *
 * From agg_deep fuzz seed 70 (reduced).  tcc -O0/-Os agreed with gcc; tcc
 * -O1/-O2 diverged — an O1/O2 store-load-forwarding miscompile.
 *
 * Root cause: `m28[u4&3][u3&3] = ...` writes a 2-D array with runtime indices.
 * Taking `&u4` (via the pointer chain) makes u4 address-taken, so u4&3 stays a
 * RUNTIME row index and the store lowers to `STORE_INDEXED base=(&m28 +
 * (u4&3)*16), index=#12` — a runtime base with a *constant* column index.
 * tcc_ir_opt_entry_store_prop (ir/opt_memory.c) Phase 2.6 skipped any
 * STORE_INDEXED with an immediate index as "constant index handled elsewhere",
 * but Phase 2.5 only resolves constant (lea_map) bases, not runtime (rt_base)
 * ones.  So this store invalidated nothing, and the later constant-offset load
 * `m28[u3&3][u3&3]` (== m28[3][3]) was forwarded m28[3][3]'s stale .rodata
 * initializer instead of the just-stored value.
 *
 * Fix: Phase 2.6 now skips only a fully-constant address (constant base +
 * constant index); a runtime base (rt_base) with an immediate index still
 * invalidates the whole array's entry initializers.
 *
 * Needs: a 2-D array with a runtime row index, an address-taken index variable
 * (so the index is not const-folded), and a constant-offset read-back of the
 * written element.  u3&3 == u4&3 == 3 and u3&7 == 3 at runtime, so the store
 * and the read-back touch the same cell m28[3][3].
 */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  h = (h << 13) | (h >> 19);
  return h * 2654435761u;
}

int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u3 = 2568500203u;   /* u3 & 3 == 3, u3 & 7 == 3 */
  unsigned u4 = 1632471131u;   /* u4 & 3 == 3 */
  unsigned arr5[8] = { 2787566456u, 2167748930u, 3315229705u, 3819694717u,
                       3680276846u, 402101631u, 2108499546u, 577754839u };
  unsigned m28[4][4] = { { 3229613666u, 1329186547u, 523047571u, 3606289726u },
                         { 2829927831u, 3129901980u, 1693169304u, 647205869u },
                         { 3983146138u, 3319978093u, 500979036u, 3853976788u },
                         { 249101730u, 933437633u, 3665499280u, 547568943u } };
  unsigned *pa29 = &u4;        /* u4 address-taken -> u4&3 stays runtime */
  unsigned **ppa210 = &pa29;

  m28[u4 & 3u][u3 & 3u] = arr5[u3 & 7u];    /* STORE_INDEXED, runtime base + #12 */
  arr5[u3 & 7u] = m28[u3 & 3u][u3 & 3u];    /* read m28[3][3] -> must see the store */
  for (unsigned k = 0u; k < 8u; k++)
    cs = csmix(cs, arr5[k]);
  (void)ppa210;
  printf("checksum=%08x\n", cs);
  return 0;
}
