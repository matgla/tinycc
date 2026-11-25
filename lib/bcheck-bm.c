/*
 *  bcheck-bm.c -- the `-b` bounds-checking runtime for bare-metal newlib.
 *
 *  Copyright (c) 2002 Fabrice Bellard (lib/bcheck.c, the hosted original)
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * What the armv8m cross's `-b` code calls (frontend/gen/op/bcheck.c), for
 * the QEMU newlib runs (tests/ir_tests, mps2-an505 + semihosting).  The
 * hosted lib/bcheck.c needs mmap, threads, signals and dlsym; here there is
 * one thread, newlib's allocator underneath, and a sorted array of regions
 * instead of the splay tree.  The board is out of scope: the regions and the
 * quarantine of freed blocks cost memory it does not have.
 *
 * Regions: every global (the .bounds table the linker ends and names
 * __bound_globals), every local array or struct (registered where it is
 * declared, dropped where its scope ends), VLA and alloca block, local
 * scalar whose address is taken, and every malloc'd block (one byte more is
 * allocated, so a one-past-the-end pointer never starts the next block),
 * and main's argv: each string and the array (__bound_main_arg).  A pointer
 * outside every region is not checked.
 *
 * The C library's memcpy, memmove, memset, memcmp, strlen, strcpy, strncpy,
 * strcmp, strncmp, strcat, strncat, strchr, strrchr and strdup are reached
 * through the __bound_ wrappers below (the compiler redirects the calls under
 * -b), which check what each reads and writes; __bound_longjmp drops the
 * regions of the frames it leaves (newlib's ARM jmp_buf: sp is word 8).
 *
 * Errors print one line in old bcheck's words on stderr, then exit(255):
 *   BCHECK: 0x... is outside of the region (0x...0x...)
 *   BCHECK: 0x... is outside of the region (0x...0x...): use after free
 *   BCHECK: freeing invalid region
 * A freed block stays registered, filled with 0x5a, in a quarantine of
 * BC_QUARANTINE blocks before newlib gets it back, so a use after free or a
 * double free of a recent block is caught.
 *
 * Built WITHOUT -b, with the program's own flags; tests/ir_tests/test_bcheck.py
 * links it in.  Pointer arithmetic that leaves its region returns
 * BC_INVALID_POINTER, as old bcheck did; dereferencing that (or anything near
 * it) reports the arithmetic that made it. */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <reent.h>

#define BC_INVALID_POINTER ((uintptr_t)-2)
#define BC_QUARANTINE 64

enum
{
  BC_GLOBAL,
  BC_LOCAL,
  BC_HEAP
};

typedef struct
{
  uintptr_t start;
  size_t size;
  unsigned char kind;
  unsigned char freed;
  unsigned char exact; /* a scalar's: its one-past-end address is not its own */
} Region;

static Region *regions;
static int nregions, cap_regions;
static void *quarantine[BC_QUARANTINE];
static int quarantine_next;
static int inited;

/* the arithmetic that last left its region */
static uintptr_t bad_ptr, bad_start, bad_end;

void *_malloc_r(struct _reent *, size_t);
void _free_r(struct _reent *, void *);
void *_realloc_r(struct _reent *, void *, size_t);

static void bc_error(const char *fmt, uintptr_t a, uintptr_t b, uintptr_t c, const char *tail, void *pc)
{
  fprintf(stderr, "BCHECK: ");
  fprintf(stderr, fmt, a, b, c);
  fprintf(stderr, "%s, at pc 0x%08lx\n", tail, (unsigned long)(uintptr_t)pc);
  fflush(stderr);
  exit(255);
}

static void bc_outside(uintptr_t p, const Region *r, const char *tail, void *pc)
{
  bc_error("0x%08lx is outside of the region (0x%08lx..0x%08lx)", p, r->start, r->start + r->size - 1, tail, pc);
}

/* the arithmetic that made an invalid pointer */
static void bc_invalid(const char *tail, void *pc);

/* the index of the last region starting at or below addr, or -1 */
static int bc_floor(uintptr_t addr)
{
  int lo = 0, hi = nregions - 1, found = -1;
  while (lo <= hi)
  {
    int mid = (lo + hi) / 2;
    if (regions[mid].start <= addr)
      found = mid, lo = mid + 1;
    else
      hi = mid - 1;
  }
  return found;
}

/* the region holding addr, its one-past-the-end address included, or NULL.
   A scalar's region (`&x`) leaves its one-past-the-end address out: the
   allocator packs scalars' homes, so that address is most often the next
   object's, which -b does not know (a parameter's home, a temporary). */
static Region *bc_find(uintptr_t addr)
{
  int i = bc_floor(addr);
  if (i < 0 || addr - regions[i].start > regions[i].size)
    return NULL;
  if (regions[i].exact && addr - regions[i].start == regions[i].size)
    return NULL;
  return &regions[i];
}

static void bc_remove(int i)
{
  memmove(&regions[i], &regions[i + 1], (nregions - i - 1) * sizeof(Region));
  nregions--;
}

static void bc_init(void);

static void bc_insert(uintptr_t start, size_t size, int kind, int exact)
{
  int i;
  if (!inited)
    bc_init();
  /* a new object where an old one was: the old one is dead (a local whose
     frame is gone, or a slot reused by another scope) */
  const uintptr_t end = start + (size ? size : 1);
  i = bc_floor(end - 1);
  while (i >= 0 && regions[i].start + (regions[i].size ? regions[i].size : 1) > start)
  {
    bc_remove(i);
    i--;
  }
  if (nregions == cap_regions)
  {
    cap_regions = cap_regions ? cap_regions * 2 : 256;
    regions = _realloc_r(_REENT, regions, cap_regions * sizeof(Region));
    if (!regions)
    {
      fprintf(stderr, "BCHECK: out of memory for regions\n");
      exit(255);
    }
  }
  i = bc_floor(start) + 1;
  memmove(&regions[i + 1], &regions[i], (nregions - i) * sizeof(Region));
  regions[i].start = start;
  regions[i].size = size;
  regions[i].kind = kind;
  regions[i].freed = 0;
  regions[i].exact = exact;
  nregions++;
}

extern size_t __bound_globals[] __attribute__((weak));

static void bc_init(void)
{
  inited = 1;
  if (__bound_globals)
    for (size_t *p = __bound_globals; p[0]; p += 2)
      bc_insert(p[0], p[1], BC_GLOBAL, 0);
}

__attribute__((constructor)) static void bc_constructor(void)
{
  if (!inited)
    bc_init();
}

/* p + offset for pointer arithmetic: inside p's region or one past it */
void *__bound_ptr_add(void *p, size_t offset)
{
  uintptr_t addr = (uintptr_t)p, res = addr + offset;
  Region *r = bc_find(addr);
  if (r && (r->freed || res - r->start > r->size))
  {
    bad_ptr = res, bad_start = r->start, bad_end = r->start + r->size - 1;
    return (void *)BC_INVALID_POINTER;
  }
  return (void *)res;
}

static void bc_invalid(const char *tail, void *pc)
{
  bc_error("0x%08lx is outside of the region (0x%08lx..0x%08lx)", bad_ptr, bad_start, bad_end, tail, pc);
}

static void *bc_indir(void *p, size_t offset, size_t size, void *pc)
{
  uintptr_t addr = (uintptr_t)p, res = addr + offset;
  Region *r;
  if (addr >= BC_INVALID_POINTER - 0x10000)
    bc_invalid("", pc);
  r = bc_find(addr);
  if (r)
  {
    if (r->freed)
      bc_outside(res, r, ": use after free", pc);
    if (res - r->start > r->size || res - r->start + size > r->size)
      bc_outside(res, r, "", pc);
  }
  return (void *)res;
}

#define BOUND_PTR_INDIR(n)                                                                                             \
  void *__bound_ptr_indir##n(void *p, size_t offset)                                                                   \
  {                                                                                                                    \
    return bc_indir(p, offset, n, __builtin_return_address(0));                                                       \
  }
BOUND_PTR_INDIR(1)
BOUND_PTR_INDIR(2)
BOUND_PTR_INDIR(4)
BOUND_PTR_INDIR(8)
BOUND_PTR_INDIR(12)
BOUND_PTR_INDIR(16)

/* SIZE bytes at P: an access of a size no __bound_ptr_indirN has (a struct
   copied whole), P already checked as pointer arithmetic */
void *__bound_check(void *p, size_t size)
{
  return bc_indir(p, 0, size, __builtin_return_address(0));
}

/* a local array, struct, VLA or alloca block */
void __bound_new_region(void *p, size_t size)
{
  bc_insert((uintptr_t)p, size, BC_LOCAL, 0);
}

/* a local scalar (or parameter) whose address is taken */
void __bound_new_scalar(void *p, size_t size)
{
  bc_insert((uintptr_t)p, size, BC_LOCAL, 1);
}

/* ... and where its scope ends (or its function's exits) */
void __bound_delete_region(void *p)
{
  int i = bc_floor((uintptr_t)p);
  if (i >= 0 && regions[i].start == (uintptr_t)p && regions[i].kind == BC_LOCAL)
    bc_remove(i);
}

/* main's entry (frontend/gen/op/bcheck.c bcheck_main_entry): each string of
   argv, and the array with its NULL, for the whole run */
void __bound_main_arg(int argc, char **argv)
{
  if (!argv || argc < 0)
    return;
  for (int i = 0; i < argc; i++)
    if (argv[i])
      bc_insert((uintptr_t)argv[i], strlen(argv[i]) + 1, BC_GLOBAL, 0);
  bc_insert((uintptr_t)argv, (argc + 1) * sizeof(char *), BC_GLOBAL, 0);
}

/* the malloc family, over newlib's reentrant allocator */
void *malloc(size_t size)
{
  void *p = _malloc_r(_REENT, size + 1);
  if (p)
    bc_insert((uintptr_t)p, size, BC_HEAP, 0);
  return p;
}

void free(void *ptr)
{
  int i;
  void *old;
  if (!ptr)
    return;
  i = bc_floor((uintptr_t)ptr);
  if (i < 0 || regions[i].kind != BC_HEAP || (uintptr_t)ptr - regions[i].start > regions[i].size)
  {
    _free_r(_REENT, ptr); /* not ours: newlib's own (strdup, stdio) */
    return;
  }
  if (regions[i].start != (uintptr_t)ptr || regions[i].freed)
    bc_error("freeing invalid region", 0, 0, 0, "", __builtin_return_address(0));
  regions[i].freed = 1;
  memset(ptr, 0x5a, regions[i].size);
  old = quarantine[quarantine_next];
  quarantine[quarantine_next] = ptr;
  quarantine_next = (quarantine_next + 1) % BC_QUARANTINE;
  if (old)
  {
    i = bc_floor((uintptr_t)old);
    if (i >= 0 && regions[i].start == (uintptr_t)old)
      bc_remove(i);
    _free_r(_REENT, old);
  }
}

void *calloc(size_t n, size_t size)
{
  size_t total = n * size;
  void *p;
  if (size && total / size != n)
    return NULL;
  p = malloc(total);
  if (p)
    memset(p, 0, total);
  return p;
}

void *realloc(void *ptr, size_t size)
{
  void *p;
  Region *r;
  if (!ptr)
    return malloc(size);
  r = bc_find((uintptr_t)ptr);
  if (!r || r->kind != BC_HEAP)
    return _realloc_r(_REENT, ptr, size); /* not ours */
  if (r->start != (uintptr_t)ptr || r->freed)
    bc_error("freeing invalid region", 0, 0, 0, "", __builtin_return_address(0));
  p = malloc(size);
  if (p)
  {
    r = bc_find((uintptr_t)ptr); /* malloc may have moved the array */
    memcpy(p, ptr, r->size < size ? r->size : size);
    free(ptr);
  }
  return p;
}

/* The C library's reads and writes.  TAIL names the function in the error. */

/* N bytes at P */
static void bc_range(const void *p, size_t n, const char *tail, void *pc)
{
  uintptr_t addr = (uintptr_t)p;
  Region *r;
  if (!n)
    return;
  if (addr >= BC_INVALID_POINTER - 0x10000)
    bc_invalid(tail, pc);
  r = bc_find(addr);
  if (!r)
    return;
  if (r->freed)
    bc_outside(addr, r, ": use after free", pc);
  if (addr - r->start + n > r->size)
    bc_outside(r->start + r->size, r, tail, pc);
}

/* the string at S, at most MAX bytes of it: its length, its terminator (if
   MAX lets it be read) inside S's region */
static size_t bc_strnlen(const char *s, size_t max, const char *tail, void *pc)
{
  uintptr_t addr = (uintptr_t)s;
  Region *r;
  size_t avail;
  const char *z;
  if (addr >= BC_INVALID_POINTER - 0x10000)
    bc_invalid(tail, pc);
  r = bc_find(addr);
  if (!r)
    return strnlen(s, max);
  if (r->freed)
    bc_outside(addr, r, ": use after free", pc);
  avail = r->size - (addr - r->start);
  z = memchr(s, 0, avail < max ? avail : max);
  if (z)
    return (size_t)(z - s);
  if (max > avail)
    bc_outside(r->start + r->size, r, tail, pc);
  return max;
}

#define BC_PC __builtin_return_address(0)
#define BC_STRLEN(s, tail) bc_strnlen((s), (size_t)-1, (tail), BC_PC)

void *__bound_memcpy(void *d, const void *s, size_t n)
{
  bc_range(d, n, " in memcpy", BC_PC);
  bc_range(s, n, " in memcpy", BC_PC);
  return memcpy(d, s, n);
}

void *__bound_memmove(void *d, const void *s, size_t n)
{
  bc_range(d, n, " in memmove", BC_PC);
  bc_range(s, n, " in memmove", BC_PC);
  return memmove(d, s, n);
}

void *__bound_memset(void *d, int c, size_t n)
{
  bc_range(d, n, " in memset", BC_PC);
  return memset(d, c, n);
}

int __bound_memcmp(const void *a, const void *b, size_t n)
{
  bc_range(a, n, " in memcmp", BC_PC);
  bc_range(b, n, " in memcmp", BC_PC);
  return memcmp(a, b, n);
}

size_t __bound_strlen(const char *s)
{
  return BC_STRLEN(s, " in strlen");
}

char *__bound_strcpy(char *d, const char *s)
{
  size_t n = BC_STRLEN(s, " in strcpy") + 1;
  bc_range(d, n, " in strcpy", BC_PC);
  return memcpy(d, s, n);
}

char *__bound_strncpy(char *d, const char *s, size_t n)
{
  bc_strnlen(s, n, " in strncpy", BC_PC);
  bc_range(d, n, " in strncpy", BC_PC);
  return strncpy(d, s, n);
}

int __bound_strcmp(const char *a, const char *b)
{
  BC_STRLEN(a, " in strcmp");
  BC_STRLEN(b, " in strcmp");
  return strcmp(a, b);
}

int __bound_strncmp(const char *a, const char *b, size_t n)
{
  bc_strnlen(a, n, " in strncmp", BC_PC);
  bc_strnlen(b, n, " in strncmp", BC_PC);
  return strncmp(a, b, n);
}

char *__bound_strcat(char *d, const char *s)
{
  size_t dl = BC_STRLEN(d, " in strcat"), sl = BC_STRLEN(s, " in strcat");
  bc_range(d, dl + sl + 1, " in strcat", BC_PC);
  return strcat(d, s);
}

char *__bound_strncat(char *d, const char *s, size_t n)
{
  size_t dl = BC_STRLEN(d, " in strncat"), sl = bc_strnlen(s, n, " in strncat", BC_PC);
  bc_range(d, dl + sl + 1, " in strncat", BC_PC);
  return strncat(d, s, n);
}

char *__bound_strchr(const char *s, int c)
{
  BC_STRLEN(s, " in strchr");
  return strchr(s, c);
}

char *__bound_strrchr(const char *s, int c)
{
  BC_STRLEN(s, " in strrchr");
  return strrchr(s, c);
}

char *__bound_strdup(const char *s)
{
  size_t n = BC_STRLEN(s, " in strdup") + 1;
  char *p = malloc(n); /* registered, unlike newlib's own */
  if (p)
    memcpy(p, s, n);
  return p;
}

/* the frames below the one setjmp saved are gone: their locals' regions too
   (the stack grows down; newlib's ARM jmp_buf keeps sp in word 8) */
void __bound_longjmp(jmp_buf env, int val)
{
  uintptr_t sp = ((uintptr_t *)env)[8];
  for (int i = bc_floor(sp - 1); i >= 0; i--)
    if (regions[i].kind == BC_LOCAL)
      bc_remove(i);
  longjmp(env, val);
}
