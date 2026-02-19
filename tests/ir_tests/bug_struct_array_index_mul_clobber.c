#include <stdio.h>

/*
 * Regression test for codegen bug: mul clobbers base register during
 * struct array indexing.
 *
 * When computing &container->items[idx] where sizeof(Elem) is not a
 * power of 2 (here 12), the codegen emits:
 *
 *   mov   r0, r2             @ r0 = container
 *   adds  r0, #offset        @ r0 = &container->items (or container + offset)
 *   movs  r2, #12            @ r2 = sizeof(Elem)
 *   mul.w r0, r1, r2         @ r0 = idx * 12  -- CLOBBERS base address!
 *
 * The mul overwrites r0 which still holds the base pointer, so the
 * returned pointer is just (idx * sizeof(Elem)) instead of
 * (container->items + idx * sizeof(Elem)).
 *
 * Observed in tcc_ir_pool_get_symref_ptr (IRPoolSymref is 12 bytes).
 */

/* 12-byte struct (3 * 4), NOT a power of 2 → forces mul for indexing */
typedef struct
{
  int a;
  int b;
  int c;
} Elem;

/* Container with some padding fields before the pointer, so the pointer
 * sits at a non-zero offset (mirrors TCCIRState.pool_symref at offset 48). */
typedef struct
{
  int field0;
  int field1;
  int field2;
  int count;
  Elem *items;
} Container;

/* This function mirrors tcc_ir_pool_get_symref_ptr:
 *   return &container->items[idx];
 * The bug causes the returned pointer to be wrong. */
Elem *get_item(const Container *c, unsigned idx)
{
  if (!c || idx >= (unsigned)c->count)
    return 0;
  return &c->items[idx];
}

int main(void)
{
  Elem arr[4] = {{10, 20, 30}, {40, 50, 60}, {70, 80, 90}, {100, 110, 120}};
  Container cont;
  cont.field0 = 0xAA;
  cont.field1 = 0xBB;
  cont.field2 = 0xCC;
  cont.count = 4;
  cont.items = arr;

  int pass = 1;

  for (unsigned i = 0; i < 4; i++)
  {
    Elem *e = get_item(&cont, i);
    if (!e)
    {
      printf("FAIL: get_item returned NULL for index %u\n", i);
      pass = 0;
      continue;
    }
    printf("items[%u] = {%d, %d, %d}\n", i, e->a, e->b, e->c);
  }

  /* Spot-check: element 0 must point into arr, not near address 0 */
  Elem *e0 = get_item(&cont, 0);
  if (!e0 || e0 != &arr[0])
  {
    printf("FAIL: items[0] pointer mismatch: got %p, expected %p\n", (void *)e0, (void *)&arr[0]);
    pass = 0;
  }

  /* Spot-check: element 2 */
  Elem *e2 = get_item(&cont, 2);
  if (!e2 || e2->a != 70 || e2->b != 80 || e2->c != 90)
  {
    printf("FAIL: items[2] values wrong\n");
    pass = 0;
  }

  /* Spot-check: element 3 */
  Elem *e3 = get_item(&cont, 3);
  if (!e3 || e3->a != 100 || e3->b != 110 || e3->c != 120)
  {
    printf("FAIL: items[3] values wrong\n");
    pass = 0;
  }

  if (pass)
    printf("PASS\n");

  return pass ? 0 : 1;
}
