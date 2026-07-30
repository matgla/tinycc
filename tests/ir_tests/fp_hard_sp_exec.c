/* Hard-float (-mfloat-abi=hard -mfpu=fpv5-sp-d16) execution gate — pure
 * single-precision.  Exercises arithmetic, negate, compare, int<->float
 * conversion, and float parameters/returns across calls.  Returns 1 on
 * success, else a distinct diagnostic code.  Avoids printf-with-float so no
 * float value crosses into the soft-ABI runtime — the result is an exit code.
 *
 * Passes today (hard-float is currently byte-identical to softfp) and is the
 * regression net that must stay green through the Phase 2-4 VFP-register
 * rewrite (docs/plan_vfp_hard_float.md).  Mixed int+float parameter passing is
 * covered separately (fp_hard_mixed_exec.c) because it currently miscompiles. */

static float addf(float a, float b) { return a + b; }
static float subf(float a, float b) { return a - b; }
static float mulf(float a, float b) { return a * b; }
static float divf(float a, float b) { return a / b; }
static float negf(float a) { return -a; }

int main(void)
{
  if ((int)addf(10.0f, 3.0f) != 13) return 10;
  if ((int)subf(10.0f, 3.0f) != 7) return 11;
  if ((int)mulf(10.0f, 3.0f) != 30) return 12;
  if ((int)divf(10.0f, 3.0f) != 3) return 13;
  if ((int)negf(5.0f) != -5) return 14;

  float a = 2.5f, b = 2.5f, c = 3.5f;
  if (!(a == b)) return 20;
  if (!(a < c)) return 21;
  if (a > c) return 22;

  /* loop accumulation (register pressure): 10 * 1.5 = 15.0 */
  float acc = 0.0f;
  for (int i = 0; i < 10; i++)
    acc = acc + 1.5f;
  if ((int)acc != 15) return 40;

  return 1;
}
