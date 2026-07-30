/* Pointer-IV exit-value substitution regression
   (docs/plan_legacy_loop_ptr_iv_exit_subst_ssa.md).

   Pins the behavior of the pointer-IV exit-value substitution
   (legacy tcc_ir_opt_loop_ptr_iv_exit_subst, replaced by
   ssa:ptr_iv_exit_subst) across the pre-SSA -> SSA migration:

   1. the pr49644 idiom: a pointer walks a local array in a counted loop
      with a compile-time trip count (64 — large enough to defeat full
      unrolling), then `if (p != &b[64]) abort();`.  The substitution
      folds the check; runtime behavior must be identical either way.
   2. the same idiom with a different element size (char, step 1).
   3. a runtime-trip-count control loop with the same post-loop check
      that must NOT be folded (and still behave correctly). */
#include <stdio.h>

extern void abort(void);

volatile int vn = 10;

int main(void)
{
  /* shape 1: static trip count, int elements (step 4) */
  int b[64];
  int *p = b;
  int i;
  for (i = 0; i < 64; i++)
    *p++ = i;
  if (p != &b[64])
    abort();
  printf("static b10=%d\n", b[10]);

  /* shape 2: static trip count, char elements (step 1) */
  char c[100];
  char *q = c;
  for (i = 0; i < 100; i++)
    *q++ = (char)i;
  printf("char q_ok=%d last=%d\n", q == &c[100], c[99]);

  /* shape 3: runtime trip count — must not fold, must stay correct */
  int r[16];
  int *pr = r;
  for (i = 0; i < vn; i++)
    *pr++ = i * 2;
  printf("runtime p_ok=%d r9=%d\n", pr == &r[vn], r[9]);
  return 0;
}
