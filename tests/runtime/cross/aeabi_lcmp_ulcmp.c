/* Force references to the ARM EABI 64-bit comparison helpers. */
extern int __aeabi_lcmp(unsigned int a_lo, int a_hi, unsigned int b_lo, int b_hi);
extern int __aeabi_ulcmp(unsigned int a_lo, unsigned int a_hi,
                         unsigned int b_lo, unsigned int b_hi);

volatile long long x, y;

int force_aeabi_cmp(void) {
    return __aeabi_lcmp((unsigned int)x, (int)(x >> 32),
                        (unsigned int)y, (int)(y >> 32))
         + __aeabi_ulcmp((unsigned int)x, (unsigned int)(x >> 32),
                         (unsigned int)y, (unsigned int)(y >> 32));
}
