/* Guard: a loop condition entered only by FALLTHROUGH plus its own backedge
 * must keep is_jump_target set on the condition instruction.
 *
 * The frontend emitted backward jumps (loop backedges, backward goto) as raw
 * JUMP quads without marking the target (gjmp_addr never flagged it); a
 * forward jump to the same spot would have set the flag via backpatch, but
 * the run-once innermost loop of a bitmask scan
 *
 *     for (uint64_t bits = words[w]; bits; bits &= bits - 1)
 *       for (int idx = base + ctzll(bits), go = idx < limit; go; go = 0) {
 *         if (cand[idx] < 0) continue;   // continue -> go=0 -> backedge
 *         ...
 *       }
 *
 * enters its TEST_ZERO condition only by fallthrough, so the backedge was the
 * only jump to it.  setif_branch_fuse trusted the (missing) flag, fused
 * CMP+SETIF+TEST_ZERO+JUMPIF to CMP+JUMPIF and NOPed the TEST_ZERO the
 * backedge pointed at — the continue path then fell through the NOP onto the
 * fused conditional branch with the BODY's stale flags and re-entered the
 * body without advancing the mask: an infinite loop whenever cand[idx] < 0.
 * The on-device tcc hung compiling anything whose register allocation reached
 * ra_coalesce_graph's bitspan scan (most of the -O2 ir_tests suite).
 */

#include <stdio.h>

int cand[130];
int out;

void scan(unsigned long long *words, int n, int limit, int d, int csrc)
{
  for (unsigned int w = 0; w < (unsigned int)n; w++)
    for (unsigned long long bits = words[w]; bits; bits &= bits - 1)
      for (int idx = (int)(w << 6) + __builtin_ctzll(bits), go = idx < limit; go; go = 0) {
        if (cand[idx] < 0) continue;
        if (idx == d) continue;
        if (csrc >= 0 && idx == csrc) continue;
        out += cand[idx];
      }
}

int main(void)
{
  unsigned long long words[3] = {0x8000000000000105ull, 0x3ull, 0x10ull};
  for (int i = 0; i < 130; i++) cand[i] = (i % 3 == 0) ? -1 : i;
  scan(words, 3, 128, 8, 64);
  printf("out=%d\n", out);
  return 0;
}
