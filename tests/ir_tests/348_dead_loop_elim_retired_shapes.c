/* Anti-regression pin for the retirement of the legacy tcc_ir_opt_dead_loop_elim
 * (ir/opt_dce.c, retired 2026-07-07 — proven inert; dead-loop collapse is owned
 * by ssa:dead_loop, ir/opt/ssa_opt_dead_loop.c).  See
 * docs/plan_legacy_loop_dead_loop_elim_ssa.md.
 *
 * These are exactly the "distinctive legacy domains" the Step 0 experiment
 * probed — memory-resident (address-taken) locals and self-stores.  They must
 * produce the correct value at every -O level whether or not any loop survives:
 *  - memvar_rt: runtime-bound write of a constant to an address-taken local.
 *    The n==0 case pins the correctness the legacy pass's UNSOUND unconditional
 *    preheader hoist would have violated (it must return the init 0, not 5).
 *  - selfstore: `a = a` struct self-copy in a counted loop — an observable
 *    no-op DSE now removes.
 *  - memvar_const: compile-time-bound variant (loop fully unrolls). */
#include <stdio.h>

static int memvar_rt(int n)
{
    int a = 0;
    int *p = &a;
    for (int i = 0; i < n; i++)
        a = 5;
    return *p;
}

struct S { int x; int y; };
static int selfstore(int n)
{
    struct S a = {1, 2};
    for (int i = 0; i < n; i++)
        a = a;
    return a.x + a.y;
}

static int memvar_const(void)
{
    int a = 0;
    int *p = &a;
    for (int i = 0; i < 10; i++)
        a = 5;
    return *p;
}

int main(void)
{
    printf("%d %d %d %d %d\n",
           memvar_rt(0), memvar_rt(5),
           selfstore(0), selfstore(3),
           memvar_const());
    return 0;
}
