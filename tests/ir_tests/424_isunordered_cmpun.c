/* __builtin_isunordered lowering.
 *
 * It used to expand to `isnan(x) | isnan(y)`, which forced BOTH operands
 * through __aeabi_f2d first (isnan takes a double) and then made two more
 * library calls.  It is now a single __aeabi_fcmpun / __aeabi_dcmpun call,
 * which returns exactly 1/0 — so the observable result must be unchanged for
 * every operand class, and a float pair must NOT be widened to double.
 *
 * What each block pins:
 *  1. every ordered/unordered combination for FLOAT operands, including the
 *     asymmetric ones (NaN on the left vs the right) — picking the wrong
 *     helper or dropping an operand shows up here;
 *  2. the same for DOUBLE operands, which take the dcmpun path;
 *  3. MIXED float/double, where the float side must still be widened before
 *     the double helper is called;
 *  4. the derived macros (ORD/UNORD/UNEQ/UNLT/UNLE/UNGT/UNGE) exactly as the
 *     gcc torture compare-fp tests build them, since those are what the
 *     change was made for;
 *  5. __builtin_islessgreater, which already called the cmpun helpers, so it
 *     must be unaffected;
 *  6. the compile-time-constant path, which folds without any call.
 *
 * -0.0 and the infinities are included because they are ordered: a helper
 * that tested "exponent all ones" without checking the mantissa would call
 * them unordered.
 */
#include <stdio.h>

/* Runtime values: globals so nothing is constant-folded. */
float fnan, fpinf, fninf, fzero, fnzero, fone;
double dnan, dpinf, dninf, dzero, done;

#define ORD(a, b)   (!__builtin_isunordered ((a), (b)))
#define UNORD(a, b) (__builtin_isunordered ((a), (b)))
#define UNEQ(a, b)  (__builtin_isunordered ((a), (b)) || ((a) == (b)))
#define UNLT(a, b)  (__builtin_isunordered ((a), (b)) || ((a) <  (b)))
#define UNLE(a, b)  (__builtin_isunordered ((a), (b)) || ((a) <= (b)))
#define UNGT(a, b)  (__builtin_isunordered ((a), (b)) || ((a) >  (b)))
#define UNGE(a, b)  (__builtin_isunordered ((a), (b)) || ((a) >= (b)))

int main(void)
{
  fnan = __builtin_nanf("");
  fpinf = __builtin_inff();
  fninf = -__builtin_inff();
  fzero = 0.0f;
  fnzero = -0.0f;
  fone = 1.0f;
  dnan = __builtin_nan("");
  dpinf = __builtin_inf();
  dninf = -__builtin_inf();
  dzero = 0.0;
  done = 1.0;

  /* 1: float operands, both orders. */
  printf("f=%d%d%d%d%d%d%d%d\n",
         __builtin_isunordered(fnan, fnan),
         __builtin_isunordered(fnan, fone),
         __builtin_isunordered(fone, fnan),
         __builtin_isunordered(fone, fone),
         __builtin_isunordered(fpinf, fninf),
         __builtin_isunordered(fpinf, fnan),
         __builtin_isunordered(fzero, fnzero),
         __builtin_isunordered(fninf, fninf));

  /* 2: double operands. */
  printf("d=%d%d%d%d%d%d\n",
         __builtin_isunordered(dnan, dnan),
         __builtin_isunordered(dnan, done),
         __builtin_isunordered(done, dnan),
         __builtin_isunordered(done, done),
         __builtin_isunordered(dpinf, dninf),
         __builtin_isunordered(dzero, dzero));

  /* 3: mixed float/double — the float side must be widened first. */
  printf("m=%d%d%d%d\n",
         __builtin_isunordered(fnan, done),
         __builtin_isunordered(fone, dnan),
         __builtin_isunordered(fone, done),
         __builtin_isunordered(dnan, fone));

  /* 4: the compare-fp torture macros. */
  printf("o=%d%d%d%d\n", ORD(fone, fone), ORD(fnan, fone),
         UNORD(fone, fone), UNORD(fnan, fone));
  printf("u=%d%d%d%d%d\n", UNEQ(fnan, fone), UNEQ(fone, fone),
         UNLT(fnan, fone), UNLE(fone, fone), UNGT(fone, fnan));
  printf("v=%d%d%d%d\n", UNGE(fnan, fone), UNGE(fone, fone),
         UNLT(fone, fone), UNGT(fone, fone));

  /* 5: islessgreater already used the cmpun helpers — must not regress. */
  printf("l=%d%d%d%d\n",
         __builtin_islessgreater(fone, fnan),
         __builtin_islessgreater(fone, fone),
         __builtin_islessgreater(fzero, fone),
         __builtin_islessgreater(done, dnan));

  /* 6: compile-time constants fold with no call at all. */
  printf("c=%d%d%d\n",
         __builtin_isunordered(1.0f, 2.0f),
         __builtin_isunordered(__builtin_nanf(""), 2.0f),
         __builtin_isunordered(1.0, __builtin_nan("")));

  /* 7: __builtin_isnan on a FLOAT now calls isnanf instead of widening with
   * __aeabi_f2d and calling isnan(double).  Results must be identical for
   * every class, including the infinities and -0.0 (all ordered numbers).
   * The double and explicit -f/-l variants must be unaffected. */
  printf("n=%d%d%d%d%d\n",
         __builtin_isnan(fnan), __builtin_isnan(fone),
         __builtin_isnan(fpinf), __builtin_isnan(fninf),
         __builtin_isnan(fnzero));
  printf("N=%d%d%d%d\n",
         __builtin_isnan(dnan), __builtin_isnan(done),
         __builtin_isnanf(fnan), __builtin_isnanf(fone));
  /* isnanf applied to a double argument must still narrow to float first. */
  printf("F=%d%d\n", __builtin_isnanf(dnan), __builtin_isnanf(done));
  /* isinf is the pattern isnan now mirrors — keep it pinned alongside. */
  printf("i=%d%d%d%d\n",
         !!__builtin_isinf(fpinf), !!__builtin_isinf(fone),
         !!__builtin_isinf(dninf), !!__builtin_isinf(dnan));
  return 0;
}
