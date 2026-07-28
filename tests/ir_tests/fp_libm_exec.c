/* Float ABI × libm interop.
 *
 * Unlike the fp_hard_*_exec.c tests, which are self-contained (both caller and
 * callee compiled by tcc), this one calls into libm.  Those are ordinary C
 * functions, not __aeabi_* runtime helpers, so they follow the *program's*
 * float ABI: under -mfloat-abi=hard a float argument travels in s0 and the
 * result comes back in s0, while a soft-float libm would read it from a GPR.
 * That makes this the test that actually proves the libraries we link were
 * built for the ABI we compiled with.
 *
 * Run under each ABI with TCC_FLOAT_ABI=soft|softfp|hard (see qemu_run.py).
 * Returns 1 on success, else a distinct diagnostic code.
 */

#include <math.h>

int main(void)
{
    /* volatile keeps the calls real: no constant folding into the answer. */
    volatile float x = 16.0f;
    volatile float y = 2.5f;
    volatile float neg = -3.5f;

    if ((int)sqrtf(x) != 4)
        return 10;
    if ((int)floorf(y) != 2)
        return 11;
    if ((int)ceilf(y) != 3)
        return 12;
    if ((int)fabsf(neg) != 3)
        return 13;

    /* Result of one libm call feeding the next, so the return register and the
     * argument register have to agree. */
    if ((int)sqrtf(sqrtf(x)) != 2)
        return 14;

    /* Doubles travel in d0-d7 under -mfloat-abi=hard (the ABI is about where
     * arguments live, not about which arithmetic the FPU has — on an SP-only
     * unit the callee unpacks d0 into a GPR pair to call __aeabi_dadd).  So a
     * double libm call has to work too. */
    volatile double d = 81.0;
    if ((int)sqrt(d) != 9)
        return 15;
    if ((int)floor(2.75) != 2)
        return 16;

    return 1;
}
