/* Fuzz regression: agg_deep seed 36641 (O1/O2), root cause = redundant-store
 * elimination (ir/opt_memory.c tcc_ir_opt_store_redundant).
 *
 * Two stores in the loop body write the SAME 2-D array element m216[2][1] via
 * different index expressions (m216[3855043490&3][u6&3] and m216[u8&3][1]).
 * Between them sits a read m216[i21&3][1], lowered to a LOAD_INDEXED whose base
 * is a RUNTIME address (`&m216 + ((i21&3)<<4)`) with a CONSTANT column index #4.
 * The RSE LOAD_INDEXED handler only flushed the array range for a runtime INDEX;
 * for a constant index it tried to resolve the base to a fixed offset, which
 * bails on the runtime addend, so it treated the load as "no read" and wrongly
 * eliminated the first store.  When i21&3 == 2 the intervening load reads the
 * just-overwritten element from memory, so dropping the store yields a stale
 * value.
 *
 * Fix: in the constant-index branch, fall back to rse_resolve_runtime_base and
 * flush the whole array range when the base is `arr + runtime` (mirroring the
 * runtime-index branch).
 *
 * Expected checksum is the gcc -m32 -funsigned-char / tcc -O0 oracle value.
 */
#include <stdio.h>
int main(void)
{
  unsigned u6 = 1055387981u; /* u6 & 3 == 1 */
  unsigned u8 = 1513616014u; /* u8 & 3 == 2 */
  unsigned cs = 0x12345678u;
  unsigned m216[4][4] = { { 1464642045u, 1947564958u, 2317504912u, 2722323909u },
                          { 3785269730u, 3156244134u, 171256231u, 2421863616u },
                          { 2781935630u, 196600475u, 2634124507u, 3577152933u },
                          { 398365474u, 1969756494u, 1973336082u, 1450347974u } };
  for (unsigned g = 0u; g < 12u; g++) {
    unsigned i21 = g;
    /* store #1: m216[2][u6&3] == m216[2][1] */
    m216[((unsigned)(3855043490u) & 3u)][((unsigned)(u6) & 3u)] =
        (unsigned)(cs ^ (i21 * 2654435761u));
    /* read #1: m216[2][u6&3] */
    cs += *(&m216[((unsigned)(3855043490u) & 3u)][0] + ((unsigned)(u6) & 3u));
    /* store #2 to the SAME slot m216[u8&3][1] == m216[2][1]; RHS is a
     * runtime-base + constant-index load of m216[i21&3][1]. */
    m216[((unsigned)(u8) & 3u)][((unsigned)(1719555913u) & 3u)] =
        (unsigned)(m216[((unsigned)(i21) & 3u)][((unsigned)(3963124205u) & 3u)]);
    /* read #2: m216[u8&3][1] */
    cs += *(&m216[((unsigned)(u8) & 3u)][0] + ((unsigned)(1719555913u) & 3u));
  }
  printf("checksum=%08x\n", cs);
  return 0;
}
