/* Exercise ssa:dce: a dead local assignment is removed. */
int simple_dce(int x) {
    int a = x + 1;  /* dead: result never used */
    return x;
}
