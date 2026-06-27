/* Exercise ssa:phi_simplify: both arms of the if assign the same value,
 * so the merge phi is trivial (all non-self operands are the same vreg).
 */
int phi_simplify_same_operand(int x, int y) {
    int r;
    if (x == 0)
        r = y;
    else
        r = y;
    return r;
}
