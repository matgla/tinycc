/* GVN value-numbering of "const" runtime helpers (gvn_try_pure_call).
 *
 * Two calls to a helper that touches no memory are interchangeable when their
 * arguments are equal — that is what lets duplicate soft-float compares and
 * conversions collapse, and what keeps `isunordered(x,y) ||
 * !isunordered(x,y)` provably true.  Everything below pins a way that
 * reasoning can go wrong.
 *
 *  1. DISTINCT double constants that share their low 32 bits.  This is the
 *     bug that shipped in the first prototype: the key read the argument with
 *     irop_get_imm64_ex and narrowed it to int32, but an F64 operand yields
 *     the raw double bits, and 1.0, -1.0, 2.0, -2.0 and 0.0 ALL have zero low
 *     words.  Every one of these divisions then collapsed onto the first and
 *     -inf came back as +inf (this is gcc torture ieee/hugeval, which only
 *     caught it because it aborts).  Keyed on the pool index instead, they
 *     stay distinct.
 *  2. Genuine duplicates DO collapse and still give the right answer.
 *  3. A helper result must not be reused across a write to one of its
 *     arguments...
 *  4. ...nor across an opaque call that could write it...
 *  5. ...nor when an argument is volatile, where each read is mandated.
 *  6. The tautology that motivated the pass, plus its negation, so a rewrite
 *     that folds one arm the wrong way shows up.
 *  7. Arity: a 1-argument helper and a 2-argument one must never share a key.
 */
#include <stdio.h>

/* `static const` so the frontend materializes them as F64 pool constants —
 * the operand class that carried the truncation bug. */
static const double zero  =  0.0;
static const double pone  =  1.0;
static const double none  = -1.0;
static const double ptwo  =  2.0;
static const double ntwo  = -2.0;

double dv, dw;
volatile double dvol;
int sink;

void opaque(void) { dv = 4.0; }

int main(void)
{
  /* 1: same low word, different values — must NOT merge. */
  printf("inf=%f %f %f %f\n", pone / zero, none / zero, ptwo / zero, ntwo / zero);
  printf("cmp=%d%d%d%d\n",
         pone / zero > 0.0, none / zero > 0.0,
         ptwo / zero < 0.0, ntwo / zero < 0.0);

  /* Same idea through multiply and compare rather than divide. */
  printf("mul=%f %f\n", pone * ptwo, none * ptwo);
  printf("eq=%d%d\n", pone == none, pone == pone);

  dv = 3.0;
  dw = 3.0;

  /* 2: genuine duplicates — collapsing them must not change the answer. */
  printf("dup=%d%d %f %f\n", dv == dw, dv == dw, dv + dw, dv + dw);

  /* 3: a write to an argument between two calls invalidates the first. */
  double a1 = dv + dw;
  dv = 10.0;
  double a2 = dv + dw;
  printf("wr=%f %f\n", a1, a2);

  /* 4: an opaque call can write a global argument. */
  dv = 5.0;
  double b1 = dv + dw;
  opaque();                 /* sets dv = 4.0 */
  double b2 = dv + dw;
  printf("call=%f %f\n", b1, b2);

  /* 5: volatile argument — every read is a mandated access. */
  dvol = 1.0;
  double c1 = dvol + dw;
  dvol = 2.0;
  double c2 = dvol + dw;
  printf("vol=%f %f\n", c1, c2);

  /* 6: the tautology, and its always-false negation. */
  double x = dv, y = dw;
  printf("taut=%d%d\n",
         __builtin_isunordered(x, y) || !__builtin_isunordered(x, y),
         __builtin_isunordered(x, y) && !__builtin_isunordered(x, y));

  /* 7: one-arg and two-arg helpers keyed apart.  (float)x is a 1-arg
   * conversion helper; x < y is a 2-arg compare on the same value. */
  printf("mix=%f %d\n", (double)(float)x, x < y);
  return 0;
}
