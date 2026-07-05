#ifndef _STDDEF_H
#define _STDDEF_H

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ssize_t;
typedef __WCHAR_TYPE__ wchar_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef __PTRDIFF_TYPE__ intptr_t;
typedef __SIZE_TYPE__ uintptr_t;

/* newlib's <sys/_types.h>, <wchar.h> and <wctype.h> all expect stddef.h to
   supply wint_t (they set __need_wint_t before including us). YASOS' newlib
   headers live in a read-only git submodule we cannot patch, so the typedef
   must come from here. It is emitted UNCONDITIONALLY inside the include guard
   (not gated behind __need_wint_t, and with nothing following the guard's
   #endif) so tcc's multiple-include optimization still fires -- see the note at
   the bottom of this file. _WINT_T is the canonical gcc/newlib guard, so a
   toolchain header that also defines wint_t under it composes cleanly. */
#ifndef _WINT_T
#define _WINT_T
typedef __WINT_TYPE__ wint_t;
#endif

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
   and skips re-reading this header.  Upstream tcc kept the wint_t typedef
   OUTSIDE the guard (gated behind __need_wint_t) for legacy glibc
   partial-includes; that trailing content defeated the optimization and forced
   a full re-read + re-tokenize on every #include (3x for one stdio.h compile).
   We keep the wint_t typedef (newlib needs it) but move it INSIDE the guard
   above, so the optimization still fires and nothing trails this #endif. */
#endif
