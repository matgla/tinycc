/* ssa:dead_loop, rotated (bottom-test) arm.  const_result's loop must vanish;
 * every other function trips one of the pass's conditions and must keep its
 * back-edge. */
int sink;
volatile int vsink;

int const_result(int n)
{
    int r = 7;
    for (int i = 0; i < n; i++)
        r = 91967;
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
