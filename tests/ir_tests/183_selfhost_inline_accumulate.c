/* Self-host regression: a separate function whose loop accumulates the same
 * element twice, inlined into main and printed directly.  At -O1 the inlined
 * loop has two distinct loop-carried phis (induction var i, accumulator s)
 * plus DEREF-fused operands.
 *
 * Two self-host miscompiles were exposed by this exact shape (the gcc-built
 * host cross is always correct; only the self-hosted device tcc miscompiled):
 *   1. try_unroll_loop_ex zero-init of a 9-byte-stride struct array emitted an
 *      unaligned STRD -> UNALIGNED UsageFault (FIXED: ir/codegen.c byte-store
 *      coalescing no longer pairs INT8-origin stores into STRD off a reg base).
 *   2. ra_coalesce_graph gives the induction var and the accumulator the SAME
 *      register though both are live across the loop -> prints 12 not 72
 *      (OPEN at the time of writing; xfailed at -O1 in the on-device smoke
 *      suite, see tests/smoke/tcc_suite_test.py IR_TESTS_XFAIL).
 *
 * NOTE: the coalescer miscompile is shape-fragile — adding an intermediate
 * `int r = sum_loop(...); if (r != 72) ...` perturbs register allocation and
 * hides it.  Keep the direct `printf("%d\n", sum_loop(...))` form; the .expect
 * file does the value check.  The host-cross IR harness compiles with the
 * (correct) cross so it passes at every level; the on-device smoke harness
 * compiles with the device tcc, which is where the miscompiles bite.
 */
#include <stdio.h>

int arr[8];

int sum_loop(int *p, int n)
{
    int s = 0;
    for (int i = 0; i < n; i++) {
        s += p[i];
        s += p[i];
    }
    return s;
}

int main(void)
{
    for (int i = 0; i < 8; i++)
        arr[i] = i + 1;
    printf("%d\n", sum_loop(arr, 8));
    return 0;
}
