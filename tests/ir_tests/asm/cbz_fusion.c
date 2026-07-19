/* Phase D lever: cbz/cbnz fusion (cmp #0; b.eq/ne -> cbz/cbnz).
 * The fusion is enabled: the rehearsal codegen pass lays the function out the
 * way the real pass will, so the forward distance can be bounded from both
 * sides and a committed 16-bit CBZ can never fall out of its 0..126 range.
 * Both functions now emit a single cbz/cbnz with no cmp and no branch.
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
