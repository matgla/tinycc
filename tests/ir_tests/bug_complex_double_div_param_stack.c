/* Soft-float complex double division with the operands still in their
 * incoming stack-parameter slots.  The backend sets up __divdc3 by loading each
 * 64-bit component into a fixed register pair one 32-bit word at a time; a
 * component living in a stack parameter (MACH_OP_PARAM_STACK) kept its double
 * type, so each word load was a 64-bit LDRD into a free scratch high register.
 * For the r0:r1 pair the second load picked r0 as that scratch:
 *     ldrd r0, r1, [r7, #96]
 *     ldrd r1, r0, [r7, #100]    @ r0 = the word after b_im
 * so b_im's low word was replaced by whatever followed it on the stack -- here
 * the canary argument.  Returns 0 on success. */
#include <stdio.h>

__attribute__((noinline)) _Complex double divd(_Complex double a, _Complex double b, unsigned canary)
{
  (void)canary;
  return a / b;
}

int main(void)
{
  volatile double dr = 4.0, di = 6.0, er = 2.0, ei = 2.0;
  _Complex double da = dr + di * 1.0i, db = er + ei * 1.0i;
  _Complex double dd = divd(da, db, 0x12345678u); /* (4+6i)/(2+2i) = 2.5+0.5i */
  printf("%.17g %.17g\n", __real__ dd, __imag__ dd);
  if (__real__ dd != 2.5 || __imag__ dd != 0.5)
    return 1;
  return 0;
}
