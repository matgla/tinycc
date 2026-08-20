/* memmove_to_indexed_stores must keep the DESTINATION's name.
 *
 * The pass folds `memcpy(dst, tmp, k)` of a fully-store-covered stack temp
 * by relocating the covering stores onto dst.  When dst resolved to a stack
 * offset it rebuilt the stores as ANONYMOUS StackLoc writes — but if that
 * offset is a NAMED local's storage, an anonymous store into it is invisible
 * to every name-keyed analysis: the anonymous-StackLoc DCE sees no anonymous
 * reader and kills the stores, var liveness sees no named writer, and NOPing
 * the dst LEA (its only user was the call) un-address-takes the var, so the
 * named LOAD survives alone and reads uninitialized frame.
 *
 * That is the mirror image of the named-VAR source-store guard added for
 * 221_fuzz_inline_memcpy_param_named_local.c.  The destination side became
 * reachable when the inliner started expanding small `static inline`
 * functions and inline_param_copy collapsed the argument copy: the classic
 * memcpy type-pun (`memcpy(&b, &f, sizeof b)` under an inlined helper)
 * produces exactly this shape.  First seen as the on-device self-host break:
 * every compile died with "thop_emit: 'ldr': no variant matched" because the
 * device tcc's thop_feat32_subset read its 64-bit feature-mask parameter
 * from a never-written slot.
 *
 * The fix keeps the dst var's vreg identity on each relocated store (the
 * same named interior-offset shape a struct member write produces), and
 * refuses the fold when a tracked memset contributes coverage (the memset
 * rewrite would still leave an anonymous address of the named slot behind).
 *
 *   pair_subset — the self-host shape: 8-byte pun of a register-passed
 *                 struct param through an inlined helper.
 *   pun4        — the 4-byte float pun (the 221 shape, destination side).
 *   assemble    — named dst fed from explicit halfword stores.
 *   zpun        — memset-covered source into a named dst: the refusal path.
 */
#include <stdio.h>
#include <string.h>

typedef struct {
  unsigned lo, hi;
} pair64;

static inline unsigned long long pair_bits(pair64 f)
{
  unsigned long long b;
  memcpy(&b, &f, sizeof b);
  return b;
}

__attribute__((noinline)) static int pair_subset(unsigned s, pair64 sup)
{
  return (s & (unsigned)pair_bits(sup)) == s;
}

static inline unsigned fbits(float f)
{
  unsigned u;
  memcpy(&u, &f, sizeof u);
  return u;
}

__attribute__((noinline)) static unsigned pun4(float f)
{
  return fbits(f);
}

__attribute__((noinline)) static unsigned long long assemble(unsigned a, unsigned b)
{
  pair64 t;
  t.lo = a;
  t.hi = b;
  unsigned long long out;
  memcpy(&out, &t, sizeof out);
  return out;
}

__attribute__((noinline)) static unsigned long long zpun(unsigned a)
{
  pair64 t;
  memset(&t, 0, sizeof t);
  t.lo = a;
  unsigned long long out;
  memcpy(&out, &t, sizeof out);
  return out;
}

int main(void)
{
  pair64 sup;
  sup.lo = 0x00000040u;
  sup.hi = 0x00010002u;
  printf("subset=%d %d %d\n",
         pair_subset(0x40u, sup),      /* bit present in lo -> 1 */
         pair_subset(0x80u, sup),      /* bit absent -> 0 */
         pair_subset(0u, sup));        /* empty set is always a subset -> 1 */

  printf("pun4=%08x %08x\n", pun4(1.0f), pun4(-2.5f));

  unsigned long long v = assemble(0xdeadbeefu, 0x00c0ffeeu);
  printf("assemble=%08x %08x\n", (unsigned)(v >> 32), (unsigned)v);

  v = zpun(0x12345678u);
  printf("zpun=%08x %08x\n", (unsigned)(v >> 32), (unsigned)v);
  return 0;
}
