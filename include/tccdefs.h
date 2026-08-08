/*  tccdefs.h

    Nothing is defined before this file except target machine, target os
    and the few things related to option settings in tccpp.c:tcc_predefs().

    This file is either included at runtime as is, or converted and
    included as C-strings at compile-time (depending on CONFIG_TCC_PREDEFS).

    Note that line indent matters:

    - in lines starting at column 1, platform macros are replaced by
      corresponding TCC target compile-time macros.  See conftest.c for
      the list of platform macros supported in lines starting at column 1.

    - only lines indented >= 4 are actually included into the executable,
      check tccdefs_.h.
*/

#pragma once

#if __SIZEOF_POINTER__ == 4
/* 32bit systems. */
#if defined __OpenBSD__
#define __SIZE_TYPE__ unsigned long
#define __PTRDIFF_TYPE__ long
#else
#define __SIZE_TYPE__ unsigned int
#define __PTRDIFF_TYPE__ int
#endif
#define __ILP32__ 1
#define __INT64_TYPE__ long long
#define __INTMAX_TYPE__ long long
#define __UINTMAX_TYPE__ unsigned long long
#elif __SIZEOF_LONG__ == 4
/* 64bit Windows. */
#define __SIZE_TYPE__ unsigned long long
#define __PTRDIFF_TYPE__ long long
#define __LLP64__ 1
#define __INT64_TYPE__ long long
#define __INTMAX_TYPE__ long long
#define __UINTMAX_TYPE__ unsigned long long
#else
/* Other 64bit systems. */
#define __SIZE_TYPE__ unsigned long
#define __PTRDIFF_TYPE__ long
#define __LP64__ 1
#if defined __linux__
#define __INT64_TYPE__ long
#define __INTMAX_TYPE__ long
#define __UINTMAX_TYPE__ unsigned long
#else /* APPLE, BSD */
#define __INT64_TYPE__ long long
#define __INTMAX_TYPE__ long long
#define __UINTMAX_TYPE__ unsigned long long
#endif
#endif
#define __SIZEOF_SHORT__ 2
#define __SIZEOF_INT__ 4
#define __INT_MAX__ 0x7fffffff
#define __SCHAR_MAX__ 0x7f
#define __SHRT_MAX__ 0x7fff
#if __SIZEOF_LONG__ == 4
#define __LONG_MAX__ 0x7fffffffL
#else
#define __LONG_MAX__ 0x7fffffffffffffffL
#endif
#define __SIZEOF_LONG_LONG__ 8
#define __LONG_LONG_MAX__ 0x7fffffffffffffffLL
#define __INTMAX_MAX__ 0x7fffffffffffffffLL
#define __INTMAX_WIDTH__ 64
#if __SIZEOF_POINTER__ == 4
#define __PTRDIFF_MAX__ 0x7fffffff
#define __SIZE_MAX__ 0xffffffffU
#elif __SIZEOF_LONG__ == 4
#define __PTRDIFF_MAX__ 0x7fffffffffffffffLL
#define __SIZE_MAX__ 0xffffffffffffffffULL
#else
#define __PTRDIFF_MAX__ 0x7fffffffffffffffL
#define __SIZE_MAX__ 0xffffffffffffffffUL
#endif
#define __CHAR_BIT__ 8
#define __ORDER_LITTLE_ENDIAN__ 1234
#define __ORDER_BIG_ENDIAN__ 4321
#define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
#if defined _WIN32
#define __WCHAR_TYPE__ unsigned short
#define __WINT_TYPE__ unsigned short
#elif defined __linux__
#define __WCHAR_TYPE__ int
#define __WINT_TYPE__ unsigned int
#else
#define __WCHAR_TYPE__ int
#define __WINT_TYPE__ int
#endif

    #if __STDC_VERSION__ >= 201112L
    #define __STDC_NO_ATOMICS__ 1
    #define __STDC_NO_COMPLEX__ 1
    #define __STDC_NO_THREADS__ 1
#if !defined _WIN32
    #define __STDC_UTF_16__ 1
    #define __STDC_UTF_32__ 1
#endif
    #endif

#if defined _WIN32
#define __declspec(x) __attribute__((x))
#define __cdecl

#elif defined __FreeBSD__
#define __GNUC__ 9
#define __GNUC_MINOR__ 3
#define __GNUC_PATCHLEVEL__ 0
#define __GNUC_STDC_INLINE__ 1
#define __NO_TLS 1
#define __RUNETYPE_INTERNAL 1
#if __SIZEOF_POINTER__ == 8
/* FIXME, __int128_t is used by setjump */
#define __int128_t                                                                                                     \
  struct                                                                                                               \
  {                                                                                                                    \
    unsigned char _dummy[16] __attribute((aligned(16)));                                                               \
  }
#define __SIZEOF_SIZE_T__ 8
#define __SIZEOF_PTRDIFF_T__ 8
#else
#define __SIZEOF_SIZE_T__ 4
#define __SIZEOF_PTRDIFF_T__ 4
#endif

#elif defined __FreeBSD_kernel__

#elif defined __NetBSD__
#define __GNUC__ 4
#define __GNUC_MINOR__ 1
#define __GNUC_PATCHLEVEL__ 0
#define _Pragma(x)
#define __ELF__ 1
#if defined __aarch64__
#define _LOCORE /* avoids usage of __asm */
#endif

#elif defined __OpenBSD__
#define __GNUC__ 4
#define _ANSI_LIBRARY 1

#elif defined __YasOS__
#define __GNUC__ 4
/* Note: no `#define __linux__` here — target_os_defs (tccpp.c) already
 * predefines __linux__/__linux for YasOS, and c2str would mangle the line
 * into a bogus `#define TARGETOS_Linux 1` predefine (unreserved name). */

#elif defined __APPLE__
/* emulate APPLE-GCC to make libc's headerfiles compile: */
#define __GNUC__ 4     /* darwin emits warning on GCC<4 */
#define __APPLE_CC__ 1 /* for <TargetConditionals.h> */
#define __LITTLE_ENDIAN__ 1
#define _DONT_USE_CTYPE_INLINE_ 1
/* avoids usage of GCC/clang specific builtins in libc-headerfiles: */
#define __FINITE_MATH_ONLY__ 1
#define _FORTIFY_SOURCE 0
// #define __has_builtin(x) 0

#elif defined __ANDROID__
#define BIONIC_IOCTL_NO_SIGNEDNESS_OVERLOAD

#else
/* Linux */

#endif

/* Some derived integer types needed to get stdint.h to compile correctly on some platforms */
#ifndef __NetBSD__
#define __UINTPTR_TYPE__ unsigned __PTRDIFF_TYPE__
#define __INTPTR_TYPE__ __PTRDIFF_TYPE__
#endif
#define __INT8_TYPE__ signed char
#define __INT16_TYPE__ short
#define __INT32_TYPE__ int
#define __UINT8_TYPE__ unsigned char
#define __UINT16_TYPE__ unsigned short
#define __UINT32_TYPE__ unsigned int

/* Least-width integer types (C99 stdint.h, GCC predefined macros) */
#define __INT_LEAST8_TYPE__ signed char
#define __INT_LEAST16_TYPE__ short
#define __INT_LEAST32_TYPE__ int
#define __INT_LEAST64_TYPE__ long long
#define __UINT_LEAST8_TYPE__ unsigned char
#define __UINT_LEAST16_TYPE__ unsigned short
#define __UINT_LEAST32_TYPE__ unsigned int
#define __UINT_LEAST64_TYPE__ unsigned long long
#define __INT_LEAST8_MAX__ 0x7f
#define __INT_LEAST16_MAX__ 0x7fff
#define __INT_LEAST32_MAX__ 0x7fffffff
#define __INT_LEAST64_MAX__ 0x7fffffffffffffffLL
#define __UINT_LEAST8_MAX__ 0xff
#define __UINT_LEAST16_MAX__ 0xffff
#define __UINT_LEAST32_MAX__ 0xffffffffU
#define __UINT_LEAST64_MAX__ 0xffffffffffffffffULL

/* Sized integer max/min values needed by stdint.h on some platforms.
   These are indented with 4 spaces so that c2str stringifies the guards
   instead of emitting them as real host-preprocessor directives (which
   would cause the host GCC to strip the blocks). */
    #ifndef __INT8_MAX__
    #define __INT8_MAX__ 0x7f
    #endif
    #ifndef __INT16_MAX__
    #define __INT16_MAX__ 0x7fff
    #endif
    #ifndef __INT32_MAX__
    #define __INT32_MAX__ 0x7fffffff
    #endif
    #ifndef __INT64_MAX__
    #define __INT64_MAX__ 0x7fffffffffffffffLL
    #endif
    #ifndef __UINT8_MAX__
    #define __UINT8_MAX__ 0xff
    #endif
    #ifndef __UINT16_MAX__
    #define __UINT16_MAX__ 0xffff
    #endif
    #ifndef __UINT32_MAX__
    #define __UINT32_MAX__ 0xffffffffU
    #endif
    #ifndef __UINT64_MAX__
    #define __UINT64_MAX__ 0xffffffffffffffffULL
    #endif

/* Floating point limits (IEEE 754). These match include/float.h values. */
#define __FLT_MAX__ 3.40282347e+38F
#define __FLT_MIN__ 1.17549435e-38F
#define __FLT_EPSILON__ 1.19209290e-07F
#define __FLT_DIG__ 6
#define __FLT_MANT_DIG__ 24
#define __FLT_MAX_EXP__ 128
#define __FLT_MIN_EXP__ (-125)
#define __DBL_MAX__ 1.7976931348623157e+308
#define __DBL_MIN__ 2.2250738585072014e-308
#define __DBL_EPSILON__ 2.2204460492503131e-16
#define __DBL_DIG__ 15
#define __DBL_MANT_DIG__ 53
#define __DBL_MAX_EXP__ 1024
#define __DBL_MIN_EXP__ (-1021)
#define __LDBL_MAX__ 1.7976931348623157e+308L
#define __LDBL_MIN__ 2.2250738585072014e-308L
#define __LDBL_EPSILON__ 2.2204460492503131e-16L
#define __LDBL_DIG__ 15
#define __LDBL_MANT_DIG__ 53
#define __LDBL_MAX_EXP__ 1024
#define __LDBL_MIN_EXP__ (-1021)

    #ifdef __leading_underscore
    #define __USER_LABEL_PREFIX__ _
    #else
    #define __USER_LABEL_PREFIX__
    #endif
/* not implemented */
#define __PRETTY_FUNCTION__ __FUNCTION__
#define __has_builtin(x) 0
#define __has_feature(x) 0
#define __has_attribute(x) 0
/* C23 Keywords */
#define _Nonnull
#define _Nullable
#define _Nullable_result
#define _Null_unspecified

/* skip __builtin... with -E */
#ifndef __TCC_PP__

#define __builtin_offsetof(type, field) ((__SIZE_TYPE__) & ((type *)0)->field)
#define __builtin_extract_return_addr(x) x
#if !defined __linux__ && !defined _WIN32
/* used by math.h */
#define __builtin_huge_val() 1e500
#define __builtin_huge_valf() 1e50f
#define __builtin_huge_vall() 1e5000L
#if defined __APPLE__
#define __builtin_nanf(ignored_string) (0.0F / 0.0F)
/* used by floats.h to implement FLT_ROUNDS C99 macro. 1 == to nearest */
#define __builtin_flt_rounds() 1
/* used by _fd_def.h */
#define __builtin_bzero(p, ignored_size) bzero(p, sizeof(*(p)))
#else
#define __builtin_nanf(ignored_string) (0.0F / 0.0F)
#endif
#endif

/* __builtin_va_list */
#if defined __x86_64__
#if !defined _WIN32
/* GCC compatible definition of va_list. */
/* This should be in sync with the declaration in our lib/libtcc1.c */
typedef struct
{
  unsigned gp_offset, fp_offset;
  union
  {
    unsigned overflow_offset;
    char *overflow_arg_area;
  };
  char *reg_save_area;
} __builtin_va_list[1];

void *__va_arg(__builtin_va_list ap, int arg_type, int size, int align);
#define __builtin_va_start(ap, last) (*(ap) = *(__builtin_va_list)((char *)__builtin_frame_address(0) - 24))
#define __builtin_va_arg(ap, t) (*(t *)(__va_arg(ap, __builtin_va_arg_types(t), sizeof(t), __alignof__(t))))
#define __builtin_va_copy(dest, src) (*(dest) = *(src))

#else /* _WIN64 */
typedef char *__builtin_va_list;
#define __builtin_va_arg(ap, t)                                                                                        \
  ((sizeof(t) > 8 || (sizeof(t) & (sizeof(t) - 1))) ? **(t **)((ap += 8) - 8) : *(t *)((ap += 8) - 8))
#define __builtin_va_copy(dest, src) (dest) = (src)
#endif

#elif defined __arm__
/* ARM EABI va_list: simple char pointer (GCC-compatible ABI).
   Runtime helpers in lib/va_list.c. */
typedef char *__builtin_va_list;

/* __builtin_va_start and __builtin_va_arg are compiler intrinsics
   (TOK_builtin_va_start / TOK_builtin_va_arg), so ARM needs no va_list runtime
   helpers at all.  va_start expands to the address of the first anonymous
   argument, which the backend knows exactly; va_arg expands to the pointer bump
   (align up, take, advance), which also keeps `ap` in a register because its
   address is never taken.  va_arg being an intrinsic additionally supports VLA
   struct types passed by invisible reference. */
#define __builtin_va_copy(dest, src) (dest) = (src)

#elif defined __aarch64__
#if defined __APPLE__
typedef struct
{
  void *__stack;
} __builtin_va_list;

#else
typedef struct
{
  void *__stack, *__gr_top, *__vr_top;
  int __gr_offs, __vr_offs;
} __builtin_va_list;

#endif
#elif defined __riscv
typedef char *__builtin_va_list;
#define __va_reg_size (__riscv_xlen >> 3)
#define _tcc_align(addr, type) (((unsigned long)addr + __alignof__(type) - 1) & -(__alignof__(type)))
#define __builtin_va_arg(ap, type)                                                                                     \
  (*(sizeof(type) > (2 * __va_reg_size)                                                                                \
         ? *(type **)((ap += __va_reg_size) - __va_reg_size)                                                           \
         : (ap = (va_list)(_tcc_align(ap, type) + (sizeof(type) + __va_reg_size - 1) & -__va_reg_size),                \
            (type *)(ap - ((sizeof(type) + __va_reg_size - 1) & -__va_reg_size)))))
#define __builtin_va_copy(dest, src) (dest) = (src)

#else /* __i386__ */
typedef char *__builtin_va_list;
#define __builtin_va_start(ap, last) (ap = ((char *)&(last)) + ((sizeof(last) + 3) & ~3))
#define __builtin_va_arg(ap, t) (*(t *)((ap += (sizeof(t) + 3) & ~3) - ((sizeof(t) + 3) & ~3)))
#define __builtin_va_copy(dest, src) (dest) = (src)

#endif
#define __builtin_va_end(ap) (void)(ap)


#endif /* ndef __TCC_PP__ */
