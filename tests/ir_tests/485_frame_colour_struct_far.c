/* A frame just over 32 KiB: small struct locals declared first, so the
   frontend gives them offsets near the frame base, then a 32 KiB buffer whose
   address escapes.  At -O1 the frame relayout colours the hot struct copies
   down to the frame bottom, below the buffer, and a struct-typed stack
   operand keeps its offset in a 16-bit field: StackLoc[-32776] wrapped to
   StackLoc[32760], so the by-value argument was read from 32 KiB past the
   copy.  Zig's std.AutoHashMapUnmanaged with a FixedBufferAllocator over a
   64 KiB stack buffer reported count=0 after 500 puts.

   scribble() fills the stack first, so a word read from the wrong slot is
   0xA5A5A5A5 rather than a lucky copy of the right one. */
#include <stdio.h>

struct map
{
  unsigned *meta;
  unsigned size, avail;
};

struct fba
{
  unsigned char *buf;
  unsigned len, end;
};

__attribute__((noinline)) void scribble(void)
{
  volatile unsigned char junk[33024];
  for (unsigned i = 0; i < sizeof junk; i++)
    junk[i] = 0xA5;
}

__attribute__((noinline)) void put(struct map *m, struct fba *f, unsigned k)
{
  m->size++;
  f->buf[f->end] = (unsigned char)k;
  f->end += k;
}

__attribute__((noinline)) unsigned count(struct map m) { return m.size; }
__attribute__((noinline)) unsigned avail(struct map m) { return m.avail; }

__attribute__((noinline)) void run(unsigned n)
{
  struct map m2;
  struct map m;
  struct fba f1;
  unsigned i;
  unsigned char buf[32800];
  f1.buf = buf;
  f1.len = sizeof buf;
  f1.end = 0;
  m = (struct map){0, 0, 7};
  for (i = 0; i < n; i++)
    put(&m, &f1, i);
  m2 = m;
  unsigned c = count(m2);
  m2 = m;
  unsigned a = avail(m2);
  printf("count=%u avail=%u end=%u byte=%u\n", c, a, f1.end, buf[f1.end - n + 1]);
}

int main(void)
{
  scribble();
  run(10);
  scribble();
  run(20);
  return 0;
}
