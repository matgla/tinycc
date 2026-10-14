/* A packed scalar_storage_order("big-endian") bitfield group was accessed through
 * a power-of-two unit anchored at the group's first byte, which ran past the end
 * of the struct (ICE "initializer overflow" in a static initializer, an
 * out-of-bounds access at run time).  The unit is now anchored so it stays inside
 * the struct. */
#include <stdio.h>
#include <string.h>

struct __attribute__((scalar_storage_order("big-endian"), packed)) Nested2
{
  unsigned C1 : 7;
  unsigned C2 : 7;
  unsigned C3 : 7;
  unsigned B : 3;
};

struct __attribute__((scalar_storage_order("big-endian"), packed)) R2
{
  unsigned S1 : 6;
  unsigned I : 32;
  unsigned S2 : 2;
  struct Nested2 N;
};

struct R2 g = {5, 0x12345678u, 2, {0x11, 0x22, 0x33, 5}};

static void dump(const char *tag, const void *p, unsigned n)
{
  const unsigned char *b = p;
  printf("%s", tag);
  for (unsigned i = 0; i < n; i++)
    printf(" %02x", b[i]);
  printf("\n");
}

__attribute__((noinline)) void fill(struct R2 *r)
{
  memset(r, 0xa5, sizeof *r);
  r->S1 = 5;
  r->I = 0x12345678u;
  r->S2 = 2;
  r->N.C1 = 0x11;
  r->N.C2 = 0x22;
  r->N.C3 = 0x33;
  r->N.B = 5;
}

int main(void)
{
  struct R2 l;
  printf("size %u\n", (unsigned)sizeof(struct R2));
  dump("static", &g, sizeof g);
  fill(&l);
  dump("local ", &l, sizeof l);
  printf("%u %x %u %u %u %u %u\n", l.S1, l.I, l.S2, l.N.C1, l.N.C2, l.N.C3, l.N.B);
  printf("%u %x %u %u %u %u %u\n", g.S1, g.I, g.S2, g.N.C1, g.N.C2, g.N.C3, g.N.B);
  return 0;
}
