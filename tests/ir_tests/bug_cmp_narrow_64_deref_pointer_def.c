/* cmp_narrow_64 must not prove a loaded 64-bit operand's high word zero from
 * the definition of the *pointer* it is loaded through.
 *
 * cmp_narrow_64 narrows a 64-bit CMP to 32 bits once both operands provably
 * have a zero high word.  Its proof helper looked up the defining instruction
 * of the operand's vreg without asking whether the operand was a deref at all,
 * so for `*(unsigned long long *)(q & 0x7ffffff8u)` it inspected the AND that
 * produced the *address*: a positive 32-bit mask has a zero high word, and the
 * compare was rewritten to a 32-bit cmp of the loaded low word only.  The high
 * word of the value in memory is never read.
 *
 * X[0] = 0x100000007 has a nonzero high word and a small low word, so ==, <
 * and > all disagree with a low-word-only compare.  Y[0] = 0x1234 has a zero
 * high word and must still compare equal, and mask_cmp pins the narrowing that
 * does apply: an operand that really is a register, ANDed with a positive 32-bit
 * mask, has a zero high word. */
#include <stdio.h>

__attribute__((noinline)) int eq_masked(unsigned a, unsigned q)
{
  return (unsigned long long)a == *(unsigned long long *)(q & 0x7ffffff8u);
}

__attribute__((noinline)) int lt_masked(unsigned a, unsigned q)
{
  return (unsigned long long)a < *(unsigned long long *)(q & 0x7ffffff8u);
}

__attribute__((noinline)) int gt_masked(unsigned a, unsigned q)
{
  return (unsigned long long)a > *(unsigned long long *)(q & 0x7ffffff8u);
}

__attribute__((noinline)) int mask_cmp(unsigned r, unsigned n)
{
  unsigned long long m = (unsigned long long)r & 0x7fffffffull;
  return m > n;
}

/* const -> flash, and both arrays must sit below 0x80000000 for the mask to
 * keep the address intact. */
const unsigned long long X[2] __attribute__((aligned(8))) = { 0x100000007ull, 0 };
const unsigned long long Y[2] __attribute__((aligned(8))) = { 0x1234ull, 0 };

int main(void)
{
  /* +3, then masked down to 8 bytes: the AND is what the pass reasons about. */
  unsigned px = (unsigned)X + 3;
  unsigned py = (unsigned)Y + 3;

  if (eq_masked(7u, px) != 0)
  {
    printf("FAIL eq-low-words-match\n");
    return 1;
  }
  if (lt_masked(8u, px) != 1)
  {
    printf("FAIL lt-high-word-decides\n");
    return 1;
  }
  if (gt_masked(8u, px) != 0)
  {
    printf("FAIL gt-high-word-decides\n");
    return 1;
  }
  if (eq_masked(0x1234u, py) != 1)
  {
    printf("FAIL eq-zero-high-word\n");
    return 1;
  }
  if (mask_cmp(0x7fffffffu, 4u) != 1)
  {
    printf("FAIL register-operand-narrowing\n");
    return 1;
  }

  printf("PASS\n");
  return 0;
}
