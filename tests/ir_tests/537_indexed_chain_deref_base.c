/* indexed_chain folds `T <- B ADD #k1; LOAD_INDEXED [T, #k2]` into
   `LOAD_INDEXED [B, #(k1+k2)]`.  When the ADD's operand is a memory read --
   `T <- P***DEREF*** ADD #k1`, the value stored at P plus k1 -- the fold
   dropped that read and addressed P + k1 + k2 instead.  Here P is &l, so
   `l.ptr[1].b` read the frame slot 12 bytes past l rather than the array
   element: a local struct holding a pointer, kept in memory because its
   address escapes, indexed with a constant and used as an arithmetic
   operand. */
#include <stdio.h>

struct elem
{
  unsigned a, b, c;
};

struct list
{
  struct elem *ptr;
  unsigned len;
};

static struct elem items[3] = {{1, 2, 3}, {40, 50, 60}, {700, 800, 900}};

__attribute__((noinline)) void touch(struct list *l)
{
  l->len = 3;
}

__attribute__((noinline)) unsigned pick(unsigned k)
{
  struct list l;
  l.ptr = items;
  l.len = 0;
  touch(&l);
  return l.ptr[1].b * 10u + l.ptr[2].c * k + l.len;
}

int main(void)
{
  printf("pick=%u\n", pick(2));
  printf("pick=%u\n", pick(5));
  return 0;
}
