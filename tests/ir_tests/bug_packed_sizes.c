/*
 * Test packed struct array stride for different non-power-of-2 sizes.
 * Find which sizes trigger wrong stride computation.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* Size 5 (not power of 2) */
typedef struct __attribute__((packed)) S5 {
    int32_t a;
    uint8_t b;
} S5;

/* Size 6 */
typedef struct __attribute__((packed)) S6 {
    int32_t a;
    uint16_t b;
} S6;

/* Size 7 */
typedef struct __attribute__((packed)) S7 {
    int32_t a;
    uint16_t b;
    uint8_t c;
} S7;

/* Size 9 */
typedef struct __attribute__((packed)) S9 {
    int32_t a;
    int32_t b;
    uint8_t c;
} S9;

/* Size 10 */
typedef struct __attribute__((packed)) S10 {
    int32_t a;
    int32_t b;
    uint8_t c;
    uint8_t d;
} S10;

/* Size 11 */
typedef struct __attribute__((packed)) S11 {
    int32_t a;
    int32_t b;
    uint8_t c;
    uint8_t d;
    uint8_t e;
} S11;

/* Size 12 (power-of-2-friendly) */
typedef struct __attribute__((packed)) S12 {
    int32_t a;
    int32_t b;
    int32_t c;
} S12;

/* Size 3 */
typedef struct __attribute__((packed)) S3 {
    uint16_t a;
    uint8_t b;
} S3;

/* Size 14 */
typedef struct __attribute__((packed)) S14 {
    int32_t a;
    int32_t b;
    int32_t c;
    uint16_t d;
} S14;

_Static_assert(sizeof(S5) == 5, "");
_Static_assert(sizeof(S6) == 6, "");
_Static_assert(sizeof(S7) == 7, "");
_Static_assert(sizeof(S9) == 9, "");
_Static_assert(sizeof(S10) == 10, "");
_Static_assert(sizeof(S11) == 11, "");
_Static_assert(sizeof(S12) == 12, "");
_Static_assert(sizeof(S3) == 3, "");
_Static_assert(sizeof(S14) == 14, "");

#define TEST_STRIDE(type, name) do { \
    type pool[2]; \
    uint8_t *raw = (uint8_t*)pool; \
    memset(pool, 0xFF, sizeof(pool)); \
    pool[1].a = 0; \
    int off = -1; \
    for (int b = 0; b < (int)sizeof(pool); b++) { \
        if (raw[b] != 0xFF) { off = b; break; } \
    } \
    int expected = (int)sizeof(type); \
    printf("%-4s (size=%2d): pool[1].a offset=%2d expected=%2d %s\n", \
           name, (int)sizeof(type), off, expected, \
           off == expected ? "OK" : "FAIL"); \
    if (off != expected) errors++; \
} while(0)

int main(void) {
    int errors = 0;

    TEST_STRIDE(S3,  "S3");
    TEST_STRIDE(S5,  "S5");
    TEST_STRIDE(S6,  "S6");
    TEST_STRIDE(S7,  "S7");
    TEST_STRIDE(S9,  "S9");
    TEST_STRIDE(S10, "S10");
    TEST_STRIDE(S11, "S11");
    TEST_STRIDE(S12, "S12");
    TEST_STRIDE(S14, "S14");

    if (errors == 0) printf("ALL PASSED\n");
    else printf("FAILED: %d sizes broken\n", errors);
    return errors;
}
