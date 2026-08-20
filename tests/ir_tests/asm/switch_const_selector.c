/* ssa:switch_fold: a constant selector must pick its arm at compile time; a
 * runtime selector must still dispatch. */
int const_seven(void)
{
    int i = 7;
    int r = 1000;
    switch (i) {
    case 0: r += 1; break;
    case 1: r -= 1; break;
    case 2: r *= 2; break;
    case 3: r /= 2; break;
    case 4: r ^= 4; break;
    case 5: r &= 5; break;
    case 6: r |= 6; break;
    case 7: r = (r ^ 0xff) ^ 0xff; break;
    default: r = -1; break;
    }
    return r;
}

int const_out_of_range(void)
{
    int i = 42;
    int r = 1000;
    switch (i) {
    case 0: r += 1; break;
    case 1: r -= 1; break;
    case 2: r *= 2; break;
    case 3: r /= 2; break;
    case 4: r ^= 4; break;
    case 5: r &= 5; break;
    case 6: r |= 6; break;
    case 7: r = (r ^ 0xff) ^ 0xff; break;
    default: r = -1; break;
    }
    return r;
}

/* The bench_switch shape: fold the dispatch, then the loop around it goes. */
int const_switch_in_loop(int iterations)
{
    int r = 0;
    for (int n = 0; n < iterations; n++) {
        int i = 7;
        r = 1000;
        switch (i) {
        case 0: r += i + 1; break;
        case 1: r -= i; break;
        case 2: r += 1; break;
        case 3: r = r / 2 + 1; break;
        case 4: r ^= i; break;
        case 5: r &= (0xffff + i); break;
        case 6: r |= (i & 0x0f); break;
        case 7: r = (r ^ 0xff) ^ 0xff; break;
        default: r = 0; break;
        }
    }
    return r;
}

int variable_selector(int i)
{
    switch (i) {
    case 0: return 70;
    case 1: return 71;
    case 2: return 72;
    case 3: return 73;
    case 4: return 74;
    default: return -70;
    }
}
