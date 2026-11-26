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

#include "tcc.h"
#include "tccld.h"
#include "source/opt/include/opt.h"

/********************************************************/
/* global variables */

/* XXX: get rid of this ASAP (or maybe not) */
ST_DATA struct TCCState *tcc_state;
TCC_SEM(static tcc_compile_sem);
/* an array of pointers to memory to be free'd after errors */
ST_DATA void **stk_data;
ST_DATA int nb_stk_data;

/********************************************************/

PUB_FUNC void tcc_enter_state(TCCState *s1)
{
  if (s1->error_set_jmp_enabled)
    return;
  WAIT_SEM(&tcc_compile_sem);
  tcc_state = s1;
}

PUB_FUNC void tcc_exit_state(TCCState *s1)
{
  if (s1->error_set_jmp_enabled)
    return;
  tcc_state = NULL;
  POST_SEM(&tcc_compile_sem);
}

/********************************************************/
/* copy a string and truncate it. */
ST_FUNC char *pstrcpy(char *buf, size_t buf_size, const char *s)
{
  char *q, *q_end;
  int c;

  if (buf_size > 0)
  {
    q = buf;
    q_end = buf + buf_size - 1;
    while (q < q_end)
    {
      c = *s++;
      if (c == '\0')
        break;
      *q++ = c;
    }
    *q = '\0';
  }
  return buf;
}

/* strcat and truncate. */
ST_FUNC char *pstrcat(char *buf, size_t buf_size, const char *s)
{
  size_t len;
  len = strlen(buf);
  if (len < buf_size)
    pstrcpy(buf + len, buf_size - len, s);
  return buf;
}

ST_FUNC char *pstrncpy(char *out, const char *in, size_t num)
{
  memcpy(out, in, num);
  out[num] = '\0';
  return out;
}

/* extract the basename of a file */
PUB_FUNC char *tcc_basename(const char *name)
{
  char *p = (char *)strchr(name, 0);
  while (p > name && !IS_DIRSEP(p[-1]))
    --p;
  return p;
}

/* extract extension part of a file
 *
 * (if no extension, return pointer to end-of-string)
 */
PUB_FUNC char *tcc_fileextension(const char *name)
{
  const char *b = tcc_basename(name);
  const char *e = strrchr(b, '.');
  return (char *)(e ? e : strchr(b, 0));
}

ST_FUNC char *tcc_load_text(int fd)
{
  int len = lseek(fd, 0, SEEK_END);
  char *buf = load_data(fd, 0, len + 1);
  if (buf)
    buf[len] = 0;
  return buf;
}

/********************************************************/
/* memory management */

/* we'll need the actual versions for a minute */
#undef free
#undef realloc

#if defined(TCC_ALLOC_PROFILE) && defined(TCC_IS_NATIVE) && defined(TARGETOS_YasOS)
static void alloc_trace(void *in, unsigned long size, void *out);
#elif defined(TCC_ALLOC_BACKTRACE) && !defined(TCC_IS_NATIVE)
/* TCC_ALLOC_BACKTRACE (host cross only, scripts/alloc_trace/alloc_trace.py):
 * TCC_ALLOCTRACE=FILE writes every malloc/realloc/free made through
 * default_reallocator as {in, size, out, stack} 32-bit words (in 0 = malloc,
 * size 0 = free, stack 0 for a free), and FILE.stacks each call stack the
 * first time it is seen, as {id, depth, pc[depth]}.  Built -m32 -malign-double
 * -mlong-double-64, the cross's structures have the device compiler's sizes,
 * so scripts/alloc_trace replays the trace against the device's malloc and names
 * whatever holds the memory at the peak by its callers, past the helpers
 * (dynarray_add, sym pools, section_realloc) that a file:line site stops at. */
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#define BT_DEPTH 24
static int bt_fd = -1, bt_stack_fd = -1, bt_tried;
static unsigned bt_buf[4 * 4096], bt_n;
static unsigned *bt_tab_hash, *bt_tab_id, bt_tab_cap, bt_tab_n;
static void **bt_pcs; /* bt_pcs[id * BT_DEPTH ...], id from 1 */
static unsigned char *bt_depth;

static void bt_flush(void)
{
  if (bt_fd >= 0 && bt_n)
    write(bt_fd, bt_buf, bt_n * sizeof(unsigned));
  bt_n = 0;
}

static void bt_close(void)
{
  bt_flush();
  if (bt_fd >= 0)
    close(bt_fd);
  if (bt_stack_fd >= 0)
    close(bt_stack_fd);
  bt_fd = bt_stack_fd = -1;
}

static unsigned bt_hash(void **pc, int n)
{
  unsigned h = 2166136261u;
  for (int i = 0; i < n; i++)
    h = (h ^ (unsigned)(unsigned long)pc[i]) * 16777619u;
  return h | 1;
}

static void bt_tab_put(unsigned h, unsigned id)
{
  unsigned m = bt_tab_cap - 1, s = h & m;
  while (bt_tab_hash[s])
    s = (s + 1) & m;
  bt_tab_hash[s] = h;
  bt_tab_id[s] = id;
}

/* The id of this call stack, written to FILE.stacks the first time. */
static unsigned bt_stack_id(void)
{
  void *pc[BT_DEPTH + 3];
  int n = backtrace(pc, BT_DEPTH + 3) - 3; /* drop bt_stack_id, alloc_trace, default_reallocator */
  if (n <= 0)
    return 0;
  void **p = pc + 3;
  unsigned h = bt_hash(p, n);
  if (2 * (bt_tab_n + 1) > bt_tab_cap)
  {
    unsigned *oh = bt_tab_hash, *oi = bt_tab_id, oc = bt_tab_cap;
    bt_tab_cap = oc ? oc * 2 : 1u << 16;
    bt_tab_hash = calloc(bt_tab_cap, sizeof(unsigned));
    bt_tab_id = calloc(bt_tab_cap, sizeof(unsigned));
    bt_pcs = realloc(bt_pcs, sizeof(void *) * BT_DEPTH * (bt_tab_cap / 2 + 1));
    bt_depth = realloc(bt_depth, bt_tab_cap / 2 + 1);
    for (unsigned i = 0; i < oc; i++)
      if (oh[i])
        bt_tab_put(oh[i], oi[i]);
    free(oh);
    free(oi);
  }
  unsigned m = bt_tab_cap - 1;
  for (unsigned s = h & m; bt_tab_hash[s]; s = (s + 1) & m)
  {
    unsigned id = bt_tab_id[s];
    if (bt_tab_hash[s] == h && bt_depth[id] == n && !memcmp(bt_pcs + id * BT_DEPTH, p, sizeof(void *) * n))
      return id;
  }
  unsigned id = ++bt_tab_n;
  memcpy(bt_pcs + id * BT_DEPTH, p, sizeof(void *) * n);
  bt_depth[id] = (unsigned char)n;
  bt_tab_put(h, id);
  unsigned rec[2 + BT_DEPTH];
  rec[0] = id;
  rec[1] = (unsigned)n;
  for (int i = 0; i < n; i++)
    rec[2 + i] = (unsigned)(unsigned long)p[i];
  write(bt_stack_fd, rec, sizeof(unsigned) * (2 + n));
  return id;
}

static void alloc_trace(void *in, unsigned long size, void *out)
{
  if (!bt_tried)
  {
    bt_tried = 1;
    const char *path = getenv("TCC_ALLOCTRACE");
    if (!path)
      return;
    char sp[1024];
    snprintf(sp, sizeof sp, "%s.stacks", path);
    void *warm[2];
    backtrace(warm, 2); /* loads the unwinder before the first record */
    bt_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    bt_stack_fd = open(sp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (bt_fd < 0 || bt_stack_fd < 0)
      bt_close();
    else
      atexit(bt_close);
  }
  if (bt_fd < 0)
    return;
  bt_buf[bt_n++] = (unsigned)(unsigned long)in;
  bt_buf[bt_n++] = (unsigned)size;
  bt_buf[bt_n++] = (unsigned)(unsigned long)out;
  bt_buf[bt_n++] = size ? bt_stack_id() : 0;
  if (bt_n == sizeof bt_buf / sizeof bt_buf[0])
    bt_flush();
}

/* Pass marks {0xffffffff, 5 begin / 6 end, id, 0}; TRACE.passes names each id
 * once, as "id name". */
PUB_FUNC void tcc_alloc_trace_pass(const char *name, int begin)
{
  static const char **names;
  static int nb_names;
  static FILE *names_file;
  if (bt_fd < 0 || !name)
    return;
  int id = 0;
  while (id < nb_names && names[id] != name && strcmp(names[id], name))
    id++;
  if (id == nb_names)
  {
    if (!names_file)
    {
      char p[1024];
      snprintf(p, sizeof p, "%s.passes", getenv("TCC_ALLOCTRACE"));
      names_file = fopen(p, "w");
    }
    names = realloc(names, sizeof(*names) * (nb_names + 1));
    names[nb_names++] = name;
    if (names_file)
      fprintf(names_file, "%d %s\n", id, name), fflush(names_file);
  }
  bt_buf[bt_n++] = 0xffffffffu;
  bt_buf[bt_n++] = begin ? 5 : 6;
  bt_buf[bt_n++] = (unsigned)id;
  bt_buf[bt_n++] = 0;
  if (bt_n == sizeof bt_buf / sizeof bt_buf[0])
    bt_flush();
}

/* A record {0xffffffff, what, 0, 0}: alloc_peak and the replays skip it. */
PUB_FUNC void tcc_alloc_trace_mark(int what)
{
  if (bt_fd < 0)
    return;
  bt_buf[bt_n++] = 0xffffffffu;
  bt_buf[bt_n++] = (unsigned)what;
  bt_buf[bt_n++] = 0;
  bt_buf[bt_n++] = 0;
  if (bt_n == sizeof bt_buf / sizeof bt_buf[0])
    bt_flush();
}
#else
#define alloc_trace(in, size, out) ((void)0)
#endif

static void *default_reallocator(void *ptr, unsigned long size)
{
  void *ptr1;
  if (size == 0)
  {
    free(ptr);
    ptr1 = NULL;
  }
  else
  {
    ptr1 = realloc(ptr, size);
    if (!ptr1)
    {
      fprintf(stderr, "memory full\n");
      exit(1);
    }
  }
  alloc_trace(ptr, size, ptr1);
  return ptr1;
}

ST_FUNC void libc_free(void *ptr)
{
  free(ptr);
}

#define free(p) use_tcc_free(p)
#define realloc(p, s) use_tcc_realloc(p, s)

/* global so that every tcc_alloc()/tcc_free() call doesn't need to be changed
 */
static void *(*reallocator)(void *, unsigned long) = default_reallocator;

LIBTCCAPI void tcc_set_realloc(TCCReallocFunc *realloc)
{
  reallocator = realloc ? realloc : default_reallocator;
}

/* in case MEM_DEBUG is #defined */
#undef tcc_free
#undef tcc_malloc
#undef tcc_realloc
#undef tcc_mallocz
#undef tcc_strdup

/* -----------------------------------------------------------------------------
 * Hot IR arrays in fast memory.
 *
 * On the board a data miss costs 84 cycles and on-chip SRAM cannot miss, but SRAM
 * is 336 KB against a compile's megabytes and goes to whoever asks first.  Most of
 * a compile's misses fall on a few arrays every pass walks, so this process keeps
 * its heap out of SRAM (MAP_SLOW) and places those arrays in an arena there,
 * taken the first time a function is compiled.  An array that does not fit stays
 * on the heap, so nothing depends on how much SRAM is free.
 *
 * TCC_ALLOC_PROFILE builds declare every allocation to the board model's allocprof
 * port (tools/board_model/rp2350_timing.c) for scripts/selfhost_profile.py, and
 * take the knobs TCC_TIER (0 first come, 1 arena, 2 arena + slow heap, the default,
 * 3 all slow), TCC_ARENA_KB, TCC_ALLOCPROF and TCC_POOLMAP.
 * -------------------------------------------------------------------------- */
#if TCC_HOT_ARENA
#include "memory/hot_arena.h"
#ifdef TCC_ALLOC_BACKTRACE
/* The tracing cross (TCC_ALLOC_BACKTRACE) keeps the device's hot arena, so
 * the hot arrays leave the traced heap as they do on the board; the arena
 * itself is one traced block, as the board's MAP_FAST mapping is one run. */
#define HOT_ARENA_MAP(len) tcc_malloc(len)
#define HOT_ARENA_UNMAP(p, len) tcc_free(p)
#else
#include <sys/mman.h>
extern int __malloc_map_flags;
#define HOT_ARENA_MAP(len) mmap(NULL, (len), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FAST, -1, 0)
#define HOT_ARENA_UNMAP(p, len) munmap((p), (len))
#endif

#define HOT_ARENA_MAX_BYTES (320u * 1024u)
#define HOT_ARENA_MIN_BYTES (16u * 1024u)
#define HOT_ARENA_STEP_BYTES (8u * 1024u)

static HotArena hot_arena;
static size_t hot_arena_mapped;
static char hot_arena_tried;

#ifdef TCC_ALLOC_PROFILE
#include <fcntl.h>
#include <unistd.h>
#define PROF_SITES 4000
static int hot_mode = 2;
static int prof_on;
static volatile unsigned prof_port;
static struct ProfSite
{
  const char *file;
  int line;
} *prof_sites;

static int prof_site(const char *file, int line)
{
  unsigned h = ((unsigned)(unsigned long)file * 31u + (unsigned)line) % PROF_SITES;
  while (prof_sites[h].file)
  {
    if (prof_sites[h].file == file && prof_sites[h].line == line)
      return (int)h;
    h = (h + 1) % PROF_SITES;
  }
  prof_sites[h].file = file;
  prof_sites[h].line = line;
  return (int)h;
}

/* TCC_LIVEPROF=1: what each allocation site holds at the compile's peak, on
 * the device itself (no board model needed, any image size).  A table keyed
 * by pointer (libc malloc, outside the counts) gives each block's site and
 * size back at its free; whenever the total live grows 32 KB past the last
 * snapshot, every site's live bytes are copied, so the snapshot taken last is
 * the peak to within 32 KB.  Printed at exit as LIVE lines. */
static int live_on;
typedef struct LiveEnt
{
  void *p;
  unsigned size;
  unsigned site;
} LiveEnt;
static LiveEnt *live_tab;
static unsigned live_cap, live_n;
static unsigned long live_site[PROF_SITES], live_snap[PROF_SITES];
static unsigned live_site_blocks[PROF_SITES], live_snap_blocks[PROF_SITES];
static unsigned long live_total, live_peak, live_snap_total;
/* live bytes and blocks by allocator class: <2 KB, <32 KB (pooled once the
 * class widened), >= 32 KB (a mapping of its own) */
static unsigned long live_cls[3], live_cls_n[3], live_cls_snap[3], live_cls_n_snap[3];
static int live_class(unsigned long size)
{
  return size < 2048 ? 0 : size < 32768 ? 1 : 2;
}
/* /proc/poolmap as it was at the last snapshot: whose mappings, and of what
 * kind, the process held at its peak (allocator overhead = mapped - live). */
static char live_poolmap[96 * 1024];
static long live_poolmap_len;

static char live_meminfo[2048];
static long live_meminfo_len;

static long live_read_file(const char *path, char *buf, long cap)
{
  int fd = open(path, O_RDONLY);
  long n = 0, r;
  if (fd < 0)
    return 0;
  while (n < cap - 1 && (r = read(fd, buf + n, cap - 1 - n)) > 0)
    n += r;
  close(fd);
  return n;
}

/* TCC_ALLOCTRACE=FILE: every malloc/realloc/free this process makes through
 * default_reallocator, as {in, size, out} 32-bit words (in 0 = malloc, size 0
 * = free), for replaying against libc's malloc.c on the host
 * (libs/libc/tests/malloc_replay.c). */
static int trace_fd = -1, trace_tried;
static unsigned trace_buf[3 * 4096], trace_n;
static unsigned long trace_events;

static void trace_flush(void)
{
  if (trace_fd >= 0 && trace_n)
    write(trace_fd, trace_buf, trace_n * sizeof(unsigned));
  trace_n = 0;
}

static void alloc_trace(void *in, unsigned long size, void *out)
{
  if (!trace_tried)
  {
    trace_tried = 1;
    const char *path = getenv("TCC_ALLOCTRACE");
    if (path)
      trace_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  }
  if (trace_fd < 0)
    return;
  trace_buf[trace_n++] = (unsigned)(unsigned long)in;
  trace_buf[trace_n++] = (unsigned)size;
  trace_buf[trace_n++] = (unsigned)(unsigned long)out;
  trace_events++;
  if (trace_n == sizeof trace_buf / sizeof trace_buf[0])
    trace_flush();
}

void __malloc_stats(long *out, int n);
static long live_mstats[19];

static void live_poolmap_capture(void)
{
  __malloc_stats(live_mstats, 19);
  /* poolmap stops at the kernel's 4 KB buffer; meminfo's MemProcessUsed is
   * the whole pool in use at this instant */
  live_poolmap_len = live_read_file("/proc/poolmap", live_poolmap, sizeof live_poolmap);
  live_meminfo_len = live_read_file("/proc/meminfo", live_meminfo, sizeof live_meminfo);
}

static unsigned live_hash(void *p)
{
  return (unsigned)(((unsigned long)p >> 3) * 2654435761u);
}

static void live_insert(LiveEnt e)
{
  unsigned m = live_cap - 1, h = live_hash(e.p) & m;
  while (live_tab[h].p)
    h = (h + 1) & m;
  live_tab[h] = e;
  live_n++;
}

static void live_grow(void)
{
  LiveEnt *old = live_tab;
  unsigned oc = live_cap;
  live_cap = oc ? oc * 2 : 1u << 16;
  live_tab = calloc(live_cap, sizeof *live_tab);
  live_n = 0;
  for (unsigned i = 0; i < oc; i++)
    if (old[i].p)
      live_insert(old[i]);
  libc_free(old);
}

static void live_add(void *p, unsigned long size, int site)
{
  if (!p)
    return;
  if (2 * (live_n + 1) > live_cap)
    live_grow();
  live_insert((LiveEnt){p, (unsigned)size, (unsigned)site});
  live_site[site] += size;
  live_site_blocks[site]++;
  live_cls[live_class(size)] += size;
  live_cls_n[live_class(size)]++;
  live_total += size;
  if (live_total > live_peak)
    live_peak = live_total;
  if (live_total > live_snap_total + 32768)
  {
    memcpy(live_snap, live_site, sizeof live_snap);
    memcpy(live_snap_blocks, live_site_blocks, sizeof live_snap_blocks);
    memcpy(live_cls_snap, live_cls, sizeof live_cls);
    memcpy(live_cls_n_snap, live_cls_n, sizeof live_cls_n);
    live_snap_total = live_total;
    live_poolmap_capture();
  }
}

static void live_del(void *p)
{
  if (!p || !live_cap)
    return;
  unsigned m = live_cap - 1, h = live_hash(p) & m;
  while (live_tab[h].p && live_tab[h].p != p)
    h = (h + 1) & m;
  if (!live_tab[h].p)
    return; /* allocated before tracking or outside it */
  LiveEnt e = live_tab[h];
  live_site[e.site] -= e.size;
  live_site_blocks[e.site]--;
  live_cls[live_class(e.size)] -= e.size;
  live_cls_n[live_class(e.size)]--;
  live_total -= e.size;
  live_tab[h].p = NULL;
  live_n--;
  /* backward-shift deletion keeps probe chains whole */
  for (unsigned j = (h + 1) & m; live_tab[j].p; j = (j + 1) & m)
  {
    LiveEnt x = live_tab[j];
    live_tab[j].p = NULL;
    live_n--;
    live_insert(x);
  }
}

static void prof_alloc(void *p, unsigned long size, const char *file, int line)
{
  if (!prof_on)
    return;
  int site = prof_site(file, line);
  if (live_on)
    live_add(p, size, site);
  prof_port = (1u << 28) | (unsigned)site;
  prof_port = (unsigned)(unsigned long)p;
  prof_port = (unsigned)size;
}

static void prof_free(void *p)
{
  if (!prof_on || !p)
    return;
  if (live_on)
    live_del(p);
  prof_port = 2u << 28;
  prof_port = (unsigned)(unsigned long)p;
}

static void poolmap_dump(const char *when)
{
  char buf[4096];
  int fd = open("/proc/poolmap", O_RDONLY);
  if (fd < 0)
    return;
  long n = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (n <= 0)
    return;
  buf[n] = 0;
  char *line = buf;
  while (*line)
  {
    char *e = (char *)strchr(line, '\n');
    if (e)
      *e = 0;
    printf("POOLMAP %s %s\n", when, line);
    if (!e)
      break;
    line = e + 1;
  }
}

static void prof_exit(void)
{
  if (trace_fd >= 0)
  {
    trace_flush();
    close(trace_fd);
    trace_fd = -1;
    printf("ALLOCTRACE events=%lu\n", trace_events);
  }
  if (prof_on)
  {
    prof_on = 0;
    if (live_on)
    {
      printf("LIVEPEAK peak=%lu snap=%lu end=%lu pid=%d\n", live_peak, live_snap_total, live_total, (int)getpid());
      for (int c = 0; c < 3; c++)
        printf("LIVECLASS %d bytes=%lu blocks=%lu\n", c, live_cls_snap[c], live_cls_n_snap[c]);
      printf("MSTATS pools=%ld pool_bytes=%ld free=%ld free_pages=%ld bump=%ld spare=%ld retired=%ld retired_bytes=%ld "
             "retired_free=%ld retired_free_pages=%ld parked=%ld\n",
             live_mstats[0], live_mstats[1], live_mstats[2], live_mstats[3], live_mstats[4], live_mstats[5], live_mstats[6],
             live_mstats[7], live_mstats[8], live_mstats[9], live_mstats[10]);
      printf("MFREEHIST <64=%ld <256=%ld <1K=%ld <2K=%ld <4K=%ld <8K=%ld <16K=%ld >=16K=%ld\n", live_mstats[11],
             live_mstats[12], live_mstats[13], live_mstats[14], live_mstats[15], live_mstats[16], live_mstats[17],
             live_mstats[18]);
      live_poolmap[live_poolmap_len] = 0;
      printf("POOLMAP_AT_PEAK_BEGIN\n%sPOOLMAP_AT_PEAK_END\n", live_poolmap);
      live_meminfo[live_meminfo_len] = 0;
      printf("MEMINFO_AT_PEAK_BEGIN\n%sMEMINFO_AT_PEAK_END\n", live_meminfo);
      for (int i = 0; i < PROF_SITES; i++)
        if (live_snap[i])
          printf("LIVE %lu %u %s %d\n", live_snap[i], live_snap_blocks[i], prof_sites[i].file, prof_sites[i].line);
    }
    else
      for (int i = 0; i < PROF_SITES; i++)
        if (prof_sites[i].file)
          printf("ALLOCSITE %d %s %d\n", i, prof_sites[i].file, prof_sites[i].line);
  }
  if (getenv("TCC_POOLMAP"))
    poolmap_dump("exit");
  printf("ARENA bytes=%lu peak=%lu misses=%u\n", (unsigned long)hot_arena_mapped, (unsigned long)hot_arena.peak,
         hot_arena.misses);
  fflush(stdout);
}
#else
#define prof_alloc(p, size, file, line) ((void)0)
#define prof_free(p) ((void)0)
#endif

PUB_FUNC void tcc_memory_tiers_setup(void)
{
  static char done;
  if (done)
    return;
  done = 1;
#ifdef TCC_ALLOC_PROFILE
  const char *e = getenv("TCC_TIER");
  if (e)
    hot_mode = atoi(e);
  if (hot_mode == 0 || hot_mode == 3)
    hot_arena_tried = 1;
  if (hot_mode >= 2)
    __malloc_map_flags = MAP_SLOW;
  if (getenv("TCC_POOLMAP"))
    poolmap_dump("start");
  if (getenv("TCC_ALLOCPROF") || getenv("TCC_LIVEPROF"))
  {
    prof_sites = tcc_mallocz(PROF_SITES * sizeof(*prof_sites));
    prof_port = 0xA110C8EDu;
    prof_on = 1;
    live_on = getenv("TCC_LIVEPROF") != NULL;
  }
  atexit(prof_exit);
#elif !defined(TCC_ALLOC_BACKTRACE)
  __malloc_map_flags = MAP_SLOW;
#endif
}

static void *hot_alloc(unsigned long size)
{
  if (!hot_arena_tried)
  {
    hot_arena_tried = 1;
    size_t want = HOT_ARENA_MAX_BYTES;
#ifdef TCC_ALLOC_PROFILE
    if (getenv("TCC_ARENA_KB"))
      want = (size_t)atoi(getenv("TCC_ARENA_KB")) * 1024u;
#endif
    for (; want >= HOT_ARENA_MIN_BYTES; want -= HOT_ARENA_STEP_BYTES)
    {
      void *m = HOT_ARENA_MAP(want);
      if (m != NULL && m != (void *)-1)
      {
        hot_arena_init(&hot_arena, m, want);
        hot_arena_mapped = want;
        break;
      }
    }
  }
  return hot_arena_alloc(&hot_arena, size);
}

PUB_FUNC void tcc_hot_release(void)
{
  if (hot_arena_mapped && !hot_arena.used)
  {
    HOT_ARENA_UNMAP(hot_arena.base, hot_arena_mapped);
    hot_arena.base = hot_arena.end = NULL;
    hot_arena_mapped = 0;
  }
  hot_arena_tried = 1;
}

PUB_FUNC void *tcc_hot_mallocz_at(unsigned long size, const char *file, int line)
{
  void *p = hot_alloc(size);
  if (!p)
    return tcc_mallocz_at(size, file, line);
  memset(p, 0, size);
  prof_alloc(p, size, file, line);
  return p;
}

PUB_FUNC void tcc_hot_free(void *ptr)
{
  if (!ptr)
    return;
  if (!hot_arena_contains(&hot_arena, ptr))
  {
    tcc_free(ptr);
    return;
  }
  prof_free(ptr);
  hot_arena_free(&hot_arena, ptr);
}

PUB_FUNC void *tcc_hot_realloc_at(void *ptr, unsigned long size, const char *file, int line)
{
  if (!ptr)
  {
    void *p = hot_alloc(size);
    if (p)
    {
      prof_alloc(p, size, file, line);
      return p;
    }
    return tcc_malloc_at(size, file, line);
  }
  /* A block that had to leave comes back only through tcc_hot_settle. */
  if (!hot_arena_contains(&hot_arena, ptr))
    return tcc_realloc_at(ptr, size, file, line);
  const size_t old = hot_arena_block_size(ptr);
  if (hot_arena_resize(&hot_arena, ptr, size))
  {
    prof_free(ptr);
    prof_alloc(ptr, size, file, line);
    return ptr;
  }
  void *n = hot_alloc(size);
  if (n)
    prof_alloc(n, size, file, line);
  else
    n = tcc_malloc_at(size, file, line);
  memcpy(n, ptr, old < size ? old : size);
  tcc_hot_free(ptr);
  return n;
}

PUB_FUNC void *tcc_hot_settle_at(void *ptr, unsigned long used, unsigned long cap, const char *file, int line)
{
  if (ptr && hot_arena_contains(&hot_arena, ptr) && hot_arena_resize(&hot_arena, ptr, cap))
  {
    prof_free(ptr);
    prof_alloc(ptr, cap, file, line);
    return ptr;
  }
  void *n = hot_alloc(cap);
  if (n)
    prof_alloc(n, cap, file, line);
  else if (ptr && hot_arena_contains(&hot_arena, ptr))
    n = tcc_malloc_at(cap, file, line);
  else
    return tcc_realloc_at(ptr, cap, file, line);
  if (used)
    memcpy(n, ptr, used);
  tcc_hot_free(ptr);
  return n;
}
#else
#define prof_alloc(p, size, file, line) ((void)0)
#define prof_free(p) ((void)0)
#endif

PUB_FUNC void tcc_free(void *ptr)
{
  prof_free(ptr);
  reallocator(ptr, 0);
}

#if TCC_RECORD_ALLOC_SITES
/* -bench instrumentation: allocations at or above the yasos libc MSETMAX
   (2 KiB) bypass the freelist and become raw mmap syscalls with kernel
   page-zeroing (~0.5 ms each on the RP2350) — count them so a compile's
   allocation profile is visible in the bench stats. */
static unsigned tcc_nb_big_allocs;
static unsigned long tcc_big_alloc_bytes;

/* Per-site totals for those allocations. Sites are identified by the file and
   line the macros in tcc.h capture; the table is fixed and small because the
   distinct sites that allocate >= 2 KiB number in the tens, and a site that
   does not fit is folded into the totals above (so the bytes still add up)
   rather than growing the table on the allocation path.  LOW_MEM keeps the
   dozen biggest: the table is .bss in every compiler process. */
#ifdef CONFIG_TCC_LOW_MEM
#define TCC_ALLOC_SITES 12
#else
#define TCC_ALLOC_SITES 48
#endif
typedef struct TCCAllocSite
{
  const char *file;
  int line;
  unsigned count;
  unsigned long bytes;
} TCCAllocSite;
static TCCAllocSite tcc_alloc_sites[TCC_ALLOC_SITES];
static int tcc_nb_alloc_sites;

static void tcc_record_alloc(unsigned long size, const char *file, int line)
{
  int i;
  if (size < 2048)
    return;
  tcc_nb_big_allocs++;
  tcc_big_alloc_bytes += size;
  for (i = 0; i < tcc_nb_alloc_sites; i++)
  {
    /* Pointer compare on file: every call site passes the same __FILE__
       literal, so this is an address match, not a string walk. */
    if (tcc_alloc_sites[i].line == line && tcc_alloc_sites[i].file == file)
    {
      tcc_alloc_sites[i].count++;
      tcc_alloc_sites[i].bytes += size;
      return;
    }
  }
  if (tcc_nb_alloc_sites >= TCC_ALLOC_SITES)
    return;
  i = tcc_nb_alloc_sites++;
  tcc_alloc_sites[i].file = file;
  tcc_alloc_sites[i].line = line;
  tcc_alloc_sites[i].count = 1;
  tcc_alloc_sites[i].bytes = size;
}
#else
#define tcc_record_alloc(size, file, line) ((void)0)
#endif

PUB_FUNC void *tcc_malloc_at(unsigned long size, const char *file, int line)
{
  tcc_record_alloc(size, file, line);
  void *r = reallocator(0, size);
  prof_alloc(r, size, file, line);
  return r;
}

PUB_FUNC void *tcc_realloc_at(void *ptr, unsigned long size, const char *file, int line)
{
  tcc_record_alloc(size, file, line);
  prof_free(ptr);
  void *r = reallocator(ptr, size);
  prof_alloc(r, size, file, line);
  return r;
}

PUB_FUNC void *tcc_mallocz_at(unsigned long size, const char *file, int line)
{
  void *ptr = tcc_malloc_at(size, file, line);
  if (size)
    memset(ptr, 0, size);
  return ptr;
}

PUB_FUNC void *tcc_malloc(unsigned long size)
{
  return tcc_malloc_at(size, "<unattributed>", 0);
}

PUB_FUNC void *tcc_realloc(void *ptr, unsigned long size)
{
  return tcc_realloc_at(ptr, size, "<unattributed>", 0);
}

PUB_FUNC void *tcc_mallocz(unsigned long size)
{
  void *ptr;
  ptr = tcc_malloc(size);
  /* Always zero. A prior optimization skipped this memset on yasos-native,
   * assuming malloc() always pre-zeroes — but that invariant is FALSE for bump
   * allocations served from a RECYCLED pool: mk_pool() resets the pool's bump
   * pointer (size = sizeof(*pool)) without re-zeroing the pool body, so those
   * bytes still hold stale data. Skipping the memset therefore handed tcc
   * non-zeroed memory and crashed self-host -O2 compiles (e.g. builtin-bitops-1:
   * free() of a -1 sentinel read from a struct field that should have been 0). */
  if (size)
    memset(ptr, 0, size);
  return ptr;
}

PUB_FUNC char *tcc_strdup(const char *str)
{
  char *ptr;
  ptr = tcc_malloc(strlen(str) + 1);
  strcpy(ptr, str);
  return ptr;
}

#ifdef MEM_DEBUG

#define MEM_DEBUG_MAGIC1 0xFEEDDEB1
#define MEM_DEBUG_MAGIC2 0xFEEDDEB2
#define MEM_DEBUG_MAGIC3 0xFEEDDEB3
#define MEM_DEBUG_FILE_LEN 40
#define MEM_DEBUG_CHECK3(header) ((mem_debug_header_t *)((char *)header + header->size))->magic3
#define MEM_USER_PTR(header) ((char *)header + offsetof(mem_debug_header_t, magic3))
#define MEM_HEADER_PTR(ptr) (mem_debug_header_t *)((char *)ptr - offsetof(mem_debug_header_t, magic3))

struct mem_debug_header
{
  unsigned magic1;
  unsigned size;
  struct mem_debug_header *prev;
  struct mem_debug_header *next;
  int line_num;
  char file_name[MEM_DEBUG_FILE_LEN + 1];
  unsigned magic2;
  ALIGNED(16) unsigned char magic3[4];
};

typedef struct mem_debug_header mem_debug_header_t;

TCC_SEM(static mem_sem);
static mem_debug_header_t *mem_debug_chain;
static unsigned mem_cur_size;
static unsigned mem_max_size;
static int nb_states;

static mem_debug_header_t *malloc_check(void *ptr, const char *msg)
{
  mem_debug_header_t *header = MEM_HEADER_PTR(ptr);
  if (header->magic1 != MEM_DEBUG_MAGIC1 || header->magic2 != MEM_DEBUG_MAGIC2 ||
      read32le(MEM_DEBUG_CHECK3(header)) != MEM_DEBUG_MAGIC3 || header->size == (unsigned)-1)
  {
    fprintf(stderr, "%s check failed\n", msg);
    if (header->magic1 == MEM_DEBUG_MAGIC1)
      fprintf(stderr, "%s:%u: block allocated here.\n", header->file_name, header->line_num);
    exit(1);
  }
  return header;
}

PUB_FUNC void *tcc_malloc_debug(unsigned long size, const char *file, int line)
{
  int ofs;
  mem_debug_header_t *header;
  if (!size)
    return NULL;
  header = tcc_malloc(sizeof(mem_debug_header_t) + size);
  header->magic1 = MEM_DEBUG_MAGIC1;
  header->magic2 = MEM_DEBUG_MAGIC2;
  header->size = size;
  write32le(MEM_DEBUG_CHECK3(header), MEM_DEBUG_MAGIC3);
  header->line_num = line;
  ofs = strlen(file) - MEM_DEBUG_FILE_LEN;
  strncpy(header->file_name, file + (ofs > 0 ? ofs : 0), MEM_DEBUG_FILE_LEN);
  header->file_name[MEM_DEBUG_FILE_LEN] = 0;
  WAIT_SEM(&mem_sem);
  header->next = mem_debug_chain;
  header->prev = NULL;
  if (header->next)
    header->next->prev = header;
  mem_debug_chain = header;
  mem_cur_size += size;
  if (mem_cur_size > mem_max_size)
    mem_max_size = mem_cur_size;
  POST_SEM(&mem_sem);
  return MEM_USER_PTR(header);
}

PUB_FUNC void tcc_free_debug(void *ptr)
{
  mem_debug_header_t *header;
  if (!ptr)
    return;
  header = malloc_check(ptr, "tcc_free");
  WAIT_SEM(&mem_sem);
  mem_cur_size -= header->size;
  header->size = (unsigned)-1;
  if (header->next)
    header->next->prev = header->prev;
  if (header->prev)
    header->prev->next = header->next;
  if (header == mem_debug_chain)
    mem_debug_chain = header->next;
  POST_SEM(&mem_sem);
  tcc_free(header);
}

PUB_FUNC void *tcc_mallocz_debug(unsigned long size, const char *file, int line)
{
  void *ptr;
  ptr = tcc_malloc_debug(size, file, line);
  if (size)
    memset(ptr, 0, size);
  return ptr;
}

PUB_FUNC void *tcc_realloc_debug(void *ptr, unsigned long size, const char *file, int line)
{
  mem_debug_header_t *header;
  int mem_debug_chain_update = 0;

  if (!ptr)
    return tcc_malloc_debug(size, file, line);
  if (!size)
  {
    tcc_free_debug(ptr);
    return NULL;
  }
  header = malloc_check(ptr, "tcc_realloc");
  WAIT_SEM(&mem_sem);
  mem_cur_size -= header->size;
  mem_debug_chain_update = (header == mem_debug_chain);
  header = tcc_realloc(header, sizeof(mem_debug_header_t) + size);
  header->size = size;
  write32le(MEM_DEBUG_CHECK3(header), MEM_DEBUG_MAGIC3);
  if (header->next)
    header->next->prev = header;
  if (header->prev)
    header->prev->next = header;
  if (mem_debug_chain_update)
    mem_debug_chain = header;
  mem_cur_size += size;
  if (mem_cur_size > mem_max_size)
    mem_max_size = mem_cur_size;
  POST_SEM(&mem_sem);
  return MEM_USER_PTR(header);
}

PUB_FUNC char *tcc_strdup_debug(const char *str, const char *file, int line)
{
  char *ptr;
  ptr = tcc_malloc_debug(strlen(str) + 1, file, line);
  strcpy(ptr, str);
  return ptr;
}

PUB_FUNC void tcc_memcheck(int d)
{
  WAIT_SEM(&mem_sem);
  nb_states += d;
  if (0 == nb_states && mem_cur_size)
  {
    mem_debug_header_t *header = mem_debug_chain;
    fflush(stdout);
    fprintf(stderr, "MEM_DEBUG: mem_leak= %d bytes, mem_max_size= %d bytes\n", mem_cur_size, mem_max_size);
    while (header)
    {
      fprintf(stderr, "%s:%u: error: %u bytes leaked\n", header->file_name, header->line_num, header->size);
      header = header->next;
    }
    fflush(stderr);
    mem_cur_size = 0;
    mem_max_size = 0;
    mem_debug_chain = NULL;
#if MEM_DEBUG - 0 == 2
    exit(2);
#endif
  }
  POST_SEM(&mem_sem);
}

/* restore the debug versions */
#define tcc_free(ptr) tcc_free_debug(ptr)
#define tcc_malloc(size) tcc_malloc_debug(size, __FILE__, __LINE__)
#define tcc_mallocz(size) tcc_mallocz_debug(size, __FILE__, __LINE__)
#define tcc_realloc(ptr, size) tcc_realloc_debug(ptr, size, __FILE__, __LINE__)
#define tcc_strdup(str) tcc_strdup_debug(str, __FILE__, __LINE__)

#else /* !MEM_DEBUG */

/* Re-arm the call-site attribution the #undef above had to switch off so the
   definitions in this file could be written. Without this every allocation
   made by libtcc.c -- sections, the state, file buffers -- lands in the
   profile as "<unattributed>", which was a third of a small compile's bytes. */
#if TCC_RECORD_ALLOC_SITES
#define tcc_malloc(size) tcc_malloc_at(size, __FILE__, __LINE__)
#define tcc_mallocz(size) tcc_mallocz_at(size, __FILE__, __LINE__)
#define tcc_realloc(ptr, size) tcc_realloc_at(ptr, size, __FILE__, __LINE__)
#endif

#endif /* MEM_DEBUG */

/* for #pragma once */
ST_FUNC int normalized_PATHCMP(const char *f1, const char *f2)
{
  char *p1, *p2;
  int ret = 1;
  if (!!(p1 = realpath(f1, NULL)))
  {
    if (!!(p2 = realpath(f2, NULL)))
    {
      ret = PATHCMP(p1, p2);
      libc_free(p2); /* realpath() requirement */
    }
    libc_free(p1);
  }
  return ret;
}

/********************************************************/
/* dynarrays */

ST_FUNC void dynarray_add(void *ptab, int *nb_ptr, void *data)
{
  int nb, nb_alloc;
  void **pp;

  nb = *nb_ptr;
  pp = *(void ***)ptab;
  /* every power of two we double array size */
  if ((nb & (nb - 1)) == 0)
  {
    if (!nb)
      nb_alloc = 1;
    else
      nb_alloc = nb * 2;
    pp = tcc_realloc(pp, nb_alloc * sizeof(void *));
    *(void ***)ptab = pp;
  }
  pp[nb++] = data;
  *nb_ptr = nb;
}

/* NAME, as one copy shared by every record that names its file: the saved
 * function bodies and static initializers each carried their own (thousands of
 * them in Zig's kernel.c, each a full path).  A compile reads a handful of
 * files, so a list does. */
ST_FUNC const char *tcc_filename_intern(const char *name)
{
  for (int i = 0; i < tcc_state->nb_interned_files; i++)
    if (!strcmp(tcc_state->interned_files[i], name))
      return tcc_state->interned_files[i];
  char *copy = tcc_strdup(name);
  dynarray_add(&tcc_state->interned_files, &tcc_state->nb_interned_files, copy);
  return copy;
}

/* The yasc dialect is a file's property: the main file under -std=yasc (and
   the safe default), and a file that says #pragma yasc.  Headers without it
   -- the system's above all -- are regular C, whoever includes them. */
ST_FUNC int tcc_yasc_file(TCCState *s1, const char *filename)
{
  const char *name = tcc_filename_intern(filename);
  for (int i = 0; i < s1->nb_yasc_files; i++)
    if (s1->yasc_files[i] == name)
      return 1;
  return 0;
}

ST_FUNC void tcc_yasc_mark_file(TCCState *s1, const char *filename)
{
  if (!tcc_yasc_file(s1, filename))
    dynarray_add(&s1->yasc_files, &s1->nb_yasc_files, (void *)tcc_filename_intern(filename));
}

/* A frontend warning about the code at the parse position that depends on
   the values (the constant checks: -Woverflow, -Warray-bounds, ...):
   tcc_warning_once, except that const_eval.c's speculative parse of a body
   with constant arguments says nothing, an error neither (its values are not
   the body's: -O0 has no such parse). */
static void diag_vwarn_once(int woff, int at_line, int value_dependent, const char *fmt, va_list ap);

ST_FUNC void tcc_fe_warning(int warn_offset, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  diag_vwarn_once(warn_offset, 0, 1, fmt, ap);
  va_end(ap);
}

ST_FUNC void dynarray_reset(void *pp, int *n)
{
  void **p;
  for (p = *(void ***)pp; *n; ++p, --*n)
    if (*p)
      tcc_free(*p);
  tcc_free(*(void **)pp);
  *(void **)pp = NULL;
}

static void tcc_split_path(TCCState *s, void *p_ary, int *p_nb_ary, const char *in)
{
  const char *p;
  do
  {
    int c;
    CString str;

    cstr_new(&str);
    for (p = in; c = *p, c != '\0' && c != PATHSEP[0]; ++p)
    {
      if (c == '{' && p[1] && p[2] == '}')
      {
        c = p[1], p += 2;
        if (c == 'B')
          cstr_cat(&str, s->tcc_lib_path, -1);
        if (c == 'R')
          cstr_cat(&str, CONFIG_SYSROOT, -1);
        if (c == 'f' && file)
        {
          /* substitute current file's dir */
          const char *f = file->true_filename;
          const char *b = tcc_basename(f);
          if (b > f)
            cstr_cat(&str, f, b - f - 1);
          else
            cstr_cat(&str, ".", 1);
        }
      }
      else
      {
        cstr_ccat(&str, c);
      }
    }
    if (str.size)
    {
      cstr_ccat(&str, '\0');
      dynarray_add(p_ary, p_nb_ary, tcc_strdup(str.data));
    }
    cstr_free(&str);
    in = p + 1;
  } while (*p);
}

/********************************************************/
/* warning / error */

/* warn_... option bits */
#define WARN_ON 1  /* warning is on (-Woption) */
#define WARN_ERR 2 /* warning is an error (-Werror=option) */
#define WARN_NOE 4 /* warning is not an error (-Wno-error=option) */

/* error1() modes */
enum
{
  ERROR_WARN,
  ERROR_NOABORT,
  ERROR_ERROR,
  ERROR_NOTE /* follows a diagnostic, shown only when that one was */
};

/* -fdiagnostics-color: gcc's escapes and colours (locus bold, error red,
   warning magenta, note cyan, 'quoted' names bold).  "auto" colours when
   stderr is a terminal, NO_COLOR is unset and TERM is not "dumb".  An unset
   TERM still colours: YasOS sets none, and its consoles are ANSI terminals.
   Output that goes to tcc_set_error_func() is never coloured. */
#define DIAG_SGR(c) "\033[" c "m\033[K"
#define DIAG_RESET DIAG_SGR("")
#define DIAG_LOCUS DIAG_SGR("01")
#define DIAG_QUOTE DIAG_SGR("01")
#define DIAG_ERROR DIAG_SGR("01;31")
#define DIAG_WARNING DIAG_SGR("01;35")
#define DIAG_NOTE DIAG_SGR("01;36")

/* stderr is a terminal a person reads: TERM is not "dumb" */
static int diag_on_terminal(void)
{
#ifdef _WIN32
  return 0;
#else
  const char *term = getenv("TERM");
  if (term && !strcmp(term, "dumb"))
    return 0;
  return isatty(2);
#endif
}

static int diag_color_on(TCCState *s1)
{
  const char *no_color;
  if (s1->error_func || s1->diag_color == DIAG_COLOR_NEVER)
    return 0;
  if (s1->diag_color == DIAG_COLOR_ALWAYS)
    return 1;
  no_color = getenv("NO_COLOR");
  if (no_color && no_color[0])
    return 0;
  return diag_on_terminal();
}

/* -fdiagnostics-show-caret / -show-option / -show-line-numbers (SHOW, a
   DIAG_SHOW_*): the source line under a diagnostic, the [-Wname] after it.
   Auto shows them where a person reads the text -- stderr a terminal, or
   -fdiagnostics-color=always -- so a pipe, a log, an editor's parser or a
   test reads the plain one-line format, byte for byte. */
static int diag_show(TCCState *s1, int show)
{
  if (show != DIAG_SHOW_AUTO)
    return show == DIAG_SHOW_ON;
  if (s1->error_func)
    return 0;
  return s1->diag_color == DIAG_COLOR_ALWAYS || diag_on_terminal();
}

/* -fdiagnostics-color[=always|never|auto], -fno-diagnostics-color, and
   clang's -f[no-]color-diagnostics; -f[no-]diagnostics-show-caret,
   -show-option, -show-line-numbers; -fdiagnostics-plain-output (no colour,
   no source line, no [-Wname]: what a pipe gets).  Returns 0 when OPT was
   one of them. */
static int set_diag_color(TCCState *s, const char *opt)
{
  static const struct
  {
    const char *name;
    size_t off; /* offsetof(TCCState, diag_show_x) */
  } shows[] = {{"diagnostics-show-caret", offsetof(TCCState, diag_show_caret)},
               {"diagnostics-show-option", offsetof(TCCState, diag_show_option)},
               {"diagnostics-show-line-numbers", offsetof(TCCState, diag_line_numbers)}};
  int no = !strncmp(opt, "no-", 3);

  for (size_t i = 0; i < sizeof shows / sizeof shows[0]; i++)
    if (!strcmp(opt + 3 * no, shows[i].name))
    {
      ((unsigned char *)s)[shows[i].off] = no ? DIAG_SHOW_OFF : DIAG_SHOW_ON;
      return 0;
    }
  if (!strcmp(opt, "diagnostics-color") || !strcmp(opt, "color-diagnostics") ||
      !strcmp(opt, "diagnostics-color=always"))
    s->diag_color = DIAG_COLOR_ALWAYS;
  else if (!strcmp(opt, "no-diagnostics-color") || !strcmp(opt, "no-color-diagnostics") ||
           !strcmp(opt, "diagnostics-color=never"))
    s->diag_color = DIAG_COLOR_NEVER;
  else if (!strcmp(opt, "diagnostics-color=auto"))
    s->diag_color = DIAG_COLOR_AUTO;
  else if (!strcmp(opt, "diagnostics-plain-output"))
  {
    s->diag_color = DIAG_COLOR_NEVER;
    s->diag_show_caret = s->diag_show_option = DIAG_SHOW_OFF;
  }
  else
    return -1;
  return 0;
}

/* ------------------------------------------------------------------------ */
/* #pragma GCC diagnostic push / pop / ignored / warning / error "-Wname"
   (also spelled `tcc` and `clang`).  The live options change at once, for
   what the frontend reports while parsing; each change is also recorded with
   its file and line, for what is reported later about an earlier line. */

#define DIAG_W_SIZE (offsetof(TCCState, warn_num) - offsetof(TCCState, warn_none))
typedef char diag_snap_fits[DIAG_W_SIZE <= DIAG_SNAP_SIZE ? 1 : -1];

static DiagSnap *diag_record_w(TCCState *s1, const char *file, int line, const unsigned char *w)
{
  DiagSnap *d;
  if (!(s1->nb_diag_snaps & (s1->nb_diag_snaps - 1)))
    s1->diag_snaps = tcc_realloc(s1->diag_snaps, (s1->nb_diag_snaps ? 2 * s1->nb_diag_snaps : 4) * sizeof(DiagSnap));
  d = &s1->diag_snaps[s1->nb_diag_snaps++];
  d->file = file ? tcc_filename_intern(file) : NULL;
  d->line = line;
  d->unsafe = 0;
  memmove(d->w, w, DIAG_W_SIZE);
  for (size_t i = 0; i < DIAG_W_SIZE; i++)
    s1->diag_snaps_or[i] |= w[i];
  return d;
}

static void diag_record(TCCState *s1, const char *file, int line)
{
  diag_record_w(s1, file, line, &s1->warn_none);
}

/* the options at LINE of file F: the last change recorded there at or
   before it, else the command line's; the live ones when nothing says */
static const unsigned char *diag_state_at_ex(TCCState *s1, const char *fname, int line, int skip_unsafe);
static void diag_w_strict_off(unsigned char *w);

static const unsigned char *diag_state_at(TCCState *s1, const char *fname, int line)
{
  return diag_state_at_ex(s1, fname, line, 0);
}

/* ... skip_unsafe: as if no unsafe region were recorded */
static const unsigned char *diag_state_at_ex(TCCState *s1, const char *fname, int line, int skip_unsafe)
{
  const char *name;
  if (!s1->nb_diag_snaps || !line || !fname)
    return &s1->warn_none;
  name = tcc_filename_intern(fname);
  /* the closest record at or before LINE (the latest of equals): unsafe
     regions are recorded when a body is parsed, which at -O1+ may come after
     the records of later lines */
  const DiagSnap *best = NULL, *edge = NULL;
  for (int i = s1->nb_diag_snaps; i-- > 0;)
  {
    const DiagSnap *d = &s1->diag_snaps[i];
    if (d->unsafe && d->file == name && d->line <= line && (!edge || d->line > edge->line))
      edge = d;
    if (skip_unsafe && d->unsafe)
      continue;
    if (d->file == name && d->line <= line && (!best || d->line > best->line))
      best = d;
    else if (!d->file && !best)
      return d->w;
  }
  if (!best)
    return &s1->warn_none;
  /* a #pragma diagnostic inside a region, recorded before the body's parse
     (-O1+) recorded the strict options on: the region keeps them off */
  if (!best->unsafe && !skip_unsafe && edge && edge->unsafe == DIAG_UNSAFE_OFF)
  {
    static unsigned char w[DIAG_SNAP_SIZE];
    memcpy(w, best->w, DIAG_W_SIZE);
    diag_w_strict_off(w);
    return w;
  }
  return best->w;
}

/* The warning WOFF (offsetof(TCCState, warn_x)) is shown at LINE of FNAME:
   on there as #pragma diagnostic left it, and not in a system header -- for
   a check that reports later, at the link, what it found in this unit
   (-Wtainted-format across units). */
ST_FUNC int tcc_warning_shown_at(TCCState *s1, int woff, const char *fname, int line)
{
  const unsigned char *w = diag_state_at(s1, fname, line);
  if (w[0] || !(w[woff - offsetof(TCCState, warn_none)] & WARN_ON))
    return 0;
  return s1->warn_system_headers || !fname || !tcc_sys_file(s1, fname, line);
}

/* ------------------------------------------------------------------------ */
/* Report once.  The frontend parses a body more than once: the -O1+ inliner
   replays it at every call site, a static is compiled again (late
   re-optimisation) or on its own after its calls were inlined, a body every
   call inlined gets a check-only parse (check_uncompiled_inline_functions),
   the speculative constant evaluation parses it with constant arguments.
   What the frontend says about a place is said once per unit: one set of
   hashes over (kind, option or key, file, line, text), reset with the unit
   (tcc_diag_reset).  It is only looked at when something is reported. */

static unsigned diag_once_hash(int ns, unsigned key, const char *file, int line, const char *text)
{
  unsigned h = 2166136261u ^ (unsigned)ns;
  h = (h ^ key) * 16777619u;
  h = (h ^ (unsigned)(uintptr_t)file) * 16777619u;
  h = (h ^ (unsigned)line) * 16777619u;
  if (text)
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
      h = (h ^ *p) * 16777619u;
  return h | 1; /* 0 marks a free slot */
}

static int diag_once_set(TCCState *s1, unsigned h, int insert);

ST_FUNC int tcc_diag_once(TCCState *s1, int ns, unsigned key, const char *file, int line, const char *text,
                          int insert)
{
  return diag_once_set(s1, diag_once_hash(ns, key, file, line, text), insert);
}

/* H (never 0) not in the set yet?  Then it is now, when INSERT. */
static int diag_once_set(TCCState *s1, unsigned h, int insert)
{
  unsigned mask;
  if (!s1->diag_once)
  {
    if (!insert)
      return 1;
  }
  if (insert && 2 * (s1->nb_diag_once + 1) > s1->diag_once_cap)
  {
    /* grow (or start) the open-addressed table and re-insert */
    int cap = s1->diag_once_cap ? 2 * s1->diag_once_cap : 64;
    unsigned *set = tcc_mallocz(cap * sizeof *set);
    for (int i = 0; i < s1->diag_once_cap; i++)
      if (s1->diag_once[i])
      {
        unsigned j = s1->diag_once[i] & (cap - 1);
        while (set[j])
          j = (j + 1) & (cap - 1);
        set[j] = s1->diag_once[i];
      }
    tcc_free(s1->diag_once);
    s1->diag_once = set;
    s1->diag_once_cap = cap;
  }
  mask = s1->diag_once_cap - 1;
  for (unsigned j = h & mask;; j = (j + 1) & mask)
  {
    if (s1->diag_once[j] == h)
      return 0;
    if (!s1->diag_once[j])
    {
      if (insert)
      {
        s1->diag_once[j] = h;
        s1->nb_diag_once++;
      }
      return 1;
    }
  }
}

/* The file a diagnostic at the parse position names, interned: the body's
   when an inline expansion replays one (diag_replay_file), else the file
   being read (past ':asm:' and ':paste:').  *PF: that BufferedFile (NULL
   outside a parse). */
ST_FUNC const char *tcc_diag_file(BufferedFile **pf)
{
  BufferedFile *f = file;
  while (f && f->filename[0] == ':' && f->prev)
    f = f->prev;
  if (pf)
    *pf = f;
  if (!f)
    return NULL;
  return tcc_filename_intern(tcc_state->diag_replay_file ? tcc_state->diag_replay_file : f->filename);
}

/* The frontend checks of statements (-Wimplicit-fallthrough, -Wswitch, ...):
 * KEY (DIAG_KEY) tells apart the warnings of LINE in the current file.
 * Returns 1 the first time. */
ST_FUNC int tcc_diag_first(TCCState *s1, int line, int key)
{
  return tcc_diag_once(s1, DIAG_ONCE_PLACE, (unsigned)key, tcc_diag_file(NULL), line <= 0 ? 1 : line, NULL, 1);
}

/* Whether tcc_diag_first(LINE, KEY) was said in this unit: also how the
 * preprocessor leaves marks for the parser (a fall-through comment). */
ST_FUNC int tcc_diag_marked(TCCState *s1, int line, int key)
{
  return !tcc_diag_once(s1, DIAG_ONCE_PLACE, (unsigned)key, tcc_diag_file(NULL), line <= 0 ? 1 : line, NULL, 0);
}

static void tcc_error_stash_free(TCCState *s1);

/* a new unit: the command line's options again */
ST_FUNC void tcc_diag_reset(TCCState *s1)
{
  tcc_free(s1->err_seen);
  s1->err_seen = NULL;
  s1->nb_err_seen = s1->err_seen_mask = 0;
  tcc_free(s1->undecl_toks);
  s1->undecl_toks = NULL;
  s1->nb_undecl_toks = s1->undecl_cap = 0;
  s1->undecl_func = NULL;
  if (s1->nb_diag_snaps)
    memcpy(&s1->warn_none, s1->diag_snaps[0].w, DIAG_W_SIZE);
  tcc_free(s1->diag_snaps);
  tcc_free(s1->diag_stack);
  s1->diag_snaps = s1->diag_stack = NULL;
  s1->nb_diag_snaps = s1->nb_diag_stack = s1->nb_diag_pragmas = 0;
  memset(s1->diag_snaps_or, 0, sizeof s1->diag_snaps_or);
  tcc_free(s1->diag_once);
  s1->diag_once = NULL;
  s1->nb_diag_once = s1->diag_once_cap = 0;
  tcc_free(s1->sys_files);
  s1->sys_files = NULL;
  s1->nb_sys_files = 0;
  s1->diag_replay_file = NULL; /* an error may have left an expansion's */
  tcc_error_stash_free(s1);
  s1->diag_parse = s1->diag_parse_next = 0;
  s1->diag_spec_recheck = 0;
}

/* A warning about FNAME at LINE (0: the parse position of F) is in a system
 * header: F found on a system include path or after its #pragma GCC
 * system_header, or a body replayed from one (tcc_sys_mark_file). */
static int diag_in_sys_header(TCCState *s1, BufferedFile *f, const char *fname, int line)
{
  if (fname == f->filename && f->sys)
    return 1;
  if (!s1->nb_sys_files)
    return 0;
  if (!line)
    line = f->line_num - ((tok_flags & TOK_FLAG_BOL) && !macro_ptr);
  return tcc_sys_file(s1, fname, line);
}

#include "diag_out.c"

static const char *warn_option_name(int wnum);

/* The next 'quoted' part of MSG at or after P: its closing quote, *OPEN at
   its opening one; NULL when there is none.  An apostrophe inside a word
   (the function's) neither opens nor closes one. */
static const char *diag_next_quote(const char *msg, const char *p, const char **open)
{
  for (; *p; p++)
  {
    const char *q;
    if (*p != '\'' || (p > msg && (isid(p[-1]) || isnum(p[-1]))))
      continue;
    for (q = p + 1; *q; q++)
      if (*q == '\'' && !isid(q[1]) && !isnum(q[1]))
        break;
    if (!*q)
      return NULL;
    if (q > p + 1)
    {
      *open = p;
      return q;
    }
    p = q; /* '' */
  }
  return NULL;
}

/* MSG onto CS, its 'quoted' names bold when COLOR (gcc's %qs) */
static void diag_cat_msg(CString *cs, const char *msg, int color)
{
  const char *p = msg, *open, *close;
  while (color && (close = diag_next_quote(msg, p, &open)))
  {
    if (open > p) /* cstr_cat's length 0 is the whole string with its NUL */
      cstr_cat(cs, p, open - p);
    cstr_cat(cs, DIAG_QUOTE, -1);
    cstr_cat(cs, open, close + 1 - open);
    cstr_cat(cs, DIAG_RESET, -1);
    p = close + 1;
  }
  if (*p)
    cstr_cat(cs, p, -1);
}

/* the display column after C at column COL: tabs to the next multiple of 8,
   a UTF-8 continuation byte takes none */
static int diag_col_next(int col, int c)
{
  if (c == '\t')
    return (col + 8) & ~7;
  return col + ((c & 0xc0) != 0x80);
}

/* The source line under a diagnostic, as gcc shows it:
       3 | int main(void) { return f(1) + undefined_var; }
         |                               ^~~~~~~~~~~~~
   The caret is under the first 'quoted' name of MSG that is on the line
   exactly once (tcc keeps no column); with none, the line alone.  A note
   on the line shown last shows it again only with a caret of its own.
   Out of line: error1 is reached from deep in the recursive parse. */
#if defined(__GNUC__) || defined(__TINYC__)
__attribute__((noinline))
#endif
static void diag_excerpt(TCCState *s1, CString *cs, int note, const char *fname, int line, const char *msg,
                         int color, const char *kind_color)
{
  static char last_file[256];
  static int last_line;
  CString text;
  const char *p, *open, *close;
  int col = 0, len = 0, w, c, x;

  cstr_new(&text);
  if (!fixit_line(fname, line, &text))
    goto out;
  for (p = msg; !col && (close = diag_next_quote(msg, p, &open)); p = close + 1)
  {
    char word[128];
    len = close - open - 1;
    if (len >= (int)sizeof word)
      continue;
    memcpy(word, open + 1, len);
    word[len] = 0;
    col = fixit_find_in(text.data, word);
  }
  if (note && !col && line == last_line && !strcmp(fname, last_file))
    goto out;
  pstrcpy(last_file, sizeof last_file, fname);
  last_line = line;

  /* the line, its trailing CR and blanks off */
  len = strlen(text.data);
  while (len && (text.data[len - 1] == '\r' || text.data[len - 1] == ' ' || text.data[len - 1] == '\t'))
    len--;
  w = s1->diag_line_numbers != DIAG_SHOW_OFF; /* with the line, as gcc */
  if (w)
    cstr_printf(cs, "\n %4d | ", line);
  else
    cstr_cat(cs, "\n ", -1);
  {
    /* the name highlighted in the kind's colour; tabs expanded */
    const int end = col ? col - 1 + (int)(close - open - 1) : 0;
    for (int i = 0, d = 0; i < len; i++)
    {
      c = (unsigned char)text.data[i];
      if (color && col && i == col - 1)
        cstr_cat(cs, kind_color, -1);
      if (c == '\t')
        for (x = diag_col_next(d, c); d < x; d++)
          cstr_ccat(cs, ' ');
      else
      {
        cstr_ccat(cs, c);
        d = diag_col_next(d, c);
      }
      if (color && col && i == end - 1)
        cstr_cat(cs, DIAG_RESET, -1);
    }
    if (!col)
      goto out;
    /* the caret line: ^ at the name's first column, ~ under the rest */
    cstr_cat(cs, w ? "\n      | " : "\n ", -1);
    {
      int from = 0, to = 0;
      for (int i = 0; i < end && text.data[i]; i++)
      {
        if (i == col - 1)
          from = to;
        to = diag_col_next(to, (unsigned char)text.data[i]);
      }
      for (x = 0; x < from; x++)
        cstr_ccat(cs, ' ');
      if (color)
        cstr_cat(cs, kind_color, -1);
      cstr_ccat(cs, '^');
      for (x = from + 1; x < to; x++)
        cstr_ccat(cs, '~');
      if (color)
        cstr_cat(cs, DIAG_RESET, -1);
    }
  }
out:
  cstr_free(&text);
}

/* a finished line of text output: stderr, or tcc_set_error_func()'s */
static void diag_text_out(TCCState *s1, const char *line)
{
  if (dq_on(s1))
  {
    dq_add(s1, DQ_TEXT)->a = tcc_strdup(line);
    return;
  }
  if (!s1->error_func)
  {
    /* default case: stderr */
    if (s1 && s1->output_type == TCC_OUTPUT_PREPROCESS && s1->ppfp == stdout)
      printf("\n"); /* print a newline during tcc -E */
    fflush(stdout); /* flush -v output */
    fprintf(stderr, "%s\n", line);
    fflush(stderr); /* print error/warning now (win32) */
  }
  else
  {
    s1->error_func(s1->error_opaque, line);
  }
}

/* tag: printed after "error: " -- keeps the internal-compiler-error prefix
 * in one place instead of in every message literal */
/* An error's way out: to the recovery point (core/recover.c) when it is not
   FATAL, else out of the compile. */
static NORETURN void error_longjmp(TCCState *s1, int fatal)
{
  s1->error_fatal = 0;
  s1->max_errors_defer = s1->max_errors_hit = 0;
  if (s1->recover && !fatal && !s1->fe_speculative)
    tcc_recover_jump(s1); /* returns when this error cannot be recovered from */
  if (!s1->fe_speculative)
    tcc_recover_release(s1);
  while (nb_stk_data)
    tcc_free(*(void **)stk_data[--nb_stk_data]);
  s1->diag_noabort = 0;
  longjmp(s1->error_jmp_buf, 1);
}

static int diag_is_strict(TCCState *s1, int wnum);

static void error1(int mode, const char *tag, const char *fmt, va_list ap)
{
  BufferedFile **pf, *f;
  TCCState *s1 = tcc_state;
  CString cs, msg;
  int line = 0, color, any_header = s1->diag_any_header, wnum = 0;
  const char *kind, *kind_color, *fname;

  tcc_exit_state(s1);
  s1->diag_any_header = 0;
  /* -fmax-errors was reached where the stop waits (tcc_max_errors_defer):
     nothing more is said until it comes */
  if (s1->max_errors_hit && mode != ERROR_ERROR)
    return;

  if (mode == ERROR_NOTE)
  {
    if (!s1->diag_last_shown)
      return;
  }
  else
    s1->diag_last_shown = 0;

  if (fmt[0] == '%' && fmt[1] == 'i' && fmt[2] == ':')
    line = va_arg(ap, int), fmt += 3;
  f = NULL;
  if (s1->error_set_jmp_enabled)
  { /* we're called while parsing a file */
    /* use upper file if inline ":asm:" or token ":paste:" */
    for (f = file; f && f->filename[0] == ':'; f = f->prev)
      ;
  }

  /* an inline expansion replays the body's lines: they are its file's, not
     the caller's; the link names a unit's place itself (diag_link_file) */
  fname = !f ? (line ? s1->diag_link_file : NULL) : s1->diag_replay_file ? s1->diag_replay_file : f->filename;
  if (mode == ERROR_WARN && s1->diag_flow_only && !line)
    return;
  /* As gcc: no warning in a system header -- whatever -Werror or the safe
     default make of it; an error is always given -- unless -Wsystem-headers.
     #warning is given there too.  The one place for every warning (the
     report-once layer too); the frontend checks that skip their work there
     ask tcc_in_sys_header() first. */
  if (mode == ERROR_WARN && f && !any_header && !s1->warn_system_headers && diag_in_sys_header(s1, f, fname, line))
  {
    s1->warn_num = 0;
    return;
  }
  /* nor in the expansion of a macro a system header defined (tccpp.c
     tcc_sys_macro_here): a warning about the position read now -- one given
     its line is about another place, which decided for itself */
  if (mode == ERROR_WARN && f && !line && !any_header && !s1->warn_system_headers && tcc_sys_macro_prev())
  {
    s1->warn_num = 0;
    return;
  }
  if (mode == ERROR_WARN)
  {
    /* the options as they were at the diagnostic's line (#pragma diagnostic) */
    const unsigned char *w = diag_state_at(s1, fname, line);
    if (w[offsetof(TCCState, warn_error) - offsetof(TCCState, warn_none)])
      mode = ERROR_ERROR;
    if (s1->warn_num)
    {
      /* handle tcc_warning_c(warn_option)(fmt, ...) */
      int wopt = w[s1->warn_num];
      wnum = s1->warn_num;
      s1->warn_num = 0;
      if (0 == (wopt & WARN_ON))
        return;
      if (wopt & WARN_ERR)
        mode = ERROR_ERROR;
      if (wopt & WARN_NOE)
        mode = ERROR_WARN;
    }
    /* warn_none (-w); the strict lifetime errors are errors still */
    if (w[0] && !(mode == ERROR_ERROR && diag_is_strict(s1, wnum)))
      return;
    /* A warning made an error leaves the parser where it was: the compile
       goes on and reports what comes after it (gcc), only fails. */
    if (mode == ERROR_ERROR && (s1->diag_noabort || (s1->error_set_jmp_enabled && !s1->fe_speculative)))
      mode = ERROR_NOABORT;
  }

  if (f && 0 == line)
    line = f->line_num - ((tok_flags & TOK_FLAG_BOL) && !macro_ptr);
  /* the message alone (the JSON / SARIF output takes it without the locus) */
  cstr_new(&msg);
  if (tag)
    cstr_cat(&msg, tag, -1);
  if (pp_expr > 1)
    pp_error(&msg); /* special handler for preprocessor expression errors */
  else
    cstr_vprintf(&msg, fmt, ap);
  cstr_ccat(&msg, 0);
  /* the lexer's error (it cannot read past it: error_fatal) or a
     preprocessor directive's (a missing include) in a body being saved, or
     read just after it: said when the parse of the body reaches it, after
     the errors before it (decl.c save_body_cut) */
  if (mode == ERROR_ERROR && s1->save_body_jmp && f && !tag && !s1->fe_speculative && !s1->asm_inline &&
      (s1->error_fatal || pp_expr || (parse_flags & PARSE_FLAG_LINEFEED)))
  {
    s1->error_fatal = 0;
    char *m = tcc_strdup((char *)msg.data);
    cstr_free(&msg);
    save_body_error(s1, line, m);
  }
  color = diag_color_on(s1);
  cstr_new(&cs);
  int locus = 0; /* where the message starts, after the include chain */
  if (f)
  {
    /* a note belongs to the diagnostic above it: no include chain again */
    if (mode != ERROR_NOTE)
      for (pf = s1->include_stack; pf < s1->include_stack_ptr; pf++)
        cstr_printf(&cs, "In file included from %s%s:%d%s:\n", color ? DIAG_LOCUS : "", (*pf)->filename,
                    (*pf)->line_num - 1, color ? DIAG_RESET : "");
    locus = cs.size;
    cstr_printf(&cs, "%s%s:%d:%s ", color ? DIAG_LOCUS : "", fname, line, color ? DIAG_RESET : "");
  }
  else if (fname)
    cstr_printf(&cs, "%s%s:%d:%s ", color ? DIAG_LOCUS : "", fname, line, color ? DIAG_RESET : "");
  else
  {
    cstr_printf(&cs, "%s%s:%s ", color ? DIAG_LOCUS : "", s1->current_filename ? s1->current_filename : "tcc",
                color ? DIAG_RESET : "");
  }
  if (mode == ERROR_WARN)
    kind = "warning:", kind_color = DIAG_WARNING;
  else if (mode == ERROR_NOTE)
    kind = "note:", kind_color = DIAG_NOTE;
  else
    kind = "error:", kind_color = DIAG_ERROR;
  cstr_printf(&cs, "%s%s%s ", color ? kind_color : "", kind, color ? DIAG_RESET : "");
  diag_cat_msg(&cs, (char *)msg.data, color);
  /* [-Wname] after a warning, [-Werror=name] after one made an error */
  if (wnum && warn_option_name(wnum) && diag_show(s1, s1->diag_show_option))
    cstr_printf(&cs, " %s[-W%s%s]%s", color ? kind_color : "", mode == ERROR_WARN ? "" : "error=",
                warn_option_name(wnum), color ? DIAG_RESET : "");
  /* An error a replay says again (an inline expansion, then the function's
     own compile) is reported and counted once. */
  if ((mode == ERROR_ERROR || mode == ERROR_NOABORT) && s1->error_set_jmp_enabled && !s1->fe_speculative &&
      tcc_error_seen(s1, (char *)cs.data + locus, cs.size - locus))
  {
    cstr_free(&cs);
    cstr_free(&msg);
    s1->diag_last_shown = 0;
    dout.next_note = NULL;
    if (mode == ERROR_ERROR)
      goto raise;
    return;
  }
  if (mode != ERROR_NOTE)
    diag_out_place(fname, line);
  dq.head_next = mode == ERROR_NOTE ? 0 : mode == ERROR_WARN ? 2 : 1; /* held: where a diagnostic starts */
  if (diag_out_on(s1))
    diag_out_result(mode == ERROR_WARN ? 0 : mode == ERROR_NOTE ? 2 : 1, wnum ? warn_option_name(wnum) : NULL,
                    fname ? fname : s1->current_filename, fname ? line : 0, (char *)msg.data);
  else
  {
    if (fname && line > 0 && diag_show(s1, s1->diag_show_caret))
      diag_excerpt(s1, &cs, mode == ERROR_NOTE, fname, line, (char *)msg.data, color, kind_color);
    cstr_ccat(&cs, 0);
    diag_text_out(s1, cs.data);
  }
  cstr_free(&cs);
  cstr_free(&msg);
  s1->diag_last_shown = 1;
  if (mode == ERROR_NOTE)
    return;
  diag_out_next(); /* the note and fix-it tcc_fixit_next() left for it */
  if (mode == ERROR_NOABORT || mode == ERROR_ERROR)
  {
    s1->nb_errors++;
    /* -fmax-errors=N: the Nth error ends the compile */
    if (s1->error_set_jmp_enabled && !s1->fe_speculative && s1->max_errors > 0 &&
        s1->nb_errors >= s1->max_errors)
    {
      if (!dq_on(s1))
      {
        diag_terminated(s1);
        if (s1->max_errors_defer && mode != ERROR_ERROR)
        {
          /* the caller holds what a longjmp would leak: it stops the
             compile itself (tcc_max_errors_stop) */
          s1->max_errors_hit = 1;
          return;
        }
        s1->error_fatal = 1;
        mode = ERROR_ERROR;
      }
      else if (s1->diag_in_parse && !s1->diag_stop && s1->recover)
      {
        /* held (diag_out.c): the errors are counted in -O0's order when
           written -- the parse stops, the bodies saved before here still
           speak; at the end of the unit the parse goes on */
        if (s1->max_errors_defer && mode != ERROR_ERROR)
        {
          s1->max_errors_hit = 2; /* tcc_max_errors_stop stops it */
          return;
        }
        s1->diag_stop = 2;
        mode = ERROR_ERROR;
        tcc_recover_stop(s1);
        s1->error_fatal = 1;
      }
    }
  }
raise:
  if (mode == ERROR_ERROR && s1->error_set_jmp_enabled)
  {
    /* go on after it when the state allows (core/recover.c): not inside a
       preprocessor directive, a macro's substitution or an assembler
       file, nor after an internal error */
    const int fatal = s1->error_fatal || tag || pp_expr || pp_subst_depth ||
                      ((parse_flags & (PARSE_FLAG_LINEFEED | PARSE_FLAG_ASM_FILE)) && !s1->asm_inline);
    /* held diagnostics (diag_out.c): a fatal error in the parse stops it,
       but the bodies saved before it still speak, as they did at -O0 */
    if (fatal && !tag && dq_on(s1) && s1->recover && !s1->fe_speculative && s1->diag_in_parse && !s1->diag_stop)
    {
      s1->error_fatal = 0;
      s1->diag_stop = 1;
      s1->diag_stop_anchor = s1->diag_anchor;
      s1->diag_stop_seq = dq.seq - 1;
      tcc_recover_stop(s1);
    }
    error_longjmp(s1, fatal);
  }
}

/* -fmax-errors in a section whose allocations an error's longjmp would leak
   (tcc_ir_flow_checks): the Nth error says it stops, the ones after it are
   not said, and tcc_max_errors_stop ends the compile once the section has
   freed what it holds. */
ST_FUNC void tcc_max_errors_defer(TCCState *s1)
{
  s1->max_errors_defer = 1;
}

ST_FUNC void tcc_max_errors_stop(TCCState *s1)
{
  const int hit = s1->max_errors_hit;
  s1->max_errors_defer = s1->max_errors_hit = 0;
  if (!hit || !s1->error_set_jmp_enabled)
    return;
  if (hit == 2)
  {
    /* held diagnostics: the parse stops as error1 would have stopped it */
    s1->diag_stop = 2;
    tcc_recover_stop(s1);
  }
  error_longjmp(s1, 1);
}

/* "compilation terminated due to -fmax-errors=N." */
static void diag_terminated(TCCState *s1)
{
  char stop[80];
  snprintf(stop, sizeof stop, "compilation terminated due to -fmax-errors=%d.", s1->max_errors);
  if (diag_out_on(s1))
    diag_out_result(2, NULL, NULL, 0, stop);
  else if (!s1->error_func)
  {
    fprintf(stderr, "%s\n", stop);
    fflush(stderr);
  }
  else
    s1->error_func(s1->error_opaque, stop);
}

/* The error message MSG (from its locus on) was printed in this unit already;
   else remember it. */
ST_FUNC int tcc_error_seen(TCCState *s1, const char *msg, int len)
{
  unsigned h = 2166136261u, i;
  for (int k = 0; k < len; k++)
    h = (h ^ (unsigned char)msg[k]) * 16777619u;
  h |= 1; /* 0 is an empty slot */
  if (2 * (s1->nb_err_seen + 1) > s1->err_seen_mask)
  {
    unsigned *old = s1->err_seen;
    int n = old ? s1->err_seen_mask + 1 : 0, m = n ? 2 * n : 32;
    s1->err_seen = tcc_mallocz(m * sizeof *old);
    s1->err_seen_mask = m - 1;
    for (int k = 0; k < n; k++)
      if (old[k])
      {
        for (i = old[k] & s1->err_seen_mask; s1->err_seen[i]; i = (i + 1) & s1->err_seen_mask)
          ;
        s1->err_seen[i] = old[k];
      }
    tcc_free(old);
  }
  for (i = h & s1->err_seen_mask; s1->err_seen[i]; i = (i + 1) & s1->err_seen_mask)
    if (s1->err_seen[i] == h)
      return 1;
  s1->err_seen[i] = h;
  s1->nb_err_seen++;
  return 0;
}

static void tcc_error_stash_free(TCCState *s1)
{
  for (int i = 0; i < s1->nb_diag_stash; i++)
    tcc_free(s1->diag_stash[i].msg);
  tcc_free(s1->diag_stash);
  s1->diag_stash = NULL;
  s1->nb_diag_stash = s1->diag_stash_cap = 0;
}

LIBTCCAPI void tcc_set_error_func(TCCState *s, void *error_opaque, TCCErrorFunc *error_func)
{
  s->error_opaque = error_opaque;
  s->error_func = error_func;
}

/* error without aborting current compilation */
PUB_FUNC int _tcc_error_noabort(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  error1(ERROR_NOABORT, NULL, fmt, ap);
  va_end(ap);
  return -1;
}

#undef _tcc_error
PUB_FUNC void _tcc_error(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  error1(ERROR_ERROR, NULL, fmt, ap);
  exit(1);
}
#define _tcc_error use_tcc_error_noabort

/* internal compiler error: a compiler invariant broke (not a user mistake).
 * Same level as tcc_error (aborts the compile), tagged "compiler_error: ". */
PUB_FUNC void _tcc_ice(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  error1(ERROR_ERROR, "compiler_error: ", fmt, ap);
  exit(1);
}

PUB_FUNC void _tcc_ice_stop(void)
{
  _tcc_ice("internal error");
}

PUB_FUNC void _tcc_warning(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  error1(ERROR_WARN, NULL, fmt, ap);
  va_end(ap);
}

/* FILENAME is a system header from LINE on (0: all of it) */
ST_FUNC void tcc_sys_mark_file(TCCState *s1, const char *filename, int line)
{
  const char *name = tcc_filename_intern(filename);
  for (int i = 0; i < s1->nb_sys_files; i++)
    if (s1->sys_files[i].name == name)
    {
      if (line < s1->sys_files[i].line)
        s1->sys_files[i].line = line;
      return;
    }
  if (!(s1->nb_sys_files & (s1->nb_sys_files - 1)))
    s1->sys_files = tcc_realloc(s1->sys_files, (s1->nb_sys_files ? 2 * s1->nb_sys_files : 4) * sizeof *s1->sys_files);
  s1->sys_files[s1->nb_sys_files].name = name;
  s1->sys_files[s1->nb_sys_files++].line = line;
}

/* FILENAME is a system header at LINE (tcc_sys_mark_file) */
ST_FUNC int tcc_sys_file(TCCState *s1, const char *filename, int line)
{
  const char *name;
  if (!s1->nb_sys_files)
    return 0;
  name = tcc_filename_intern(filename);
  for (int i = 0; i < s1->nb_sys_files; i++)
    if (s1->sys_files[i].name == name)
      return line >= s1->sys_files[i].line;
  return 0;
}

/* LINE of the diagnostic file NAME (tcc_diag_file; F the BufferedFile being
   read) is in a system header: F was found on a system include path or said
   #pragma GCC system_header -- unless F only replays a body of another file
   (an inline expansion), whose own registry entry then decides */
static int diag_sys_at(TCCState *s1, BufferedFile *f, const char *name, int line)
{
  if (f->sys && !s1->diag_replay_file)
    return 1;
  if (!s1->nb_sys_files)
    return 0;
  for (int i = 0; i < s1->nb_sys_files; i++)
    if (s1->sys_files[i].name == name)
      return line >= s1->sys_files[i].line;
  return 0;
}

/* the reporting position is in a system header (BufferedFile.sys: found on
   a system include path, or after #pragma GCC system_header) -- also in a
   body from one replayed later: an inline expansion, a deferred or an
   inline function generated at the end of the unit.  For the diagnostics,
   which are quiet there: never so under -Wsystem-headers. */
ST_FUNC int tcc_in_sys_header(void)
{
  TCCState *s1 = tcc_state;
  BufferedFile *f;
  const char *name;
  if (s1->warn_system_headers)
    return 0; /* -Wsystem-headers: speak there as anywhere */
  if (!file)
    return 0;
  if (!s1->diag_replay_file && !s1->nb_sys_files)
  {
    /* the common case, without interning the name */
    for (f = file; f->filename[0] == ':' && f->prev; f = f->prev)
      ;
    return f->sys;
  }
  name = tcc_diag_file(&f);
  return diag_sys_at(s1, f, name, f->line_num - ((tok_flags & TOK_FLAG_BOL) && !macro_ptr));
}

/* tcc_warning_once(warn_x, ...): see tcc.h.  WOFF is the option's
   offsetof(TCCState, warn_x) (warn_none: no option); AT_LINE the line of the
   current file it is about (0: the parse position).

   Once per (option, file, line, message) in the unit, at every -O.  Quiet in
   a system header (error1), also in a body of one an inline expansion
   replays.  Said
   with its line ("%i:"), so #pragma diagnostic applies as it stood at that
   line (a body generated after its file was read: deferred, or an inline
   function at the end of the unit), and the check-only parse of a body every
   call inlined (diag_flow_only) keeps it -- the set drops it there when an
   expansion said it already.  Nothing is recorded (or costs more than the
   test of the option) while the option is off and no #pragma diagnostic can
   turn it on at another line. */
static void diag_vwarn_once(int woff, int at_line, int value_dependent, const char *fmt, va_list ap)
{
  TCCState *s1 = tcc_state;
  BufferedFile *f;
  const char *name;
  char buf[512];
  int line, wrel = woff - (int)offsetof(TCCState, warn_none);

  s1->diag_last_shown = 0; /* a note after this one goes with it (tcc_note) */
  if (!file || (s1->warn_none && !diag_is_strict(s1, wrel)) || (value_dependent && s1->fe_speculative))
    return;
  if (wrel && !s1->diag_snaps_or[wrel] && !(((unsigned char *)s1)[woff] & WARN_ON))
    return;
  name = tcc_diag_file(&f);
  if (!f)
    return;
  /* the position read now is in a system header's macro (error1's rule) */
  if (!at_line && !s1->warn_system_headers && tcc_sys_macro_prev())
    return;
  line = at_line ? at_line : f->line_num - ((tok_flags & TOK_FLAG_BOL) && !macro_ptr);
  /* quiet in a system header: error1 decides, as for every warning
     (-Wsystem-headers) */
  vsnprintf(buf, sizeof buf, fmt, ap);
  /* const_eval.c's speculative parse with constant arguments: error1 hands
     it to a function that drops it (as an error it ends the speculation, and
     the body is expanded and says it), so it does not count as said */
  if (s1->fe_speculative)
    s1->diag_spec_dropped = 1;
  else
  {
    /* Two of a line with one text (`f(0) + g(0)`) are two warnings, as gcc
       tells them apart by column: the k-th of this parse of a body
       (diag_parse) is the k-th of every other parse of it. */
    unsigned h = diag_once_hash(DIAG_ONCE_WARN, (unsigned)woff, name, line, buf), k = 0;
    while (!diag_once_set(s1, ((h ^ s1->diag_parse * 2654435761u) * 16777619u ^ k) * 16777619u | 1, 1))
      k++;
    if (!diag_once_set(s1, (h ^ k * 2246822519u) * 16777619u | 1, 1))
      return;
  }
  s1->warn_num = wrel;
  _tcc_warning("%i:%s", line, buf);
}

ST_FUNC void tcc_vwarning_once(int woff, int at_line, const char *fmt, va_list ap)
{
  diag_vwarn_once(woff, at_line, 0, fmt, ap);
}

PUB_FUNC void _tcc_warning_once(int woff, int at_line, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  tcc_vwarning_once(woff, at_line, fmt, ap);
  va_end(ap);
}

/* "note: ..." after a warning or error, about the same problem (where a
   value came from, the other end of a conflict).  Dropped when the
   diagnostic before it was not shown.  "%i:" in front of the format gives
   the line, as for the others. */
PUB_FUNC void _tcc_note(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  error1(ERROR_NOTE, NULL, fmt, ap);
  va_end(ap);
}

/********************************************************/
/* I/O layer */

ST_FUNC void tcc_open_bf(TCCState *s1, const char *filename, int initlen)
{
  BufferedFile *bf;
  int buflen = initlen ? initlen : IO_BUF_SIZE;

  /* Only the header is zeroed.  The buffer body is a read buffer: the fd path
     fills it from read(), every initlen != 0 caller memcpy's its own bytes in,
     and the sentinel below is planted explicitly — so zeroing 8 KB per file
     open bought nothing.  The header still needs it: line_ref, ifndef_macro,
     ifndef_macro_saved, include_next_index and unget are not assigned here. */
  bf = tcc_malloc(sizeof(BufferedFile) + buflen);
  memset(bf, 0, sizeof(BufferedFile));
  bf->buf_ptr = bf->buffer;
  bf->buf_end = bf->buffer + initlen;
  bf->buf_end[0] = CH_EOB; /* put eob symbol */
  pstrcpy(bf->filename, sizeof(bf->filename), filename);
  bf->true_filename = bf->filename;
  bf->line_num = 1;
  bf->ifdef_stack_ptr = s1->ifdef_stack_ptr;
  bf->fd = -1;
  bf->prev = file;
  bf->prev_tok_flags = tok_flags;
  /* the dialect: an internal buffer (":asm:", ":paste:", ...) is part of the
     file it came from; the first file is the main one */
  if (filename[0] == ':')
    bf->yasc = file ? file->yasc : 0;
  else if (!file && s1->yasc_std)
  {
    bf->yasc = 1;
    tcc_yasc_mark_file(s1, filename);
  }
  else
    bf->yasc = s1->nb_yasc_files && tcc_yasc_file(s1, filename);
  if (!file && filename[0] != ':')
    s1->main_file = tcc_filename_intern(filename);
  s1->yasc = bf->yasc;
  file = bf;
  tok_flags = TOK_FLAG_BOL | TOK_FLAG_BOF;
}

ST_FUNC void tcc_close(void)
{
  TCCState *s1 = tcc_state;
  BufferedFile *bf = file;
  /* closing the synthetic predefines buffer marks the end of the per-compile
     macro-table build — the startup phase the init stamps exist to split out */
  if (bf->filename[0] == '<' && 0 == strcmp(bf->filename, "<command line>"))
    tcc_init_stamp("predef macros");
  if (bf->fd > 0)
  {
    close(bf->fd);
    total_lines += bf->line_num - 1;
  }
  if (bf->true_filename != bf->filename)
    tcc_free(bf->true_filename);
  file = bf->prev;
  tok_flags = bf->prev_tok_flags;
  if (file)
    s1->yasc = file->yasc;
  tcc_free(bf);
}

static int _tcc_open(TCCState *s1, const char *filename)
{
  int fd;
  if (strcmp(filename, "-") == 0)
    fd = 0, filename = "<stdin>";
  else
    fd = open(filename, O_RDONLY | O_BINARY);
  if ((s1->verbose == 2 && fd >= 0) || s1->verbose == 3)
    printf("%s %*s%s\n", fd < 0 ? "nf" : "->", (int)(s1->include_stack_ptr - s1->include_stack), "", filename);
  return fd;
}

ST_FUNC int tcc_open(TCCState *s1, const char *filename)
{
  int fd = _tcc_open(s1, filename);
  if (fd < 0)
    return -1;
  tcc_open_bf(s1, filename, 0);
  file->fd = fd;
  return 0;
}

/* compile the file opened in 'file'. Return non zero if errors. */
static void tcc_safe_defaults(TCCState *s);
static void tcc_implied_warnings(TCCState *s);

static int tcc_compile(TCCState *s1, int filetype, const char *str, int fd)
{
  unsigned compile_start = 0;
  unsigned phase_start = 0;

  /* Here we enter the code section where we use the global variables for
     parsing and code generation (tccpp.c, tccgen.c, <target>-gen.c).
     Other threads need to wait until we're done.

     Alternatively we could use thread local storage for those global
     variables, which may or may not have advantages */

  if (s1->do_bench)
  {
    compile_start = tcc_getclock_us();
    phase_start = compile_start;
  }

  tcc_safe_defaults(s1);
  tcc_implied_warnings(s1);
  tcc_enter_state(s1);
  s1->error_set_jmp_enabled = 1;

  if (setjmp(s1->error_jmp_buf) == 0)
  {
    s1->nb_errors = 0;

    if (fd == -1)
    {
      int len = strlen(str);
      tcc_open_bf(s1, "<string>", len);
      memcpy(file->buffer, str, len);
    }
    else
    {
      tcc_open_bf(s1, str, 0);
      file->fd = fd;
    }

    preprocess_start(s1, filetype);
    tcc_init_stamp("preprocess setup");
    tccgen_init(s1);
    tcc_init_stamp("tccgen init");

    if (s1->output_type != TCC_OUTPUT_PREPROCESS)
      tccelf_begin_file(s1);
    tcc_init_stamp("elf begin");

    if (s1->do_bench)
    {
      unsigned elapsed = tcc_getclock_us() - phase_start;
      s1->bench_compile_setup_time += elapsed;
      s1->bench_compile_setup_count++;
      tcc_bench_log(s1, "compile-setup", str, elapsed);
      phase_start = tcc_getclock_us();
    }

    if (s1->output_type == TCC_OUTPUT_PREPROCESS)
    {
      if (s1->module_deps)
        yasc_module_scan(s1);
      else
        tcc_preprocess(s1);
    }
    else
    {
      if (filetype & (AFF_TYPE_ASM | AFF_TYPE_ASMPP))
      {
        tcc_assemble(s1, !!(filetype & AFF_TYPE_ASMPP));
      }
      else
      {
        tccgen_compile(s1);
      }
    }
    tcc_init_stamp("parse+codegen");

    if (s1->do_bench)
    {
      unsigned elapsed = tcc_getclock_us() - phase_start;
      s1->bench_compile_exec_time += elapsed;
      s1->bench_compile_exec_count++;
      tcc_bench_log(s1, "compile-exec", str, elapsed);
      phase_start = tcc_getclock_us();
    }

    if (s1->output_type != TCC_OUTPUT_PREPROCESS)
    {
      /* -fcallgraph-info: the unit's address-taken functions, by its own symbols */
      if (s1->callgraph_info)
        tcc_stack_usage_unit_end(s1);
      tccelf_end_file(s1);
    }
    tcc_init_stamp("elf end");
  }
  tcc_diag_queue_flush(s1); /* the unit's held diagnostics, in order */
  tccgen_finish(s1);
  preprocess_end(s1);
  s1->error_set_jmp_enabled = 0;
  s1->recover = NULL;
  s1->error_fatal = 0;
  s1->asm_inline = 0;
  s1->diag_in_parse = 0;
  if (s1->recover_flipped)
  {
    /* an error turned the rest of the unit into -fsyntax-only */
    s1->syntax_only = s1->recover_syntax_only;
    s1->recover_flipped = 0;
  }
  tcc_exit_state(s1);
  if (s1->do_bench)
  {
    unsigned now = tcc_getclock_us();
    unsigned finalize_elapsed = now - phase_start;
    unsigned elapsed = now - compile_start;
    s1->bench_compile_finalize_time += finalize_elapsed;
    s1->bench_compile_finalize_count++;
    tcc_bench_log(s1, "compile-finalize", str, finalize_elapsed);
    s1->bench_compile_time += elapsed;
    s1->bench_compile_count++;
    tcc_bench_log(s1, filetype & (AFF_TYPE_ASM | AFF_TYPE_ASMPP) ? "assemble" : "compile", str, elapsed);
  }
  return s1->nb_errors != 0 ? -1 : 0;
}

LIBTCCAPI int tcc_compile_string(TCCState *s, const char *str)
{
  return tcc_compile(s, s->filetype, str, -1);
}

/* define a preprocessor symbol. value can be NULL, sym can be "sym=val" */
LIBTCCAPI void tcc_define_symbol(TCCState *s1, const char *sym, const char *value)
{
  const char *eq;
  if (NULL == (eq = strchr(sym, '=')))
    eq = strchr(sym, 0);
  if (NULL == value)
    value = *eq ? eq + 1 : "1";
  cstr_printf(&s1->cmdline_defs, "#define %.*s %s\n", (int)(eq - sym), sym, value);
}

/* undefine a preprocessor symbol */
LIBTCCAPI void tcc_undefine_symbol(TCCState *s1, const char *sym)
{
  cstr_printf(&s1->cmdline_defs, "#undef %s\n", sym);
}

LIBTCCAPI TCCState *tcc_new(void)
{
  TCCState *s;

  s = tcc_mallocz(sizeof(TCCState));
#ifdef MEM_DEBUG
  tcc_memcheck(1);
#endif

#undef gnu_ext
  s->gnu_ext = 1;
  s->tcc_ext = 1;
  s->nocommon = 1;
  s->fortify = 2;
  s->dollars_in_identifiers = 1; /*on by default like in gcc/clang*/
  s->cversion = 201112;          /* default to C11 */
  s->warn_implicit_function_declaration = 1;
  s->warn_discarded_qualifiers = 1;
  s->warn_int_conversion = 1;
  s->warn_incompatible_pointer_types = 1;
  s->warn_return_type = 1;
  s->warn_implicit_int = 1;
  s->warn_implicit_move = 1; /* yasc only: nothing else has owned pointers */
  s->warn_unused_result = 1;
  s->warn_deprecated_declarations = 1;
  s->ms_extensions = 1;
  s->max_errors = 8; /* -fmax-errors=8: the first errors, then stop */
  s->unwind_tables = 1;
  s->internal_spill = 1;
  /* vreg 0 is a real vreg, so "no vreg bound" has to be -1, not the zeroed
   * default. */
  s->inline_return_vr = -1;

#ifdef CHAR_IS_UNSIGNED
  s->char_is_unsigned = 1;
#endif
  s->pic = 0;
  s->no_pie = 0;
#if defined(TCC_TARGET_ARM) || defined(TCC_TARGET_ARM_THUMB)
  s->float_abi = ARM_SOFTFP_FLOAT;
  /* CONFIG_TCC_DEFAULT_FPU names an arm_fpu_type enumerator (not a string), so
   * a build can ship a compiler whose default -mfpu is the FPU its target
   * actually has -- e.g. -DCONFIG_TCC_DEFAULT_FPU=ARM_FPU_RP2350 for a build
   * whose whole userland runs on RP2350.  It is only a default: an explicit
   * -mfpu= on the command line still wins, and the float ABI is untouched
   * (softfp either way), so objects stay link-compatible across the switch.
   *
   * Note this changes what the *installed* compiler does with no flags at all,
   * which is the point -- it reaches Makefiles that never learned to pass
   * -mfpu, and on-device compiles too.  Anything that must stay soft-float
   * (e.g. a test suite running on a part without the unit) has to say
   * -mfpu=none rather than rely on the default. */
#ifdef CONFIG_TCC_DEFAULT_FPU
  s->fpu_type = CONFIG_TCC_DEFAULT_FPU;
#else
  s->fpu_type = ARM_FPU_AUTO; /* default to auto-detect */
#endif
  s->fp_inline = ARM_FP_INLINE_AUTO;
  /* CONFIG_TCC_DEFAULT_FP_LIB names an arm_fp_lib enumerator, so a build whose
   * rootfs keeps one shared copy of the __aeabi_ runtime can ship a compiler
   * that binds it that way with no flags at all -- the same reach argument as
   * CONFIG_TCC_DEFAULT_FPU above, and for the same reason: half the Makefiles
   * in this tree never learned to pass a link flag. */
#ifdef CONFIG_TCC_DEFAULT_FP_LIB
  s->fp_lib = CONFIG_TCC_DEFAULT_FP_LIB;
#else
  s->fp_lib = ARM_FP_LIB_AUTO;
#endif
#if defined(TCC_TARGET_YASOS)
  s->text_and_data_separation = 1;
  s->pic = 1;
  s->section_align = 4;
  s->text_addr = 0;
  s->has_text_addr = 1;
  /* RELRO rodata sharing is on by default for YASOS: .rodata is kept pure-const
   * (pointer-bearing const objects go to the writable data segment) and shared
   * XIP across processes. Disable with -no-share-rodata. */
  s->share_rodata = 1;
  /* SB-relative GOT addressing is on by default for YASOS; disable per build
   * with -mno-sb-relative-got when a module's GOT exceeds 4096 bytes. */
  s->sb_relative_got = 1;
  /* Calls to undefined functions that no library in the library paths exports
   * keep R9 (no reload after the call); -fno-module-local-calls restores the
   * reload after every call out of the translation unit. */
#ifdef CONFIG_TCC_LOW_MEM
  /* The r9-reload elision reads every library's export table into the heap
   * (tens of KB for libc.so) to decide which callees bind locally; a low-memory
   * compiler reloads r9 after every external call instead, which makes code
   * about 5% bigger.  -fmodule-local-calls still turns it on. */
  s->module_local_calls = 0;
#else
  s->module_local_calls = 1;
#endif
#else
  s->text_and_data_separation = 0;
#endif
#endif
#ifdef CONFIG_NEW_DTAGS
  s->enable_new_dtags = 1;
#endif
  s->ppfp = stdout;
  /* might be used in error() before preprocess_start() */
  s->include_stack_ptr = s->include_stack;

  tcc_set_lib_path(s, CONFIG_TCCDIR);
  return s;
}

LIBTCCAPI void tcc_delete(TCCState *s1)
{
  diag_out_state_end();
  tcc_typesig_free(s1);
  tcc_typesig_reset();
  tcc_taint_link_free(s1);
  tcc_stack_usage_free(s1);
  /* free target-specific backend state */
#if defined(TCC_TARGET_ARM) || defined(TCC_TARGET_ARM_THUMB)
  arm_deinit(s1);
#endif

  /* free IR-level interprocedural caches */
  tcc_ir_free_switch_func_cache(s1);
  tcc_free(s1->func_purity_cache);
  tcc_free(s1->sys_globals);
  tcc_free(s1->func_const_result_cache);
  tcc_free(s1->split_pads);  /* an error before the object is written */
  tcc_free(s1->text_pieces);

  /* free lazy object files (Phase 2 GC) */
  tcc_free_lazy_objfiles(s1);

  /* free the -fmodule-local-calls import set (its libtcc1 index lives in the
     archive cache freed next) */
  tcc_yaff_import_set_free(s1);

  /* free cached archive symbol tables */
  tcc_archive_cache_free(s1);

  /* free sections */
  tccelf_delete(s1);

  /* free library paths */
  dynarray_reset(&s1->library_paths, &s1->nb_library_paths);
  dynarray_reset(&s1->crt_paths, &s1->nb_crt_paths);

  /* free include paths */
  dynarray_reset(&s1->include_paths, &s1->nb_include_paths);
  dynarray_reset(&s1->sysinclude_paths, &s1->nb_sysinclude_paths);
  tcc_free(s1->yasc_include_paths); /* its entries are include_paths' */
  s1->yasc_include_paths = NULL;
  s1->nb_yasc_include_paths = 0;
  dynarray_reset(&s1->inc_dir_lists, &s1->nb_inc_dir_lists);
  tcc_free(s1->yasc_module);
  dynarray_reset(&s1->yasc_includes, &s1->nb_yasc_includes);
  dynarray_reset(&s1->module_dirs, &s1->nb_module_dirs);
  tcc_free(s1->module_header_dir);

  tcc_free(s1->tcc_lib_path);
  tcc_free(s1->soname);
  tcc_free(s1->rpath);
  tcc_free(s1->elf_entryname);
  tcc_free(s1->init_symbol);
  tcc_free(s1->fini_symbol);
  tcc_free(s1->mapfile);
  tcc_free(s1->outfile);
  tcc_free(s1->deps_outfile);
  tcc_free(s1->linker_script);
  tcc_free(s1->hot_text_file);
  tcc_free(s1->ram_text_file);
  tcc_free(s1->yasc_files); /* its names belong to interned_files */
  tcc_free(s1->sys_files);  /* so do these */
  tcc_free(s1->diag_once);
  tcc_error_stash_free(s1);
  dynarray_reset(&s1->interned_files, &s1->nb_interned_files);
  dynarray_reset(&s1->hot_text_flash_names, &s1->nb_hot_text_flash_names);
  dynarray_reset(&s1->ram_text_names, &s1->nb_ram_text_names);
  dynarray_reset(&s1->ram_text_section_names, &s1->nb_ram_text_section_names);
#ifdef CONFIG_TCC_DEBUG
  tcc_free(s1->dump_ir_passes);
#endif
  if (s1->ld_script)
  {
    ld_script_cleanup(s1->ld_script);
    tcc_free(s1->ld_script);
  }
  dynarray_reset(&s1->files, &s1->nb_files);
  dynarray_reset(&s1->target_deps, &s1->nb_target_deps);
  dynarray_reset(&s1->pragma_libs, &s1->nb_pragma_libs);
  dynarray_reset(&s1->argv, &s1->argc);
  cstr_free(&s1->cmdline_defs);
  cstr_free(&s1->cmdline_incl);
  cstr_free(&s1->linker_arg);
  tcc_free(s1->dState);
  /* free loaded dlls array */
  dynarray_reset(&s1->loaded_dlls, &s1->nb_loaded_dlls);
  tcc_yaff_libs_free(s1);
  tcc_free(s1);
#ifdef MEM_DEBUG
  tcc_memcheck(-1);
#endif
}

LIBTCCAPI int tcc_set_output_type(TCCState *s, int output_type)
{
#if defined(CONFIG_TCC_PIE)
  if (output_type == TCC_OUTPUT_EXE)
  {
#if defined(TCC_TARGET_ARM_THUMB)
    /* Disable PIE for ARM Thumb targets - the YAFF format handles
       data relocations directly from R_ARM_ABS32 entries without
       needing the full DYN infrastructure (which breaks GOT filling
       for local symbols) */
#elif defined(s)
    if (s->no_pie)
    {
      /* Explicitly disabled via -no-pie */
    }
    else
    {
      output_type |= TCC_OUTPUT_DYN;
    }
#else
    output_type |= TCC_OUTPUT_DYN;
#endif
  }
#endif
  s->output_type = output_type;

  if (!s->nostdinc)
  {
    /* default include paths */
    /* -isystem paths have already been handled */
    tcc_add_sysinclude_path(s, CONFIG_TCC_SYSINCLUDEPATHS);
  }

  if (output_type == TCC_OUTPUT_PREPROCESS)
  {
    s->do_debug = 0;
    return 0;
  }

  /* add sections */
  tccelf_new(s);

  /* Objects need the library paths too: -fmodule-local-calls reads the
   * export tables of the shared libraries found there while compiling. */
  tcc_add_library_path(s, CONFIG_TCC_LIBPATHS);

  if (output_type == TCC_OUTPUT_OBJ)
  {
    /* always elf for objects */
    s->output_format = TCC_OUTPUT_FORMAT_ELF;
    return 0;
  }

  /* paths for crt objects */
  tcc_split_path(s, &s->crt_paths, &s->nb_crt_paths, CONFIG_TCC_CRTPREFIX);
  if (output_type != TCC_OUTPUT_MEMORY && !s->nodefaultlibs && !s->nostdlib)
    tccelf_add_crtbegin(s);
  return 0;
}

LIBTCCAPI int tcc_add_include_path(TCCState *s, const char *pathname)
{
  tcc_split_path(s, &s->include_paths, &s->nb_include_paths, pathname);
  return 0;
}

LIBTCCAPI int tcc_add_sysinclude_path(TCCState *s, const char *pathname)
{
  tcc_split_path(s, &s->sysinclude_paths, &s->nb_sysinclude_paths, pathname);
  return 0;
}

/* -iyasc DIR: searched in its place among the -I directories, and a header
   found there is a yasc file, as if it began with #pragma yasc (so are the
   "..." headers next to it) -- parse_include */
LIBTCCAPI int tcc_add_yasc_include_path(TCCState *s, const char *pathname)
{
  int first = s->nb_include_paths;
  tcc_split_path(s, &s->include_paths, &s->nb_include_paths, pathname);
  for (int i = first; i < s->nb_include_paths; i++)
    dynarray_add(&s->yasc_include_paths, &s->nb_yasc_include_paths, s->include_paths[i]);
  return 0;
}

/* DIR (an include_paths entry) came from -iyasc */
ST_FUNC int tcc_yasc_include_dir(TCCState *s1, const char *dir)
{
  for (int i = 0; i < s1->nb_yasc_include_paths; i++)
    if (s1->yasc_include_paths[i] == dir)
      return 1;
  return 0;
}

/* add/update a 'DLLReference', Just find if level == -1  */
ST_FUNC DLLReference *tcc_add_dllref(TCCState *s1, const char *dllname, int level)
{
  DLLReference *ref = NULL;
  int i;
  for (i = 0; i < s1->nb_loaded_dlls; i++)
    if (0 == strcmp(s1->loaded_dlls[i]->name, dllname))
    {
      ref = s1->loaded_dlls[i];
      break;
    }
  if (level == -1)
    return ref;
  if (ref)
  {
    if (level < ref->level)
      ref->level = level;
    ref->found = 1;
    return ref;
  }
  ref = tcc_mallocz(sizeof(DLLReference) + strlen(dllname));
  strcpy(ref->name, dllname);
  dynarray_add(&s1->loaded_dlls, &s1->nb_loaded_dlls, ref);
  ref->level = level;
  ref->index = s1->nb_loaded_dlls;
  return ref;
}

static int guess_filetype(const char *filename);

ST_FUNC int tcc_add_file_internal(TCCState *s1, const char *filename, int flags)
{
  int fd, ret = -1;
  unsigned open_start = 0;
  unsigned elapsed = 0;

  if (0 == (flags & AFF_TYPE_MASK))
    flags |= guess_filetype(filename);

  /* ignore binary files with -E */
  if (s1->output_type == TCC_OUTPUT_PREPROCESS && (flags & AFF_TYPE_BIN))
    return 0;

  /* open the file */
  if (s1->do_bench)
    open_start = tcc_getclock_us();
  fd = _tcc_open(s1, filename);
  tcc_init_stamp("source open");
  if (s1->do_bench)
  {
    elapsed = tcc_getclock_us() - open_start;
    s1->bench_file_open_time += elapsed;
    s1->bench_file_open_count++;
    tcc_bench_log(s1, "open", filename, elapsed);
  }
  if (fd < 0)
  {
    if (flags & AFF_PRINT_ERROR)
      tcc_error_noabort("file '%s' not found", filename);
    return FILE_NOT_FOUND;
  }

  s1->current_filename = filename;
  s1->current_archive_offset = 0;  /* Reset archive offset for regular files */
  s1->current_archive_path = NULL; /* Reset archive path for regular files */
  if (flags & AFF_TYPE_BIN)
  {
    ElfW(Ehdr) ehdr;
    int obj_type;

    obj_type = tcc_object_type(fd, &ehdr);
    lseek(fd, 0, SEEK_SET);

    switch (obj_type)
    {
    case AFF_BINTYPE_REL:
      ret = tcc_load_object_file(s1, fd, 0);
      break;

    case AFF_BINTYPE_AR:
      ret = tcc_load_archive(s1, fd, !(flags & AFF_WHOLE_ARCHIVE));
      break;
    case AFF_BINTYPE_YAFF:
      ret = tcc_load_yaff(s1, fd, filename, (flags & AFF_REFERENCED_DLL) != 0);
      break;

    case AFF_BINTYPE_DYN:
      if (s1->output_type == TCC_OUTPUT_MEMORY)
      {
      }
      else
      {
        ret = tcc_load_dll(s1, fd, filename, (flags & AFF_REFERENCED_DLL) != 0);
      }
      break;

    default:
      /* as GNU ld, consider it is an ld script if not recognized */
      ret = tcc_load_ldscript(s1, fd);
      goto check_success;

    check_success:
      if (ret < 0)
        tcc_error_noabort("%s: unrecognized file type", filename);
      break;
    }
    close(fd);
  }
  else
  {
    /* update target deps */
    dynarray_add(&s1->target_deps, &s1->nb_target_deps, tcc_strdup(filename));
    ret = tcc_compile(s1, flags, filename, fd);
    /* -fstack-usage linking straight away: <source basename>.su here, as gcc
       did before version 11; with -c, the object's name (tcc.c) */
    if (!ret && (s1->stack_usage || s1->callgraph_info) && s1->output_type != TCC_OUTPUT_OBJ &&
        s1->output_type != TCC_OUTPUT_PREPROCESS && !s1->syntax_only &&
        !(flags & (AFF_TYPE_ASM | AFF_TYPE_ASMPP)))
      tcc_stack_usage_write(s1, tcc_basename(filename));
  }
  s1->current_filename = NULL;
  return ret;
}

static int guess_filetype(const char *filename)
{
  int filetype = 0;
  if (1)
  {
    /* use a file extension to detect a filetype */
    const char *ext = tcc_fileextension(filename);
    if (ext[0])
    {
      ext++;
      if (!strcmp(ext, "S"))
        filetype = AFF_TYPE_ASMPP;
      else if (!strcmp(ext, "s"))
        filetype = AFF_TYPE_ASM;
      else if (!PATHCMP(ext, "c") || !PATHCMP(ext, "h") || !PATHCMP(ext, "i"))
        filetype = AFF_TYPE_C;
      else
        filetype |= AFF_TYPE_BIN;
    }
    else
    {
      filetype = AFF_TYPE_C;
    }
  }
  return filetype;
}

LIBTCCAPI int tcc_add_file(TCCState *s, const char *filename)
{
  return tcc_add_file_internal(s, filename, s->filetype | AFF_PRINT_ERROR);
}

LIBTCCAPI int tcc_add_library_path(TCCState *s, const char *pathname)
{
  tcc_split_path(s, &s->library_paths, &s->nb_library_paths, pathname);
  return 0;
}

static int tcc_add_library_internal(TCCState *s1, const char *fmt, const char *filename, int flags, char **paths,
                                    int nb_paths)
{
  char buf[1024];
  int i, ret;
  unsigned resolve_start = 0;

  if (s1->do_bench)
    resolve_start = tcc_getclock_us();

  for (i = 0; i < nb_paths; i++)
  {
    snprintf(buf, sizeof(buf), fmt, paths[i], filename);
    ret = tcc_add_file_internal(s1, buf, flags & ~AFF_PRINT_ERROR);
    if (ret != FILE_NOT_FOUND)
    {
      if (s1->do_bench)
      {
        unsigned elapsed = tcc_getclock_us() - resolve_start;
        s1->bench_library_resolve_time += elapsed;
        s1->bench_library_resolve_count++;
        tcc_bench_log(s1, "resolve-lib", buf, elapsed);
      }
      return ret;
    }
  }
  if (s1->do_bench)
  {
    unsigned elapsed = tcc_getclock_us() - resolve_start;
    s1->bench_library_resolve_time += elapsed;
    s1->bench_library_resolve_count++;
    tcc_bench_log(s1, "resolve-lib", filename, elapsed);
  }
  if (flags & AFF_PRINT_ERROR)
    tcc_error_noabort("library '%s' not found", filename);
  return FILE_NOT_FOUND;
}

/* find and load a dll. Return non zero if not found */
ST_FUNC int tcc_add_dll(TCCState *s, const char *filename, int flags)
{
  return tcc_add_library_internal(s, "%s/%s", filename, flags, s->library_paths, s->nb_library_paths);
}

/* find [cross-]libtcc1.a and tcc helper objects in library path */
ST_FUNC int tcc_add_support(TCCState *s1, const char *filename)
{
  char buf[100];
  if (CONFIG_TCC_CROSSPREFIX[0])
    filename = strcat(strcpy(buf, CONFIG_TCC_CROSSPREFIX), filename);
  return tcc_add_dll(s1, filename, AFF_PRINT_ERROR);
}

#if !defined TCC_TARGET_PE && !defined TCC_TARGET_MACHO
ST_FUNC int tcc_add_crt(TCCState *s1, const char *filename)
{
  return tcc_add_library_internal(s1, "%s/%s", filename, AFF_PRINT_ERROR, s1->crt_paths, s1->nb_crt_paths);
}
#endif

/* the library name is the same as the argument of the '-l' option */
LIBTCCAPI int tcc_add_library(TCCState *s, const char *libraryname)
{
  static const char *const libs[] = {"%s/lib%s.so", "%s/lib%s.a", NULL};
  const char *const *pp = s->static_link ? libs + 1 : libs;
  int flags = s->filetype & AFF_WHOLE_ARCHIVE;
  while (*pp)
  {
    int ret = tcc_add_library_internal(s, *pp, libraryname, flags, s->library_paths, s->nb_library_paths);
    if (ret != FILE_NOT_FOUND)
      return ret;
    ++pp;
  }
  return tcc_add_dll(s, libraryname, AFF_PRINT_ERROR);
}

/* handle #pragma comment(lib,) */
ST_FUNC void tcc_add_pragma_libs(TCCState *s1)
{
  int i;
  for (i = 0; i < s1->nb_pragma_libs; i++)
    tcc_add_library(s1, s1->pragma_libs[i]);
}

LIBTCCAPI int tcc_add_symbol(TCCState *s1, const char *name, const void *val)
{
  char buf[256];
  if (s1->leading_underscore)
  {
    buf[0] = '_';
    pstrcpy(buf + 1, sizeof(buf) - 1, name);
    name = buf;
  }
  set_global_sym(s1, name, NULL, (addr_t)(uintptr_t)val); /* NULL: SHN_ABS */
  return 0;
}

LIBTCCAPI void tcc_set_lib_path(TCCState *s, const char *path)
{
  tcc_free(s->tcc_lib_path);
  s->tcc_lib_path = tcc_strdup(path);
}

/********************************************************/
/* options parser */

static int strstart(const char *val, const char **str)
{
  const char *p, *q;
  p = *str;
  q = val;
  while (*q)
  {
    if (*p != *q)
      return 0;
    p++;
    q++;
  }
  *str = p;
  return 1;
}

/* Like strstart, but automatically takes into account that ld options can
 *
 * - start with double or single dash (e.g. '--soname' or '-soname')
 * - arguments can be given as separate or after '=' (e.g. '-Wl,-soname,x.so'
 *   or '-Wl,-soname=x.so')
 *
 * you provide `val` always in 'option[=]' form (no leading -)
 */
static int link_option(const char *str, const char *val, const char **ptr)
{
  const char *p, *q;
  int ret;

  /* there should be 1 or 2 dashes */
  if (*str++ != '-')
    return 0;
  if (*str == '-')
    str++;

  /* then str & val should match (potentially up to '=') */
  p = str;
  q = val;

  ret = 1;
  if (q[0] == '?')
  {
    ++q;
    if (strstart("no-", &p))
      ret = -1;
  }

  while (*q != '\0' && *q != '=')
  {
    if (*p != *q)
      return 0;
    p++;
    q++;
  }

  /* '=' near eos means ',' or '=' is ok */
  if (*q == '=')
  {
    if (*p == 0)
      *ptr = p;
    if (*p != ',' && *p != '=')
      return 0;
    p++;
  }
  else if (*p && *p != ',')
  {
    /* A bare boolean flag (no '=') may still be followed by a ',' when it
       precedes another suboption in the same -Wl, comma chain.  Leave *ptr
       pointing at that comma so tcc_set_linker()'s skip_linker_arg() advances
       to the next suboption instead of rejecting the whole chain. */
    return 0;
  }
  *ptr = p;
  return ret;
}

static int link_arg(const char *opt, const char *str)
{
  int l = strlen(opt);
  return 0 == strncmp(opt, str, l) && (str[l] == '\0' || str[l] == ',');
}

static const char *skip_linker_arg(const char **str)
{
  const char *s1 = *str;
  const char *s2 = strchr(s1, ',');
  *str = s2 ? s2++ : (s2 = s1 + strlen(s1));
  return s2;
}

static void copy_linker_arg(char **pp, const char *s, int sep)
{
  const char *q = s;
  char *p = *pp;
  int l = 0;
  if (p && sep)
    p[l = strlen(p)] = sep, ++l;
  skip_linker_arg(&q);
  pstrncpy(l + (*pp = tcc_realloc(p, q - s + l + 1)), s, q - s);
}

static void args_parser_add_file(TCCState *s, const char *filename, int filetype)
{
  struct filespec *f = tcc_malloc(sizeof *f + strlen(filename));
  f->type = filetype;
  strcpy(f->name, filename);
  dynarray_add(&s->files, &s->nb_files, f);
}

static void args_parser_add_group_marker(TCCState *s, int marker_type)
{
  struct filespec *f = tcc_malloc(sizeof *f + 1);
  f->type = marker_type;
  f->name[0] = '\0';
  dynarray_add(&s->files, &s->nb_files, f);
}

/* set linker options */
static int tcc_set_linker(TCCState *s, const char *option)
{
  TCCState *s1 = s;
  while (*option)
  {

    const char *p = NULL;
    char *end = NULL;
    int ignoring = 0;
    int ret;

    if (link_option(option, "Bsymbolic", &p))
    {
      s->symbolic = 1;
    }
    else if (link_option(option, "nostdlib", &p))
    {
      s->nostdlib = 1;
    }
    else if (link_option(option, "nodefaultlibs", &p))
    {
      s->nodefaultlibs = 1;
    }
    else if (link_option(option, "e=", &p) || link_option(option, "entry=", &p))
    {
      copy_linker_arg(&s->elf_entryname, p, 0);
    }
    else if (link_option(option, "fini=", &p))
    {
      copy_linker_arg(&s->fini_symbol, p, 0);
      ignoring = 1;
    }
    else if (link_option(option, "image-base=", &p) || link_option(option, "Ttext=", &p))
    {
      s->text_addr = strtoull(p, &end, 16);
      s->has_text_addr = 1;
    }
    else if (link_option(option, "init=", &p))
    {
      copy_linker_arg(&s->init_symbol, p, 0);
      ignoring = 1;
    }
    else if (link_option(option, "Map=", &p))
    {
      copy_linker_arg(&s->mapfile, p, 0);
      ignoring = 1;
    }
    else if (link_option(option, "oformat=", &p))
    {
#if PTR_SIZE == 8
      if (strstart("elf64-", &p))
      {
#else
      if (strstart("elf32-", &p))
      {
#endif
        s->output_format = TCC_OUTPUT_FORMAT_ELF;
      }
      else if (link_arg("binary", p))
      {
        s->output_format = TCC_OUTPUT_FORMAT_BINARY;
#ifdef TCC_TARGET_YAFF
      }
      else if (link_arg("yaff", p))
      {
        s->output_format = TCC_OUTPUT_FORMAT_YAFF;
#endif
      }
      else
        goto err;
    }
    else if (link_option(option, "as-needed", &p))
    {
      ignoring = 1;
    }
    else if (link_option(option, "O", &p))
    {
      ignoring = 1;
    }
    else if (link_option(option, "export-all-symbols", &p))
    {
      s->rdynamic = 1;
    }
    else if (link_option(option, "export-dynamic", &p))
    {
      s->rdynamic = 1;
    }
    else if (link_option(option, "rpath=", &p))
    {
      copy_linker_arg(&s->rpath, p, ':');
    }
    else if (link_option(option, "enable-new-dtags", &p))
    {
      s->enable_new_dtags = 1;
    }
    else if (link_option(option, "gc-sections", &p))
    {
      s->gc_sections = 1;
    }
    else if (link_option(option, "hot-text=", &p))
    {
      copy_linker_arg(&s->hot_text_file, p, 0);
      ignoring = 1;
    }
    else if (link_option(option, "ram-text=", &p))
    {
      copy_linker_arg(&s->ram_text_file, p, 0);
      ignoring = 1;
    }
    else if (link_option(option, "gc-sections-aggressive", &p))
    {
      /* The lazy per-object loader this used to enable linked symbols
         against object-local section numbers (and collected nothing the
         plain pass does not).  Kept as a spelling of --gc-sections. */
      s->gc_sections = 1;
    }
    else if (link_option(option, "no-gc-sections", &p))
    {
      s->gc_sections = 0;
    }
    else if (link_option(option, "section-alignment=", &p))
    {
      s->section_align = strtoul(p, &end, 16);
    }
    else if (link_option(option, "soname=", &p))
    {
      copy_linker_arg(&s->soname, p, 0);
    }
    else if (link_option(option, "install_name=", &p))
    {
      copy_linker_arg(&s->soname, p, 0);
    }
    else if (link_option(option, "start-group", &p))
    {
      args_parser_add_group_marker(s, AFF_GROUP_START);
    }
    else if (link_option(option, "end-group", &p))
    {
      args_parser_add_group_marker(s, AFF_GROUP_END);
    }
    else if (ret = link_option(option, "?whole-archive", &p), ret)
    {
      if (ret > 0)
        s->filetype |= AFF_WHOLE_ARCHIVE;
      else
        s->filetype &= ~AFF_WHOLE_ARCHIVE;
    }
    else if (link_option(option, "z=", &p))
    {
      ignoring = 1;
    }
    else if (p)
    {
      return 0;
    }
    else
    {
    err:
      return tcc_error_noabort("unsupported linker option '%s'", option);
    }
    if (ignoring)
      tcc_warning_c(warn_unsupported)("unsupported linker option '%s'", option);
    option = skip_linker_arg(&p);
  }
  return 1;
}

typedef struct TCCOption
{
  const char *TCC_RODATA_REL name;
  uint16_t index;
  uint16_t flags;
} TCCOption;

enum
{
  TCC_OPTION_ignored = 0,
  TCC_OPTION_HELP,
  TCC_OPTION_HELP2,
  TCC_OPTION_v,
  TCC_OPTION_I,
  TCC_OPTION_D,
  TCC_OPTION_U,
  TCC_OPTION_P,
  TCC_OPTION_L,
  TCC_OPTION_B,
  TCC_OPTION_l,
  TCC_OPTION_bench,
  TCC_OPTION_bt,
  TCC_OPTION_b,
  TCC_OPTION_ba,
  TCC_OPTION_g,
  TCC_OPTION_c,
  TCC_OPTION_dumpdefaults,
  TCC_OPTION_dumpmachine,
  TCC_OPTION_dumpversion,
  TCC_OPTION_d,
  TCC_OPTION_static,
  TCC_OPTION_ccompat,
  TCC_OPTION_std,
  TCC_OPTION_shared,
  TCC_OPTION_soname,
  TCC_OPTION_o,
  TCC_OPTION_r,
  TCC_OPTION_Wl,
  TCC_OPTION_Wp,
  TCC_OPTION_W,
  TCC_OPTION_O,
  TCC_OPTION_mfloat_abi,
  TCC_OPTION_mfpu,
  TCC_OPTION_mfp_inline,
  TCC_OPTION_mfp_lib,
  TCC_OPTION_march,
  TCC_OPTION_m,
  TCC_OPTION_f,
  TCC_OPTION_isystem,
  TCC_OPTION_iyasc,
  TCC_OPTION_iwithprefix,
  TCC_OPTION_include,
  TCC_OPTION_nostdinc,
  TCC_OPTION_nostdlib,
  TCC_OPTION_nodefaultlibs,
  TCC_OPTION_print_search_dirs,
  TCC_OPTION_rdynamic,
  TCC_OPTION_pthread,
  TCC_OPTION_run,
  TCC_OPTION_w,
  TCC_OPTION_E,
  TCC_OPTION_M,
  TCC_OPTION_MD,
  TCC_OPTION_MF,
  TCC_OPTION_MM,
  TCC_OPTION_MMD,
  TCC_OPTION_MP,
  TCC_OPTION_x,
  TCC_OPTION_ar,
  TCC_OPTION_impdef,
  TCC_OPTION_yasc_c_header,
  TCC_OPTION_dynamiclib,
  TCC_OPTION_flat_namespace,
  TCC_OPTION_two_levelnamespace,
  TCC_OPTION_undefined,
  TCC_OPTION_install_name,
  TCC_OPTION_compatibility_version,
  TCC_OPTION_current_version,
  TCC_OPTION_mpic_data_is_text_relative,
  TCC_OPTION_stack_size,
  TCC_OPTION_slow_memory,
  TCC_OPTION_heap_size,
  TCC_OPTION_share_rodata,
  TCC_OPTION_no_share_rodata,
  TCC_OPTION_sb_relative_got,
  TCC_OPTION_no_sb_relative_got,
  TCC_OPTION_fpic,
  TCC_OPTION_fno_pic,
  TCC_OPTION_fpie,
  TCC_OPTION_no_pie,
  TCC_OPTION_T,
#ifdef CONFIG_TCC_DEBUG
  TCC_OPTION_dump_ir,
  TCC_OPTION_dump_ir_passes,
#endif
};

#define TCC_OPTION_HAS_ARG 0x0001
#define TCC_OPTION_NOSEP 0x0002 /* cannot have space before option and arg */

static const TCCOption tcc_options[] = {
    {"h", TCC_OPTION_HELP, 0},
    {"-help", TCC_OPTION_HELP, 0},
    {"?", TCC_OPTION_HELP, 0},
    {"hh", TCC_OPTION_HELP2, 0},
    {"v", TCC_OPTION_v, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"-version", TCC_OPTION_v, 0}, /* handle as verbose, also prints version*/
    {"I", TCC_OPTION_I, TCC_OPTION_HAS_ARG},
    {"D", TCC_OPTION_D, TCC_OPTION_HAS_ARG},
    {"U", TCC_OPTION_U, TCC_OPTION_HAS_ARG},
    {"P", TCC_OPTION_P, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"L", TCC_OPTION_L, TCC_OPTION_HAS_ARG},
    {"B", TCC_OPTION_B, TCC_OPTION_HAS_ARG},
    {"l", TCC_OPTION_l, TCC_OPTION_HAS_ARG},
    {"bench", TCC_OPTION_bench, 0},
#ifdef CONFIG_TCC_BCHECK
    {"b", TCC_OPTION_b, 0},
#endif
    {"g", TCC_OPTION_g, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"c", TCC_OPTION_c, 0},
    {"dumpdefaults", TCC_OPTION_dumpdefaults, 0},
    {"dumpmachine", TCC_OPTION_dumpmachine, 0},
    {"dumpversion", TCC_OPTION_dumpversion, 0},
#ifdef CONFIG_TCC_DEBUG
    /* Must appear before the short "-d" option, otherwise "-dump-ir" is parsed as "-d ump-ir". */
    {"dump-ir", TCC_OPTION_dump_ir, 0},
    {"dump-ir-passes=", TCC_OPTION_dump_ir_passes, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
#endif
    {"d", TCC_OPTION_d, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"static", TCC_OPTION_static, 0},
    {"ccompat", TCC_OPTION_ccompat, 0},
    {"std", TCC_OPTION_std, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"shared", TCC_OPTION_shared, 0},
    {"soname", TCC_OPTION_soname, TCC_OPTION_HAS_ARG},
    {"o", TCC_OPTION_o, TCC_OPTION_HAS_ARG},
    {"pthread", TCC_OPTION_pthread, 0},
    {"run", TCC_OPTION_run, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"rdynamic", TCC_OPTION_rdynamic, 0},
    {"r", TCC_OPTION_r, 0},
    {"Wl,", TCC_OPTION_Wl, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"Wp,", TCC_OPTION_Wp, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"W", TCC_OPTION_W, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"O", TCC_OPTION_O, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"fpie", TCC_OPTION_fpie, 0},
    {"fpic", TCC_OPTION_fpic, 0},
    {"fno-pic", TCC_OPTION_fno_pic, 0},
    {"no-pie", TCC_OPTION_no_pie, 0},
#if defined(TCC_TARGET_ARM) || defined(TCC_TARGET_ARM_THUMB)
    {"mfloat-abi=", TCC_OPTION_mfloat_abi, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"mfloat-abi", TCC_OPTION_mfloat_abi, TCC_OPTION_HAS_ARG},
    {"mfpu=", TCC_OPTION_mfpu, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"mfpu", TCC_OPTION_mfpu, TCC_OPTION_HAS_ARG},
    {"mfp-inline=", TCC_OPTION_mfp_inline, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"mfp-inline", TCC_OPTION_mfp_inline, TCC_OPTION_HAS_ARG},
    {"mfp-lib=", TCC_OPTION_mfp_lib, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"mfp-lib", TCC_OPTION_mfp_lib, TCC_OPTION_HAS_ARG},
    {"march=", TCC_OPTION_march, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"march", TCC_OPTION_march, TCC_OPTION_HAS_ARG},
    {"mpic-data-is-text-relative", TCC_OPTION_mpic_data_is_text_relative, 0},
    {"slow-memory", TCC_OPTION_slow_memory, 0},
    {"stack-size=", TCC_OPTION_stack_size, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"stack-size", TCC_OPTION_stack_size, TCC_OPTION_HAS_ARG},
    {"heap-size=", TCC_OPTION_heap_size, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"heap-size", TCC_OPTION_heap_size, TCC_OPTION_HAS_ARG},
    {"share-rodata", TCC_OPTION_share_rodata, 0},
    {"no-share-rodata", TCC_OPTION_no_share_rodata, 0},
    {"msb-relative-got", TCC_OPTION_sb_relative_got, 0},
    {"mno-sb-relative-got", TCC_OPTION_no_sb_relative_got, 0},
#endif
    {"m", TCC_OPTION_m, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"f", TCC_OPTION_f, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"isystem", TCC_OPTION_isystem, TCC_OPTION_HAS_ARG},
    {"iyasc", TCC_OPTION_iyasc, TCC_OPTION_HAS_ARG},
    {"include", TCC_OPTION_include, TCC_OPTION_HAS_ARG},
    {"nostdinc", TCC_OPTION_nostdinc, 0},
    {"nostdlib", TCC_OPTION_nostdlib, 0},
    {"nodefaultlibs", TCC_OPTION_nodefaultlibs, 0},
    {"print-search-dirs", TCC_OPTION_print_search_dirs, 0},
    {"w", TCC_OPTION_w, 0},
    {"E", TCC_OPTION_E, 0},
    {"M", TCC_OPTION_M, 0},
    {"MD", TCC_OPTION_MD, 0},
    {"MF", TCC_OPTION_MF, TCC_OPTION_HAS_ARG},
    {"MM", TCC_OPTION_MM, 0},
    {"MMD", TCC_OPTION_MMD, 0},
    {"MP", TCC_OPTION_MP, 0},
    {"x", TCC_OPTION_x, TCC_OPTION_HAS_ARG},
    {"T", TCC_OPTION_T, TCC_OPTION_HAS_ARG},
    {"ar", TCC_OPTION_ar, 0},
    {"yasc-c-header", TCC_OPTION_yasc_c_header, 0},
    /* ignored (silently, except after -Wunsupported) */
    {"arch", 0, TCC_OPTION_HAS_ARG},
    {"C", 0, 0},
    {"-param", 0, TCC_OPTION_HAS_ARG},
    {"pedantic", 0, 0},
    {"pipe", 0, 0},
    {"s", 0, 0},
    {"traditional", 0, 0},
    {NULL, 0, 0},
};

typedef struct FlagDef
{
  uint16_t offset;
  uint16_t flags;
  const char *TCC_RODATA_REL name;
} FlagDef;

#define WD_ALL 0x0001    /* warning is activated when using -Wall */
#define FD_INVERT 0x0002 /* invert value before storing */
#define WD_SAFE 0x0004   /* on in the safe default (tcc_safe_defaults), -Wall or not */
#define WD_SAFE_ERR 0x0008 /* an error in the safe default */
#define WD_UNUSED 0x0010 /* switched by -Wunused (the "unused" entry) too */
#define WD_EXTRA 0x0020  /* switched by -Wextra too (gcc's -Wextra members tcc has) */
#define WD_EXTRA_UNUSED 0x0040 /* ... only with -Wunused or -Wall as well, as gcc */
#define WD_STRICT 0x0080 /* the safe default: an error the command line and #pragma diagnostic cannot
                            lower (strict lifetimes); only unsafe turns it off */

/* options_W entries the command line set, by index */
static int warn_user_test(TCCState *s, int i)
{
  return s->warn_user[i >> 5] >> (i & 31) & 1;
}
static void warn_user_set(TCCState *s, int i)
{
  s->warn_user[i >> 5] |= 1u << (i & 31);
}

static const FlagDef options_W[] = {
    {offsetof(TCCState, warn_all), WD_ALL, "all"},
    /* the group: every WD_EXTRA entry follows it (set_flag walks on from here).
       The safe default (WD_SAFE) has every check but the ones that take a
       value (frame-larger-than=, stack-usage=) and the switches that are no
       check (error, system-headers). */
    {offsetof(TCCState, warn_extra), WD_SAFE, "extra"},
    {offsetof(TCCState, warn_error), 0, "error"},
    {offsetof(TCCState, warn_write_strings), WD_SAFE, "write-strings"},
    {offsetof(TCCState, warn_unsupported), WD_SAFE, "unsupported"},
    /* Not a warning: the warnings of system headers are not given (gcc), and
       this gives them (error1).  Not in -Wall, as in gcc. */
    {offsetof(TCCState, warn_system_headers), 0, "system-headers"},
    /* Not in -Wall: a signature per global symbol in each object, compared
       at the link (typesig.c) */
    {offsetof(TCCState, warn_cross_tu), WD_SAFE, "cross-tu"},
    /* Not in -Wall (gcc has no such check): a CFG walk in a function with a
       printf-style call and an untrusted source (ir/taint.c) */
    {offsetof(TCCState, warn_tainted_format), WD_SAFE, "tainted-format"},
    {offsetof(TCCState, warn_implicit_function_declaration), WD_ALL | WD_SAFE_ERR, "implicit-function-declaration"},
    {offsetof(TCCState, warn_discarded_qualifiers), WD_ALL | WD_SAFE_ERR, "discarded-qualifiers"},
    /* Not in -Wall: a CFG walk per function, yasc only; the safe default has it. */
    {offsetof(TCCState, warn_lifetime), WD_SAFE | WD_STRICT, "lifetime"},
    {offsetof(TCCState, warn_int_conversion), WD_ALL | WD_SAFE_ERR, "int-conversion"},
    {offsetof(TCCState, warn_incompatible_pointer_types), WD_ALL | WD_SAFE_ERR, "incompatible-pointer-types"},
    {offsetof(TCCState, warn_return_type), WD_ALL | WD_SAFE_ERR, "return-type"},
    {offsetof(TCCState, warn_implicit_int), WD_ALL | WD_SAFE_ERR, "implicit-int"},
    /* Not in -Wall: a CFG walk per function; the safe default has them. */
    {offsetof(TCCState, warn_uninitialized), WD_SAFE, "uninitialized"},
    {offsetof(TCCState, warn_maybe_uninitialized), WD_SAFE, "maybe-uninitialized"},
    {offsetof(TCCState, warn_return_local_addr), WD_ALL | WD_STRICT, "return-local-addr"},
    /* Not in -Wall: -Wall -Werror builds of code using format() (tinycc's own)
       would stop; the safe default has it. */
    {offsetof(TCCState, warn_ignored_attributes), WD_SAFE, "ignored-attributes"},
    {offsetof(TCCState, warn_nonnull), WD_ALL, "nonnull"},
    /* Not in -Wall: a CFG walk per function holding a local's address; the safe default has it. */
    {offsetof(TCCState, warn_dangling_pointer), WD_SAFE | WD_STRICT, "dangling-pointer"},
    /* Not in -Wall: what it finds depends on -O; the safe default has it. */
    {offsetof(TCCState, warn_null_dereference), WD_SAFE, "null-dereference"},
    /* On by default, yasc only: an owned variable handed on without move(p). */
    {offsetof(TCCState, warn_implicit_move), WD_STRICT, "implicit-move"},
    {offsetof(TCCState, warn_unchecked), WD_STRICT, "unchecked"},
    /* yasc spells the null pointer one way: nullptr (NULL and 0 are
       regular C).  On in the safe default; -ccompat code says nothing. */
    {offsetof(TCCState, warn_zero_as_null_pointer_constant), WD_SAFE, "zero-as-null-pointer-constant"},
    /* yasc only: `REG->f = 1` on volatile memory is a hidden read-modify-write
       of the whole unit (W1C bits, write-only registers, no atomicity) */
    {offsetof(TCCState, warn_volatile_bitfield), WD_STRICT, "volatile-bitfield"},
    /* yasc only: a register field marked ro written, wo read, w1c written back */
    {offsetof(TCCState, warn_register_access), WD_STRICT, "register-access"},
    /* Not in -Wall (nor in gcc's).  The safe default reads `()` as `(void)`
       (C23), so what is left is an old-style definition: never written by
       accident. */
    {offsetof(TCCState, warn_strict_prototypes), WD_SAFE, "strict-prototypes"},
    {offsetof(TCCState, warn_missing_prototypes), WD_SAFE, "missing-prototypes"},
    /* gcc: -Wextra.  A designator naming bits an earlier element already
       set: the earlier value is lost (in a union, the whole other member). */
    {offsetof(TCCState, warn_override_init), WD_SAFE | WD_EXTRA, "override-init"},
    {offsetof(TCCState, warn_pointer_arith), WD_SAFE, "pointer-arith"},
    {offsetof(TCCState, warn_cast_qual), WD_SAFE, "cast-qual"},
    {offsetof(TCCState, warn_cast_align), WD_SAFE, "cast-align"},
    /* gcc has it on by default */
    {offsetof(TCCState, warn_address_of_packed_member), WD_ALL, "address-of-packed-member"},
    /* Not in -Wall (nor in gcc's): they take a limit, -Wframe-larger-than=N
       and -Wstack-usage=N; the plain names are for -Werror=, -Wno- and
       #pragma diagnostic (stack_usage.c). */
    {offsetof(TCCState, warn_frame_larger_than), 0, "frame-larger-than"},
    {offsetof(TCCState, warn_stack_usage), 0, "stack-usage"},
    /* Not in -Wall (gcc has them in neither): a VLA or alloca is valid C.  In
       the safe default: YasOS stacks are 4-8 KB and these are the frames no
       limit bounds; a frontend test at the declaration (a VLA parameter takes
       no stack and is not reported), nothing in our counted C trips it. */
    {offsetof(TCCState, warn_vla), WD_SAFE, "vla"},
    {offsetof(TCCState, warn_alloca), WD_SAFE, "alloca"},
    {offsetof(TCCState, warn_format), WD_ALL, "format"},
    /* Not in -Wall (gcc keeps it out too); cheap, and a non-literal format
       with nothing to format is the format-string hole: the safe default has it. */
    {offsetof(TCCState, warn_format_security), WD_SAFE, "format-security"},
    /* Not in -Wall (gcc has no such warning): %n in a literal printf format
       writes through an argument, never written by accident; an error in the
       safe default. */
    {offsetof(TCCState, warn_format_n), WD_SAFE_ERR, "format-n"},
    /* On by default, as with gcc */
    {offsetof(TCCState, warn_unused_result), WD_ALL, "unused-result"},
    {offsetof(TCCState, warn_deprecated_declarations), WD_ALL, "deprecated-declarations"},
    /* Not in -Wall (gcc: -Wextra): hits where the signed side is known
       non-negative in ways tcc does not see. */
    {offsetof(TCCState, warn_sign_compare), WD_EXTRA | WD_SAFE, "sign-compare"},
    /* Noisy (gcc: in neither -Wall nor -Wextra); -Wconversion implies
       -Wsign-conversion unless that is given. */
    {offsetof(TCCState, warn_conversion), WD_SAFE, "conversion"},
    {offsetof(TCCState, warn_sign_conversion), WD_SAFE, "sign-conversion"},
    /* gcc has these on by default: -Wall here, so the safe default too. */
    {offsetof(TCCState, warn_shift_count_overflow), WD_ALL, "shift-count-overflow"},
    {offsetof(TCCState, warn_shift_count_negative), WD_ALL, "shift-count-negative"},
    {offsetof(TCCState, warn_shift_overflow), WD_ALL, "shift-overflow"},
    {offsetof(TCCState, warn_div_by_zero), WD_ALL, "div-by-zero"},
    {offsetof(TCCState, warn_overflow), WD_ALL, "overflow"},
    {offsetof(TCCState, warn_tautological_compare), WD_ALL, "tautological-compare"},
    /* Not in -Wall (gcc: -Wextra): generic code compares unsigned values
       against limits that happen to be 0. */
    {offsetof(TCCState, warn_type_limits), WD_EXTRA | WD_SAFE, "type-limits"},
    {offsetof(TCCState, warn_array_bounds), WD_ALL, "array-bounds"},
    {offsetof(TCCState, warn_sizeof_pointer_memaccess), WD_ALL, "sizeof-pointer-memaccess"},
    {offsetof(TCCState, warn_sizeof_array_argument), WD_ALL, "sizeof-array-argument"},
    /* gcc: -Wextra.  The safe default has it: a case reaching the next one
       unmarked is a classic bug, and [[fallthrough]], __attribute__((fallthrough))
       or a "fallthrough" comment says it was meant. */
    {offsetof(TCCState, warn_implicit_fallthrough), WD_SAFE | WD_EXTRA, "implicit-fallthrough"},
    {offsetof(TCCState, warn_parentheses), WD_ALL, "parentheses"},
    /* gcc: -Wall */
    {offsetof(TCCState, warn_logical_not_parentheses), WD_ALL, "logical-not-parentheses"},
    /* gcc: on with -Wparentheses (so -Wall) */
    {offsetof(TCCState, warn_dangling_else), WD_ALL, "dangling-else"},
    /* gcc: -Wextra.  The safe default has it: one token to look at, and an
       empty `if` body that was meant says so with braces. */
    {offsetof(TCCState, warn_empty_body), WD_SAFE | WD_EXTRA, "empty-body"},
    {offsetof(TCCState, warn_switch), WD_ALL, "switch"},
    /* gcc: in neither -Wall nor -Wextra; noisy (every `i`, `len`, `buf` in an
       inner block). */
    {offsetof(TCCState, warn_shadow), WD_SAFE, "shadow"},
    /* -Wunused-*: gcc's C -Wall has all of them but the parameter ones, which
       are -Wextra there (and noisy: callbacks keep their signatures). */
    {offsetof(TCCState, warn_unused), WD_UNUSED, "unused"},
    {offsetof(TCCState, warn_unused_variable), WD_ALL | WD_UNUSED, "unused-variable"},
    {offsetof(TCCState, warn_unused_const_variable), WD_ALL | WD_UNUSED, "unused-const-variable"},
    {offsetof(TCCState, warn_unused_function), WD_ALL | WD_UNUSED, "unused-function"},
    {offsetof(TCCState, warn_unused_but_set_variable), WD_ALL | WD_UNUSED, "unused-but-set-variable"},
    {offsetof(TCCState, warn_unused_value), WD_ALL | WD_UNUSED, "unused-value"},
    {offsetof(TCCState, warn_unused_label), WD_ALL | WD_UNUSED, "unused-label"},
    {offsetof(TCCState, warn_unused_local_typedefs), WD_ALL | WD_UNUSED, "unused-local-typedefs"},
    {offsetof(TCCState, warn_unused_parameter), WD_EXTRA_UNUSED | WD_SAFE, "unused-parameter"},
    {offsetof(TCCState, warn_unused_but_set_parameter), WD_EXTRA_UNUSED | WD_SAFE, "unused-but-set-parameter"},
    /* Object sizes (builtin/objsize.c).  gcc has -Wstringop-overflow,
       -Walloc-size-larger-than and -Wfree-nonheap-object on by default and the
       rest in -Wall: -Wall here.  The safe default adds nothing to that. */
    {offsetof(TCCState, warn_stringop_overflow), WD_ALL, "stringop-overflow"},
    {offsetof(TCCState, warn_format_overflow), WD_ALL, "format-overflow"},
    {offsetof(TCCState, warn_format_truncation), WD_ALL, "format-truncation"},
    {offsetof(TCCState, warn_alloc_size_larger_than), WD_ALL, "alloc-size-larger-than"},
    /* Not in -Wall (no gcc counterpart): malloc(n * sizeof *p) is everywhere,
       and the check cannot see a test of n before it. */
    {offsetof(TCCState, warn_alloc_mul_overflow), WD_SAFE, "alloc-mul-overflow"},
    {offsetof(TCCState, warn_free_nonheap_object), WD_ALL, "free-nonheap-object"},
    {offsetof(TCCState, warn_mismatched_dealloc), WD_ALL, "mismatched-dealloc"},
    {offsetof(TCCState, warn_use_after_free), WD_ALL, "use-after-free"},
    {0, 0, NULL}};

/* the -W name of the option at WNUM (TCCState.warn_num), for the
   machine-readable diagnostics' ruleId / option */
static const char *warn_option_name(int wnum)
{
  for (size_t i = 0; i < sizeof options_W / sizeof options_W[0]; i++)
    if (options_W[i].offset - offsetof(TCCState, warn_none) == (size_t)wnum)
      return options_W[i].name;
  return NULL;
}


typedef char options_W_fit_warn_user[sizeof options_W / sizeof options_W[0] - 1 <= 32 * 4 ? 1 : -1];

/* Safe by default: unless -ccompat (or an explicit -std=c.. / gnu..), the
   yasc dialect, -Wall, the WD_SAFE options, and the WD_SAFE_ERR ones as
   errors.  An option the command line set keeps what it said, wherever it
   came. */
static void tcc_safe_defaults(TCCState *s)
{
  TCCState *s1 = s; /* tcc_warning's */
  const FlagDef *p;

  if (s->ccompat)
    return;
  s->yasc_std = 1;
  s->cversion = 202311;
  s->strict_lifetime = 1;
  for (p = options_W; p->name; ++p)
  {
    unsigned char *f = (unsigned char *)s + p->offset;
    /* strict lifetimes: errors, whatever the command line said */
    if (p->flags & WD_STRICT)
    {
      if (warn_user_test(s, p - options_W) && (!(*f & WARN_ON) || (*f & WARN_NOE)))
        tcc_warning("-W%s is an error in yasc and cannot be lowered: use unsafe { } or -ccompat", p->name);
      *f = WARN_ON | WARN_ERR;
      continue;
    }
    if (warn_user_test(s, p - options_W))
    {
      /* -Wno-error=X keeps X on as a warning, as it does for one on by default */
      if ((*f & WARN_NOE) && (p->flags & (WD_ALL | WD_SAFE | WD_SAFE_ERR)))
        *f |= WARN_ON;
      continue;
    }
    if (p->flags & (WD_ALL | WD_SAFE))
      *f |= WARN_ON;
    if (p->flags & WD_SAFE_ERR)
      *f = WARN_ON | WARN_ERR;
  }
}

/* As gcc: -Wconversion implies -Wsign-conversion, unless the command line
   said something about that one; -Wno-extra turns off the unused
   parameters. */
static void tcc_implied_warnings(TCCState *s)
{
  int no_extra = 0;
  for (const FlagDef *p = options_W; p->name; ++p)
    if (p->offset == offsetof(TCCState, warn_extra))
      no_extra = warn_user_test(s, p - options_W) && !(s->warn_extra & WARN_ON);
  for (const FlagDef *p = options_W; p->name; ++p)
  {
    if (warn_user_test(s, p - options_W))
      continue;
    /* -Wno-extra: the unused parameters go too (the safe default has them) */
    if ((p->flags & WD_EXTRA_UNUSED) && no_extra)
      *((unsigned char *)s + p->offset) = 0;
    if (p->offset == offsetof(TCCState, warn_sign_conversion) && (s->warn_conversion & WARN_ON))
      s->warn_sign_conversion = s->warn_conversion;
    /* -Wextra with -Wunused or -Wall: the unused parameters, wherever the
       options came */
    if ((p->flags & WD_EXTRA_UNUSED) && (s->warn_extra & WARN_ON) && ((s->warn_unused | s->warn_all) & WARN_ON))
      *((unsigned char *)s + p->offset) = s->warn_extra;
  }
  unused_options_changed(s);
}

/* -fsanitize=LIST / -fno-sanitize=LIST: gcc's names (ubsan.c).  A check
   traps (`udf #254`) unless the handler mode is asked for (sanitize_mode).
   LENIENT: a name not known here is skipped (the mode options' lists may
   name other sanitizers), else an error. */
static int sanitize_names(TCCState *s1, const char *list, unsigned *bits, int lenient)
{
  static const struct
  {
    const char *name;
    unsigned short bits;
  } names[] = {{"undefined", SAN_UNDEFINED},
               {"signed-integer-overflow", SAN_SIGNED_OVERFLOW},
               {"shift", SAN_SHIFT_BASE | SAN_SHIFT_EXP},
               {"shift-base", SAN_SHIFT_BASE},
               {"shift-exponent", SAN_SHIFT_EXP},
               {"integer-divide-by-zero", SAN_DIV_ZERO},
               {"null", SAN_NULL},
               {"alignment", SAN_ALIGN},
               {"vla-bound", SAN_VLA},
               {"bounds", SAN_BOUNDS},
               {"array-bounds", SAN_BOUNDS},
               {"unreachable", SAN_UNREACHABLE},
               {"all", SAN_UNDEFINED}};
  *bits = 0;
  while (*list)
  {
    const char *e = strchr(list, ',');
    size_t len = e ? (size_t)(e - list) : strlen(list);
    size_t i;
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
      if (strlen(names[i].name) == len && !strncmp(names[i].name, list, len))
        break;
    if (i < sizeof names / sizeof names[0])
      *bits |= names[i].bits;
    else if (!lenient)
      return tcc_error_noabort("-fsanitize=%.*s is not supported (only the undefined-behaviour checks)", (int)len,
                               list);
    list += len + (e != NULL);
  }
  return 0;
}

/* 1: a runtime-check option, handled; 0: not one; -1: an error */
static int tcc_runtime_check_option(TCCState *s, const char *opt)
{
  unsigned bits;
  if (strstart("sanitize=", &opt))
  {
    if (sanitize_names(s, opt, &bits, 0) < 0)
      return -1;
    s->sanitize |= bits;
    return 1;
  }
  if (strstart("no-sanitize=", &opt))
  {
    if (sanitize_names(s, opt, &bits, 0) < 0)
      return -1;
    s->sanitize &= ~bits;
    return 1;
  }
  /* The mode of the checks (gcc's and clang's options, without a list: all).
     Trap stays the default: tcc ships no UBSan runtime of its own, and a
     trap needs none.  Asking for the runtime's behaviour picks the handler
     mode: -fno-sanitize-trap, -fsanitize-recover (report and go on) or
     -fno-sanitize-recover (report and abort, the _abort handlers).
     -fsanitize-trap wins over the other two, in any order, as clang's. */
  {
    static const struct
    {
      const char *name;
      unsigned char what;
    } modes[] = {{"sanitize-trap", 1},       {"no-sanitize-trap", 2},  {"sanitize-recover", 3},
                 {"no-sanitize-recover", 4}, {"sanitize-undefined-trap-on-error", 1}};
    for (size_t m = 0; m < sizeof modes / sizeof modes[0]; m++)
    {
      size_t n = strlen(modes[m].name);
      if (strncmp(opt, modes[m].name, n) || (opt[n] && (opt[n] != '=' || m == 4)))
        continue;
      bits = SAN_UNDEFINED;
      if (opt[n] == '=')
        sanitize_names(s, opt + n + 1, &bits, 1);
      bits &= SAN_UNDEFINED;
      switch (modes[m].what)
      {
      case 1:
        s->sanitize_trap |= bits;
        break;
      case 2:
        s->sanitize_trap &= ~bits;
        s->sanitize_handler |= bits;
        break;
      case 3:
        s->sanitize_handler |= bits;
        s->sanitize_abort &= ~bits;
        break;
      default:
        s->sanitize_handler |= bits;
        s->sanitize_abort |= bits;
        break;
      }
      return 1;
    }
  }
  if (!strcmp(opt, "trapv") || !strcmp(opt, "no-trapv"))
  {
    if (opt[0] == 'n')
      s->sanitize &= ~SAN_TRAPV;
    else
      s->sanitize |= SAN_TRAPV;
    return 1;
  }
  if (!strcmp(opt, "stack-protector"))
    s->stack_protector = SSP_PLAIN;
  else if (!strcmp(opt, "stack-protector-strong"))
    s->stack_protector = SSP_STRONG;
  else if (!strcmp(opt, "stack-protector-all"))
    s->stack_protector = SSP_ALL;
  else if (!strcmp(opt, "stack-protector-explicit"))
    s->stack_protector = SSP_EXPLICIT;
  else if (!strcmp(opt, "no-stack-protector"))
    s->stack_protector = 0;
  else
    return 0;
  return 1;
}

static const FlagDef options_f[] = {{offsetof(TCCState, char_is_unsigned), 0, "unsigned-char"},
                                    {offsetof(TCCState, char_is_unsigned), FD_INVERT, "signed-char"},
                                    {offsetof(TCCState, nocommon), FD_INVERT, "common"},
                                    {offsetof(TCCState, leading_underscore), 0, "leading-underscore"},
                                    {offsetof(TCCState, ms_extensions), 0, "ms-extensions"},
                                    {offsetof(TCCState, dollars_in_identifiers), 0, "dollars-in-identifiers"},
                                    {offsetof(TCCState, test_coverage), 0, "test-coverage"},
                                    {offsetof(TCCState, reverse_funcargs), 0, "reverse-funcargs"},
                                    {offsetof(TCCState, gnu89_inline), 0, "gnu89-inline"},
                                    {offsetof(TCCState, unwind_tables), 0, "asynchronous-unwind-tables"},
                                    {offsetof(TCCState, function_sections), 0, "function-sections"},
                                    {offsetof(TCCState, data_sections), 0, "data-sections"},
                                    {offsetof(TCCState, internal_spill), 0, "internal-spill"},
                                    {offsetof(TCCState, syntax_only), 0, "syntax-only"},
                                    {offsetof(TCCState, unsafe_report), 0, "unsafe-report"},
                                    {offsetof(TCCState, yasc_facts), 0, "yasc-facts"},
                                    {offsetof(TCCState, fortify), 0, "fortify"},
                                    /* IR optimization flags */
                                    {offsetof(TCCState, opt_dce), 0, "dce"},
                                    {offsetof(TCCState, opt_const_prop), 0, "const-prop"},
                                    {offsetof(TCCState, opt_copy_prop), 0, "copy-prop"},
                                    {offsetof(TCCState, opt_cse), 0, "cse"},
                                    {offsetof(TCCState, opt_bool_idempotent), 0, "bool-idempotent"},
                                    {offsetof(TCCState, opt_bool_simplify), 0, "bool-simplify"},
                                    {offsetof(TCCState, opt_store_load_fwd), 0, "store-load-fwd"},
                                    {offsetof(TCCState, opt_redundant_store), 0, "redundant-store-elim"},
                                    {offsetof(TCCState, opt_dead_store), 0, "dead-store-elim"},
                                    {offsetof(TCCState, opt_fp_offset_cache), 0, "fp-offset-cache"},
                                    {offsetof(TCCState, opt_indexed_memory), 0, "indexed-memory"},
                                    {offsetof(TCCState, opt_disp_fusion), 0, "disp-fusion"},
                                    {offsetof(TCCState, opt_lea_fold), 0, "lea-fold"},
                                    {offsetof(TCCState, opt_mla_fusion), 0, "mla-fusion"},
                                    {offsetof(TCCState, opt_stack_addr_cse), 0, "stack-addr-cse"},
                                    {offsetof(TCCState, opt_licm), 0, "licm"},
                                    {offsetof(TCCState, opt_strength_red), 0, "strength-red"},
                                    {offsetof(TCCState, opt_iv_strength_red), 0, "iv-strength-red"},
                                    {offsetof(TCCState, opt_loop_unroll), 0, "loop-unroll"},
                                    {offsetof(TCCState, opt_reroll), 0, "reroll-blocks"},
                                    {offsetof(TCCState, opt_peephole), 0, "peephole"},
                                    {offsetof(TCCState, opt_jump_threading), 0, "jump-threading"},
                                    {offsetof(TCCState, opt_vrp), 0, "vrp"},
                                    {offsetof(TCCState, opt_float_narrow), 0, "float-narrow"},
                                    {offsetof(TCCState, opt_inline_functions), 0, "inline-functions"},
                                    {offsetof(TCCState, opt_inline_small), 0, "inline-small-functions"},
                                    {offsetof(TCCState, opt_inline_called_once), 0, "inline-functions-called-once"},
                                    {offsetof(TCCState, opt_drop_unused_statics), 0, "drop-unused-statics"},
                                    {offsetof(TCCState, instrument_functions), 0, "instrument-functions"},
                                    {offsetof(TCCState, module_local_calls), 0, "module-local-calls"},
                                    {0, 0, NULL}};

static const FlagDef options_m[] = {{offsetof(TCCState, ms_bitfields), 0, "ms-bitfields"},
                                    {offsetof(TCCState, inline_atomics), 0, "inline-atomics"},
                                    {0, 0, NULL}};

/* WNUM (an option's offset from warn_none) is a strict lifetime error in this
   unit: -w does not hide it */
static int diag_is_strict(TCCState *s1, int wnum)
{
  if (!s1->strict_lifetime || wnum <= 0)
    return 0;
  for (const FlagDef *p = options_W; p->name; ++p)
    if ((int)(p->offset - offsetof(TCCState, warn_none)) == wnum)
      return (p->flags & WD_STRICT) != 0;
  return 0;
}

/* a -W option by name, for #pragma diagnostic */
static const struct FlagDef *diag_option(const char *name)
{
  /* "-Wframe-larger-than=" / "-Wstack-usage=N": the option without its limit */
  const char *eq = strchr(name, '=');
  const size_t len = eq ? (size_t)(eq - name) : strlen(name);
  for (const FlagDef *p = options_W; p->name; ++p)
    if (!strncmp(p->name, name, len) && !p->name[len])
      return p;
  return NULL;
}

static void unsafe_live(TCCState *s1, int what);

ST_FUNC void tcc_pragma_diagnostic(TCCState *s1, int action, const char *option, const char *file, int line)
{
  if (!s1->nb_diag_snaps)
    diag_record(s1, NULL, 0); /* the command line's, before the first change */
  if (action == DIAG_PRAGMA_PUSH)
  {
    if (!(s1->nb_diag_stack & (s1->nb_diag_stack - 1)))
      s1->diag_stack = tcc_realloc(s1->diag_stack, (s1->nb_diag_stack ? 2 * s1->nb_diag_stack : 4) * sizeof(DiagSnap));
    memcpy(s1->diag_stack[s1->nb_diag_stack++].w, &s1->warn_none, DIAG_W_SIZE);
    return;
  }
  if (action == DIAG_PRAGMA_POP)
  {
    if (!s1->nb_diag_stack)
    {
      tcc_warning("#pragma diagnostic pop without a push");
      return;
    }
    memcpy(&s1->warn_none, s1->diag_stack[--s1->nb_diag_stack].w, DIAG_W_SIZE);
    /* the strict options are what the region says, not what the push saw: a
       push inside unsafe { } saved them off */
    if (s1->unsafe_depth)
      unsafe_live(s1, 2);
    else if (s1->strict_lifetime)
      for (const FlagDef *p = options_W; p->name; ++p)
        if (p->flags & WD_STRICT)
          *((unsigned char *)s1 + p->offset) = WARN_ON | WARN_ERR;
  }
  else
  {
    const struct FlagDef *p;
    int all, value;
    if (!option || option[0] != '-' || option[1] != 'W')
    {
      tcc_warning_c(warn_unsupported)("#pragma diagnostic: '%s' is not a -W option", option ? option : "");
      return;
    }
    option += 2;
    all = !strcmp(option, "all") ? WD_ALL : !strcmp(option, "extra") ? WD_EXTRA : 0;
    p = all ? NULL : diag_option(option);
    if (!all && !p)
    {
      /* gcc's names tcc does not have: nothing to switch */
      tcc_warning_c(warn_unsupported)("#pragma diagnostic: unknown warning option '-W%s'", option);
      return;
    }
    value = action == DIAG_PRAGMA_IGNORED ? 0 : action == DIAG_PRAGMA_ERROR ? WARN_ON | WARN_ERR : WARN_ON | WARN_NOE;
    if (s1->strict_lifetime && value != (WARN_ON | WARN_ERR) && (all ? 0 : (p->flags & WD_STRICT)))
    {
      tcc_warning("#pragma diagnostic: -W%s is an error in yasc and cannot be lowered: use unsafe { }", p->name);
      return;
    }
    const int group = !all && !strcmp(option, "unused");
    for (const struct FlagDef *q = all || group ? options_W : p; q->name; ++q)
    {
      if (!all && !group && q != p)
        break;
      if (all && q->offset != (all == WD_ALL ? offsetof(TCCState, warn_all) : offsetof(TCCState, warn_extra)) &&
          !(q->flags & all) &&
          !(all == WD_EXTRA && (q->flags & WD_EXTRA_UNUSED) && ((s1->warn_unused | s1->warn_all) & WARN_ON)))
        continue;
      if (group && !(q->flags & WD_UNUSED))
        continue;
      if (s1->strict_lifetime && (q->flags & WD_STRICT))
        continue; /* an error outside unsafe { }, off inside: no group moves it */
      *((unsigned char *)s1 + q->offset) = value;
    }
    unused_options_changed(s1);
    s1->nb_diag_pragmas++;
    if ((s1->warn_tautological_compare | s1->warn_sizeof_pointer_memaccess) & WARN_ON)
      fe_tokfp_on = 1; /* the operand hash from here on */
  }
  diag_record(s1, file, line);
}

/* yasc `unsafe { ... }` / `unsafe(expr)`: the way past strict lifetimes.
   -Wlifetime and -Wimplicit-move (and the yasc ownership rules said under
   it) are off from LINE of FILE to the region's end -- now, for what the
   parse says, and recorded by line for the flow checks, which report after
   the body.  By lines: what else is on the first and the last line goes with
   it.  The records carry the options the file has at those lines, not the
   live ones: a body is parsed later at -O1+ (diag_state_at takes the closest
   record at or before a line). */
static void diag_w_strict_off(unsigned char *w)
{
  for (const FlagDef *p = options_W; p->name; ++p)
    if (p->flags & WD_STRICT)
      w[p->offset - offsetof(TCCState, warn_none)] = 0;
}

static void unsafe_record(TCCState *s1, const char *file, int line, int off)
{
  unsigned char w[DIAG_SNAP_SIZE];
  const int kind = off ? DIAG_UNSAFE_OFF : DIAG_UNSAFE_ON;
  memcpy(w, diag_state_at_ex(s1, file, line, 1), DIAG_W_SIZE);
  if (off)
    diag_w_strict_off(w);
  /* the same edge again (the inliner replays the body at each call site):
     one record, or the records grow with the calls */
  const char *name = tcc_filename_intern(file);
  for (int i = s1->nb_diag_snaps; i-- > 0;)
  {
    DiagSnap *d = &s1->diag_snaps[i];
    if (d->unsafe == kind && d->line == line && d->file == name)
    {
      memmove(d->w, w, DIAG_W_SIZE);
      return;
    }
  }
  diag_record_w(s1, file, line, w)->unsafe = kind;
}

/* the strict options (WD_STRICT), saved / restored / turned off now */
static void unsafe_live(TCCState *s1, int what)
{
  int k = 0;
  for (const FlagDef *p = options_W; p->name; ++p)
    if ((p->flags & WD_STRICT) && k < (int)sizeof s1->unsafe_saved)
    {
      unsigned char *f = (unsigned char *)s1 + p->offset;
      if (what == 0)
        s1->unsafe_saved[k] = *f;
      else if (what == 1)
        *f = s1->unsafe_saved[k];
      else
        *f = 0;
      k++;
    }
}

ST_FUNC void tcc_unsafe_begin(TCCState *s1, const char *file, int line)
{
  if (!s1->nb_diag_snaps)
    diag_record(s1, NULL, 0); /* the command line's, before the first change */
  if (!s1->unsafe_depth++)
    unsafe_live(s1, 0);
  unsafe_live(s1, 2);
  if (file && line)
  {
    unsafe_record(s1, file, line, 1);
    /* -funsafe-report: each region once, however often its body is parsed */
    if (s1->unsafe_report &&
        diag_once_set(s1, diag_once_hash(0x5afe, 0, tcc_filename_intern(file), line, NULL), 1))
      fprintf(stderr, "%s:%d: note: unsafe region\n", file, line);
  }
}

/* LINE: the first line after the region */
ST_FUNC void tcc_unsafe_end(TCCState *s1, const char *file, int line)
{
  if (!s1->unsafe_depth)
    return;
  if (!--s1->unsafe_depth)
    unsafe_live(s1, 1);
  if (file && line)
    unsafe_record(s1, file, line, s1->unsafe_depth > 0);
}

/* an error recovered from inside a region: its end was not parsed, so the
   region ends where the parse goes on (FILE, LINE) */
ST_FUNC void tcc_unsafe_reset(TCCState *s1, const char *file, int line)
{
  if (!s1->unsafe_depth)
    return;
  unsafe_live(s1, 1);
  s1->unsafe_depth = 0;
  if (file && line)
    unsafe_record(s1, file, line, 0);
}

/* -Wframe-larger-than=N, -Wstack-usage=N (also after -Werror=), and their
   -Wno- forms, which drop the limit.  1: handled, 0: not one of them, -1: a
   bad limit.  -Werror=frame-larger-than without "=N" goes to set_flag. */
static int set_limit_flag(TCCState *s1, const char *name)
{
  TCCState *s = s1;
  static const char *const names[] = {"frame-larger-than", "stack-usage"};
  const char *r = name;
  int no = strstart("no-", &r), err = !no && strstart("error=", &r);
  for (int i = 0; i < 2; i++)
  {
    const char *v = r;
    if (!strstart(names[i], &v) || (*v && *v != '='))
      continue;
    unsigned long long *limit = i ? &s->stack_usage_limit : &s->frame_larger_than;
    unsigned char *set = i ? &s->stack_usage_limit_set : &s->frame_larger_than_set;
    unsigned char *w = i ? &s->warn_stack_usage : &s->warn_frame_larger_than;
    if (no)
    {
      if (*v)
        return 0;
      *set = 0, *w = 0;
    }
    else if (!*v)
    {
      if (!err)
      {
        tcc_error_noabort("missing argument to '-W%s=': a size in bytes", r);
        return -1;
      }
      return 0;
    }
    else
    {
      char *end;
      *limit = strtoull(v + 1, &end, 10);
      if (!v[1] || *end)
      {
        tcc_error_noabort("invalid size in '-W%s'", name);
        return -1;
      }
      *set = 1;
      *w = err ? WARN_ON | WARN_ERR : (*w & WARN_ERR) | WARN_ON;
    }
    warn_user_set(s, (int)(diag_option(names[i]) - options_W));
    return 1;
  }
  return 0;
}

/* -Walloc-size-larger-than=N (a size; SIZE_MAX or more turns it off, as
   with gcc's "byte-size"), -Wstringop-overflow=N, -Wformat-overflow=N,
   -Wformat-truncation=N and -Warray-bounds=N (levels: 0 turns the warning
   off).  1: handled, 0: not
   one of them, -1: a bad value. */
static int set_level_flag(TCCState *s1, const char *name)
{
  TCCState *s = s1;
  static const char *const names[] = {"alloc-size-larger-than", "stringop-overflow", "format-overflow",
                                      "format-truncation", "array-bounds"};
  const char *r = name;
  int err = strstart("error=", &r);
  /* plain -Warray-bounds is level 1, as in gcc (set_flag does the rest) */
  if (!err && !strcmp(r, "array-bounds"))
    s->array_bounds_level = 1;
  for (int i = 0; i < 5; i++)
  {
    const char *v = r;
    char *end;
    unsigned long long n;
    unsigned char *w;
    if (!strstart(names[i], &v) || *v != '=')
      continue;
    n = strtoull(v + 1, &end, 10);
    if (!v[1] || *end || (i && n > (i == 4 ? 2 : 4)))
    {
      tcc_error_noabort("invalid argument in '-W%s'", name);
      return -1;
    }
    w = i == 0 ? &s->warn_alloc_size_larger_than : i == 1 ? &s->warn_stringop_overflow
        : i == 2 ? &s->warn_format_overflow : i == 3 ? &s->warn_format_truncation : &s->warn_array_bounds;
    if (i == 0)
      s->alloc_size_limit = n, s->alloc_size_limit_set = 1;
    else if (i == 1)
      s->stringop_level = n;
    else if (i == 2)
      s->format_overflow_level = n;
    else if (i == 3)
      s->format_truncation_level = n;
    else
      s->array_bounds_level = n;
    if (i ? !n : n >= 0xffffffffull)
      *w = 0;
    else
      *w = err ? WARN_ON | WARN_ERR : (*w & WARN_ERR) | WARN_ON;
    warn_user_set(s, (int)(diag_option(names[i]) - options_W));
    return 1;
  }
  return 0;
}

static int set_flag(TCCState *s, const FlagDef *flags, const char *name)
{
  int value, mask, ret;
  const FlagDef *p;
  const char *r;
  unsigned char *f;

  r = name, value = !strstart("no-", &r), mask = 0;

  /* when called with options_W, look for -W[no-]error=<option> */
  if ((flags->flags & WD_ALL) && strstart("error=", &r))
    value = value ? WARN_ON | WARN_ERR : WARN_NOE, mask = WARN_ON;

  /* -Wall and -Wextra: the entry, then the group's members after it */
  int group = 0;
  for (ret = -1, p = flags; p->name; ++p)
  {
    if (ret)
    {
      if (strcmp(r, p->name))
        continue;
    }
    else
    {
      if (0 == (p->flags & group))
        continue;
    }

    f = (unsigned char *)s + p->offset;
    *f = (*f & mask) | (value ^ !!(p->flags & FD_INVERT));
    if (flags == options_W)
      warn_user_set(s, p - flags); /* tcc_safe_defaults leaves it */

    if (ret)
    {
      ret = 0;
      group = flags != options_W ? 0 : !strcmp(r, "all") ? WD_ALL : !strcmp(r, "extra") ? WD_EXTRA : 0;
      if (!group)
        break;
    }
  }
  /* -W[no-][error=]unused: the group */
  if (!ret && flags == options_W && !strcmp(r, "unused"))
    for (p = flags; p->name; ++p)
      if (p->flags & WD_UNUSED)
      {
        f = (unsigned char *)s + p->offset;
        *f = (*f & mask) | value;
        warn_user_set(s, p - flags);
      }
  if (!ret && flags == options_W)
    unused_options_changed(s);
  return ret;
}

#define TCC_STR_(x) #x
#define TCC_STR(x) TCC_STR_(x)
/* The flags that give a rebuilt compiler this one's defaults: configure asks
 * for them on YasOS, where they depend on the board the rootfs was built for. */
static const char dumpdefaults_str[] = ""
#ifdef CONFIG_TCC_DEFAULT_FPU
    "-DCONFIG_TCC_DEFAULT_FPU=" TCC_STR(CONFIG_TCC_DEFAULT_FPU) " "
#endif
#ifdef CONFIG_TCC_DEFAULT_FP_LIB
    "-DCONFIG_TCC_DEFAULT_FP_LIB=" TCC_STR(CONFIG_TCC_DEFAULT_FP_LIB) " "
#endif
    ;

static const char dumpmachine_str[] =
/* this is a best guess, please refine as necessary */
#if defined TCC_TARGET_ARM_THUMB
    "armv8m"
#if defined TCC_TARGET_YasOS
    "-yasos"
#endif
#endif
    ;

static int args_parser_make_argv(const char *r, int *argc, char ***argv)
{
  int ret = 0, q, c;
  CString str;
  for (;;)
  {
    while (c = (unsigned char)*r, c && c <= ' ')
      ++r;
    if (c == 0)
      break;
    q = 0;
    cstr_new(&str);
    while (c = (unsigned char)*r, c)
    {
      ++r;
      if (c == '\\' && (*r == '"' || *r == '\\'))
      {
        c = *r++;
      }
      else if (c == '"')
      {
        q = !q;
        continue;
      }
      else if (q == 0 && c <= ' ')
      {
        break;
      }
      cstr_ccat(&str, c);
    }
    cstr_ccat(&str, 0);
    dynarray_add(argv, argc, tcc_strdup(str.data));
    cstr_free(&str);
    ++ret;
  }
  return ret;
}

/* read list file */
static int args_parser_listfile(TCCState *s, const char *filename, int optind, int *pargc, char ***pargv)
{
  TCCState *s1 = s;
  int fd, i;
  char *p;
  int argc = 0;
  char **argv = NULL;

  fd = open(filename, O_RDONLY | O_BINARY);
  if (fd < 0)
    return tcc_error_noabort("listfile '%s' not found", filename);

  p = tcc_load_text(fd);
  for (i = 0; i < *pargc; ++i)
    if (i == optind)
      args_parser_make_argv(p, &argc, &argv);
    else
      dynarray_add(&argv, &argc, tcc_strdup((*pargv)[i]));

  tcc_free(p);
  dynarray_reset(&s->argv, &s->argc);
  *pargc = s->argc = argc, *pargv = s->argv = argv;
  return 0;
}

PUB_FUNC int tcc_parse_args(TCCState *s, int *pargc, char ***pargv, int optind)
{
  TCCState *s1 = s;
  const TCCOption *popt;
  const char *optarg, *r;
  const char *run = NULL;
  int x;
  int tool = 0, arg_start = 0, noaction = optind;
  char **argv = *pargv;
  int argc = *pargc;

  cstr_reset(&s->linker_arg);

  while (optind < argc)
  {
    r = argv[optind];
    if (r[0] == '@' && r[1] != '\0')
    {
      if (args_parser_listfile(s, r + 1, optind, &argc, &argv))
        return -1;
      continue;
    }
    optind++;
    if (tool)
    {
      if (r[0] == '-' && r[1] == 'v' && r[2] == 0)
        ++s->verbose;
      continue;
    }
  reparse:
    if (r[0] != '-' || r[1] == '\0')
    {
      args_parser_add_file(s, r, s->filetype);
      if (run)
      {
      dorun:
        if (tcc_set_options(s, run))
          return -1;
        arg_start = optind - 1;
        break;
      }
      continue;
    }

    /* allow "tcc files... -run -- args ..." */
    if (r[1] == '-' && r[2] == '\0' && run)
      goto dorun;

    /* find option in table */
    for (popt = tcc_options;; ++popt)
    {
      const char *p1 = popt->name;
      const char *r1 = r + 1;
      if (p1 == NULL)
        return tcc_error_noabort("invalid option -- '%s'", r);
      if (!strstart(p1, &r1))
        continue;
      optarg = r1;
      if (popt->flags & TCC_OPTION_HAS_ARG)
      {
        if (*r1 == '\0' && !(popt->flags & TCC_OPTION_NOSEP))
        {
          if (optind >= argc)
          arg_err:
            return tcc_error_noabort("argument to '%s' is missing", r);
          optarg = argv[optind++];
        }
      }
      else if (*r1 != '\0')
        continue;
      break;
    }

    switch (popt->index)
    {
    case TCC_OPTION_HELP:
      x = OPT_HELP;
      goto extra_action;
    case TCC_OPTION_HELP2:
      x = OPT_HELP2;
      goto extra_action;
    case TCC_OPTION_I:
      tcc_add_include_path(s, optarg);
      break;
    case TCC_OPTION_D:
      tcc_define_symbol(s, optarg, NULL);
      break;
    case TCC_OPTION_U:
      tcc_undefine_symbol(s, optarg);
      break;
    case TCC_OPTION_L:
      tcc_add_library_path(s, optarg);
      break;
    case TCC_OPTION_B:
      /* set tcc utilities path (mainly for tcc development) */
      tcc_set_lib_path(s, optarg);
      ++noaction;
      break;
    case TCC_OPTION_l:
      args_parser_add_file(s, optarg, AFF_TYPE_LIB | (s->filetype & ~AFF_TYPE_MASK));
      s->nb_libraries++;
      break;
    case TCC_OPTION_pthread:
      s->option_pthread = 1;
      break;
    case TCC_OPTION_bench:
      s->do_bench = 1;
      break;
#ifdef CONFIG_TCC_BCHECK
    case TCC_OPTION_b: /* the armv8m cross only (tcc.h); runtime lib/bcheck-bm.c */
      s->do_bounds_check = 1;
      break;
#endif
    case TCC_OPTION_g:
      s->do_debug = 2;
      s->dwarf = CONFIG_DWARF_VERSION;
      if (strstart("dwarf", &optarg))
      {
        s->dwarf = (*optarg) ? (0 - atoi(optarg)) : DEFAULT_DWARF_VERSION;
      }
      else if (isnum(*optarg))
      {
        x = *optarg - '0';
        /* -g0 = no info, -g1 = lines/functions only, -g2 = full info */
        s->do_debug = x > 2 ? 2 : x == 0 && s->do_backtrace ? 1 : x;
      }
      break;
    case TCC_OPTION_c:
      x = TCC_OUTPUT_OBJ;
    set_output_type:
      if (s->output_type)
        tcc_warning("-%s: overriding compiler action already specified", popt->name);
      s->output_type = x;
      break;
    case TCC_OPTION_d:
      if (*optarg == 'D')
        s->dflag = 3;
      else if (*optarg == 'M')
        s->dflag = 7;
      else if (*optarg == 't')
        s->dflag = 16;
      else if (isnum(*optarg))
        s->g_debug |= atoi(optarg);
      else
        goto unsupported_option;
      break;
    case TCC_OPTION_static:
      s->static_link = 1;
      break;
    case TCC_OPTION_ccompat:
      /* regular C: none of the safe defaults (tcc_safe_defaults) */
      s->ccompat = 1;
      break;
    case TCC_OPTION_std:
      if (strcmp(optarg, "=c11") == 0 || strcmp(optarg, "=gnu11") == 0)
        s->cversion = 201112;
      else if (strcmp(optarg, "=c17") == 0 || strcmp(optarg, "=gnu17") == 0 || strcmp(optarg, "=c18") == 0 ||
               strcmp(optarg, "=gnu18") == 0)
        s->cversion = 201710;
      else if (strcmp(optarg, "=c23") == 0 || strcmp(optarg, "=gnu23") == 0 || strcmp(optarg, "=c2x") == 0 ||
               strcmp(optarg, "=gnu2x") == 0)
        s->cversion = 202311;
      else if (strcmp(optarg, "=yasc") == 0)
        /* C23 with the yasc ownership keywords (owned view maybe out drop) */
        s->cversion = 202311, s->yasc_std = 1;
      /* asking for a C standard means regular C: -ccompat */
      if (strcmp(optarg, "=yasc") != 0)
        s->ccompat = 1;
      break;
    case TCC_OPTION_shared:
      x = TCC_OUTPUT_DLL;
      goto set_output_type;
    case TCC_OPTION_soname:
      s->soname = tcc_strdup(optarg);
      break;
    case TCC_OPTION_o:
      if (s->outfile)
      {
        tcc_warning("multiple -o option");
        tcc_free(s->outfile);
      }
      s->outfile = tcc_strdup(optarg);
      break;
    case TCC_OPTION_r:
      /* generate a .o merging several output files */
      s->option_r = 1;
      x = TCC_OUTPUT_OBJ;
      goto set_output_type;
    case TCC_OPTION_isystem:
      tcc_add_sysinclude_path(s, optarg);
      break;
    case TCC_OPTION_iyasc:
      tcc_add_yasc_include_path(s, optarg);
      break;
    case TCC_OPTION_include:
      cstr_printf(&s->cmdline_incl, "#include \"%s\"\n", optarg);
      break;
    case TCC_OPTION_nostdinc:
      s->nostdinc = 1;
      break;
    case TCC_OPTION_nostdlib:
      s->nostdlib = 1;
      break;
    case TCC_OPTION_nodefaultlibs:
      s->nodefaultlibs = 1;
      break;
    case TCC_OPTION_v:
      do
        ++s->verbose;
      while (*optarg++ == 'v');
      ++noaction;
      break;
    case TCC_OPTION_f:
      /* Handle -finline-limit=N */
      if (!strncmp(optarg, "inline-limit=", 13))
      {
        int n = atoi(optarg + 13);
        if (n > 0)
        {
          s->opt_inline_limit = n;
          /* Latch that the limit came from the command line: the -O handlers
           * below must not override it, whichever order the two appear in. */
          s->opt_inline_limit_user = 1;
        }
        break;
      }
      /* -fmax-errors=N: stop after the Nth error (0: never), as gcc */
      if (strstart("max-errors=", &optarg))
      {
        char *end;
        long n = strtol(optarg, &end, 10);
        if (end == optarg || *end || n < 0)
          return tcc_error_noabort("invalid argument to -fmax-errors=: '%s'", optarg);
        s->max_errors = n > 0x7fffffff ? 0x7fffffff : (int)n;
        break;
      }
      /* -fmodule-deps: print each file's `module` and `import`s, compile
         nothing (decl/module.c yasc_module_scan) */
      if (!strcmp(optarg, "module-deps"))
      {
        s->module_deps = 1;
        x = TCC_OUTPUT_PREPROCESS;
        goto set_output_type;
      }
      /* -fmodule-header=DIR: a module's C header, DIR/a/b.h */
      if (strstart("module-header=", &optarg))
      {
        tcc_free(s->module_header_dir);
        s->module_header_dir = tcc_strdup(optarg);
        break;
      }
      /* -fmodule-dir=DIR: yasc module interfaces are written to the first
         and looked up in each (docs/yasc_modules.md) */
      if (strstart("module-dir=", &optarg))
      {
        tcc_split_path(s, &s->module_dirs, &s->nb_module_dirs, optarg);
        break;
      }
      /* -fsanitize=..., -fsanitize-trap, -ftrapv, -fstack-protector* */
      {
        int r = tcc_runtime_check_option(s, optarg);
        if (r < 0)
          return -1;
        if (r)
          break;
      }
      /* -fstack-usage: <object>.su; -fcallgraph-info[=su]: <object>.ci
         (backend/generators/stack_usage.c) */
      if (!strcmp(optarg, "stack-usage") || !strcmp(optarg, "no-stack-usage"))
      {
        s->stack_usage = optarg[0] != 'n';
        break;
      }
      if (!strcmp(optarg, "callgraph-info") || !strcmp(optarg, "callgraph-info=su") ||
          !strcmp(optarg, "no-callgraph-info"))
      {
        s->callgraph_info = optarg[0] != 'n';
        break;
      }
      /* -fvisibility=: the visibility of definitions that name none */
      if (strstart("visibility=", &optarg))
      {
        if (!strcmp(optarg, "default"))
          s->default_visibility = STV_DEFAULT;
        else if (!strcmp(optarg, "hidden"))
          s->default_visibility = STV_HIDDEN;
        else if (!strcmp(optarg, "internal"))
          s->default_visibility = STV_INTERNAL;
        else if (!strcmp(optarg, "protected"))
          s->default_visibility = STV_PROTECTED;
        else
          return tcc_error_noabort("unknown visibility '%s' in -fvisibility=", optarg);
        break;
      }
      /* Plain -fno-builtin: disable all builtin call inlining (mem* expansion). */
      if (!strcmp(optarg, "no-builtin"))
      {
        s->no_builtin_funcs |= NO_BUILTIN_MEMFUNCS | NO_BUILTIN_ALL;
        break;
      }
      /* Handle -fno-builtin-<name> flags */
      if (!strncmp(optarg, "no-builtin-", 11))
      {
        const char *bname = optarg + 11;
        if (!strcmp(bname, "memcpy") || !strcmp(bname, "memset") ||
            !strcmp(bname, "memmove"))
          s->no_builtin_funcs |= NO_BUILTIN_MEMFUNCS;
        else if (!strcmp(bname, "abs"))
          s->no_builtin_funcs |= NO_BUILTIN_ABS;
        else if (!strcmp(bname, "labs"))
          s->no_builtin_funcs |= NO_BUILTIN_LABS;
        else if (!strcmp(bname, "llabs"))
          s->no_builtin_funcs |= NO_BUILTIN_LLABS;
        else if (!strcmp(bname, "uabs"))
          s->no_builtin_funcs |= NO_BUILTIN_UABS;
        else if (!strcmp(bname, "ulabs"))
          s->no_builtin_funcs |= NO_BUILTIN_ULABS;
        else if (!strcmp(bname, "ullabs"))
          s->no_builtin_funcs |= NO_BUILTIN_ULLABS;
        else if (!strcmp(bname, "umaxabs"))
          s->no_builtin_funcs |= NO_BUILTIN_UMAXABS;
        /* Silently accept other -fno-builtin-<name> flags */
        break;
      }
      if (set_diag_color(s, optarg) == 0 || set_diag_format(optarg) == 0)
        break;
      if (set_flag(s, options_f, optarg) < 0)
        goto unsupported_option;
      break;
    case TCC_OPTION_fpic:
    case TCC_OPTION_fpie:
      s->pic = 1;
      break;
    case TCC_OPTION_no_pie:
      s->no_pie = 1;
      break;
    case TCC_OPTION_fno_pic:
      /* Position-dependent code with absolute addressing, for code that is
       * linked at a fixed address (a kernel): no GOT, no r9 data base, and r9
       * becomes an ordinary allocatable register. */
      s->pic = 0;
      s->text_and_data_separation = 0;
      s->sb_relative_got = 0;
      s->share_rodata = 0;
      break;
#if defined(TCC_TARGET_ARM) || defined(TCC_TARGET_ARM_THUMB)
    case TCC_OPTION_mfloat_abi:
      if (!strcmp(optarg, "soft"))
      {
        s->float_abi = ARM_SOFT_FLOAT;
      }
      else if (!strcmp(optarg, "softfp"))
      {
        s->float_abi = ARM_SOFTFP_FLOAT;
      }
      else if (!strcmp(optarg, "hard"))
        s->float_abi = ARM_HARD_FLOAT;
      else
        return tcc_error_noabort("unsupported float abi '%s'", optarg);
      break;
    case TCC_OPTION_mfpu:
      if (!strcmp(optarg, "vfp") || !strcmp(optarg, "vfpv2"))
      {
        s->fpu_type = ARM_FPU_VFP;
      }
      else if (!strcmp(optarg, "vfpv3") || !strcmp(optarg, "vfpv3-d16"))
      {
        s->fpu_type = ARM_FPU_VFPV3;
      }
      else if (!strcmp(optarg, "vfpv4") || !strcmp(optarg, "vfpv4-d16"))
      {
        s->fpu_type = ARM_FPU_VFPV4;
      }
      else if (!strcmp(optarg, "fpv4-sp-d16"))
      {
        s->fpu_type = ARM_FPU_FPV4_SP_D16;
      }
      else if (!strcmp(optarg, "fpv5-sp-d16"))
      {
        s->fpu_type = ARM_FPU_FPV5_SP_D16;
      }
      else if (!strcmp(optarg, "fpv5-d16"))
      {
        s->fpu_type = ARM_FPU_FPV5_D16;
      }
      else if (!strcmp(optarg, "rp2350") || !strcmp(optarg, "rp2350-dcp"))
      {
        /* RP2350: FPv5-SP single-precision FPU plus the Raspberry Pi double
         * coprocessor (DCP) on CP4.  Selects librp2350fp for the runtime
         * calls the compiler does not lower inline. */
        s->fpu_type = ARM_FPU_RP2350;
      }
      else if (!strcmp(optarg, "neon") || !strcmp(optarg, "neon-vfpv3"))
      {
        s->fpu_type = ARM_FPU_NEON;
      }
      else if (!strcmp(optarg, "neon-vfpv4"))
      {
        s->fpu_type = ARM_FPU_NEON_VFPV4;
      }
      else if (!strcmp(optarg, "neon-fp-armv8") || !strcmp(optarg, "crypto-neon-fp-armv8"))
      {
        s->fpu_type = ARM_FPU_NEON_FP_ARMV8;
      }
      else if (!strcmp(optarg, "auto"))
      {
        s->fpu_type = ARM_FPU_AUTO;
      }
      else if (!strcmp(optarg, "none"))
      {
        s->fpu_type = ARM_FPU_NONE;
      }
      else
      {
        return tcc_error_noabort("unsupported FPU type '%s'", optarg);
      }
      break;
    case TCC_OPTION_mfp_inline:
      if (!strcmp(optarg, "auto"))
        s->fp_inline = ARM_FP_INLINE_AUTO;
      else if (!strcmp(optarg, "none"))
        s->fp_inline = ARM_FP_INLINE_NONE;
      else
        return tcc_error_noabort("unsupported -mfp-inline value '%s' (auto, none)", optarg);
      break;
    case TCC_OPTION_mfp_lib:
      if (!strcmp(optarg, "auto"))
        s->fp_lib = ARM_FP_LIB_AUTO;
      else if (!strcmp(optarg, "static"))
        s->fp_lib = ARM_FP_LIB_STATIC;
      else if (!strcmp(optarg, "shared"))
        s->fp_lib = ARM_FP_LIB_SHARED;
      else
        return tcc_error_noabort("unsupported -mfp-lib value '%s' (auto, static, shared)", optarg);
      break;
    case TCC_OPTION_march:
      s->march_str = optarg;
      break;
    case TCC_OPTION_mpic_data_is_text_relative:
      printf("Setting text and data separation to: 1\n");
      s->text_and_data_separation = 1;
      break;
    case TCC_OPTION_slow_memory:
      s->yaff_slow_memory = 1;
      break;
    case TCC_OPTION_stack_size:
      s->yaff_stack_size = (unsigned int)strtoul(optarg, NULL, 0);
      break;
    case TCC_OPTION_heap_size:
      s->yaff_heap_size = (unsigned int)strtoul(optarg, NULL, 0);
      break;
    case TCC_OPTION_sb_relative_got:
      s->sb_relative_got = 1;
      break;
    case TCC_OPTION_no_sb_relative_got:
      s->sb_relative_got = 0;
      break;
    case TCC_OPTION_share_rodata:
      s->share_rodata = 1;
      break;
    case TCC_OPTION_no_share_rodata:
      s->share_rodata = 0;
      break;
#endif
    case TCC_OPTION_m:
      if (set_flag(s, options_m, optarg) < 0)
      {
        if (x = atoi(optarg), x != 32 && x != 64)
          goto unsupported_option;
        if (PTR_SIZE != x / 8)
          return x;
        ++noaction;
      }
      break;
    case TCC_OPTION_W:
      s->warn_none = 0;
      x = set_limit_flag(s, optarg);
      if (!x)
        x = set_level_flag(s, optarg);
      if (x < 0)
        return -1;
      if (!x && optarg[0] && set_flag(s, options_W, optarg) < 0)
        goto unsupported_option;
      break;
    case TCC_OPTION_w:
      s->warn_none = 1;
      break;
    case TCC_OPTION_rdynamic:
      s->rdynamic = 1;
      break;
    case TCC_OPTION_Wl:
      if (s->linker_arg.size)
        ((char *)s->linker_arg.data)[s->linker_arg.size - 1] = ',';
      cstr_cat(&s->linker_arg, optarg, 0);
      x = tcc_set_linker(s, s->linker_arg.data);
      if (x)
        cstr_reset(&s->linker_arg);
      if (x < 0)
        return -1;
      break;
    case TCC_OPTION_Wp:
      r = optarg;
      goto reparse;
    case TCC_OPTION_E:
      x = TCC_OUTPUT_PREPROCESS;
      goto set_output_type;
    case TCC_OPTION_P:
      s->Pflag = atoi(optarg) + 1;
      break;
    case TCC_OPTION_M:
      s->include_sys_deps = 1;
      // fall through
    case TCC_OPTION_MM:
      s->just_deps = 1;
      if (!s->deps_outfile)
        s->deps_outfile = tcc_strdup("-");
      // fall through
    case TCC_OPTION_MMD:
      s->gen_deps = 1;
      break;
    case TCC_OPTION_MD:
      s->gen_deps = 1;
      s->include_sys_deps = 1;
      break;
    case TCC_OPTION_MF:
      s->deps_outfile = tcc_strdup(optarg);
      break;
    case TCC_OPTION_MP:
      s->gen_phony_deps = 1;
      break;
    case TCC_OPTION_dumpdefaults:
      printf("%s\n", dumpdefaults_str);
      exit(0);
    case TCC_OPTION_dumpmachine:
      printf("%s\n", dumpmachine_str);
      exit(0);
    case TCC_OPTION_dumpversion:
      printf("%s\n", TCC_VERSION);
      exit(0);
    case TCC_OPTION_x:
      x = 0;
      if (*optarg == 'c')
        x = AFF_TYPE_C;
      else if (*optarg == 'a')
        x = AFF_TYPE_ASMPP;
      else if (*optarg == 'b')
        x = AFF_TYPE_BIN;
      else if (*optarg == 'n')
        x = AFF_TYPE_NONE;
      else
        tcc_warning("unsupported language '%s'", optarg);
      s->filetype = x | (s->filetype & ~AFF_TYPE_MASK);
      break;
    case TCC_OPTION_O:
      /* -Os / -Oz went through atoi() and came out 0, so they were silently
       * -O0 -- and because -O comes from a plain option scan, a command line
       * of `-O2 ... -Os` (toybox's) reset an explicit -O2 back to nothing.
       *
       * They run the -O2 pipeline and set optimize_size, which the choices
       * that trade code size for speed read: no word-alignment pads on branch
       * targets, and a block copy past the largest copy stub stays a call.
       * -Oz is -Os.  Inlining and unrolling are NOT cut back: on Zig's C both
       * shrink the code (a lower -finline-limit, -fno-loop-unroll and -O1 are
       * all larger), and gcc's own -O2 -> -Os gap there is mostly inlining
       * tcc already does not do. */
      if (optarg[0] == 's' || optarg[0] == 'z')
      {
        s->optimize = 2;
        s->optimize_size = optarg[0] == 'z' ? 2 : 1;
      }
      else
      {
        s->optimize = atoi(optarg);
        s->optimize_size = 0;
      }
#ifdef CONFIG_TCC_O0_ONLY
      /* A note, not a warning: a Makefile's -O2 -Werror still has to build. */
      if (s->optimize)
        fprintf(stderr, "tcc: note: built with --disable-optimizer, -O%s has no effect\n", optarg);
#endif
      /* -O1: scalar cleanup, cheap ARM addressing-mode fusion, constant/range
       * folding, loop rotation, and light inlining of explicitly-inline / tiny
       * functions — the same class of work GCC does at -O1.  The heavy tier
       * (loop unrolling / IV strength reduction / LICM, MUL+ADD→MLA fusion,
       * interprocedural const-prop, re-rolling, and full small-function
       * inlining) is gated to -O2. */
      if (TCC_OPT(s, optimize) >= 1)
      {
        s->opt_dce = 1;
        s->opt_const_prop = 1;
        s->opt_copy_prop = 1;
        s->opt_cse = 1;
        s->opt_bool_idempotent = 1;
        s->opt_bool_simplify = 1;
        s->opt_store_load_fwd = 1;
        s->opt_redundant_store = 1;
        s->opt_dead_store = 1;
        s->opt_indexed_memory = 1; /* Fuse SHL+ADD+LOAD/STORE into indexed ops */
        s->opt_disp_fusion = 1;    /* Fuse ADD+imm+LOAD/STORE into displacement-addressed ops */
        s->opt_lea_fold = 1;       /* Fold LEA Addr[StackLoc]+deref into direct stack slot access */
        s->opt_stack_addr_cse = 1;  /* Hoist repeated stack address computations */
        s->opt_strength_red = 1;    /* Strength reduction for multiply (peephole) */
        s->opt_vrp = 1;             /* Value range propagation branch folding */
        s->opt_float_narrow = 1;    /* Narrow double math to float when safe */
        s->opt_jump_threading = 1;  /* Jump threading optimization */
        s->opt_peephole = 1;        /* Instruction fusion, code generation caches */
        s->opt_inline_small = 1;    /* Inline tiny static/inline functions (≤30 words) */
        s->opt_inline_called_once = 1; /* Inline static functions called from one place, bodies deferred to TU end */
        s->opt_drop_unused_statics = 1; /* Emit only the statics something live refers to */
        /* Loop-invariant code motion is gcc -O1 (-ftree-loop-im,
         * -fmove-loop-invariants).  ssa:loop_rotate runs at -O1 too, and a
         * rotated loop without it keeps its invariants inside: a constant, a
         * const table's address (bench_conditionals +25%, indirect_calls +15%). */
        s->opt_licm = 1;
        if (!TCC_OPT(s, opt_inline_limit_user))
          s->opt_inline_limit = 30;
      }
      /* -O2: everything in -O1 plus the heavy tier — loop unrolling / IV
       * strength reduction / re-rolling, MUL+ADD→MLA fusion,
       * interprocedural constant propagation, and full small-function
       * inlining. */
      if (TCC_OPT(s, optimize) >= 2)
      {
        s->opt_mla_fusion = 1;      /* Fuse MUL+ADD into MLA */
        /* fp-offset-cache disabled: miscompiles loops when combined with
           iv-strength-red (e.g. SHA-1 sha_transform).  Can still be
           enabled manually with -ffp-offset-cache for debugging. */
        s->opt_ipc = 1;             /* Interprocedural constant propagation */
        s->opt_iv_strength_red = 1; /* IV strength reduction for array loops */
        s->opt_loop_unroll = 1;     /* Full-unroll small constant-trip-count loops */
        s->opt_reroll = 1;          /* Re-roll runs of identical macro-unrolled blocks */
        s->opt_inline_functions = 1; /* Inline small static/inline functions (≤100 words) */
        /* Only raise to the -O2 default when the user did not ask for a limit.
         * The old `if (limit < 100) limit = 100` force-raised, so
         * `-finline-limit=20 -O2` was silently ignored while
         * `-O2 -finline-limit=20` worked — the meaning depended on argv order. */
        if (!TCC_OPT(s, opt_inline_limit_user))
          s->opt_inline_limit = 100;
      }
      break;
    case TCC_OPTION_T:
      if (s->linker_script)
      {
        tcc_warning("multiple -T option");
        tcc_free(s->linker_script);
      }
      s->linker_script = tcc_strdup(optarg);
      break;
#ifdef CONFIG_TCC_DEBUG
    case TCC_OPTION_dump_ir:
      s->dump_ir = 1;
      break;
    case TCC_OPTION_dump_ir_passes:
      tcc_free(s->dump_ir_passes);
      s->dump_ir_passes = tcc_strdup(optarg);
      break;
#endif
    case TCC_OPTION_print_search_dirs:
      x = OPT_PRINT_DIRS;
      goto extra_action;
    case TCC_OPTION_impdef:
      x = OPT_IMPDEF;
      goto extra_action;
    case TCC_OPTION_yasc_c_header:
      x = OPT_YASC_C_HEADER;
      goto extra_action;
    case TCC_OPTION_ar:
      x = OPT_AR;
    extra_action:
      arg_start = optind - 1;
      if (arg_start != noaction)
        return tcc_error_noabort("cannot parse %s here", r);
      tool = x;
      break;
    default:
    unsupported_option:
      tcc_warning_c(warn_unsupported)("unsupported option '%s'", r);
      break;
    }
  }
  if (s->linker_arg.size)
  {
    r = s->linker_arg.data;
    goto arg_err;
  }
  *pargc = argc - arg_start;
  *pargv = argv + arg_start;
  if (tool)
    return tool;
  if (optind != noaction)
    return 0;
  if (s->verbose == 2)
    return OPT_PRINT_DIRS;
  if (s->verbose)
    return OPT_V;
  return OPT_HELP;
}

LIBTCCAPI int tcc_set_options(TCCState *s, const char *r)
{
  char **argv = NULL;
  int argc = 0, ret;
  args_parser_make_argv(r, &argc, &argv);
  ret = tcc_parse_args(s, &argc, &argv, 0);
  dynarray_reset(&argv, &argc);
  return ret < 0 ? ret : 0;
}

/* Startup-phase stamps: µs timestamps at fixed milestones between main() entry
   and tcc_delete(), dumped under -bench. The per-compile floor on the RP2350 is
   ~41 ms of tcc-side init that neither -bench's ms buckets nor PASS_TIME can
   attribute (it is before the passes). Recording is unconditional because the
   first stamps land before -bench has been parsed; the cost is one gettimeofday
   per stamp.  A compile records ~13; LOW_MEM keeps room for 16 (.bss). */
#ifdef CONFIG_TCC_LOW_MEM
#define TCC_INIT_STAMP_MAX 16
#else
#define TCC_INIT_STAMP_MAX 48
#endif
static struct
{
  const char *label;
  unsigned long us;
} tcc_init_stamps[TCC_INIT_STAMP_MAX];
static int tcc_init_stamp_count;

PUB_FUNC void tcc_init_stamp(const char *label)
{
  struct timeval tv;
  if (tcc_init_stamp_count >= TCC_INIT_STAMP_MAX)
    return;
  if (gettimeofday(&tv, NULL) != 0)
    return;
  tcc_init_stamps[tcc_init_stamp_count].label = label;
  tcc_init_stamps[tcc_init_stamp_count].us =
      (unsigned long)tv.tv_sec * 1000000UL + (unsigned long)tv.tv_usec;
  tcc_init_stamp_count++;
}

PUB_FUNC void tcc_init_stamps_dump(void)
{
  int i;
  unsigned long base, prev;
  if (tcc_init_stamp_count < 2)
    return;
  base = prev = tcc_init_stamps[0].us;
  fprintf(stderr, "# init %-24s %9s %9s\n", "phase", "delta_us", "at_us");
  for (i = 0; i < tcc_init_stamp_count; i++)
  {
    unsigned long us = tcc_init_stamps[i].us;
    /* Rows that cost nothing are still ~50 bytes of console each, and a
       profiling run pays that 4,449 times. Half the milestones are sub-µs on
       any real source, so print only what moved; `prev` still advances, so the
       surviving deltas remain the time since the previous milestone. */
    if (us - prev >= 100 || i == tcc_init_stamp_count - 1)
      fprintf(stderr, "# init %-24s %9lu %9lu\n", tcc_init_stamps[i].label,
              us - prev, us - base);
    prev = us;
  }
  fprintf(stderr, "# init %-24s %9lu\n", "total",
          tcc_init_stamps[tcc_init_stamp_count - 1].us - base);
}

/* Per-item rows (one per function per phase, one per file opened) are what you
   want when reading a single compile by hand and exactly what you must not
   have when profiling a corpus: they are printed *inside* the window they
   measure, and on the RP2350's 460800-baud console a compile with a dozen
   functions spends more time emitting them than compiling. So they are opt-in;
   the per-phase totals below always print. */
TCC_DBG_ENV_FLAG(tcc_bench_verbose, "TCC_BENCH_VERBOSE")

PUB_FUNC void tcc_bench_log(TCCState *s1, const char *operation, const char *name, unsigned elapsed_us)
{
  if (!s1 || !s1->do_bench || !tcc_bench_verbose())
    return;
  if (!name || !name[0])
    name = "<unknown>";
  fprintf(stderr, "# bench %-14s %8u us  %s\n", operation, elapsed_us, name);
}

static void tcc_print_bench_breakdown(const char *label, unsigned total_us, unsigned count)
{
  if (!count)
    return;
  fprintf(stderr, "# bench total %-16s %8u us  %4u calls  %9.1f us avg\n", label, total_us, count,
         (double)total_us / count);
}

extern unsigned long tcc_dryreason[8]; /* defined in ir/codegen.c, which writes it */

PUB_FUNC void tcc_print_stats(TCCState *s1, unsigned total_time)
{
  fprintf(stderr, "# dryreason fwd=%lu fp=%lu op=%lu i64=%lu callargs=%lu stackparam=%lu bigframe=%lu regs=%lu\n",
          tcc_dryreason[0], tcc_dryreason[1], tcc_dryreason[2], tcc_dryreason[3],
          tcc_dryreason[4], tcc_dryreason[5], tcc_dryreason[6], tcc_dryreason[7]);
  if (!total_time)
    total_time = 1;
  fprintf(stderr,
          "# %d idents, %d lines, %u bytes\n"
          "# %0.3f s, %u lines/s, %0.1f MB/s\n",
          total_idents, total_lines, total_bytes, (double)total_time / 1000, (unsigned)total_lines * 1000 / total_time,
          (double)total_bytes / 1000 / total_time);
  fprintf(stderr, "# text %u, data.rw %u, data.ro %u, bss %u bytes\n", s1->total_output[0], s1->total_output[1],
          s1->total_output[2], s1->total_output[3]);
  tcc_print_bench_breakdown("open", s1->bench_file_open_time, s1->bench_file_open_count);
  tcc_print_bench_breakdown("resolve", s1->bench_library_resolve_time, s1->bench_library_resolve_count);
    tcc_print_bench_breakdown("compile-setup", s1->bench_compile_setup_time, s1->bench_compile_setup_count);
    tcc_print_bench_breakdown("compile-exec", s1->bench_compile_exec_time, s1->bench_compile_exec_count);
    tcc_print_bench_breakdown("compile-finalize", s1->bench_compile_finalize_time, s1->bench_compile_finalize_count);
  tcc_print_bench_breakdown("func-body", s1->bench_function_body_time, s1->bench_function_body_count);
  tcc_print_bench_breakdown("func-opt", s1->bench_function_opt_time, s1->bench_function_opt_count);
  tcc_print_bench_breakdown("func-alloc", s1->bench_function_alloc_time, s1->bench_function_alloc_count);
  tcc_print_bench_breakdown("func-codegen", s1->bench_function_codegen_time, s1->bench_function_codegen_count);
  tcc_print_bench_breakdown("compile", s1->bench_compile_time, s1->bench_compile_count);
  tcc_print_bench_breakdown("obj", s1->bench_object_load_time, s1->bench_object_load_count);
  tcc_print_bench_breakdown("archive", s1->bench_archive_load_time, s1->bench_archive_load_count);
  if (s1->bench_archive_member_count)
    fprintf(stderr, "# bench total archive-members %u\n", s1->bench_archive_member_count);
  tcc_print_bench_breakdown("dll", s1->bench_dll_load_time, s1->bench_dll_load_count);
  tcc_print_bench_breakdown("ldscript", s1->bench_ldscript_load_time, s1->bench_ldscript_load_count);
  tcc_print_bench_breakdown("output", s1->bench_output_time, s1->bench_output_count);
  if (s1->bench_output_count)
  {
    /* Where the output phase goes. `runtime` is pulling in libc/libtcc1,
       `write` is emitting the file; the rest is linking. Anything not listed
       is section-name allocation and the smaller fixups between them. */
    unsigned accounted = s1->bench_output_runtime_us + s1->bench_output_got_us +
                         s1->bench_output_layout_us + s1->bench_output_reloc_us +
                         s1->bench_output_write_us;
    fprintf(stderr,
            "# bench output-split runtime %u us, got %u us, layout %u us, reloc %u us, write %u us, other %u us\n",
            s1->bench_output_runtime_us, s1->bench_output_got_us,
            s1->bench_output_layout_us, s1->bench_output_reloc_us,
            s1->bench_output_write_us,
            s1->bench_output_time > accounted ? s1->bench_output_time - accounted : 0);
  }
#if TCC_RECORD_ALLOC_SITES
  if (tcc_nb_big_allocs)
  {
    int i, j, shown;
    fprintf(stderr, "# bench total big-allocs        %6u calls  %lu KiB (>=2K, mmap-class)\n",
            tcc_nb_big_allocs, tcc_big_alloc_bytes / 1024);
    /* Top sites by bytes: selection sort over a table of tens, printed once. */
    shown = tcc_nb_alloc_sites < 8 ? tcc_nb_alloc_sites : 8;
    for (i = 0; i < shown; i++)
    {
      int best = i;
      TCCAllocSite tmp;
      for (j = i + 1; j < tcc_nb_alloc_sites; j++)
        if (tcc_alloc_sites[j].bytes > tcc_alloc_sites[best].bytes)
          best = j;
      tmp = tcc_alloc_sites[i];
      tcc_alloc_sites[i] = tcc_alloc_sites[best];
      tcc_alloc_sites[best] = tmp;
      fprintf(stderr, "# bench alloc-site %s:%d %u calls %lu KiB\n",
              tcc_basename((char *)tcc_alloc_sites[i].file), tcc_alloc_sites[i].line,
              tcc_alloc_sites[i].count, tcc_alloc_sites[i].bytes / 1024);
    }
  }
#endif
#ifdef MEM_DEBUG
  fprintf(stderr, "# memory usage");
#ifdef TCC_IS_NATIVE
  if (s1->run_size)
  {
    Section *s = s1->symtab;
    unsigned ms = s->data_offset + s->link->data_offset + s->hash->data_offset;
    unsigned rs = s1->run_size;
    fprintf(stderr, ": %d to run, %d symbols, %d other,", rs, ms, mem_cur_size - rs - ms);
  }
#endif
  fprintf(stderr, " %d max (bytes)\n", mem_max_size);
#endif
}

#include "tcchelpers.c"
