/* Phase D lever: forward conditional branch narrowing (b.w -> b.n).
 * The loop contains a forward conditional branch that currently stays wide
 * (bge.w) and a backward conditional branch that is already narrow (blt.n).
 * Characterizes current codegen; once forward relaxation lands, flip the
 * assertion to expect bge.n / zero wide conditional branches.
 */
int cond(int x) {
    if (x == 0)
        return 1;
    return 0;
}

int loop(int n) {
    int s = 0;
    for (int i = 0; i < n; i++)
        s += i;
    return s;
}
