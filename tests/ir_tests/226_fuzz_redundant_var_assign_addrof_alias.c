/* Fuzz ptr seed 22: redundant VAR assign elimination must not drop a write when
 * address-of copies let later pointer dereferences read it. */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}

static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(pa) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  short s2 = (short)(1720817577u & 0xffff);
  unsigned u3 = 414747138u;
  unsigned u4 = 3846144942u;
  unsigned *p5 = &u4;
  unsigned *p6 = &u4;
  struct S st7 = {2703866056u, 1905182530u, 3174162897u};

  cs = csmix(cs, *p5);
  cs = csmix(cs, *p6);
  u4 = (unsigned)(((unsigned)(((unsigned)(2431279280u) & (unsigned)(st7.f0))) >>
                   ((unsigned)(2592723829u) & 31u))) &
       0xffffffffu;
  u4 = (unsigned)(((unsigned)(((unsigned)((*p6)) +
                               (unsigned)(((unsigned)((~((unsigned)(1525477429u) | 0u))) >>
                                           ((unsigned)((*p5)) & 31u))))) /
                   ((unsigned)(((unsigned)((*p6)) & (unsigned)(((unsigned)((*p6)) ^ cs)))) | 1u))) &
       0xffffffffu;
  cs = csmix(cs, (unsigned)((unsigned)(s2)));
  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, st7.f0);
  cs = csmix(cs, st7.f1);
  cs = csmix(cs, st7.f2);
  cs = csmix(cs, *p5);
  cs = csmix(cs, *p6);
  printf("checksum=%08x\n", cs);
  return 0;
}
