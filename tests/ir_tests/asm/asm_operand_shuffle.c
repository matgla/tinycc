/* Inline asm operand marshalling: one operand's destination register must not
 * clobber another operand's still-pending source.
 *
 * Float parameters are what made this bite: the allocator placed them in
 * registers that permute against the ones the constraint solver picked (a in
 * r2, b in r0, while "r" wanted r0 and r1), so loading %1 destroyed the source
 * of %2 and both operands ended up holding `a`.  Every __asm__ over two
 * same-typed float operands silently computed f(a, a) -- including
 * libvfpv4sp's own __aeabi_fadd/fsub/fmul/fdiv.
 *
 * The bodies here are integer mov/add on the raw float bits: the point is which
 * value reaches each operand register, not the arithmetic, and staying off the
 * FPU keeps the case independent of -mfpu. */

unsigned two_floats(float a, float b)
{
  unsigned r;
  __asm__ volatile("mov %0, %1\n\tadd %0, %0, %2" : "=r"(r) : "r"(a), "r"(b));
  return r;
}

unsigned three_floats(float a, float b, float c)
{
  unsigned r;
  __asm__ volatile("mov %0, %1\n\tadd %0, %0, %2\n\tadd %0, %0, %3"
                   : "=r"(r)
                   : "r"(a), "r"(b), "r"(c));
  return r;
}

unsigned two_ints(int a, int b)
{
  unsigned r;
  __asm__ volatile("mov %0, %1\n\tadd %0, %0, %2" : "=r"(r) : "r"(a), "r"(b));
  return r;
}
