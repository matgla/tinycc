#ifndef _LIMITS_H___
#define _LIMITS_H___

/* The compiler-supplied <limits.h>, mirroring the one GCC ships.
   tcc predefines __GNUC__, so every libc whose <limits.h> follows the GCC
   convention (newlib, glibc, the BSDs) stops defining the ISO limits itself and
   ends with `#include_next <limits.h>`, expecting the compiler to finish the
   job.  Without this file that chain has nowhere to land: a freestanding cross
   compile (-nostdinc, newlib headers only) fails with "include file 'limits.h'
   not found" the moment anything reaches <limits.h> -- which newlib does from
   <machine/_default_types.h> and <sys/_intsup.h>, i.e. from <stdio.h>.

   `_GCC_LIMITS_H_` is the macro those libc headers test to detect that the
   compiler's limits.h has been seen, so define it whichever way round the two
   files end up being included. */
#define _GCC_LIMITS_H_

#ifndef _LIBC_LIMITS_H_
/* We were not reached through a libc <limits.h> (which is what defines
   _LIBC_LIMITS_H_), so pull one in first if the configuration has one -- its
   POSIX limits (PATH_MAX, SSIZE_MAX, ...) are not ours to invent.  A
   freestanding build has none, hence the __has_include_next guard. */
# if defined __has_include_next
#  if __has_include_next(<limits.h>)
#   include_next <limits.h>
#  endif
# endif
#endif

/* Number of bits in a `char'. */
#undef CHAR_BIT
#define CHAR_BIT __CHAR_BIT__

/* Maximum length of a multibyte character. */
#ifndef MB_LEN_MAX
#define MB_LEN_MAX 1
#endif

/* Minimum and maximum values a `signed char' can hold. */
#undef SCHAR_MIN
#define SCHAR_MIN (-SCHAR_MAX - 1)
#undef SCHAR_MAX
#define SCHAR_MAX __SCHAR_MAX__

/* Maximum value an `unsigned char' can hold.  (Minimum is 0). */
#undef UCHAR_MAX
#if __SCHAR_MAX__ == __INT_MAX__
# define UCHAR_MAX (SCHAR_MAX * 2U + 1U)
#else
# define UCHAR_MAX (SCHAR_MAX * 2 + 1)
#endif

/* Minimum and maximum values a `char' can hold. */
#ifdef __CHAR_UNSIGNED__
# undef CHAR_MIN
# if __SCHAR_MAX__ == __INT_MAX__
#  define CHAR_MIN 0U
# else
#  define CHAR_MIN 0
# endif
# undef CHAR_MAX
# define CHAR_MAX UCHAR_MAX
#else
# undef CHAR_MIN
# define CHAR_MIN SCHAR_MIN
# undef CHAR_MAX
# define CHAR_MAX SCHAR_MAX
#endif

/* Minimum and maximum values a `signed short int' can hold. */
#undef SHRT_MIN
#define SHRT_MIN (-SHRT_MAX - 1)
#undef SHRT_MAX
#define SHRT_MAX __SHRT_MAX__

/* Maximum value an `unsigned short int' can hold.  (Minimum is 0). */
#undef USHRT_MAX
#if __SHRT_MAX__ == __INT_MAX__
# define USHRT_MAX (SHRT_MAX * 2U + 1U)
#else
# define USHRT_MAX (SHRT_MAX * 2 + 1)
#endif

/* Minimum and maximum values a `signed int' can hold. */
#undef INT_MIN
#define INT_MIN (-INT_MAX - 1)
#undef INT_MAX
#define INT_MAX __INT_MAX__

/* Maximum value an `unsigned int' can hold.  (Minimum is 0). */
#undef UINT_MAX
#define UINT_MAX (INT_MAX * 2U + 1U)

/* Minimum and maximum values a `signed long int' can hold. */
#undef LONG_MIN
#define LONG_MIN (-LONG_MAX - 1L)
#undef LONG_MAX
#define LONG_MAX __LONG_MAX__

/* Maximum value an `unsigned long int' can hold.  (Minimum is 0). */
#undef ULONG_MAX
#define ULONG_MAX (LONG_MAX * 2UL + 1UL)

#if defined __STDC_VERSION__ && __STDC_VERSION__ >= 199901L
/* Minimum and maximum values a `signed long long int' can hold. */
# undef LLONG_MIN
# define LLONG_MIN (-LLONG_MAX - 1LL)
# undef LLONG_MAX
# define LLONG_MAX __LONG_LONG_MAX__

/* Maximum value an `unsigned long long int' can hold.  (Minimum is 0). */
# undef ULLONG_MAX
# define ULLONG_MAX (LLONG_MAX * 2ULL + 1ULL)
#endif

#ifndef __STRICT_ANSI__
/* The GNU spellings of the same three. */
# undef LONG_LONG_MIN
# define LONG_LONG_MIN (-LONG_LONG_MAX - 1LL)
# undef LONG_LONG_MAX
# define LONG_LONG_MAX __LONG_LONG_MAX__
# undef ULONG_LONG_MAX
# define ULONG_LONG_MAX (LONG_LONG_MAX * 2ULL + 1ULL)
#endif

#endif /* _LIMITS_H___ */
