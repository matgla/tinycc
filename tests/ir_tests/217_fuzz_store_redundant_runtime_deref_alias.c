/* Regression for differential-fuzz seed 6447: wrong-code at -O1/-O2.
 *
 * Root cause: tcc_ir_opt_store_redundant (ir/opt_memory.c) treats a runtime
 * LOAD_INDEXED as an aliasing read of its whole array (flushing tracked stores
 * in the array's range), but MISSES a plain DEREF read through a TEMP that
 * holds `array_base + RUNTIME_index` (e.g. `T = &arr[0] + (i<<2); x = *T`).
 * rse_resolve_temp_addr() bails the moment it meets the non-constant addend, so
 * such a read looked like "no read" — and a later store to a *constant* element
 * of the same array then wrongly killed the array's initializer store, even
 * though the runtime DEREF may have read that element first.
 *
 * Here `arr[3] = 0x7fffffff & arr[i&7]` reads arr[4] (i==4) via a runtime
 * base+offset DEREF before `arr[4] = 99` overwrites it; the elided init store
 * to arr[4] left garbage for that read, corrupting the final checksum.  The
 * `~(~(x|0)|0)` wrapper keeps the read a plain DEREF (it blocks LOAD_INDEXED
 * fusion), matching the fuzz seed's IR shape.
 *
 * (-fno-redundant-store-elim / -fno-store-load-fwd / -fno-const-prop all "fix"
 * it; redundant-store-elim is the pass that creates the bad value.)
 *
 * Fix: store_redundant now also flushes the array range on a plain DEREF read
 * whose address resolves to `base + runtime_offset` (rse_resolve_runtime_base).
 * Ground truth (gcc -m32 -funsigned-char): checksum=d98209f5 (== tcc -O0).
 * The unfixed -O1/-O2 build dropped arr[4]'s initializer and read garbage.
 */
#include <stdio.h>

int main(void)
{
  unsigned arr[8] = { 10u, 11u, 12u, 13u, 14u, 15u, 16u, 17u };
  unsigned i = 4u;
  arr[3] = (unsigned)(2147483647u & (~((~((unsigned)(arr[i & 7u]) | 0u)) | 0u)));
  arr[4] = 99u;
  /* second def of i keeps SCCP from folding the read above to a const index */
  for (unsigned g = 0u; g < 2u; g++)
    i = (i * 7u + g) & 7u;
  unsigned cs = 0u;
  for (unsigned k = 0u; k < 8u; k++)
    cs = cs * 31u + arr[k];
  cs = cs * 31u + i; /* keep i live so the loop's 2nd def of i survives DCE */
  printf("checksum=%08x\n", cs);
  return 0;
}
