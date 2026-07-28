/* Hard-float mixed int+float parameter passing — currently MISCOMPILES.
 *
 * `mix` takes int and float parameters whose live ranges overlap.  Under
 * -mfloat-abi=hard the allocator assigns the float a VFP register
 * (LS_VFP_REG_BASE + n) but codegen masks it back to GPR n, so the float
 * parameter (in r1 per the soft argument layout) aliases the int in r0 and is
 * clobbered — mix() computes n*n instead of x*x (see docs/plan_vfp_hard_float.md,
 * "VFP≡GPR accident").  Volatile args + external linkage force a real call so
 * the parameter-passing bug reproduces at every optimization level rather than
 * being constant-folded away.
 *
 * Marked xfail today; the Phase 2-4 rewrite (real s-registers + hard-float ABI)
 * makes it pass — at which point the xfail flips to xpass and is removed. */

int mix(int n, float x, int m)
{
  float y = x * x;
  return n + m + (int)y;
}

/* Volatile function pointer: forces a real indirect call at every optimization
 * level (no inlining/folding), so the mixed-parameter miscompile reproduces
 * uniformly rather than only at -O0/-Os. */
static int (*volatile mixp)(int, float, int) = mix;

int main(void)
{
  volatile int vn = 3, vm = 5;
  volatile float vx = 4.0f;

  /* 3 + 5 + (int)(4.0*4.0) = 24 */
  if (mixp(vn, vx, vm) != 24) return 30;

  return 1;
}
