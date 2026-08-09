/*
 * A predefined macro materialises lazily, at the moment its name is first
 * interned (source/frontend/tccpp.c, predef_materialize).  That moment can be
 * inside a #if expression, where pp_expr tells parse_number() to give every
 * integer constant intmax_t type -- correct for the expression being evaluated,
 * wrong for the macro body being *stored*, which then keeps the 64-bit type for
 * the rest of the translation unit.
 *
 * <limits.h> triggers it on its first real line of work:
 *
 *     #if __SCHAR_MAX__ == __INT_MAX__      <- interns __INT_MAX__ here
 *     ...
 *     #define INT_MAX  __INT_MAX__
 *     #define UINT_MAX (INT_MAX * 2U + 1U)
 *
 * so UINT_MAX became a *signed long long* 4294967295 instead of an unsigned
 * int.  That is not just a wider type: it changes the usual arithmetic
 * conversions, so `(unsigned)x < UINT_MAX` compiled to a signed 64-bit compare
 * and `-1 < UINT_MAX` answered 1 where C says 0.  gcc.c-torture/execute/
 * 20041114-1.c is the in-tree witness -- its tautology stopped folding, leaving
 * an undefined-symbol link error at -O1/-O2.
 *
 * The first #if below stands in for limits.h's: it interns the predefines
 * while pp_expr is set, before any of them is used in ordinary code.
 */
#include <stdio.h>
#include <limits.h>

#if __INT_MAX__ == 2147483647 && __SCHAR_MAX__ == 127 && __SHRT_MAX__ == 32767
#define REACHED 1
#else
#define REACHED 0
#endif

/* The 20041114-1 shape: var<=0 || (unsigned)(var-1) < UINT_MAX is a tautology
 * only under unsigned comparison, so a signed UINT_MAX leaves the call live. */
static int tautology(int var)
{
  return var <= 0 || ((long unsigned)(unsigned)(var - 1) < UINT_MAX);
}

int main(void)
{
  volatile int v = 5;

  printf("reached=%d\n", REACHED);

  /* Sizes: the predefines and everything derived from them stay 32-bit. */
  printf("sz_intmax=%d\n", (int)sizeof(__INT_MAX__));
  printf("sz_INT_MAX=%d\n", (int)sizeof(INT_MAX));
  printf("sz_UINT_MAX=%d\n", (int)sizeof(UINT_MAX));
  printf("sz_SCHAR_MAX=%d\n", (int)sizeof(__SCHAR_MAX__));

  /* Values: -1 converts to UINT_MAX, so the compare is unsigned and false.
   * With UINT_MAX typed long long both operands promote to signed 64-bit and
   * this answers 1. */
  printf("neg_lt_umax=%d\n", (int)(-1 < UINT_MAX));
  printf("umax_plus1=%u\n", (unsigned)(UINT_MAX + 1U));

  printf("taut0=%d\n", tautology(0));
  printf("taut5=%d\n", tautology(v));

  printf("PASS\n");
  return 0;
}
