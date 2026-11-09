/*
 * alloc_peak: replay a TCC_ALLOC_BACKTRACE trace (source/driver/libtcc.c) and
 * print what is live at the compile's memory peak, by call stack.
 *
 *   alloc_peak TRACE              -> E lines: event count, both peaks and their events
 *   alloc_peak TRACE --stop N     -> replay events 0..N, then one line per stack
 *                                    holding memory: "S stack bytes blocks"
 *
 * The trace is {in, size, out, stack} 32-bit words per call into the
 * allocator (in 0 = malloc, size 0 = free, otherwise realloc of `in`).
 * alloc_trace.py runs it twice: once for the peak, once to stop there.
 *
 * Built with -DMALLOC_C=path/to/yasos/libs/libc/malloc.c the replay runs
 * through the device's malloc over a port of the device's page allocator (as
 * libs/libc/tests/malloc_replay.c), and `mapped` (the default then) is the
 * peak of pages the process holds -- what /proc/mempeak reports, less the
 * stack and the hot arena.  `live` is the peak of requested bytes.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef MALLOC_C
#define DEV_PAGE 4096
#define DEV_PAGES 32768 /* 128 MiB */
/* static: malloc.c defines aligned_alloc, which would land in the model */
static unsigned char dev_arena[(long)DEV_PAGES * DEV_PAGE] __attribute__((aligned(DEV_PAGE)));
static unsigned char dev_bitmap[DEV_PAGES];
static long dev_used, dev_peak, dev_low;

static long dev_pages_for(long len) { return (len + DEV_PAGE - 1) / DEV_PAGE; }

static void dev_set(long from, long n, unsigned char v)
{
  memset(dev_bitmap + from, v, n);
  dev_used += v ? n : -n;
  if (dev_used > dev_peak)
    dev_peak = dev_used;
  if (!v && from < dev_low)
    dev_low = from;
}

static void *dev_mmap(long len)
{
  long need = dev_pages_for(len), i = dev_low;
  while (i < DEV_PAGES && dev_bitmap[i])
    i++;
  dev_low = i;
  while (i + need <= DEV_PAGES)
  {
    long j = 0;
    while (j < need && !dev_bitmap[i + j])
      j++;
    if (j == need)
    {
      dev_set(i, need, 1);
      return dev_arena + i * DEV_PAGE;
    }
    i += j + 1;
  }
  return (void *)-1;
}

static long dev_index(void *addr) { return ((unsigned char *)addr - dev_arena) / DEV_PAGE; }

static int dev_munmap(void *addr, long len)
{
  dev_set(dev_index(addr), dev_pages_for(len), 0);
  return 0;
}

static void *dev_mremap(void *addr, long old_len, long new_len, int flags)
{
  (void)flags;
  long idx = dev_index(addr), oldn = dev_pages_for(old_len), newn = dev_pages_for(new_len);
  if (newn == oldn)
    return addr;
  if (newn < oldn)
  {
    if (newn <= 0)
      return (void *)-1;
    dev_set(idx + newn, oldn - newn, 0);
    return addr;
  }
  if (idx + newn > DEV_PAGES)
    return (void *)-1;
  for (long k = oldn; k < newn; k++)
    if (dev_bitmap[idx + k])
      return (void *)-1;
  dev_set(idx + oldn, newn - oldn, 1);
  return addr;
}

#define MAP_PRIVATE 0
#define MAP_ANONYMOUS 0
#define PROT_READ 0
#define PROT_WRITE 0
#define MAP_FAILED ((void *)-1)
#define mmap(addr, len, prot, flags, fd, off) dev_mmap((long)(len))
#define munmap(addr, len) dev_munmap((addr), (long)(len))
#define mremap(addr, old, new, flags) dev_mremap((addr), (long)(old), (long)(new), (flags))
#define _SYS_MMAN_H 1

#define fprintf(...) (0)
#define malloc sut_malloc
#define free sut_free
#define calloc sut_calloc
#define realloc sut_realloc
#include MALLOC_C
#undef fprintf
#undef malloc
#undef free
#undef calloc
#undef realloc
#define HAVE_MODEL 1
#ifndef MOFF_SLACK8 /* a malloc.c from before the slack bit */
#define MOFF_SLACK8 0
static int mhdr_block_size(const struct mhdr *h) { return (h->size + (int)sizeof(struct mhdr) + 7) & ~7; }
#endif
#else
#define HAVE_MODEL 0
#endif

/* ---- traced pointer -> {replay pointer, size, stack} ---- */
static uint32_t *key, *bsize, *bstack, *bborn; /* bstack: stack id | GROWN; bborn: the event that made it */
#define GROWN 0x80000000u
static void **val;
static uint32_t cap = 1u << 23;

static uint32_t slot(uint32_t k)
{
  uint32_t m = cap - 1, h = (k >> 3) * 2654435761u & m;
  while (key[h] && key[h] != k)
    h = (h + 1) & m;
  return h;
}

static void del(uint32_t h)
{
  uint32_t m = cap - 1;
  key[h] = 0;
  for (uint32_t j = (h + 1) & m; key[j]; j = (j + 1) & m)
  {
    uint32_t k = key[j], s = bsize[j], st = bstack[j], b = bborn[j];
    void *v = val[j];
    key[j] = 0;
    uint32_t t = slot(k);
    key[t] = k, val[t] = v, bsize[t] = s, bstack[t] = st, bborn[t] = b;
  }
}

static long live, live_peak, events, peak_event_live = -1;
static int fn_depth;           /* function compiles open (tcc_alloc_trace_mark 1/2) */
/* passes (tcc_alloc_trace_pass): the open ones, and per pass the most it held above
   what was live when it started, and the most live while it ran */
#define MAX_PASSES 1024
static struct { int id; long base; } pass_stack[64];
static int pass_depth;
static long pass_max_delta[MAX_PASSES], pass_max_live[MAX_PASSES], pass_max_live_event[MAX_PASSES];
static long pass_max_map[MAX_PASSES], pass_max_map_event[MAX_PASSES]; /* model pages mapped */
static int pass_seen[MAX_PASSES];
static long fn_start = -1;     /* the event the outermost one started at */
#if HAVE_MODEL
static long map_peak, peak_event_map = -1;
#endif

/* Replay the trace; stop after event `stop` (-1: to the end). */
static void replay(const char *path, long stop)
{
  FILE *f = fopen(path, "rb");
  if (!f)
  {
    perror(path);
    exit(1);
  }
  uint32_t rec[4 * 4096];
  size_t n;
  while ((n = fread(rec, 16, 4096, f)) > 0)
    for (size_t i = 0; i < n; i++)
    {
      uint32_t in = rec[4 * i], size = rec[4 * i + 1], out = rec[4 * i + 2], st = rec[4 * i + 3];
      if (in == 0xffffffffu)
      {
        /* a mark (tcc_alloc_trace_mark), not an allocation */
        if (size == 1 && fn_depth++ == 0)
          fn_start = events;
        else if (size == 2 && --fn_depth == 0)
          fn_start = -1;
        else if (size == 5 && pass_depth < 64)
          pass_stack[pass_depth].id = (int)out, pass_stack[pass_depth].base = live, pass_depth++;
        else if (size == 6 && pass_depth > 0)
          pass_depth--;
        continue;
      }
      void *old = NULL;
      int grew = 0; /* a realloc that grew the block: a growable array */
      if (in)
      {
        uint32_t h = slot(in);
        if (key[h])
        {
          old = val[h];
          live -= bsize[h];
          grew = size > bsize[h];
          del(h);
        }
      }
      void *p = NULL;
#if HAVE_MODEL
      if (size == 0)
        sut_free(old);
      else
        p = in ? sut_realloc(old, size) : sut_malloc(size);
      if (size && !p)
      {
        fprintf(stderr, "replay arena full at event %ld\n", events);
        exit(1);
      }
#else
      (void)old;
      p = (void *)(uintptr_t)out;
#endif
      if (size)
      {
        uint32_t h = slot(out);
        key[h] = out, val[h] = p, bsize[h] = size, bstack[h] = st | (grew ? GROWN : 0), bborn[h] = (uint32_t)events;
        live += size;
      }
      if (live > live_peak)
        live_peak = live, peak_event_live = events;
      if (pass_depth > 0 && pass_depth <= 64)
      {
        int id = pass_stack[pass_depth - 1].id;
        if (id >= 0 && id < MAX_PASSES)
        {
          long d = live - pass_stack[pass_depth - 1].base;
          pass_seen[id] = 1;
          if (d > pass_max_delta[id])
            pass_max_delta[id] = d;
          if (live > pass_max_live[id])
            pass_max_live[id] = live, pass_max_live_event[id] = events;
#if HAVE_MODEL
          if ((long)dev_used > pass_max_map[id])
            pass_max_map[id] = (long)dev_used, pass_max_map_event[id] = events;
#endif
        }
      }
#if HAVE_MODEL
      if (dev_used > map_peak)
        map_peak = dev_used, peak_event_map = events;
#endif
#ifdef REPLAY_HOOK
      REPLAY_HOOK(in, size, out);
#endif
      if (events++ == stop)
      {
        fclose(f);
        return;
      }
    }
  fclose(f);
}

static int cmp_u32(const void *a, const void *b)
{
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
  if (argc != 2 && !(argc == 4 && !strcmp(argv[2], "--stop")))
  {
    fprintf(stderr, "usage: %s TRACE [--stop EVENT]\n", argv[0]);
    return 2;
  }
  key = calloc(cap, sizeof *key);
  bsize = calloc(cap, sizeof *bsize);
  bstack = calloc(cap, sizeof *bstack);
  bborn = calloc(cap, sizeof *bborn);
  val = calloc(cap, sizeof *val);
  long stop = argc == 4 ? atol(argv[3]) : -1;
  replay(argv[1], stop);
  printf("E events %ld\n", events);
  printf("E model %d\n", HAVE_MODEL);
  printf("E live_peak %ld at %ld\n", live_peak, peak_event_live);
#if HAVE_MODEL
  printf("E mapped_peak %ld at %ld\n", map_peak * (long)DEV_PAGE, peak_event_map);
#endif
  if (stop < 0)
  {
    for (int id = 0; id < MAX_PASSES; id++)
      if (pass_seen[id])
        printf("P %d %ld %ld %ld %ld %ld\n", id, pass_max_delta[id], pass_max_live[id], pass_max_live_event[id],
               pass_max_map[id] * (long)DEV_PAGE, pass_max_map_event[id]);
    return 0;
  }
  printf("E live_now %ld\n", live);
  {
    /* the function being compiled at the stop, if any: what it holds (born since it
       started) against what was there before */
    unsigned long spike = 0;
    for (uint32_t i = 0; i < cap; i++)
      if (key[i] && fn_start >= 0 && (long)bborn[i] >= fn_start)
        spike += bsize[i];
    printf("E in_function %d since_event %ld spike %lu base %lu\n", fn_start >= 0, fn_start, spike, live - spike);
  }
#if HAVE_MODEL
  printf("E mapped_now %ld\n", dev_used * (long)DEV_PAGE);
  {
    /* Where mapped - live goes: the 8-byte header and 8-byte rounding of each
       pool block, the pools' free space, page rounding of the mappings, the
       parked mappings, and whatever else is mapped (the retired[] table). */
    long small_n = 0, small_req = 0, small_blk = 0, large_n = 0, large_req = 0, large_map = 0;
    long large_pages_hist[6] = {0}; /* waste per mapping: <256, <1K, <2K, <3K, <4K */
    for (uint32_t i = 0; i < cap; i++)
    {
      if (!key[i])
        continue;
      char *u = val[i];
      struct mhdr *h = (struct mhdr *)(u - sizeof(struct mhdr));
      if (h->moff == LARGE_MOFF)
      {
        long total = *(long *)(u - LARGE_HDR);
        large_n++, large_req += bsize[i], large_map += total;
        long w = total - bsize[i];
        large_pages_hist[w < 256 ? 0 : w < 1024 ? 1 : w < 2048 ? 2 : w < 3072 ? 3 : 4]++;
      }
      else
      {
        small_n++, small_req += bsize[i];
        small_blk += mhdr_block_size(h);
      }
    }
    long pools_b = 0, pools_n = 0;
    for (struct mset *m = old_pools; m; m = m->next_pool)
      pools_b += m->len, pools_n++;
    if (pool)
      pools_b += pool->len, pools_n++;
    if (pool1)
      pools_b += pool1->len, pools_n++;
    long fl_b = 0, fl_n = 0, fl_hist[8] = {0}, cur_bump = 0;
    for (int w = 0; w < 3; w++)
      for (struct mset *m = w == 0 ? old_pools : w == 1 ? pool : pool1; m; m = w == 0 ? m->next_pool : NULL)
      {
        for (struct free_node *fn = m->freelist; fn; fn = fn->next)
        {
          int b = fn->block_size, c = b < 64 ? 0 : b < 256 ? 1 : b < 1024 ? 2 : b < 2048 ? 3 : b < 4096 ? 4 : b < 8192 ? 5 : b < 16384 ? 6 : 7;
          fl_b += b, fl_n++, fl_hist[c] += b;
        }
        if (w == 1)
          cur_bump = m->len - m->size;
      }
    printf("O freelists %ld blocks %ld bytes  (<64:%ld <256:%ld <1K:%ld <2K:%ld <4K:%ld <8K:%ld <16K:%ld >=16K:%ld)  current_bump %ld\n",
           fl_n, fl_b, fl_hist[0], fl_hist[1], fl_hist[2], fl_hist[3], fl_hist[4], fl_hist[5], fl_hist[6], fl_hist[7], cur_bump);
    {
      /* Real extent of every pool block: the distance to whatever starts
         next (block, free node, pool end).  Bytes past the block its size
         implies belong to nobody -- free() would not give them back. */
      long ne = small_n + fl_n + 2 * (pools_n + 1), k = 0;
      uintptr_t(*ev)[2] = malloc(sizeof(*ev) * ne); /* {addr, computed block size or 0} */
      for (uint32_t i = 0; i < cap; i++)
        if (key[i] && ((struct mhdr *)((char *)val[i] - sizeof(struct mhdr)))->moff != LARGE_MOFF)
          ev[k][0] = (uintptr_t)val[i] - sizeof(struct mhdr),
          ev[k][1] = mhdr_block_size((struct mhdr *)((char *)val[i] - sizeof(struct mhdr))), k++;
      for (int w = 0; w < 3; w++)
        for (struct mset *m = w == 0 ? old_pools : w == 1 ? pool : pool1; m; m = w == 0 ? m->next_pool : NULL)
        {
          for (struct free_node *fn = m->freelist; fn; fn = fn->next)
            ev[k][0] = (uintptr_t)fn, ev[k][1] = (uintptr_t)fn->block_size | 1, k++; /* odd: a free node */
          ev[k][0] = (uintptr_t)m + (w == 1 ? m->size : m->len), ev[k][1] = 0, k++; /* end marker */
        }
      qsort(ev, k, sizeof(*ev), cmp_u32);
      long orphan[2] = {0}, orphan_n[2] = {0}, gh[2][6] = {{0}};
      for (long i = 0; i + 1 < k; i++)
        if (ev[i][1])
        {
          int isfree = ev[i][1] & 1;
          long gap = (long)(ev[i + 1][0] - ev[i][0]) - (long)(ev[i][1] & ~(uintptr_t)1);
          if (gap > 0 && gap < 65536)
          {
            orphan[isfree] += gap, orphan_n[isfree]++;
            gh[isfree][gap <= 4 ? 0 : gap <= 8 ? 1 : gap <= 16 ? 2 : gap <= 64 ? 3 : gap <= 1024 ? 4 : 5] += gap;
          }
        }
      for (int f = 0; f < 2; f++)
        printf("O orphaned_after_%s %ld bytes in %ld gaps (bytes by gap: <=4:%ld <=8:%ld <=16:%ld <=64:%ld <=1K:%ld >1K:%ld)\n",
               f ? "free_nodes" : "blocks", orphan[f], orphan_n[f], gh[f][0], gh[f][1], gh[f][2], gh[f][3], gh[f][4], gh[f][5]);
      free(ev);
    }
    printf("O small_blocks %ld requested %ld blocks_bytes %ld header_and_rounding %ld\n", small_n, small_req, small_blk,
           small_blk - small_req);
    printf("O pools %ld bytes %ld free_in_pools %ld\n", pools_n, pools_b, pools_b - small_blk);
    printf("O large_blocks %ld requested %ld mapped %ld rounding %ld (waste <256:%ld <1K:%ld <2K:%ld <3K:%ld <4K:%ld)\n",
           large_n, large_req, large_map, large_map - large_req, large_pages_hist[0], large_pages_hist[1],
           large_pages_hist[2], large_pages_hist[3], large_pages_hist[4]);
    printf("O parked %ld\n", large_cache_bytes);
    printf("O other_mapped %ld\n", dev_used * (long)DEV_PAGE - pools_b - large_map - large_cache_bytes);
  }
  long ms[19] = {0};
  __malloc_stats(ms, 19);
  printf("M");
  for (int i = 0; i < 19; i++)
    printf(" %ld", ms[i]);
  printf("\n");
#endif
  /* live bytes and blocks per stack: sort the live blocks by stack */
  uint32_t n = 0;
  for (uint32_t i = 0; i < cap; i++)
    if (key[i])
      n++;
  /* "S stack bytes blocks grown_bytes": grown_bytes are blocks a realloc
     last grew -- growable arrays, whose capacity past their use is slack */
  /* {stack, size | GROWN, what the allocator spends past the size} */
  uint32_t *pairs = malloc(sizeof(uint32_t) * 4 * (n ? n : 1)), k = 0;
  for (uint32_t i = 0; i < cap; i++)
    if (key[i])
    {
      uint32_t over = 0;
#if HAVE_MODEL
      struct mhdr *h = (struct mhdr *)((char *)val[i] - sizeof(struct mhdr));
      over = h->moff == LARGE_MOFF ? (uint32_t)(*(long *)((char *)val[i] - LARGE_HDR) - bsize[i])
                                   : (uint32_t)(mhdr_block_size(h) - bsize[i]);
#endif
      pairs[4 * k] = bstack[i] & ~GROWN, pairs[4 * k + 1] = bsize[i] | (bstack[i] & GROWN), pairs[4 * k + 2] = over;
      pairs[4 * k + 3] = fn_start >= 0 && (long)bborn[i] >= fn_start;
      k++;
    }
  qsort(pairs, n, 16, cmp_u32);
  for (uint32_t i = 0; i < n;)
  {
    uint32_t st = pairs[4 * i], blocks = 0, smin = ~0u, smax = 0;
    unsigned long bytes = 0, grown = 0, over = 0, spike = 0;
    for (; i < n && pairs[4 * i] == st; i++)
    {
      uint32_t sz = pairs[4 * i + 1] & ~GROWN;
      bytes += sz, blocks++, over += pairs[4 * i + 2];
      smin = sz < smin ? sz : smin, smax = sz > smax ? sz : smax;
      if (pairs[4 * i + 1] & GROWN)
        grown += sz;
      if (pairs[4 * i + 3])
        spike += sz;
    }
    printf("S %u %lu %u %lu %u %u %lu %lu\n", st, bytes, blocks, grown, smin, smax, over, spike);
  }
  /* block sizes: B size_class_upper bytes blocks (power-of-two classes) */
  unsigned long cb[33] = {0}, cn[33] = {0};
  for (uint32_t i = 0; i < n; i++)
  {
    int c = 0;
    uint32_t sz = pairs[4 * i + 1] & ~GROWN;
    while ((1ul << c) < sz)
      c++;
    cb[c] += sz, cn[c]++;
  }
  for (int c = 0; c < 33; c++)
    if (cn[c])
      printf("B %lu %lu %lu\n", 1ul << c, cb[c], cn[c]);
  return 0;
}
