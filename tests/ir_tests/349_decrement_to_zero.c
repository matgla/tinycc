/* Decrement-to-zero regression (docs/plan_legacy_loop_decrement_to_zero_ssa.md).

   Pins the runtime behavior of the count-up -> count-down-to-zero rewrite
   (ssa:decrement_to_zero, engine dtz_try_region) across the pre-SSA -> SSA
   migration and the codegen SUBS/CMP#0 fusion that consumes it (the peephole
   now scans over the out-of-SSA phi self-move that sits between CMP and JUMPIF
   in a rotated count-down latch).  Output must be identical at every -O level
   whether or not the loop is rewritten.

   FIRES (constant limit, pure counter, plain/volatile store body):
   - spin100 / spin7 / spin50: `g=0; for(i=0;i<N;i++) g++;` — the -O1+ latch
     becomes `subs; bne` (count-down); the returned g==N verifies the rewrite
     kept the exact trip count.
   DECLINES (must stay correct):
   - IV read in body (`sink=i` — not a pure counter): result sink==N-1.
   - symbolic limit (`count_to(n)` — dtz needs a constant limit): result n.
   - runtime limit (`for(i=0;i<nrt;i++)` — nrt volatile): result nrt.
   - nested counted loops: outermost-only detection / collapse must be exact. */
#include <stdio.h>

volatile int g;
volatile int sink;
volatile int nrt = 100;

/* constant-limit pure counter -> fires; g==N confirms exact trip count */
static int spin(void)  { g = 0; for (int i = 0; i < 100; i++) g++; return g; }
static int spin7(void) { g = 0; for (int i = 0; i < 7;   i++) g++; return g; }
static int spin50(void){ g = 0; for (int i = 0; i < 50;  i++) g++; return g; }

/* symbolic limit -> declines, must stay correct */
static int count_to(int n) { g = 0; for (int i = 0; i < n; i++) g++; return g; }

int main(void)
{
  printf("spin=%d s7=%d s50=%d\n", spin(), spin7(), spin50());

  /* IV read in body -> decline, stays correct */
  for (int i = 0; i < 30; i++)
    sink = i;
  printf("sink=%d\n", sink);

  /* nested counted loops, exact product */
  int prod = 0;
  for (int i = 0; i < 12; i++)
    for (int j = 0; j < 5; j++)
      prod++;
  printf("prod=%d\n", prod);

  /* symbolic-limit decline */
  int sym = count_to(42);

  /* runtime-limit decline */
  g = 0;
  for (int i = 0; i < nrt; i++)
    g++;
  int runtime = g;

  printf("sym=%d runtime=%d\n", sym, runtime);
  return 0;
}
