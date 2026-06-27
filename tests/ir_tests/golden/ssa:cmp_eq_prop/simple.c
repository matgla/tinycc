/* Golden-IR test for ssa:cmp_eq_prop.
 *
 * The first `if (x == y)` branches to `equal:` when the equality holds.  On the
 * fall-through (inequality) edge, ssa:cmp_eq_prop records the fact x != y and
 * folds the second `if (x == y)` to never-taken: the CMP becomes NOP and the
 * JUMPIF becomes an unconditional JUMP to `equal:`.  The surrounding while loop
 * provides promotable locals so the SSA optimizer (and therefore this pass) runs.
 */
int cmp_eq_prop(int a, int b, int n) {
    int i = 0;
    while (i < n) {
        int x = a;
        int y = b;
        if (x == y) goto equal;
        if (x == y) return 1;
equal:
        i++;
    }
    return 0;
}
