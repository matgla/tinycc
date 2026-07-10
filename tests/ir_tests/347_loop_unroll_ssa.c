/* Loop unrolling / constant-trip elimination regression
   (docs/plan_legacy_loop_unroll_ssa.md).

   Pins the runtime behavior of the three legacy unroll mutators
   (try_eliminate_loop, try_eliminate_loop_symbolic, try_unroll_loop_ex,
   driven by tcc_ir_opt_loop_unroll) across the pre-SSA -> SSA migration to
   ssa:loop_unroll.  Behavior must be identical at every -O level whether or
   not the loop is folded/unrolled.

   1. constant-trip accumulator whose step is the IV (0+1+2+3+4 = 10) — the
      full-unroll candidate.
   2. symbolic-limit accumulator (SELECT/ITE closed form).  Called with a
      positive, a zero, and a NEGATIVE limit: the zero-trip guard must hold so
      the negative case returns 0, NOT limit*step (test-185 class).
   3. an empty counted loop as the then-arm of an `if`: eliminating it must not
      drop control into the else arm (need_exit_jump, seed-198468 class).
   4. a runtime-trip accumulator that must NOT fold and must stay correct. */
#include <stdio.h>

volatile int vn = 10;
volatile int vz = 0;
volatile int vneg = -4;
volatile int vc1 = 1;
volatile int vc0 = 0;

/* Non-static so the args stay symbolic (no call-site specialisation). */
int sym_accum(int n)
{
  int a = 0;
  for (int i = 0; i < n; i++)
    a += 3;
  return a;
}

int then_arm(int cond)
{
  int r = 1;
  if (cond)
  {
    for (int i = 0; i < 7; i++)
      ;
  }
  else
  {
    r = 2;
  }
  return r;
}

int main(void)
{
  /* shape 1: constant-trip accumulator, step = IV */
  int s = 0;
  for (int i = 0; i < 5; i++)
    s += i;
  printf("accum=%d\n", s);

  /* shape 2: symbolic-limit accumulator + zero-trip guard */
  printf("sym_pos=%d sym_zero=%d sym_neg=%d\n",
         sym_accum(vn), sym_accum(vz), sym_accum(vneg));

  /* shape 3: empty then-arm loop must not fall into the else */
  printf("then=%d else=%d\n", then_arm(vc1), then_arm(vc0));

  /* shape 4: runtime-trip accumulator, must not fold */
  int rt = 0;
  for (int i = 0; i < vn; i++)
    rt += i;
  printf("runtime=%d\n", rt);
  return 0;
}
