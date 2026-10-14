/* ssa:setif_mask_fold on a compare that itself carries a barrel-shift
 * annotation (reduced from a generated program).  Barrel-shift fusion folded a
 * shift of a SETIF-derived value into the outer CMP's src2; the fold then
 * treated that src2 as the plain boolean and rewrote the CMP's operands under
 * the stale annotation -- an immediate under the shift, which a debug cross
 * aborts on ("immediate substituted into barrel-shift-annotated src2"), or a
 * compare of the wrong value in a release build. */
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

NI int16_t f14(int i, int j, int8_t pa, uint8_t pb, int16_t pc, uint16_t pd) {
  struct BF *bp = &g_bf[j]; struct S *sp = &g_s[i]; int32_t s = 0;
  uint8_t v0 = (uint8_t)((((uint16_t)(int8_t)((int32_t)((uint32_t)((int32_t)((uint32_t)(pb) << 31)) << 16) >> 16)) != ((int32_t)((uint32_t)((uint8_t)((_Bool)(g_u32[j]))) << 17))));
  int16_t v1 = (int16_t)((uint16_t)(((1) ? (bp->h) : (v0))));
  uint16_t v2 = (uint16_t)(((((int32_t)((uint8_t)(uint16_t)(sp->u8)) >> 25)) ? ((int32_t)((uint32_t)((int32_t)((uint32_t)(pc) >> 31)) * (uint32_t)((int32_t)((uint32_t)(bp->g) + (uint32_t)(bp->b))))) : ((int16_t)(((-128) == (g_u32[(j+3)%NW]))))));
  uint8_t v3 = (uint8_t)((((int16_t)(uint16_t)(g_i8[i])) ^ ((int32_t)((uint32_t)(v2) * (uint32_t)(pa)))));
  s = (int32_t)((uint32_t)s * 1000003u + (uint32_t)((int16_t)(uint8_t)(((int32_t)((uint8_t)(v3)) >> 25))));
  s = (int32_t)((uint32_t)s * 1000003u + (uint32_t)((uint8_t)((int32_t)(((int32_t)(((int32_t)(g_i16[i]) >> 24)) >> 25)))));
  s = (int32_t)((uint32_t)s * 1000003u + (uint32_t)((((int32_t)((uint32_t)((((uint16_t)(pc)) | ((int32_t)((uint32_t)(bp->b) - (uint32_t)(v0))))) * (uint32_t)((_Bool)((int32_t)(g_u8[(i+1)%NW]))))) != (((int32_t)((int32_t)((uint32_t)((int32_t)((uint32_t)(bp->f) - (uint32_t)(g_i8[i]))) >> 7)) >> 8)))));
  if (s & 1) return (int16_t)((int32_t)((uint32_t)((int32_t)((uint32_t)(g_i16[j]) << 23)) + (uint32_t)((int32_t)(r_i8(j)))));
  return (int16_t)((uint32_t)s + (uint32_t)(uint8_t)((uint16_t)(uint8_t)((int8_t)(sp->w))));
}
int main(void){
  for (int i = 0; i < NW; i++) { uint32_t w = W[i]; g_u8[i]=w; g_i8[i]=w; g_u16[i]=w; g_i16[i]=w; g_u32[i]=w; g_i32[i]=w;
    g_bf[i].a=w; g_bf[i].b=w>>3; g_bf[i].c=w>>5; g_bf[i].d=w>>9; g_bf[i].e=w>>2; g_bf[i].f=w>>11; g_bf[i].g=w>>1; g_bf[i].h=w>>13;
    g_s[i].w=w*2654435761u; g_s[i].s16=w>>3; g_s[i].u16=w>>1; g_s[i].s8=w>>4; g_s[i].u8=w>>6; }
  uint32_t h = 0;
  for (int i = 0; i < NW; i++) {
    for (int j = 0; j < NW; j++) {
      int16_t r = f14(i, j, (int8_t)W[j], (uint8_t)W[(j+1)%NW], (int16_t)W[(j+2)%NW], (uint16_t)W[i]);
      h = h * 31u + (uint16_t)r;
    }
    printf("%d %08x\n", i, (unsigned)h);
  }
  return 0; }
