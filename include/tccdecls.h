/*  tccdecls.h — the builtin alias declarations, split out of tccdefs.h.

    These create compiler symbols (extern prototypes with __RENAME asm
    labels), not macros.  On the RP2350 parsing them costs ~30 ms of every
    compile — the macro-expander/parser alternation thrashes the 16 KiB XIP
    cache — so the default configuration builds the same prototypes
    programmatically (tccgen_predef_protos in tccgen.c) and never parses this
    text.  It remains the source of truth for every other configuration:
    -E (skipped entirely, as this section always was), bounds checking
    (__bound_ renames), and -fleading-underscore.  Keep the two in sync; the
    corpus byte-identity A/B (TCC_NO_PROGRAMMATIC_DECLS=1) is the check.

    Same column-1 platform-macro rules as tccdefs.h (see conftest.c);
    converted to tccdecls_.h the same way. */

#ifndef __TCC_PP__

/* TCC BBUILTIN AND BOUNDS ALIASES */
#ifdef __leading_underscore
#define __RENAME(X) __asm__("_" X)
#else
#define __RENAME(X) __asm__(X)
#endif

#ifdef __TCC_BCHECK__
#define __BUILTINBC(ret, name, params) ret __builtin_##name params __RENAME("__bound_" #name);
#define __BOUND(ret, name, params) ret name params __RENAME("__bound_" #name);
#else
#define __BUILTINBC(ret, name, params) ret __builtin_##name params __RENAME(#name);
#define __BOUND(ret, name, params)
#endif
#ifdef _WIN32
#define __BOTH __BOUND
#define __BUILTIN(ret, name, params)
#else
#define __BOTH(ret, name, params) __BUILTINBC(ret, name, params) __BOUND(ret, name, params)
#define __BUILTIN(ret, name, params) ret __builtin_##name params __RENAME(#name);
#endif

__BOTH(void *, memcpy, (void *, const void *, __SIZE_TYPE__))
__BOTH(void *, memmove, (void *, const void *, __SIZE_TYPE__))
__BOTH(void *, memset, (void *, int, __SIZE_TYPE__))
__BOTH(int, memcmp, (const void *, const void *, __SIZE_TYPE__))
__BOTH(__SIZE_TYPE__, strlen, (const char *))
__BOTH(char *, strcpy, (char *, const char *))
__BOTH(char *, strncpy, (char *, const char *, __SIZE_TYPE__))
__BOTH(int, strcmp, (const char *, const char *))
__BOTH(int, strncmp, (const char *, const char *, __SIZE_TYPE__))
__BOTH(char *, strcat, (char *, const char *))
__BOTH(char *, strncat, (char *, const char *, __SIZE_TYPE__))
__BOTH(char *, strchr, (const char *, int))
__BOTH(char *, strrchr, (const char *, int))
__BOTH(char *, strdup, (const char *))
#if defined __ARM_EABI__
__BOUND(void *, __aeabi_memcpy, (void *, const void *, __SIZE_TYPE__))
__BOUND(void *, __aeabi_memmove, (void *, const void *, __SIZE_TYPE__))
__BOUND(void *, __aeabi_memmove4, (void *, const void *, __SIZE_TYPE__))
__BOUND(void *, __aeabi_memmove8, (void *, const void *, __SIZE_TYPE__))
__BOUND(void *, __aeabi_memset, (void *, int, __SIZE_TYPE__))
#endif

#if defined __linux__ || defined __APPLE__ // HAVE MALLOC_REDIR
#define __MAYBE_REDIR __BUILTIN
#else
#define __MAYBE_REDIR __BOTH
#endif
__MAYBE_REDIR(void *, malloc, (__SIZE_TYPE__))
__MAYBE_REDIR(void *, realloc, (void *, __SIZE_TYPE__))
__MAYBE_REDIR(void *, calloc, (__SIZE_TYPE__, __SIZE_TYPE__))
__MAYBE_REDIR(void *, memalign, (__SIZE_TYPE__, __SIZE_TYPE__))
__MAYBE_REDIR(void, free, (void *))
#if defined __i386__ || defined __x86_64__
__BOTH(void *, alloca, (__SIZE_TYPE__))
#else
__BUILTIN(void *, alloca, (__SIZE_TYPE__))
#endif
__BUILTIN(void, abort, (void))
__BUILTIN(void, exit, (int))
__BUILTIN(int, printf, (const char *, ...))
__BUILTIN(int, puts, (const char *))
__BUILTIN(int, putchar, (int))
__BUILTIN(int, fputc, (int, void *))
__BUILTIN(__SIZE_TYPE__, fwrite, (const void *, __SIZE_TYPE__, __SIZE_TYPE__, void *))
__BUILTIN(int, sprintf, (char *, const char *, ...))
__BUILTIN(int, snprintf, (char *, __SIZE_TYPE__, const char *, ...))
char *__builtin_index(const char *, int) __RENAME("strchr");
char *__builtin_rindex(const char *, int) __RENAME("__tcc_strrchr");
void __builtin_bcopy(const void *, void *, __SIZE_TYPE__) __RENAME("bcopy");
void __builtin_bzero(void *, __SIZE_TYPE__) __RENAME("bzero");
int __builtin_printf_unlocked(const char *, ...) __RENAME("printf_unlocked");
int __builtin_fprintf_unlocked(void *, const char *, ...) __RENAME("fprintf_unlocked");
int __builtin_fputs_unlocked(const char *, void *) __RENAME("fputs_unlocked");
unsigned int __builtin_uabs(int) __RENAME("uabs");
unsigned long __builtin_ulabs(long) __RENAME("ulabs");
unsigned long long __builtin_ullabs(long long) __RENAME("ullabs");
unsigned long long __builtin_umaxabs(long long) __RENAME("umaxabs");
__BOUND(void, longjmp, ())
#if !defined _WIN32
__BOUND(void *, mmap, ())
__BOUND(int, munmap, ())
#endif
#undef __BUILTINBC
#undef __BUILTIN
#undef __BOUND
#undef __BOTH
#undef __MAYBE_REDIR
#undef __RENAME

#define __BUILTIN_EXTERN(name, u)                                                                                      \
  int __builtin_##name(u int);                                                                                         \
  int __builtin_##name##l(u long);                                                                                     \
  int __builtin_##name##ll(u long long);
__BUILTIN_EXTERN(ffs, )
__BUILTIN_EXTERN(clz, unsigned)
__BUILTIN_EXTERN(ctz, unsigned)
__BUILTIN_EXTERN(clrsb, )
__BUILTIN_EXTERN(popcount, unsigned)
__BUILTIN_EXTERN(parity, unsigned)
#undef __BUILTIN_EXTERN

#endif /* ndef __TCC_PP__ */
