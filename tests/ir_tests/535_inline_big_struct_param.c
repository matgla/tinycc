/* A static function taking a struct larger than 16 bytes by value is an
   inline candidate.  The expansion must still bind a COPY: the callee may
   write its parameter, and the caller's object may change through a pointer
   the callee also holds, and neither may show on the other side.  The
   accessor shape Zig emits -- a whole struct passed to read one field -- has
   to come out as that field's load. */
#include <stdio.h>

struct block
{
  unsigned a, b, c, d, e;
  unsigned char tag;
  unsigned short w[5];
};

static unsigned block_src(const struct block blk) { return blk.c; }

static unsigned bump(struct block blk, unsigned k)
{
  blk.c += k; /* writes the copy only */
  return blk.c * 3 + blk.tag;
}

static unsigned alias(struct block blk, struct block *orig)
{
  orig->a = 1000;       /* the caller's object changes ... */
  return blk.a + orig->a; /* ... but the parameter keeps the value passed */
}

static struct block make(unsigned seed)
{
  struct block r = {seed, seed + 1, seed + 2, seed + 3, seed + 4, (unsigned char)(seed * 7), {1, 2, 3, 4, 5}};
  return r;
}

static unsigned sum_w(struct block blk)
{
  unsigned s = 0;
  for (int i = 0; i < 5; i++)
    s += blk.w[i];
  return s;
}

struct holder
{
  int pad;
  struct block blk;
};

static unsigned via_ptr(const struct holder *h) { return block_src(h->blk); }

int main(void)
{
  struct block b = make(10);
  struct holder h = {7, make(20)};
  printf("src %u\n", block_src(b));
  printf("bump %u c %u\n", bump(b, 5), b.c);
  unsigned r = alias(b, &b);
  printf("alias %u a %u\n", r, b.a);
  printf("w %u\n", sum_w(b));
  printf("ptr %u\n", via_ptr(&h));
  h.blk.c = 99;
  printf("ptr %u\n", via_ptr(&h));
  return 0;
}
