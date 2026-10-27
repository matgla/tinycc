/* ssa:loop_rotate left the old header CMP (now the zero-trip guard) marked as
 * a jump target after moving the back-edge off it, and the IV analysis reads
 * that mark as a join between `i = 0` and the loop: no IV, no strength
 * reduction.  sum_iv must step `i * 7 + 13` by 7, not multiply each trip. */
int sum_iv(int n)
{
    volatile int sum = 0;
    for (int i = 0; i < n; i++)
        sum += i * 7 + 13;
    return sum;
}
