#include "utest.h"
#include <stdint.h>

// Declare the functions we're testing
extern int __aeabi_d2iz(double a);
extern unsigned int __aeabi_d2uiz(double a);

UTEST(d2iz, positive_values) {
    ASSERT_EQ(__aeabi_d2iz(0.0), 0);
    ASSERT_EQ(__aeabi_d2iz(0.5), 0);
    ASSERT_EQ(__aeabi_d2iz(0.99), 0);
    ASSERT_EQ(__aeabi_d2iz(1.0), 1);
    ASSERT_EQ(__aeabi_d2iz(1.5), 1);
    ASSERT_EQ(__aeabi_d2iz(42.7), 42);
    ASSERT_EQ(__aeabi_d2iz(100.0), 100);
}

UTEST(d2iz, negative_values) {
    ASSERT_EQ(__aeabi_d2iz(-0.5), 0);
    ASSERT_EQ(__aeabi_d2iz(-0.99), 0);
    ASSERT_EQ(__aeabi_d2iz(-1.0), -1);
    ASSERT_EQ(__aeabi_d2iz(-1.5), -1);
    ASSERT_EQ(__aeabi_d2iz(-42.7), -42);
    ASSERT_EQ(__aeabi_d2iz(-100.0), -100);
}

UTEST(d2iz, overflow) {
    ASSERT_EQ(__aeabi_d2iz(2147483648.0), 0x7FFFFFFF);  // INT_MAX
    ASSERT_EQ(__aeabi_d2iz(-2147483649.0), (int)0x80000000);  // INT_MIN
}

UTEST(d2uiz, positive_values) {
    ASSERT_EQ(__aeabi_d2uiz(0.0), 0u);
    ASSERT_EQ(__aeabi_d2uiz(0.5), 0u);
    ASSERT_EQ(__aeabi_d2uiz(0.99), 0u);
    ASSERT_EQ(__aeabi_d2uiz(1.0), 1u);
    ASSERT_EQ(__aeabi_d2uiz(1.5), 1u);
    ASSERT_EQ(__aeabi_d2uiz(42.7), 42u);
    ASSERT_EQ(__aeabi_d2uiz(100.0), 100u);
}

UTEST(d2uiz, negative_values) {
    ASSERT_EQ(__aeabi_d2uiz(-0.5), 0u);
    ASSERT_EQ(__aeabi_d2uiz(-1.0), 0u);
    ASSERT_EQ(__aeabi_d2uiz(-42.7), 0u);
    ASSERT_EQ(__aeabi_d2uiz(-100.0), 0u);
}

UTEST(d2uiz, overflow) {
    ASSERT_EQ(__aeabi_d2uiz(4294967296.0), 0xFFFFFFFFu);  // UINT_MAX
}
