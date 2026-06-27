/* Force references to the 32-bit ARM EABI division helpers. */
extern int __aeabi_idiv(int numerator, int denominator);
extern unsigned int __aeabi_uidiv(unsigned int numerator, unsigned int denominator);

volatile int ai, bi, ci;
volatile unsigned au, bu, cu;

int force_aeabi_idiv_uidiv(void) {
    ci = __aeabi_idiv(ai, bi);
    cu = __aeabi_uidiv(au, bu);
    return ci + (int)cu;
}
