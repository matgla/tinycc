#include <stdio.h>
#include <stdint.h>

#define FLOAT_SIGN_BIT (1U << 31)
#define FLOAT_MANT_MASK 0x007FFFFFU

typedef union { float f; uint32_t u; } float_union;

static inline int float_sign(uint32_t bits) { return (bits >> 31) & 1; }
static inline int float_exp(uint32_t bits) { return (bits >> 23) & 0xFF; }
static inline uint32_t float_mant(uint32_t bits) { return bits & FLOAT_MANT_MASK; }
static inline int is_nan_f(uint32_t bits) {
    return (float_exp(bits) == 0xFF) && (float_mant(bits) != 0);
}
static inline int is_zero_f(uint32_t bits) {
    return (float_exp(bits) == 0) && (float_mant(bits) == 0);
}

static int fcmp_core(float a, float b) {
    float_union ua = {.f = a}, ub = {.f = b};
    uint32_t a_bits = ua.u, b_bits = ub.u;
    
    printf("fcmp_core: a=0x%08X, b=0x%08X\n", a_bits, b_bits);
    
    if (is_nan_f(a_bits) || is_nan_f(b_bits)) return 2;
    if (is_zero_f(a_bits) && is_zero_f(b_bits)) return 0;
    
    int a_sign = float_sign(a_bits);
    int b_sign = float_sign(b_bits);
    
    if (a_sign != b_sign) return a_sign ? -1 : 1;
    
    uint32_t a_mag = a_bits & ~FLOAT_SIGN_BIT;
    uint32_t b_mag = b_bits & ~FLOAT_SIGN_BIT;
    
    if (a_mag == b_mag) return 0;
    
    int mag_cmp = (a_mag > b_mag) ? 1 : -1;
    return a_sign ? -mag_cmp : mag_cmp;
}

int __aeabi_fcmpeq(float a, float b) {
    return fcmp_core(a, b) == 0 ? 1 : 0;
}

int main() {
    float a = 1.5f;
    float b = 1.5f;
    
    int eq = __aeabi_fcmpeq(a, b);
    printf("__aeabi_fcmpeq(1.5, 1.5) = %d (expected 1)\n", eq);
    
    return eq == 1 ? 0 : 1;
}
