/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2001-2004 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _TCC_H
#define _TCC_H

/* The Makefile also passes these on the command line, so a file that includes a
 * system header before tcc.h gets the same declarations (and a unity group the
 * same headers whichever member includes them first); see gen_unity.py. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
/* gnu headers use to #define __attribute__ to empty for non-gcc compilers */
#ifdef __TINYC__
#undef __attribute__
/* TCC does not provide these as true builtins when self-compiling */
static inline int __builtin_ctz(unsigned int x)
{
  int n = 0;
  if (x == 0)
    return 32;
  while (!(x & 1))
  {
    n++;
    x >>= 1;
  }
  return n;
}
static inline int __builtin_popcount(unsigned int x)
{
  int c = 0;
  while (x)
  {
    c += x & 1;
    x >>= 1;
  }
  return c;
}
#endif
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <setjmp.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <sys/time.h>
#include <unistd.h>
#ifndef CONFIG_TCC_STATIC
#include <dlfcn.h>
#endif
/* XXX: need to define this to use them in non ISOC99 context */
extern float strtof(const char *__nptr, char **__endptr);
extern long double strtold(const char *__nptr, char **__endptr);
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

#ifdef _WIN32
static inline unsigned tcc_getclock_ms(void)
{
  return GetTickCount();
}
#else
static inline unsigned tcc_getclock_ms(void)
{
  struct timeval tv;
  if (0 == gettimeofday(&tv, NULL))
    return tv.tv_sec * 1000 + (tv.tv_usec + 500) / 1000;
  return (unsigned)time(NULL) * 1000;
}
#endif

/* Microsecond variant, for phases that are a few milliseconds long and would
   otherwise be rounded to 0 or 1. Wraps every ~71 minutes, which no phase
   outlives; differences are taken immediately. */
static inline unsigned tcc_getclock_us(void)
{
  struct timeval tv;
  if (0 == gettimeofday(&tv, NULL))
    return (unsigned)(tv.tv_sec * 1000000 + tv.tv_usec);
  return 0;
}

#ifndef offsetof
/* The builtin, not &((type *)0)->field: that member access through a null
 * pointer is undefined, and UBSan reports it on every warning switch and
 * short-Sym allocation, burying real hits.  tccdefs.h gives tcc itself a
 * __builtin_offsetof. */
#define offsetof(type, field) __builtin_offsetof(type, field)
#endif

#ifndef countof
#define countof(tab) (sizeof(tab) / sizeof((tab)[0]))
#endif

#ifdef _MSC_VER
#define NORETURN __declspec(noreturn)
#define ALIGNED(x) __declspec(align(x))
#define PRINTF_LIKE(x, y)
#define HOT
#else
#define NORETURN __attribute__((noreturn))
#define ALIGNED(x) __attribute__((aligned(x)))
#define PRINTF_LIKE(x, y) __attribute__((format(printf, (x), (y))))
#define HOT __attribute__((hot))
#endif

#ifdef _WIN32
#define IS_DIRSEP(c) (c == '/' || c == '\\')
#define IS_ABSPATH(p) (IS_DIRSEP(p[0]) || (p[0] && p[1] == ':' && IS_DIRSEP(p[2])))
#define PATHCMP stricmp
#define PATHSEP ";"
#else
#define IS_DIRSEP(c) (c == '/')
#define IS_ABSPATH(p) IS_DIRSEP(p[0])
#define PATHCMP strcmp
#define PATHSEP ":"
#endif

#define LDOUBLE_SIZE 8

/* Target uses 8-byte long double (same as double).
 * This must be set whenever LDOUBLE_SIZE == sizeof(double) so that
 * constant folding code stores long double values as doubles, avoiding
 * host/target long double size mismatches during cross-compilation. */
#ifndef TCC_USING_DOUBLE_FOR_LDOUBLE
#define TCC_USING_DOUBLE_FOR_LDOUBLE 1
#endif

/* -------------------------------------------- */

/* Every include below names its module directory in full, and must keep doing
 * so.  These headers used to sit beside tcc.h in the repo root, where a quoted
 * #include resolves relative to the including file and therefore always found
 * tinycc's own copy.  Now that they live in their modules, a bare "elf.h" falls
 * through to the -I list instead -- and the native YasOS bootstrap compiles
 * with the target sysroot on that list, ahead of the tinycc paths, where a
 * different elf.h lives.  That shadowed the ARM relocation constants and broke
 * the device build with "'R_ARM_MOVT_ABS' undeclared" while the host build
 * stayed green.  Spelling the module path out makes the resolution independent
 * of -I order; do not "simplify" these back to basenames. */

/* Unified logging — see log.h for all scope switches */
#include "source/support/log.h"

/* getenv-driven debug/bisect knobs (TCC_DISABLE_PASS, DBG_CLINL, ...) and the
 * CONFIG_TCC_DEBUG_ENV switch that compiles them out of a release build */
#include "source/support/tccdbgenv.h"

/* Legacy aliases — will be removed once all callers migrate */
#define TCC_MACH_DBG(...) LOG_MACH(__VA_ARGS__)

/* target selection */
/* #define TCC_TARGET_I386   */    /* i386 code generator */
/* #define TCC_TARGET_X86_64 */    /* x86-64 code generator */
/* #define TCC_TARGET_ARM    */    /* ARMv4 code generator */
/* #define TCC_TARGET_ARM_THUMB */ /* ARMvX-m code generator */
/* #define TCC_TARGET_ARM64  */    /* ARMv8 code generator */
/* #define TCC_TARGET_C67    */    /* TMS320C67xx code generator */
/* #define TCC_TARGET_RISCV64 */   /* risc-v code generator */

/* default target is I386 */
#if !defined(TCC_TARGET_I386) && !defined(TCC_TARGET_ARM) && !defined(TCC_TARGET_ARM64) && !defined(TCC_TARGET_C67) && \
    !defined(TCC_TARGET_X86_64) && !defined(TCC_TARGET_RISCV64) && !defined(TCC_TARGET_ARM_THUMB)
#if defined __x86_64__
#define TCC_TARGET_X86_64
#elif defined __arm__
#define TCC_TARGET_ARM
#define TCC_ARM_EABI
#define TCC_ARM_VFP
#define TCC_ARM_HARDFLOAT
#elif defined __aarch64__
#define TCC_TARGET_ARM64
#elif defined __riscv
#define TCC_TARGET_RISCV64
#else
#define TCC_TARGET_I386
#endif
#ifdef _WIN32
#define TCC_TARGET_PE 1
#endif
#ifdef __APPLE__
#define TCC_TARGET_MACHO 1
#endif
#endif

#ifdef TARGETOS_YasOS
#define TCC_TARGET_YASOS 1
#endif

/* YAFF output format support is always available */
#define TCC_TARGET_YAFF 1

/* only native compiler supports -run */
#if defined _WIN32 == defined TCC_TARGET_PE && defined __APPLE__ == defined TCC_TARGET_MACHO
#if defined __i386__ && defined TCC_TARGET_I386 && !defined TCC_IS_NATIVE
#define TCC_IS_NATIVE
#elif defined __x86_64__ && defined TCC_TARGET_X86_64 && !defined TCC_IS_NATIVE
#define TCC_IS_NATIVE
#elif defined __arm__ && defined TCC_TARGET_ARM && !defined TCC_IS_NATIVE
#define TCC_IS_NATIVE
#elif defined __aarch64__ && defined TCC_TARGET_ARM64 && !defined TCC_IS_NATIVE
#define TCC_IS_NATIVE
#elif defined __riscv && defined __LP64__ && defined TCC_TARGET_RISCV64 && !defined TCC_IS_NATIVE
#define TCC_IS_NATIVE
#endif
#endif

#if defined CONFIG_TCC_BACKTRACE && CONFIG_TCC_BACKTRACE == 0
#undef CONFIG_TCC_BACKTRACE
#else
#define CONFIG_TCC_BACKTRACE 1 /* enable builtin stack backtraces */
#endif

/* `-b` on the armv8m cross, for QEMU runs: config.mak's CFLAGS say
   -DCONFIG_TCC_BCHECK=0 for every stage (build_rootfs.sh), so the Makefile
   adds -DCONFIG_TCC_BCHECK_CROSS=1 to the cross compiler alone (configure
   --disable-bcheck-cross drops it).  The device compiler never has it: the
   runtime's memory cost rules the board out. */
#if defined CONFIG_TCC_BCHECK_CROSS && CONFIG_TCC_BCHECK_CROSS && !defined TCC_IS_NATIVE
#undef CONFIG_TCC_BCHECK
#define CONFIG_TCC_BCHECK 1
#elif defined CONFIG_TCC_BCHECK && CONFIG_TCC_BCHECK == 0
#undef CONFIG_TCC_BCHECK
#else
#define CONFIG_TCC_BCHECK 1 /* enable bound checking code */
#endif

#if defined CONFIG_NEW_MACHO && CONFIG_NEW_MACHO == 0
#undef CONFIG_NEW_MACHO
#else
#define CONFIG_NEW_MACHO 1 /* enable new macho code */
#endif

#if defined TARGETOS_OpenBSD || defined TARGETOS_FreeBSD || defined TARGETOS_NetBSD || defined TARGETOS_FreeBSD_kernel
#define TARGETOS_BSD 1
#elif !(defined TCC_TARGET_PE || defined TCC_TARGET_MACHO)
#define TARGETOS_Linux 1 /* for tccdefs_.h */
#endif

#if defined TCC_TARGET_PE || defined TCC_TARGET_MACHO
#define ELF_OBJ_ONLY /* create elf .o but native executables */
#endif

/* No ten-byte long doubles on window and macos except in
   cross-compilers made by a mingw-GCC */
#if defined TCC_TARGET_PE || (defined TCC_TARGET_MACHO && defined TCC_TARGET_ARM64) ||                                 \
    (defined _WIN32 && !defined __GNUC__)
#define TCC_USING_DOUBLE_FOR_LDOUBLE 1
#endif

#ifdef CONFIG_TCC_PIE
#ifndef CONFIG_TCC_PIC
#define CONFIG_TCC_PIC 0
#endif
#endif

/* support using libtcc from threads */
#ifndef CONFIG_TCC_SEMLOCK
#define CONFIG_TCC_SEMLOCK 1
#endif

/* ------------ path configuration ------------ */

#ifndef CONFIG_SYSROOT
#define CONFIG_SYSROOT ""
#endif
#if !defined CONFIG_TCCDIR && !defined _WIN32
#define CONFIG_TCCDIR "/usr/local/lib/tcc"
#endif
#ifndef CONFIG_LDDIR
#define CONFIG_LDDIR "lib"
#endif
#ifdef CONFIG_TRIPLET
#define USE_TRIPLET(s) s "/" CONFIG_TRIPLET
#define ALSO_TRIPLET(s) USE_TRIPLET(s) ":" s
#else
#define USE_TRIPLET(s) s
#define ALSO_TRIPLET(s) s
#endif

/* path to find crt1.o, crti.o and crtn.o */
#ifndef CONFIG_TCC_CRTPREFIX
#define CONFIG_TCC_CRTPREFIX USE_TRIPLET(CONFIG_SYSROOT "/usr/" CONFIG_LDDIR)
#endif

#ifndef CONFIG_USR_INCLUDE
#define CONFIG_USR_INCLUDE "/usr/include"
#endif

/* Below: {B} is substituted by CONFIG_TCCDIR (rsp. -B option) */

/* system include paths */
#ifndef CONFIG_TCC_SYSINCLUDEPATHS
#if defined TCC_TARGET_PE || defined _WIN32
#define CONFIG_TCC_SYSINCLUDEPATHS "{B}/include" PATHSEP "{B}/include/winapi"
#else
#define CONFIG_TCC_SYSINCLUDEPATHS                                                                                     \
  "{B}/include"                                                                                                        \
  ":" ALSO_TRIPLET(CONFIG_SYSROOT "/usr/local/include") ":" ALSO_TRIPLET(CONFIG_SYSROOT CONFIG_USR_INCLUDE)
#endif
#endif

/* library search paths */
#ifndef CONFIG_TCC_LIBPATHS
#if defined TARGETOS_YasOS
/* /usr/lib first, {B} (= /usr/lib/tcc) after it.
 *
 * On YasOS every shared library -- libc, libm, librp2350fp -- lives in
 * /usr/lib, and {B} holds only libtcc1.a, which is reached by its own path
 * rather than by a -l search. With the generic order below, every compile
 * probed {B}/lib<x>.so three times and missed three times, and a miss is not
 * free: the VFS answers a failed lookup by walking the path component by
 * component looking for a symlink, so each one cost ~1 ms against ~0 ms for
 * the hit that followed.
 *
 * Note that ./configure's --libpaths cannot express this: it writes the value
 * into config.h inside `#if !(TCC_TARGET_...)`, and this compiler is built
 * with TCC_TARGET_ARM defined, so the configured value is discarded and the
 * default below is what ships. */
#define CONFIG_TCC_LIBPATHS                                                                                            \
  ALSO_TRIPLET(CONFIG_SYSROOT "/usr/" CONFIG_LDDIR)                                                                    \
  ":{B}"                                                                                                               \
  ":" ALSO_TRIPLET(CONFIG_SYSROOT "/" CONFIG_LDDIR)
#elif defined TCC_TARGET_PE || defined _WIN32
#define CONFIG_TCC_LIBPATHS "{B}/lib"
#else
#define CONFIG_TCC_LIBPATHS                                                                                            \
  "{B}"                                                                                                                \
  ":" ALSO_TRIPLET(CONFIG_SYSROOT "/usr/" CONFIG_LDDIR) ":" ALSO_TRIPLET(                                              \
      CONFIG_SYSROOT "/" CONFIG_LDDIR) ":" ALSO_TRIPLET(CONFIG_SYSROOT "/usr/local/" CONFIG_LDDIR)
#endif
#endif

/* name of ELF interpreter */
#ifndef CONFIG_TCC_ELFINTERP
#if defined __GNU__
#define CONFIG_TCC_ELFINTERP "/lib/ld.so"
#elif defined(TCC_TARGET_PE)
#define CONFIG_TCC_ELFINTERP "-"
#elif defined TCC_TARGET_ARM64
#define CONFIG_TCC_ELFINTERP "/lib/ld-linux-aarch64.so.1"
#elif defined(TCC_TARGET_X86_64)
#define CONFIG_TCC_ELFINTERP "/lib64/ld-linux-x86-64.so.2"
#elif defined(TCC_TARGET_RISCV64)
#define CONFIG_TCC_ELFINTERP "/lib/ld-linux-riscv64-lp64d.so.1"
#elif defined(TCC_ARM_EABI)
#define DEFAULT_ELFINTERP(s) default_elfinterp(s)
#else
#define CONFIG_TCC_ELFINTERP "/lib/ld-linux.so.2"
#endif
#endif

/* var elf_interp dans *-gen.c */
#ifndef DEFAULT_ELFINTERP
#define DEFAULT_ELFINTERP(s) CONFIG_TCC_ELFINTERP
#endif

/* (target specific) libtcc1.a */
#ifndef TCC_LIBTCC1
#define TCC_LIBTCC1 "libtcc1.a"
#endif

/* library to use with CONFIG_USE_LIBGCC instead of libtcc1.a */
#if defined CONFIG_USE_LIBGCC && !defined TCC_LIBGCC
#define TCC_LIBGCC USE_TRIPLET(CONFIG_SYSROOT "/" CONFIG_LDDIR) "/libgcc_s.so.1"
#endif

/* <cross-prefix-to->libtcc1.a */
#ifndef CONFIG_TCC_CROSSPREFIX
#define CONFIG_TCC_CROSSPREFIX ""
#endif

/* -------------------------------------------- */

#include "source/obj/dwarf.h"
#include "source/driver/libtcc.h"
#include "source/obj/stab.h"
#include "tcctypes.h"

/* -------------------------------------------- */

#ifndef PUB_FUNC /* functions used by tcc.c but not in libtcc.h */
#define PUB_FUNC
#endif

/* A pointer in a constant table of strings: an offset into .rodata when tcc
   builds tcc for the target, so the table stays in shared flash instead of a
   relocated per-process copy (docs/rodata_relative.md).  Plain elsewhere. */
#ifndef TCC_RODATA_REL
#ifdef __TINYC_RODATA_RELATIVE__
#define TCC_RODATA_REL __rodata_relative
#else
#define TCC_RODATA_REL
#endif
#endif

/* Always compile from separate objects */
#define ST_INLN
#define ST_FUNC
#define ST_DATA extern

/* Target-specific definitions (after ST_FUNC is defined) */
#if defined(TCC_TARGET_ARM_THUMB)
#include "source/backend/arch/arm/thumb/arm-thumb-defs.h"
#endif

#ifdef TCC_PROFILE /* profile all functions */
#define static
#define inline
#endif

/* Call ABI assignment query (types; target hook prototype is later). */
#include "source/machine/tccabi.h"

/* -------------------------------------------- */
/* Forward declarations needed by target includes */
typedef struct Sym Sym;

/* include the target specific definitions */

/* -------------------------------------------- */

#if PTR_SIZE == 8 && !defined TCC_TARGET_PE
#define LONG_SIZE 8
#else
#define LONG_SIZE 4
#endif

/* -------------------------------------------- */

/* CONFIG_TCC_LOW_MEM (./configure --enable-low-mem): size the compiler's fixed
 * tables and first arenas for a part whose processes live in a few hundred KB
 * of SRAM (the Pico 2, no PSRAM).  Growable structures start small and grow in
 * equal steps; fixed ones are just smaller.  CONFIG_TCC_O0_ONLY is separate: a
 * board with PSRAM can want a small compiler without these limits. */

#define INCLUDE_STACK_SIZE 32
#define IFDEF_STACK_SIZE 64
#ifdef CONFIG_TCC_LOW_MEM
/* Not growable: code all over the frontend holds SValue pointers into the
 * stack across a push, so it cannot move.  128 still takes a 100-argument
 * call; deeper expressions stop with the same clean error. */
#define VSTACK_SIZE 128
#else
#define VSTACK_SIZE 256 /* YASOS: 512 was an upstream bump for the yarpgen fuzzer's
                           pathological expressions; 256 fits real code and saves
                           ~10 KiB .bss (513->257 * 40 B SValue). Clean tcc_error on overflow. */
#endif
#define STRING_MAX_SIZE 1024
#define TOKSTR_MAX_SIZE 256
/* Nested auto-inline expansion: how deep, and how many body tokens one
 * function may take in through expansions below the first level.  A body of
 * at most TINY_INLINE_TOKENS is smaller than the call it replaces and costs
 * nothing from the budget.  24 was too few for that: zig.h's 128-bit helpers
 * (zig_shr_u128, 92 tokens; the truncates, 20-48) take their zig_u128 by value
 * -- 16 bytes each way through registers and the stack -- and fold to a few
 * instructions once the constant shift or width arrives, yet a large caller
 * spent the budget before reaching them (Wyhash.hash kept 8 calls to
 * zig_u64_truncate_u64 alone).  At 96 the Zig compiler's .text shrinks 55 KB
 * and it runs 3.3% fewer instructions; 128-256 change nothing more, and the
 * 2441-file corpus is byte-identical at -O1/-O2 (no smaller caller runs out). */
#define INLINE_NEST_MAX 32
#define NESTED_INLINE_BUDGET 4096
#define TINY_INLINE_TOKENS 96
/* A leaf this small expands at every site, whatever its token length. */
#define SMALL_LEAF_INLINE_OPS 24
#define STATIC_INLINE_LEAF_TOKENS 512
/* inline_body_leaf_ops counts a third over what the body compiles to: over the 567
 * leaves of tinycc's own units, bodies estimated at 28-31 compile to 21 ops or so. */
#define STATIC_INLINE_LEAF_EST_OPS 32
#define PACK_STACK_SIZE 8
#define VISIBILITY_STACK_SIZE 8

#ifdef CONFIG_TCC_LOW_MEM
#define TOK_HASH_SIZE 1024 /* must be a power of two */
#else
#define TOK_HASH_SIZE 2048 /* must be a power of two. YASOS: 4096 -> 2048 saves 8 KiB
                              .bss; device compiles intern few-hundred symbols so the
                              table stays sparse (lazy-lib interning keeps it sparser). */
#endif
/* Identifier hash: shared by tccpp.c and the build-time keyword index
   (gen_kw_index.c), which must agree on it. */
#define TOK_HASH_INIT 1
#define TOK_HASH_FUNC(h, c) ((h) + ((h) << 5) + ((h) >> 27) + (c))
#define KW_HASH_SIZE 1024 /* keyword index buckets; power of two, > NB_BUILTIN_TOKS */
#define PREDEF_HASH_SIZE 256 /* predefine name index buckets; power of two, > predefines */
#define TOK_ALLOC_INCR 256 /* must be a power of two */
#define TOK_MAX_SIZE 4     /* token max size in int unit when stored in string */

/* token symbol management */
typedef struct TokenSym
{
  struct TokenSym *hash_next;
  /* A macro of this name is in a small map instead (define_find; has_define
   * says there is one), as struct tags and labels are: a Zig kernel compile
   * interns 13K names and defines a few hundred. */
  struct Sym *sym_identifier; /* direct pointer to identifier */
  /* Struct tags and labels are bound through two small maps instead
     (sym_tag_head / sym_label_head): few names are ever either. */
  int tok;                    /* token number */
  int len : 31;
  unsigned has_define : 1;
  /* The name, NUL-terminated, in a name chunk (tccpp.c).  Once the TU is
   * parsed, with the internal spill on, the chunks go to the spill file and
   * this holds the name's offset there, tagged odd: read it with tok_name. */
  const char *str;
} TokenSym;

/* One predefined macro in the build-time table (tccdefs_table_.h, produced by
   gen_predef_table.c from include/tccdefs.h).  The table exists so the ~97
   predefines a target actually has do not have to be lexed and defined on
   every invocation: on the RP2350 that phase measured 17.2 ms, 83% of it the
   first walk through the define path, for macros 89% of translation units
   never name.  Entries are materialised on demand -- see the predefine index
   in tccpp.c.

   `flags` carries the two conditions that cannot be resolved at build time
   because they depend on runtime options rather than on the target. */
#define TCC_PREDEF_C11 1            /* only when cversion >= 201112 (-std=) */
#define TCC_PREDEF_LEADING_US 2     /* only with -fleading-underscore */
#define TCC_PREDEF_NO_LEADING_US 4  /* only without it */

/* CONFIG_TCC_PREDEF_TABLE=0 restores the eager arm -- the whole tccdefs text
   is lexed through the <command line> buffer on every invocation -- and is the
   byte-identity A/B seam for the lazy path. Compile-time rather than an env
   knob because build_rootfs.sh compiles the getenv knobs out of the device
   compiler, which is what made the TCC_NO_PROGRAMMATIC_DECLS seam inert. */
#ifndef CONFIG_TCC_PREDEF_TABLE
#define CONFIG_TCC_PREDEF_TABLE 1
#endif

typedef struct TCCPredefMacro
{
  /* Offset into tcc_predef_strs[] (generated, like everything here, by
     gen_predef_table.c; offsets instead of pointers keep the tables out of
     RAM on YasOS): the name incl. the parameter list for function-like
     macros, and the length of the bare name before it. */
  unsigned short name;
  unsigned char name_len;
  unsigned char flags;
  /* Span into tcc_predef_toks[]: the body pre-tokenised, so materialising a
     macro never has to re-enter the lexer. */
  unsigned short tok_off;
  unsigned short tok_count;
} TCCPredefMacro;

#ifdef TCC_TARGET_PE
typedef unsigned short nwchar_t;
#else
typedef int nwchar_t;
#endif

typedef struct CString
{
  int size; /* size in bytes */
  int size_allocated;
  char *data; /* nwchar_t* in cases */
} CString;

/* type definition */
typedef struct CType
{
  int t;
  struct Sym *ref;
} CType;

/* constant value */
typedef union CValue
{
  long double ld;
  double d;
  float f;
  uint64_t i;
  struct
  {
    char *data;
    int size;
  } str;
  int tab[LDOUBLE_SIZE / 4];
  /* _Complex double constants pack {real, imag} as two doubles at offsets
   * 0 and 8 (see TOK_CDOUBLE_I in unary() and the many `(char *)&...c + 8`
   * accesses).  On 64-bit hosts `ld` already makes the union 16 bytes, but
   * on a 32-bit host (long double == double) the union would otherwise be
   * 8 bytes and every imag access would be out of bounds — complex double
   * constants silently lost their imaginary half when tcc ran on-target. */
  double cplx[2];
} CValue;

/* value on stack */
/* Temp local variable index encoded in vr field: vr = -2 - index (0..7)
 * This allows tracking which temp local slot an SValue uses without a separate field.
 * vr = -1 remains the sentinel for "no virtual register". */
#define VR_TEMP_LOCAL(idx) (-2 - (idx))
#define VR_IS_TEMP_LOCAL(vr) ((vr) <= -2 && (vr) >= -9)
#define VR_TEMP_LOCAL_IDX(vr) (-2 - (vr))

#include "source/frontend/svalue.h"

// _Static_assert(sizeof(SValue) == 40, "SValue size changed");

/* symbol attributes */
struct SymAttr
{
  unsigned aligned : 5, /* alignment as log2+1 (0 == unspecified) */
      packed : 1, weak : 1, visibility : 2, dllexport : 1,
#ifdef TCC_TARGET_PE
      nodecorate : 1, /* PE only: a bit of the 32 this target needs */
#endif
      dllimport : 1, addrtaken : 1, nodebug : 1,
      naked : 1, nested_func : 1, /* nested function flag */
      sso_be : 1,                 /* scalar_storage_order("big-endian") */
      transparent_union : 1,      /* __attribute__((transparent_union)) */
      possibly_written : 1,       /* (a local pointer variable: stored with a
                                     value that is not an allocation's result,
                                     builtin/objsize.c)
                                     global may have been written after its
                                     initializer (a store was emitted, or its
                                     address escaped to a non-const pointer).
                                     Used by inline-eval to decide whether
                                     `*&g` can fold to the initializer. */
      tu_no_readers : 1,          /* end-of-TU analysis confirmed no reachable
                                     function reads this static global.  Set
                                     by tcc_ir_tu_analyze_dead_statics; read
                                     by dead-static-store-elim during the
                                     end-of-TU late_reopt phase. */
      param_volatile : 1,         /* original parameter declaration was volatile
                                     before function-type normalization stripped
                                     top-level qualifiers. */
      tentative : 1,              /* file-scope object declared without an
                                     initializer, whose definition may still
                                     follow: its bytes are not its value yet.
                                     Cleared by the definition or at the end
                                     of the TU (finalize_tentative_definitions). */
      used : 1,                   /* __attribute__((used)): emitted even when
                                     nothing in the TU refers to it. */
      vis_explicit : 1,           /* visibility came from an attribute or a
                                     #pragma GCC visibility, so -fvisibility=
                                     does not apply (visibility("default")
                                     is STV_DEFAULT == 0, like no attribute). */
      tu_unused : 1,              /* a static nothing live in the TU refers to
                                     (prune_unused_statics): never emitted. */
      guard_head : 1,             /* has a tcc_state->guard_heads entry */
      regpair_ret : 2,            /* REGPAIR_RET_*: an internal function's
                                     8-byte aggregate comes back in r0/r1
                                     (gfunc_regpair_ret), decided once. */
      proto_own : 1,              /* a function type Sym a file-scope prototype's
                                     declarator built for that one function: no
                                     typedef, typeof or other declaration holds
                                     it, so its definition's type is adopted into
                                     it (patch_type, sym_functype_adopt). */
      nonnull : 1,                /* a parameter that __attribute__((nonnull)) names:
                                     a null argument is a -Wnonnull warning */
      param_array : 1,            /* a parameter declared as an array, now a
                                     pointer: sizeof of it (-Wsizeof-array-argument) */
      pure_via_sret : 1,          /* inferred: the function's only observable
                                     effect is its aggregate result (*sret_arg
                                     writes, or r0/r1).  In SymAttr, not
                                     FuncAttr: a global Sym's f shares its
                                     storage with sym_scope. */
      attr_facts : 1;             /* the Sym has facts in its SymLocalFacts that
                                     say something without a pass asking:
                                     deprecated, format, warn_unused_result or
                                     nodiscard (decl/attribute.c
                                     attr_apply_decl_facts), the SYMF_* flags
                                     (sym_flag_set).  Such a prototype is never
                                     parked. */
};
/* SymAttr is full: its 32 bits are a _Static_assert in symtab.c, and Sym
   (48 bytes on the device) must not grow.  A new per-symbol fact goes to
   SymLocalFacts, the side table keyed by the Sym: a value as a field there,
   a 1-bit attribute as a SYMF_* flag (sym_flag_set / sym_flag).  Testing a
   flag costs one bit when the Sym has no facts (attr_facts).  The table is
   keyed by the Sym's address: a copy of a Sym (an inline expansion's
   parameters, the IR's) does not carry it -- copy the flag where the copy is
   made, as the SymAttr bits are copied. */
#define REGPAIR_RET_UNKNOWN 0
#define REGPAIR_RET_NO 1
#define REGPAIR_RET_YES 2

/* function attributes or temporary attributes for parsing */
struct FuncAttr
{
  unsigned func_call : 3,               /* calling convention (0..5), see below */
      func_type : 2,                    /* FUNC_OLD/NEW/ELLIPSIS */
      func_noreturn : 1,                /* attribute((noreturn)) */
      func_ctor : 1,                    /* attribute((constructor)) */
      func_dtor : 1,                    /* attribute((destructor)) */
#ifdef TCC_TARGET_PE
      func_args : 8,                    /* PE __stdcall args */
#else
      func_args : 1,                    /* PE __stdcall args: only PE reads it (the @N
                                           decoration), elsewhere it is just written and
                                           merged.  One bit, not eight: FuncAttr is 37
                                           bits (two words) and every bit more is on
                                           every Sym, whose c/f union it widens. */
#endif
      func_alwinl : 1,                  /* always_inline */
      func_noinline : 1,                /* noinline — suppress auto-inline */
      func_pure : 1,                    /* attribute((pure)) - no side effects, reads memory */
      func_const : 1,                   /* attribute((const)) - no side effects, no memory reads */
      func_no_instrument : 1,           /* attribute((no_instrument_function)) */
      func_va_arg_pack : 1,             /* uses __builtin_va_arg_pack() */
      func_rewritten_extern_inline : 1, /* extern inline rewritten to non-extern inline-only def */
      func_outofline_needed : 1,        /* always_inline call could not stay call-site-only */
      func_auto_inline : 1,             /* compiler-selected auto-inline candidate (small func) */
      func_icf_folded : 1,              /* body dropped: the symbol points at an identical one (tcc_icf_try_fold) */
      func_deferred_inline : 1,         /* auto-inline candidate that was NOT compiled at definition time:
                                           a `static inline` whose body gen_inline_functions still owes the
                                           TU.  Emit it there iff some call site fell back to a real call
                                           (sym->c set); without this bit that emission is skipped and a
                                           declined call site links against nothing. */
      func_inline_call_heavy : 1,       /* auto-inline body keeps a non-foldable call: budget-limit expansions */
      func_eval_only_inline : 1,        /* body saved for const-fold only, not regular inlining */
      func_const_arg_loop : 1,          /* post-opt body is a register-only loop: keep tokens so an
                                           all-constant call site can inline it and let
                                           ssa:loop_const_sim fold the loop to its final value */
      func_late_reopt : 1,              /* body kept for end-of-TU re-optimization (non-const static global fold) */
      tu_static_writer : 1,             /* function writes >=1 non-const static global (set during summary collection) */
      tu_reachable : 1,                 /* function is reachable from a TU root (non-static or addr-taken) per call graph */
      func_compiled : 1,                /* gen_function has completed for this sym at least once — distinguishes
                                           forward-declared-not-yet-defined functions from already-emitted ones,
                                           used by late_reopt triggering for inter-procedural noreturn propagation */
      func_keep_tokens_for_noreturn : 1, /* tokens preserved so end-of-TU noreturn propagation can decide whether to
                                            re-emit — separate from func_late_reopt so we don't trigger unnecessary
                                            re-emit before we know if any callee turned out to be noreturn */
      func_called_once : 1,             /* static, one call site in the TU, address never taken: that site inlines it
                                            whatever its size (-finline-functions-called-once) */
      func_inl_wrapper : 1,             /* static acyclic forwarder of a few ops around one call: inlined at most
                                            INLINE_WRAPPER_PER_CALLER times into one function (gen_function) */
      func_inl_sret_loop : 1,           /* small aggregate-return loop, bounded per caller */
      func_inl_few_callers : 1,         /* static leaf with a few call sites, each one inlines it: the TU
                                            grows by (sites - 1) bodies (gen_function, few_callers) */
      func_small_leaf : 1,              /* static leaf whose optimized body is a few straight-line ops: inlined
                                            whatever its token length (see gen_function's post-opt promotion) */
      func_leaf : 1,                    /* attribute((leaf)): never calls back into this unit, so it
                                           cannot touch its static globals (-Wlifetime keeps their paths) */
      func_sys_decl : 1,                /* declared in a system header (libc): it writes the program's
                                           globals only through its arguments (-Wlifetime) */
      func_ret_zext : 2;                /* r0 at every return is zero-extended from 1/8/16 bits (1/2/3), as
                                           ra:known_ext proved it of the emitted body; a direct call to the
                                           function then needs no extension of its result, and a recompile
                                           keeps the promise (gfunc_return masks the value).  See known_ext.c */
};

/* symbol management */
/* The fields a macro uses come first: macros, their parameters and the
   expander's nesting list are allocated only that far (SYM_SHORT_SIZE,
   sym_push2_short) -- 28 of 44 bytes on ARM.  r fills the hole after v where
   pointers are 8 bytes, and follows prev where they are 4. */
struct Sym
{
  int v; /* symbol token */
#if defined __SIZEOF_POINTER__ && __SIZEOF_POINTER__ == 8
  unsigned short r; /* associated register or VT_CONST/VT_LOCAL and LVAL type */
#endif
  CType type; /* associated type */
  union
  {
    struct
    {
      int c; /* associated number or Elf symbol index */
      union
      {
        int sym_scope;     /* scope level for locals */
        int jnext;         /* next jump label */
        int jind;          /* label position */
        struct FuncAttr f; /* function attributes */
        int auxtype;       /* bitfield access type */
      };
    };
    /* enum constant if IS_ENUM_VAL, a long long read and written through
       sym_enum_val / sym_set_enum_val: as two ints it does not make Sym
       8-byte aligned on ARM, which would pad it from 44 to 48 bytes. */
    int enum_val_w[2];
    int *d; /* define token stream */
    struct Sym *cleanup_func;
    struct TokenString *defer_str; /* a yasc defer's body (CLEANUP_DEFER entries) */
  };
  union
  {
    struct Sym *next;         /* next related symbol (for fields and anoms) */
    int *e;                   /* expanded token stream */
    int asm_label;            /* associated asm label */
    struct Sym *cleanupstate; /* in defined labels */
    int *vla_array_str;       /* vla array code */
  };
  struct Sym *prev; /* prev symbol in stack */
  /* ---- a short Sym ends here ---- */
#if !(defined __SIZEOF_POINTER__ && __SIZEOF_POINTER__ == 8)
  unsigned short r;
#endif
  unsigned char yasc; /* YASC_*: on a pointer node, that pointer's qualifiers;
                         in the padding after r on 32-bit hosts */
  unsigned char ymod; /* yasc modules: YASC_MODULE_OWN, YASC_EXPORT (padding too) */
  struct SymAttr a; /* symbol attributes */
  int vreg;
  struct Sym *prev_tok; /* previous symbol for this token */
};
#define SYM_SHORT_SIZE (offsetof(struct Sym, prev) + sizeof(struct Sym *))

/* yasc ownership qualifiers.  Those of a pointer live in its pointer node
 * (type.ref, interned per pointee type and qualifiers by sym_pointer_node_q),
 * so they follow the type through typedefs, members, parameters and returns,
 * and compare_types, which compares pointee types, ignores them. */
#define YASC_OWNED 0x01 /* `T * owned p`: p owns its target, freed or handed on once */
#define YASC_MAYBE 0x02 /* `T * maybe p`: p may be null */
#define YASC_OUT 0x04   /* `out T *p`: the callee initialises *p */
#define YASC_DROP 0x08  /* `drop T *p`: the callee consumes *p */
#define YASC_VIEW 0x10  /* `T * view p`: p borrows its target, never frees it */
#define YASC_FRESH 0x20 /* a call's owned result, not yet taken by anything */
/* yasc errors (docs/plans/yasc_errors.md), on other Syms than pointer nodes: */
#define YASC_ERRSET 0x40   /* an error set's tag Sym (enum-like); also `error` alone */
#define YASC_ERRUNION 0x80 /* an error union's node: type is the payload; in an
                              AttributeDef, a `!` before the type */
/* On a pointer node (ERRSET's bit, which only goes on other Syms): the
   pointer was written in regular C -- a header, or a macro defined in one --
   in a unit that has yasc in it.  C says nothing about ownership, so such a
   pointer converts to and from owned / view ones without a diagnostic:
   checks start at the yasc variable it lands in (docs/yasc_modules.md,
   "Legacy C"). */
#define YASC_UNCHECKED 0x40
/* On a parameter's pointer node (ERRUNION's bit, which only goes on other
   Syms): the function's result takes it over -- `takes(p)` on the result */
#define YASC_TAKEN 0x80
/* yasc modules (decl/module.c), in Sym.ymod of a file-scope identifier Sym or
   a struct/union/enum tag Sym: declared by the module itself, and exported */
#define YASC_MODULE_OWN 0x01
#define YASC_EXPORT 0x02
#define YASC_HAS_METHODS 0x04 /* a struct tag with yasc methods (decl/method.c) */
#define YASC_SLICE_NODE 0x80  /* a slice type's node (expr/yasc_slices.c): its type the element */
/* -Wunused-* (frontend/gen/core/unused.c), in Sym.ymod of a variable, a
   parameter, a function or a label: set only while unused_on */
#define SYM_UNUSED_TRACK 0x08 /* reported if still unused when its scope (or the unit) ends */
#define SYM_UNUSED_REF 0x10   /* named somewhere: used, or at least assigned to */
#define SYM_UNUSED_READ 0x20  /* its value is read (or its address taken) */
#define SYM_UNUSED_OK 0x40    /* __attribute__((unused)) / [[maybe_unused]] */
/* unused.c */
ST_FUNC void unused_unit_begin(TCCState *s1);
ST_FUNC void unused_unit_end(TCCState *s1);
ST_FUNC void unused_unit_free(TCCState *s1);
ST_FUNC void unused_options_changed(TCCState *s1);
ST_FUNC int unused_locals_wanted(void);
ST_FUNC void unused_mention(Sym *s);
ST_FUNC void unused_scan_tokens(struct TokenString *str, Sym *self, int any);
ST_FUNC void unused_declare_local(Sym *s, int line, int attr_unused);
ST_FUNC void unused_declare_static(Sym *s, int line, int is_func);
ST_FUNC void unused_params_begin(Sym *func, Sym *params_end);
ST_FUNC void unused_param_line(int v, int line, int first, int sys);
ST_FUNC void unused_params_bind(Sym *func);
ST_FUNC int unused_param_decl_line(Sym *func, int v);
ST_FUNC int unused_param_decl_sys(Sym *func, int v);
ST_FUNC void unused_sym_popped(Sym *s);
ST_FUNC void unused_label_defined(Sym *s, int line, int attr_unused);
ST_FUNC void unused_alias_target(int tok_target);
ST_FUNC Sym *unused_lhs(void);
ST_FUNC void unused_stmt_expr_end(int effects_before, int line);
ST_FUNC void unused_stmt_expr_drop(void);
ST_FUNC void unused_declare_typedef(Sym *s, int line, int attr_unused);
ST_FUNC Sym *unused_lhs_assign(void);
ST_FUNC Sym *unused_part_begin(unsigned char *newly_read, int subscript);
ST_FUNC void unused_part_end(Sym *s, unsigned char newly_read, int subscript);
ST_FUNC void unused_assigned_to(Sym *lhs);
ST_FUNC void unused_label_popped(Sym *s);
/* -Wunused-value: what the expression parsed last did at its top */
#define UV_VALUE 1  /* computed a value: an operator, a cast, a member */
#define UV_EFFECT 2 /* a call, a statement expression, a builtin */
#define UV_ASSIGN 3 /* an assignment or ++/-- */
#define UV_COMMA 4  /* a comma operator (uv_comma_* tell about its right operand) */
#define UV_INCDEC 5 /* ++ or -- */
ST_DATA int uv_effects; /* side effects parsed so far: calls, stores, ++/--, ... */
ST_DATA int uv_last;    /* UV_*: the operation the parser completed last */
ST_DATA int uv_ident;   /* the name unary_primary() resolved last (not a builtin) */
ST_FUNC void unused_assigned(void);
ST_FUNC void unused_discard(int effects_before, int line, int comma_lhs);
ST_FUNC void unused_void_cast(void);
ST_FUNC void unused_comma_end(int effects_before_rhs, int rhs_last);
/* A yasc keyword followed by one of these was meant as a name (`int out;`,
   `f(char *view)`): say so instead of "identifier expected". */
#define YASC_NAME_FOLLOWS(t) ((t) == ';' || (t) == ',' || (t) == '=' || (t) == ')' || (t) == '[' || (t) == ':')
ST_FUNC void yasc_keyword_as_name(int kw);
/* yasc error unions (frontend/gen/expr/yasc_errors.c) */
ST_FUNC int type_is_errunion(CType *type);
ST_FUNC int type_is_slice(CType *type); /* yasc `[]T` (expr/yasc_slices.c) */
ST_FUNC void yasc_slice_misuse(void);
/* the section of the yasc error-name records, read by libtcc1's
   __yasc_error_name and checked by the linker */
#define YASC_ERRNAME_SECTION "yasc_errname"
ST_FUNC void yasc_errunion_misuse(void);
/* cheap pre-test for the checks on hot paths: an error union is an unsigned
 * long long with a type.ref */
#define MAYBE_ERRUNION(type) (((type)->t & VT_BTYPE) == VT_LLONG && (type)->ref)
/* a macro: the VT_* flags are defined further down */
/* yasc `int owned(close) maybe fd`: an int handle -- the int type's ref is a
 * handle node (v == SYM_HANDLE, interned by sym_handle_node) with the
 * qualifiers, and the deallocator in its vreg */
#define SYM_HANDLE (SYM_FIELD | 1)
#define type_is_handle(type)                                                                                           \
  (((type)->t & (VT_BTYPE | VT_ARRAY)) == VT_INT && (type)->ref && (type)->ref->v == SYM_HANDLE)
/* yasc `T * restrict view p`: an exclusive borrow (S3).  Beyond the yasc
 * byte: a pointer node keeps it in ymod (YASC_PMOD_RESTRICT, unused there),
 * and type_yasc / sym_pointer_node_q take the qualifiers as an int with it. */
#define YASC_RESTRICT 0x100
/* yasc `inout` / `in` before a parameter's type: in AttributeDef.yasc only,
   never on a node -- after the declaration a view parameter's target is
   const (in, the default) or not (inout) */
#define YASC_INOUT 0x200
#define YASC_IN 0x400
#define YASC_PMOD_RESTRICT 0x80
#define type_yasc(type)                                                                                                \
  ((((type)->t & (VT_BTYPE | VT_ARRAY)) == VT_PTR && (type)->ref)                                                     \
       ? ((type)->ref->yasc | (((type)->ref->ymod & YASC_PMOD_RESTRICT) ? YASC_RESTRICT : 0))                         \
   : type_is_handle(type) ? (type)->ref->yasc                                                                         \
                          : 0)
ST_FUNC Sym *sym_handle_node(int yasc, int dealloc);
/* yasc tagged unions (decl/struct.c): `union(k) { T m when(K); ... } u;` in a
 * struct.  vals: the tag values the members name, a bit each in a mask;
 * YASC_TAG_OTHER is any other value. */
#define YASC_TAG_MAXVALS 31
#define YASC_TAG_OTHER (1u << 31)
#define YASC_TAG_MAXOWN 8
typedef struct YascTagged
{
  Sym *u, *outer;        /* the union's tag Sym, the struct holding it */
  int tag_tok, name_tok; /* the tag member's name, the union member's */
  int tag_rel;           /* the tag's offset from the union's start */
  int resolved, nvals, nmem;
  int64_t vals[YASC_TAG_MAXVALS];
  int vnames[YASC_TAG_MAXVALS]; /* the constant's name (token) the value was written as, 0 */
  int *mtok;             /* [nmem] the members' names */
  uint32_t *mmask;       /* [nmem] the values that select each */
  int *mname;            /* [nmem] the first of them (an index in vals) */
  /* owned pointers (or handles) in its members: offset from the union,
     the values that make them live, a name for messages */
  int nown;
  int own_off[YASC_TAG_MAXOWN];
  uint32_t own_mask[YASC_TAG_MAXOWN];
  char own_name[YASC_TAG_MAXOWN][48];
} YascTagged;
ST_DATA YascTagged *yasc_tagged;
ST_DATA int yasc_ntagged;
ST_FUNC YascTagged *yasc_tagged_of(Sym *u);
ST_FUNC int yasc_tagged_member(const YascTagged *t, int tok);
ST_FUNC void yasc_tagged_free(void);
ST_FUNC void yasc_tb_pending_free(void); /* declarator.c: out parameters' borrows(...) names */
/* yasc register fields (decl/struct.c): `uint32_t irq:1 w1c;` */
#define YASC_REG_RO 1
#define YASC_REG_WO 2
#define YASC_REG_W1C 3
ST_FUNC int yasc_reg_access(Sym *st, int tok);
/* the w1c bits of an object of type T in the 32 bits from byte BASE, and up
   to 8 of their fields (bits, name) for messages */
ST_FUNC uint32_t yasc_reg_w1c_mask(CType *t, int base, uint32_t *fbits, int *ftok, int *nf);
ST_DATA int yasc_nreg;
ST_FUNC void yasc_requalify(CType *type, int yasc);

/* yasc `borrows(...)`: where a view points.  On a pointer node, in its vreg
 * (-1 on every other one), part of the node's identity: the positions of a
 * function's parameters its result points into, and static storage.  Only a
 * function's result type is read this way (its own, or a callee's); a copy
 * of the type elsewhere carries the bits along unread. */
#define YASC_BORROWS_STATIC (1 << 28)
#define YASC_BORROWS_ARGS ((1 << 28) - 1)
#define YASC_BORROWS_MAX_PARAMS 28
#define type_borrows(type)                                                                                             \
  ((((type)->t & (VT_BTYPE | VT_ARRAY)) == VT_PTR && (type)->ref && (type)->ref->vreg > 0 &&                          \
    (type)->ref->vreg < (1 << 29))                                                                                     \
       ? (type)->ref->vreg                                                                                             \
       : 0)
ST_FUNC Sym *sym_pointer_node_b(CType *type, int yasc, int borrows);
/* yasc `out T * borrows(s) * view p`: what the callee writes into *p points
 * into s -- the same bits, on the node of the pointer an out parameter points
 * to (a parameter's type; 0 for none).  While the parameter list is read the
 * node holds YASC_TBORROWS_PENDING | an index of the names written, resolved
 * once every parameter is known (declarator.c). */
#define YASC_TBORROWS_PENDING (1 << 30)
#define type_target_borrows(ct)                                                                                        \
  ((((ct)->t & (VT_BTYPE | VT_ARRAY)) == VT_PTR && (ct)->ref) ? type_borrows(&(ct)->ref->type) : 0)
/* yasc `T * owned(fclose)`: the deallocator's name (token), in an owned
 * pointer node's vreg; 0 for plain `owned` (not said) */
#define type_dealloc(type) ((type_yasc(type) & YASC_OWNED) && (type)->ref->vreg > 0 && (type)->ref->vreg < (1 << 29) ? (type)->ref->vreg : 0)
/* yasc `regex_t owned(regfree) * view preg`: a pointer to an object that is
 * owned as a whole -- its node's vreg is (1 << 29) | the deallocator */
#define YASC_OBJ_MARK (1 << 29)
#define type_pointee_owned(type)                                                                                       \
  ((((type)->t & (VT_BTYPE | VT_ARRAY)) == VT_PTR && (type)->ref && (type)->ref->vreg >= YASC_OBJ_MARK &&             \
    (type)->ref->vreg < (YASC_OBJ_MARK << 1))                                                                          \
       ? (type)->ref->vreg & (YASC_OBJ_MARK - 1)                                                                       \
       : 0)
/* yasc `T * borrows(buf) key` on a struct member: the member's pointer node
 * has vreg -2 - i, an index into this table (struct.c) */
#define YASC_FIELD_SRC_MAX 4
typedef struct YascFieldBorrows
{
  int self_tok, self_off;
  int nsrc;
  int src_tok[YASC_FIELD_SRC_MAX], src_off[YASC_FIELD_SRC_MAX], src_size[YASC_FIELD_SRC_MAX];
  int is_static;
} YascFieldBorrows;
#define type_borrows_field(type)                                                                                       \
  ((((type)->t & (VT_BTYPE | VT_ARRAY)) == VT_PTR && (type)->ref && (type)->ref->vreg <= -2) ? -2 - (type)->ref->vreg \
                                                                                              : -1)
ST_FUNC YascFieldBorrows *yasc_field_borrows(int i);
/* yasc `struct T borrows(text) { ... }`: a struct whose values borrow (struct.c) */
ST_FUNC int yasc_struct_params(Sym *s);              /* how many borrows(...) names it takes, 0: none */
ST_FUNC int yasc_struct_param(Sym *s, int tok);      /* the name's position, -1 */
ST_FUNC int yasc_borrowing_struct(CType *type);      /* a value of TYPE borrows */
ST_FUNC void yasc_struct_result_bind(int name, int code); /* function NAME's result: its sources (YASC_BORROWS_*) */
ST_FUNC int yasc_struct_result_code(int name);         /* -1: none recorded */
ST_DATA int yasc_sbind_pending, yasc_sbind_static, yasc_sbind_nb, yasc_sbind_names[4];
ST_DATA Sym yasc_static_bound; /* SValue.sym of a call result bound to static storage */
ST_DATA int yasc_borrows_field_ok, yasc_field_bnb, yasc_field_bstatic, yasc_field_bnames[28];

/* Internal spill: the parameter list of a file-scope prototype goes to the spill
 * file between declarations and comes back when something reads it
 * (sym_params_release in symtab.c).  A spilled function type Sym's next is
 * this sentinel, so every walk of a type's member or parameter list starts
 * with sym_first(ref), never ref->next.  A spilled list is never empty, so
 * `ref->next != NULL` stays a valid "has parameters" test. */
extern struct Sym sym_params_spilled;
struct Sym *sym_params_load(struct Sym *ref);
static inline struct Sym *sym_first(struct Sym *ref)
{
  return ref->next == &sym_params_spilled ? sym_params_load(ref) : ref->next;
}

/* A function's key in the TU's by-token analysis caches (purity, constant
 * result, switch snapshot): 0, never cached, for a GNU nested function, whose
 * name other functions of the TU (file-scope or nested) may share. */
static inline int func_cache_token(const struct Sym *s)
{
  return s->a.nested_func ? 0 : s->v;
}

static inline long long sym_enum_val(const struct Sym *s)
{
  long long v;
  memcpy(&v, s->enum_val_w, sizeof v);
  return v;
}

static inline void sym_set_enum_val(struct Sym *s, long long v)
{
  memcpy(s->enum_val_w, &v, sizeof v);
}

/* Facts a few local variables carry, kept out of Sym so the tens of thousands
 * of prototypes, fields and type nodes a TU creates do not pay for them (Sym
 * was 88 bytes on ARM, 44 without them; the symbol table is the largest part
 * of a compile's memory that lives for the whole TU).  Not even a pointer
 * stays in Sym: a side map keyed by the Sym holds them.  Read through
 * SYM_FACTS(s), which yields an all-zero record when s has none, or
 * sym_facts_peek(s), NULL then; write through sym_facts(s), which allocates
 * it.  Freed with the Sym (sym_free). */
/* C23's keywords (bool, true, false, nullptr, static_assert, alignas, ...):
   C23, or the yasc dialect, which is C23 -- also in a file that says
   #pragma yasc under -ccompat */
#define TCC_C23(s) ((s)->cversion >= 202311 || (s)->yasc)

/* access(mode, ...) modes, and how many access attributes a function keeps */
#define ATTR_ACCESS_NONE 0
#define ATTR_ACCESS_READ 1
#define ATTR_ACCESS_WRITE 2
#define ATTR_ACCESS_RW 3
#define FORTIFY_MAX_ACCESS 4

typedef struct SymLocalFacts
{
  unsigned long long objsize_max_value;    /* conservative max scalar value assigned locally */
  unsigned long long objsize_strlen_value; /* conservative max NUL-terminated string bytes */
  /* Captured constant initializer bytes for small local arrays/vectors.
   * Set by decl_initializer_alloc when all init values are compile-time
   * constants; invalidated by vstore when the variable is later written.
   * Used by __builtin_shuffle and similar intrinsics to fold runtime
   * mask loads into constant indices. */
  unsigned char *const_init_data;
  int const_init_size;
  uint32_t param_nocapture; /* compiled static function: borrowed parameter bits */
  struct Sym *knr_proto;    /* old-style definition: the prototype its calls take */
  /* The checked attributes (SymAttr.attr_facts): from the AttributeDef fields
     of the same names, merged over every declaration of the Sym. */
  int deprecated_msg;          /* a token, or 0 */
  const char *deprecated_file; /* where `deprecated` was said, for the note (interned) */
  int deprecated_line;
  int nodiscard_msg;           /* a token, or 0 */
  unsigned char deprecated;    /* -Wdeprecated-declarations at each use */
  unsigned char unused_result; /* ATTR_WUR / ATTR_NODISCARD: -Wunused-result */
  unsigned char fmt_kind, fmt_idx, fmt_first; /* format(): -Wformat */
  unsigned char fmt_arg;                      /* format_arg(n): the result is argument n's format */
  unsigned char tainted_result;                /* tainted_result: -Wtainted-format (ir/taint.c) */
  uint32_t tainted_args;                       /* tainted_arg(n), bit n-1 */
  /* -ffortify (builtin/fortify.c): AttributeDef's attr_cstr_args / attr_access */
  uint32_t fort_cstr;
  unsigned char fort_n;
  unsigned char fort_access[FORTIFY_MAX_ACCESS][3];
  unsigned char objsize_max_valid;
  unsigned char objsize_strlen_valid;
  unsigned char const_init_valid;
  unsigned char const_init_in_progress;
  /* A function's allocation attributes (builtin/objsize.c): alloc_size(a[, b])
     (from 1), malloc, malloc(dealloc, i); and, for a deallocator some
     malloc(dealloc, i) names, the argument it releases. */
  int alloc_dealloc;              /* the deallocator's token, or 0 */
  unsigned char alloc_size_a, alloc_size_b, alloc_dealloc_arg, alloc_malloc;
  unsigned char dealloc_arg;
  /* A local pointer variable's allocations (objsize.c): os_state 1 while
     every store to it so far was an allocation's result (or null). */
  unsigned char os_state;
  unsigned char os_watch;         /* a variable an allocation's size came from: */
  unsigned os_stores;             /* ... its stores, counted */
  struct ObjHeap
  {
    unsigned lo, hi;              /* the sizes it may have (when known) */
    int dealloc;                  /* the deallocator's token: 0 none yet, -1 several or unknown */
    int alloc_tok, alloc_line;    /* the allocator (the last one), for the note */
    struct Sym *nsym;             /* malloc(n): the variable n (the dynamic size) */
    unsigned nstores;             /* ... its store count at the allocation */
    unsigned factor;              /* calloc(n, k): k */
    unsigned char known;          /* lo and hi hold */
    unsigned char decl;           /* (also) a declared object's address: lo/hi its bytes from there */
  } os_heap;
  unsigned char flags; /* SYMF_*: 1-bit facts SymAttr has no room for (sym_flag_set) */
} SymLocalFacts;
typedef struct ObjHeap ObjHeap;
/* SymLocalFacts.flags (SYMF_x (1u << n) as they come) */
#define SYMF_STACK_PROTECT 1u    /* stack_protect: a canary under any -fstack-protector*, -explicit too */
#define SYMF_NO_STACK_PROTECT 2u /* no_stack_protector: none, even under -fstack-protector-all */
#define SYMF_BC_SCOPED 4u /* -b: a local scalar its scope drops once its address is taken (op/bcheck.c) */
#define SYMF_BC_ADDR 8u   /* -b: ... and `&x` was seen: registered there */
#define SYMF_PARAM_MIN 16u /* an array parameter's element Sym, `[static N]`: at least N, no end (op/ubsan.c) */

#define ATTR_WUR 1       /* warn_unused_result: (void) silences it, as with clang */
#define ATTR_NODISCARD 2 /* C23 [[nodiscard]] */
#define ATTR_FMT_PRINTF 1
#define ATTR_FMT_SCANF 2
#define ATTR_FMT_OTHER 3 /* an archetype tcc does not know: accepted, not checked */
#define ATTR_FMT_STRFTIME 4 /* the conversions and their flags; no arguments */
#define ATTR_FMT_STRFMON 5  /* %i / %n take a double (L: long double) */

SymLocalFacts *sym_facts_peek(const struct Sym *s);
int objsize_alloc_size_args(struct Sym *fsym, int *a, int *b); /* ir/bounds.c: an allocator's size arguments */
int objsize_dealloc_arg(struct Sym *fsym); /* ir/bounds.c: the argument a deallocator releases */
void type_to_str(char *buf, int buf_size, CType *type, const char *varstr); /* gen/type: gcc's spelling of a type */
const SymLocalFacts *sym_facts_get(const struct Sym *s);
#define SYM_FACTS(s) sym_facts_get(s)
SymLocalFacts *sym_facts(struct Sym *s);
void sym_free_facts(struct Sym *s);
void sym_facts_free_all(void);
/* a 1-bit per-symbol fact (SYMF_*) in the side table: SymAttr is full */
static inline void sym_flag_set(struct Sym *s, unsigned flag)
{
  sym_facts(s)->flags |= flag;
  s->a.attr_facts = 1;
}
static inline int sym_flag(const struct Sym *s, unsigned flag)
{
  return s->a.attr_facts && (sym_facts_get(s)->flags & flag);
}

#include "source/ir/machine_op.h"
#include "source/ir/tccir.h"

/* Relocation patch for lazy sections - stores a single relocation modification
 * to be applied during streaming output instead of materializing the section */
typedef struct RelocPatch
{
  uint32_t offset; /* Offset within section */
  uint32_t value;  /* Value to write (for 32-bit relocations) */
  struct RelocPatch *next;
} RelocPatch;

/* Deferred chunk for lazy section loading - optimized for memory
 * Using 32-bit sizes for compactness (object file sections are < 4GB) */
typedef struct DeferredChunk
{
  const char *source_path; /* Path to source file (owned by s1->deferred_paths) */
  uint32_t file_offset;    /* Relative offset within source file */
  uint32_t size;           /* Size of this chunk */
  uint32_t dest_offset;    /* Offset in destination section */
  struct DeferredChunk *next;
  int materialized; /* 1 if this chunk has been loaded */
} DeferredChunk;

/* A section kept in a file rather than in memory (see tccelf.c, "spilled
 * sections"): one piece per input section folded into it, read where it lies
 * and written, relocated, to a temporary file next to the output. */
typedef struct SpillPiece
{
  unsigned long base, size; /* where it goes in the section */
  unsigned char *data;      /* its bytes that were in memory, data_size of them */
  unsigned long data_size;
  DeferredChunk *chunks;    /* and where the rest are on disk */
} SpillPiece;

typedef struct SectionSpill
{
  SpillPiece *pieces; /* by base, which is the order they were added in */
  int nb_pieces, alloc_pieces;
  unsigned char *written;    /* per piece: its bytes are in the file */
  unsigned long written_end; /* the file holds [0, written_end) */
  int cur;                   /* the piece in buf, or -1 */
  unsigned char *buf;
  unsigned long buf_size;
  int fd, src_fd;
  const char *src_path;
  char *path;
} SectionSpill;

/* Where one object's relocations for a function are, and how to read them
 * (see tccelf.c, "relocations left on disk"). */
typedef struct LazyRels
{
  struct LazyRels *next;
  const char *path;    /* owned by s1->deferred_paths */
  uint32_t file_offset;
  uint32_t count;
  uint32_t offset;     /* of that object's bytes in the section */
  uint32_t nb_syms;
  const int *remap;    /* the object's symbol -> symtab_section index, s1->reloc_remaps */
} LazyRels;

/* section definition */
typedef struct Section
{
  unsigned long data_offset;    /* current data offset */
  unsigned char *data;          /* section data: offsets [data_base, data_base + data_allocated) */
  unsigned long data_allocated; /* used for realloc() handling */
  unsigned long data_base;      /* 0, except in a spilled section: its pieces are below */
  /* .text while functions are generated into it (text_window_spill): the code
   * below data_bias is in the internal spill file, and data is biased so that
   * data + off is still the byte at off for every off from data_bias on;
   * data_allocated counts from 0 as ever.  0 everywhere else. */
  unsigned long data_bias;
  struct SectionSpill *spill;   /* NULL, or where the bytes below data_base are */
  LazyRels *lazy_rels;          /* relocations still in the object files, in load order */
  TCCState *s1;
  int sh_name;             /* elf section name (only used during output) */
  int sh_num;              /* elf section number */
  int sh_type;             /* elf section type */
  int sh_flags;            /* elf section flags */
  int sh_info;             /* elf section info */
  int sh_addralign;        /* elf section alignment */
  int sh_entsize;          /* elf entry size */
  unsigned long sh_size;   /* section size (only used during output) */
  addr_t sh_addr;          /* address at which the section is relocated */
  unsigned long sh_offset; /* file offset */
  struct Section *link;    /* link to another section */
  struct Section *reloc;   /* corresponding section for relocation, if any */
  struct Section *hash;    /* hash table for symbols */
  struct Section *prev;    /* previous section on section stack */
  /* Lazy loading support */
  unsigned char lazy;                /* 1 = section uses lazy loading */
  unsigned char materialized;        /* 1 = data has been loaded (legacy) */
  unsigned char has_deferred_chunks; /* 1 = has chunks not yet materialized */
  unsigned char per_function;        /* one function's code (function_text_section): fitted once generated */
  DeferredChunk *deferred_head;      /* List of chunks to load */
  DeferredChunk *deferred_tail;      /* For O(1) append */
  struct SectionAux *aux;            /* what few sections need: section_aux() */
  char name[1];                      /* section name */
} Section;

/* What only a few kinds of section need, out of the Section every function of
 * a link has (a link holds thousands, see "relocations left on disk"). */
typedef struct SectionAux
{
  /* A lazy debug section: relocation patches, 8 bytes each */
  uint32_t *reloc_patch_offsets;
  uint32_t *reloc_patch_values;
  int nb_reloc_patches;
  int alloc_reloc_patches;
  /* A symbol table's hash section: full hash values by symbol, for find_elf_sym */
  unsigned int *hash_val_cache;
  int hash_val_alloc;
  int nb_hashed_syms;
} SectionAux;

#define SECTION_NB_PATCHES(s) ((s)->aux ? (s)->aux->nb_reloc_patches : 0)

/* -------------------------------------------------- */
/* Garbage Collection During Loading (Phase 2) - Lazy Section Info */

/* Represents a section that may be loaded lazily based on GC */
typedef struct LazySectionInfo
{
  char *name;              /* Section name (owned) */
  uint32_t size;           /* Section size */
  uint32_t file_offset;    /* Offset in source file */
  uint32_t archive_offset; /* Archive member offset (0 if not in archive) */
  Section *section;        /* NULL until loaded */
  int referenced;          /* Set by GC mark phase */
  int sh_type;             /* Section type */
  int sh_flags;            /* Section flags */
  int sh_addralign;        /* Section alignment */
  int reloc_index;         /* Index of relocation section, or 0 */
} LazySectionInfo;

/* Represents an object file being loaded lazily */
typedef struct LazyObjectFile
{
  char *filename;            /* Object file path */
  LazySectionInfo *sections; /* Array of lazy sections */
  int nb_sections;           /* Number of sections */
  int fd;                    /* File descriptor (kept open during loading) */
  unsigned long file_offset; /* Offset within file (for archives) */
  ElfW(Ehdr) ehdr;           /* ELF header */
  ElfW(Shdr) * shdr;         /* Section headers (loaded) */
  char *strsec;              /* Section name string table */
  ElfW(Sym) * symtab;        /* Symbol table (loaded immediately) */
  char *strtab;              /* String table for symbols */
  int nb_syms;               /* Number of symbols */
  int *old_to_new_syms;      /* Symbol index mapping */
} LazyObjectFile;

typedef struct DLLReference
{
  int level;
  void *handle;
  unsigned char found, index;
  char name[1];
} DLLReference;

/* A loaded YAFF library kept for on-demand symbol resolution.  Rather than
   interning all of a library's exports, tcc reads its on-disk exported-symbol
   tables (name/value region + index->offset lookup + name hash) and resolves a
   referenced symbol by hashing its name into the on-disk hash (tcc_yaff_resolve)
   — touching only the handful of symbols the link actually uses. */
typedef struct YaffLib
{
  char *region;         /* exported-symbol entries (4B YaffSymbolEntry + name), owned */
  unsigned short *lookup; /* symbol index -> byte offset into region, owned */
  unsigned int *hash;   /* [nbucket, nchain, bucket[nbucket], chain[nchain]], owned */
  unsigned int nsyms;   /* exported_symbols_amount (== lookup/chain length) */
} YaffLib;

/* One library's part of the -fmodule-local-calls import set.  Only names
   are needed (membership), so the export entries are boiled down to their
   names, back to back without the 4-byte entry headers and the alignment
   padding, and the on-disk name hash is kept as u16 when its indices fit. */
typedef struct YaffImportLib
{
  char *names;             /* NUL-terminated names, owned */
  unsigned short *lookup;  /* symbol index -> offset of its name in names, owned */
  void *hash;              /* bucket[nbucket] then chain[nchain], u16 or (wide) u32, owned */
  unsigned int nsyms, nbucket, nchain, names_len;
  unsigned char wide;
  unsigned char bias;      /* name i is at names + lookup[i] + bias */
} YaffImportLib;

/* -------------------------------------------------- */

#define SYM_STRUCT 0x40000000     /* struct/union/enum symbol space */
#define SYM_FIELD 0x20000000      /* struct/union field symbol space */
#define SYM_FIRST_ANOM 0x10000000 /* first anonymous sym */

/* stored in 'Sym->f.func_type' field */
#define FUNC_NEW 1      /* ansi function prototype */
#define FUNC_OLD 2      /* old function prototype */
#define FUNC_ELLIPSIS 3 /* ansi function prototype with ... */

/* stored in 'Sym->f.func_call' field */
#define FUNC_CDECL 0     /* standard c call */
#define FUNC_STDCALL 1   /* pascal c call */
#define FUNC_FASTCALL1 2 /* first param in %eax */
#define FUNC_FASTCALL2 3 /* first parameters in %eax, %edx */
#define FUNC_FASTCALL3 4 /* first parameter in %eax, %edx, %ecx */
#define FUNC_FASTCALLW 5 /* first parameter in %ecx, %edx */
#define FUNC_THISCALL 6  /* first param in %ecx */

/* field 'Sym.t' for macros */
#define MACRO_OBJ 0  /* object like macro */
#define MACRO_FUNC 1 /* function like macro */
#define MACRO_JOIN 2 /* macro uses ## */
#define MACRO_YASC 4 /* defined in yasc code: its expansion is yasc too */
#define MACRO_SYS 8  /* defined in a system header: warnings in its expansion are quiet (gcc) */

/* field 'Sym.r' for C labels */
#define LABEL_DEFINED 0  /* label is defined */
#define LABEL_FORWARD 1  /* label is forward defined */
#define LABEL_DECLARED 2 /* label is declared but never used */
#define LABEL_GONE                                                                                                     \
  3 /* label isn't in scope, but not yet popped                                                                        \
       from local_label_stack (stmt exprs) */

/* Sym::c of a label an asm goto names in a check-only parse (tcc_state->
   check_only), which may register no ELF symbol: an id below the -3 of
   `&&label`, matched by tcc_ir_asm_label_note as an ELF index would be */
#define ASM_CHECK_LABEL_ID(n) (-3 - (n))
#define ASM_LABEL_ID_VALID(c) ((c) > 0 || (c) <= ASM_CHECK_LABEL_ID(1))

/* type_decl() types */
#define TYPE_ABSTRACT 1 /* type without variable */
#define TYPE_DIRECT 2   /* type with variable */
#define TYPE_PARAM 4    /* type declares function parameter */
#define TYPE_NEST 8     /* nested call to post_type */
#define TYPE_NESTDECL 16 /* nested declarator: the caller applies its suffix to
                           the target of the first pointer read here */

#ifdef CONFIG_TCC_LOW_MEM
#define IO_BUF_SIZE 2048 /* one per open file; headers nest */
#else
#define IO_BUF_SIZE 8192
#endif

typedef struct BufferedFile
{
  uint8_t *buf_ptr;
  uint8_t *buf_end;
  int fd;
  struct BufferedFile *prev;
  int line_num;           /* current line number - here to simplify code */
  int line_ref;           /* tcc -E: last printed line */
  int ifndef_macro;       /* #ifndef macro / #endif search */
  int ifndef_macro_saved; /* saved ifndef_macro */
  int *ifdef_stack_ptr;   /* ifdef_stack value at the start of the file */
  int include_next_index; /* next search path */
  int prev_tok_flags;     /* saved tok_flags */
  char filename[1024];    /* filename */
  char *true_filename;    /* filename not modified by # line directive */
  int col_base;           /* -fstack-usage: bytes of the current line before buffer[0] */
  unsigned char unget[4];
  unsigned char yasc;      /* this file is yasc (see TCCState.yasc) */
  unsigned char sys;       /* found on a system include path (or next to such a header) */
  unsigned char pp_sys;    /* tcc -E: the sys of the last line marker written (flag 3) */
  unsigned char yasc_dir;  /* found through -iyasc (1), its #pragma yasc written by tcc -E (2) */
  unsigned char buffer[1]; /* extra size for CH_EOB char */
} BufferedFile;

#define CH_EOB '\\' /* end of buffer or '\0' char in file */
#define CH_EOF (-1) /* end of file */

/* used to record tokens */
/* Small Buffer Optimization: keep a modest inline token buffer so common macro
   expansions avoid heap traffic entirely.
   allocated_len == 0 means using inline buffer (small_buf).
   allocated_len > 0 means using heap buffer (str pointer). */
#define TOKSTR_SMALL_BUFSIZE 16 /* number of ints in inline buffer */

typedef struct TokenString
{
  int allocated_len; /* 0 = inline, >0 = heap capacity (in ints) */
  int len;           /* current length in ints */
  int spill_pos;     /* 1 + the offset of its copy in the spill file, while allocated_len == -1 or read back from it */
  /* A kept string (tok_str_keep), whose tokens are always on the heap, ends
   * after data.str (TOKSTR_KEPT_SIZE, 16 bytes on ARM): thousands of saved
   * bodies wait for the end of the TU.  What follows is for a string being
   * built or replayed; a kept one is replayed through a header of its own
   * (begin_macro_kept). */
  union
  {
    int *str;                            /* heap buffer pointer */
    int small_buf[TOKSTR_SMALL_BUFSIZE]; /* inline buffer for small strings */
  } data;
  char alloc;
  signed char need_spc; /* space insertion state: -1, 0, 1, 2, 3 */
  unsigned char sys;    /* a live expansion of a system header's macro (next()) */
  int last_line_num;    /* last recorded line number (0 = none) */
  int save_line_num;    /* saved line number for macro */
  /* used to chain token-strings with begin/end_macro() */
  const int *prev_ptr;
  struct TokenString *prev;
  struct TokenString *owner; /* begin_macro_kept: the string this header replays, freed with it */
  /* begin_macro: the token current when the replay began (what the parser
   * would have gone back to) and the order of the replays, for error
   * recovery (recover.c), which unwinds the replays an error left */
  int save_tok;
  unsigned serial;
  /* a replay of a saved string (tok_str_borrow): that string, whose ranges
   * of tokens from system-header macros (tok_str_sys_*) say where its
   * warnings are quiet */
  struct TokenString *src;
  /* tccpp.c, a macro expansion being built and read: which of its tokens a
   * system header's macro wrote (spelled there: an argument's tokens are
   * where the argument was written) -- xs, the string's own (owned); xv, the
   * tokens a view (macro_subst's macro_view) reads, from xv_base on */
  struct PPSys *xs;
  const struct PPSys *xv;
  const int *xv_base;
} TokenString;

/* The header of a kept string: everything up to and including data.str. */
#define TOKSTR_KEPT_SIZE (offsetof(TokenString, data) + sizeof(int *))

/* A kept body written to the spill file (allocated_len == -1): read back on access. */
int *tok_str_unspill(TokenString *s);

/* Access TokenString buffer (inline small_buf, heap str, or reloaded from the spill file) */
static inline int *tok_str_access(TokenString *s)
{
  if (s->allocated_len > 0)
    return s->data.str;
  if (s->allocated_len == 0)
    return s->data.small_buf;
  return tok_str_unspill(s);
}
#define tok_str_buf(s) tok_str_access((TokenString *)(s))

/* GNUC attribute definition */
typedef struct AttributeDef
{
  struct SymAttr a;
  struct FuncAttr f;
  struct Section *section;
  Sym *cleanup_func;
  int alias_target; /* token */
  int asm_label;    /* associated asm label */
  char attr_mode;   /* __attribute__((__mode__(...))) */
  unsigned short yasc; /* yasc qualifiers written before the type: out, inout, in, drop, view */
  unsigned char yslice; /* yasc `[]T` (1) or `[:0]T` (2): the declarator wraps the type */
  unsigned char attr_unused; /* unused / maybe_unused: no -Wunused-* for it */
  int vector_size;  /* __attribute__((vector_size(N))) — total bytes, 0 if not a vector */
  /* What GCC attributes say about pointers (applied by attr_apply_pointer_facts):
     -Wnonnull reads nonnull, the yasc checks read all of them. */
  unsigned char attr_malloc;          /* malloc: a fresh block, nothing else points to it */
  unsigned char attr_returns_nonnull; /* returns_nonnull */
  unsigned char attr_nonnull_all;     /* nonnull with no list: every pointer parameter */
  uint32_t attr_nonnull;              /* nonnull(i, ...): bit i - 1 */
  int attr_dealloc;                   /* malloc(dealloc, i): the deallocator's token */
  int attr_dealloc_arg;               /* and its argument, from 1 */
  unsigned char attr_alloc_a, attr_alloc_b; /* alloc_size(a[, b]), from 1 (builtin/objsize.c) */
  /* yasc_view / yasc_maybe on a member of a struct from regular C: its
     pointer is `view` (YASC_VIEW), `view maybe` with YASC_MAYBE
     (attr_apply_member_facts) */
  unsigned char attr_yasc_ptr;
  /* Attributes the frontend checks at uses (decl/attribute.c, expr/attr_checks.c);
     attr_apply_decl_facts moves them into the declared Sym's SymLocalFacts. */
  unsigned char attr_deprecated;      /* deprecated; attr_deprecated_msg its text (a token), or 0 */
  unsigned char attr_unused_result;   /* ATTR_WUR / ATTR_NODISCARD */
  unsigned char attr_fmt_kind;        /* format(): ATTR_FMT_* */
  unsigned char attr_fmt_idx;         /* format(kind, idx, first): the format's argument, from 1 */
  unsigned char attr_fmt_first;       /* the first argument it formats, 0 for a va_list */
  unsigned char attr_fmt_arg;         /* format_arg(n): n, from 1 */
  unsigned char attr_tainted_result;  /* tainted_result: -Wtainted-format's source (ir/taint.c) */
  uint32_t attr_tainted_args;         /* tainted_arg(n): what argument n points to, bit n-1 */
  unsigned char attr_ssp;             /* stack_protect / no_stack_protector: SYMF_(NO_)STACK_PROTECT */
  /* -ffortify's (builtin/fortify.c): null_terminated_string_arg(n), bit n-1;
     access(mode, ref[, size]): {mode (ATTR_ACCESS_*), ref, size} from 1 */
  uint32_t attr_cstr_args;
  unsigned char attr_access_n;
  unsigned char attr_access[FORTIFY_MAX_ACCESS][3];
  int attr_deprecated_msg;
  int attr_nodiscard_msg;             /* [[nodiscard("why")]]: a token, or 0 */
  /* C23 `auto` (C23 or yasc) or `__auto_type` with no type specifier: the
     keyword; the declaration takes its type from the initializer (decl.c) */
  int auto_tok;
  /* `int owned(close) maybe`: an int handle's qualifiers and deallocator */
  unsigned char yhandle;
  int yhandle_dealloc;
  /* `regex_t owned(regfree)`: an object owned as a whole, its deallocator */
  int yobj_dealloc;
  /* `struct T borrows(src)` where the type is used: what its values borrow */
  unsigned char sbind_set, sbind_static, sbind_nb;
  int sbind_names[4];
} AttributeDef;

/* inline functions */
/* TCCState.sanitize: -fsanitize=<name> (gcc's names), -ftrapv */
#define SAN_SIGNED_OVERFLOW 0x0001 /* signed-integer-overflow: + - * negation, MIN / -1 */
#define SAN_SHIFT_BASE 0x0002      /* shift-base: a negative or overflowing signed << */
#define SAN_SHIFT_EXP 0x0004       /* shift-exponent: a count < 0 or >= the width */
#define SAN_DIV_ZERO 0x0008        /* integer-divide-by-zero */
#define SAN_NULL 0x0010            /* null: a dereference of a null pointer */
#define SAN_ALIGN 0x0020           /* alignment: of a misaligned pointer */
#define SAN_VLA 0x0040             /* vla-bound: a VLA bound <= 0 */
#define SAN_BOUNDS 0x0080          /* bounds: an index outside an array of known size */
#define SAN_TRAPV 0x0100           /* -ftrapv: signed + - * and negation only */
#define SAN_UNREACHABLE 0x0200     /* unreachable: a __builtin_unreachable() reached (yasc's unreachable always is) */
#define SAN_ARITH_ALL (SAN_SIGNED_OVERFLOW | SAN_SHIFT_BASE | SAN_SHIFT_EXP | SAN_DIV_ZERO)
#define SAN_UNDEFINED (SAN_ARITH_ALL | SAN_NULL | SAN_ALIGN | SAN_VLA | SAN_BOUNDS | SAN_UNREACHABLE)
/* TCCState.stack_protector */
#define SSP_PLAIN 1    /* -fstack-protector: char arrays of 8 bytes or more, alloca, VLAs */
#define SSP_STRONG 2   /* -fstack-protector-strong: any array, any address-taken local */
#define SSP_ALL 3      /* -fstack-protector-all: every function */
#define SSP_EXPLICIT 4 /* -fstack-protector-explicit: only functions with stack_protect */
/* frontend/gen/op/ubsan.c: ir->ssp / ir->ssp_off at a function's start */
void ssp_begin_function(struct TCCIRState *ir, struct Sym *sym);
int ubsan_param_bound_wanted(struct Sym *arg, int dynamic); /* -fsanitize=bounds, array parameters */
void ubsan_param_bound(struct Sym *arg, int dynamic);
int ubsan_param_bounds_mark(void);
void ubsan_param_bounds_release(int mark);
void ubsan_param_bounds_finish(struct TCCIRState *ir);
#ifdef CONFIG_TCC_BCHECK
void bcheck_func_exit(void); /* -b: the locals registered die (gen/op/bcheck.c) */
void bcheck_main_entry(struct Sym *func); /* -b: main's argv becomes regions */
void bcheck_ir_operand(struct SValue *sv); /* -b: an access reaching tcc_ir_put */
void gbound(void);                         /* -b: vtop, an lvalue, checked */
#endif

/* the warning options (TCCState.warn_none up to warn_num) at a place */
#define DIAG_SNAP_SIZE 96
enum
{
  DIAG_UNSAFE_ON = 1,
  DIAG_UNSAFE_OFF = 2
};
typedef struct DiagSnap
{
  const char *file; /* tcc_filename_intern; NULL: everywhere (the command line) */
  int line;
  unsigned char unsafe; /* an unsafe region's edge: DIAG_UNSAFE_OFF where the strict options
                           go off (tcc_unsafe_begin), DIAG_UNSAFE_ON back on (_end); 0 none */
  unsigned char w[DIAG_SNAP_SIZE];
} DiagSnap;

typedef struct InlineFunc
{
  TokenString *func_str;
  Sym *sym;
  int *pack;        /* #pragma pack state at the body (pp_pack_snapshot), NULL = default */
  int caller_gen;   /* expansions into the function being compiled (tcc_state->inline_caller_gen) */
  struct InlineFuncRare *rare; /* NULL but for a guard head, a nested function, a va_arg_pack clone */
  const char *filename; /* tcc_filename_intern */
  int anchor;           /* its diagnostics' place in the unit (tcc_diag_anchor) */
  /* As a small leaf (not a wrapper): its cost, flat backward jumps and whether
   * it is bounded per caller (func_inl_sret_loop), for a caller wrapping it
   * (tcc_state->inline_leaf_ops).  A small leaf's are small. */
  uint16_t leaf_ops, leaf_backedges;
  int16_t few_callers_left; /* func_inl_few_callers: expansions still allowed in the TU (2 x <= 97) */
  /* Read only as >= 8 and >= INLINE_WRAPPER_PER_CALLER (2): they stop counting
   * past what those can tell apart. */
  unsigned inline_count : 4; /* auto-inline expansions so far (call-heavy budget) */
  unsigned caller_count : 2; /* expansions into the function being compiled */
  unsigned leaf_bounded : 1;
  unsigned expanded : 1; /* inlined at a call site at least once */
  unsigned folded : 1;   /* a call evaluated at compile time (const_eval.c), which reports nothing */
} InlineFunc; /* 4.9K in a Zig kernel compile: 44 -> 32 bytes */

/* What few InlineFuncs carry, out of the one every function of a TU has
 * (4.9k at the peak of a Zig kernel compile: 72 -> 44 bytes each). */
typedef struct InlineFuncRare
{
  int *guard_free; /* guard head: the names it reads but does not bind, [0] = count */
  int *spliced;    /* a __builtin_va_arg_pack clone: the caller's argument names, [0] = ranges, then [start, end) */
  /* A nested function's copy: what each name of its body (in order) bound to at
   * the definition (inline_record_def_bindings), and where that was. */
  struct Sym **def_binds;
  struct Sym *def_func, *def_scope_top;
  struct NestedFunc *def_nf;
  int def_has_capture;
} InlineFuncRare;

/* FN's rare part, made on first use (emit.c); INLINE_RARE reads a field of it,
 * 0 if none; inline_func_free_rare frees it and what it holds. */
ST_FUNC InlineFuncRare *inline_func_rare(InlineFunc *fn);
ST_FUNC void inline_func_free_rare(InlineFunc *fn);
#define INLINE_RARE(fn, field) ((fn)->rare ? (fn)->rare->field : 0)

/* A file-scope function definition whose body is generated at the end of the
 * TU (gen_deferred_function_bodies): its tokens from the opening '{', and the
 * section attribute the definition carried. */
typedef struct DeferredFunc
{
  Sym *sym;
  Section *section;
  TokenString *body;
  int *pack; /* #pragma pack state at the body (pp_pack_snapshot), NULL = default */
  /* A static function referenced only by this many direct calls (>= 2), none
   * from itself and none from code emitted before the end of the TU; else 0. */
  int call_sites;
  const char *filename; /* tcc_filename_intern */
  int anchor;           /* its diagnostics' place in the unit (tcc_diag_anchor) */
} DeferredFunc;

/* A file-scope static object whose initializer is parsed at the end of the TU
 * (prune_unused_statics), and only if something live refers to it: its tokens
 * from the first after '=', and the declaration they initialize. */
typedef struct DeferredData
{
  Sym *sym;
  CType type;
  AttributeDef ad;
  int r;
  TokenString *init;
  const char *filename; /* tcc_filename_intern */
  int anchor;           /* its diagnostics' place in the unit (tcc_diag_anchor) */
} DeferredData;

/* nested functions */
#define MAX_CAPTURED_VARS 32
#define MAX_NONLOCAL_GOTOS 8

typedef struct NestedFunc
{
  TokenString *func_str;                       /* saved token stream of function body */
  Sym *sym;                                    /* function symbol in parent's local scope */
  CType type;                                  /* full function type */
  AttributeDef ad;                             /* function attributes */
  int v;                                       /* token id (function name) */
  char filename[256];                          /* source filename for error messages */
  int captured_offsets[MAX_CAPTURED_VARS];     /* FP offsets of captured parent vars (resolved after regalloc) */
  int captured_tokens[MAX_CAPTURED_VARS];      /* token IDs of captured parent vars */
  int captured_vregs[MAX_CAPTURED_VARS];       /* vreg IDs of captured parent vars (for offset resolution) */
  CType captured_types[MAX_CAPTURED_VARS];     /* full type of captured vars */
  int captured_chain_depth[MAX_CAPTURED_VARS]; /* 1 = parent, 2 = grandparent, ... */
  struct NestedFunc *parent_nf;                /* parent nested function (for multi-level nesting) */
  int nb_captured;                             /* number of captured parent variables */
  int needs_chain_save;                        /* 1 if a child func needs multi-hop chain (depth>1) */
  int compiled;                                /* number of captured parent variables */
  int trampoline_needed;                       /* address of this nested function was taken */
  int nb_real_calls;                           /* call sites that emitted a real call (were not inlined) */
  Sym *trampoline_tcc_sym;                     /* TCC symbol for trampoline code (.text) */
  Sym *chain_slot_tcc_sym;                     /* TCC symbol for chain slot (.data) */
  /* Non-local goto support: nested function does 'goto label' targeting parent __label__ */
  int nlgoto_label_tokens[MAX_NONLOCAL_GOTOS]; /* token IDs of parent labels targeted by goto */
  int nlgoto_buf_offsets[MAX_NONLOCAL_GOTOS];  /* FP-relative offset of 12-byte jmp_buf in parent frame */
  int nb_nlgotos;                              /* number of non-local goto targets */
  /* Address-taken parent labels: nested function uses &&label referencing parent __label__ */
  Sym *addr_label_syms[MAX_NONLOCAL_GOTOS]; /* parent label syms referenced via &&label */
  int nb_addr_labels;                       /* number of addr-taken parent labels */
  /* Parent scope typedefs visible to nested function body */
  int parent_typedef_tokens[MAX_CAPTURED_VARS];  /* token IDs */
  CType parent_typedef_types[MAX_CAPTURED_VARS]; /* saved types */
  int nb_parent_typedefs;                        /* count of saved typedefs */
  /* Parent scope enumeration constants visible to the nested function body
   * (copied: a constant's own Sym is freed with its scope when its value is 0) */
  int parent_enum_tokens[MAX_CAPTURED_VARS];
  CType parent_enum_types[MAX_CAPTURED_VARS];
  long long parent_enum_vals[MAX_CAPTURED_VARS];
  int nb_parent_enums;
  /* Parent scope static locals and block-scope extern variables visible to
   * the nested function body: their Syms outlive the scope (c is the ELF
   * symbol), relinked like the struct tags while the body is compiled. */
  Sym *parent_static_syms[MAX_CAPTURED_VARS];
  int nb_parent_statics;
  /* The GNU nested functions whose names were bound at the definition, itself
   * included (nested_define); compile_nested_functions binds them for the
   * body.  Owned, freed by nested_func_release. */
  Sym **def_vis;
  int nb_def_vis;
  /* Parent scope struct/union/enum tags visible to nested function body.
   * We store pointers to the original Sym (which survives pop_local_syms
   * because completed struct tags have c != 0). */
  Sym *parent_struct_tag_syms[MAX_CAPTURED_VARS]; /* original struct tag syms */
  int nb_parent_struct_tags;                      /* count of saved struct tags */
} NestedFunc;

/* captured_vregs[j] of `nf` names a vreg of the function captured_chain_depth[j]
 * levels up (NULL = the outermost parent).  The nested_funcs table lists the
 * whole family, so a lookup of that vreg in some IR is meaningful only while
 * that function is the one being compiled (`cur`, i.e. current_nested_func). */
static inline int nested_capture_owned_by(const NestedFunc *nf, int j, const NestedFunc *cur)
{
  const NestedFunc *owner = nf;
  for (int d = nf->captured_chain_depth[j]; d > 0; d--)
  {
    if (!owner)
      return 0;
    owner = owner->parent_nf;
  }
  return owner == cur;
}

/* include file cache, used to find files faster and also to eliminate
   inclusion if the include file is protected by #ifndef ... #endif */
typedef struct CachedInclude
{
  int ifndef_macro;
  int once;
  int hash_next;    /* -1 if none */
  char filename[1]; /* path specified in #include */
} CachedInclude;

#define CACHED_INCLUDES_HASH_SIZE 32

/* NOTE: precompiled-header (PCH) support was removed on YasOS (unused; it cost
   runtime heap for the loaded ident/macro/token tables plus flash for the
   serializer).  The enum below is retained because TCC_PCH_REPLAY_PACK_* is
   reused by the general deferred-#pragma-pack replay mechanism (TOK_PACK_REPLAY
   in saved token streams), which is NOT PCH-specific. */
enum
{
  TCC_PCH_REPLAY_TOKENS = 1,
  TCC_PCH_REPLAY_PACK_SET,
  TCC_PCH_REPLAY_PACK_PUSH,
  TCC_PCH_REPLAY_PACK_POP,
  TCC_PCH_REPLAY_PACK_PUSH_DUP, /* bare push: duplicate the top at replay time */
  TCC_PCH_REPLAY_YASC,          /* the yasc dialect off (0) / on (1): around a regular-C
                                   macro's expansion in yasc code (macro_subst_tok) */
  TCC_PCH_REPLAY_ERROR,         /* an error said where the parse reaches it: index 0
                                   the lexer's error that cut a saved body (save_body_cut),
                                   else tcc_error_stash's entry index - 1 */
};

#ifdef CONFIG_TCC_ASM

/* Target-specific register count for inline asm constraints.
 * In this fork we currently support ARM Thumb only. */
#if defined(TCC_TARGET_ARM_THUMB) && !defined(NB_ASM_REGS)
#define NB_ASM_REGS 16
#endif

typedef struct ExprValue
{
  uint64_t v;
  Sym *sym;
  int pcrel;
} ExprValue;

#define MAX_ASM_OPERANDS 30
typedef struct ASMOperand
{
  int id; /* GCC 3 optional identifier (0 if number only supported) */
  char constraint[16];
  char asm_str[16]; /* computed asm string for operand */
  SValue *vt;       /* C value of the expression */
  int ref_index;    /* if >= 0, gives reference to a output constraint */
  int input_index;  /* if >= 0, gives reference to an input constraint */
  int priority;     /* priority, used to assign registers */
  int reg;          /* if >= 0, register number used for this operand */
  int regvar;       /* 1 + register a `register T x __asm("rN")` operand names;
                       0 (what zero-initialisation gives) means none */
  int is_llong;     /* true if double register value */
  int is_memory;    /* true if memory operand */
  int is_rw;        /* for '+' modifier */
  int is_label;     /* for asm goto */
  int label_sym;    /* ELF symbol identity, including block-scoped labels. */
  int label_jind;   /* Original IR label position, resolved at its definition. */
  int addr_read;    /* IR: a memory output addressed through a register; its
                       ASM_INPUT marker keeps that address live (tccasm.c) */
} ASMOperand;
#endif

/* extra symbol attributes (not in symbol table) */
struct sym_attr
{
  unsigned got_offset;
  unsigned plt_offset;
  int plt_sym;
  int dyn_index;
};

/* Cached archive symbol index for reuse across --start-group rescans.
   Avoids re-reading archive data, re-computing hashes, and rebuilding
   the chained hash table on every pass through the same archive. */
typedef struct ArchiveSymbolCache
{
  char *filename;                     /* cache key: archive file path */
  uint8_t *data;                      /* raw archive index data (owns memory) */
  int nsyms;                          /* number of symbols in index */
  int entrysize;                      /* 4 or 8 (32/64-bit offsets) */
  const char **sym_names;             /* pointers into data */
  unsigned int *name_hashes;          /* pre-computed archive_name_hash per symbol */
  const uint8_t *ar_index;            /* member offset table (big-endian, entrysize each) */
  int *ar_ht_buckets;                 /* chained hash table buckets */
  int *ar_ht_next;                    /* chained hash table chain links */
  int ar_ht_mask;                     /* hash table mask (size - 1) */
  unsigned long long *loaded_members; /* dedup set for loaded members */
  unsigned int loaded_member_mask;    /* dedup set mask */
} ArchiveSymbolCache;


struct TCCState
{
  unsigned char verbose;           /* if true, display some information during compilation */
  unsigned char nostdinc;          /* if true, no standard headers are added */
  unsigned char nostdlib;          /* if true, no standard libraries are added */
  unsigned char nodefaultlibs;     /* if true, no default libraries at all (incl. compiler-rt) */
  unsigned char nocommon;          /* if true, do not use common symbols for .bss data */
  unsigned char default_visibility; /* -fvisibility=: STV_* for definitions with no
                                      visibility attribute or #pragma */
  unsigned char static_link;       /* if true, static linking is performed */
  unsigned char rdynamic;          /* if true, all symbols are exported */
  unsigned char symbolic;          /* if true, resolve symbols in the current module first */
  unsigned char filetype;          /* file type for compilation (NONE,C,ASM) */
  unsigned char optimize;          /* only to #define __OPTIMIZE__ */
  unsigned char optimize_size;     /* -Os (1) / -Oz (2): the -O2 pipeline, but
                                      where a choice trades code size for
                                      speed, take the smaller code */
  unsigned char option_pthread;    /* -pthread option */
  unsigned char enable_new_dtags;  /* -Wl,--enable-new-dtags */
  unsigned char gc_sections;       /* -Wl,--gc-sections: garbage collect unused sections */
  unsigned char function_sections; /* -ffunction-sections: place each function
                                      in its own section */
  /* -ffunction-sections into an object, without debug info: the functions go
   * to .text while compiling and are split into their own sections when the
   * object is written (split_text_by_function in tccelf_output.c). */
  unsigned char text_split_pending;
  /* Where .text holds a 2-byte pad that is no function's (a function start
   * padded to a word, an erase's filler), sorted: split_text_by_function
   * leaves them out of the sections, as the linker pads between real ones. */
  unsigned *split_pads;
  int nb_split_pads, cap_split_pads;
  /* func_ind for the backend's split checks (arm-thumb-asm/gen): where the
   * function being generated starts in .text, -1 outside one. */
  int split_func_start;
  /* .text spilled by text_window_spill: {offset, size, file offset} each */
  struct TextPiece *text_pieces;
  int nb_text_pieces, cap_text_pieces;
  unsigned char text_window_done; /* read back for good (text_window_unspill_all) */
  unsigned char data_sections;     /* -fdata-sections: place each data item in its
                                      own section */
  unsigned char internal_spill;    /* -f[no-]internal-spill (on): saved bodies and prototype
                                      parameter lists go to <output>.<pid>.spill (tccpp.c) */
  unsigned int cversion;           /* supported C ISO version, 199901 (the default), 201112, ... */
  unsigned char yasc_std;          /* -std=yasc: the yasc dialect from the start of each TU */
  unsigned char strict_lifetime;   /* the safe default: -Wlifetime, -Wimplicit-move, -Wdangling-pointer and
                                      -Wreturn-local-addr are errors nothing but unsafe turns off
                                      (tcc_unsafe_begin) */
  unsigned char yasc;              /* the yasc dialect is on here: in a yasc file (the main one with
                                      -std=yasc, or one that said #pragma yasc), outside a regular-C
                                      macro's expansion */
  const char **yasc_files;         /* tcc_filename_intern'ed names of the yasc files */
  int nb_yasc_files;
  char **yasc_include_paths;       /* -iyasc DIR / YASC_INCLUDE_PATH: the include_paths entries
                                      whose headers are yasc files */
  int nb_yasc_include_paths;
  uint32_t pp_marker_yasc;         /* compiling tcc -E output: the dialects line markers with
                                      flag 1 left, one bit per level (pp_line_marker) */
  int pp_marker_depth;
  unsigned char ccompat;           /* -ccompat or -std=c../gnu..: regular C, no safe defaults */
  unsigned char fortify;           /* -f[no-]fortify: calls checked against the access and
                                      null_terminated_string_arg attributes (builtin/fortify.c);
                                      2 (the default): on, off under -ccompat (unless
                                      _FORTIFY_SOURCE asks for it) */
  unsigned char yasc_main_decl_seen; /* the main file has had a declaration: too late for `module` */
  char *yasc_module;                /* `module NAME;` of this TU (decl/module.c), else NULL */
  char **yasc_includes;             /* the main file's #includes, for the module interface */
  int nb_yasc_includes;
  char **module_dirs;               /* -fmodule-dir=: where interfaces are written (the first) and read */
  unsigned char module_deps;        /* -fmodule-deps: print module/import lines, compile nothing */
  char *module_header_dir;          /* -fmodule-header=DIR: also write a module's C header there */
  int nb_module_dirs;
  uint32_t warn_user[4];           /* options_W entries the command line set (by index) */

  /* C language options */
  unsigned char char_is_unsigned;
  unsigned char leading_underscore;
  /* build the builtin alias prototypes programmatically at tccgen_compile
     start instead of parsing the tccdecls text (see tcc_predefs_base) */
  unsigned char predef_protos_pending;
  unsigned char ms_extensions;          /* allow nested named struct w/o identifier
                                           behave like unnamed */
  unsigned char dollars_in_identifiers; /* allows '$' char in identifiers */
  unsigned char ms_bitfields;           /* if true, emulate MS algorithm for aligning bitfields */
  unsigned char inline_atomics;         /* -minline-atomics: read-modify-write atomics as LDREX/STREX
                                           loops instead of runtime calls (parse_atomic) */
  /* parse_atomic: while set, unary & records the locals whose address it
   * takes here instead of marking them address-taken (see there). */
  unsigned char defer_addrtaken;
  unsigned char nb_deferred_addrtaken;
  struct Sym *deferred_addrtaken[4];
  SValue deferred_addr_lval; /* the lvalue the first deferred & took the address of */
  unsigned char reverse_funcargs;       /* if true, evaluate last function arg first */
  unsigned char gnu89_inline;           /* treat 'extern inline' like 'static inline' */
  unsigned char unwind_tables;          /* create eh_frame section */

  /* -fno-builtin-<func> bitmask: disable individual builtin inlining */
#define NO_BUILTIN_ABS (1u << 0)
#define NO_BUILTIN_LABS (1u << 1)
#define NO_BUILTIN_LLABS (1u << 2)
#define NO_BUILTIN_UABS (1u << 3)
#define NO_BUILTIN_ULABS (1u << 4)
#define NO_BUILTIN_ULLABS (1u << 5)
#define NO_BUILTIN_UMAXABS (1u << 6)
#define NO_BUILTIN_MEMFUNCS (1u << 7) /* keep memcpy/memset/__aeabi_mem* as real calls
                                       * (plain -fno-builtin, or -fno-builtin-mem{cpy,set}) */
#define NO_BUILTIN_ALL (1u << 8)      /* plain -fno-builtin: no folding/rewriting of libc calls by name */
  unsigned int no_builtin_funcs;

  /* warning switches */
  unsigned char warn_none;
  unsigned char warn_all;
  unsigned char warn_extra; /* -Wextra: the name of the WD_EXTRA group */
  unsigned char warn_error;
  unsigned char warn_write_strings;
  unsigned char warn_unsupported;
  unsigned char warn_system_headers; /* -Wsystem-headers: warnings in system headers too (error1) */
  unsigned char warn_implicit_function_declaration;
  unsigned char warn_discarded_qualifiers;
  unsigned char warn_lifetime; /* -Wlifetime: the flow checks on the raw IR (ir/lifetime.c) */
  unsigned char warn_int_conversion;            /* pointer <-> integer without a cast */
  unsigned char warn_incompatible_pointer_types; /* pointer to a different type */
  unsigned char warn_return_type;               /* return with or without a value against the type */
  unsigned char warn_implicit_int;              /* a declaration with no type */
  unsigned char warn_uninitialized;             /* a local read where no path stored it */
  unsigned char warn_maybe_uninitialized;       /* ... where some path did not */
  unsigned char warn_return_local_addr;         /* return &local */
  unsigned char warn_ignored_attributes;        /* an attribute promising a check tcc does not make */
  unsigned char warn_nonnull;                   /* null passed to a nonnull parameter */
  unsigned char warn_dangling_pointer;          /* a local's address stored where it outlives the function */
  unsigned char warn_null_dereference;          /* a dereference the optimizer proved null */
  unsigned char warn_implicit_move;             /* yasc: ownership leaves a variable without move() */
  unsigned char warn_unchecked;                 /* yasc: a regular-C pointer becomes owned / view outside unsafe */
  unsigned char warn_zero_as_null_pointer_constant; /* yasc: a null pointer written NULL or 0, not nullptr */
  unsigned char warn_volatile_bitfield;         /* yasc: a bit-field store to volatile memory (hidden read-modify-write) */
  unsigned char warn_register_access;           /* yasc: a register field marked ro written, wo read, w1c written back */
  unsigned char warn_strict_prototypes;         /* a function declarator without parameter types */
  unsigned char warn_missing_prototypes;        /* a global function defined with no prototype before it */
  unsigned char warn_override_init;             /* an initializer element replacing an earlier one (a union's other member) */
  unsigned char warn_pointer_arith;             /* arithmetic on void * / function pointers, sizeof(void) */
  unsigned char warn_cast_qual;                 /* a cast dropping const/volatile from the pointed-to type */
  unsigned char warn_cast_align;                /* a cast raising the pointed-to alignment */
  unsigned char warn_address_of_packed_member;  /* &packed->member converted to an over-aligned pointer */
  unsigned char warn_frame_larger_than;         /* -Wframe-larger-than=N: a frame over N bytes (stack_usage.c) */
  unsigned char warn_stack_usage;               /* -Wstack-usage=N: the same, and any dynamic frame */
  unsigned char warn_vla;                       /* a variable-length array declared */
  unsigned char warn_alloca;                    /* a call to alloca / __builtin_alloca */
  unsigned char warn_format;                    /* a format(printf|scanf) call's literal against its arguments */
  unsigned char warn_format_security;           /* a format that is not a literal, with no arguments */
  unsigned char warn_format_n;                  /* %n in a printf format */
  unsigned char warn_unused_result;             /* warn_unused_result / [[nodiscard]] result discarded */
  unsigned char warn_deprecated_declarations;   /* a deprecated declaration used */
  unsigned char warn_sign_compare;              /* signed and unsigned compared, the signed side converted */
  unsigned char warn_conversion;                /* an implicit conversion that may change a value */
  unsigned char warn_sign_conversion;           /* ... that may change its sign */
  unsigned char warn_shift_count_overflow;      /* a constant shift count >= the width */
  unsigned char warn_shift_count_negative;      /* a constant shift count < 0 */
  unsigned char warn_shift_overflow;            /* 2 << 31: a constant shifted past the sign bit */
  unsigned char warn_div_by_zero;               /* an integer division by the constant 0 */
  unsigned char warn_overflow;                  /* a constant that does not fit, signed overflow folding */
  unsigned char warn_tautological_compare;      /* x == x */
  unsigned char warn_type_limits;               /* unsigned < 0, a char compared out of its range */
  unsigned char warn_array_bounds;              /* a constant index outside a known-size array */
  unsigned char warn_sizeof_pointer_memaccess;  /* memset(p, 0, sizeof p) */
  unsigned char warn_sizeof_array_argument;     /* sizeof of an array parameter */
  unsigned char warn_implicit_fallthrough;      /* a case reached from the one above it, unmarked */
  unsigned char warn_parentheses;               /* `if (a = b)`, `a && b || c`, `a & b == c` */
  unsigned char warn_logical_not_parentheses;   /* `!a == b` */
  unsigned char warn_dangling_else;             /* `if (a) if (b) x; else y;` */
  unsigned char warn_empty_body;                /* `if (x);`, `else;`, `do; while (x);` */
  unsigned char warn_switch;                    /* an enum switch missing an enumerator, no default */
  unsigned char warn_shadow;                    /* a local hiding a local, a parameter or a global */
  unsigned char warn_cross_tu;                  /* a global's types differ between units (at the link) */
  unsigned char warn_tainted_format;            /* a printf format from untrusted input (ir/taint.c) */
  /* -Wunused-* (frontend/gen/core/unused.c) */
  unsigned char warn_unused;                    /* -Wunused: the name of the group below */
  unsigned char warn_unused_variable;           /* a local, or a file-scope static, never used */
  unsigned char warn_unused_const_variable;     /* ... a const file-scope static of the main file */
  unsigned char warn_unused_function;           /* a static function defined and never used */
  unsigned char warn_unused_but_set_variable;   /* a local only ever assigned to */
  unsigned char warn_unused_value;              /* a statement whose value goes nowhere */
  unsigned char warn_unused_label;              /* a label nothing jumps to */
  unsigned char warn_unused_local_typedefs;     /* a typedef in a block never used */
  unsigned char warn_unused_parameter;          /* a parameter never used */
  unsigned char warn_unused_but_set_parameter;  /* a parameter only ever assigned to */
  /* object sizes (frontend/gen/builtin/objsize.c) */
  unsigned char warn_stringop_overflow;         /* memcpy/strcpy/... writing past a known-size destination */
  unsigned char warn_format_overflow;           /* sprintf output longer than its destination */
  unsigned char warn_format_truncation;         /* snprintf output longer than its bound */
  unsigned char warn_alloc_size_larger_than;    /* an allocation's constant size over the limit */
  unsigned char warn_alloc_mul_overflow;        /* malloc(n * m) with no overflow check */
  unsigned char warn_free_nonheap_object;       /* free of a local, a global or a literal */
  unsigned char warn_mismatched_dealloc;        /* fclose(malloc(...)), free(fopen(...)) */
  unsigned char warn_use_after_free;            /* *p or free(p) once free(p) ran (ir/bounds.c) */
#define WARN_ON 1         /* warning is on (-Woption) */
  unsigned char warn_num; /* temp var for tcc_warning_c() */
  unsigned char diag_color;      /* -fdiagnostics-color= (DIAG_COLOR_*) */
#define DIAG_COLOR_AUTO 0
#define DIAG_COLOR_ALWAYS 1
#define DIAG_COLOR_NEVER 2
  unsigned char diag_show_caret;  /* -f[no-]diagnostics-show-caret: the source line under it (DIAG_SHOW_*) */
  unsigned char diag_show_option; /* -f[no-]diagnostics-show-option: [-Wname] after it */
  unsigned char diag_line_numbers; /* -f[no-]diagnostics-show-line-numbers: with the source line unless OFF */
#define DIAG_SHOW_AUTO 0 /* on where a person reads it: a terminal, or -fdiagnostics-color=always */
#define DIAG_SHOW_ON 1
#define DIAG_SHOW_OFF 2
  unsigned char diag_last_shown; /* the last warning/error was printed: its notes are too */
  unsigned char diag_any_header; /* the next warning is given in a system header too (#warning) */
  unsigned char diag_noabort;    /* a warning made an error does not abort (a checker's report walk) */
  unsigned char diag_flow_only;  /* only diagnostics that carry their line ("%i:", the flow checks):
                                    the check-only parse of an inlined body the expansions already
                                    reported on (check_uncompiled_inline_functions) */
  const char *main_file;         /* tcc_filename_intern of the unit's main file */
  unsigned char unused_on;       /* some -Wunused-* check was on in this unit: uses are recorded
                                    (sticky: a #pragma turning it off must not lose a use) */
  unsigned char syntax_only;     /* -fsyntax-only: parse and check, generate and write nothing */
  unsigned char unsafe_report;   /* -funsafe-report: a line per yasc unsafe region */
  unsigned char yasc_facts;      /* -fyasc-facts: the optimizer may assume what checked yasc types say
                                    (S4; only with strict_lifetime, never at -O0) */
  /* stack use (backend/generators/stack_usage.c) */
  unsigned long long frame_larger_than; /* -Wframe-larger-than=N (when frame_larger_than_set) */
  unsigned long long stack_usage_limit; /* -Wstack-usage=N (when stack_usage_limit_set) */
  unsigned char frame_larger_than_set, stack_usage_limit_set;
  /* -Walloc-size-larger-than=N (default PTRDIFF_MAX), -Wstringop-overflow=N,
     -Wformat-overflow=N, -Wformat-truncation=N (levels; 0: the default) */
  unsigned long long alloc_size_limit;
  unsigned char alloc_size_limit_set, stringop_level, format_overflow_level, format_truncation_level;
  /* -Warray-bounds=N: 1 (also plain -Warray-bounds, -Wall) the frontend's
     checks, 2 also the value ranges on the raw IR (ir/bounds.c); 0 unset */
  unsigned char array_bounds_level;
  unsigned char stack_usage;     /* -fstack-usage: write <object>.su */
  unsigned char callgraph_info;  /* -fcallgraph-info[=su]: write <object>.ci */
  struct StackUsage *su;         /* the unit's functions, written by tcc_stack_usage_write */
  /* the last declarator's name (type_decl, only while TCC_SU_WANTED): a
   * function definition's .su location, gcc's line and column of the name */
  int su_name_tok, su_name_line, su_name_col;
  unsigned warned_attributes;    /* -Wignored-attributes: the ones already reported in this unit */
  /* #pragma GCC/tcc diagnostic: the warning options as each pragma left them,
     by file and line, so a diagnostic reported later than its line was parsed
     (the flow checks, at the end of a function) sees the state at its line;
     entry 0 is the command line's.  And the push stack. */
  struct DiagSnap *diag_snaps;
  int nb_diag_snaps;
  int nb_diag_pragmas; /* the #pragma diagnostic changes among them (not unsafe's edges) */
  unsigned char diag_snaps_or[DIAG_SNAP_SIZE]; /* each option OR'd over the records: on somewhere */
  int unsafe_depth;               /* unsafe { } / unsafe(expr) open now (yasc) */
  unsigned char unsafe_saved[12]; /* the strict options (WD_STRICT) before the outermost one */
  uint8_t *sys_globals;           /* -Wlifetime: a bit per token, a global variable declared in a
                                     system header (errno, stdout): what a libc call may write */
  int sys_globals_size;           /* in bytes */
  struct DiagSnap *diag_stack;
  int nb_diag_stack;
  /* the report-once set (tcc_diag_once): what was already reported in this
     unit -- by option or kind, file, line and text -- as open-addressed
     hashes (0 marks a free slot).  Every "once" of the frontend goes here:
     tcc_warning_once, tcc_fe_warning, tcc_diag_first, the attribute and the
     -Wunused checks. */
  unsigned *diag_once;
  int nb_diag_once, diag_once_cap;
  /* the parse of a body being read (tcc_diag_parse_enter): a function's
     compile, check-only parse, inline expansion or cleanup replay.  The k-th
     warning of a line and text in one parse is the k-th in every other. */
  unsigned diag_parse, diag_parse_next;
  /* the system headers of this unit, from a line on (0: the whole file;
     #pragma GCC system_header: its line), by tcc_filename_intern'ed name --
     for a body replayed after its file was closed (tcc_in_sys_header) */
  struct SysFile
  {
    const char *name;
    int line;
  } *sys_files;
  int nb_sys_files;

  unsigned char option_r;         /* option -r */
  unsigned char do_bench;         /* option -bench */
  unsigned char just_deps;        /* option -M  */
  unsigned char gen_deps;         /* option -MD  */
  unsigned char include_sys_deps; /* option -MD  */
  unsigned char gen_phony_deps;   /* option -MP */

  /* compile with debug symbol (and use them if error during execution) */
  unsigned char do_debug;
  unsigned char dwarf;
  unsigned char do_backtrace;
#ifdef CONFIG_TCC_BCHECK
  /* compile with built-in memory and bounds checker */
  unsigned char do_bounds_check;
#endif
  unsigned char test_coverage; /* generate test coverage code */
  /* -fsanitize=... (frontend/gen/op/ubsan.c) and -ftrapv: SAN_* bits, 0 =
     off */
  unsigned short sanitize;
  /* the checks that call libc's __ubsan_handle_* instead of trapping:
     asked for by -fno-sanitize-trap / -fsanitize-recover /
     -fno-sanitize-recover, unless -fsanitize-trap names them; _abort
     variants for -fno-sanitize-recover's (SAN_* bits) */
  unsigned short sanitize_handler, sanitize_trap, sanitize_abort;
  /* -fstack-protector[-strong|-all|-explicit]: SSP_* (0 = off) */
  unsigned char stack_protector;

  /* IR optimization flags (-f options) */
  unsigned char opt_dce;              /* -fdce: dead code elimination */
  unsigned char opt_const_prop;       /* -fconst-prop: constant propagation */
  unsigned char opt_copy_prop;        /* -fcopy-prop: copy propagation */
  unsigned char opt_cse;              /* -fcse: common subexpression elimination */
  unsigned char opt_bool_idempotent;  /* -fbool-idempotent: boolean idempotent simplification */
  unsigned char opt_bool_simplify;    /* -fbool-simplify: boolean expression simplification */
  unsigned char opt_store_load_fwd;   /* -fstore-load-fwd: store-load forwarding */
  unsigned char opt_redundant_store;  /* -fredundant-store-elim: redundant store elimination */
  unsigned char opt_dead_store;       /* -fdead-store-elim: dead store elimination */
  unsigned char opt_fp_offset_cache;  /* -ffp-offset-cache: frame pointer offset caching */
  unsigned char opt_indexed_memory;   /* -findexed-memory: indexed load/store fusion */
  unsigned char opt_disp_fusion;      /* -fdisp-fusion: ADD+LOAD/STORE -> displacement-addressed mem op */
  unsigned char opt_lea_fold;         /* -flea-fold: LEA Addr[StackLoc]+deref -> direct stack slot access */
  unsigned char opt_mla_fusion;       /* -fmla-fusion: multiply-accumulate fusion */
  unsigned char opt_stack_addr_cse;   /* -fstack-addr-cse: stack address CSE */
  unsigned char opt_licm;             /* -flicm: loop-invariant code motion */
  unsigned char opt_strength_red;     /* -fstrength-reduce: strength reduction for multiply */
  unsigned char opt_iv_strength_red;  /* -fiv-strength-red: IV strength reduction for array access */
  unsigned char opt_loop_unroll;      /* -floop-unroll: full unroll small constant-trip-count loops */
  unsigned char opt_reroll;           /* -freroll-blocks: re-roll N identical consecutive blocks into a loop */
  unsigned char opt_vrp;              /* -fvrp: value range propagation branch folding */
  unsigned char opt_float_narrow;     /* -ffloat-narrow: narrow double math to float when safe */
  unsigned char opt_peephole;         /* -fpeephole: instruction fusion and code generation caches */
  unsigned char opt_jump_threading;   /* -fjump-threading: jump threading optimization */
  /* Per-caller budget for const-arg inlining of *loop* bodies; reset for each
   * function in gen_function(). See the CONST_LOOP_INLINE_BUDGET use in
   * frontend/gen/builtin/call.c. */
  int const_loop_inline_used;
  unsigned char opt_inline_functions; /* -finline-functions: auto-inline small functions at -O2 */
  unsigned char opt_inline_small;     /* -finline-small-functions: auto-inline tiny functions at -O1 */
  unsigned char opt_inline_called_once; /* -finline-functions-called-once: defer function bodies to the end of the
                                           TU and inline each static function called from one place there */
  unsigned char opt_drop_unused_statics; /* -fdrop-unused-statics: defer static functions and initialized static
                                            objects to the end of the TU and emit only those something live refers to */
  unsigned char opt_ipc;              /* interprocedural constant propagation */
  int opt_inline_limit;               /* -finline-limit=N: token-stream word threshold (default 0=use level default) */
  unsigned char opt_inline_limit_user; /* -finline-limit=N given explicitly: -O levels must not override it */
  unsigned char instrument_functions; /* -finstrument-functions */

  /* Function purity cache for LICM optimization */
  /* Cache stores inferred purity for functions in the current translation unit */
#ifdef CONFIG_TCC_O0_ONLY
#define FUNC_PURITY_CACHE_SIZE 1 /* filled only by the optimizer */
#else
#define FUNC_PURITY_CACHE_SIZE (1 << 16)
#endif
  /* Heap arrays grown on demand up to the *_CACHE_SIZE caps (here and the
   * const-result cache below): embedded in TCCState they were 8 KiB that
   * every compile carried, while a TU fills a few entries of each. */
  struct
  {
    int token;  /* Function name token (v field of Sym); 0 = empty slot */
    int purity; /* TCC_FUNC_PURITY_* value */
  } *func_purity_cache; /* hash table of func_purity_cache_cap slots, see func_purity.c */
  int func_purity_cache_count;
  int func_purity_cache_cap;

  /* Constant function result cache for interprocedural constant propagation.
   * After optimization, functions that reduce to "return #const" are cached
   * so callers can replace FUNCCALLVAL with ASSIGN #const. */
#ifdef CONFIG_TCC_O0_ONLY
#define FUNC_CONST_RESULT_CACHE_SIZE 1 /* filled only by the optimizer */
#else
#define FUNC_CONST_RESULT_CACHE_SIZE 256
#endif
  struct
  {
    int token;      /* Function name token (v field of Sym) */
    int64_t value;  /* Constant return value */
    int btype;      /* IROP_BTYPE_* of return value */
  } *func_const_result_cache;
  int func_const_result_cache_count;
  int func_const_result_cache_cap;

  /* Switch-value function snapshot cache: holds replayable bodies of
   * side-effect-free single-parameter functions whose return value depends on
   * the argument (e.g. `static int f(int x) { switch (x) { case K: return C; } }`).
   * Callers with a constant argument simulate the snapshot to fold the call. */
#ifdef CONFIG_TCC_O0_ONLY
#define FUNC_SWITCH_CACHE_SIZE 1 /* filled only by the optimizer */
#else
#define FUNC_SWITCH_CACHE_SIZE 64
#endif
  struct TCCFuncSwitchSnapshot *func_switch_cache[FUNC_SWITCH_CACHE_SIZE];
  int func_switch_cache_count;

  /* Debug-only runtime features.  These fields are kept UNCONDITIONALLY (not
   * under #ifdef CONFIG_TCC_DEBUG) so that the TCCState layout is identical in
   * debug and release builds.  CONFIG_TCC_DEBUG lives in config.mak's CFLAGS,
   * not config.h, and object files don't depend on config.mak — so flipping
   * --debug while reusing stale objects (e.g. the FORCE-rebuilt arch lib vs.
   * unchanged core objects) would otherwise shift every field after this one,
   * making symtab_section read as common_section and crashing th_sym_t. */
  unsigned char dump_ir; /* -dump-ir: print IR (pre/post opts) to stdout */
  /* -dump-ir-passes=name[,name...] (or "all"): after each named optimization
   * pass in the optimize loop, print "=== AFTER <name> ===" + IR.  Used to
   * bisect which pass corrupts the IR.  NULL = disabled. */
  char *dump_ir_passes;

  /* use GNU C extensions */
  unsigned char gnu_ext;
  /* use TinyCC extensions */
  unsigned char tcc_ext;

  unsigned char dflag; /* -dX value */
  unsigned char Pflag; /* -P switch (LINE_MACRO_OUTPUT_FORMAT) */

  unsigned char pic;    /* enable position independent code */
  unsigned char no_pie; /* disable PIE for executables */
#ifdef TCC_TARGET_X86_64
  unsigned char nosse; /* For -mno-sse support. */
#endif
#if defined(TCC_TARGET_ARM) || defined(TCC_TARGET_ARM_THUMB)
  unsigned char float_abi; /* float ABI of the generated code*/
  unsigned char fpu_type;  /* FPU type for ARM hardfp */
  unsigned char fp_inline;  /* -mfp-inline=: enum arm_fp_inline */
  unsigned char fp_lib;     /* -mfp-lib=: enum arm_fp_lib */
  const char *march_str;   /* -march= value, NULL means default */
#endif
  unsigned char text_and_data_separation; /* support for GCC
                                             -mno-pic-data-is-text-relative */
  unsigned char yaff_slow_memory; /* -slow-memory: YAFF_MEMORY_SLOW */
  char *hot_text_file;            /* --hot-text=FILE: names of the functions the loader runs from RAM */
  char **hot_text_flash_names;    /* --hot-text's '=name': flash callers call its flash copy */
  int nb_hot_text_flash_names;
  char *ram_text_file;            /* --ram-text=FILE: functions the linker script runs from RAM */
  char **interned_files;          /* tcc_filename_intern: one copy of each source path */
  int nb_interned_files;
  const char *diag_replay_file;   /* the file of the body an inline expansion (or a cleanup) replays:
                                     its diagnostics name it, and its system-header state is that file's */
  unsigned char fe_speculative;   /* const_eval.c's speculative parse: no frontend warnings */
  unsigned char diag_spec_dropped; /* the speculative parse met a warning it could not say */
  unsigned char diag_spec_recheck; /* ... and its call folded: the check-only parse runs */
  char **ram_text_names, **ram_text_section_names;
  int nb_ram_text_names, nb_ram_text_section_names;
  unsigned char ram_text_read;
  unsigned long hot_text_start, hot_text_end; /* that block of .text; both 0 without any */
  unsigned int yaff_stack_size; /* -stack-size=N: per-image stack hint (bytes)
                                   written into the YAFF header; 0 = default */
  unsigned int yaff_heap_size;  /* -heap-size=N: per-image heap cap (bytes)
                                   written into the YAFF header; 0 = default */
  unsigned char share_rodata;   /* -share-rodata: RELRO — route const objects
                                   with pointer-bearing types to the writable
                                   data segment so .rodata stays pure-const and
                                   can be shared (XIP) across processes */
  /* -msb-relative-got: one ldr.w Rt,[r9,#imm12] per global instead of
     literal+add+load; caps the GOT at 4096 B. See docs/sb_relative_got.md */
  unsigned char sb_relative_got;

  unsigned char has_text_addr;
  addr_t text_addr;       /* address of text section */
  unsigned section_align; /* section alignment */
#ifdef TCC_TARGET_I386
  int seg_size; /* 32. Can be 16 with i386 assembler (.code16) */
#endif

  char *tcc_lib_path;  /* CONFIG_TCCDIR or -B option */
  char *soname;        /* as specified on the command line (-soname) */
  char *rpath;         /* as specified on the command line (-Wl,-rpath=) */
  char *elf_entryname; /* "_start" unless set */
  char *init_symbol;   /* symbols to call at load-time (not used currently) */
  char *fini_symbol;   /* symbols to call at unload-time (not used currently) */
  char *mapfile;       /* create a mapfile (not used currently) */

  /* output type, see TCC_OUTPUT_XXX */
  int output_type;
  /* output format, see TCC_OUTPUT_FORMAT_xxx */
  int output_format;
  /* nth test to run with -dt -run */
  int run_test;

  /* array of all loaded dlls (including those referenced by loaded dlls) */
  DLLReference **loaded_dlls;
  int nb_loaded_dlls;

  /* Loaded YAFF libraries, kept for on-demand symbol resolution (see YaffLib). */
  YaffLib *yaff_libs;
  int nb_yaff_libs;

  /* -fmodule-local-calls (default on under text_and_data_separation): a call
     to an undefined global function that no shared library in the library
     paths exports is compiled as module-local, so its R9 reload is dropped
     (arm-thumb-gen.c thumb_callee_in_this_module). Whether the assumption held
     is checked at link time through R_ARM_YASOS_LOCAL_CALL. The import set is
     built lazily by tcc_yaff_import_set_has: the export tables of every
     lib*.so in the library paths, minus what libtcc1.a defines (the archive is
     pulled before the shared libraries resolve, so its members win). */
  unsigned char module_local_calls;
  unsigned char import_set_state; /* 0 = not built, 1 = built, 2 = unavailable */
  YaffImportLib *import_libs; /* distinct export tables */
  int nb_import_libs;
  int nb_import_libs_read;    /* libraries read, duplicates included */
  char *import_libtcc1;           /* path of the libtcc1.a whose index was read, or NULL */
  char *import_fp_archive;       /* fp/lib<runtime>.a when the FP runtime links statically, or NULL */

  /* include paths */
  char **include_paths;
  int nb_include_paths;

  char **sysinclude_paths;
  int nb_sysinclude_paths;

  /* what each of those directories holds, read on first use (tccpp.c) */
  struct IncDirList **inc_dir_lists;
  int nb_inc_dir_lists;

  /* library paths */
  char **library_paths;
  int nb_library_paths;

  /* crt?.o object path */
  char **crt_paths;
  int nb_crt_paths;

  /* -D / -U options */
  CString cmdline_defs;
  /* -include options */
  CString cmdline_incl;

  /* error handling */
  void *error_opaque;
  void (*error_func)(void *opaque, const char *msg);
  int error_set_jmp_enabled;
  jmp_buf error_jmp_buf;
  int nb_errors;
  /* Error recovery (frontend/gen/core/recover.c): an error goes on to the
   * next external declaration, or the next statement of a function body,
   * until max_errors (-fmax-errors=N, 0 = no limit) have been reported. */
  int max_errors;
  unsigned char max_errors_defer, max_errors_hit; /* tcc_max_errors_defer / _stop */
  struct TCCRecover *recover;  /* the innermost armed recovery point, or NULL */
  unsigned char error_fatal;   /* the error being raised leaves no state to go on from */
  unsigned char recover_syntax_only; /* syntax_only before the first error turned it on */
  unsigned char recover_flipped;     /* the unit went on as -fsyntax-only after an error */
  unsigned char asm_inline;          /* assembling an asm statement's text (tccasm.c) */
  /* The diagnostics' order (driver/diag_out.c): each has an anchor, the
   * file-scope declaration it belongs to in -O0's parse order.  A body
   * parsed away from its place (a deferred function, a static's saved
   * initializer, a static inline, an inline expansion) speaks with its own
   * definition's anchor.  From the first body saved for later on
   * (diag_queue_on), the unit's diagnostics are held and written at its end
   * in anchor order: -O0's order at every -O. */
  int diag_anchor, diag_decl_seq;
  unsigned char diag_queue_on;
  unsigned char diag_in_parse;   /* the file-scope parse, not the end of the unit's replays */
  unsigned char diag_stop;       /* a fatal error or the -fmax-errors cap stopped the parse: the bodies before it still speak */
  int diag_stop_anchor, diag_stop_seq;
  int diag_queue_base_errors;    /* the errors written before the queue began */
  unsigned *err_seen;          /* hashes of the errors printed in this unit: a replay says nothing twice */
  int nb_err_seen, err_seen_mask;
  int *undecl_toks;            /* the undeclared names reported in the current function */
  int nb_undecl_toks, undecl_cap;
  const void *undecl_func;     /* ... which is this one (its recovery point) */
  /* a function body being saved as tokens (decl.c save_function_body): an
     error the lexer raises, or the end of the input, cuts the body there and
     the saved part is parsed at once -- the errors come where -O0 says them */
  jmp_buf *save_body_jmp;
  char *save_body_msg;         /* the lexer's error, said when the parse reaches it */
  int save_body_line;
  struct DeferredFunc *save_body_deferred; /* defer_function_body's entry while its body is
                                              saved: an error there ends the compile, and
                                              tccgen_finish frees it */
  /* errors said where the parse reaches their marker (tcc_error_stash): a
     macro used with the wrong number of arguments, a number's error in a
     body being saved as tokens */
  struct DiagStash *diag_stash;
  int nb_diag_stash, diag_stash_cap;
  struct TypeSig *typesigs; /* -Wcross-tu: the link's type signatures (obj/tccelf_load.c) */
  int nb_typesigs;
  /* -Wtainted-format across units: the units' ".tcc.taint" records, for
     the link (ir/taint.c) */
  unsigned char *taint_link;
  size_t taint_link_size;
  const char *diag_link_file; /* a diagnostic of the link about a unit's line: its file */

  /* output file for preprocessing (-E) */
  FILE *ppfp;

  /* for -MD/-MF: collected dependencies for this compilation */
  char **target_deps;
  int nb_target_deps;

  /* compilation */
  BufferedFile *include_stack[INCLUDE_STACK_SIZE];
  BufferedFile **include_stack_ptr;

  int ifdef_stack[IFDEF_STACK_SIZE];
  int *ifdef_stack_ptr;

  /* included files enclosed with #ifndef MACRO */
  int cached_includes_hash[CACHED_INCLUDES_HASH_SIZE];
  CachedInclude **cached_includes;
  int nb_cached_includes;

  /* #pragma pack stack */
  int pack_stack[PACK_STACK_SIZE];
  int *pack_stack_ptr;
  /* #pragma GCC visibility push/pop: STV_* values, depth 0 when none */
  unsigned char visibility_stack[VISIBILITY_STACK_SIZE];
  int visibility_stack_depth;
  char **pragma_libs;
  int nb_pragma_libs;

  /* inline functions are stored as token lists and compiled last
     only if referenced */
  struct InlineFunc **inline_fns;
  int nb_inline_fns;
  /* inline_fns by symbol token, so a call site finds its callee's body without
   * scanning every entry (inline_fn_lookup): an open-addressing table keyed by
   * token, inline_fn_by_tok_size slots (a power of two), _count in use. */
  struct InlineFnSlot *inline_fn_by_tok;
  int inline_fn_by_tok_size;
  int inline_fn_by_tok_count;
  int inline_fn_indexed; /* inline_fns entries already in inline_fn_by_tok */
  /* One per SymAttr.guard_head symbol (docs/inline_guard_heads.md). */
  struct InlineFunc **guard_heads;
  int nb_guard_heads;
  struct InlineFnSlot *guard_head_by_tok; /* guard_heads by symbol token, as inline_fn_by_tok */
  int guard_head_by_tok_size;
  /* Function bodies saved at their definition for end-of-TU generation
   * (-finline-functions-called-once, gen_deferred_function_bodies). */
  struct DeferredFunc **deferred_fns;
  int nb_deferred_fns;
  /* Initialized static objects saved for -fdrop-unused-statics. */
  struct DeferredData **deferred_data;
  int nb_deferred_data;
  /* The statics prune_unused_statics dropped: parsed at the end of the TU for
   * their diagnostics only (check_dropped_statics), never emitted. */
  struct DeferredFunc **dropped_fns;
  int nb_dropped_fns;
  struct DeferredData **dropped_data;
  int nb_dropped_data;
  /* > 0 while a dropped static is parsed for its diagnostics: nothing it
   * produces may reach the object -- no data (NODATA_WANTED), no symbol
   * (put_extern_sym, get_sym_ref), no relocation (greloca).  Its code goes to
   * an IR that is freed, its static objects to check_scratch. */
  int check_only;
  struct Section *check_scratch;
  /* Objects declared without an initializer at file scope, settled at the end
   * of the TU (finalize_tentative_definitions). */
  struct Sym **tentative_syms;
  int nb_tentative_syms;
  /* The whole TU has been parsed: what is generated from here on (deferred
   * bodies, owed inline bodies) sees every declaration and every use. */
  int tu_parsed;
  /* The first symbol this TU added: those before are the objects' and the
   * earlier TUs' (gc_unreferenced_statics drops this TU's only). */
  int tu_first_sym;
  /* Body length (saved-token ints) the function being generated may still
   * absorb by expanding called-once functions (called_once_budget_begin). */
  int called_once_budget;
  /* The deferred body being generated and its DeferredFunc.call_sites, for
   * gen_function's few-callers inline class (define_deferred_function). */
  struct Sym *deferred_sym;
  int deferred_call_sites;
  /* The function body being compiled: its token length (the giant-caller rule)
   * and a generation counter that resets per-caller expansion counts. */
  int inline_caller_len, inline_caller_gen;

  /* Current function symbol being compiled.  Set by gen_function so IR opt
   * passes can mark the function for end-of-TU re-optimization. */
  Sym *cur_func_sym;
  /* When set, tcc_ir_opt_global_init_prop bypasses the VT_CONSTANT gate.
   * Set only during the end-of-TU late_reopt pass, when possibly_written
   * reflects the entire TU. */
  int ir_late_reopt_phase;
  int ir_post_float_narrow;

  /* Inline-eval parameter overlay: during try_inline_const_eval, identifier
   * resolution substitutes these SValues when a token matches a param token.
   * This preserves the caller's full SValue (sym + offset + type) for VT_SYM
   * pointer arguments so that `*p` in the callee body can see the underlying
   * global and potentially fold to its initializer. */
  int inline_eval_overlay_n; /* number of active overlay entries (0 when inactive) */
  int inline_eval_overlay_tok[8];
  SValue inline_eval_overlay_sv[8];

  /* __builtin_va_arg_pack() context: when expanding a clone of an
     always_inline variadic function, this points to the token stream
     of the caller's variadic arguments (comma-separated).  NULL when
     not inside such an expansion. */
  TokenString *va_arg_pack_tokens;

  /* sections */
  Section **sections;
  int nb_sections; /* number of sections, including first dummy section */

  /* Hash table for fast section name lookup (avoids linear scan) */
  Section **section_ht;         /* open-addressing hash table: name → Section* */
  unsigned int section_ht_mask; /* table size - 1 (power of 2) */
  int section_ht_count;         /* number of entries */

  /* Tracking list of UNDEF symbol indices in symtab for fast alacarte
     resolution.  Append-only; entries may become defined later. */
  int *undef_sym_list;
  int nb_undef_syms;
  int undef_sym_alloc;

  Section **priv_sections;
  int nb_priv_sections; /* number of private sections */

  /* predefined sections */
  Section *text_section, *data_section, *rodata_section, *bss_section;
  Section *common_section;
  Section *cur_text_section; /* current section where function code is generated */
#ifdef CONFIG_TCC_BCHECK
  /* bound check related sections */
  Section *bounds_section;  /* contains global data bound description */
  Section *lbounds_section; /* contains local data bound description */
#endif
  /* symbol section */
  union
  {
    Section *symtab_section, *symtab;
  }; /* historical alias */
  /* temporary dynamic symbol sections (for dll loading) */
  Section *dynsymtab_section;
  /* exported dynamic symbol section */
  Section *dynsym;
  /* got & plt handling */
  Section *got, *plt;
  /* exception handling */
  Section *eh_frame_section;
  Section *eh_frame_hdr_section;
  unsigned long eh_start;
#if defined(TCC_TARGET_ARM_THUMB)
  Section *arm_exidx_section;
  Section *arm_extab_section;
#endif
  /* debug sections */
  Section *stab_section;
  Section *dwarf_info_section;
  Section *dwarf_abbrev_section;
  Section *dwarf_line_section;
  Section *dwarf_aranges_section;
  Section *dwarf_ranges_section;
  Section *dwarf_str_section;
  Section *dwarf_line_str_section;
  Section *dwarf_frame_section; /* .debug_frame: call-frame information (Thumb) */
  Section *dwarf_loclists_section; /* .debug_loclists: register-held variables (Thumb, DWARF 5) */
  int dwlo, dwhi; /* dwarf section range */
  /* test coverage */
  Section *tcov_section;
  /* debug state */
  struct _tccdbg *dState;

  /* Is there a new undefined sym since last new_undef_sym() */
  int new_undef_sym;
  /* Number of archive members loaded in current group rescan pass */
  int group_rescan_loaded;
  /* extra attributes (eg. GOT/PLT value) for symtab symbols */
  /* See tcc_link_arena_prepare. */
  unsigned char *link_arena;
  unsigned long link_arena_size, link_arena_used;
  struct sym_attr **sym_attr_chunks; /* see get_sym_attr */
  int nb_sym_attr_chunks;
  /* ptr to next reloc entry reused */
  ElfW_Rel *qrel;
#define qrel s1->qrel

#ifndef ELF_OBJ_ONLY
  int nb_sym_versions;
  struct sym_version *sym_versions;
  int nb_sym_to_version;
  int *sym_to_version;
  int dt_verneednum;
  Section *versym_section;
  Section *verneed_section;
#endif

  /* benchmark info */
  int total_idents;
  int total_lines;
  unsigned int total_bytes;
  unsigned int total_output[4];
  /* All bench_*_time accumulators are microseconds. They were milliseconds
     until the corpus-wide profile needed them: the per-function seams are
     charged once per function, so truncating each to whole ms lost most of the
     time on any source made of small functions -- which is most of the
     suite. */
  unsigned int bench_file_open_time;
  unsigned int bench_file_open_count;
  unsigned int bench_library_resolve_time;
  unsigned int bench_library_resolve_count;
  unsigned int bench_compile_setup_time;
  unsigned int bench_compile_setup_count;
  unsigned int bench_compile_exec_time;
  unsigned int bench_compile_exec_count;
  unsigned int bench_compile_finalize_time;
  unsigned int bench_compile_finalize_count;
  unsigned int bench_function_body_time;
  unsigned int bench_function_body_count;
  unsigned int bench_function_opt_time;
  unsigned int bench_function_opt_count;
  unsigned int bench_function_alloc_time;
  unsigned int bench_function_alloc_count;
  unsigned int bench_function_codegen_time;
  unsigned int bench_function_codegen_count;
  unsigned int bench_compile_time;
  unsigned int bench_compile_count;
  unsigned int bench_object_load_time;
  unsigned int bench_object_load_count;
  unsigned int bench_archive_load_time;
  unsigned int bench_archive_load_count;
  unsigned int bench_archive_member_count;
  unsigned int bench_dll_load_time;
  unsigned int bench_dll_load_count;
  unsigned int bench_ldscript_load_time;
  unsigned int bench_ldscript_load_count;
  unsigned int bench_output_time;
  unsigned int bench_output_count;
  /* Split of bench_output_time, in microseconds: output averages 32 ms a
     compile and 10% of tcc's own clock, which is too much to leave as one
     number. Millisecond resolution would round most of these to 0/1, hence us.
     runtime = pulling in libc/libtcc1, got/layout/reloc = link steps,
     write = emitting the file. */
  unsigned int bench_output_runtime_us;
  unsigned int bench_output_got_us;
  unsigned int bench_output_layout_us;
  unsigned int bench_output_reloc_us;
  unsigned int bench_output_write_us;

  /* option -dnum (for general development purposes) */
  int g_debug;

  /* used by tcc_load_ldscript */
  int fd, cc;

  /* for warnings/errors for object files */
  const char *current_filename;
  /* Archive member offset for lazy loading (0 if not in archive) */
  unsigned long current_archive_offset;
  /* Archive file path for lazy loading (NULL if not in archive) */
  const char *current_archive_path;
  /* Paths of files with deferred section chunks, shared by those chunks. */
  char **deferred_paths;
  int nb_deferred_paths;
  int **reloc_remaps; /* per object with LazyRels: its symbol remap, until coalesce */
  int nb_reloc_remaps;
  /* Cached archive symbol tables for --start-group rescan avoidance */
  ArchiveSymbolCache *archive_sym_caches;
  int nb_archive_sym_caches;

  /* Phase 2: Garbage Collection During Loading */
  LazyObjectFile **lazy_objfiles; /* Array of lazy-loaded object files */
  int nb_lazy_objfiles;           /* Number of lazy object files */
  int gc_sections_aggressive;     /* Enable aggressive GC during loading */

  /* used by main and tcc_parse_args only */
  struct filespec **files; /* files seen on command line */
  int nb_files;            /* number thereof */
  int nb_libraries;        /* number of libs thereof */
  char *outfile;           /* output filename */
  char *spill_path;        /* during a link: where a spilled .text goes, see tccelf.c */
  char *deps_outfile;      /* option -MF */
  int argc;
  char **argv;
  CString linker_arg; /* collect -Wl options */
  int thumb_func;
  TCCIRState *ir;
  /* Nested functions - saved token streams for functions defined inside other functions */
  /* Entries are allocated one by one and the slots reused across parents, so
   * a NestedFunc * (parent_nf, current_nested_func, a caller's local) stays
   * valid while a nested function registered mid-compilation grows the array. */
  NestedFunc **nested_funcs;
  int nb_nested_funcs;
  int nested_funcs_capacity;
  int had_nested_funcs;
  NestedFunc *current_nested_func; /* nested func currently being compiled */
  int rt_num_callers;
  int parameters_registers;
  int registers_for_allocator;
  uint64_t registers_map_for_allocator;
  uint8_t float_registers_for_allocator;
  uint64_t float_registers_map_for_allocator;
  uint8_t omit_frame_pointer;
  uint8_t need_frame_pointer;
  uint8_t force_frame_pointer;  /* required for VLA/dynamic SP even if omit_frame_pointer */
  uint8_t func_dynamic_sp;      /* function contains VLA_ALLOC: SP moves at runtime, so
                                   SP-relative frame slots (nested-call save area) must be
                                   addressed FP-relative instead */
  uint8_t force_lr_save;        /* __builtin_return_address needs LR saved even in leaf */
  uint8_t func_save_apply_args; /* __builtin_apply_args: save r0-r3 in prologue */
  int apply_args_offset;        /* stack offset of saved r0-r3 block for apply_args */
  int stack_location;

  /* Inline expansion state: when replaying an inline function's token
     stream at a call site, these track the return value destination. */
  uint8_t in_inline_expansion; /* nonzero while expanding inline body */
  uint8_t inline_expansion_depth; /* nested expansion depth, capped to bound work */
  /* The header replaying the body of the function being compiled from its own
   * saved tokens (tok_str_borrow): a call inside it may still inline the
   * function, as when the body was replayed from a copy. */
  struct TokenString *compile_replay;
  /* The saved body define_deferred_function is replaying; deferred_adopted is
   * 1 once save_function_body made it the function's kept tokens, 2 if those
   * were then dropped (freed when the replay ends). */
  struct TokenString *deferred_body;
  int deferred_adopted;
  /* The functions whose bodies are being expanded, outermost first: a callee
   * already here is recursion, never expanded again. */
  struct Sym *inline_expansion_syms[INLINE_NEST_MAX];
  /* Tokens nested auto-inline expansions may still add to the function being
   * compiled (reset by gen_function): helpers calling helpers can fan out. */
  int nested_inline_budget;
  /* r0/r1-returning internal functions whose address something takes, and
   * the AAPCS adapter each address names (gfunc_regpair_address_sym). */
  struct Sym **regpair_fns, **regpair_adapters;
  int nb_regpair_fns, nb_regpair_adapters;
  /* The function a direct r0/r1 call is being put for: its call target keeps
   * the function itself, not the adapter. */
  struct Sym *regpair_direct_call;
  /* Every name the asm statements of the saved function bodies spell out
   * (gfunc_regpair_note_asm_names): asm run after the TU is parsed must not
   * meet a function already decided r0/r1. */
  char **regpair_asm_names;
  int nb_regpair_asm_names;
  /* The small leaves expanded at the first level of the function being
   * compiled (reset by gen_function): their count, summed cost, backward jumps
   * and per-caller-bounded ones, for sizing a wrapper by the ops it adds
   * (ir_body_wraps_small_leaf). */
  int inline_leaf_sites, inline_leaf_ops, inline_leaf_backedges, inline_leaf_bounded;
  int inline_return_loc;       /* stack offset for storing return value */
  int inline_return_vr;        /* vreg bound to that slot for scalar returns,
                                * -1 when the return goes through memory only.
                                * Bound like a compiler-generated local so the
                                * allocator can keep the inlined return value in
                                * a register instead of a frame round trip. */
  uint8_t inline_return_redirected; /* a struct `return <local>;` already
                                     * retargeted inline_return_loc; any later
                                     * return in the same body must COPY into
                                     * that slot instead of retargeting again */
  uint8_t inline_return_copied; /* a struct return already copied into the slot: no later return may retarget it */
  int inline_const_arg_count;  /* constant-like current inline params */

  /* Named Return Value Optimization (NRVO) target.
     When set, an upcoming function call returning a struct/complex via
     hidden sret pointer should use this stack slot as its return buffer
     instead of allocating a fresh temp.  Saves the temp + the temp→dst
     copy.  Active during evaluation of a single initializer expression. */
  uint8_t nrvo_target_active;
  int nrvo_target_loc;   /* stack offset of destination */
  int nrvo_target_vreg;  /* vreg of destination variable */
  int nrvo_target_size;  /* size in bytes — must match function return size */
  int nrvo_target_align; /* alignment — must match */
  /* Alternate destination form: when >= 0, the destination is addressed
     through this vreg (a register-deref lvalue, e.g. the LHS of
     `local.field = sret_call(...)` whose address was materialized by an
     LEA).  Takes precedence over nrvo_target_loc when set. */
  int nrvo_target_ptr_vreg;
  struct
  {
    int vreg;
    int stack_offset;
    SValue value;
  } inline_const_args[16];

  /* Outermost VLA parameter expressions: saved token streams for evaluating
     side effects at function entry (C11 6.9.1p10). Stored separately from Sym
     because the sym union field (vla_array_str/next) would corrupt the type chain. */
  struct VlaParamExpr
  {
    Sym *param;  /* the parameter sym (used for identification) */
    int *tokens; /* heap-allocated token stream */
  } *vla_param_exprs;
  int nb_vla_param_exprs;

  /* Inner (nested) VLA dimension token streams saved on a SYM_FIELD's
     vla_array_str.  Materialization (func_vla_arg_code) frees and NULLs them at
     a function definition's entry, but an inner VLA inside an abstract /
     function-pointer declarator (e.g. a typedef `void(*)(int[][n()])`) is never
     materialized, so its heap token stream would leak.  Tracked here so any
     unconsumed buffer is reclaimed at end of translation unit. */
  int **vla_inner_exprs;
  int nb_vla_inner_exprs;

  /* linker script support */
  char *linker_script;        /* path to linker script file (-T option) */
  struct LDScript *ld_script; /* parsed linker script */

  /* Deferred label-difference fixups for static initializers like
     static int b[] = { &&lab1 - &&lab0, ... };
     These are recorded during parsing and resolved after codegen
     when label ELF symbol values are known. */
  struct LabelDiffFixup *label_diff_fixups;
};

/* Reads of the optimization level and the -f<opt> flags.  A compiler built
 * with CONFIG_TCC_O0_ONLY (./configure --disable-optimizer: a native tcc where
 * its own size matters more than optimizing) sees them all as constant 0: every pass gated on
 * them folds out of its build and --gc-sections drops the optimizer.  -O0
 * output is unchanged, since every one of them is 0 at -O0 already. */
#ifdef CONFIG_TCC_O0_ONLY
#define TCC_OPT(s, field) 0
#else
#define TCC_OPT(s, field) ((s)->field)
#endif


/* Whether built-in bound checking is on; constant 0 in a compiler built
 * without it (CONFIG_TCC_BCHECK=0, as the YasOS cross is), where the
 * do_bounds_check member does not exist. */
#ifdef CONFIG_TCC_BCHECK
#define tcc_bounds_checking(s) ((s)->do_bounds_check)
#else
#define tcc_bounds_checking(s) 0
#endif

/* String/memory builtin IDs for table-driven dispatch.
 * Used by tccgen.c and ir/opt.c to avoid repeated strcmp. */
enum StrBuiltinId
{
  STRBI_UNKNOWN = 0,
  STRBI_STRLEN,
  STRBI_STRNLEN,
  STRBI_STRCMP,
  STRBI_STRNCMP,
  STRBI_STRCPY,
  STRBI_STRNCPY,
  STRBI_STPCPY,
  STRBI_STPNCPY,
  STRBI_STRCAT,
  STRBI_STRNCAT,
  STRBI_STRCHR,
  STRBI_STRRCHR,
  STRBI_STRSTR,
  STRBI_STRPBRK,
  STRBI_STRCSPN,
  STRBI_STRSPN,
  STRBI_MEMCMP,
  STRBI_MEMCMP_EQ,
  STRBI_MEMCHR,
  STRBI_MEMMOVE,
  STRBI_BCOPY,
  STRBI_MEMPCPY,
  STRBI_INDEX,
  STRBI_RINDEX,
  STRBI_FORTIFY_STR, /* -ffortify's string check: const_string_fold only
                        (resolve_str_builtin_id does not know it) */
  STRBI__COUNT,
};

/* A deferred fixup for a label-difference expression (&&sym1 - &&sym2)
   used in a static initializer.  Recorded during parsing, resolved
   after code generation when both label symbols have their final
   code offsets. */
typedef struct LabelDiffFixup
{
  Section *sec;          /* data section containing the value */
  unsigned long offset;  /* byte offset within sec->data */
  int size;              /* bytes stored there: 1, 2 or 4 */
  struct Sym *sym_plus;  /* positive label symbol (&&lab1) */
  struct Sym *sym_minus; /* negative label symbol (&&lab0) */
  struct LabelDiffFixup *next;
} LabelDiffFixup;

/* Forward declaration for linker script */
struct LDScript;

struct filespec
{
  int type;
  char name[1];
};

/* The current value can be: */
#define VT_VALMASK 0x001F /* mask for value location (bits 0-6 of r field) */
#define VT_CONST 0x0010   /* constant in vc */
#define VT_LLOCAL 0x0011  /* lvalue, offset on stack */
#define VT_LOCAL 0x0012   /* offset on stack */
#define VT_CMP 0x0013     /* the value is stored in processor flags (in vc) */
#define VT_JMP 0x0014     /* value is the consequence of jmp true (even) */
#define VT_JMPI 0x0015    /* value is the consequence of jmp false (odd) */
#define VT_PARAM 0x0020   /* register allocation */
#define VT_LVAL 0x0040    /* var is an lvalue */
#define VT_SYM 0x0080     /* a symbol value is added */
#define VT_MUSTCAST                                                                                                    \
  0x0100 /* value must be casted to be correct (used for                                                               \
            char/short stored in integer registers) */
#define VT_NONCONST                                                                                                    \
  0x0200 /* VT_CONST, but not an (C standard) integer                                                                  \
            constant expression */
#define VT_MUSTBOUND                                                                                                   \
  0x0400 /* bound checking must be done before                                                                         \
            dereferencing value */
#define VT_BOUNDED                                                                                                     \
  0x0800 /* value is bounded. The address of the                                                                       \
            bounding function call point is in vc */

/* Legacy inline wrappers - for compatibility */
static inline SValue tcc_svalue_const_i64(int64_t v)
{
  return svalue_const_i64(v);
}

static inline SValue tcc_ir_svalue_call_id(int call_id)
{
  return svalue_call_id(call_id);
}

static inline SValue tcc_ir_svalue_call_id_argc(int call_id, int argc)
{
  return svalue_call_id_argc(call_id, argc);
}
/* types */
#define VT_BTYPE 0x000f /* mask for basic type */
#define VT_VOID 0       /* void type */
#define VT_BYTE 1       /* signed byte type */
#define VT_SHORT 2      /* short type */
#define VT_INT 3        /* integer type */
#define VT_LLONG 4      /* 64 bit integer */
#define VT_PTR 5        /* pointer */
#define VT_FUNC 6       /* function type */
#define VT_STRUCT 7     /* struct/union definition */
#define VT_FLOAT 8      /* IEEE float */
#define VT_DOUBLE 9     /* IEEE double */
#define VT_LDOUBLE 10   /* IEEE long double */
#define VT_BOOL 11      /* ISOC99 boolean type */
#define VT_QLONG 13     /* 128-bit integer. Only used for x86-64 ABI */
#define VT_QFLOAT 14    /* 128-bit float. Only used for x86-64 ABI */

#define VT_UNSIGNED 0x0010 /* unsigned type */
#define VT_DEFSIGN 0x0020  /* explicitly signed or unsigned */
#define VT_ARRAY 0x0040    /* array type (also has VT_PTR) */
#define VT_BITFIELD 0x0080 /* bitfield modifier */
#define VT_CONSTANT 0x0100 /* const modifier */
#define VT_VOLATILE 0x0200 /* volatile modifier */
#define VT_VLA 0x0400      /* VLA type (also has VT_PTR and VT_ARRAY) */
#define VT_LONG 0x0800     /* long type (also has VT_INT rsp. VT_LLONG) */
/* storage */
#define VT_EXTERN 0x00001000  /* extern definition */
#define VT_STATIC 0x00002000  /* static variable */
#define VT_TYPEDEF 0x00004000 /* typedef definition */
#define VT_INLINE 0x00008000  /* inline definition */
#define VT_COMPLEX 0x00010000 /* Complex type flag (bit 16) */
#define VT_VECTOR 0x00020000  /* GCC vector type flag (bit 17): element type in sym->type, total bytes in sym->c */
#define VT_RODATA_REL 0x00040000 /* __rodata_relative pointer: stored as an offset into .rodata (rodata_rel.c) */
/* currently unused: 0x00080000  */

#define VT_STRUCT_SHIFT 20 /* shift for bitfield shift values (32 - 2*6) */
#define VT_STRUCT_MASK (((1U << (6 + 6)) - 1) << VT_STRUCT_SHIFT | VT_BITFIELD)
#define BIT_POS(t) (((t) >> VT_STRUCT_SHIFT) & 0x3f)
#define BIT_SIZE(t) (((t) >> (VT_STRUCT_SHIFT + 6)) & 0x3f)

#define VT_UNION (1 << VT_STRUCT_SHIFT | VT_STRUCT)
#define VT_ENUM (2 << VT_STRUCT_SHIFT)     /* integral type is an enum really */
#define VT_ENUM_VAL (3 << VT_STRUCT_SHIFT) /* integral type is an enum constant really */

#define IS_ENUM(t) ((t & VT_STRUCT_MASK) == VT_ENUM)
#define IS_ENUM_VAL(t) ((t & VT_STRUCT_MASK) == VT_ENUM_VAL)
#define IS_UNION(t) ((t & (VT_STRUCT_MASK | VT_BTYPE)) == VT_UNION)

#define VT_ATOMIC VT_VOLATILE

/* type mask (except storage) */
#define VT_STORAGE (VT_EXTERN | VT_STATIC | VT_TYPEDEF | VT_INLINE)
#define VT_TYPE (~(VT_STORAGE | VT_STRUCT_MASK))

/* symbol was created by tccasm.c first */
#define VT_ASM (VT_VOID | 1 << VT_STRUCT_SHIFT)
#define VT_ASM_FUNC (VT_ASM | 2 << VT_STRUCT_SHIFT)
#define IS_ASM_SYM(sym) (((sym)->type.t & (VT_BTYPE | VT_ASM)) == VT_ASM)

/* general: set/get the pseudo-bitfield value for bit-mask M */
#define BFVAL(M, N) ((unsigned)((M) & ~((M) << 1)) * (N))
#define BFGET(X, M) (((X) & (M)) / BFVAL(M, 1))
#define BFSET(X, M, N) ((X) = ((X) & ~(M)) | BFVAL(M, N))

/* token values */

/* conditional ops */
#define TOK_LAND 0x90
#define TOK_LOR 0x91
/* warning: the following compare tokens depend on i386 asm code */
#define TOK_ULT 0x92
#define TOK_UGE 0x93
#define TOK_EQ 0x94
#define TOK_NE 0x95
#define TOK_ULE 0x96
#define TOK_UGT 0x97
#define TOK_Nset 0x98
#define TOK_Nclear 0x99
#define TOK_LT 0x9c
#define TOK_GE 0x9d
#define TOK_LE 0x9e
#define TOK_GT 0x9f

#define TOK_ISCOND(t) (t >= TOK_LAND && t <= TOK_GT)

#define TOK_DEC 0x80    /* -- */
#define TOK_MID 0x81    /* inc/dec, to void constant */
#define TOK_INC 0x82    /* ++ */
#define TOK_UDIV 0x83   /* unsigned division */
#define TOK_UMOD 0x84   /* unsigned modulo */
#define TOK_PDIV 0x85   /* fast division with undefined rounding for pointers */
#define TOK_UMULL 0x86  /* unsigned 32x32 -> 64 mul */
#define TOK_SMULL 0x9a  /* signed 32x32 -> 64 mul */
#define TOK_ADDC1 0x87  /* add with carry generation */
#define TOK_ADDC2 0x88  /* add with carry use */
#define TOK_SUBC1 0x89  /* add with carry generation */
#define TOK_SUBC2 0x8a  /* add with carry use */
#define TOK_SHL '<'     /* shift left */
#define TOK_SAR '>'     /* signed shift right */
#define TOK_SHR 0x8b    /* unsigned shift right */
#define TOK_NEG TOK_MID /* unary minus operation (for floats) */

#define TOK_ARROW 0xa0                         /* -> */
#define TOK_DOTS 0xa1                          /* three dots */
#define TOK_TWODOTS 0xa2                       /* C++ token ? */
#define TOK_TWOSHARPS 0xa3                     /* ## preprocessing token */
#define TOK_PLCHLDR 0xa4                       /* placeholder token as defined in C99 */
#define TOK_PPJOIN (TOK_TWOSHARPS | SYM_FIELD) /* A '##' in a macro to mean pasting */
#define TOK_SOTYPE 0xa7                        /* alias of '(' for parsing sizeof (type) */
#define TOK_EMBED_RESUME 0xa5                  /* #embed pushed its tokens: read them */

/* assignment operators */
#define TOK_A_ADD 0xb0
#define TOK_A_SUB 0xb1
#define TOK_A_MUL 0xb2
#define TOK_A_DIV 0xb3
#define TOK_A_MOD 0xb4
#define TOK_A_AND 0xb5
#define TOK_A_OR 0xb6
#define TOK_A_XOR 0xb7
#define TOK_A_SHL 0xb8
#define TOK_A_SAR 0xb9

#define TOK_ASSIGN(t) (t >= TOK_A_ADD && t <= TOK_A_SAR)
#define TOK_ASSIGN_OP(t) ("+-*/%&|^<>"[t - TOK_A_ADD])

/* tokens that carry values (in additional token string space / tokc) --> */
#define TOK_CCHAR 0xc0 /* char constant in tokc */
#define TOK_LCHAR 0xc1
#define TOK_CINT 0xc2    /* number in tokc */
#define TOK_CUINT 0xc3   /* unsigned int constant */
#define TOK_CLLONG 0xc4  /* long long constant */
#define TOK_CULLONG 0xc5 /* unsigned long long constant */
#define TOK_CLONG 0xc6   /* long constant */
#define TOK_CULONG 0xc7  /* unsigned long constant */
#define TOK_STR 0xc8     /* pointer to string in tokc */
#define TOK_LSTR 0xc9
#define TOK_CFLOAT 0xca     /* float constant */
#define TOK_CDOUBLE 0xcb    /* double constant */
#define TOK_CLDOUBLE 0xcc   /* long double constant */
#define TOK_CFLOAT_I 0xcd   /* imaginary float constant (GNU ext) */
#define TOK_CDOUBLE_I 0xce  /* imaginary double constant (GNU ext) */
#define TOK_CLDOUBLE_I 0xcf /* imaginary long double constant (GNU ext) */
#define TOK_CINT_I 0xd0     /* imaginary integer constant (GNU ext) */
#define TOK_PPNUM 0xd1      /* preprocessor number */
#define TOK_PPSTR 0xd2      /* preprocessor string */
#define TOK_LINENUM 0xd3    /* line number info */
#define TOK_PACK_REPLAY 0xd4 /* deferred #pragma pack action embedded in a saved
                                token stream; tokc.i encodes (kind<<16)|value.
                                Applied (not emitted) when replayed via next(). */

#define TOK_HAS_VALUE(t) (t >= TOK_CCHAR && t <= TOK_PACK_REPLAY)

#define TOK_EOF (-1)    /* end of file */
#define TOK_LINEFEED 10 /* line feed */

/* all identifiers and strings have token above that */
#define TOK_IDENT 256

enum tcc_token
{
  TOK_LAST = TOK_IDENT - 1
#define DEF(id, str) , id
#include "source/frontend/tcctok.h"
#undef DEF
  /* Sentinel: one past the last builtin token.  The tcc_keywords blob holds
     every builtin token string in this same (enum) order, so the i-th blob
     entry has token id TOK_IDENT + i, and NB_BUILTIN_TOKS is exactly the
     number of builtin tokens.  Used by the lazy builtin-token interner. */
  , TOK_BUILTIN_END
};

/* number of reserved builtin token ids in [TOK_IDENT, TOK_IDENT+NB_BUILTIN_TOKS) */
#define NB_BUILTIN_TOKS (TOK_BUILTIN_END - TOK_IDENT)

/* keywords: tok >= TOK_IDENT && tok < TOK_UIDENT */
#define TOK_UIDENT TOK_DEFINE

/* STRBI_* id of a string builtin token / of a call by name (the __tcc_*
 * redirect names map back); STRBI_UNKNOWN otherwise.  Defined in
 * source/opt/util/const_string_eval.c. */
int resolve_str_builtin_by_tok(int tok);
int resolve_str_builtin_id(int tok, const char *name);

/* Returns 1 if this STRBI id is a "simple redirect" target that the IR
   optimizer will replace with a __tcc_* helper call.  Inlining these
   functions defeats the redirect and can preserve unwanted side-effects
   (e.g. test-harness abort checks) that the helper avoids. */
static inline int strbi_is_redirect_target(int id)
{
  switch (id)
  {
  case STRBI_MEMMOVE:
  case STRBI_BCOPY:
  case STRBI_MEMPCPY:
  case STRBI_STRCAT:
  case STRBI_STRCHR:
  case STRBI_INDEX:
  case STRBI_STRCPY:
  case STRBI_STPCPY:
  case STRBI_STPNCPY:
  case STRBI_STRNLEN:
  case STRBI_STRPBRK:
  case STRBI_STRRCHR:
  case STRBI_RINDEX:
  case STRBI_STRSTR:
  case STRBI_STRCSPN:
  case STRBI_STRNCPY:
  case STRBI_STRNCAT:
    return 1;
  default:
    return 0;
  }
}

/* ------------ libtcc.c ------------ */

ST_DATA struct TCCState *tcc_state;
ST_DATA void **stk_data;
ST_DATA int nb_stk_data;

/* public functions currently used by the tcc main function */
ST_FUNC char *pstrcpy(char *buf, size_t buf_size, const char *s);
ST_FUNC char *pstrcat(char *buf, size_t buf_size, const char *s);
ST_FUNC char *pstrncpy(char *out, const char *in, size_t num);
PUB_FUNC char *tcc_basename(const char *name);
PUB_FUNC char *tcc_fileextension(const char *name);

/* all allocations - even MEM_DEBUG - use these */
PUB_FUNC void tcc_free(void *ptr);
PUB_FUNC void *tcc_malloc(unsigned long size);
PUB_FUNC void *tcc_mallocz(unsigned long size);
PUB_FUNC void *tcc_realloc(void *ptr, unsigned long size);
PUB_FUNC char *tcc_strdup(const char *str);

/* Attribute mmap-class allocations (>= 2 KiB) to the source line that asked
   for them. A 376-line compile spends 304 KiB across 41 such calls, every one
   of them a syscall plus kernel page-zeroing, so knowing *which* structure
   they belong to is the whole question. Only the >= 2 KiB path records
   anything, and the record is one table probe, so this is cheap enough to keep
   on: the counters it feeds already print under -bench. */
/* CONFIG_TCC_CHECKS=0 is the device compiler (build_rootfs.sh): no record of who
 * allocated what for -bench, no contract checks on hot paths, ICEs that stop
 * without a message.  It runs from flash behind a 16 KB cache, where every call
 * site's extra arguments are fetched.  The host cross keeps all of it. */
#ifndef CONFIG_TCC_CHECKS
#define CONFIG_TCC_CHECKS 1
#endif
#if CONFIG_TCC_CHECKS || defined(TCC_ALLOC_PROFILE)
#define TCC_RECORD_ALLOC_SITES 1
#else
#define TCC_RECORD_ALLOC_SITES 0
#endif

#if !defined(MEM_DEBUG)
#if TCC_RECORD_ALLOC_SITES
#define tcc_malloc(size) tcc_malloc_at(size, __FILE__, __LINE__)
#define tcc_mallocz(size) tcc_mallocz_at(size, __FILE__, __LINE__)
#define tcc_realloc(ptr, size) tcc_realloc_at(ptr, size, __FILE__, __LINE__)
#endif
/* Growing arrays: tcc_grow_size(have, need) is the capacity in bytes for an
 * array of `have` bytes that needs `need`; tcc_fit_size rounds a size to what
 * the device's malloc hands out anyway.  Memory, not reallocs, is what runs
 * out on the board: doubling leaves a quarter of an array unused on average,
 * and a power-of-two size past the pools is the worst fit the device's malloc
 * has -- from 32 KB a block is a mapping of whole 4 KB pages behind a 16-byte
 * header, so 64 KB takes 68 KB.  So a large array grows by a quarter (an
 * eighth past 256 KB); a mapping usually grows in place (mremap), so the
 * smaller steps copy little.  Below 32 KB a block may be a pool block, which
 * costs its size in 8-byte steps: rounded to whole pages there, it became a
 * mapping of its own instead (libc pools exactly the sizes whose header would
 * spill onto a page), and a Zig kernel compile peaked 0.18 MB higher.  Small
 * arrays still double: they live in pools, where slack is a few bytes. */
static inline unsigned long tcc_fit_size(unsigned long size)
{
  if (size < 32768)
    return (size + 7) & ~7ul;
  return ((size + 16 + 4095) & ~4095ul) - 16;
}

static inline unsigned long tcc_grow_size(unsigned long have, unsigned long need)
{
  unsigned long n = have ? have : 16;
  while (n < need)
    n += n < 4096 ? n : n < 262144 ? n / 4 : n / 8;
  return tcc_fit_size(n);
}

/* A mark in the TCC_ALLOC_BACKTRACE trace (1: a function's compile starts,
 * 2: it ends); nothing in any other build. */
#ifdef TCC_ALLOC_BACKTRACE
PUB_FUNC void tcc_alloc_trace_mark(int what);
/* A pass starts (begin 1) or ends (0): a mark naming it (TRACE.passes). */
PUB_FUNC void tcc_alloc_trace_pass(const char *name, int begin);
#else
#define tcc_alloc_trace_mark(what) ((void)0)
#define tcc_alloc_trace_pass(name, begin) ((void)0)
#endif
PUB_FUNC void *tcc_malloc_at(unsigned long size, const char *file, int line);
PUB_FUNC void *tcc_mallocz_at(unsigned long size, const char *file, int line);
/* The IR arrays every pass walks: on the board, in memory that cannot miss (see
 * libtcc.c), and in the tracing cross that stands in for it (TCC_ALLOC_BACKTRACE).
 * Everywhere else, the plain heap. */
#if (defined(TCC_IS_NATIVE) && defined(TARGETOS_YasOS)) || defined(TCC_ALLOC_BACKTRACE)
#define TCC_HOT_ARENA 1
#else
#define TCC_HOT_ARENA 0
#endif
#if TCC_HOT_ARENA
PUB_FUNC void *tcc_hot_mallocz_at(unsigned long size, const char *file, int line);
PUB_FUNC void *tcc_hot_realloc_at(void *ptr, unsigned long size, const char *file, int line);
PUB_FUNC void tcc_hot_free(void *ptr);
/* Reseat a hot array at exactly `cap` bytes, keeping its first `used`: into the
 * arena if it now fits, so doubling slack is not what decides who gets SRAM. */
PUB_FUNC void *tcc_hot_settle_at(void *ptr, unsigned long used, unsigned long cap, const char *file, int line);
PUB_FUNC void tcc_memory_tiers_setup(void);
PUB_FUNC void tcc_hot_release(void);
#if TCC_RECORD_ALLOC_SITES
#define tcc_hot_mallocz(size) tcc_hot_mallocz_at(size, __FILE__, __LINE__)
#define tcc_hot_realloc(ptr, size) tcc_hot_realloc_at(ptr, size, __FILE__, __LINE__)
#define tcc_hot_settle(ptr, used, cap) tcc_hot_settle_at(ptr, used, cap, __FILE__, __LINE__)
#else
#define tcc_hot_mallocz(size) tcc_hot_mallocz_at(size, NULL, 0)
#define tcc_hot_realloc(ptr, size) tcc_hot_realloc_at(ptr, size, NULL, 0)
#define tcc_hot_settle(ptr, used, cap) tcc_hot_settle_at(ptr, used, cap, NULL, 0)
#endif
#else
#define tcc_hot_mallocz(size) tcc_mallocz(size)
#define tcc_hot_realloc(ptr, size) tcc_realloc(ptr, size)
#define tcc_hot_free(ptr) tcc_free(ptr)
#define tcc_hot_settle(ptr, used, cap) tcc_realloc(ptr, cap)
#define tcc_memory_tiers_setup() ((void)0)
#define tcc_hot_release() ((void)0)
#endif
PUB_FUNC void *tcc_realloc_at(void *ptr, unsigned long size, const char *file, int line);
#endif

#ifdef MEM_DEBUG
#define tcc_free(ptr) tcc_free_debug(ptr)
#define tcc_malloc(size) tcc_malloc_debug(size, __FILE__, __LINE__)
#define tcc_mallocz(size) tcc_mallocz_debug(size, __FILE__, __LINE__)
#define tcc_realloc(ptr, size) tcc_realloc_debug(ptr, size, __FILE__, __LINE__)
#define tcc_strdup(str) tcc_strdup_debug(str, __FILE__, __LINE__)
PUB_FUNC void tcc_free_debug(void *ptr);
PUB_FUNC void *tcc_malloc_debug(unsigned long size, const char *file, int line);
PUB_FUNC void *tcc_mallocz_debug(unsigned long size, const char *file, int line);
PUB_FUNC void *tcc_realloc_debug(void *ptr, unsigned long size, const char *file, int line);
PUB_FUNC char *tcc_strdup_debug(const char *str, const char *file, int line);
#endif

ST_FUNC void libc_free(void *ptr);
#define free(p) use_tcc_free(p)
#define malloc(s) use_tcc_malloc(s)
#define realloc(p, s) use_tcc_realloc(p, s)
#undef strdup
#define strdup(s) use_tcc_strdup(s)
PUB_FUNC int _tcc_error_noabort(const char *fmt, ...) PRINTF_LIKE(1, 2);
PUB_FUNC NORETURN void _tcc_error(const char *fmt, ...) PRINTF_LIKE(1, 2);
PUB_FUNC NORETURN void _tcc_ice(const char *fmt, ...) PRINTF_LIKE(1, 2);
PUB_FUNC NORETURN void _tcc_ice_stop(void);
PUB_FUNC void _tcc_warning(const char *fmt, ...) PRINTF_LIKE(1, 2);
PUB_FUNC void _tcc_warning_once(int woff, int line, const char *fmt, ...) PRINTF_LIKE(3, 4);
ST_FUNC void tcc_vwarning_once(int woff, int line, const char *fmt, va_list ap);
ST_FUNC int tcc_in_sys_header(void);
ST_FUNC void tcc_sys_mark_file(TCCState *s1, const char *filename, int line);
PUB_FUNC void _tcc_note(const char *fmt, ...) PRINTF_LIKE(1, 2);
/* A fix-it for the diagnostic just shown (dropped with it): WORD, found once
   on LINE (0: the diagnostic's) of its file, is replaced by TEXT, or TEXT goes
   before or after it.  Shown as -fdiagnostics-parseable-fixits lines, or in
   the JSON / SARIF output; nothing when the word is not there exactly once
   (tcc keeps no columns).  Give the note saying it first. */
#define FIXIT_REPLACE 0
#define FIXIT_BEFORE 1
#define FIXIT_AFTER 2
/* FIXIT_REPLACE, but not where WORD follows a ',': a declarator's head
   (`const * view b` cannot follow `char *a = s,`) */
#define FIXIT_REPLACE_FIRST 3
PUB_FUNC int tcc_fixit(int line, const char *word, int where, const char *text);
/* A call's fix-its, for the diagnostic just shown.  tcc_fixit_call() finds
   the call `NAME(` once on LINE (0: the diagnostic's) of its file, with its
   arguments up to ARG (from 0) on that line, and copies the first
   argument's text into BUF (BUFSZ bytes): 0 when it is not all there.  Look before the note,
   whose text may want the argument; then tcc_fixit_call_emit() after the
   note: RENAME replaces the name, APPEND goes after argument ARG (each may be
   NULL). */
PUB_FUNC int tcc_fixit_call(int line, const char *name, int arg, char *buf, int bufsz);
PUB_FUNC void tcc_fixit_call_emit(const char *rename, const char *append);
/* the note NOTE and that fix-it, given right after the next diagnostic: for
   an error, which aborts before its caller could add them.  Strings must live
   on (literals, token names). */
PUB_FUNC void tcc_fixit_next(int line, const char *word, int where, const char *text, const char *note);
/* after tcc_fixit_next(): a fix-it to give instead when its word is not on
   the line exactly once (WORD NULL: none); up to two, tried in turn */
PUB_FUNC void tcc_fixit_next_alt(const char *word, int where, const char *text);
#define tcc_internal_error(msg) tcc_error("internal compiler error in %s:%d: %s", __FUNCTION__, __LINE__, msg)

/* other utilities */
ST_FUNC void dynarray_add(void *ptab, int *nb_ptr, void *data);
ST_FUNC void dynarray_reset(void *pp, int *n);
ST_FUNC void tcc_qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
/* libc qsort orders ties differently on the host and the device: the self-host objects would differ */
#define qsort qsort_is_libc_dependent_use_tcc_qsort
ST_INLN void cstr_ccat(CString *cstr, int ch);
ST_FUNC void cstr_cat(CString *cstr, const char *str, int len);
ST_FUNC void cstr_wccat(CString *cstr, int ch);
ST_FUNC void cstr_new(CString *cstr);
ST_FUNC void cstr_free(CString *cstr);
ST_FUNC int cstr_printf(CString *cs, const char *fmt, ...) PRINTF_LIKE(2, 3);
ST_FUNC int cstr_vprintf(CString *cstr, const char *fmt, va_list ap);
ST_FUNC void cstr_reset(CString *cstr);
ST_FUNC void tcc_open_bf(TCCState *s1, const char *filename, int initlen);
ST_FUNC int tcc_open(TCCState *s1, const char *filename);
ST_FUNC void tcc_close(void);

/* mark a memory pointer on stack for cleanup after errors */
#define stk_push(p) dynarray_add(&stk_data, &nb_stk_data, p)
#define stk_pop() (--nb_stk_data)
/* mark CString on stack for cleanup errors */
#define cstr_new_s(cstr) (cstr_new(cstr), stk_push(&(cstr)->data))
#define cstr_free_s(cstr) (cstr_free(cstr), stk_pop())

ST_FUNC int tcc_add_file_internal(TCCState *s1, const char *filename, int flags);
/* flags: */
#define AFF_PRINT_ERROR 0x10    /* print error if file not found */
#define AFF_REFERENCED_DLL 0x20 /* load a referenced dll from another dll */
#define AFF_TYPE_BIN 0x40       /* file to add is binary */
#define AFF_WHOLE_ARCHIVE 0x80  /* load all objects from archive */
/* file list markers */
#define AFF_GROUP_START 0x100 /* begin --start-group */
#define AFF_GROUP_END 0x200   /* end --end-group */
/* s->filetype: */
#define AFF_TYPE_NONE 0
#define AFF_TYPE_C 1
#define AFF_TYPE_ASM 2
#define AFF_TYPE_ASMPP 4
#define AFF_TYPE_LIB 8
#define AFF_TYPE_MASK (15 | AFF_TYPE_BIN)
/* values from tcc_object_type(...) */
#define AFF_BINTYPE_REL 1
#define AFF_BINTYPE_DYN 2
#define AFF_BINTYPE_AR 3
#define AFF_BINTYPE_C67 4
#define AFF_BINTYPE_YAFF 5

/* return value of tcc_add_file_internal(): 0, -1, or FILE_NOT_FOUND */
#define FILE_NOT_FOUND -2

#ifndef ELF_OBJ_ONLY
ST_FUNC int tcc_add_crt(TCCState *s, const char *filename);
#endif
ST_FUNC int tcc_add_dll(TCCState *s, const char *filename, int flags);
ST_FUNC int tcc_add_support(TCCState *s1, const char *filename);
#ifdef CONFIG_TCC_BCHECK
ST_FUNC void tcc_add_bcheck(TCCState *s1);
#endif
#ifdef CONFIG_TCC_BACKTRACE
ST_FUNC void tcc_add_btstub(TCCState *s1);
#endif
ST_FUNC void tcc_add_pragma_libs(TCCState *s1);
PUB_FUNC int tcc_add_library_err(TCCState *s, const char *f);
PUB_FUNC void tcc_bench_log(TCCState *s1, const char *operation, const char *name, unsigned elapsed_us);
PUB_FUNC void tcc_init_stamp(const char *label);
PUB_FUNC void tcc_init_stamps_dump(void);
PUB_FUNC void tcc_print_stats(TCCState *s, unsigned total_time);
PUB_FUNC int tcc_parse_args(TCCState *s, int *argc, char ***argv, int optind);
#ifdef _WIN32
ST_FUNC char *normalize_slashes(char *path);
#endif
ST_FUNC DLLReference *tcc_add_dllref(TCCState *s1, const char *dllname, int level);
ST_FUNC char *tcc_load_text(int fd);
/* for #pragma once */
ST_FUNC int normalized_PATHCMP(const char *f1, const char *f2);

/* tcc_parse_args return codes: */
#define OPT_HELP 1
#define OPT_HELP2 2
#define OPT_V 3
#define OPT_PRINT_DIRS 4
#define OPT_AR 5
#define OPT_IMPDEF 6
#define OPT_YASC_C_HEADER 7
#define OPT_M32 32
#define OPT_M64 64

/* ------------ tccpp.c ------------ */

ST_DATA struct BufferedFile *file;
ST_DATA int tok;
ST_DATA CValue tokc;
ST_DATA unsigned char fe_tokfp_on;
ST_DATA uint64_t fe_tokfp_h, fe_tokfp_prev;
ST_DATA unsigned fe_tokfp_n;
ST_DATA const int *macro_ptr;
ST_DATA int parse_flags;
ST_DATA unsigned tok_macro_serial; /* begin_macro's count (error recovery) */
ST_DATA int pp_subst_depth;        /* > 0 while a macro is substituted */
ST_FUNC TokenString *tok_macro_stack(void);
ST_FUNC void tok_macro_restore(TokenString *stack, const int *ptr);
/* warnings in the expansion of a system header's macro are quiet (gcc): the
   last token read came from one -- a live expansion, or a range of a saved
   string being replayed */
ST_FUNC int tcc_sys_macro_here(void);
ST_FUNC int tcc_sys_macro_prev(void);
ST_FUNC int tok_str_sys_save(TokenString *to, int sys);   /* skip_or_save_block: per token */
ST_FUNC void tok_str_sys_copy(TokenString *from, TokenString *to);
ST_DATA int tok_sys_ranges;                               /* saved strings with such ranges */
ST_DATA int tok_flags;
ST_DATA CString tokcstr; /* current parsed string, if any */
/* When non-NULL (set by skip_or_save_block while recording a function body for
   later token-stream replay), #pragma pack directives are appended to this
   stream as TOK_PACK_REPLAY actions instead of mutating pack_stack now, so the
   pack state is applied at the struct's position during replay, not eagerly
   during the recording scan. */
ST_DATA TokenString *pp_pragma_capture;
/* yasc: is the token just read regular C, in a unit that has yasc in it?
   (TCCState.yasc follows the file and macro being read; tccpp.c) */
ST_FUNC int pp_in_c(void);
/* does the unit have yasc in it -- its main file, or any file from a #pragma
   yasc, or an imported interface?  (TCCState.yasc is the file being read) */
#define tcc_yasc_unit(s1) ((s1)->yasc_std || (s1)->nb_yasc_files)
/* Count of #pragma pack actions ever added to a pp_pragma_capture stream: lets
   a caller of skip_or_save_block tell whether the saved tokens hold any. */
ST_DATA int pp_pack_captures;

/* display benchmark infos */
ST_DATA int tok_ident;
ST_DATA TokenSym **table_ident;
ST_FUNC const char *tok_name_load(TokenSym *ts);
ST_FUNC const char *tok_name_tmp(int v);
/* A local symbol put_extern_sym2 named by its token: st_name holds the token
 * with this bit, and its name goes into strtab only at the end of the TU
 * (elf_names_materialize) -- a Zig kernel compile's strtab was 180 KB of its
 * peak, the names of its thousands of static functions.  tccelf_end_file
 * fills them in, for a C or an asm file alike. */
#define ELF_NAME_DEFERRED 0x80000000u
ST_FUNC void elf_names_materialize(TCCState *s1);
ST_FUNC void funcname_copies_free(void);
ST_FUNC void tok_names_spill(void);
/* TS's name, read back from the spill file if it is there (TokenSym.str). */
static inline const char *tok_name(TokenSym *ts)
{
  return ((uintptr_t)ts->str & 1) ? tok_name_load(ts) : ts->str;
}
/* Whether token T is the identifier S of LEN bytes: the length first, so a
 * spilled name of another length is not read back for it. */
static inline int tok_is_ident(int t, const char *s, int len)
{
  TokenSym *ts;
  if (t < TOK_IDENT || t >= tok_ident || !(ts = table_ident[t - TOK_IDENT]) || ts->len != len)
    return 0;
  return !memcmp(tok_name(ts), s, len);
}
ST_DATA int pp_expr;

#define TOK_FLAG_BOL 0x0001   /* beginning of line before */
#define TOK_FLAG_BOF 0x0002   /* beginning of file before */
#define TOK_FLAG_ENDIF 0x0004 /* a endif was found matching starting #ifdef */

#define PARSE_FLAG_PREPROCESS 0x0001 /* activate preprocessing */
#define PARSE_FLAG_TOK_NUM 0x0002    /* return numbers instead of TOK_PPNUM */
#define PARSE_FLAG_LINEFEED                                                                                            \
  0x0004 /* line feed is returned as a                                                                                 \
            token. line feed is also                                                                                   \
            returned at eof */
#define PARSE_FLAG_ASM_FILE                                                                                            \
  0x0008                                /* we processing an asm file: '#' can be used for line comment, etc.           \
                                         */
#define PARSE_FLAG_SPACES 0x0010        /* next() returns space tokens (for -E) */
#define PARSE_FLAG_ACCEPT_STRAYS 0x0020 /* next() returns '\\' token */
#define PARSE_FLAG_TOK_STR 0x0040       /* return parsed strings instead of TOK_PPSTR */

/* isidnum_table flags: */
#define IS_SPC 1
#define IS_ID 2
#define IS_NUM 4

enum line_macro_output_format
{
  LINE_MACRO_OUTPUT_FORMAT_GCC,
  LINE_MACRO_OUTPUT_FORMAT_NONE,
  LINE_MACRO_OUTPUT_FORMAT_STD,
  LINE_MACRO_OUTPUT_FORMAT_P10 = 11
};

ST_FUNC TokenSym *tok_alloc(const char *str, int len);
ST_FUNC TokenSym *tok_ensure(int v); /* table_ident[v], materializing a lazy builtin slot */
ST_FUNC TokenSym *tok_find(const char *str, int len); /* existing token or NULL; interns nothing */
ST_FUNC int tok_alloc_const(const char *str);
ST_FUNC const char *get_tok_str(int v, CValue *cv);
ST_FUNC TokenString *tok_str_borrow(TokenString *str, int len);
ST_FUNC void split_pad_note(TCCState *s1, unsigned off);
ST_FUNC void text_window_spill(TCCState *s1);
ST_FUNC void text_window_unspill_all(TCCState *s1);
ST_FUNC const unsigned char *text_window_bytes(TCCState *s1, Section *sec, unsigned long off, unsigned long size,
                                               unsigned char **tmp);
ST_FUNC void split_pads_erased(TCCState *s1, unsigned start, unsigned size, unsigned shift);
ST_FUNC int tok_reading_compile_replay(void);
ST_FUNC void begin_macro(TokenString *str, int alloc);
ST_FUNC void begin_macro_kept(TokenString *str);
ST_FUNC void end_macro(void);
ST_FUNC void end_macro_to(TokenString *target);
ST_FUNC int set_idnum(int c, int val);
ST_INLN void tok_str_new(TokenString *s);
ST_FUNC TokenString *tok_str_alloc(void);
ST_FUNC void tok_str_free(TokenString *s);
ST_FUNC void tok_str_free_str(int *str);
ST_FUNC int *tok_str_ensure_heap(TokenString *s);
ST_FUNC void tok_str_fit(TokenString *s);
ST_FUNC TokenString *tok_str_keep(TokenString *s);
ST_FUNC void tok_str_spill(TokenString *s);
ST_FUNC void tok_str_release(TokenString *s);
ST_FUNC void tok_str_release_parsed(void);
ST_FUNC void tok_str_release_spilled(void);
ST_FUNC void tok_str_spill_close(void);
ST_FUNC int tcc_spill_active(void);
ST_FUNC long tcc_spill_append(const void *p, size_t n);
ST_FUNC void tcc_spill_read(long off, void *p, size_t n);
ST_FUNC const char *tcc_filename_intern(const char *name);
ST_FUNC int tcc_yasc_file(TCCState *s1, const char *filename);
ST_FUNC int tcc_yasc_include_dir(TCCState *s1, const char *dir);
/* tcc_warning_once for a warning that depends on constant values (-Woverflow,
   -Warray-bounds, ...): also quiet in const_eval.c's speculative parse */
ST_FUNC void tcc_fe_warning(int warn_offset, const char *fmt, ...) PRINTF_LIKE(2, 3);
#define tcc_fe_warning_c(sw, ...) tcc_fe_warning(offsetof(TCCState, sw), __VA_ARGS__)
enum
{
  DIAG_PRAGMA_PUSH,
  DIAG_PRAGMA_POP,
  DIAG_PRAGMA_IGNORED,
  DIAG_PRAGMA_WARNING,
  DIAG_PRAGMA_ERROR
};
ST_FUNC void tcc_pragma_diagnostic(TCCState *s1, int action, const char *option, const char *file, int line);
ST_FUNC void tcc_unsafe_begin(TCCState *s1, const char *file, int line);
ST_FUNC void tcc_unsafe_end(TCCState *s1, const char *file, int line);
ST_FUNC void tcc_unsafe_reset(TCCState *s1, const char *file, int line);
ST_FUNC void tcc_diag_reset(TCCState *s1);
ST_FUNC int tcc_diag_first(TCCState *s1, int line, int key);
ST_FUNC int tcc_diag_marked(TCCState *s1, int line, int key);
/* the report-once set itself: NS one of DIAG_ONCE_*, KEY and TEXT (may be
   NULL) tell apart what is said at LINE of FILE (tcc_filename_intern'ed, or
   NULL).  Returns 1 the first time (and records it when INSERT); 0 after. */
enum
{
  DIAG_ONCE_WARN = 1, /* a warning: KEY its option's warn_ offset, TEXT its message */
  DIAG_ONCE_PLACE,    /* tcc_diag_first: KEY a DIAG_KEY */
  DIAG_ONCE_DECL      /* -Wunused: KEY mixes the kind and the declaration's token */
};
ST_FUNC int tcc_diag_once(TCCState *s1, int ns, unsigned key, const char *file, int line, const char *text,
                          int insert);
ST_FUNC const char *tcc_diag_file(BufferedFile **pf);
/* a body's parse starts (returns what tcc_diag_parse_leave restores) */
static inline unsigned tcc_diag_parse_enter(TCCState *s1)
{
  unsigned saved = s1->diag_parse;
  s1->diag_parse = ++s1->diag_parse_next;
  return saved;
}
#define tcc_diag_parse_leave(s1, saved) ((s1)->diag_parse = (saved))
/* tcc_diag_first keys of the frontend checks */
enum
{
  DIAG_KEY_FALLTHROUGH_COMMENT = 1, /* a fall-through comment right before this line's case label */
  DIAG_KEY_FALLTHROUGH,
  DIAG_KEY_PARENTHESES,
  DIAG_KEY_DANGLING_ELSE,
  DIAG_KEY_EMPTY_BODY,
  DIAG_KEY_SWITCH,
  DIAG_KEY_SHADOW
};
#define DIAG_KEY(kind, extra) ((int)((unsigned)(extra) << 4) | (kind))
ST_FUNC void tccpp_setfile_replay(const char *filename);
ST_FUNC void tcc_yasc_mark_file(TCCState *s1, const char *filename);
ST_FUNC int tcc_sys_file(TCCState *s1, const char *filename, int line);
ST_FUNC void tok_str_add(TokenString *s, int t);
ST_FUNC void tok_str_add2(TokenString *s, int t, CValue *cv);
ST_FUNC void tok_str_lead_yasc(TokenString *s, int on);
ST_FUNC void tok_str_add_tok(TokenString *s);
ST_FUNC void tok_get(int *t, const int **pp, CValue *cv);
ST_FUNC long long pp_error_name(void);
ST_FUNC void pp_errno_macros(void (*cb)(int tok, int value));
ST_FUNC void pp_apply_pack_replay(TCCState *s1, int code);
ST_FUNC void pp_apply_pack_replays(TCCState *s1, const int *p, const int *end);
ST_FUNC int *pp_pack_snapshot(TCCState *s1);
ST_FUNC void pp_pack_enter(TCCState *s1, const int *snap, int *saved);
ST_FUNC void pp_pack_leave(TCCState *s1, const int *saved);
ST_INLN void define_push(int v, int macro_type, int *str, Sym *first_arg);
ST_FUNC void define_undef(Sym *s);
ST_INLN Sym *define_find(int v);
ST_FUNC void free_defines(Sym *b);
ST_FUNC void parse_define(void);
ST_FUNC void skip_to_eol(int warn);
ST_FUNC void preprocess(int is_bof);
ST_FUNC void next(void);
ST_INLN void unget_tok(int last_tok);
ST_FUNC void preprocess_start(TCCState *s1, int filetype);
ST_FUNC void preprocess_end(TCCState *s1);
ST_FUNC void tccpp_new(TCCState *s);
ST_FUNC void tccpp_delete(TCCState *s);
ST_FUNC void tccpp_putfile(const char *filename);
ST_FUNC void tccpp_setfile(const char *filename);
ST_FUNC int tcc_preprocess(TCCState *s1);
ST_FUNC void skip(int c);
ST_FUNC NORETURN void expect(const char *msg);
ST_FUNC void pp_error(CString *cs);

/* space excluding newline */
static inline int is_space(int ch)
{
  return ch == ' ' || ch == '\t' || ch == '\v' || ch == '\f' || ch == '\r';
}
static inline int isid(int c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static inline int isnum(int c)
{
  return c >= '0' && c <= '9';
}
static inline int isoct(int c)
{
  return c >= '0' && c <= '7';
}
static inline int toup(int c)
{
  return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
}

/* Whether parse_attribute() acts on the attribute spelled by token `t`
 * (backs the __has_attribute preprocessor operator). */
ST_FUNC int tcc_attribute_supported(int t);
ST_FUNC int tcc_builtin_supported(int t);
/* thumb-outline.c: -Os machine outliner */
ST_FUNC int tcc_gen_machine_outline_enabled(void);
ST_FUNC void tcc_gen_machine_outline_capture_begin(void);
ST_FUNC void tcc_gen_machine_outline_capture_end(void);
ST_FUNC void tcc_gen_machine_outline_capture(uint32_t opcode, int size, int at);
ST_FUNC void tcc_gen_machine_outline_capture_reloc(struct Sym *sym, int at);
struct TCCIRState;
ST_FUNC int tcc_gen_machine_outline_analyze(struct TCCIRState *ir, const uint32_t *map, int end);
ST_FUNC void tcc_gen_machine_outline_op_end(int i);
ST_FUNC void tcc_gen_machine_outline_op_start(int i);
ST_FUNC void tcc_gen_machine_outline_function_end(void);
ST_FUNC int tcc_gen_machine_outline_suppress(uint32_t opcode, int size);
ST_FUNC int tcc_gen_machine_outline_suppressing(void);
ST_FUNC int tcc_gen_machine_outline_shrink_between(int from_ir, int to_ir);
ST_FUNC void tcc_gen_machine_outline_reset(void);
ST_FUNC void tcc_gen_machine_outline_window_open(void);
ST_FUNC void tcc_gen_machine_outline_window_closed(int bytes);
ST_FUNC void tcc_gen_machine_outline_emit_bl(int esym);
ST_FUNC void tcc_gen_machine_outline_emit_raw(uint16_t hw);
ST_FUNC int tcc_gen_machine_prolog_saved_lr(void);

/* ------------ tccgen.c ------------ */

#ifdef CONFIG_TCC_LOW_MEM
#define SYM_POOL_NB (2048 / sizeof(Sym)) /* pools chain; smaller ones waste less of the last */
#else
#define SYM_POOL_NB (8192 / sizeof(Sym))
#endif

ST_DATA Sym *global_stack;
ST_DATA Sym *local_stack;
ST_DATA Sym *local_label_stack;
ST_DATA Sym *global_label_stack;
ST_DATA Sym *define_stack;
ST_DATA CType int_type, func_old_type, char_pointer_type;
ST_DATA SValue *vtop;
ST_DATA int rsym, anon_sym, ind, loc;
ST_DATA char debug_modes;

ST_DATA int nocode_wanted; /* true if no code generation wanted for an expression */
/* -Wuninitialized: a local's initializer expression is being parsed, and an
   IR operand read from that local is reported (init/initializer.c) */
extern int init_self_on;
void init_self_ir(int op, const SValue *src1, const SValue *src2, const SValue *dest);
ST_DATA int yasc_decl_undefined; /* yasc: the local being declared said `= undefined` */
ST_DATA int unevaluated_operand; /* inside the operand of sizeof/typeof/_Generic (expr_type) */
ST_DATA SValue *discarded_call_vtop; /* vtop where an expression statement began, while its
                                       value is being thrown away; NULL anywhere else */
ST_DATA int global_expr;   /* true if compound literals must be allocated globally
                              (used during initializers parsing */
ST_DATA CType func_vt;     /* current function return type (used by return instruction) */
ST_DATA int func_var;      /* true if current function is variadic */
ST_DATA int func_vc;
ST_DATA int func_ind;
ST_DATA int func_align_pad; /* bytes codegen padded in front of the body (ir/codegen.c) */
ST_DATA int func_has_label_addr; /* true if current function uses &&label (computed goto) */
ST_DATA const char *funcname;
ST_DATA int tok_prev_line; /* the line of the token before the current one (tccpp.c next) */

ST_FUNC void tccgen_init(TCCState *s1);
ST_FUNC int tccgen_compile(TCCState *s1);
/* error recovery (frontend/gen/core/recover.c) */
ST_FUNC int tcc_error_seen(TCCState *s1, const char *msg, int len);
ST_FUNC void tcc_recover_jump(TCCState *s1);
ST_FUNC void tcc_recover_release(TCCState *s1);
ST_FUNC void tcc_max_errors_defer(TCCState *s1);
ST_FUNC void tcc_max_errors_stop(TCCState *s1);
ST_FUNC void save_body_error(TCCState *s1, int line, const char *msg); /* longjmps */
ST_FUNC void save_body_raise(TCCState *s1);
typedef struct DiagStash
{
  char *msg;
  int line;
  unsigned char kind;   /* DIAG_STASH_* */
  unsigned char raised; /* the parse reached its marker */
} DiagStash;
enum
{
  DIAG_STASH_ERROR,   /* an error the parse recovers from */
  DIAG_STASH_NOABORT, /* #error: the parse goes on */
  DIAG_STASH_WARNING, /* #warning */
};
ST_FUNC int tcc_pp_diag_deferred(int kind, const char *msg); /* #error, #warning in a saved body */
ST_FUNC int tcc_error_stash(const char *fmt, ...) PRINTF_LIKE(1, 2); /* the marker's value */
ST_FUNC void tcc_error_stash_raise(TCCState *s1, int idx);
ST_FUNC void tcc_error_stash_unraised(TCCState *s1);
ST_FUNC int tcc_lex_error_deferred(const char *fmt, ...) PRINTF_LIKE(1, 2);
ST_FUNC void tcc_undeclared(int t, const char *name);
ST_FUNC int gen_body_recovering(void); /* block(0) of a body, going on after errors */
/* a struct or union body's members, each by MEMBER(ARG), going on with the
   next member after an error in one */
ST_FUNC void struct_body_recovering(void (*member)(void *), void *arg);
ST_FUNC int replay_recovering(void (*fn)(void *), void *arg); /* a replay: goes on after it */
ST_FUNC int recover_call(void (*fn)(void *), void *arg, int kind); /* recover.c RECOVER_* */
ST_FUNC void tcc_recover_stop(TCCState *s1); /* the parse stops: the saved bodies still speak */
#define DIAG_ANCHOR_END 0x7fffffff /* the end of the unit: after every declaration */
ST_FUNC void tcc_diag_queue_begin(TCCState *s1);
ST_FUNC void tcc_diag_queue_flush(TCCState *s1);
/* a body replayed away from its place speaks at ANCHOR: returns the anchor to give back */
static inline int tcc_diag_anchor_enter(TCCState *s1, int anchor)
{
  int old = s1->diag_anchor;
  s1->diag_anchor = anchor;
  return old;
}
#define RECOVER_CALL_FUNC 4 /* RECOVER_FUNC */
ST_FUNC void asm_inline_recover(void); /* tccasm.c: an error in inline asm text */
ST_FUNC void replay_body_recovering(TokenString *ts, Sym *callee); /* an inline expansion's or a defer's */
/* The builtin alias prototypes are built lazily (predef_protos.c): armed at
   tccgen_compile start, a name is queued when first interned and its
   prototype materialised before the next identifier lookup or push. */
ST_DATA unsigned char tcc_predef_protos_armed;
ST_DATA unsigned char tcc_predef_protos_queued;
ST_FUNC void tccgen_predef_protos_arm(void);
ST_FUNC void tccgen_predef_protos_disarm(void);
ST_FUNC void tccgen_predef_protos_flush(void);
ST_FUNC void tccgen_predef_proto_interned(TokenSym *ts);
ST_FUNC struct Sym *tccgen_predef_proto_find_unseen(int v);
/* Intern hook: only names spelled __builtin_* can be prototypes. */
#define TCC_PREDEF_PROTO_INTERNED(ts)                                                   \
  do                                                                                    \
  {                                                                                     \
    if (tcc_predef_protos_armed && (ts)->len > 10 && !memcmp(tok_name(ts), "__builtin_", 10)) \
      tccgen_predef_proto_interned(ts);                                                 \
  } while (0)
#define TCC_PREDEF_PROTOS_FLUSH()       \
  do                                    \
  {                                     \
    if (tcc_predef_protos_queued)       \
      tccgen_predef_protos_flush();     \
  } while (0)
ST_FUNC void tccgen_finish(TCCState *s1);
ST_FUNC void check_vstack(void);

ST_INLN int is_float(int t);
ST_FUNC int ieee_finite(double d);
ST_FUNC int exact_log2p1(unsigned int i);
ST_FUNC void test_lvalue(void);

ST_FUNC ElfSym *elfsym(Sym *);
ST_FUNC void update_storage(Sym *sym);
ST_FUNC void put_extern_sym2(Sym *sym, int sh_num, addr_t value, unsigned long size, int can_add_underscore);
ST_FUNC void put_extern_sym(Sym *sym, Section *section, addr_t value, unsigned long size);

/* Identical code folding: a generated body equal to one already emitted is
 * dropped and its symbol points at the survivor (backend/generators/icf.c). */
#ifdef CONFIG_TCC_O0_ONLY
/* Folding only runs above -O0: an O0-only compiler leaves out icf.c and its
 * 16 KB hash table. */
static inline int tcc_icf_try_fold(TCCState *s1, Sym *sym, Section *sec, addr_t start, addr_t size, size_t reloc_mark)
{
  (void)s1; (void)sym; (void)sec; (void)start; (void)size; (void)reloc_mark;
  return 0;
}
static inline void tcc_icf_reset(void) {}
static inline void tcc_icf_section_changed(Section *sec) { (void)sec; }
#else
int tcc_icf_try_fold(TCCState *s1, Sym *sym, Section *sec, addr_t start, addr_t size, size_t reloc_mark);
void tcc_icf_reset(void);
void tcc_icf_section_changed(Section *sec);
#endif
/* Stack use per function: -Wframe-larger-than=, -Wstack-usage=, -fstack-usage,
 * -fcallgraph-info (backend/generators/stack_usage.c). */
/* Indirect calls of one function type (-fcallgraph-info): SIG indexes the
 * unit's signature table (tcc_stack_usage_sig), 0 when the type is not known. */
#define TCC_SU_MAX_INDIRECT_SIGS 16
typedef struct TCCStackIndirect
{
  int sig;
  int calls, tail;
} TCCStackIndirect;
typedef struct TCCStackFrame
{
  int entry;          /* bytes SP drops in the prologue: pushes and the SUB SP */
  int transient;      /* the most SP drops further in the body (scratch pushes, call windows) */
  int dynamic;        /* SP moves by a run-time amount (VLA, alloca) */
  int indirect_calls; /* BLX through a register */
  int indirect_tail;  /* BX through a register (an indirect tail call) */
  /* the indirect calls by function type; one with sig 0 holds those of no
   * known type (and those past TCC_SU_MAX_INDIRECT_SIGS types) */
  TCCStackIndirect by_sig[TCC_SU_MAX_INDIRECT_SIGS + 1];
  int nb_sigs;
} TCCStackFrame;
/* any stack option on: the frontend's records for it are made only then */
#define TCC_SU_WANTED(s1)                                                                                              \
  ((s1)->stack_usage || (s1)->callgraph_info || (s1)->frame_larger_than_set || (s1)->stack_usage_limit_set)
ST_FUNC int tcc_stack_usage_wanted(TCCState *s1);
ST_FUNC void tcc_stack_usage_declarator(TCCState *s1, Sym *sym, int v, int line);
ST_FUNC int tcc_stack_usage_name_column(int v);
ST_FUNC int tcc_stack_usage_sig(TCCState *s1, Sym *ftype);
ST_FUNC void tcc_stack_usage_note_call(TCCState *s1, int call_id, Sym *ftype);
ST_FUNC int tcc_stack_usage_call_sig(TCCState *s1, int call_id);
ST_FUNC void tcc_stack_usage_function(TCCState *s1, Sym *sym, int line, int end_line, Section *sec, addr_t start,
                                      size_t reloc_mark, int naked);
ST_FUNC void tcc_stack_usage_unit_end(TCCState *s1);
ST_FUNC void tcc_stack_usage_write(TCCState *s1, const char *object);
ST_FUNC void tcc_stack_usage_free(TCCState *s1);
ST_FUNC void tcc_gen_machine_stack_begin(int naked);
ST_FUNC void tcc_gen_machine_stack_end(TCCStackFrame *f);
ST_FUNC void tcc_gen_machine_stack_indirect(int is_jmp);
ST_FUNC void tcc_gen_machine_stack_call_sig(int sig);
#if PTR_SIZE == 4
ST_FUNC void greloc(Section *s, Sym *sym, unsigned long offset, int type);
#endif
ST_FUNC void greloca(Section *s, Sym *sym, unsigned long offset, int type, addr_t addend);

ST_INLN void sym_free(Sym *sym);
ST_FUNC Sym *sym_push(int v, CType *type, int r, int c);
ST_FUNC void sym_pop(Sym **ptop, Sym *b, int keep);
ST_FUNC Sym *sym_push2(Sym **ps, int v, int t, int c);
ST_FUNC Sym *sym_find2(Sym *s, int v);
ST_INLN Sym *sym_find(int v);
ST_FUNC Sym *label_find(int v);
ST_FUNC Sym *label_push(Sym **ptop, int v, int flags);
ST_FUNC void label_pop(Sym **ptop, Sym *slast, int keep);
ST_INLN Sym *struct_find(int v);
/* yasc modules (frontend/gen/decl/module.c): -fmodule-deps */
ST_FUNC void yasc_module_scan(TCCState *s1);
ST_FUNC Sym *sym_tag_head(int v);
ST_FUNC void sym_set_tag_head(int v, Sym *s);
ST_FUNC Sym *sym_label_head(int v);
ST_FUNC void sym_set_label_head(int v, Sym *s);
ST_FUNC void sym_heads_free(void);
ST_FUNC Sym *sym_push2_short(Sym **ps, int v, int t, int c);
ST_FUNC void sym_free_short(Sym *s);
ST_FUNC void sym_short_pools_free(void);
ST_FUNC int sym_outlived_locals_mark(void);
ST_FUNC void sym_free_outlived_locals(int mark);
ST_FUNC Sym *sym_pointer_node(CType *type);
ST_FUNC Sym *sym_pointer_node_q(CType *type, int yasc);
ST_FUNC void sym_pointer_nodes_free(void);
/* A parked prototype (sym_park_proto): its name's sym_identifier holds the
   record, tagged in bit 0, until sym_unpark builds the Syms back. */
#define SYM_IS_PARKED(s) ((uintptr_t)(s) & 1)
ST_FUNC Sym *sym_unpark(TokenSym *ts);
ST_FUNC void sym_park_proto(Sym *s, Sym *stack_before);
ST_FUNC void sym_params_track(Sym *ref, Sym *stack_before);
ST_FUNC void sym_params_release(void);
ST_FUNC void sym_params_spill_close(void);
ST_FUNC int sym_functype_adopt(Sym *old_ref, Sym *new_ref);
extern Sym *declarator_last_functype;
extern Sym *patch_own_functype; /* attr_merge.c: set by decl around external_sym */
ST_DATA int gen_function_depth;
ST_FUNC void sym_parked_free_all(void);

ST_FUNC Sym *global_identifier_push(int v, int t, int c);
ST_FUNC Sym *external_global_sym(int v, CType *type);
ST_FUNC Sym *external_helper_sym(int v);
ST_FUNC void vpush_helper_func(int v);
ST_FUNC void vpush_return_address(void);
ST_FUNC void vpush_typed_helper_func(int v, CType *type);
ST_FUNC void vset(CType *type, int r, int v);
ST_FUNC void vset_VT_CMP(int op);
ST_FUNC void vpushi(int v);
ST_FUNC void vpushv(SValue *v);
ST_FUNC void vpushsym(CType *type, Sym *sym);
ST_FUNC void vswap(void);
ST_FUNC void vrott(int n);
ST_FUNC void vrotb(int n);
ST_FUNC void vrev(int n);
ST_FUNC void vpop(void);
/* Force the read of a volatile lvalue whose value is about to be discarded
 * (`*p;`, a comma operand, a for-loop increment).  vpop alone drops the lvalue
 * without ever loading it, which silently deletes the read-to-clear idiom. */
ST_FUNC void gv_discarded_volatile(void);
ST_FUNC void vpop_discarded(void);
#if PTR_SIZE == 4
ST_FUNC void lexpand(void);
#endif
ST_FUNC void gaddrof(void);
ST_FUNC int gv(int rc);
ST_FUNC void sso_load_operand(const SValue *sv, SValue *out);
ST_FUNC void gv2(int rc1, int rc2);
ST_FUNC void gen_op(int op);
ST_FUNC void gen_op_vector_reset(void);
ST_FUNC int type_size(const CType *type, int *a);
ST_FUNC void mk_pointer(CType *type);
ST_FUNC void mk_pointer_yasc(CType *type, int yasc);
ST_FUNC void type_yasc_strip(CType *type);
ST_FUNC void type_yasc_derive(CType *type);
ST_FUNC void yasc_verify_discard(void);
ST_FUNC void mk_private_pointer(CType *type);
ST_FUNC void vstore(void);
ST_FUNC void inc(int post, int c);
ST_FUNC CString *parse_mult_str(const char *msg);
ST_FUNC CString *parse_asm_str(void);
ST_FUNC void gen_internal_call(const char *name, int call_argc, CType *ct);
/* A call the backend emits as instructions in place of the BL: an inline
 * atomic (__tcc_ax_*, -minline-atomics) or the system instructions of an asm
 * statement (__tcc_mc_*).  There is no function to branch to. */
static inline int tcc_is_inline_machine_call(const char *name)
{
  return !strncmp(name, "__tcc_ax_", 9) || !strncmp(name, "__tcc_mc_", 9);
}
ST_FUNC void indir(void);
ST_FUNC void unary(void);
ST_FUNC void gexpr(void);
ST_FUNC int64_t expr_const64(void);
ST_FUNC int expr_const(void);
/* get_sym_ref is used unconditionally by the IR optimization passes
   (ir/opt.c, ir/opt_switch_data.c) and for string/rodata literals in
   tccgen.c, so its prototype must always be visible. */
ST_FUNC Sym *get_sym_ref(CType *type, Section *sec, unsigned long offset, unsigned long size);
#if defined TCC_TARGET_X86_64 && !defined TCC_TARGET_PE
ST_FUNC int classify_x86_64_va_arg(CType *ty);
#endif

/* ------------ tccelf.c ------------ */

#define TCC_OUTPUT_FORMAT_ELF 0    /* default output format: ELF */
#define TCC_OUTPUT_FORMAT_BINARY 1 /* binary image output */
#define TCC_OUTPUT_FORMAT_COFF 2   /* COFF */
#define TCC_OUTPUT_FORMAT_YAFF 3   /* YAFF */
#define TCC_OUTPUT_DYN TCC_OUTPUT_DLL

#define ARMAG "!<arch>\n" /* For COFF and a.out archives */
#define YAFFMAG "YAFF"

typedef struct
{
  unsigned int n_strx;   /* index into string table of name */
  unsigned char n_type;  /* type of symbol */
  unsigned char n_other; /* misc info (usually empty) */
  unsigned short n_desc; /* description field */
  unsigned int n_value;  /* value of symbol */
} Stab_Sym;

ST_FUNC void tccelf_new(TCCState *s);
ST_FUNC void tccelf_delete(TCCState *s);
ST_FUNC void tccelf_begin_file(TCCState *s1);
ST_FUNC void tccelf_end_file(TCCState *s1);
ST_FUNC Section *new_section(TCCState *s1, const char *name, int sh_type, int sh_flags);
ST_FUNC void section_realloc(Section *sec, unsigned long new_size);
ST_FUNC void section_fit(Section *sec);
ST_FUNC void section_materialize(TCCState *s1, Section *sec);
ST_FUNC int deferred_chunks_read(TCCState *s1, DeferredChunk **chunks, unsigned char *dest, int *fd, const char **fd_path);
ST_FUNC void section_reserve_exact(Section *sec, unsigned long size);
ST_FUNC void section_release(Section *sec);
ST_FUNC unsigned char *section_spill_ptr(TCCState *s1, Section *s, unsigned long off);
ST_FUNC int section_spill_flush(TCCState *s1, Section *s);
ST_FUNC int section_spill_write(TCCState *s1, Section *s, FILE *f, unsigned long size);
ST_FUNC void section_spill_free(Section *s);
ST_FUNC SectionAux *section_aux(Section *s);
ST_FUNC const char *deferred_path(TCCState *s1, const char *path);
ST_FUNC ElfW_Rel *lazy_rels_read(TCCState *s1, Section *s, int *count);
ST_FUNC int lazy_rels_count(Section *s);
ST_FUNC void lazy_rels_free(Section *s);
ST_FUNC void *link_arena_alloc(TCCState *s1, unsigned long size);
ST_FUNC int link_arena_owns(const TCCState *s1, const void *p);
ST_FUNC char **tcc_name_list_read(TCCState *s1, const char *path, const char *option, int *count);
ST_FUNC int tcc_name_list_has(char **names, int count, const char *name, size_t len);
ST_FUNC int tcc_name_list_has_unnumbered(char **names, int count, const char *name);
ST_FUNC void tcc_name_list_sort(char **names, int count);
ST_FUNC Section *new_input_section(TCCState *s1, const char *name, int sh_type, int sh_flags);
ST_FUNC void tcc_link_arena_prepare(TCCState *s1);
ST_FUNC Section *section_reloc(Section *symtab, Section *s);
ST_FUNC size_t section_add(Section *sec, addr_t size, int align);
ST_FUNC void *section_ptr_add(Section *sec, addr_t size);
ST_FUNC void section_prealloc(Section *sec, unsigned long size);
ST_FUNC Section *find_section(TCCState *s1, const char *name);
ST_FUNC void free_section(Section *s);
ST_FUNC Section *new_symtab(TCCState *s1, const char *symtab_name, int sh_type, int sh_flags, const char *strtab_name,
                            const char *hash_name, int hash_sh_flags);
ST_FUNC void init_symtab(Section *s);

ST_FUNC int put_elf_str(Section *s, const char *sym);
ST_FUNC int put_elf_sym(Section *s, addr_t value, unsigned long size, int info, int other, int shndx, const char *name);
ST_FUNC int set_elf_sym(Section *s, addr_t value, unsigned long size, int info, int other, int shndx, const char *name);
ST_FUNC int find_elf_sym(Section *s, const char *name);
ST_FUNC int tcc_dynsym_find(TCCState *s1, const char *name); /* find_elf_sym(dynsymtab) + lazy YAFF resolve */
ST_FUNC int tcc_yaff_resolve(TCCState *s1, const char *name); /* on-disk-hash lookup + intern; 0 if absent */
ST_FUNC void tcc_yaff_libs_free(TCCState *s1);
/* 1 if a shared library in the library paths exports `name` (and libtcc1.a does
   not define it), 0 if not, -1 when no import set could be built. */
ST_FUNC int tcc_yaff_import_set_has(TCCState *s1, const char *name);
/* 1 if the libtcc1.a found next to the import set defines `name`. */
ST_FUNC int tcc_yaff_libtcc1_has(TCCState *s1, const char *name);
ST_FUNC void tcc_yaff_import_set_free(TCCState *s1);
ST_FUNC void put_elf_reloc(Section *symtab, Section *s, unsigned long offset, int type, int symbol);
int ld_section_memory_region(TCCState *s1, const char *name, addr_t *origin);
#ifdef TCC_TARGET_ARM
ST_FUNC void arm_add_range_veneers(TCCState *s1);
ST_FUNC unsigned long arm_hot_text_veneer_bytes(TCCState *s1);
ST_FUNC void arm_add_hot_text_veneers(TCCState *s1);
ST_FUNC void arm_hot_text_fill_got(TCCState *s1);
#endif
ST_FUNC void put_elf_reloca(Section *symtab, Section *s, unsigned long offset, int type, int symbol, addr_t addend);

ST_FUNC void resolve_common_syms(TCCState *s1);
ST_FUNC void relocate_syms(TCCState *s1, Section *symtab, int do_resolve);
ST_FUNC void relocate_sections(TCCState *s1);

ST_FUNC ssize_t full_read(int fd, void *buf, size_t count);
ST_FUNC void *load_data(int fd, unsigned long file_offset, unsigned long size);
/* -Wcross-tu (frontend/gen/decl/typesig.c, obj/tccelf_load.c) */
ST_FUNC void tcc_typesig_unit(TCCState *s1, const char *unit);
ST_FUNC void tcc_typesig_note(int v, int line, int defined);
ST_FUNC void tcc_typesig_reset(void);
ST_FUNC void tcc_typesig_type(char *buf, int size, CType *type);
/* -Wtainted-format across units (ir/taint.c) */
ST_FUNC void tcc_taint_link_add(TCCState *s1, const void *data, size_t size);
ST_FUNC void tcc_taint_link_check(TCCState *s1);
ST_FUNC void tcc_taint_link_free(TCCState *s1);
ST_FUNC int tcc_warning_shown_at(TCCState *s1, int woff, const char *fname, int line);
ST_FUNC void tcc_typesig_add(TCCState *s1, const char *name, const char *sig, int kind, const char *loc,
                             const char *unit);
ST_FUNC void tcc_typesig_check(TCCState *s1);
ST_FUNC void tcc_typesig_free(TCCState *s1);
ST_FUNC int tcc_object_type(int fd, ElfW(Ehdr) * h);
ST_FUNC int tcc_load_object_file(TCCState *s1, int fd, unsigned long file_offset);
ST_FUNC int tcc_load_object_file_lazy(TCCState *s1, int fd, unsigned long file_offset);
ST_FUNC void tcc_gc_mark_phase(TCCState *s1);
ST_FUNC void tcc_load_referenced_sections(TCCState *s1);
ST_FUNC void tcc_free_lazy_objfiles(TCCState *s1);
ST_FUNC int tcc_load_archive(TCCState *s1, int fd, int alacarte);
ST_FUNC void tcc_archive_cache_free(TCCState *s1);
/* Read an archive's symbol index into the archive symbol cache without loading
   a member; 1 on success, 0 if `path` is not an archive with an index. */
ST_FUNC int tcc_archive_index_load(TCCState *s1, const char *path);
/* Does the index read by tcc_archive_index_load(path) name `name`? */
ST_FUNC int tcc_archive_index_has(TCCState *s1, const char *path, const char *name);
ST_FUNC int tcc_group_has_satisfiable_undefs(TCCState *s1);
ST_FUNC void add_array(TCCState *s1, const char *sec, int c);

ST_FUNC struct sym_attr *get_sym_attr(TCCState *s1, int index, int alloc);
ST_FUNC addr_t get_sym_addr(TCCState *s, const char *name, int err, int forc);
ST_FUNC void list_elf_symbols(TCCState *s, void *ctx, void (*symbol_cb)(void *ctx, const char *name, const void *val));
ST_FUNC int set_global_sym(TCCState *s1, const char *name, Section *sec, addr_t offs);

/* Browse each elem of type <type> in section <sec> starting at elem <startoff>
   using variable <elem> */
#define for_each_elem(sec, startoff, elem, type)                                                                       \
  for (elem = (type *)sec->data + startoff; elem < (type *)(sec->data + sec->data_offset); elem++)

#ifndef ELF_OBJ_ONLY
ST_FUNC int tcc_load_dll(TCCState *s1, int fd, const char *filename, int level);
ST_FUNC int tcc_load_ldscript(TCCState *s1, int fd);
ST_FUNC int tcc_load_linker_script(TCCState *s1, const char *filename);
ST_FUNC void tccelf_add_crtbegin(TCCState *s1);
ST_FUNC void tccelf_add_crtend(TCCState *s1);
#if defined TCC_TARGET_ARM
ST_FUNC void tccelf_add_arm_fp_lib(TCCState *s1);
ST_FUNC int tccelf_arm_fp_lib_is_shared(TCCState *s1);
/* The __aeabi_ runtime this link binds ("softfp", "rp2350fp", ...), NULL for hard float. */
ST_FUNC const char *tccelf_get_fp_lib_name(TCCState *s1);
#endif
#endif
#ifndef TCC_TARGET_PE
ST_FUNC void tcc_add_runtime(TCCState *s1);
#endif
ST_FUNC int tcc_load_yaff(TCCState *s1, int fd, const char *filename, int level);
ST_FUNC void tcc_elf_sort_syms(TCCState *s1, Section *s);

/* ------------ xxx-link.c ------------ */

#if !defined ELF_OBJ_ONLY || defined TCC_TARGET_MACHO
ST_FUNC int code_reloc(int reloc_type);
ST_FUNC int gotplt_entry_type(int reloc_type);
/* Whether to generate a GOT/PLT entry and when. NO_GOTPLT_ENTRY is first so
   that unknown relocation don't create a GOT or PLT entry */
enum gotplt_entry
{
  NO_GOTPLT_ENTRY,    /* never generate (eg. GLOB_DAT & JMP_SLOT relocs) */
  BUILD_GOT_ONLY,     /* only build GOT (eg. TPOFF relocs) */
  AUTO_GOTPLT_ENTRY,  /* generate if sym is UNDEF */
  ALWAYS_GOTPLT_ENTRY /* always generate (eg. PLTOFF relocs) */
};
#define NEED_RELOC_TYPE

#if !defined TCC_TARGET_MACHO || defined TCC_IS_NATIVE
ST_FUNC unsigned create_plt_entry(TCCState *s1, unsigned got_offset, struct sym_attr *attr);
ST_FUNC void relocate_plt(TCCState *s1);
ST_FUNC int build_got(TCCState *s1);                       /* in tccelf.c */
ST_FUNC void build_got_entries(TCCState *s1, int got_sym); /* in tccelf.c */
#define NEED_BUILD_GOT

#endif
#endif

ST_FUNC void relocate(TCCState *s1, ElfW_Rel *rel, int type, unsigned char *ptr, addr_t addr, addr_t val);

/* ------------ xxx-gen.c ------------ */
ST_DATA const char *const target_machine_defs;
ST_DATA const int reg_classes[NB_REGS];

ST_FUNC void gsym_addr(int t, int a);
ST_FUNC void gsym(int t);
ST_FUNC int gfunc_sret(CType *vt, int variadic, CType *ret, int *align, int *regsize);
ST_FUNC int gfunc_hfa(CType *type, int *base_size);
ST_FUNC int gfunc_sret_vfp_words(CType *vt, int variadic);
ST_FUNC int gfunc_regpair_ret(Sym *fsym);
ST_FUNC int gfunc_sret_fn(Sym *fsym, CType *vt, int variadic, CType *ret, int *align, int *regsize);
ST_FUNC Sym *gfunc_regpair_address_sym(Sym *sym);
/* How asm text names a C function (gfunc_regpair_asm_sym). */
#define REGPAIR_ASM_ADDRESS 0 /* an operand: the address of its AAPCS entry */
#define REGPAIR_ASM_DEFINE 1  /* a label definition, .global, .weak */
#define REGPAIR_ASM_MENTION 2 /* .type, .hidden */
ST_FUNC Sym *gfunc_regpair_asm_sym(Sym *sym, int how);
ST_FUNC void gfunc_regpair_check_redecl(Sym *fn);
ST_FUNC void gfunc_regpair_note_asm_names(TokenString *body);
ST_FUNC int gfunc_regpair_asm_named(Sym *fn);
ST_FUNC void gfunc_regpair_emit_adapters(TCCState *s1);
ST_FUNC void gfunc_regpair_reset(TCCState *s1);
ST_FUNC void gfunc_regpair_emit_adapter(TCCState *s1, Sym *fn, Sym *adapter, Section *sec);
ST_FUNC int gen_vfp_ret_transfer(const SValue *ptr, int words, int load);
ST_FUNC void gfunc_call(int nb_args);
ST_FUNC void gfunc_prolog(Sym *func_sym);
ST_FUNC void gfunc_epilog(void);
ST_FUNC void gen_fill_nops(int);
ST_FUNC int gjmp(int t);
ST_FUNC void gjmp_addr(int a);
ST_FUNC int gjmp_cond(int op, int t);
ST_FUNC int gjmp_append(int n, int t);
ST_FUNC void gen_opi(int op);
ST_FUNC void gen_opf(int op);
ST_FUNC void gen_cvt_ftoi(int t);
ST_FUNC void gen_cvt_itof(int t);
ST_FUNC void gen_cvt_ftof(int t);
ST_FUNC void ggoto(void);
#ifndef TCC_TARGET_C67
ST_FUNC void o(unsigned int c);
#endif
ST_FUNC void gen_vla_sp_save(int addr);
ST_FUNC void gen_vla_sp_restore(int addr);
ST_FUNC void gen_vla_alloc(CType *type, int align);
ST_FUNC addr_t gen_nested_func_trampoline(Sym *chain_slot_sym, Sym *func_sym);

ST_FUNC uint16_t read16le(unsigned char *p);
ST_FUNC void write16le(unsigned char *p, uint16_t x);
ST_FUNC uint32_t read32le(unsigned char *p);
ST_FUNC void write32le(unsigned char *p, uint32_t x);
ST_FUNC void add32le(unsigned char *p, int32_t x);
ST_FUNC uint64_t read64le(unsigned char *p);
ST_FUNC void write64le(unsigned char *p, uint64_t x);

#define DWARF_MAX_128 ((8 * sizeof(int64_t) + 6) / 7)
#define dwarf_read_1(ln, end) ((ln) < (end) ? *(ln)++ : 0)
#define dwarf_read_2(ln, end) ((ln) + 1 < (end) ? (ln) += 2, read16le((ln) - 2) : 0)
#define dwarf_read_4(ln, end) ((ln) + 3 < (end) ? (ln) += 4, read32le((ln) - 4) : 0)
#define dwarf_read_8(ln, end) ((ln) + 7 < (end) ? (ln) += 8, read64le((ln) - 8) : 0)
static inline uint64_t dwarf_read_uleb128(unsigned char **ln, unsigned char *end)
{
  unsigned char *cp = *ln;
  uint64_t retval = 0;
  int i;

  for (i = 0; i < DWARF_MAX_128; i++)
  {
    uint64_t byte = dwarf_read_1(cp, end);

    retval |= (byte & 0x7f) << (i * 7);
    if ((byte & 0x80) == 0)
      break;
  }
  *ln = cp;
  return retval;
}
static inline int64_t dwarf_read_sleb128(unsigned char **ln, unsigned char *end)
{
  unsigned char *cp = *ln;
  int64_t retval = 0;
  int i;

  for (i = 0; i < DWARF_MAX_128; i++)
  {
    uint64_t byte = dwarf_read_1(cp, end);

    retval |= (byte & 0x7f) << (i * 7);
    if ((byte & 0x80) == 0)
    {
      if ((byte & 0x40) && (i + 1) * 7 < 64)
        retval |= -1LL << ((i + 1) * 7);
      break;
    }
  }
  *ln = cp;
  return retval;
}

/* ------------ i386-gen.c ------------ */
#if defined TCC_TARGET_I386 || defined TCC_TARGET_X86_64 || defined TCC_TARGET_ARM || TCC_TARGET_ARM_THUMB
ST_FUNC void g(int c);
ST_FUNC void gen_le16(int c);
ST_FUNC void gen_le32(int c);
#endif
#if defined TCC_TARGET_I386 || defined TCC_TARGET_X86_64
ST_FUNC void gen_addr32(int r, Sym *sym, int c);
ST_FUNC void gen_addrpc32(int r, Sym *sym, int c);
ST_FUNC void gen_cvt_csti(int t);
ST_FUNC void gen_increment_tcov(SValue *sv);
#endif

/* ------------ x86_64-gen.c ------------ */
#ifdef TCC_TARGET_X86_64
ST_FUNC void gen_addr64(int r, Sym *sym, int64_t c);
ST_FUNC void gen_opl(int op);
#ifdef TCC_TARGET_PE
ST_FUNC void gen_vla_result(int addr);
#endif
ST_FUNC void gen_cvt_sxtw(void);
ST_FUNC void gen_cvt_csti(int t);
#endif

#include "source/machine/tcc_target.h"

/* ------------ arm-gen.c ------------ */
#if defined(TCC_TARGET_ARM) || defined(TCC_TARGET_ARM_THUMB)
#if defined(TCC_ARM_EABI) && !defined(CONFIG_TCC_ELFINTERP)
PUB_FUNC const char *default_elfinterp(struct TCCState *s);
#endif
ST_FUNC void arm_init(struct TCCState *s);
ST_FUNC void arm_deinit(struct TCCState *s);
ST_FUNC void gen_increment_tcov(SValue *sv);

/* ARM EABI build attributes (.ARM.attributes) describing the code we emit.
 * Derived from the resolved target profile and float ABI by the ARM backend;
 * the generic ELF writer only formats them.  A zero fp_arch/hardfp_use/vfp_args
 * means "omit that attribute" (its absence is the ABI's default). */
typedef struct ArmEabiAttrs
{
  const char *cpu_name; /* Tag_CPU_name (string) */
  int cpu_arch;         /* Tag_CPU_arch */
  int fp_arch;          /* Tag_FP_arch */
  int hardfp_use;       /* Tag_ABI_HardFP_use */
  int vfp_args;         /* Tag_ABI_VFP_args */
} ArmEabiAttrs;
ST_FUNC void arm_get_eabi_attrs(struct TCCState *s, ArmEabiAttrs *out);
#endif

/* ------------ tccasm.c ------------ */
ST_FUNC void asm_instr(void);
ST_FUNC void asm_global_instr(void);
ST_FUNC int tcc_assemble(TCCState *s1, int do_preprocess);
#ifdef CONFIG_TCC_ASM
ST_FUNC void tcc_asm_cleanup(void);
ST_FUNC int find_constraint(ASMOperand *operands, int nb_operands, const char *name, const char **pp);
ST_FUNC Sym *get_asm_sym(int name, Sym *csym);
ST_FUNC void asm_expr(TCCState *s1, ExprValue *pe);
ST_FUNC int asm_int_expr(TCCState *s1);
/* ------------ i386-asm.c ------------ */
ST_FUNC void gen_expr32(ExprValue *pe);
ST_FUNC void gen_expr64(ExprValue *pe);
ST_FUNC void asm_opcode(TCCState *s1, int opcode);
ST_FUNC int asm_parse_regvar(int t);
ST_FUNC void asm_compute_constraints(ASMOperand *operands, int nb_operands, int nb_outputs, const uint8_t *clobber_regs,
                                     const uint8_t *reserved_regs, int *pout_reg);
ST_FUNC void asm_check_constraints(ASMOperand *operands, int nb_operands, int nb_outputs);
ST_FUNC void subst_asm_operand(CString *add_str, SValue *sv, int modifier);
ST_FUNC void asm_gen_code(ASMOperand *operands, int nb_operands, int nb_outputs, int is_output, uint8_t *clobber_regs,
                          int out_reg);
ST_FUNC void asm_clobber(uint8_t *clobber_regs, const char *str);
ST_FUNC int asm_machine_call_name(const char *str, ASMOperand *operands, int nb_operands, int nb_outputs,
                                  const uint8_t *clobber_regs, char *name, int size);
#ifdef TCC_TARGET_ARM
/* `.fpu <name>` directive: enable FP-unit instruction encodings for the rest
   of the translation unit (GNU as compatible). Defined in arm-thumb-asm.c. */
ST_FUNC void tcc_asm_set_fpu(const char *name);
#endif

/* Emit a fully prepared GCC-style inline asm block.
 * Used by IR codegen to lower TCCIR_OP_INLINE_ASM without relying on front-end load/store helpers. */
ST_FUNC void tcc_asm_emit_inline(ASMOperand *operands, int nb_operands, int nb_outputs, int nb_labels,
                                 uint8_t *clobber_regs, const uint8_t *reserved_regs, const char *asm_str, int asm_len,
                                 int must_subst);
#endif

/* ------------ tccpe.c -------------- */
#ifdef TCC_TARGET_PE
ST_FUNC int pe_load_file(struct TCCState *s1, int fd, const char *filename);
ST_FUNC int pe_output_file(TCCState *s1, const char *filename);
ST_FUNC int pe_putimport(TCCState *s1, int dllindex, const char *name, addr_t value);
#if defined TCC_TARGET_I386 || defined TCC_TARGET_X86_64
#endif
#ifdef TCC_TARGET_X86_64
ST_FUNC void pe_add_unwind_data(unsigned start, unsigned end, unsigned stack);
#endif
PUB_FUNC int tcc_get_dllexports(const char *filename, char **pp);
/* symbol properties stored in Elf32_Sym->st_other */
#define ST_PE_EXPORT 0x10
#define ST_PE_IMPORT 0x20
#define ST_PE_STDCALL 0x40
#endif
#define ST_ASM_SET 0x04

/* ------------ tccmacho.c ----------------- */
#ifdef TCC_TARGET_MACHO
ST_FUNC int macho_output_file(TCCState *s1, const char *filename);
ST_FUNC int macho_load_dll(TCCState *s1, int fd, const char *filename, int lev);
ST_FUNC int macho_load_tbd(TCCState *s1, int fd, const char *filename, int lev);
#ifdef TCC_IS_NATIVE
ST_FUNC void tcc_add_macos_sdkpath(TCCState *s);
ST_FUNC const char *macho_tbd_soname(const char *filename);
#endif
#endif

/* ------------ tccyaff.c ------------ */
#ifdef TCC_TARGET_YAFF
ST_FUNC int tcc_output_yaff(TCCState *s1, FILE *f, const char *filename);
ST_FUNC void tcc_yaff_prepare_init_fini(TCCState *s1);
#endif
/* ------------ tccrun.c ----------------- */
#ifdef TCC_IS_NATIVE2
#ifdef CONFIG_TCC_STATIC
#define RTLD_LAZY 0x001
#define RTLD_NOW 0x002
#define RTLD_GLOBAL 0x100
#define RTLD_DEFAULT NULL
/* dummy function for profiling */
ST_FUNC void *dlopen(const char *filename, int flag);
ST_FUNC void dlclose(void *p);
ST_FUNC const char *dlerror(void);
ST_FUNC void *dlsym(void *handle, const char *symbol);
#endif
ST_FUNC void tcc_run_free(TCCState *s1);
#endif

/* ------------ tcctools.c ----------------- */
#if 0 /* included in tcc.c */
ST_FUNC int tcc_tool_ar(TCCState *s, int argc, char **argv);
ST_FUNC int tcc_tool_yasc_c_header(TCCState *s, int argc, char **argv);
#ifdef TCC_TARGET_PE
ST_FUNC int tcc_tool_impdef(TCCState *s, int argc, char **argv);
#endif
ST_FUNC int tcc_tool_cross(TCCState *s, char **argv, int option);
ST_FUNC int gen_makedeps(TCCState *s, const char *target, const char *filename);
#endif

/* ------------ tccdbg.c ------------ */

ST_FUNC void tcc_debug_new(TCCState *s);

ST_FUNC void tcc_debug_start(TCCState *s1);
ST_FUNC void tcc_debug_end(TCCState *s1);
ST_FUNC void tcc_debug_bincl(TCCState *s1);
ST_FUNC void tcc_debug_eincl(TCCState *s1);
ST_FUNC void tcc_debug_newfile(TCCState *s1);

ST_FUNC void tcc_debug_line(TCCState *s1);
ST_FUNC void tcc_debug_line_erase_range(TCCState *s1, Section *sec, addr_t start, addr_t size, addr_t shift,
                                        int tomb_sym);
ST_FUNC void tcc_debug_line_num(TCCState *s1, int line_num);
ST_FUNC void tcc_add_debug_info(TCCState *s1, int param, Sym *s, Sym *e);
ST_FUNC void tcc_debug_save_state(TCCState *s1, void **saved_info, void **saved_root);
ST_FUNC void tcc_debug_restore_state(TCCState *s1, void *saved_info, void *saved_root);
ST_FUNC void tcc_debug_funcstart(TCCState *s1, Sym *sym);
ST_FUNC void tcc_debug_prolog_epilog(TCCState *s1, int value);
/* Call-frame information for .debug_frame (tccdbg.c), reported by the backend
 * as it emits the code that moves SP or saves registers, at the current `ind`.
 * No-ops without -g, and the backend skips them in its dry runs. */
ST_FUNC void tcc_debug_cfi_sp_moved(TCCState *s1, int delta);
ST_FUNC void tcc_debug_cfi_push(TCCState *s1, unsigned mask);
ST_FUNC void tcc_debug_cfi_pop(TCCState *s1, unsigned mask);
ST_FUNC void tcc_debug_cfi_frame_pointer(TCCState *s1, int reg);
ST_FUNC void tcc_debug_cfi_remember_state(TCCState *s1);
ST_FUNC void tcc_debug_cfi_restore_state(TCCState *s1);
ST_FUNC void tcc_debug_funcend(TCCState *s1, int size);
ST_FUNC void tcc_debug_extern_sym(TCCState *s1, Sym *sym, int sh_num, int sym_bind, int sym_type);
ST_FUNC void tcc_debug_typedef(TCCState *s1, Sym *sym);
ST_FUNC void tcc_debug_stabn(TCCState *s1, int type, int value);
ST_FUNC void tcc_debug_fix_anon(TCCState *s1, CType *t);

#if !(defined ELF_OBJ_ONLY || defined TCC_TARGET_ARM || defined TARGETOS_BSD || defined TCC_TARGET_ARM_THUMB)
ST_FUNC void tcc_eh_frame_start(TCCState *s1);
ST_FUNC void tcc_eh_frame_end(TCCState *s1);
ST_FUNC void tcc_eh_frame_hdr(TCCState *s1, int final);
#define TCC_EH_FRAME 1
#endif

ST_FUNC void tcc_tcov_start(TCCState *s1);
ST_FUNC void tcc_tcov_end(TCCState *s1);
ST_FUNC void tcc_tcov_check_line(TCCState *s1, int start);
ST_FUNC void tcc_tcov_block_end(TCCState *s1, int line);
ST_FUNC void tcc_tcov_block_begin(TCCState *s1);
ST_FUNC void tcc_tcov_reset_ind(TCCState *s1);

/*
 * Target-independent helpers that IR-side load/spill materialization will invoke
 * before delegating to any backend machine op. Backend implementations live in
 * their respective *-gen.c files and follow the contract documented in
 * docs/IR_MACHINE_CONTRACT.md.
 */

ST_FUNC void tcc_machine_acquire_scratch(TCCMachineScratchRegs *scratch, unsigned flags);
ST_FUNC void tcc_machine_release_scratch(const TCCMachineScratchRegs *scratch);

ST_FUNC int tcc_machine_can_encode_stack_offset_for_reg(int frame_offset, int dest_reg);
ST_FUNC int tcc_machine_can_encode_stack_offset_with_param_adj(int frame_offset, int is_param, int dest_reg);
ST_FUNC void tcc_machine_load_spill_slot(int dest_reg, int frame_offset);
ST_FUNC void tcc_machine_store_spill_slot(int src_reg, int frame_offset);
ST_FUNC void tcc_machine_store_param_slot(int src_reg, int frame_offset);
ST_FUNC void tcc_machine_addr_of_stack_slot(int dest_reg, int frame_offset, int is_param);

/* Constant/value materialization - load various value types into registers */
ST_FUNC void tcc_machine_load_constant(int dest_reg, int dest_reg_high, int64_t value, int is_64bit, Sym *sym);
ST_FUNC void tcc_machine_load_cmp_result(int dest_reg, int condition_code);
ST_FUNC void tcc_machine_load_jmp_result(int dest_reg, int jmp_addr, int invert);

ST_FUNC void tcc_gen_machine_data_processing_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest,
                                                 TccIrOp op, uint32_t barrel_shift);
/* `barrel_shift` carries the same annotation word as tcc_gen_machine_data_processing_mop:
 * bits 0-15 the barrel shift, 16-17 shift64_dead_half, 18-23 zero_half64.  Every
 * dispatch site must pass it -- zero_half64's producer analysis assumes the
 * emitter can see its verdict, and a site that drops it makes codegen read a
 * register nothing wrote. */
ST_FUNC void tcc_gen_machine_data_processing_mop_flags(MachineOperand src1, MachineOperand src2, MachineOperand dest,
                                                       TccIrOp op, uint32_t barrel_shift);
/* Whether `ANDS rd, rn, <src2>` is one instruction: a register, or an
 * immediate the AND encoding holds (a mask UXTB/UXTH/UBFX/BIC would lower
 * needs a materialized constant once it must set flags). */
ST_FUNC int tcc_gen_machine_ands_is_direct(MachineOperand src2);
ST_FUNC void tcc_gen_machine_cmp_eq64_mop(MachineOperand src1, MachineOperand src2);
/* SUBS+IT peephole helper: emits `SUBS dest, src1, src2; IT NE; MOVNE dest, #1`
 * collapsing a CMP+SELECT(1,0,NE) / SELECT(0,1,EQ) pair into 3 instructions.
 * src2 must be MACH_OP_IMM. Returns 1 on emit, 0 if the SUBS immediate can't
 * be encoded and the caller should fall back to the regular CMP+SELECT. */
ST_FUNC int tcc_gen_machine_subs_eq_select_01(MachineOperand src1, MachineOperand src2, MachineOperand dest);
ST_FUNC void tcc_gen_machine_ubfx_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest);
ST_FUNC void tcc_gen_machine_sbfx_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest);
ST_FUNC void tcc_gen_machine_bfi_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, uint32_t params);
/* Single-operand bit manipulation: dest = <op>(src1) for TCCIR_OP_CLZ / RBIT /
 * REV / REV16.  Only called when tcc_machine_has_bit_ops() is true. */
ST_FUNC void tcc_gen_machine_bitop1_mop(MachineOperand src1, MachineOperand dest, TccIrOp op);
/* Does the active target encode clz/rbit/rev/rev16?  Front ends use this to
 * decide between the native opcode and the libgcc helper call. */
ST_FUNC int tcc_machine_has_bit_ops(void);
ST_FUNC int tcc_machine_has_unaligned_words(void);
/* UMAAL (DSP extension): gates the umaal fusion; see tcc_gen_machine_umaal_mop. */
ST_FUNC int tcc_machine_has_umaal(void);
ST_FUNC void tcc_gen_machine_umaal_mop(MachineOperand src1, MachineOperand src2, MachineOperand accum,
                                       MachineOperand dest);
ST_FUNC void tcc_gen_machine_assign_mop(MachineOperand src, MachineOperand dest, TccIrOp op);
/* Same, plus zero_half64's verdict in `zh` (bits 4-5: which halves of a 64-bit
 * destination no consumer reads and so need not be written). */
ST_FUNC void tcc_gen_machine_assign_mop_ex(MachineOperand src, MachineOperand dest, TccIrOp op, uint32_t zh);
ST_FUNC void tcc_gen_machine_pack64_mop(MachineOperand src_lo, MachineOperand src_hi, MachineOperand dest);
ST_FUNC void tcc_gen_machine_setif_mop(MachineOperand src, MachineOperand dest, TccIrOp op);
ST_FUNC void tcc_gen_machine_bool_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op);
ST_FUNC void tcc_gen_machine_load_mop(MachineOperand src, MachineOperand dest, TccIrOp op);
ST_FUNC void tcc_gen_machine_store_mop(MachineOperand dest, MachineOperand src, TccIrOp op);
ST_FUNC void tcc_gen_machine_store_spill(int src_reg, int32_t spill_offset);
ST_FUNC int tcc_gen_machine_try_strd_spill(int reg1, int32_t off1, int reg2, int32_t off2);
ST_FUNC int tcc_gen_machine_try_ldrd_spill(int reg1, int32_t off1, int reg2, int32_t off2);
ST_FUNC int tcc_gen_machine_try_ldrd_base(int reg1, int reg2, int base_reg, int32_t off);
ST_FUNC int tcc_gen_machine_try_strd_base(int reg1, int reg2, int base_reg, int32_t off);
ST_FUNC int tcc_gen_machine_try_strd_imm_spill(int64_t val1, int64_t val2, int32_t off1, int32_t off2);
ST_FUNC int tcc_gen_machine_try_strd_imm_base(int64_t val1, int64_t val2, int base_reg, int32_t off);
ST_FUNC void tcc_gen_machine_load_indexed_mop(MachineOperand dest, MachineOperand base, MachineOperand index,
                                              MachineOperand scale, TccIrOp op);
ST_FUNC void tcc_gen_machine_store_indexed_mop(MachineOperand base, MachineOperand index, MachineOperand scale,
                                               MachineOperand value, TccIrOp op);
ST_FUNC void tcc_gen_machine_load_postinc_mop(MachineOperand dest, MachineOperand ptr, MachineOperand offset,
                                              TccIrOp op);
ST_FUNC void tcc_gen_machine_store_postinc_mop(MachineOperand ptr, MachineOperand value, MachineOperand offset,
                                               TccIrOp op);
ST_FUNC void tcc_gen_machine_indirect_jump_mop(MachineOperand src, TccIrOp op);
ST_FUNC void tcc_gen_machine_func_parameter_mop(MachineOperand src1, MachineOperand src2_enc, TccIrOp op);
ST_FUNC void tcc_gen_machine_store_to_stack(int reg, int offset);
ST_FUNC void tcc_gen_machine_store_to_stack_ex(int reg, int offset, uint32_t extra_exclude);
ST_FUNC void tcc_gen_machine_store_to_sp(int reg, int offset);
ST_FUNC void tcc_gen_machine_store_to_sp_ex(int reg, int offset, uint32_t extra_exclude);

ST_FUNC void tcc_gen_machine_lea_mop(MachineOperand dest, MachineOperand src);
ST_FUNC int tcc_gen_machine_number_of_registers(void);
ST_FUNC void tcc_gen_machine_return_value_mop(MachineOperand src, TccIrOp op);
ST_FUNC void tcc_gen_machine_muldiv_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op);
ST_FUNC int tcc_gen_machine_mul_const_add_fused_mop(MachineOperand mul_var, int64_t mul_const,
                                                    MachineOperand mul_dest, MachineOperand add_base,
                                                    MachineOperand add_dest);
ST_FUNC void tcc_gen_machine_mla_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest,
                                     MachineOperand accum);
ST_FUNC void tcc_gen_machine_umull_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest);
ST_FUNC void tcc_gen_machine_smull_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest);
ST_FUNC int tcc_gen_machine_mlal_accum_mop(MachineOperand src1, MachineOperand src2, MachineOperand accum,
                                           MachineOperand dest, int is_signed);
ST_FUNC void tcc_gen_machine_fp_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op,
                                    int is_complex);
ST_FUNC void tcc_gen_machine_vla_mop(MachineOperand dest, MachineOperand src1, MachineOperand src2, TccIrOp op);
ST_FUNC void tcc_gen_machine_epilog(int leaffunc);
ST_FUNC void tcc_gen_machine_finish_noreturn(void);
ST_FUNC void tcc_gen_machine_prolog(int leaffunc, uint64_t used_registers, int stack_size,
                                    uint32_t extra_prologue_regs);
ST_FUNC void tcc_gen_machine_func_call_mop(MachineOperand func_mop, IROperand call_id, MachineOperand dest,
                                           int drop_value, TCCIRState *ir, int call_idx);
ST_FUNC int tcc_gen_machine_abi_assign_call_args(const TCCAbiArgDesc *args, int argc, TCCAbiCallLayout *out_layout);
ST_FUNC void tcc_gen_machine_save_call_context(void);
ST_FUNC void tcc_gen_machine_restore_call_context(void);
ST_FUNC int tcc_gen_machine_jump_mop(TccIrOp op, int32_t target_ir, int ir_idx);
/* ir/codegen.c: the real pass drops this JUMP / return jump (span is code-free). */
int tcc_ir_codegen_jump_elidable(struct TCCIRState *ir, int i);
/* ir/codegen.c: ... or a JUMP a JUMPIF other than fused_jumpif inverts over. */
int tcc_ir_codegen_jump_may_drop(struct TCCIRState *ir, int i, int fused_jumpif);
ST_FUNC int tcc_gen_machine_conditional_jump_mop(int32_t condition, TccIrOp op, int32_t target_ir, int ir_idx);
ST_FUNC void tcc_gen_machine_dry_run_set_rehearsal(int on);
ST_FUNC int tcc_gen_machine_cbz_forward_ok(int32_t target_ir, int current_ir_idx);
ST_FUNC int tcc_gen_machine_pool_flushes_total(void);
ST_FUNC int tcc_gen_machine_pool_entries_total(void);

/* Pad the instruction stream with a 2-byte NOP when the next instruction would
 * land halfword-odd.  Called for an IRQuadCompact carrying `align_target`; see
 * that field in tccir.h for the RP2350 measurement that motivates it. */
/* widen_from: start of the code a branch to the pad's address could come from
 * (the last IR op that emitted code, or TCC_ALIGN_WIDEN_FROM_PROLOG when none
 * has), or -1 when the pad must stay NOPs. */
#define TCC_ALIGN_WIDEN_FROM_PROLOG (-2)
ST_FUNC void tcc_gen_machine_align_branch_target(int align, int widen_from);
/* Whether a pad here could replace itself by widening the instruction just
 * before it (dry passes too, from what they laid out). */
ST_FUNC int tcc_gen_machine_pad_could_widen(void);
/* Real pass: emit the first instruction of the next IR op in its 32-bit form
 * when it has one, so an aligned join lands aligned with no NOP on the path
 * falling into it.  _done disarms and says whether it happened. */
ST_FUNC void tcc_gen_machine_widen_next_op(void);
ST_FUNC int tcc_gen_machine_widen_next_op_done(void);
ST_FUNC int tcc_gen_machine_const_needs_pool(int32_t value);
ST_FUNC int tcc_gen_machine_pending_pool_size(void);
ST_FUNC int tcc_gen_machine_cbz_jump_mop(int rn, int nonzero, int32_t target_ir, int ir_idx);
/* An inline compare-exchange whose result the next CMP + JUMPIF tests: offer
 * that branch before the call, ask after it whether the loop took it. */
ST_FUNC void tcc_gen_machine_cas_branch_offer(MachineOperand lhs, MachineOperand rhs, int cond, int target_ir,
                                              int cmp_ir);
ST_FUNC int tcc_gen_machine_cas_branch_taken(int *branch_addr);
ST_FUNC int tcc_gen_machine_switch_table_dry_run_size(int num_entries);
ST_FUNC void tcc_gen_machine_switch_table_mop(MachineOperand src, struct TCCIRSwitchTable *table, struct TCCIRState *ir,
                                              int ir_idx);
ST_FUNC int tcc_gen_machine_switch_load_dry_run_size(int num_entries);
ST_FUNC void tcc_gen_machine_switch_load_mop(MachineOperand src, MachineOperand dest,
                                             struct TCCIRSwitchValueTable *vtab, struct TCCIRState *ir, int ir_idx);
ST_FUNC void tcc_gen_machine_set_chain(int level);
ST_FUNC void tcc_gen_machine_chain_reset(void);
ST_FUNC void tcc_gen_machine_flush_chain(void);
ST_FUNC int tcc_gen_machine_frame_top_offset(void);
ST_FUNC void tcc_gen_machine_restore_chain(void);
ST_FUNC void tcc_gen_machine_init_chain_slot(IROperand src1, int level);
ST_FUNC void tcc_gen_machine_backpatch_jump(int address, int offset);
ST_FUNC void tcc_gen_machine_end_instruction(void);

/* Dry-run code generation interface for two-pass optimization */
ST_FUNC void tcc_gen_machine_dry_run_init(void);
ST_FUNC void tcc_gen_machine_dry_run_start(void);
ST_FUNC void tcc_gen_machine_dry_run_end(void);
ST_FUNC int tcc_gen_machine_dry_run_get_lr_push_count(void);
ST_FUNC uint32_t tcc_gen_machine_dry_run_get_scratch_regs_pushed(void);
ST_FUNC int tcc_gen_machine_dry_run_get_scratch_push_count(void);
ST_FUNC void tcc_gen_machine_dry_run_set_prolog_pushes(uint32_t regs);
ST_FUNC uint32_t tcc_gen_machine_dry_run_get_prolog_pushes_used(void);
ST_FUNC int tcc_gen_machine_dry_run_get_max_nested_saves(void);
/* Shared-.rodata base hoisting: claim() picks a callee-saved register for the
 * body and returns the mask the prologue must additionally push (0 = keep the
 * per-reference push/pop form).  Must be called once per function, after the
 * allocation has settled and before the prologue is emitted. */
ST_FUNC uint32_t tcc_gen_machine_rodata_anchor_claim(uint64_t used_registers);
ST_FUNC int tcc_gen_machine_rodata_anchor_ir_sites(const struct TCCIRState *ir, int stop_at);
ST_FUNC void tcc_gen_machine_rodata_anchor_reset(void);
ST_FUNC int tcc_gen_machine_rodata_anchor_get(void);
ST_FUNC void tcc_gen_machine_reset_scratch_state(void);
ST_FUNC int tcc_gen_machine_dry_run_is_active(void);
ST_FUNC int tcc_gen_machine_real_run_had_scratch_push(void);
/* Phase-3 per-instruction scratch constraint recording.
 * Call reset before each mop-dispatched instruction (in both dry-run and
 * real-emit passes); call count after to read how many scratch registers the
 * instruction allocated.  In debug builds the two passes should agree. */
ST_FUNC void tcc_gen_machine_insn_scratch_reset(void);
ST_FUNC int tcc_gen_machine_insn_scratch_count(void);
ST_FUNC uint16_t tcc_gen_machine_insn_scratch_saves_mask(void);
ST_FUNC int tcc_gen_machine_insn_scratch_save_count(void);
ST_FUNC uint32_t tcc_gen_machine_insn_scratch_exclude(void);

/* Branch optimization interface */
ST_FUNC void tcc_gen_machine_branch_opt_init(void);
ST_FUNC void tcc_gen_machine_branch_opt_analyze(uint32_t *ir_to_code_mapping, int mapping_size);

/* Reset the MOV-coalescing register-equivalence cache at IR instruction
 * boundaries (any IR op may be a branch target, so cross-IR equivalences
 * cannot be trusted). */
ST_FUNC void tcc_gen_machine_mov_coalesce_reset(void);
ST_FUNC void tcc_gen_machine_mov_equiv_reset(void);
ST_FUNC void tcc_gen_machine_reserve_pool_bytes(int upcoming_bytes);
ST_FUNC void tcc_gen_machine_strldr_cache_reset(void);
ST_FUNC void tcc_gen_machine_strldr_cache_set_enabled(int enabled);
ST_FUNC void tcc_gen_machine_str_elide_set_enabled(int enabled);
ST_FUNC void tcc_gen_machine_peepholes_set_enabled(int enabled);
ST_FUNC void tcc_gen_machine_slot_self_copy_set_enabled(int enabled);
ST_FUNC void tcc_gen_machine_frame_word_pair_set_enabled(int enabled);
ST_FUNC void tcc_gen_machine_imm_cache_reset(void);
ST_FUNC void tcc_gen_machine_imm_cache_invalidate_live(uint32_t live_mask);

/* Trap instruction generation */
ST_FUNC void tcc_gen_machine_trap_mop(void);

/* Prefetch instruction generation - rw: 0=read (PLD), 1=write (PLDW) */
ST_FUNC void tcc_gen_machine_prefetch_mop(MachineOperand addr, int rw);

/* Setjmp/longjmp instruction generation */
ST_FUNC void tcc_gen_machine_setjmp_mop(MachineOperand buf, MachineOperand area, MachineOperand dest);
ST_FUNC void tcc_gen_machine_longjmp_mop(MachineOperand buf);
ST_FUNC void tcc_gen_machine_nl_setjmp_mop(MachineOperand buf, MachineOperand dest);
ST_FUNC void tcc_gen_machine_nl_longjmp_mop(MachineOperand buf);

/* __builtin_apply_args / __builtin_apply instruction generation */
ST_FUNC void tcc_gen_machine_builtin_apply_args_mop(MachineOperand dest);
ST_FUNC void tcc_gen_machine_return_address_mop(MachineOperand dest);
ST_FUNC void tcc_gen_machine_builtin_apply_mop(MachineOperand fn, MachineOperand args, MachineOperand dest);

/* Block copy from const data to stack (LDM/STM on ARM) */
ST_FUNC void tcc_gen_machine_block_copy_mop(TCCIRState *ir, IROperand dest, IROperand src, int size);

/* Block copy between spill slots using LDM/STM (peephole for consecutive LOAD+STORE pairs) */
ST_FUNC void tcc_gen_machine_spill_block_copy(int32_t src_spill_off, int32_t dst_spill_off, int nwords);
ST_FUNC int tcc_gen_machine_calls_reload_r9(struct TCCIRState *ir);
/* Is a direct call to `sym` known (or, under -fmodule-local-calls, assumed) to
   bind inside this module, so that R9 survives it? */
ST_FUNC int tcc_gen_machine_callee_in_this_module(Sym *sym);
ST_FUNC int tcc_gen_machine_spill_block_copy_free(int32_t src_spill_off, int32_t dst_spill_off, int nwords,
                                                  uint32_t also_free);

/* A side of tcc_gen_machine_reg_block_copy that is a frame slot rather than a
 * base register: its offset is a spill-slot offset, to be taken from whichever
 * register the frame is addressed through. */
#define MACH_BLOCK_COPY_FRAME (-1)

/* Block copy of `nwords` words between two addresses, each a base register (a
 * pointer deref) or MACH_BLOCK_COPY_FRAME, through registers the caller owns */
ST_FUNC int tcc_gen_machine_reg_block_copy(int src_base, int32_t src_off, int dst_base, int32_t dst_off,
                                           const int *regs, int nwords, int end_idx);
/* Copy `nwords` words base+src_off -> base+dst_off that the IR copies one word
 * at a time over [start_idx, end_idx], through `word_regs` and registers free
 * across that run, as LDRD/STRD pairs or LDM/STM chunks; 0 when neither is
 * shorter (nothing emitted). */
ST_FUNC int tcc_gen_machine_interleaved_word_copy(int base, int32_t src_off, int32_t dst_off, int nwords,
                                                  uint32_t word_regs, int start_idx, int end_idx);

/* Conditional select: dest = (cond) ? then_val : else_val (ITE on ARM) */
ST_FUNC void tcc_gen_machine_select_mop(MachineOperand then_val, MachineOperand else_val, MachineOperand dest,
                                        int cond_code);

/* Predicated-compute fusion for an else-identity SELECT (see arm-thumb-gen.c). */
ST_FUNC int tcc_gen_machine_can_predicate_alu(MachineOperand src1, MachineOperand src2,
                                              MachineOperand dest, TccIrOp op);
ST_FUNC void tcc_gen_machine_predicated_alu_mop(MachineOperand src1, MachineOperand src2,
                                                MachineOperand dest, TccIrOp op, int cond_code);

/* MachineOperand load/store into specific physical registers (for inline asm) */
void tcc_gen_mach_load_to_reg(int dest_reg, const MachineOperand *op);
void tcc_gen_mach_store_from_reg(int src_reg, const MachineOperand *op);

ST_FUNC const char *tcc_get_abi_softcall_name(SValue *src1, SValue *src2, SValue *dest, TccIrOp op);

ST_FUNC int tcc_is_64bit_operand(SValue *sv);
ST_FUNC int tcc_has_quadruple_64bit_operand(SValue *src1, SValue *src2, SValue *dest, TccIrOp op);

#define stab_section s1->stab_section
#define stabstr_section stab_section->link
#define tcov_section s1->tcov_section
#define eh_frame_section s1->eh_frame_section
#define eh_frame_hdr_section s1->eh_frame_hdr_section
#define dwarf_info_section s1->dwarf_info_section
#define dwarf_abbrev_section s1->dwarf_abbrev_section
#define dwarf_line_section s1->dwarf_line_section
#define dwarf_aranges_section s1->dwarf_aranges_section
#define dwarf_ranges_section s1->dwarf_ranges_section
#define dwarf_str_section s1->dwarf_str_section
#define dwarf_line_str_section s1->dwarf_line_str_section
#define dwarf_frame_section s1->dwarf_frame_section

/* default dwarf version for "-gdwarf" */
#ifdef TCC_TARGET_MACHO
#define DEFAULT_DWARF_VERSION 2
#else
#define DEFAULT_DWARF_VERSION 5
#endif

/* default dwarf version for "-g". Always use DWARF (stabs removed). */
#ifndef CONFIG_DWARF_VERSION
#define CONFIG_DWARF_VERSION 5
#endif

#if defined TCC_TARGET_PE
#define R_DATA_32DW 'Z' /* fake code to avoid DLL relocs */
#elif defined TCC_TARGET_X86_64
#define R_DATA_32DW R_X86_64_32
#else
#define R_DATA_32DW R_DATA_32
#endif

/********************************************************/
#if CONFIG_TCC_SEMLOCK
#if defined _WIN32
typedef struct
{
  int init;
  CRITICAL_SECTION cs;
} TCCSem;
static inline void wait_sem(TCCSem *p)
{
  if (!p->init)
    InitializeCriticalSection(&p->cs), p->init = 1;
  EnterCriticalSection(&p->cs);
}
static inline void post_sem(TCCSem *p)
{
  LeaveCriticalSection(&p->cs);
}
#elif defined __APPLE__
#include <dispatch/dispatch.h>
typedef struct
{
  int init;
  dispatch_semaphore_t sem;
} TCCSem;
static inline void wait_sem(TCCSem *p)
{
  if (!p->init)
    p->sem = dispatch_semaphore_create(1), p->init = 1;
  dispatch_semaphore_wait(p->sem, DISPATCH_TIME_FOREVER);
}
static inline void post_sem(TCCSem *p)
{
  dispatch_semaphore_signal(p->sem);
}
#else
#include <semaphore.h>
typedef struct
{
  int init;
  sem_t sem;
} TCCSem;
static inline void wait_sem(TCCSem *p)
{
  if (!p->init)
    sem_init(&p->sem, 0, 1), p->init = 1;
  while (sem_wait(&p->sem) < 0 && errno == EINTR)
    ;
}
static inline void post_sem(TCCSem *p)
{
  sem_post(&p->sem);
}
#endif
#define TCC_SEM(s) TCCSem s
#define WAIT_SEM wait_sem
#define POST_SEM post_sem
#else
#define TCC_SEM(s)
#define WAIT_SEM(p)
#define POST_SEM(p)
#endif

/********************************************************/
#undef ST_DATA
#define ST_DATA
/********************************************************/

#define text_section TCC_STATE_VAR(text_section)
#define data_section TCC_STATE_VAR(data_section)
#define rodata_section TCC_STATE_VAR(rodata_section)
#define bss_section TCC_STATE_VAR(bss_section)
#define common_section TCC_STATE_VAR(common_section)
#define cur_text_section TCC_STATE_VAR(cur_text_section)
#define bounds_section TCC_STATE_VAR(bounds_section)
#define lbounds_section TCC_STATE_VAR(lbounds_section)
#define symtab_section TCC_STATE_VAR(symtab_section)
#define gnu_ext TCC_STATE_VAR(gnu_ext)
#define tcc_error_noabort TCC_SET_STATE(_tcc_error_noabort)
#define tcc_error TCC_SET_STATE(_tcc_error)
#if CONFIG_TCC_CHECKS
#define tcc_ice TCC_SET_STATE(_tcc_ice)
#else
#define tcc_ice(...) TCC_SET_STATE(_tcc_ice_stop)()
#endif
#define tcc_warning TCC_SET_STATE(_tcc_warning)
#define tcc_note TCC_SET_STATE(_tcc_note)

#define total_idents TCC_STATE_VAR(total_idents)
#define total_lines TCC_STATE_VAR(total_lines)
#define total_bytes TCC_STATE_VAR(total_bytes)

PUB_FUNC void tcc_enter_state(TCCState *s1);
PUB_FUNC void tcc_exit_state(TCCState *s1);

/* conditional warning depending on switch */
#define tcc_warning_c(sw)                                                                                              \
  TCC_SET_STATE((tcc_state->warn_num = offsetof(TCCState, sw) - offsetof(TCCState, warn_none), _tcc_warning))

/* tcc_warning_c(sw), once per place (file, line, option, message) and never
   in a system header: the frontend inliner replays a body at every call site
   besides compiling it on its own.  For the frontend checks. */
#define tcc_warning_once(sw, ...) _tcc_warning_once(offsetof(TCCState, sw), 0, __VA_ARGS__)
/* ... at LINE of the current file */
#define tcc_warning_once_at(sw, line, ...) _tcc_warning_once(offsetof(TCCState, sw), line, __VA_ARGS__)
/* ... a warning no -W option controls (only -w silences it) */
#define tcc_warning_once_always(...) _tcc_warning_once(offsetof(TCCState, warn_none), 0, __VA_ARGS__)
/* a yasc ownership rule the types alone break: always said; in the safe
   default under -Wlifetime, so it is an error there and unsafe { } lowers it */
#define tcc_yasc_rule(...)                                                                                             \
  _tcc_warning_once(tcc_state->strict_lifetime ? offsetof(TCCState, warn_lifetime) : offsetof(TCCState, warn_none), 0,   \
                    __VA_ARGS__)
/* the -W option SW is on at some line of the unit as #pragma diagnostic left
   it (an unsafe region only turns options off) */
#define tcc_diag_snaps_on(sw) (tcc_state->diag_snaps_or[offsetof(TCCState, sw) - offsetof(TCCState, warn_none)])
/* the -W option can speak here: set, or a #pragma diagnostic in the unit may
   turn it on (the check is skipped, at no cost, otherwise) */
#define tcc_warn_wanted(sw) (tcc_state->sw || tcc_diag_snaps_on(sw))
/* TOK names a global variable a system header declares */
#define tcc_sys_global(tok)                                                                                            \
  ((tok) >= 0 && ((tok) >> 3) < tcc_state->sys_globals_size && (tcc_state->sys_globals[(tok) >> 3] >> ((tok) & 7) & 1))

/********************************************************/
#endif /* _TCC_H */

#undef TCC_STATE_VAR
#undef TCC_SET_STATE

#ifdef USING_GLOBALS
#define TCC_STATE_VAR(sym) tcc_state->sym
#define TCC_SET_STATE(fn) fn
#undef USING_GLOBALS
#undef _tcc_error
#else
#define TCC_STATE_VAR(sym) s1->sym
#define TCC_SET_STATE(fn) (tcc_enter_state(s1), fn)
#define _tcc_error use_tcc_error_noabort
#endif

void dbg_print_vstack(const char *msg, const char *file, int line);

#define CEIL_DIV(x, y) (((x) + (y) - 1) / (y))
#define TCC_ALIGN(x, alignment) (((x) + (alignment) - 1) & ~((alignment) - 1))
#define ALIGN TCC_ALIGN

// debug helper
#if 0
#define print_vstack(msg) dbg_print_vstack(msg, __FILE__, __LINE__)
#else
#define print_vstack(msg)
#endif
