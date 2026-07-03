/* Regression: a large constant local array initializer lowers to a memcpy()
 * call, but register allocation did not treat the BLOCK_COPY as a call site, so
 * a value live across it kept its caller-saved register and was clobbered.
 *
 * From agg_deep fuzz seed 0 (reduced).  tcc -O0/-O1/-O2/-Os all disagreed with
 * gcc (and with each other) — an O0-class codegen miscompile.
 *
 * Root cause: tcc_ir_opt_block_copy_init rewrites `memset(0) + N constant
 * stores` into one `TCCIR_OP_BLOCK_COPY` from a .rodata template.  The backend
 * (tcc_gen_machine_block_copy_mop) lowers a copy of >= 64 bytes to a real
 * memcpy() call, which clobbers r0-r3/r12/lr; smaller copies use an inline
 * LDM/STM that saves/restores every scratch it touches.  ra_build_call_prefix
 * counted FUNCCALL* and soft-float ops as calls but not BLOCK_COPY, so a value
 * live across a >= 64-byte copy (here `cs`, the csmix accumulator) was left in
 * r0 and destroyed by the memcpy before the following csmix() call read it.
 *
 * Fix: ra_build_call_prefix now treats a BLOCK_COPY whose size is at least
 * TCCIR_BLOCK_COPY_MEMCPY_MIN_BYTES as a call, forcing values live across it off
 * the caller-saved registers (they land in r4/r5).  The inline (small) path is
 * unchanged.  m64[16] is 64 bytes -> the memcpy path; it is also summed into the
 * checksum so a corrupted copy would be caught too.
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
  unsigned u6 = 3699512615u;
  /* 16 words = 64 bytes: at/above the memcpy-lowering threshold. */
  unsigned m64[16] = { 594668076u, 1165540019u, 402470710u, 153182031u,
                       1158018294u, 2505903735u, 1550082102u, 556803596u,
                       493476348u, 3137566955u, 4010443245u, 1322333523u,
                       3129574109u, 1299960583u, 3588701759u, 1285163745u };

  /* cs and u6 are live across the array initializer's memcpy. */
  cs = csmix(cs, u6);
  for (unsigned i = 0u; i < 16u; i++)
    cs = csmix(cs, m64[i]);
  printf("checksum=%08x\n", cs);
  return 0;
}
