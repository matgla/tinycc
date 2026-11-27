/* yasc_src.h - yasc_src, the type of @src(): where the code is.
 *
 *   #include <yasc_src.h>
 *
 *   void log_warn(const char *msg, yasc_src at = @src());
 *   log_warn("disk full");       // at: this call's file, function and line
 *
 *   printf("%s:%u: %s\n", @src().file, @src().line, @src().func);
 *
 * tcc defines struct __yasc_src itself, a value of two words:
 *
 *   __fn                 the function's header in .rodata, one per function
 *   unsigned line;       the line, as __LINE__
 *
 * and reads at.file (as __FILE__), at.func (as __func__, "" at file scope)
 * and at.module (the yasc module, "" outside one) through the header.  A call
 * passing @src() costs an immediate and the header's address, which the
 * function's calls share; the header's names are self-relative words the
 * linker fills in, so it costs no RAM and reads the same from any module.
 * A yasc_src is copied as any struct; @src() itself is made in a function
 * only (a static one would need a load-time relocation), while
 * @src().line / .file / .func / .module work anywhere.
 * docs/yasc_default_args.md.
 *
 * Code that other compilers build too (the C header of a yasc function lists
 * every argument): YASC_SRC passes the location by hand, and YASC_SRC_FILE()
 * and its siblings read one. */
#ifndef _YASC_SRC_H
#define _YASC_SRC_H

typedef struct __yasc_src yasc_src;

#ifdef __TINYC__

#define YASC_SRC (@src())
#define YASC_SRC_FILE(at) ((at).file)
#define YASC_SRC_FUNC(at) ((at).func)
#define YASC_SRC_MODULE(at) ((at).module)

#else

/* The same layout: each name in the header is its distance from its own
   word. */
struct __yasc_src_fn
{
  __INTPTR_TYPE__ file, func, module;
};

struct __yasc_src
{
  const struct __yasc_src_fn *__fn;
  unsigned line;
};

#define __yasc_src_name(w) ((const char *)&(w) + (w))
#define YASC_SRC_FILE(at) __yasc_src_name((at).__fn->file)
#define YASC_SRC_FUNC(at) __yasc_src_name((at).__fn->func)
#define YASC_SRC_MODULE(at) __yasc_src_name((at).__fn->module)

static inline const struct __yasc_src_fn *__yasc_src_fn_set(struct __yasc_src_fn *h, const char *file,
                                                            const char *func)
{
  h->file = (__INTPTR_TYPE__)file - (__INTPTR_TYPE__)&h->file;
  h->func = (__INTPTR_TYPE__)func - (__INTPTR_TYPE__)&h->func;
  h->module = (__INTPTR_TYPE__)"" - (__INTPTR_TYPE__)&h->module;
  return h;
}

/* A distance between two objects is no constant for a C compiler: the
   header is filled in where it is passed. */
#define YASC_SRC                                                                                                       \
  (__extension__({                                                                                                     \
    static struct __yasc_src_fn __yasc_src_here;                                                                       \
    (struct __yasc_src){__yasc_src_fn_set(&__yasc_src_here, __FILE__, __func__), __LINE__};                            \
  }))

#endif

#endif /* _YASC_SRC_H */
