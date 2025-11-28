#include <stdint.h>
#include <stdlib.h>

// stubs for now, correct implementations to be added later with hw and sw support

double __attribute__((weak)) __aeabi_dadd(double a, double b) {
    return 0;
}

double __attribute__((weak)) __aeabi_dsub(double a, double b) {
    return 0;
}

double __attribute__((weak)) __aeabi_dmul(double a, double b) {
    return 0;
}

double __attribute__((weak)) __aeabi_ddiv(double a, double b) {
    return 0;
}

// Helper function for double to integer conversion
static inline uint64_t double_to_int_helper(double a, int *sign_out, int *exponent_out) {
    // IEEE 754 double format: sign(1) | exponent(11) | mantissa(52)
    union {
        double d;
        uint64_t u;
    } conv;
    conv.d = a;

    uint64_t bits = conv.u;
    *sign_out = (bits >> 63);
    *exponent_out = ((bits >> 52) & 0x7FF) - 1023;  // Remove bias
    return (bits & 0xFFFFFFFFFFFFFULL) | 0x10000000000000ULL; // Add implicit 1
}

static inline uint32_t shift_mantissa_d2i(uint64_t mantissa, int exponent) {
    if (exponent < 52) {
        // Right shift to remove fractional bits
        return (uint32_t)(mantissa >> (52 - exponent));
    } else {
        // Left shift for large values
        return (uint32_t)(mantissa << (exponent - 52));
    }
}

int __attribute__((weak)) __aeabi_d2iz(double a) {
    int sign, exponent;
    uint64_t mantissa = double_to_int_helper(a, &sign, &exponent);

    // Handle special cases
    if (exponent < 0) {
        // |a| < 1.0, truncate to 0
        return 0;
    }

    if (exponent >= 31) {
        // Overflow: return INT_MAX or INT_MIN
        return sign ? 0x80000000 : 0x7FFFFFFF;
    }

    // Shift mantissa to get integer part
    uint32_t result = shift_mantissa_d2i(mantissa, exponent);

    return sign ? -(int32_t)result : (int32_t)result;
}

unsigned int __attribute__((weak)) __aeabi_d2uiz(double a) {
    int sign, exponent;
    uint64_t mantissa = double_to_int_helper(a, &sign, &exponent);

    // Handle special cases
    if (sign) {
        // Negative numbers convert to 0 for unsigned
        return 0;
    }

    if (exponent < 0) {
        // |a| < 1.0, truncate to 0
        return 0;
    }

    if (exponent >= 32) {
        // Overflow: return UINT_MAX
        return 0xFFFFFFFF;
    }

    // Shift mantissa to get integer part
    return shift_mantissa_d2i(mantissa, exponent);
}

int __attribute__((weak)) __aeabi_f2iz(float a)  {
    // IEEE 754 float format: sign(1) | exponent(8) | mantissa(23)
    union {
        float f;
        uint32_t u;
    } conv;
    conv.f = a;

    uint32_t bits = conv.u;
    int32_t sign = (bits >> 31) ? -1 : 1;
    int32_t exponent = ((bits >> 23) & 0xFF) - 127;  // Remove bias
    uint32_t mantissa = (bits & 0x7FFFFF) | 0x800000; // Add implicit 1

    // Handle special cases
    if (exponent < 0) {
        // |a| < 1.0, truncate to 0
        return 0;
    }

    if (exponent >= 31) {
        // Overflow: return INT_MAX or INT_MIN
        return sign > 0 ? 0x7FFFFFFF : 0x80000000;
    }

    // Shift mantissa to get integer part
    int32_t result;
    if (exponent < 23) {
        // Right shift to remove fractional bits
        result = mantissa >> (23 - exponent);
    } else {
        // Left shift for large values
        result = mantissa << (exponent - 23);
    }

    return sign * result;
}

