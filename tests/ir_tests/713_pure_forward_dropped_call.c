/* A pure-forward wrapper (`f(a) { g(a); }`) is made tail-call-only while its
 * call exists; the call was then dropped (the callee, compiled first, folded
 * to an empty body) and codegen, which emits no return for a tail-call-only
 * function, left the wrapper 0 bytes: calling it ran the next function. */
#include <stdio.h>

typedef struct { int offset, phys_reg, valid, last_use; } E;
typedef struct { E *entries; int capacity; int access_count; } C;
typedef struct { void *cache; } IR;
#define ENABLED 0

int hits;
void cache_record(IR *ir, int offset, int phys_reg);

void cache_record(IR *ir, int offset, int phys_reg)
{
  if (!ir || !ir->cache)
    return;
  if (!ENABLED)
    return;
  C *cache = (C *)ir->cache;
  cache->access_count++;
  for (int i = 0; i < cache->capacity; i++)
    if (cache->entries[i].valid && cache->entries[i].offset == offset)
    {
      cache->entries[i].phys_reg = phys_reg;
      cache->entries[i].last_use = cache->access_count;
      return;
    }
}

void shim_record(IR *ir, int offset, int phys_reg) { cache_record(ir, offset, phys_reg); }

void bump(void) { hits += 1000; }

int main(void)
{
  E e = {1, 2, 1, 0};
  C c = {&e, 1, 0};
  IR ir = {&c};
  void (*volatile record)(IR *, int, int) = shim_record; /* a real call */
  record(&ir, 1, 7);
  record(0, 1, 7);
  printf("hits=%d access=%d phys=%d\n", hits, c.access_count, e.phys_reg);
  bump();
  printf("hits=%d\n", hits);
  return 0;
}
