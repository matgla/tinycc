/* Common-base branch of the CMP constant-offset fold: with a shared base X,
 * A = X + K1 and B = X + K2 reduce `A cond B` to `K1 cond K2` under the
 * signed-overflow-is-UB rule.  Only the JUMPIF (`if`) and SELECT (`?:`)
 * consumers are folded (a bare boolean `return a < b` lowers to SETIF, which
 * this pass deliberately leaves alone).  Each function must collapse to a
 * constant return with no `cmp`. */

int lt_if(int x)  { int a = x + 1, b = x + 3; if (a <  b) return 111; return 222; } /*  1 <  3 -> 111 */
int gt_if(int x)  { int a = x + 1, b = x + 3; if (a >  b) return 111; return 222; } /*  1 >  3 -> 222 */
int le_sel(int x) { int a = x + 3, b = x + 1; return a <= b ? 111 : 222; }          /*  3 <= 1 -> 222 */
int ne_sel(int x) { int a = x + 2, b = x + 9; return a != b ? 111 : 222; }          /*  2 != 9 -> 111 */
int sub_if(int x) { int a = x - 1, b = x - 4; if (a >  b) return 111; return 222; } /* -1 > -4 -> 111 */
