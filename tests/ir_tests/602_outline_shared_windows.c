/* -Os machine outliner: runs of IR ops that compile to the same code in
 * several functions are emitted once and called with BL.  The functions below
 * are instances of one template (as Zig's generics are), so their windows
 * repeat: a list append that stores two words and a halfword into a struct
 * through a pointer, a field update on the loop head (a window starting at a
 * jump target), a compare feeding a branch after the window (no CBZ fusion
 * inside one), and a conditional select (an IT block inside the window).
 * Non-static, so identical code folding does not merge them; each calls out,
 * so LR is saved and a BL may clobber it. */
#include <stdint.h>
#include <stdio.h>

typedef struct
{
  uint32_t *items;
  uint32_t len;
  uint32_t cap;
  uint16_t err;
  uint16_t flags;
  uint32_t sum;
} List;

static uint32_t pool[8][32];
static volatile uint32_t calls;

__attribute__((noinline)) uint16_t grow(List *l, uint32_t want)
{
  calls++;
  if (want > 32)
    return 7;
  l->cap = want;
  return 0;
}

#define TEMPLATE(NAME, K)                                                                                              \
  __attribute__((noinline)) uint16_t NAME(List *l, uint32_t v, uint32_t n)                                             \
  {                                                                                                                    \
    for (uint32_t i = 0; i < n; i++)                                                                                   \
    {                                                                                                                  \
      l->sum = l->sum * 31u + (v ^ i) + l->len;                                                                        \
      l->flags = (uint16_t)(l->flags | (1u << (i & 7)));                                                               \
      if (l->len == l->cap)                                                                                            \
      {                                                                                                                \
        uint16_t e = grow(l, l->cap * 2 + 1);                                                                          \
        if (e != 0)                                                                                                    \
        {                                                                                                              \
          l->err = e;                                                                                                  \
          return e;                                                                                                    \
        }                                                                                                              \
      }                                                                                                                \
      uint32_t w = (v + i * (K)) > l->sum ? v + i : l->sum - v;                                                        \
      l->items[l->len] = w;                                                                                            \
      l->len = l->len + 1;                                                                                             \
    }                                                                                                                  \
    l->err = 0;                                                                                                        \
    return 0;                                                                                                          \
  }

TEMPLATE(append_a, 3)
TEMPLATE(append_b, 5)
TEMPLATE(append_c, 7)
TEMPLATE(append_d, 9)
TEMPLATE(append_e, 11)
TEMPLATE(append_f, 13)
TEMPLATE(append_g, 15)
TEMPLATE(append_h, 17)

typedef uint16_t (*AppendFn)(List *, uint32_t, uint32_t);

int main(void)
{
  static const AppendFn fns[8] = {append_a, append_b, append_c, append_d, append_e, append_f, append_g, append_h};
  uint32_t total = 0;
  for (int f = 0; f < 8; f++)
  {
    List l = {pool[f], 0, 1, 0, 0, (uint32_t)f};
    uint16_t e = fns[f](&l, 0x1234u * (uint32_t)(f + 1), 20u + (uint32_t)f * 3u);
    uint32_t x = 0;
    for (uint32_t i = 0; i < l.len; i++)
      x = x * 33u + l.items[i];
    printf("%d: err %u len %u cap %u flags %04x sum %08x items %08x\n", f, (unsigned)e, (unsigned)l.len,
           (unsigned)l.cap, (unsigned)l.flags, (unsigned)l.sum, (unsigned)x);
    total ^= x + l.sum;
  }
  printf("calls %u total %08x\n", (unsigned)calls, (unsigned)total);
  return 0;
}
