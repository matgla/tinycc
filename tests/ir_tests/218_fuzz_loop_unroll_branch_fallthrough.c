/* Regression: loop unroll dropped the exit branch of a loop nested in an
 * if-branch, letting the unrolled body fall through into the else block.
 *
 * Reduced from differential-fuzz gen_c.py seed=6951 (-O2 wrong, -O0 correct).
 *
 * Pass:  try_unroll_loop_ex (ir/opt_loop_utils.c), the ZZ_loop_unroll pass.
 * Bug:   the unroller NOPs the entire loop region (header CMP, exit JUMPIF,
 *        body, back-edge) and writes the unrolled body in place, relying on
 *        fall-through to reach the loop's exit target.  That is only valid when
 *        the exit target is the instruction physically following the loop.
 *        Here the once-iterating `while (g11 < 1)` loop lives in the taken
 *        if-branch; its exit JUMPIF targets the MERGE block, which sits PAST the
 *        else block.  In the original loop the merge is reached ONLY via that
 *        exit JUMPIF (the body's back-edge is unconditional) — never by
 *        fall-through.  After unrolling, the exit JUMPIF was gone and the
 *        unrolled if-branch body fell straight into the else loop, so cs got
 *        mixed 2 (if) + 11 (else) = 13 times instead of 2.
 * Fix:   after writing the unrolled body, emit an explicit JUMP to exit_target
 *        when fall-through does not already land there (mirrors the
 *        need_exit_jump logic in try_rotate_loop).
 *
 * Correct checksum is gcc -m32 -funsigned-char = 5cdc7df8 (this program has no
 * char/long/pointer-width dependency, so native gcc agrees).  tcc -O0/-Os were
 * correct; the bug appeared at -O1/-O2.
 */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  return h * 2654435761u;
}

int main(void)
{
  unsigned cs = 0x12345678u;
  short s4 = (short)(1078557709u & 0xffff);
  unsigned u5 = 2845647487u, u6 = 3773734956u, u7 = 3388906536u, u8 = 3090854514u;
  unsigned arr9[8] = { 55605224u, 2527658735u, 476915998u, 4081724016u,
                       2965002114u, 2475778492u, 981509515u, 2219645079u };

  cs = csmix(cs, 0u);
  if ((unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(u8) ^ (unsigned)(1294208770u))) ^ (unsigned)((-((unsigned)((unsigned)(s4)) | 0u))))) != ((unsigned)(((unsigned)(u7) * (unsigned)((-((unsigned)((unsigned)(s4)) | 0u))))) ^ cs))) | (unsigned)(((unsigned)((~((unsigned)(((unsigned)(u7) - (unsigned)(arr9[((unsigned)(u6) & 7u)]))) | 0u))) + (unsigned)(((unsigned)(3363518941u) + (unsigned)(((unsigned)((unsigned)(s4)) ^ (unsigned)(u7))))))))) & 1u) {
    u7 = u5;
    { unsigned g11 = 0u; while (g11 < 1u) { cs = csmix(cs, 0u); cs = csmix(cs, 0u); g11++; } }
  } else {
    { unsigned g13 = 0u; while (g13 < 5u) { cs = csmix(cs, 0u); cs = csmix(cs, 0u); g13++; } }
    cs = csmix(cs, 0u);
  }
  printf("checksum=%08x\n", cs);
  return 0;
}
