/* _FORTIFY_SOURCE as glibc-style headers do it -- yasos libc had these
 * wrappers until tcc checked the calls itself -- for test_fortify.py's
 * __builtin___*_chk cases: the QEMU runs link newlib, whose own fortify
 * headers are kept out (they wrap the functions in always_inline functions,
 * whose parameters tcc's __builtin_object_size never looks through). */
#ifndef FORTIFY_LIBC_H
#define FORTIFY_LIBC_H

#ifdef _FORTIFY_SOURCE
#if _FORTIFY_SOURCE > 2
#define FORTIFY_TEST_LEVEL 3
#elif _FORTIFY_SOURCE > 1
#define FORTIFY_TEST_LEVEL 2
#elif _FORTIFY_SOURCE > 0
#define FORTIFY_TEST_LEVEL 1
#else
#define FORTIFY_TEST_LEVEL 0
#endif
#undef _FORTIFY_SOURCE
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef FORTIFY_TEST_LEVEL
#define _FORTIFY_SOURCE FORTIFY_TEST_LEVEL
#endif

/* --- sys/_fortify.h --- */
#if defined(_FORTIFY_SOURCE) && defined(__has_builtin) && !defined(__cplusplus)
#if _FORTIFY_SOURCE > 0 && __has_builtin(__builtin_object_size) && __has_builtin(__builtin___memcpy_chk)
#define __YL_FORTIFY 1

#if _FORTIFY_SOURCE > 2 && __has_builtin(__builtin_dynamic_object_size)
#define __yl_bos(p) __builtin_dynamic_object_size(p, 1)
#define __yl_bos0(p) __builtin_dynamic_object_size(p, 0)
#elif _FORTIFY_SOURCE > 1
#define __yl_bos(p) __builtin_object_size(p, 1)
#define __yl_bos0(p) __builtin_object_size(p, 0)
#else
#define __yl_bos(p) __builtin_object_size(p, 0)
#define __yl_bos0(p) __builtin_object_size(p, 0)
#endif

#define __yl_fortify_flag (_FORTIFY_SOURCE > 1)

__attribute__((__noreturn__)) void __chk_fail(void);

#endif
#endif

/* --- string.h --- */
#ifdef __YL_FORTIFY
void *__memcpy_chk(void *dst, const void *src, size_t n, size_t dstlen);
void *__memmove_chk(void *dst, const void *src, size_t n, size_t dstlen);
void *__mempcpy_chk(void *dst, const void *src, size_t n, size_t dstlen);
void *__memset_chk(void *s, int v, size_t n, size_t dstlen);
char *__strcpy_chk(char *dst, const char *src, size_t dstlen);
char *__stpcpy_chk(char *dst, const char *src, size_t dstlen);
char *__strncpy_chk(char *dst, const char *src, size_t n, size_t dstlen);
char *__stpncpy_chk(char *dst, const char *src, size_t n, size_t dstlen);
char *__strcat_chk(char *dst, const char *src, size_t dstlen);
char *__strncat_chk(char *dst, const char *src, size_t n, size_t dstlen);

#define memcpy(d, s, n) __builtin___memcpy_chk(d, s, n, __yl_bos0(d))
#define memmove(d, s, n) __builtin___memmove_chk(d, s, n, __yl_bos0(d))
#define mempcpy(d, s, n) __builtin___mempcpy_chk(d, s, n, __yl_bos0(d))
#define memset(d, v, n) __builtin___memset_chk(d, v, n, __yl_bos0(d))
#define strcpy(d, s) __builtin___strcpy_chk(d, s, __yl_bos(d))
#define stpcpy(d, s) __builtin___stpcpy_chk(d, s, __yl_bos(d))
#define strncpy(d, s, n) __builtin___strncpy_chk(d, s, n, __yl_bos(d))
#define stpncpy(d, s, n) __builtin___stpncpy_chk(d, s, n, __yl_bos(d))
#define strcat(d, s) __builtin___strcat_chk(d, s, __yl_bos(d))
#define strncat(d, s, n) __builtin___strncat_chk(d, s, n, __yl_bos(d))
#endif

/* --- stdio.h --- */
#ifdef __YL_FORTIFY
int __sprintf_chk(char *dst, int flag, size_t dstlen, const char *fmt, ...);
int __snprintf_chk(char *dst, size_t sz, int flag, size_t dstlen, const char *fmt, ...);
int __vsprintf_chk(char *dst, int flag, size_t dstlen, const char *fmt, va_list ap);
int __vsnprintf_chk(char *dst, size_t sz, int flag, size_t dstlen, const char *fmt, va_list ap);

#define sprintf(d, ...) __builtin___sprintf_chk(d, __yl_fortify_flag, __yl_bos(d), __VA_ARGS__)
#define snprintf(d, n, ...) __builtin___snprintf_chk(d, n, __yl_fortify_flag, __yl_bos(d), __VA_ARGS__)
#define vsprintf(d, f, ap) __builtin___vsprintf_chk(d, __yl_fortify_flag, __yl_bos(d), f, ap)
#define vsnprintf(d, n, f, ap) __builtin___vsnprintf_chk(d, n, __yl_fortify_flag, __yl_bos(d), f, ap)
#endif

#endif /* FORTIFY_LIBC_H */
