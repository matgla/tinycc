#include "utest.h"
#include <stdint.h>
#include <limits.h>

extern int __aeabi_f2iz(float a);

// Basic positive values
UTEST(f2iz, zero) {
    ASSERT_EQ(__aeabi_f2iz(0.0f), 0);
    ASSERT_EQ(__aeabi_f2iz(-0.0f), 0);
}

UTEST(f2iz, fractional_truncation) {
    ASSERT_EQ(__aeabi_f2iz(0.1f), 0);
    ASSERT_EQ(__aeabi_f2iz(0.5f), 0);
    ASSERT_EQ(__aeabi_f2iz(0.9f), 0);
    ASSERT_EQ(__aeabi_f2iz(0.99f), 0);
    ASSERT_EQ(__aeabi_f2iz(0.999f), 0);
}

UTEST(f2iz, positive_integers) {
    ASSERT_EQ(__aeabi_f2iz(1.0f), 1);
    ASSERT_EQ(__aeabi_f2iz(2.0f), 2);
    ASSERT_EQ(__aeabi_f2iz(10.0f), 10);
    ASSERT_EQ(__aeabi_f2iz(100.0f), 100);
    ASSERT_EQ(__aeabi_f2iz(1000.0f), 1000);
    ASSERT_EQ(__aeabi_f2iz(10000.0f), 10000);
}

UTEST(f2iz, positive_with_fraction) {
    ASSERT_EQ(__aeabi_f2iz(1.1f), 1);
    ASSERT_EQ(__aeabi_f2iz(1.5f), 1);
    ASSERT_EQ(__aeabi_f2iz(1.9f), 1);
    ASSERT_EQ(__aeabi_f2iz(3.14f), 3);
    ASSERT_EQ(__aeabi_f2iz(42.7f), 42);
    ASSERT_EQ(__aeabi_f2iz(99.999f), 99);
}

UTEST(f2iz, negative_fractional) {
    ASSERT_EQ(__aeabi_f2iz(-0.1f), 0);
    ASSERT_EQ(__aeabi_f2iz(-0.5f), 0);
    ASSERT_EQ(__aeabi_f2iz(-0.9f), 0);
    ASSERT_EQ(__aeabi_f2iz(-0.99f), 0);
}

UTEST(f2iz, negative_integers) {
    ASSERT_EQ(__aeabi_f2iz(-1.0f), -1);
    ASSERT_EQ(__aeabi_f2iz(-2.0f), -2);
    ASSERT_EQ(__aeabi_f2iz(-10.0f), -10);
    ASSERT_EQ(__aeabi_f2iz(-100.0f), -100);
    ASSERT_EQ(__aeabi_f2iz(-1000.0f), -1000);
}

UTEST(f2iz, negative_with_fraction) {
    ASSERT_EQ(__aeabi_f2iz(-1.1f), -1);
    ASSERT_EQ(__aeabi_f2iz(-1.5f), -1);
    ASSERT_EQ(__aeabi_f2iz(-1.9f), -1);
    ASSERT_EQ(__aeabi_f2iz(-2.71f), -2);
    ASSERT_EQ(__aeabi_f2iz(-42.7f), -42);
}

UTEST(f2iz, large_positive_values) {
    ASSERT_EQ(__aeabi_f2iz(1000000.0f), 1000000);
    ASSERT_EQ(__aeabi_f2iz(16777216.0f), 16777216);  // 2^24
    ASSERT_EQ(__aeabi_f2iz(33554432.0f), 33554432);  // 2^25
}

UTEST(f2iz, large_negative_values) {
    ASSERT_EQ(__aeabi_f2iz(-1000000.0f), -1000000);
    ASSERT_EQ(__aeabi_f2iz(-16777216.0f), -16777216);  // -2^24
    ASSERT_EQ(__aeabi_f2iz(-33554432.0f), -33554432);  // -2^25
}

UTEST(f2iz, overflow_positive) {
    // Values that exceed INT_MAX should return INT_MAX
    ASSERT_EQ(__aeabi_f2iz(2147483648.0f), INT_MAX);      // 2^31
    ASSERT_EQ(__aeabi_f2iz(3000000000.0f), INT_MAX);
    ASSERT_EQ(__aeabi_f2iz(1e20f), INT_MAX);
}

UTEST(f2iz, overflow_negative) {
    // Values that exceed INT_MIN should return INT_MIN
    ASSERT_EQ(__aeabi_f2iz(-2147483649.0f), INT_MIN);
    ASSERT_EQ(__aeabi_f2iz(-3000000000.0f), INT_MIN);
    ASSERT_EQ(__aeabi_f2iz(-1e20f), INT_MIN);
}

UTEST(f2iz, powers_of_two) {
    ASSERT_EQ(__aeabi_f2iz(2.0f), 2);
    ASSERT_EQ(__aeabi_f2iz(4.0f), 4);
    ASSERT_EQ(__aeabi_f2iz(8.0f), 8);
    ASSERT_EQ(__aeabi_f2iz(16.0f), 16);
    ASSERT_EQ(__aeabi_f2iz(32.0f), 32);
    ASSERT_EQ(__aeabi_f2iz(64.0f), 64);
    ASSERT_EQ(__aeabi_f2iz(128.0f), 128);
    ASSERT_EQ(__aeabi_f2iz(256.0f), 256);
    ASSERT_EQ(__aeabi_f2iz(512.0f), 512);
    ASSERT_EQ(__aeabi_f2iz(1024.0f), 1024);
}

UTEST(f2iz, negative_powers_of_two) {
    ASSERT_EQ(__aeabi_f2iz(-2.0f), -2);
    ASSERT_EQ(__aeabi_f2iz(-4.0f), -4);
    ASSERT_EQ(__aeabi_f2iz(-8.0f), -8);
    ASSERT_EQ(__aeabi_f2iz(-16.0f), -16);
    ASSERT_EQ(__aeabi_f2iz(-32.0f), -32);
    ASSERT_EQ(__aeabi_f2iz(-64.0f), -64);
    ASSERT_EQ(__aeabi_f2iz(-128.0f), -128);
    ASSERT_EQ(__aeabi_f2iz(-256.0f), -256);
}

UTEST(f2iz, near_int_max) {
    // Test values near INT_MAX boundary
    ASSERT_EQ(__aeabi_f2iz(2147483520.0f), 2147483520);  // Just below 2^31
    ASSERT_EQ(__aeabi_f2iz(2147483647.0f), INT_MAX);     // At 2^31-1
}

UTEST(f2iz, near_int_min) {
    // Test values near INT_MIN boundary
    ASSERT_EQ(__aeabi_f2iz(-2147483648.0f), INT_MIN);    // At -2^31
}
