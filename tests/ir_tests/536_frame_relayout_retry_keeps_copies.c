/* Frame relayout colours the frame, and when a STRUCT operand then sits below
   -32768 (past its 16-bit offset) it throws that layout away and colours
   again with those objects pinned.  Colouring deleted the copies it merged as
   it went -- `r = t` inside an inlined `return t` -- so the second layout no
   longer saw the copy, placed its two objects apart, and the struct the call
   returned into was not the one the next copy read.  Zig's Dir.deleteTree:
   Iterator.init returned into one slot, the stack item was filled from
   another, and the compiler crashed or hung on exit walking a garbage
   directory reader.  Needs a frame over 32 KB and a by-value struct
   argument. */
#include <stdio.h>
#include <string.h>

struct iter
{
  int fd;
  unsigned index, end;
  unsigned char state;
  unsigned char buf[2048];
};

struct item
{
  const char *name;
  int parent;
  struct iter iter;
};

__attribute__((noinline)) struct iter iter_init(int fd, unsigned char state)
{
  struct iter t;
  t.fd = fd;
  t.index = 0;
  t.end = 0;
  t.state = state;
  return t;
}

static struct iter iterate_first(int fd)
{
  struct iter t0;
  t0 = iter_init(fd, 1);
  return t0;
}

static struct item stack_items[4];
static unsigned stack_len;

__attribute__((noinline)) void append(struct item it)
{
  stack_items[stack_len++] = it;
}

__attribute__((noinline)) void scribble(unsigned char *p, unsigned n)
{
  memset(p, 0x5a, n);
}

__attribute__((noinline)) unsigned walk(const char *name, int fd)
{
  struct iter t15;
  struct item t16;
  unsigned char big[40000];
  scribble(big, sizeof big);
  t15 = iterate_first(fd);
  t16.name = name;
  t16.parent = fd - 1;
  t16.iter = t15;
  append(t16);
  t15 = iterate_first(fd + 1);
  t16.parent = fd;
  t16.iter = t15;
  append(t16);
  return big[123] + big[39999];
}

int main(void)
{
  unsigned junk = walk("tmp", 7);
  for (unsigned i = 0; i < stack_len; i++)
    printf("item %u: parent=%d fd=%d index=%u end=%u state=%u\n", i, stack_items[i].parent, stack_items[i].iter.fd,
           stack_items[i].iter.index, stack_items[i].iter.end, stack_items[i].iter.state);
  printf("junk=%u\n", junk);
  return 0;
}
