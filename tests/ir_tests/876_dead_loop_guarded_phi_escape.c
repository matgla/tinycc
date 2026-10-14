/* ssa:dead_loop, unprovable trip count (rewrite_loop_exit_phis_guarded): a loop
 * whose only post-loop results are header phis with a constant preheader and a
 * constant latch value (`acc = 0xff` every iteration) becomes one SELECT per
 * such phi and the loop is deleted.  The pass only looked at those candidate
 * phis and at the body TEMPs; a header phi with a NON-constant latch value
 * that is still read after the loop (the accumulator `s`, or the IV itself)
 * lost its back-edge operand and collapsed to its initial value.  k1 is the
 * generated program (reachable once ext_elim proves `(uint8_t)(v2 >> 31)` is
 * 0); k3 is the same with the 0 written out, wrong before ext_elim existed. */
#include <stdint.h>
#include <stdio.h>
#define NI __attribute__((noinline))
static const uint32_t W[] = {0,1,0x7f,0x80,0xff,0x100,0x7fff,0x8000,0xffff,0x10000,0x7fffffff,0x80000000u,0xffffffffu,0xffffff80u,0xffff8000u,0x12345678u,0xdeadbeefu,0x80808080u,0x7f7f7f7fu,0x00ff00ffu,0xff7fff80u,0x0080ff7fu};
#define NW (int)(sizeof W/sizeof W[0])
uint8_t g_u8[NW]; int8_t g_i8[NW]; uint16_t g_u16[NW]; int16_t g_i16[NW]; uint32_t g_u32[NW]; int32_t g_i32[NW];
struct BF { int a:5; unsigned b:3; int c:8; unsigned d:8; int e:16; unsigned f:16; int g:7; unsigned h:7; } g_bf[NW];
struct S { uint32_t w; int16_t s16; uint16_t u16; int8_t s8; uint8_t u8; } g_s[NW];
NI int8_t r_i8(int i){ return (int8_t)g_u32[i]; }
NI uint8_t r_u8(int i){ return (uint8_t)(g_u32[i] >> 3); }
NI int16_t r_i16(int i){ return (int16_t)(g_u32[i] >> 5); }
NI uint16_t r_u16(int i){ return (uint16_t)g_u32[i]; }
NI _Bool r_b(int i){ return g_u32[i] & 4; }
NI uint8_t k1(int i, int j, int8_t pa, uint8_t pb, int16_t pc, uint16_t pd) {
  struct BF *bp = &g_bf[j]; struct S *sp = &g_s[i]; int32_t s = 0;
  uint8_t v2 = (uint8_t)(1 >= ((int8_t)(int16_t)((r_u16(i) == g_i16[i]))));
  uint8_t acc = (uint8_t)(((int32_t)(v2) >> 31));
  for (int k = 0; k < (i & 7); k++) {
    acc = 0xff;
    s = (int32_t)((uint32_t)s * 31u + (uint32_t)((int16_t)(int8_t)(bp->c)));
  }
  uint8_t q;
  if (g_i8[(j+3)%NW]) q = (uint8_t)((int8_t)(((g_u32[(i+1)%NW]) ^ (acc)))); else q = 5;
  s = (int32_t)((uint32_t)s * 1000003u + (uint32_t)((uint16_t)(uint16_t)(q)));
  return (uint8_t)s;
}
NI uint8_t k3(int i, int j, int8_t pa, uint8_t pb, int16_t pc, uint16_t pd) {
  struct BF *bp = &g_bf[j]; struct S *sp = &g_s[i]; int32_t s = 0;
  uint8_t v2 = (uint8_t)(1 >= ((int8_t)(int16_t)((r_u16(i) == g_i16[i]))));
  uint8_t acc = 0;
  for (int k = 0; k < (i & 7); k++) {
    acc = 0xff;
    s = (int32_t)((uint32_t)s * 31u + (uint32_t)((int16_t)(int8_t)(bp->c)));
  }
  uint8_t q;
  if (g_i8[(j+3)%NW]) q = (uint8_t)((int8_t)(((g_u32[(i+1)%NW]) ^ (acc)))); else q = 5;
  s = (int32_t)((uint32_t)s * 1000003u + (uint32_t)((uint16_t)(uint16_t)(q)));
  return (uint8_t)s;
}

/* The same escape in its smallest form, and with the IV read after the loop. */
signed char gc[4] = {3, -2, 5, 1};

NI unsigned acc_escape(int n, const signed char *p)
{
  unsigned s = 0;
  unsigned char flag = 0;
  for (int k = 0; k < (n & 7); k++)
  {
    flag = 0xff;
    s = s * 31u + (unsigned)*p;
  }
  return s * 1000u + flag;
}

NI int iv_escape(int n)
{
  int k;
  unsigned char flag = 0;
  for (k = 0; k < (n & 7); k++)
    flag = 7;
  return k * 100 + flag;
}

int main(void){
  for (int i = 0; i < NW; i++) { uint32_t w = W[i]; g_u8[i]=w; g_i8[i]=w; g_u16[i]=w; g_i16[i]=w; g_u32[i]=w; g_i32[i]=w;
    g_bf[i].a=w; g_bf[i].b=w>>3; g_bf[i].c=w>>5; g_bf[i].d=w>>9; g_bf[i].e=w>>2; g_bf[i].f=w>>11; g_bf[i].g=w>>1; g_bf[i].h=w>>13;
    g_s[i].w=w*2654435761u; g_s[i].s16=w>>3; g_s[i].u16=w>>1; g_s[i].s8=w>>4; g_s[i].u8=w>>6; }
  for (int i = 0; i < NW; i++) {
    uint32_t h1 = 0, h3 = 0;
    for (int j = 0; j < NW; j++) {
      h1 = h1 * 31u + k1(i, j, (int8_t)W[j], (uint8_t)W[(j+1)%NW], (int16_t)W[(j+2)%NW], (uint16_t)W[i]);
      h3 = h3 * 31u + k3(i, j, (int8_t)W[j], (uint8_t)W[(j+1)%NW], (int16_t)W[(j+2)%NW], (uint16_t)W[i]);
    }
    printf("%d %08x %08x\n", i, (unsigned)h1, (unsigned)h3);
  }
  for (int n = -1; n <= 9; n++)
    printf("n=%d acc=%u iv=%d\n", n, acc_escape(n, &gc[n & 3]), iv_escape(n));
  return 0; }
