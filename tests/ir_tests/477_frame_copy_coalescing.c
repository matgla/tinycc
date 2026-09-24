/* Frame copy coalescing (frame.c, frame_find_copies + frame_colour): a
   whole-object block copy whose source dies at the copy and whose destination
   is born there gives both objects the same bytes, and the copy goes.  The
   shapes are zig's C, where a value moves through a chain of temporaries and
   is then read through a pointer (Io_File_Reader_getSize):

     t17 = t16.payload; t18 = t17; t19 = &t18; ... t19->kind ...

   Each function prints what a correct copy leaves; the ones marked "must not"
   keep a side in use across the copy, so coalescing there would be wrong. */
#include <stdio.h>

struct stat_like { int kind; int size[10]; };
struct errunion { struct stat_like payload; unsigned short error; }; /* zig: payload first */

__attribute__((noinline)) struct errunion get(int k)
{
  struct errunion e;
  e.error = (unsigned short)(k < 0);
  e.payload.kind = k;
  for (int i = 0; i < 10; i++)
    e.payload.size[i] = k * 10 + i;
  return e;
}

__attribute__((noinline)) int consume(const struct stat_like *p) { return p->kind * 1000 + p->size[9]; }

/* The zig shape: coalesced. */
__attribute__((noinline)) int zig_shape(int k)
{
  struct errunion t16 = get(k);
  struct stat_like t17, t18;
  const struct stat_like *t19;
  if (t16.error)
    return -1;
  t17 = t16.payload;
  t18 = t17;
  t19 = (const struct stat_like *)&t18;
  if (t19->kind == 5)
    return consume(t19) + 1;
  return consume(t19);
}

/* Must not: the source is read after the copy. */
__attribute__((noinline)) int source_read_after(int k)
{
  struct errunion t16 = get(k);
  struct stat_like t17, t18;
  t17 = t16.payload;
  t18 = t17;
  t18.kind = 77;
  const struct stat_like *p = &t18;
  return consume(p) * 100 + t17.kind;
}

/* Must not: the destination holds a value before the copy that is read
   after it. */
__attribute__((noinline)) int dest_live_before(int k)
{
  struct errunion t16 = get(k), t20 = get(k + 1);
  struct stat_like t17, t18;
  t18 = t20.payload;
  int before = t18.size[2];
  t17 = t16.payload;
  t18 = t17;
  const struct stat_like *p = &t18;
  return before * 100000 + consume(p);
}

/* In a loop, the chain rebuilt every iteration. */
__attribute__((noinline)) int in_loop(int n)
{
  int total = 0;
  for (int i = 0; i < n; i++)
  {
    struct errunion t16 = get(i);
    struct stat_like t17, t18;
    t17 = t16.payload;
    t18 = t17;
    const struct stat_like *p = &t18;
    total += consume(p);
  }
  return total;
}

int main(void)
{
  printf("zig_shape=%d,%d source_read_after=%d\n", zig_shape(5), zig_shape(3), source_read_after(4));
  printf("dest_live_before=%d in_loop=%d\n", dest_live_before(2), in_loop(4));
  return 0;
}
