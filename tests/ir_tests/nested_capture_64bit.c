/* nested_capture_64bit.c — capturing a 64-bit variable (long long or double)
 * from a nested function, as a local and as a parameter.
 *
 * A captured variable reaches the child as MACH_OP_CHAIN_REL, whose
 * needs_deref (from is_lval) means "the value is at [chain+offset]" — the same
 * sense as MACH_OP_PARAM_STACK, not "a pointer lives there".  Five places in
 * the ARM backend carried that rule and every one of them named only
 * PARAM_STACK, so a 64-bit capture loaded the value's low word and then
 * dereferenced it: `ldr ip,[sl,#-8]; ldrd r0,r1,[ip]`.  They now share
 * mach_op_64_names_memory().
 */
#include <stdio.h>

__attribute__((noinline)) static long long ll_loc(int a)
{
  long long q = 42;
  __attribute__((noinline)) long long get(void) { return q + 1; }
  long long before = get();
  q = 0x100000000LL;
  return before + get() + a;
}

/* Written from both sides, so the frame home is exercised, not a snapshot. */
__attribute__((noinline)) static long long ll_par(int a, int b, int c, long long q)
{
  __attribute__((noinline)) void bump(long long by) { q += by; }
  __attribute__((noinline)) long long get(void) { return q; }
  bump(0x100000000LL);
  q += 5;
  bump(1);
  return get() + a + b + c;
}

/* Soft-float doubles take the FP operand path, which had its own copy. */
__attribute__((noinline)) static double d_stk(int a, int b, int c, int d, double x)
{
  __attribute__((noinline)) double scale(void) { return x * 2.0; }
  double before = scale();
  x = 4.5;
  return before + scale() + a + b + c + d;
}

/* A captured 64-bit value passed as a STACK argument of a further call. */
__attribute__((noinline)) static long long sink(int a, int b, int c, int d, long long q, long long r)
{
  return q * 10 + r;
}
__attribute__((noinline)) static long long ll_arg(int a, long long q)
{
  __attribute__((noinline)) long long f(void) { return sink(1, 2, 3, 4, q, q + 1); }
  long long before = f();
  q = 7;
  return before * 100 + f() + a;
}

int main(void)
{
  printf("ll_loc=%lld\n", ll_loc(1));
  printf("ll_par=%lld\n", ll_par(1, 2, 3, 42LL));
  printf("d_stk=%.2f\n", d_stk(1, 2, 3, 4, 1.5));
  printf("ll_arg=%lld\n", ll_arg(1, 3LL));
  return 0;
}
