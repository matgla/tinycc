/*
 *  tcctools_stubs.c - minimal libtcc link stubs for the tcctools/ unit-test
 *  binary (build_tcctools/run_unit_tests_tcctools)
 *
 *  tcctools.c's external dependencies are:
 *   - the tcc_malloc-family allocators (tcc.h redefines malloc/free/realloc/
 *     strdup, so this TU avoids tcc.h and uses raw libc symbols directly)
 *   - tcc_fileextension() (used by gen_makedeps to derive the .d path)
 *   - _tcc_error_noabort() and tcc_enter_state() (reached through the
 *     TCC_SET_STATE expansion of tcc_error_noabort inside tcctools.c, which
 *     is compiled without USING_GLOBALS)
 *   - the global TCCState pointer (supplied separately by tcc_state_stub.c)
 *
 *  The read16le/write16le/read32le/write32le/add32le/read64le/write64le
 *  helpers and the le2belong/escape_target_dep internals live in the REAL
 *  tcctools.c linked into this binary; redefining any of them here would be
 *  a multiple-definition error.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct TCCState;

void *tcc_malloc(unsigned long size)
{
    void *p = malloc(size);
    if (!p && size)
    {
        fprintf(stderr, "tcc_malloc: out of memory\n");
        exit(1);
    }
    return p;
}

void *tcc_mallocz(unsigned long size)
{
    void *p = tcc_malloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void *tcc_realloc(void *ptr, unsigned long size)
{
    void *p = realloc(ptr, size);
    if (!p && size)
    {
        fprintf(stderr, "tcc_realloc: out of memory\n");
        exit(1);
    }
    return p;
}

/* tcc.h rewrites every tcc_malloc/tcc_mallocz/tcc_realloc call site into these
 * allocation-attribution wrappers (they record the source line of mmap-class
 * allocations for -bench), so the stub layer has to answer to the wrapper
 * names too.  The accounting is of no interest here: just forward. */
void *tcc_malloc_at(unsigned long size, const char *file, int line)
{
    (void)file;
    (void)line;
    return tcc_malloc(size);
}

void *tcc_mallocz_at(unsigned long size, const char *file, int line)
{
    (void)file;
    (void)line;
    return tcc_mallocz(size);
}

void *tcc_realloc_at(void *ptr, unsigned long size, const char *file, int line)
{
    (void)file;
    (void)line;
    return tcc_realloc(ptr, size);
}

void tcc_free(void *ptr)
{
    free(ptr);
}

char *tcc_strdup(const char *str)
{
    size_t n = strlen(str) + 1;
    char *p = (char *)tcc_malloc(n);
    memcpy(p, str, n);
    return p;
}

/* Minimal stand-in for libtcc.c's tcc_fileextension(): returns a pointer to
 * the last '.' in the basename, or to the trailing NUL if there is none.
 * Mirrors the real implementation's contract. */
char *tcc_fileextension(const char *name)
{
    const char *b = name;
    const char *p;
    for (p = name; *p; p++)
    {
        if (*p == '/')
            b = p + 1;
    }
    const char *e = strrchr(b, '.');
    return (char *)(e ? e : p);
}

/* Non-aborting error reporter used by gen_makedeps and tcc_tool_cross.
 * Returns -1, matching libtcc.c's _tcc_error_noabort. */
int _tcc_error_noabort(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, "[tcctools stub] _tcc_error_noabort: ", ap);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    return -1;
}

/* tcc_error_noabort expands to (tcc_enter_state(s1), _tcc_error_noabort(...))
 * inside tcctools.c. The multi-threaded compile-serialization semantics don't
 * matter here, so this is a no-op. */
void tcc_enter_state(struct TCCState *s1)
{
    (void)s1;
}
