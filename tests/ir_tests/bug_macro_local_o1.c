/*
 * Bug: local variable 'expected' prints garbage at -O1 when used
 * in a do-while(0) macro that is expanded multiple times.
 * Reduced from bug_packed_sizes.c
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef struct __attribute__((packed)) S3 {
    uint16_t a;
    uint8_t b;
} S3;

typedef struct __attribute__((packed)) S5 {
    int32_t a;
    uint8_t b;
} S5;

typedef struct __attribute__((packed)) S10 {
    int32_t a;
    int32_t b;
    uint8_t c;
    uint8_t d;
} S10;

_Static_assert(sizeof(S3) == 3, "");
_Static_assert(sizeof(S5) == 5, "");
_Static_assert(sizeof(S10) == 10, "");

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
    TEST_STRIDE(S10, "S10");

    if (errors == 0) printf("ALL PASSED\n");
    else printf("FAILED: %d errors\n", errors);
    return errors;
}
