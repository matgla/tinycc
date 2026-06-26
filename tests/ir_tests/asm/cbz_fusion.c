/* Phase D lever: cbz/cbnz fusion (cmp #0; b.eq/ne -> cbz/cbnz).
 * The fusion peephole is currently disabled, so these functions emit
 * cmp #0 + beq.w/bne.w.  Characterizes current behavior; once Phase 2b lands,
 * flip the assertions to require cbz/cbnz and forbid cmp #0 branch pairs.
 */
int iszero(int x) {
    if (x == 0) {
        volatile int y = 1;
        return y;
    }
    return x + 2;
}

int isnonzero(int x) {
    if (x != 0) {
        volatile int y = 1;
        return y;
    }
    return x + 3;
}
