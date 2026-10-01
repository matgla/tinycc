// ctags' isTagExtraBitMarked: `index` is a signed divide by 8 in both arms of
// an if, and the join adds it to a pointer.  The barrel-shift fusion sees the
// flat IR before SSA, where `index` has one def per arm; it folded the else
// arm's `asr #3` into the join's ADD, so the then arm reached it with the
// shift's source register never written and ctags faulted on the device.
#include <stdio.h>

typedef struct
{
  int pad[9];
  unsigned char extra[2];
  unsigned char *dyn;
} entry;

__attribute__((noinline)) static int marked(const entry *e, int bit)
{
  unsigned index, offset;
  const unsigned char *slot;

  if (bit < 9)
  {
    index = bit / 8;
    offset = bit % 8;
    slot = e->extra;
  }
  else if (!e->dyn)
    return 0;
  else
  {
    index = (bit - 9) / 8;
    offset = (bit - 9) % 8;
    slot = e->dyn;
  }
  return !!(slot[index] & (1 << offset));
}

/* The same join through a loop: the shift source changes on the back edge. */
__attribute__((noinline)) static int sum_shifted(const int *v, int n)
{
  int s = 0, x = v[0];
  int t = x >> 3;
  for (int i = 0; i < n; i++)
  {
    s += t;
    x = v[i + 1];
    t = x >> 3;
  }
  return s;
}

int main(void)
{
  static unsigned char dyn[4] = {0x00, 0x24, 0x00, 0x80};
  entry e = {{0}, {0x05, 0x01}, dyn};
  unsigned bits = 0;
  for (int b = 0; b < 9 + 32; b++)
    bits = bits * 3 + marked(&e, b);
  printf("bits=%u\n", bits);
  printf("then0=%d then8=%d else18=%d\n", marked(&e, 0), marked(&e, 8), marked(&e, 9 + 13));
  e.dyn = 0;
  printf("nodyn=%d\n", marked(&e, 12));
  int v[5] = {80, 160, -24, 7, 0};
  printf("sum=%d\n", sum_shifted(v, 4));
  return 0;
}
