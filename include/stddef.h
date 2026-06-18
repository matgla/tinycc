#ifndef _STDDEF_H
#define _STDDEF_H

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ssize_t;
typedef __WCHAR_TYPE__ wchar_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef __PTRDIFF_TYPE__ intptr_t;
typedef __SIZE_TYPE__ uintptr_t;

#if __STDC_VERSION__ >= 201112L
typedef union { long long __ll; long double __ld; } max_align_t;
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

#undef offsetof
#define offsetof(type, field) ((size_t)&((type *)0)->field)

#if defined __i386__ || defined __x86_64__
void *alloca(size_t size);
#endif

/* NOTE: nothing must follow the guard's #endif below -- it has to be the last
   token before EOF so tcc's multiple-include optimization records _STDDEF_H
   and skips re-reading this header.  Upstream tcc kept a wint_t typedef
   OUTSIDE the guard (gated behind __need_wint_t) for legacy glibc
   partial-includes; that trailing content defeated the optimization and forced
   a full re-read + re-tokenize on every #include (3x for one stdio.h compile).
   YASOS never defines __need_wint_t, and wint_t is not a stddef.h type per C
   anyway -- libc's <wchar.h> owns it -- so the block is dropped. */
#endif
