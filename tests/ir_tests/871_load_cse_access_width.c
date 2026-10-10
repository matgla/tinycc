/* Load CSE / load hoisting must key a load on what it READS -- the width and
 * extension of the load's SOURCE operand (a LOAD_INDEXED's dest) -- never on
 * its dest btype alone.  An LDRB, an LDRSB, an LDRH and an LDR of one address
 * all produce an int-typed TEMP and are four different values.
 *   ssa:cprop   cprop_load_redundant   matched deref loads by pointer vreg only
 *   ssa:load_cse iload/gload/tvstore   keyed on the dest btype, no sign
 *   invariant_global_load_hoist        keyed on the dest btype, no sign
 * ssa:load_combine made the first one fire on ordinary readInt code (the wide
 * LOAD of p next to a surviving p[0]); the rest were latent in base. */
#include <stdio.h>
#include <stdint.h>
#define NI __attribute__((noinline))
static uint8_t buf[64];
static uint32_t wbuf[8];
volatile int sink;
/* signedness through one pointer */
NI int32_t sgn(const uint8_t *p) { uint32_t a = p[0]; int32_t b = ((const int8_t *)p)[0]; return (int32_t)a * 3 + b; }
NI int32_t sgn2(const uint8_t *p) { int32_t b = ((const int8_t *)p)[0]; uint32_t a = p[0]; return (int32_t)a * 3 + b; }
NI int32_t sgn16(const uint8_t *p) { uint32_t a = *(const uint16_t *)(const void *)p; int32_t b = *(const int16_t *)(const void *)p; return (int32_t)a * 3 + b; }
/* across blocks: byte read in a dominating block */
NI uint32_t xblk(const uint8_t *p, int c)
{
  uint32_t pre = p[0];
  if (c) sink = 1;
  uint32_t v = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
  return v + pre;
}
NI uint32_t xblk2(const uint8_t *p, int c)
{
  uint32_t v = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
  if (c) sink = 1;
  uint32_t post = p[0];
  uint32_t h = p[0] | (uint32_t)p[1] << 8;
  return v + post * 7 + h;
}
/* high byte read on its own, and middle */
NI uint32_t mid(const uint8_t *p)
{
  uint32_t b1 = p[1], b3 = p[3];
  uint32_t v = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
  return v ^ (b1 << 4) ^ (b3 << 9);
}
/* two different chains on one pointer: half and word, then half again */
NI uint32_t hw(const uint8_t *p)
{
  uint32_t h = p[0] | (uint32_t)p[1] << 8;
  uint32_t w = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
  uint32_t h2 = p[0] | (uint32_t)p[1] << 8;
  return h * 5 + w + h2 * 11;
}
/* word load then byte reads through a uint32 pointer cast */
NI uint32_t wthenb(const uint32_t *q)
{
  uint32_t w = q[0];
  const uint8_t *p = (const uint8_t *)q;
  uint32_t v = p[0] | (uint32_t)p[1] << 8;
  return w ^ (v << 3);
}
/* float vs int at one address */
NI uint32_t fpun(const uint32_t *q)
{
  uint32_t a = q[0];
  float f = *(const float *)(const void *)q;
  return a + (uint32_t)(int32_t)(f * 0.0f + 2.0f);
}
/* in a loop */
NI uint32_t loopy(const uint8_t *p, int n)
{
  uint32_t s = 0;
  for (int i = 0; i < n; i++, p += 4)
  {
    uint32_t pre = p[0];
    uint32_t v = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    s = s * 31 + v + pre;
  }
  return s;
}
static uint32_t gw[4];
NI uint32_t st_b(uint8_t *p, uint32_t x, int k)
{
  uint8_t *q = p + k;
  *(uint32_t *)(void *)q = x;
  return q[0] + q[1] * 3u;
}
NI uint32_t st_b2(uint32_t *p, uint32_t x)
{
  *p = x;
  const uint8_t *b = (const uint8_t *)p;
  return b[0] + b[1] * 3u + (uint32_t)((const int8_t *)p)[0];
}
NI uint32_t glob(void)
{
  const uint8_t *g = (const uint8_t *)gw;
  uint32_t a = g[0];
  uint32_t w = gw[0];
  int32_t s = ((const int8_t *)gw)[0];
  uint32_t h = ((const uint16_t *)gw)[0];
  return a + w + (uint32_t)s * 5u + h * 9u;
}
NI uint32_t glob2(void)
{
  uint32_t w = gw[1];
  uint32_t h = ((const uint16_t *)gw)[2];
  int32_t sh = ((const int16_t *)gw)[2];
  return w + h * 3u + (uint32_t)sh * 7u;
}
int main(void)
{
  for (int k = 0; k < 64; k++) buf[k] = (uint8_t)(k * 151 + 0x37);
  uint8_t *wb = (uint8_t *)wbuf;
  for (int k = 0; k < 32; k++) wb[k] = buf[k + 3];
  for (int o = 0; o < 3; o++)
  {
    const uint8_t *p = buf + 1 + o * 5;
    printf("%d sgn=%d %d %d xblk=%08x %08x %08x %08x mid=%08x hw=%08x\n", o, (int)sgn(p), (int)sgn2(p),
           (int)sgn16(wb + 2 * o), (unsigned)xblk(p, o & 1), (unsigned)xblk(p, 1), (unsigned)xblk2(p, 0),
           (unsigned)xblk2(p, 1), (unsigned)mid(p), (unsigned)hw(p));
    printf("%d wthenb=%08x fpun=%08x loopy=%08x\n", o, (unsigned)wthenb(wbuf + o), (unsigned)fpun(wbuf + o),
           (unsigned)loopy(p, 3 + o));
  }
  {
  static uint32_t b[4];
  for (int k = 0; k < 4; k++) gw[k] = 0x80f1e2d3u ^ (k * 0x01010101u);
  printf("%08x %08x %08x %08x\n", (unsigned)st_b((uint8_t *)b, 0x9abcdef0u, 4), (unsigned)st_b2(b + 2, 0x80ff7f81u),
         (unsigned)glob(), (unsigned)glob2());
  }
  return 0;
}
