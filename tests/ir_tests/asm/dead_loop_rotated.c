/* ssa:dead_loop, rotated (bottom-test) arm.  const_result's and
 * outlined_arm's loops must vanish; every other function trips one of the
 * pass's conditions and must keep its back-edge. */
int sink;
volatile int vsink;

int const_result(int n)
{
    int r = 7;
    for (int i = 0; i < n; i++)
        r = 91967;
    return r;
}

/* bench_control.c's bench_conditionals: loop_header_dup lays the folded
 * else-if arm out past the latch, so the body is not one run of blocks. */
int outlined_arm(int n)
{
    int r = 0;
    for (int k = 0; k < n; k++) {
        int i = 42;
        r = 1234;
        if (i & 1)
            r += i * 3;
        else if (i % 3 == 0)
            r -= i;
        else
            r ^= i;
        if (r > 1000000)
            r = r >> 3;
        else if (r < -1000000)
            r = -r;
    }
    return r;
}

/* No closed form, so the loop const-sim pass leaves it alone: the exit value
 * genuinely depends on the trip count. */
int mangle(int n)
{
    int r = 1;
    for (int i = 0; i < n; i++)
        r = r * 3 + 1;
    return r;
}

int counter_escapes(int n)
{
    int i;
    for (i = 0; i < n; i++)
        ;
    return i;
}

int store_each(int n)
{
    for (int i = 0; i < n; i++)
        sink = i;
    return sink;
}

int volatile_poll(int n)
{
    int r = 0;
    for (int i = 0; i < n; i++)
        r = vsink;
    return r;
}
