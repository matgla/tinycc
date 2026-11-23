/* yasc.h - errno and syscall glue for the yasc dialect's error unions.
 *
 * In a function returning `!T`:
 *   int fd = try_errno(open(path, O_RDONLY));  -- negative: return errno
 *   void *p = try_null(malloc(n));              -- NULL: return errno
 *   int n = try_neg(raw_syscall(...));          -- negative: return -result
 * error_name(e) (a builtin) gives an error's name, strerror(e) the message
 * of an errno value.  See tinycc docs/plans/yasc_errors.md. */
#ifndef _YASC_H
#define _YASC_H

#ifndef __YASC__
#error "yasc.h is for the yasc dialect: -std=yasc or #pragma yasc"
#endif
/* yasc itself: its macros expand as yasc (error_from is a keyword) */
#pragma yasc

#include <errno.h>

#define try_errno(expr)                                                                                                \
  ({                                                                                                                   \
    __typeof__(expr) __yasc_r = (expr);                                                                                \
    if (__yasc_r < 0)                                                                                                  \
      return error_from(errno);                                                                                        \
    __yasc_r;                                                                                                          \
  })

#define try_null(expr)                                                                                                 \
  ({                                                                                                                   \
    __typeof__(expr) __yasc_p = (expr);                                                                                \
    if (!__yasc_p)                                                                                                     \
      return error_from(errno);                                                                                        \
    __yasc_p;                                                                                                          \
  })

#define try_neg(expr)                                                                                                  \
  ({                                                                                                                   \
    __typeof__(expr) __yasc_r = (expr);                                                                                \
    if (__yasc_r < 0)                                                                                                  \
      return error_from(-__yasc_r);                                                                                    \
    __yasc_r;                                                                                                          \
  })

#endif /* _YASC_H */
