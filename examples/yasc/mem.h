/* mem.h -- a small allocator written in regular C, for the yasc examples.
 *
 * It is plain C: no yasc keyword, no qualifier.  The GCC attributes are what
 * tell a yasc caller who owns what (chapter 6 of README.md):
 *   malloc(mem_free, 1)  the result is a fresh block, `owned maybe` to yasc,
 *                        and mem_free's argument takes it back (`drop`);
 *   nonnull(1)           a null constant there is a -Wnonnull warning.
 * On YasOS, libc's malloc, free and strdup carry the same attributes, so yasc
 * code can call them directly; these examples use mem.h so that they build
 * with any C library. */
#ifndef MEM_H
#define MEM_H

#include <stddef.h>

void mem_free(void *p);
void *mem_new(size_t n) __attribute__((malloc, malloc(mem_free, 1)));
char *mem_strdup(const char *s) __attribute__((malloc, malloc(mem_free, 1), nonnull(1)));

/* how many blocks are out: the examples print it to show nothing leaked */
int mem_live(void);

#endif
