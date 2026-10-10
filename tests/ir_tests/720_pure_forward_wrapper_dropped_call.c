/* pure_forward makes `shim_record(a, b, c) { cache_record(a, b, c); }` a
 * tail-call-only function while the call is there.  The callee, compiled
 * first, folds to an empty body (its feature flag is a constant 0), a later
 * pass drops the call, and codegen -- which emits no return for a
 * tail-call-only function -- left the wrapper 0 bytes: its symbol ran into
 * the next function, here bump(). */
#include <stdio.h>

typedef struct { int offset, phys_reg, valid, last_use; } Entry;
typedef struct { Entry *entries; int capacity; int access_count; } Cache;
typedef struct { void *cache; } State;
#define CACHE_ENABLED 0

int bumps;

void cache_record(State *st, int offset, int phys_reg)
{
  if (!st || !st->cache)
    return;
  if (!CACHE_ENABLED)
    return;
  Cache *cache = (Cache *)st->cache;
  cache->access_count++;
  for (int i = 0; i < cache->capacity; i++)
    if (cache->entries[i].valid && cache->entries[i].offset == offset)
    {
      cache->entries[i].phys_reg = phys_reg;
      cache->entries[i].last_use = cache->access_count;
      return;
    }
}

void shim_record(State *st, int offset, int phys_reg) { cache_record(st, offset, phys_reg); }

void bump(void) { bumps += 1000; }

int main(void)
{
  Entry e = {1, 2, 1, 0};
  Cache c = {&e, 1, 0};
  State st = {&c};
  void (*volatile record)(State *, int, int) = shim_record; /* an indirect call nothing can fold */
  record(&st, 1, 7);
  record(0, 1, 7);
  printf("bumps=%d access=%d phys=%d\n", bumps, c.access_count, e.phys_reg);
  bump();
  printf("bumps=%d\n", bumps);
  return 0;
}
