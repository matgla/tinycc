/* r0/r1 aggregate returns (gfunc_regpair_ret) next to guard heads
 * (docs/inline_guard_heads.md): an r0/r1 function whose body expands guard
 * heads, its adapter through a pointer, recursion, and an inlined K&R guard. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct slice { const char *ptr; uintptr_t len; };
struct opt { uintptr_t v; bool none; };
struct hdr { struct slice name; int freed; struct opt cached; int id; };

static int frees;
static char store[32] = "abcdefghijklmnopqrstuvwxyz";

static struct slice mkslice(int off, int n)
{
  if (off < 0) return (struct slice){NULL, 0};
  return (struct slice){store + off, (uintptr_t)n};
}

__attribute__((noinline)) static void release(struct hdr *h, struct slice s)
{
  frees += (int)s.len + h->id;
  h->freed++;
}

/* guard head: struct local assigned from a member, tested */
static void deinit(struct hdr *h)
{
  struct slice s;
  s = h->name;
  if (s.ptr != NULL)
  {
    release(h, s);
    release(h, s);
    frees ^= h->id * 7 + (int)s.len;
    h->id += (int)s.len;
    h->name = mkslice(-1, 0);
    frees += h->freed * 3 + h->id;
    release(h, h->name);
  }
  return;
}

/* guard head whose test reads an opt member set by a regpair function */
static struct opt peek(struct hdr *h)
{
  if (h->cached.none) return (struct opt){0, true};
  return h->cached;
}

static void drop_cache(struct hdr *h)
{
  bool n;
  n = h->cached.none;
  if (!n)
  {
    struct opt o = peek(h);
    frees += (int)o.v * 100;
    release(h, h->name);
    frees ^= h->id << 3;
    h->id -= 1;
    frees += (int)o.none + h->freed;
    h->cached = (struct opt){0, true};
  }
}

/* regpair function that calls guard-head functions inside */
static struct opt cycle(struct hdr *h, int k)
{
  deinit(h);
  drop_cache(h);
  if (k & 1) return (struct opt){(uintptr_t)h->freed, false};
  h->name = mkslice(k, k + 1);
  h->cached = (struct opt){(uintptr_t)k, false};
  return (struct opt){(uintptr_t)k * 3, (k & 2) != 0};
}

/* regpair recursion */
static struct slice rtrim(struct slice s)
{
  if (s.len == 0 || s.ptr[s.len - 1] != 'e') return s;
  return rtrim((struct slice){s.ptr, s.len - 1});
}

/* address taken after direct calls: adapter */
typedef struct opt (*cyc_fn)(struct hdr *, int);
typedef void (*de_fn)(struct hdr *);

static struct hdr hs[4];

/* K&R guard head with narrow param */
static void knr_guard(h, c)
  struct hdr *h; signed char c;
{
  int t;
  t = c;
  if (t < 0)
  {
    h->id += 1000;
  }
}

static void knr_all(void);
static struct opt via(cyc_fn f, struct hdr *h, int k) { return f(h, k); }

__attribute__((noinline)) static int driver(void)
{
  for (int i = 0; i < 4; i++) { hs[i].id = i + 1; hs[i].name = mkslice(i, 2); hs[i].cached = (struct opt){(uintptr_t)i, i == 2}; }
  for (int k = 0; k < 8; k++)
  {
    struct hdr *h = &hs[k & 3];
    struct opt r = cycle(h, k);
    printf("k=%d v=%lu none=%d freed=%d frees=%d\n", k, (unsigned long)r.v, r.none, h->freed, frees);
    deinit(h);
    deinit(h);
  }
  cyc_fn f = cycle;
  de_fn d = deinit;
  for (int k = 0; k < 6; k++)
  {
    struct opt r = via(f, &hs[k & 3], k + 10);
    d(&hs[(k + 1) & 3]);
    printf("p k=%d v=%lu none=%d frees=%d\n", k, (unsigned long)r.v, r.none, frees);
  }
  struct slice t = rtrim((struct slice){"abceee", 6});
  printf("trim %.*s %lu\n", (int)t.len, t.ptr, (unsigned long)t.len);
  knr_all();
  for (int i = 0; i < 4; i++) printf("id %d %d\n", i, hs[i].id);
  printf("frees %d\n", frees);
  return 0;
}

int main(void) { return driver(); }

static void knr_all(void) { for (int i = 0; i < 4; i++) knr_guard(&hs[i], 200 + i * 20); }
