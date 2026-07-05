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

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
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

#ifndef offsetof
#define offsetof(type, field) ((size_t)&((type *)0)->field)
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

/* Unified logging — see log.h for all scope switches */
#include "log.h"

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

#if defined CONFIG_TCC_BCHECK && CONFIG_TCC_BCHECK == 0
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
#if defined TCC_TARGET_PE || defined _WIN32
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

#include "dwarf.h"
#include "libtcc.h"
#include "stab.h"
#include "tcctypes.h"

/* -------------------------------------------- */

#ifndef PUB_FUNC /* functions used by tcc.c but not in libtcc.h */
#define PUB_FUNC
#endif

/* Always compile from separate objects */
#define ST_INLN
#define ST_FUNC
#define ST_DATA extern

/* Target-specific definitions (after ST_FUNC is defined) */
#if defined(TCC_TARGET_ARM_THUMB)
#include "arm-thumb-defs.h"
#endif

#ifdef TCC_PROFILE /* profile all functions */
#define static
#define inline
#endif

/* Call ABI assignment query (types; target hook prototype is later). */
#include "tccabi.h"

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

#define INCLUDE_STACK_SIZE 32
#define IFDEF_STACK_SIZE 64
#define VSTACK_SIZE 256 /* YASOS: 512 was an upstream bump for the yarpgen fuzzer's
                           pathological expressions; 256 fits real code and saves
                           ~10 KiB .bss (513->257 * 40 B SValue). Clean tcc_error on overflow. */
#define STRING_MAX_SIZE 1024
#define TOKSTR_MAX_SIZE 256
#define PACK_STACK_SIZE 8

#define TOK_HASH_SIZE 2048 /* must be a power of two. YASOS: 4096 -> 2048 saves 8 KiB
                              .bss; device compiles intern few-hundred symbols so the
                              table stays sparse (lazy-lib interning keeps it sparser). */
#define TOK_ALLOC_INCR 256 /* must be a power of two */
#define TOK_MAX_SIZE 4     /* token max size in int unit when stored in string */

/* token symbol management */
typedef struct TokenSym
{
  struct TokenSym *hash_next;
  struct Sym *sym_define;     /* direct pointer to define */
  struct Sym *sym_label;      /* direct pointer to label */
  struct Sym *sym_struct;     /* direct pointer to structure */
  struct Sym *sym_identifier; /* direct pointer to identifier */
  int tok;                    /* token number */
  int len;
  char str[1];
} TokenSym;

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

#include "svalue.h"

// _Static_assert(sizeof(SValue) == 40, "SValue size changed");

/* symbol attributes */
struct SymAttr
{
  unsigned aligned : 5, /* alignment as log2+1 (0 == unspecified) */
      packed : 1, weak : 1, visibility : 2, dllexport : 1, nodecorate : 1, dllimport : 1, addrtaken : 1, nodebug : 1,
      naked : 1, nested_func : 1, /* nested function flag */
      sso_be : 1,                 /* scalar_storage_order("big-endian") */
      transparent_union : 1,      /* __attribute__((transparent_union)) */
      possibly_written : 1,       /* global may have been written after its
                                     initializer (a store was emitted, or its
                                     address escaped to a non-const pointer).
                                     Used by inline-eval to decide whether
                                     `*&g` can fold to the initializer. */
      tu_no_readers : 1,          /* end-of-TU analysis confirmed no reachable
                                     function reads this static global.  Set
                                     by tcc_ir_tu_analyze_dead_statics; read
                                     by dead-static-store-elim during the
                                     end-of-TU late_reopt phase. */
      param_volatile : 1;         /* original parameter declaration was volatile
                                     before function-type normalization stripped
                                     top-level qualifiers. */
};

/* function attributes or temporary attributes for parsing */
struct FuncAttr
{
  unsigned func_call : 3,               /* calling convention (0..5), see below */
      func_type : 2,                    /* FUNC_OLD/NEW/ELLIPSIS */
      func_noreturn : 1,                /* attribute((noreturn)) */
      func_ctor : 1,                    /* attribute((constructor)) */
      func_dtor : 1,                    /* attribute((destructor)) */
      func_args : 8,                    /* PE __stdcall args */
      func_alwinl : 1,                  /* always_inline */
      func_noinline : 1,                /* noinline — suppress auto-inline */
      func_pure : 1,                    /* attribute((pure)) - no side effects, reads memory */
      func_const : 1,                   /* attribute((const)) - no side effects, no memory reads */
      func_no_instrument : 1,           /* attribute((no_instrument_function)) */
      func_va_arg_pack : 1,             /* uses __builtin_va_arg_pack() */
      func_rewritten_extern_inline : 1, /* extern inline rewritten to non-extern inline-only def */
      func_outofline_needed : 1,        /* always_inline call could not stay call-site-only */
      func_auto_inline : 1,             /* compiler-selected auto-inline candidate (small func) */
      func_inline_call_heavy : 1,       /* auto-inline body keeps a non-foldable call: budget-limit expansions */
      func_eval_only_inline : 1,        /* body saved for const-fold only, not regular inlining */
      func_pure_via_sret : 1,           /* inferred: only observable side effect is *sret_arg writes */
      func_late_reopt : 1,              /* body kept for end-of-TU re-optimization (non-const static global fold) */
      tu_static_writer : 1,             /* function writes >=1 non-const static global (set during summary collection) */
      tu_reachable : 1,                 /* function is reachable from a TU root (non-static or addr-taken) per call graph */
      func_compiled : 1,                /* gen_function has completed for this sym at least once — distinguishes
                                           forward-declared-not-yet-defined functions from already-emitted ones,
                                           used by late_reopt triggering for inter-procedural noreturn propagation */
      func_keep_tokens_for_noreturn : 1; /* tokens preserved so end-of-TU noreturn propagation can decide whether to
                                            re-emit — separate from func_late_reopt so we don't trigger unnecessary
                                            re-emit before we know if any callee turned out to be noreturn */
};

/* symbol management */
struct Sym
{
  int v;            /* symbol token */
  unsigned short r; /* associated register or VT_CONST/VT_LOCAL and LVAL type */
  struct SymAttr a; /* symbol attributes */
  int vreg;
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
    long long enum_val; /* enum constant if IS_ENUM_VAL */
    int *d;             /* define token stream */
    struct Sym *cleanup_func;
  };

  CType type; /* associated type */
  union
  {
    struct Sym *next;         /* next related symbol (for fields and anoms) */
    int *e;                   /* expanded token stream */
    int asm_label;            /* associated asm label */
    struct Sym *cleanupstate; /* in defined labels */
    int *vla_array_str;       /* vla array code */
  };
  struct Sym *prev;                        /* prev symbol in stack */
  struct Sym *prev_tok;                    /* previous symbol for this token */
  int vla_size_loc;                        /* for structs with VLA members: stack offset holding
                                              runtime total struct size (0 = not a VLA struct) */
  unsigned long long objsize_max_value;    /* conservative max scalar value assigned locally */
  unsigned long long objsize_strlen_value; /* conservative max NUL-terminated string bytes */
  unsigned char objsize_max_valid;
  unsigned char objsize_strlen_valid;
  /* Captured constant initializer bytes for small local arrays/vectors.
   * Set by decl_initializer_alloc when all init values are compile-time
   * constants; invalidated by vstore when the variable is later written.
   * Used by __builtin_shuffle and similar intrinsics to fold runtime
   * mask loads into constant indices. */
  unsigned char *const_init_data;
  int const_init_size;
  unsigned char const_init_valid;
  unsigned char const_init_in_progress;
};

#include "ir/machine_op.h"
#include "tccir.h"

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
  const char *source_path; /* Path to source file (reference, not owned) */
  uint32_t file_offset;    /* Relative offset within source file */
  uint32_t size;           /* Size of this chunk */
  uint32_t dest_offset;    /* Offset in destination section */
  struct DeferredChunk *next;
  int materialized; /* 1 if this chunk has been loaded */
} DeferredChunk;

/* section definition */
typedef struct Section
{
  unsigned long data_offset;    /* current data offset */
  unsigned char *data;          /* section data */
  unsigned long data_allocated; /* used for realloc() handling */
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
  int nb_hashed_syms;      /* used to resize the hash table */
  struct Section *link;    /* link to another section */
  struct Section *reloc;   /* corresponding section for relocation, if any */
  struct Section *hash;    /* hash table for symbols */
  struct Section *prev;    /* previous section on section stack */
  /* Lazy loading support - use int instead of bit fields to avoid padding issues */
  int lazy;                     /* 1 = section uses lazy loading */
  int materialized;             /* 1 = data has been loaded (legacy) */
  int has_deferred_chunks;      /* 1 = has chunks not yet materialized */
  int fully_materialized;       /* 1 = all chunks materialized */
  DeferredChunk *deferred_head; /* List of chunks to load */
  DeferredChunk *deferred_tail; /* For O(1) append */
  /* Relocation patches - stored as dynamic array for memory efficiency
   * Each patch is 8 bytes (offset+value) vs 24 bytes with linked list */
  uint32_t *reloc_patch_offsets; /* Array of patch offsets */
  uint32_t *reloc_patch_values;  /* Array of patch values */
  int nb_reloc_patches;          /* Number of patches */
  int alloc_reloc_patches;       /* Allocated size of arrays */
  /* String table deduplication - hash table for quick lookup */
  uint32_t *str_hash; /* Hash table: hash -> offset in data */
  int str_hash_size;  /* Size of hash table */
  int str_hash_count; /* Number of entries in hash */
  /* Hash value cache for fast find_elf_sym - avoids strcmp on mismatches */
  unsigned int *hash_val_cache; /* full hash values indexed by sym_index */
  int hash_val_alloc;           /* allocated size of cache */
  char name[1];                 /* section name */
} Section;

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

/* field 'Sym.r' for C labels */
#define LABEL_DEFINED 0  /* label is defined */
#define LABEL_FORWARD 1  /* label is forward defined */
#define LABEL_DECLARED 2 /* label is declared but never used */
#define LABEL_GONE                                                                                                     \
  3 /* label isn't in scope, but not yet popped                                                                        \
       from local_label_stack (stmt exprs) */

/* type_decl() types */
#define TYPE_ABSTRACT 1 /* type without variable */
#define TYPE_DIRECT 2   /* type with variable */
#define TYPE_PARAM 4    /* type declares function parameter */
#define TYPE_NEST 8     /* nested call to post_type */

#define IO_BUF_SIZE 8192

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
  unsigned char unget[4];
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
  char alloc;
  signed char need_spc;         /* space insertion state: -1, 0, 1, 2, 3 */
  unsigned short last_line_num; /* last recorded line number (0 = none) */
  unsigned short save_line_num; /* saved line number for macro */
  int allocated_len;            /* 0 = inline, >0 = heap capacity (in ints) */
  int len;                      /* current length in ints */
  /* used to chain token-strings with begin/end_macro() */
  const int *prev_ptr;
  union
  {
    int *str;                            /* heap buffer pointer */
    int small_buf[TOKSTR_SMALL_BUFSIZE]; /* inline buffer for small strings */
  } data;
  struct TokenString *prev;
} TokenString;

/* Access TokenString buffer (either inline small_buf or heap str) */
#define tok_str_buf(s) ((s)->allocated_len > 0 ? (s)->data.str : (s)->data.small_buf)

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
  int vector_size;  /* __attribute__((vector_size(N))) — total bytes, 0 if not a vector */
} AttributeDef;

/* inline functions */
typedef struct InlineFunc
{
  TokenString *func_str;
  Sym *sym;
  int inline_count; /* number of auto-inline expansions performed so far (call-heavy budget) */
  char filename[1];
} InlineFunc;

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
  /* Parent scope struct/union/enum tags visible to nested function body.
   * We store pointers to the original Sym (which survives pop_local_syms
   * because completed struct tags have c != 0). */
  Sym *parent_struct_tag_syms[MAX_CAPTURED_VARS]; /* original struct tag syms */
  int nb_parent_struct_tags;                      /* count of saved struct tags */
} NestedFunc;

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
  int is_llong;     /* true if double register value */
  int is_memory;    /* true if memory operand */
  int is_rw;        /* for '+' modifier */
  int is_label;     /* for asm goto */
} ASMOperand;
#endif

/* extra symbol attributes (not in symbol table) */
struct sym_attr
{
  unsigned got_offset;
  unsigned plt_offset;
  int plt_sym;
  int dyn_index;
#if defined(TCC_TARGET_ARM) || defined(TCC_TARGET_ARM_THUMB)
  unsigned char plt_thumb_stub : 1;
#endif
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
  unsigned int *name_hashes;          /* pre-computed elf_hash per symbol */
  unsigned long long *member_offsets; /* file offsets per symbol */
  int *ar_ht_buckets;                 /* chained hash table buckets */
  int *ar_ht_next;                    /* chained hash table chain links */
  int ar_ht_mask;                     /* hash table mask (size - 1) */
  unsigned long long *loaded_members; /* dedup set for loaded members */
  unsigned int loaded_member_mask;    /* dedup set mask */
} ArchiveSymbolCache;

/* Per-TU stash of optimized IR for `static` functions that look eligible
 * for later inlining. Populated at the end of gen_function(); flushed at
 * tccgen_finish(). Phase 0: stash only, no consumers — exists to validate
 * the lifecycle change before the inliner pass lands. */
typedef struct StashedFuncIR
{
  Sym *sym;
  TCCIRState *ir;
} StashedFuncIR;

struct TCCState
{
  unsigned char verbose;           /* if true, display some information during compilation */
  unsigned char nostdinc;          /* if true, no standard headers are added */
  unsigned char nostdlib;          /* if true, no standard libraries are added */
  unsigned char nodefaultlibs;     /* if true, no default libraries at all (incl. compiler-rt) */
  unsigned char nocommon;          /* if true, do not use common symbols for .bss data */
  unsigned char static_link;       /* if true, static linking is performed */
  unsigned char rdynamic;          /* if true, all symbols are exported */
  unsigned char symbolic;          /* if true, resolve symbols in the current module first */
  unsigned char filetype;          /* file type for compilation (NONE,C,ASM) */
  unsigned char optimize;          /* only to #define __OPTIMIZE__ */
  unsigned char option_pthread;    /* -pthread option */
  unsigned char enable_new_dtags;  /* -Wl,--enable-new-dtags */
  unsigned char gc_sections;       /* -Wl,--gc-sections: garbage collect unused sections */
  unsigned char function_sections; /* -ffunction-sections: place each function
                                      in its own section */
  unsigned char data_sections;     /* -fdata-sections: place each data item in its
                                      own section */
  unsigned int cversion;           /* supported C ISO version, 199901 (the default), 201112, ... */

  /* C language options */
  unsigned char char_is_unsigned;
  unsigned char leading_underscore;
  unsigned char ms_extensions;          /* allow nested named struct w/o identifier
                                           behave like unnamed */
  unsigned char dollars_in_identifiers; /* allows '$' char in identifiers */
  unsigned char ms_bitfields;           /* if true, emulate MS algorithm for aligning bitfields */
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
  unsigned int no_builtin_funcs;

  /* warning switches */
  unsigned char warn_none;
  unsigned char warn_all;
  unsigned char warn_error;
  unsigned char warn_write_strings;
  unsigned char warn_unsupported;
  unsigned char warn_implicit_function_declaration;
  unsigned char warn_discarded_qualifiers;
#define WARN_ON 1         /* warning is on (-Woption) */
  unsigned char warn_num; /* temp var for tcc_warning_c() */

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

  /* IR optimization flags (-f options) */
  unsigned char opt_dce;              /* -fdce: dead code elimination */
  unsigned char opt_const_prop;       /* -fconst-prop: constant propagation */
  unsigned char opt_copy_prop;        /* -fcopy-prop: copy propagation */
  unsigned char opt_cse;              /* -fcse: common subexpression elimination */
  unsigned char opt_bool_cse;         /* -fbool-cse: boolean CSE */
  unsigned char opt_bool_idempotent;  /* -fbool-idempotent: boolean idempotent simplification */
  unsigned char opt_bool_simplify;    /* -fbool-simplify: boolean expression simplification */
  unsigned char opt_store_load_fwd;   /* -fstore-load-fwd: store-load forwarding */
  unsigned char opt_redundant_store;  /* -fredundant-store-elim: redundant store elimination */
  unsigned char opt_dead_store;       /* -fdead-store-elim: dead store elimination */
  unsigned char opt_fp_offset_cache;  /* -ffp-offset-cache: frame pointer offset caching */
  unsigned char opt_indexed_memory;   /* -findexed-memory: indexed load/store fusion */
  unsigned char opt_disp_fusion;      /* -fdisp-fusion: ADD+LOAD/STORE -> displacement-addressed mem op */
  unsigned char opt_lea_fold;         /* -flea-fold: LEA Addr[StackLoc]+deref -> direct stack slot access */
  unsigned char opt_postinc_fusion;   /* -fpostinc-fusion: post-increment load/store fusion */
  unsigned char opt_mla_fusion;       /* -fmla-fusion: multiply-accumulate fusion */
  unsigned char opt_stack_addr_cse;   /* -fstack-addr-cse: stack address CSE */
  unsigned char opt_licm;             /* -flicm: loop-invariant code motion */
  unsigned char opt_strength_red;     /* -fstrength-reduce: strength reduction for multiply */
  unsigned char opt_iv_strength_red;  /* -fiv-strength-red: IV strength reduction for array access */
  unsigned char opt_loop_unroll;      /* -floop-unroll: full unroll small constant-trip-count loops */
  unsigned char opt_loop_rotation;    /* -floop-rotation: rotate top-tested loops to bottom-tested */
  unsigned char opt_reroll;           /* -freroll-blocks: re-roll N identical consecutive blocks into a loop */
  unsigned char opt_nonneg_fold;      /* -fnonneg-fold: non-negative value branch folding */
  unsigned char opt_vrp;              /* -fvrp: value range propagation branch folding */
  unsigned char opt_float_narrow;     /* -ffloat-narrow: narrow double math to float when safe */
  unsigned char opt_jump_threading;   /* -fjump-threading: jump threading optimization */
  unsigned char opt_inline_functions; /* -finline-functions: auto-inline small functions at -O2 */
  unsigned char opt_inline_small;     /* -finline-small-functions: auto-inline tiny functions at -O1 */
  unsigned char opt_ipc;              /* interprocedural constant propagation */
  int opt_inline_limit;               /* -finline-limit=N: token-stream word threshold (default 0=use level default) */
  unsigned char instrument_functions; /* -finstrument-functions */

  /* Function purity cache for LICM optimization */
  /* Cache stores inferred purity for functions in the current translation unit */
#define FUNC_PURITY_CACHE_SIZE 256
  struct
  {
    int token;  /* Function name token (v field of Sym) */
    int purity; /* TCC_FUNC_PURITY_* value */
  } func_purity_cache[FUNC_PURITY_CACHE_SIZE];
  int func_purity_cache_count;

  /* Constant function result cache for interprocedural constant propagation.
   * After optimization, functions that reduce to "return #const" are cached
   * so callers can replace FUNCCALLVAL with ASSIGN #const. */
#define FUNC_CONST_RESULT_CACHE_SIZE 256
  struct
  {
    int token;      /* Function name token (v field of Sym) */
    int64_t value;  /* Constant return value */
    int btype;      /* IROP_BTYPE_* of return value */
  } func_const_result_cache[FUNC_CONST_RESULT_CACHE_SIZE];
  int func_const_result_cache_count;

  /* Switch-value function snapshot cache: holds replayable bodies of
   * side-effect-free single-parameter functions whose return value depends on
   * the argument (e.g. `static int f(int x) { switch (x) { case K: return C; } }`).
   * Callers with a constant argument simulate the snapshot to fold the call. */
#define FUNC_SWITCH_CACHE_SIZE 64
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
  const char *march_str;   /* -march= value, NULL means default */
#endif
  unsigned char text_and_data_separation; /* support for GCC
                                             -mno-pic-data-is-text-relative */
  unsigned int yaff_stack_size; /* -stack-size=N: per-image stack hint (bytes)
                                   written into the YAFF header; 0 = default */
  unsigned int yaff_heap_size;  /* -heap-size=N: per-image heap cap (bytes)
                                   written into the YAFF header; 0 = default */
  unsigned char share_rodata;   /* -share-rodata: RELRO — route const objects
                                   with pointer-bearing types to the writable
                                   data segment so .rodata stays pure-const and
                                   can be shared (XIP) across processes */

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

  /* include paths */
  char **include_paths;
  int nb_include_paths;

  char **sysinclude_paths;
  int nb_sysinclude_paths;

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
  char **pragma_libs;
  int nb_pragma_libs;

  /* inline functions are stored as token lists and compiled last
     only if referenced */
  struct InlineFunc **inline_fns;
  int nb_inline_fns;

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
  struct sym_attr *sym_attrs;
  int nb_sym_attrs;
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
  char *deps_outfile;      /* option -MF */
  int argc;
  char **argv;
  CString linker_arg; /* collect -Wl options */
  int thumb_func;
  TCCIRState *ir;
  /* Inliner stash: optimized IR for eligible `static` functions, kept alive
   * past the per-function gen_function() free so a future inliner pass can
   * splice it into callers. Phase 0: no consumers yet. */
  StashedFuncIR *stashed_func_irs;
  int nb_stashed_func_irs;
  int stashed_func_irs_capacity;
  /* Nested functions - saved token streams for functions defined inside other functions */
  NestedFunc *nested_funcs;
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
  int inline_return_loc;       /* stack offset for storing return value */
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
  STRBI_MEMCMP,
  STRBI_MEMCMP_EQ,
  STRBI_MEMCHR,
  STRBI_MEMMOVE,
  STRBI_BCOPY,
  STRBI_MEMPCPY,
  STRBI_INDEX,
  STRBI_RINDEX,
};

/* A deferred fixup for a label-difference expression (&&sym1 - &&sym2)
   used in a static initializer.  Recorded during parsing, resolved
   after code generation when both label symbols have their final
   code offsets. */
typedef struct LabelDiffFixup
{
  Section *sec;          /* data section containing the value */
  unsigned long offset;  /* byte offset within sec->data */
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
/* currently unused: 0x000[48]0000  */

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
#include "tcctok.h"
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

static inline int resolve_str_builtin_by_tok(int tok)
{
  switch (tok)
  {
  case TOK_builtin_strlen:
    return STRBI_STRLEN;
  case TOK_builtin_strnlen:
    return STRBI_STRNLEN;
  case TOK_builtin_strcmp:
    return STRBI_STRCMP;
  case TOK_builtin_strncmp:
    return STRBI_STRNCMP;
  case TOK_builtin_strcpy:
    return STRBI_STRCPY;
  case TOK_builtin_strncpy:
    return STRBI_STRNCPY;
  case TOK_builtin_stpcpy:
    return STRBI_STPCPY;
  case TOK_builtin_stpncpy:
    return STRBI_STPNCPY;
  case TOK_builtin_strcat:
    return STRBI_STRCAT;
  case TOK_builtin_strncat:
    return STRBI_STRNCAT;
  case TOK_builtin_strchr:
    return STRBI_STRCHR;
  case TOK_builtin_strrchr:
    return STRBI_STRRCHR;
  case TOK_builtin_strstr:
    return STRBI_STRSTR;
  case TOK_builtin_strpbrk:
    return STRBI_STRPBRK;
  case TOK_builtin_strcspn:
    return STRBI_STRCSPN;
  case TOK_builtin_memcmp:
    return STRBI_MEMCMP;
  case TOK_builtin_memcmp_eq:
    return STRBI_MEMCMP_EQ;
  case TOK_builtin_memchr:
    return STRBI_MEMCHR;
  case TOK_builtin_memmove:
    return STRBI_MEMMOVE;
  case TOK_builtin_mempcpy:
    return STRBI_MEMPCPY;
  default:
    return STRBI_UNKNOWN;
  }
}

static inline int resolve_str_builtin_id(int tok, const char *name)
{
  static const struct
  {
    const char *name;
    int id;
  } map[] = {{"strlen", STRBI_STRLEN},           {"__tcc_strlen", STRBI_STRLEN},
             {"strnlen", STRBI_STRNLEN},         {"strcmp", STRBI_STRCMP},
             {"__tcc_strcmp", STRBI_STRCMP},     {"strncmp", STRBI_STRNCMP},
             {"__tcc_strncmp", STRBI_STRNCMP},   {"strcpy", STRBI_STRCPY},
             {"__tcc_strcpy", STRBI_STRCPY},     {"strncpy", STRBI_STRNCPY},
             {"__tcc_strncpy", STRBI_STRNCPY},   {"stpcpy", STRBI_STPCPY},
             {"__tcc_stpcpy", STRBI_STPCPY},     {"stpncpy", STRBI_STPNCPY},
             {"__tcc_stpncpy", STRBI_STPNCPY},   {"strcat", STRBI_STRCAT},
             {"__tcc_strcat", STRBI_STRCAT},     {"strncat", STRBI_STRNCAT},
             {"__tcc_strncat", STRBI_STRNCAT},   {"strchr", STRBI_STRCHR},
             {"__tcc_strchr", STRBI_STRCHR},     {"strrchr", STRBI_STRRCHR},
             {"__tcc_strrchr", STRBI_STRRCHR},   {"strstr", STRBI_STRSTR},
             {"__tcc_strstr", STRBI_STRSTR},     {"strpbrk", STRBI_STRPBRK},
             {"__tcc_strpbrk", STRBI_STRPBRK},   {"strcspn", STRBI_STRCSPN},
             {"__tcc_strcspn", STRBI_STRCSPN},   {"memcmp", STRBI_MEMCMP},
             {"__builtin_memcmp_eq", STRBI_MEMCMP_EQ},
             {"memchr", STRBI_MEMCHR},           {"memmove", STRBI_MEMMOVE},
             {"__tcc_memmove", STRBI_MEMMOVE},   {"bcopy", STRBI_BCOPY},
             {"__tcc_bcopy", STRBI_BCOPY},       {"mempcpy", STRBI_MEMPCPY},
             {"__tcc_mempcpy", STRBI_MEMPCPY},   {"index", STRBI_INDEX},
             {"rindex", STRBI_RINDEX},           {"__builtin_index", STRBI_INDEX},
             {"__builtin_rindex", STRBI_RINDEX}, {NULL, STRBI_UNKNOWN}};
  int id = resolve_str_builtin_by_tok(tok);
  if (id != STRBI_UNKNOWN)
    return id;
  if (!name)
    return STRBI_UNKNOWN;
  for (int i = 0; map[i].name; i++)
  {
    if (strcmp(name, map[i].name) == 0)
      return map[i].id;
  }
  return STRBI_UNKNOWN;
}

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
PUB_FUNC void _tcc_warning(const char *fmt, ...) PRINTF_LIKE(1, 2);
#define tcc_internal_error(msg) tcc_error("internal compiler error in %s:%d: %s", __FUNCTION__, __LINE__, msg)

/* other utilities */
ST_FUNC void dynarray_add(void *ptab, int *nb_ptr, void *data);
ST_FUNC void dynarray_reset(void *pp, int *n);
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
PUB_FUNC void tcc_bench_log(TCCState *s1, const char *operation, const char *name, unsigned elapsed_ms);
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
#define OPT_M32 32
#define OPT_M64 64

/* ------------ tccpp.c ------------ */

ST_DATA struct BufferedFile *file;
ST_DATA int tok;
ST_DATA CValue tokc;
ST_DATA const int *macro_ptr;
ST_DATA int parse_flags;
ST_DATA int tok_flags;
ST_DATA CString tokcstr; /* current parsed string, if any */
/* When non-NULL (set by skip_or_save_block while recording a function body for
   later token-stream replay), #pragma pack directives are appended to this
   stream as TOK_PACK_REPLAY actions instead of mutating pack_stack now, so the
   pack state is applied at the struct's position during replay, not eagerly
   during the recording scan. */
ST_DATA TokenString *pp_pragma_capture;

/* display benchmark infos */
ST_DATA int tok_ident;
ST_DATA TokenSym **table_ident;
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
ST_FUNC int tok_alloc_const(const char *str);
ST_FUNC const char *get_tok_str(int v, CValue *cv);
ST_FUNC void begin_macro(TokenString *str, int alloc);
ST_FUNC void end_macro(void);
ST_FUNC void end_macro_to(TokenString *target);
ST_FUNC int set_idnum(int c, int val);
ST_INLN void tok_str_new(TokenString *s);
ST_FUNC TokenString *tok_str_alloc(void);
ST_FUNC void tok_str_free(TokenString *s);
ST_FUNC void tok_str_free_str(int *str);
ST_FUNC int *tok_str_ensure_heap(TokenString *s);
ST_FUNC void tok_str_add(TokenString *s, int t);
ST_FUNC void tok_str_add2(TokenString *s, int t, CValue *cv);
ST_FUNC void tok_str_add_tok(TokenString *s);
ST_FUNC void tok_get(int *t, const int **pp, CValue *cv);
ST_FUNC void pp_apply_pack_replay(TCCState *s1, int code);
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

/* ------------ tccgen.c ------------ */

#define SYM_POOL_NB (8192 / sizeof(Sym))

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
ST_DATA int global_expr;   /* true if compound literals must be allocated globally
                              (used during initializers parsing */
ST_DATA CType func_vt;     /* current function return type (used by return instruction) */
ST_DATA int func_var;      /* true if current function is variadic */
ST_DATA int func_vc;
ST_DATA int func_ind;
ST_DATA int func_has_label_addr; /* true if current function uses &&label (computed goto) */
ST_DATA const char *funcname;

ST_FUNC void tccgen_init(TCCState *s1);
ST_FUNC int tccgen_compile(TCCState *s1);
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

ST_FUNC Sym *global_identifier_push(int v, int t, int c);
ST_FUNC Sym *external_global_sym(int v, CType *type);
ST_FUNC Sym *external_helper_sym(int v);
ST_FUNC void vpush_helper_func(int v);
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
#if PTR_SIZE == 4
ST_FUNC void lexpand(void);
#endif
ST_FUNC void gaddrof(void);
ST_FUNC int gv(int rc);
ST_FUNC void gv2(int rc1, int rc2);
ST_FUNC void gen_op(int op);
ST_FUNC int type_size(const CType *type, int *a);
ST_FUNC void mk_pointer(CType *type);
ST_FUNC void vstore(void);
ST_FUNC void inc(int post, int c);
ST_FUNC CString *parse_mult_str(const char *msg);
ST_FUNC CString *parse_asm_str(void);
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
ST_FUNC void section_materialize(TCCState *s1, Section *sec);
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
ST_FUNC void put_elf_reloc(Section *symtab, Section *s, unsigned long offset, int type, int symbol);
ST_FUNC void put_elf_reloca(Section *symtab, Section *s, unsigned long offset, int type, int symbol, addr_t addend);

ST_FUNC void resolve_common_syms(TCCState *s1);
ST_FUNC void relocate_syms(TCCState *s1, Section *symtab, int do_resolve);
ST_FUNC void relocate_sections(TCCState *s1);

ST_FUNC ssize_t full_read(int fd, void *buf, size_t count);
ST_FUNC void *load_data(int fd, unsigned long file_offset, unsigned long size);
ST_FUNC int tcc_object_type(int fd, ElfW(Ehdr) * h);
ST_FUNC int tcc_load_object_file(TCCState *s1, int fd, unsigned long file_offset);
ST_FUNC int tcc_load_object_file_lazy(TCCState *s1, int fd, unsigned long file_offset);
ST_FUNC void tcc_gc_mark_phase(TCCState *s1);
ST_FUNC void tcc_load_referenced_sections(TCCState *s1);
ST_FUNC void tcc_free_lazy_objfiles(TCCState *s1);
ST_FUNC int tcc_load_archive(TCCState *s1, int fd, int alacarte);
ST_FUNC void tcc_archive_cache_free(TCCState *s1);
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
static inline void add64le(unsigned char *p, int64_t x)
{
  write64le(p, read64le(p) + x);
}
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

#include "tcc_target.h"

/* ------------ arm-gen.c ------------ */
#if defined(TCC_TARGET_ARM) || defined(TCC_TARGET_ARM_THUMB)
#if defined(TCC_ARM_EABI) && !defined(CONFIG_TCC_ELFINTERP)
PUB_FUNC const char *default_elfinterp(struct TCCState *s);
#endif
ST_FUNC void arm_init(struct TCCState *s);
ST_FUNC void arm_deinit(struct TCCState *s);
ST_FUNC void gen_increment_tcov(SValue *sv);
#endif

/* ------------ tccasm.c ------------ */
ST_FUNC void asm_instr(void);
ST_FUNC void asm_global_instr(void);
ST_FUNC int tcc_assemble(TCCState *s1, int do_preprocess);
#ifdef CONFIG_TCC_ASM
ST_FUNC int find_constraint(ASMOperand *operands, int nb_operands, const char *name, const char **pp);
ST_FUNC Sym *get_asm_sym(int name, Sym *csym);
ST_FUNC void asm_expr(TCCState *s1, ExprValue *pe);
ST_FUNC int asm_int_expr(TCCState *s1);
/* ------------ i386-asm.c ------------ */
ST_FUNC void gen_expr32(ExprValue *pe);
#ifdef TCC_TARGET_X86_64
ST_FUNC void gen_expr64(ExprValue *pe);
#endif
ST_FUNC void asm_opcode(TCCState *s1, int opcode);
ST_FUNC int asm_parse_regvar(int t);
ST_FUNC void asm_compute_constraints(ASMOperand *operands, int nb_operands, int nb_outputs, const uint8_t *clobber_regs,
                                     const uint8_t *reserved_regs, int *pout_reg);
ST_FUNC void subst_asm_operand(CString *add_str, SValue *sv, int modifier);
ST_FUNC void asm_gen_code(ASMOperand *operands, int nb_operands, int nb_outputs, int is_output, uint8_t *clobber_regs,
                          int out_reg);
ST_FUNC void asm_clobber(uint8_t *clobber_regs, const char *str);
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
ST_FUNC void tcc_debug_line_num(TCCState *s1, int line_num);
ST_FUNC void tcc_add_debug_info(TCCState *s1, int param, Sym *s, Sym *e);
ST_FUNC void tcc_debug_save_state(TCCState *s1, void **saved_info, void **saved_root);
ST_FUNC void tcc_debug_restore_state(TCCState *s1, void *saved_info, void *saved_root);
ST_FUNC void tcc_debug_funcstart(TCCState *s1, Sym *sym);
ST_FUNC void tcc_debug_prolog_epilog(TCCState *s1, int value);
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
ST_FUNC void tcc_gen_machine_data_processing_mop_flags(MachineOperand src1, MachineOperand src2, MachineOperand dest,
                                                       TccIrOp op);
ST_FUNC void tcc_gen_machine_cmp_eq64_mop(MachineOperand src1, MachineOperand src2);
/* SUBS+IT peephole helper: emits `SUBS dest, src1, src2; IT NE; MOVNE dest, #1`
 * collapsing a CMP+SELECT(1,0,NE) / SELECT(0,1,EQ) pair into 3 instructions.
 * src2 must be MACH_OP_IMM. Returns 1 on emit, 0 if the SUBS immediate can't
 * be encoded and the caller should fall back to the regular CMP+SELECT. */
ST_FUNC int tcc_gen_machine_subs_eq_select_01(MachineOperand src1, MachineOperand src2, MachineOperand dest);
ST_FUNC void tcc_gen_machine_ubfx_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest);
ST_FUNC void tcc_gen_machine_bfi_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, uint32_t params);
ST_FUNC void tcc_gen_machine_assign_mop(MachineOperand src, MachineOperand dest, TccIrOp op);
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
ST_FUNC int tcc_gen_machine_conditional_jump_mop(int32_t condition, TccIrOp op, int32_t target_ir, int ir_idx);
ST_FUNC int tcc_gen_machine_pending_pool_size(void);
ST_FUNC int tcc_gen_machine_cbz_jump_mop(int rn, int nonzero, int32_t target_ir, int ir_idx);
ST_FUNC int tcc_gen_machine_switch_table_dry_run_size(int num_entries);
ST_FUNC void tcc_gen_machine_switch_table_mop(MachineOperand src, struct TCCIRSwitchTable *table, struct TCCIRState *ir,
                                              int ir_idx);
ST_FUNC int tcc_gen_machine_switch_load_dry_run_size(int num_entries);
ST_FUNC void tcc_gen_machine_switch_load_mop(MachineOperand src, MachineOperand dest,
                                             struct TCCIRSwitchValueTable *vtab, struct TCCIRState *ir, int ir_idx);
ST_FUNC void tcc_gen_machine_set_chain(void);
ST_FUNC void tcc_gen_machine_restore_chain(void);
ST_FUNC void tcc_gen_machine_init_chain_slot(IROperand src1);
ST_FUNC void tcc_gen_machine_backpatch_jump(int address, int offset);
ST_FUNC void tcc_gen_machine_end_instruction(void);

/* Dry-run code generation interface for two-pass optimization */
ST_FUNC void tcc_gen_machine_dry_run_init(void);
ST_FUNC void tcc_gen_machine_dry_run_start(void);
ST_FUNC void tcc_gen_machine_dry_run_end(void);
ST_FUNC int tcc_gen_machine_dry_run_get_lr_push_count(void);
ST_FUNC uint32_t tcc_gen_machine_dry_run_get_scratch_regs_pushed(void);
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

/* Branch optimization interface */
ST_FUNC void tcc_gen_machine_branch_opt_init(void);
ST_FUNC void tcc_gen_machine_branch_opt_analyze(uint32_t *ir_to_code_mapping, int mapping_size);
ST_FUNC int tcc_gen_machine_branch_opt_get_encoding(int ir_index); /* Returns 16 or 32 */

/* Reset the MOV-coalescing register-equivalence cache at IR instruction
 * boundaries (any IR op may be a branch target, so cross-IR equivalences
 * cannot be trusted). */
ST_FUNC void tcc_gen_machine_mov_coalesce_reset(void);
ST_FUNC void tcc_gen_machine_mov_equiv_reset(void);
ST_FUNC void tcc_gen_machine_reserve_pool_bytes(int upcoming_bytes);
ST_FUNC void tcc_gen_machine_strldr_cache_reset(void);
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
ST_FUNC void tcc_gen_machine_builtin_apply_mop(MachineOperand fn, MachineOperand args, MachineOperand dest);

/* Block copy from const data to stack (LDM/STM on ARM) */
ST_FUNC void tcc_gen_machine_block_copy_mop(TCCIRState *ir, IROperand dest, IROperand src, int size);

/* Block copy between spill slots using LDM/STM (peephole for consecutive LOAD+STORE pairs) */
ST_FUNC void tcc_gen_machine_spill_block_copy(int32_t src_spill_off, int32_t dst_spill_off, int nwords);

/* Conditional select: dest = (cond) ? then_val : else_val (ITE on ARM) */
ST_FUNC void tcc_gen_machine_select_mop(MachineOperand then_val, MachineOperand else_val, MachineOperand dest,
                                        int cond_code);

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
#define tcc_warning TCC_SET_STATE(_tcc_warning)

#define total_idents TCC_STATE_VAR(total_idents)
#define total_lines TCC_STATE_VAR(total_lines)
#define total_bytes TCC_STATE_VAR(total_bytes)

PUB_FUNC void tcc_enter_state(TCCState *s1);
PUB_FUNC void tcc_exit_state(TCCState *s1);

/* conditional warning depending on switch */
#define tcc_warning_c(sw)                                                                                              \
  TCC_SET_STATE((tcc_state->warn_num = offsetof(TCCState, sw) - offsetof(TCCState, warn_none), _tcc_warning))

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
