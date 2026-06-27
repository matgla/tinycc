/* Force references to the ARM EABI 64-bit shift helpers. */
extern unsigned long long __aeabi_llsr(unsigned long long a, int b);
extern long long __aeabi_llsl(long long a, int b);
extern long long __aeabi_lasr(long long a, int b);

volatile unsigned long long u;
volatile long long s;
volatile int n;

int force_aeabi_shifts(void) {
    u = __aeabi_llsr(u, n);
    s = __aeabi_llsl(s, n);
    s = __aeabi_lasr(s, n);
    return (int)(u + (unsigned long long)s);
}
