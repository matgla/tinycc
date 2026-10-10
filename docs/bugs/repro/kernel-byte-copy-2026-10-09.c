/* The byte fallback used by libs/libc/string.c:memcpy.
 * Inspect -O2 ARM code: the current cross retains a scalar byte loop.
 * This is a performance reproducer, not a correctness regression test. */
#include <stddef.h>

__attribute__((noinline))
void byte_copy(unsigned char *dst, const unsigned char *src, size_t n) {
    for (; n; --n)
        *dst++ = *src++;
}
