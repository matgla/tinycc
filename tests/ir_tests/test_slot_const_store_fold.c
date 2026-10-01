/* Byte and halfword field stores over a zero-filled local word fold into that
 * word's store (slot_const_store_fold) -- the compound-literal shape of a
 * struct with bool fields passed by value.  The cases that must not fold: the
 * word read between the stores, a store through a pointer that may reach the
 * local, and a volatile local. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

struct opt { uint32_t payload; bool is_null; };
struct O { struct opt w, p; uint8_t fill, align; uint16_t h; };

__attribute__((noinline)) static unsigned take(struct O o, int x)
{
  return o.w.payload * 7 + o.w.is_null * 3 + o.p.payload + o.p.is_null * 5 + o.fill * 11 + o.align * 13 + o.h * 17 +
         (unsigned)x;
}

__attribute__((noinline)) static unsigned lit(int x)
{
  return take((struct O){{100, true}, {200, false}, 2, 32, 0x1234}, x);
}

/* The word is read between the zero fill and the field store. */
__attribute__((noinline)) static unsigned read_between(int x)
{
  struct O o = {{5, false}, {6, false}, 0, 0, 0};
  unsigned before = *(volatile uint8_t *)&o.fill;
  o.fill = (uint8_t)x;
  return take(o, 1) + before;
}

/* A store through a pointer to the same local between the stores. */
__attribute__((noinline)) static unsigned via_pointer(uint8_t *p, int x)
{
  struct O o = {{1, false}, {2, true}, 0, 0, 0};
  uint8_t *q = p ? p : &o.align;
  *q = 9;
  o.fill = (uint8_t)x;
  return take(o, 2);
}

__attribute__((noinline)) static unsigned vol(int x)
{
  volatile struct O o = {{3, true}, {4, true}, 0, 0, 0};
  o.fill = 7;
  o.h = (uint16_t)x;
  return o.fill + o.h + o.w.is_null;
}

int main(void)
{
  uint8_t sink = 0;
  printf("%u %u\n", lit(1), lit(40));
  printf("%u\n", read_between(3));
  printf("%u %u\n", via_pointer(0, 4), via_pointer(&sink, 4));
  printf("%u %u\n", vol(1000), sink);
  return 0;
}
