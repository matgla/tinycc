/* Merge of fix4/bitmap-helpers-leftovers (static 8-byte aggregate returns in
 * r0/r1, AAPCS adapters for their addresses) with
 * fix4/componentiterator-leftovers (guard heads replayed at call sites).
 * Guard-head calls whose arguments are r0/r1 calls or have side effects,
 * guard heads copying an r0/r1-shaped struct, r0/r1 functions that expand
 * guard heads, and the same functions through their adapters. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

struct opt { uintptr_t v; bool none; };
struct ent { struct opt o; int hits; int pad; };
static struct ent tab[8];
static int calls, sink;

__attribute__((noinline)) static void touch(int x) { sink = sink * 31 + x; }

static struct opt lookup(int k)
{
  calls++;
  if (k < 0 || k >= 8 || tab[k].o.none) return (struct opt){0, true};
  return tab[k].o;
}

/* guard head: struct opt local copied from *e, test on .none */
static void retire(struct ent *e)
{
  struct opt o;
  int n = 0;
  o = e->o;
  n = e->hits;
  if (!o.none && n > 1)
  {
    touch((int)o.v);
    touch(n);
    e->hits = 0;
    e->o = (struct opt){0, true};
    touch(e->pad);
    touch(calls);
    sink ^= n << 4;
  }
}

/* guard head with an index parameter */
static void bump(uintptr_t idx)
{
  struct opt o;
  o = tab[idx & 7].o;
  if (o.v != 0)
  {
    tab[idx & 7].hits += 2;
    touch((int)idx);
    touch((int)o.v);
    touch(tab[idx & 7].hits);
    sink += (int)idx * 3;
  }
}

static struct opt step(int k)
{
  struct opt r = lookup(k);
  bump(lookup(k + 1).v);          /* regpair call in a guard call's argument */
  retire(&tab[(k + 2) & 7]);
  if (r.none) return (struct opt){(uintptr_t)k, true};
  bump(r.v + k);
  return (struct opt){r.v * 2, false};
}

typedef struct opt (*stepf)(int);
static stepf pick(int k) { return k & 1 ? step : lookup; }

__attribute__((noinline)) static int driver(void)
{
  for (int i = 0; i < 8; i++) { tab[i].o = (struct opt){(uintptr_t)(i * 5) & 7, i == 3}; tab[i].hits = i; tab[i].pad = 100 + i; }
  struct ent *p = tab;
  for (int k = 0; k < 12; k++)
  {
    struct opt r = step(k % 9);
    printf("s %d %lu %d sink=%d calls=%d\n", k, (unsigned long)r.v, r.none, sink, calls);
    retire(p++);                  /* side effect in a guard call's argument */
    if (p == tab + 8) p = tab;
    struct opt q = pick(k)(k % 8);
    printf("q %d %lu %d sink=%d\n", k, (unsigned long)q.v, q.none, sink);
    bump(k);
  }
  for (int i = 0; i < 8; i++) printf("t %d %lu %d %d\n", i, (unsigned long)tab[i].o.v, tab[i].o.none, tab[i].hits);
  printf("sink %d calls %d\n", sink, calls);
  return 0;
}

int main(void) { return driver(); }
