/*
 * Isolate: does removing the union or removing bitfields fix the stride bug?
 * Test variants of the 10-byte struct.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* Variant A: packed struct WITH union+bitfield (the failing case) */
typedef struct __attribute__((packed)) VarA {
    union {
        int32_t vr;
        struct {
            uint32_t lo : 18;
            uint32_t tag : 3;
            uint32_t hi : 11;
        };
    };
    int32_t payload;
    uint8_t x;
    uint8_t y;
} VarA;

/* Variant B: packed struct WITHOUT union, just int32 + bitfield separately */
typedef struct __attribute__((packed)) VarB {
    int32_t vr;
    int32_t payload;
    uint8_t x;
    uint8_t y;
} VarB;

/* Variant C: packed struct with union but NO bitfields */
typedef struct __attribute__((packed)) VarC {
    union {
        int32_t vr;
        uint32_t vr_unsigned;
    };
    int32_t payload;
    uint8_t x;
    uint8_t y;
} VarC;

/* Variant D: NON-packed struct with union+bitfield (should be 12 bytes?) */
typedef struct VarD {
    union {
        int32_t vr;
        struct {
            uint32_t lo : 18;
            uint32_t tag : 3;
            uint32_t hi : 11;
        };
    };
    int32_t payload;
    uint8_t x;
    uint8_t y;
} VarD;

_Static_assert(sizeof(VarA) == 10, "VarA must be 10 bytes");
_Static_assert(sizeof(VarB) == 10, "VarB must be 10 bytes");
_Static_assert(sizeof(VarC) == 10, "VarC must be 10 bytes");

static int test_varA(void) {
    VarA pool[2];
    uint8_t *raw = (uint8_t*)pool;
    memset(pool, 0, sizeof(pool));
    pool[0].vr = 0; pool[0].tag = 1; pool[0].payload = 100;
    pool[1].vr = 0; pool[1].tag = 7; pool[1].payload = 200;
    /* Check if pool[1].vr starts at byte 10 */
    int off = -1;
    memset(pool, 0xff, sizeof(pool));
    pool[1].vr = 0;
    for (int b = 0; b < 20; b++) { if (raw[b] != 0xFF) { off = b; break; } }
    printf("VarA (union+bitfield): pool[1].vr offset=%d (expected 10) %s\n",
           off, off == 10 ? "OK" : "FAIL");
    return off != 10;
}

static int test_varB(void) {
    VarB pool[2];
    uint8_t *raw = (uint8_t*)pool;
    memset(pool, 0xff, sizeof(pool));
    pool[1].vr = 0;
    int off = -1;
    for (int b = 0; b < 20; b++) { if (raw[b] != 0xFF) { off = b; break; } }
    printf("VarB (no union):       pool[1].vr offset=%d (expected 10) %s\n",
           off, off == 10 ? "OK" : "FAIL");
    return off != 10;
}

static int test_varC(void) {
    VarC pool[2];
    uint8_t *raw = (uint8_t*)pool;
    memset(pool, 0xff, sizeof(pool));
    pool[1].vr = 0;
    int off = -1;
    for (int b = 0; b < 20; b++) { if (raw[b] != 0xFF) { off = b; break; } }
    printf("VarC (union, no bf):   pool[1].vr offset=%d (expected 10) %s\n",
           off, off == 10 ? "OK" : "FAIL");
    return off != 10;
}

static int test_varD(void) {
    VarD pool[2];
    uint8_t *raw = (uint8_t*)pool;
    int sz = (int)sizeof(VarD);
    printf("VarD sizeof=%d\n", sz);
    memset(pool, 0xff, sizeof(pool));
    pool[1].vr = 0;
    int off = -1;
    for (int b = 0; b < (int)sizeof(pool); b++) { if (raw[b] != 0xFF) { off = b; break; } }
    printf("VarD (non-packed):     pool[1].vr offset=%d (expected %d) %s\n",
           off, sz, off == sz ? "OK" : "FAIL");
    return off != sz;
}

int main(void) {
    int err = 0;
    err += test_varA();
    err += test_varB();
    err += test_varC();
    err += test_varD();
    if (err == 0) printf("ALL PASSED\n");
    else printf("FAILED: %d variants broken\n", err);
    return err;
}
