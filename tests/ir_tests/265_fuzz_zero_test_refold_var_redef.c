#include <stdio.h>

/*
 * Fuzz switch seed 8261 reduction (O1/O2): float_branch's repeated
 * zero-test fold (ir/opt_branch.c) NOP'd the second `u8 & 1` test even
 * though u8 was redefined between the two tests:
 *
 *   T127 <- V6 AND #1        ; outer `if (u8 & 1)`
 *   TEST_ZERO T127 / JUMPIF ==
 *   V6 <- #23515 XOR V5      ; u8 = 23515 ^ u7  (plain vreg redefinition)
 *   T130 <- V6 AND #1        ; inner `u8 & 1` — folded away
 *
 * The two AND sources are spill-encoded STACKOFF reads of V6, which
 * ir_opt_nonvreg_expr_equal treats as structurally equal, and
 * ir_opt_pure_def_memory_stable did not model the intervening XOR (a
 * plain vreg def, not a STORE op) as mutating the variable.  The inner
 * ternary was folded to its then-arm although the redefined u8 is even.
 * Fix: the stability scan blocks redefinitions of any VAR/PARAM the
 * compared endpoint instructions read (and any lval/stack-slot dest).
 */
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}

volatile unsigned vol_u7 = 3786082649u; /* odd */
volatile unsigned vol_u8 = 2575901527u; /* odd -> outer branch taken */

int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u7 = vol_u7;
  unsigned u8 = vol_u8;

  if (u8 & 1u) {
    u8 = 23515u ^ u7; /* odd ^ odd -> even: inner test must pick the else arm */
    cs = csmix(cs, (u8 & 1u) ? 0x12345u : (u7 * 3u));
  }
  cs = csmix(cs, u8);
  printf("checksum=%08x\n", cs);
  return 0;
}
