#include <stdio.h>
#include <stdint.h>

#define FLOAT_IMPLICIT_BIT (1U << 23)
#define FLOAT_MANT_MASK 0x007FFFFFU
#define FLOAT_EXP_BIAS 127

typedef union { float f; uint32_t u; } float_union;

static inline int float_sign(uint32_t bits) { return (bits >> 31) & 1; }
static inline int float_exp(uint32_t bits) { return (bits >> 23) & 0xFF; }
static inline uint32_t float_mant(uint32_t bits) { return bits & FLOAT_MANT_MASK; }
static inline uint32_t make_float(int sign, int exp, uint32_t mant) {
    return ((uint32_t)sign << 31) | ((uint32_t)exp << 23) | (mant & FLOAT_MANT_MASK);
}

float __aeabi_fmul(float a, float b) {
    float_union ua = {.f = a}, ub = {.f = b}, ur;
    uint32_t a_bits = ua.u, b_bits = ub.u;
    
    int a_sign = float_sign(a_bits);
    int b_sign = float_sign(b_bits);
    int a_exp = float_exp(a_bits);
    int b_exp = float_exp(b_bits);
    uint32_t a_mant = float_mant(a_bits);
    uint32_t b_mant = float_mant(b_bits);
    
    int result_sign = a_sign ^ b_sign;
    
    /* Add implicit bit */
    if (a_exp != 0) a_mant |= FLOAT_IMPLICIT_BIT;
    if (b_exp != 0) b_mant |= FLOAT_IMPLICIT_BIT;
    
    printf("a_sign=%d, a_exp=%d, a_mant=0x%x\n", a_sign, a_exp, a_mant);
    printf("b_sign=%d, b_exp=%d, b_mant=0x%x\n", b_sign, b_exp, b_mant);
    
    /* Calculate result exponent */
    int result_exp = a_exp + b_exp - FLOAT_EXP_BIAS;
    printf("result_exp (before) = %d\n", result_exp);
    
    /* Multiply mantissas (24-bit * 24-bit = 48-bit) */
    uint64_t product = (uint64_t)a_mant * (uint64_t)b_mant;
    printf("product = 0x%llx\n", (unsigned long long)product);
    
    /* Normalize: product is in bits 46-0, implicit bit at 46 or 47 */
    if (product & (1ULL << 47)) {
        printf("Normalizing: product has bit 47 set\n");
        product >>= 1;
        result_exp++;
    }
    printf("result_exp (after norm) = %d\n", result_exp);
    
    /* Shift to get 23-bit mantissa */
    uint32_t result_mant = (uint32_t)(product >> 23);
    printf("result_mant (raw) = 0x%x\n", result_mant);
    
    result_mant &= FLOAT_MANT_MASK;
    printf("result_mant (masked) = 0x%x\n", result_mant);
    
    ur.u = make_float(result_sign, result_exp, result_mant);
    return ur.f;
}

int main() {
    float a = 1.5f;
    float b = 1.0f;
    float r = __aeabi_fmul(a, b);
    
    printf("\n%f * %f = %f (expected 1.5)\n", a, b, r);
    
    // Also check bit patterns
    float_union u;
    u.f = r;
    printf("Result bits: 0x%08X\n", u.u);
    u.f = 1.5f;
    printf("Expected:    0x%08X\n", u.u);
    
    return (r == 1.5f) ? 0 : 1;
}
