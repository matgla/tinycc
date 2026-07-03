#include <stdio.h>

/*
 * Fuzz ptr seed 6734 reduction: ssa:cprop's redundant-load CSE reused the
 * first `*p6` deref read across a plain ALU def of the address-taken pointee
 * (`u5 = ... - s1` compiles to `V5 <-- T SUB #imm`, not a STORE op), so the
 * second `st7.f1 = *p6` captured u5's pre-update value at -O2.
 */
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  int s1 = (int)(1901264714u & 0xffffffff);
  unsigned u2 = 3533422040u;
  unsigned u3 = 3727239771u;
  unsigned u4 = 1271696229u;
  unsigned u5 = 1861731958u;
  unsigned *p6 = &u5;
  struct S st7 = { 194296390u, 4059921506u, 2926510190u };
  st7.f1 = (unsigned)((*p6));
  u5 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)((*p6)) + (unsigned)(1523748975u))) - (unsigned)(u4))) % ((unsigned)(st7.f2) | 1u))) - (unsigned)((unsigned)(s1)))) & 0xffffffffu;
  st7.f1 = (unsigned)((*p6));
  cs = csmix(cs, u2);
  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, (unsigned)s1);
  cs = csmix(cs, st7.f0);
  cs = csmix(cs, st7.f1);
  cs = csmix(cs, st7.f2);
  cs = csmix(cs, *p6);
  printf("checksum=%08x\n", cs);
  return 0;
}
