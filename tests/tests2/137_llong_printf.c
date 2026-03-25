/*
 * Test printf formatting of long long / unsigned long long values.
 *
 * Isolates whether %lld/%llu work correctly when the long long argument
 * lands in registers vs. on the stack (i.e. after several preceding args).
 *
 * On 32-bit ARM, the first few args go in r0-r3; additional args go on
 * the stack.  A long long consumes a register pair (even-aligned).
 * If printf's va_arg handling or argument placement swaps the 32-bit
 * halves, the decimal output will be wrong while hex extraction (done
 * with explicit shifts/masks) would still look correct.
 */
#include <stdio.h>

static void print_ll(const char *label, long long val);
static void print_ull(const char *label, unsigned long long val);

int main(void)
{
    long long a = 1234567890123LL;
    long long b = -987654321LL;
    unsigned long long u = 1ULL;

    /* --- Case 1: long long is the only vararg (fits in registers) --- */
    printf("a=%lld\n", a);
    printf("b=%lld\n", b);
    printf("u=%llu\n", u);

    /* --- Case 2: long long after one small arg (still in registers) --- */
    printf("a2=%d,%lld\n", 42, a);
    printf("b2=%d,%lld\n", 42, b);

    /* --- Case 3: long long after enough args to push it onto the stack --- */
    printf("a3=%d,%d,%lld\n", 1, 2, a);
    printf("b3=%d,%d,%lld\n", 1, 2, b);
    printf("u3=%d,%d,%llu\n", 1, 2, u);

    /* --- Case 4: long long after three int args (definitely on stack) --- */
    printf("a4=%d,%d,%d,%lld\n", 1, 2, 3, a);
    printf("b4=%d,%d,%d,%lld\n", 1, 2, 3, b);

    /* --- Case 5: multiple long longs in one call --- */
    printf("ab=%lld,%lld\n", a, b);

    /* --- Case 6: unsigned long long edge values --- */
    printf("u0=%llu\n", 0ULL);
    printf("u1=%llu\n", 1ULL);
    printf("umax=%llu\n", 0xFFFFFFFFFFFFFFFFULL);
    printf("s32=%llu\n", (unsigned long long)1 << 32);
    printf("s63=%llu\n", (unsigned long long)1 << 63);

    /* --- Case 7: hex formatting of long long --- */
    printf("xmax=%llx\n", 0xFFFFFFFFFFFFFFFFULL);
    printf("xa=%llx\n", (unsigned long long)a);

    /* --- Case 8: addition result through printf --- */
    printf("sum=%lld\n", a + b);

    /* --- Case 9: pass-through — long long param forwarded to printf on stack.
     * This is the pattern from 136_llong_diag.c's print_ll() that triggers
     * a word-swap on 32-bit ARM when the long long lands on the stack
     * after several preceding arguments. --- */
    print_ll("fwd_a", a);
    print_ll("fwd_b", b);
    print_ull("fwd_u32", (unsigned long long)1 << 32);

    return 0;
}

/* Helper: receive long long as parameter, then forward it to printf as a
 * stack argument (preceded by enough args to fill r0-r3). */
static void print_ll(const char *label, long long val)
{
    unsigned int lo = (unsigned int)(val & 0xFFFFFFFFU);
    unsigned int hi = (unsigned int)((unsigned long long)val >> 32);
    printf("%s=0x%08x_%08x=%lld\n", label, hi, lo, val);
}

static void print_ull(const char *label, unsigned long long val)
{
    unsigned int lo = (unsigned int)(val & 0xFFFFFFFFU);
    unsigned int hi = (unsigned int)(val >> 32);
    printf("%s=0x%08x_%08x=%llu\n", label, hi, lo, val);
}
