/*
 * Minimal: just one packed struct access, measure the stride.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef struct __attribute__((packed)) S10 {
    int32_t a;
    int32_t b;
    uint8_t c;
    uint8_t d;
} S10;

_Static_assert(sizeof(S10) == 10, "");

__attribute__((noinline))
int read_a(S10 *pool, int idx) {
    return pool[idx].a;
}

int main(void) {
    S10 pool[4];
    memset(pool, 0, sizeof(pool));
    pool[0].a = 100;
    pool[1].a = 200;
    pool[2].a = 300;
    pool[3].a = 400;

    for (int i = 0; i < 4; i++) {
        printf("read_a(pool, %d) = %d (expected %d)\n",
               i, read_a(pool, i), (i + 1) * 100);
    }
    return 0;
}
