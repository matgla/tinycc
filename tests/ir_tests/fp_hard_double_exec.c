/* Hard-float double argument/return ABI (AAPCS-VFP).
 *
 * Doubles travel in d0-d7 even on a single-precision-only FPU: the ABI says
 * where arguments live, not which arithmetic exists, so a callee unpacks d0
 * into a GPR pair to call __aeabi_dadd.  This mirrors what arm-none-eabi-gcc
 * does for -mfpu=fpv5-sp-d16, which is what lets our objects interoperate with
 * its libm.
 *
 * Covers the allocation rules that are easy to get wrong: independent GPR / VFP
 * banks, the even-alignment requirement for doubles, and back-filling (a float
 * can occupy an s-register that a later double had to skip over).
 *
 * Returns 1 on success, else a distinct diagnostic code.
 */

static double addd(double a, double b) { return a + b; }

/* d0, s2, d2 — b must land in d2, not d1, because s2 is taken by f; s3 stays
 * free for a later float.  n rides the GPR bank independently. */
static double mixd(int n, double a, float f, double b) { return a + b + f + n; }

/* Fills the double bank: d0..d5 plus a trailing float. */
static double many(double a, double b, double c, double d, double e, double f, float g)
{
    return a + b + c + d + e + f + g;
}

/* Back-fill: the float takes s0, so the first double must skip to d1 (s2:s3). */
static double backfill(float g, double a, double b) { return g + a + b; }

int main(void)
{
    volatile double x = 1.5, y = 2.25;
    volatile float g = 0.25f;
    volatile int n = 1;

    if ((int)(addd(x, y) * 100) != 375)
        return 10;

    /* 1.5 + 2.25 + 0.25 + 1 = 5.0 */
    if ((int)(mixd(n, x, g, y) * 100) != 500)
        return 11;

    /* 1+2+3+4+5+6 + 0.5 = 21.5 */
    if ((int)(many(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 0.5f) * 10) != 215)
        return 12;

    /* 0.25 + 1.5 + 2.25 = 4.0 */
    if ((int)(backfill(g, x, y) * 100) != 400)
        return 13;

    /* Double returned from one call feeding the next: the return register and
     * the argument register have to agree. */
    if ((int)(addd(addd(x, y), x) * 100) != 525)
        return 14;

    return 1;
}
