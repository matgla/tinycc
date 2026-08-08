/*
 *  TCC - debug/bisect environment knobs
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * ----------------------------------------------------------------------------
 *
 * The compiler carries ~30 getenv-driven knobs: pass-disable switches for
 * bisecting a miscompile (TCC_DISABLE_PASS, TCC_NO_COALESCE, ...), A/B levers
 * an optimization decision was priced with (TCC_NO_REHEARSAL, TCC_KEEP_FWD_DRY,
 * ...) and pure traces (DBG_CLINL, DUMP_IR_CG, SCAN_OVERLAP, ...).  They are
 * developer tooling: in a production compile every one of them is unset, and
 * every one still costs.
 *
 * Two costs, both real:
 *
 *  1. The getenv itself.  glibc walks environ with a strncmp per entry, so with
 *     a 137-entry environment a lookup is ~750 instructions; the ~27 lookups a
 *     compile makes were 0.97% of the instructions of a small-file compile.
 *     (This is why the surviving lookups are latched in a static -- see the
 *     comment in source/opt/engine/pipeline_run.c, where an *unlatched* pair in
 *     a per-pass scan once reached 5.4% of a corpus compile.)
 *
 *  2. The bodies they guard.  On the RP2350 the compiler is XIP-miss-bound, so
 *     wall time answers to code *footprint* (docs: yasos-tcc-binary-size work).
 *     A trace block that never fires still occupies cache lines in the middle of
 *     a hot function, and an O(n^2) verifier like dbg_scan_overlap() is a whole
 *     dead function sitting in .text.
 *
 * CONFIG_TCC_DEBUG_ENV=0 turns every knob below into a compile-time constant, so
 * both costs go away: the accessor folds to its default and the guarded code
 * becomes dead, which for the TCC_DBG_* statement macros happens in the
 * preprocessor (independent of -O level) and for the accessors happens in the
 * optimizer.
 *
 * Default: on wherever CONFIG_TCC_DEBUG is defined -- that is today's `make
 * cross`, so the host bisect tooling (bisect_pass.sh, scripts/opt_profile.py,
 * reduce.py, tests/fuzz/triage_olevels.sh) keeps working with no extra flags.
 * build_rootfs.sh passes -DCONFIG_TCC_DEBUG_ENV=0 for the on-device compiler
 * unless --debug-tcc was given.
 *
 * ADDING A KNOB: declare it with one of the three macros below rather than
 * calling getenv() directly, and put any trace output behind TCC_DBG_TRACE.
 * A raw getenv() in the compiler is a bug -- it is unlatched by default and it
 * survives into the release binary.
 */

#ifndef TCC_DBGENV_H
#define TCC_DBGENV_H

#include <stdlib.h>

#ifndef CONFIG_TCC_DEBUG_ENV
# ifdef CONFIG_TCC_DEBUG
#  define CONFIG_TCC_DEBUG_ENV 1
# else
#  define CONFIG_TCC_DEBUG_ENV 0
# endif
#endif

/* The accessors are `static inline` in both arms: a knob that ends up used only
 * from a block some other #if removed must not trip -Wunused-function -Werror.
 *
 * The latch is a function-local static rather than a file-local one so that a
 * knob is self-contained -- the whole knob is the one macro line.  Every
 * accessor is idempotent and single-threaded (tcc is), so no locking. */

#if CONFIG_TCC_DEBUG_ENV

/* Boolean knob: non-zero when `var` is *set*, whatever its value.
 * Note the deliberate `!= NULL` rather than a value test -- VAR= (empty) still
 * counts as set, matching what these knobs have always done. */
#define TCC_DBG_ENV_FLAG(fn, var)                                                                  \
  static inline int fn(void)                                                                       \
  {                                                                                                \
    static signed char tcc_dbgenv_cached_ = -1;                                                    \
    if (tcc_dbgenv_cached_ < 0)                                                                    \
      tcc_dbgenv_cached_ = getenv(var) != NULL;                                                    \
    return tcc_dbgenv_cached_;                                                                     \
  }

/* String knob: the value, or NULL when unset. */
#define TCC_DBG_ENV_STR(fn, var)                                                                   \
  static inline const char *fn(void)                                                               \
  {                                                                                                \
    static const char *tcc_dbgenv_cached_;                                                         \
    static int tcc_dbgenv_checked_;                                                                \
    if (!tcc_dbgenv_checked_)                                                                      \
    {                                                                                              \
      tcc_dbgenv_checked_ = 1;                                                                     \
      tcc_dbgenv_cached_ = getenv(var);                                                            \
    }                                                                                              \
    return tcc_dbgenv_cached_;                                                                     \
  }

/* Integer knob: atoi(value), or `dflt` when unset or set to the empty string. */
#define TCC_DBG_ENV_INT(fn, var, dflt)                                                             \
  static inline int fn(void)                                                                       \
  {                                                                                                \
    static int tcc_dbgenv_cached_ = (dflt);                                                        \
    static int tcc_dbgenv_checked_;                                                                \
    if (!tcc_dbgenv_checked_)                                                                      \
    {                                                                                              \
      const char *tcc_dbgenv_v_ = getenv(var);                                                     \
      tcc_dbgenv_checked_ = 1;                                                                     \
      if (tcc_dbgenv_v_ && tcc_dbgenv_v_[0])                                                       \
        tcc_dbgenv_cached_ = atoi(tcc_dbgenv_v_);                                                  \
    }                                                                                              \
    return tcc_dbgenv_cached_;                                                                     \
  }

/* Trace statement.  `args` is the whole fprintf argument list including the
 * parens: TCC_DBG_TRACE(dbg_clinl, (stderr, "[CLINL] %s\n", name)).  The double
 * paren is what lets the body vanish in the preprocessor below rather than
 * relying on the optimizer to delete an `if (0)`. */
#define TCC_DBG_TRACE(fn, args)                                                                    \
  do                                                                                               \
  {                                                                                                \
    if (fn())                                                                                      \
      fprintf args;                                                                                \
  } while (0)

/* Guard for a multi-statement debug block that is too big for TCC_DBG_TRACE.
 * Prefer `#if CONFIG_TCC_DEBUG_ENV` around whole helper functions. */
#define TCC_DBG_BLOCK(fn) if (fn())

#else /* !CONFIG_TCC_DEBUG_ENV -- release */

#define TCC_DBG_ENV_FLAG(fn, var)                                                                  \
  static inline int fn(void) { return 0; }

#define TCC_DBG_ENV_STR(fn, var)                                                                   \
  static inline const char *fn(void) { return NULL; }

#define TCC_DBG_ENV_INT(fn, var, dflt)                                                             \
  static inline int fn(void) { return (dflt); }

#define TCC_DBG_TRACE(fn, args)                                                                    \
  do                                                                                               \
  {                                                                                                \
  } while (0)

#define TCC_DBG_BLOCK(fn) if (0)

#endif /* CONFIG_TCC_DEBUG_ENV */

#endif /* TCC_DBGENV_H */
