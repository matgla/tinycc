/* A value stored to a byte/halfword slot and read straight back must come back
 * truncated and sign/zero-extended, at every -O level: ssa:load_cse forwarded
 * the full-width TEMP (frame, global and pointer slots), and the codegen
 * store->reload peephole dropped the reload of a word-aligned frame slot
 * (docs/bugs/ssa-load-cse-narrow-frame-store-forward-no-extend and
 * codegen-o0-narrow-store-reload-elided-no-extend, fixed). */
#include <stdio.h>

__attribute__((noinline)) int a2(int k) { signed char b[4]; b[0] = k; return b[0]; }
__attribute__((noinline)) int a3(int k) { unsigned char b[4]; b[0] = k; return b[0]; }
__attribute__((noinline)) int a4(int k) { short b[4]; b[0] = k; return b[0]; }
__attribute__((noinline)) int a5(int k) { unsigned short b[4]; b[0] = k; return b[0]; }

signed char gs;
unsigned short gu;
__attribute__((noinline)) int g1(int k) { gs = k; return gs; }
__attribute__((noinline)) int g2(int k) { gu = k; return gu; }
__attribute__((noinline)) int p1(signed char *p, int k) { *p = k; return *p; }
__attribute__((noinline)) int p2(short *p, int k) { *p = k; return *p; }
/* a value both extensions agree on still reads back unchanged */
__attribute__((noinline)) int a6(void) { signed char b[4]; b[0] = 100; return b[0]; }
__attribute__((noinline)) int a7(void) { signed char b[4]; b[0] = 200; return b[0]; }

int main(void)
{
  signed char c;
  short s;
  printf("%d %d %d %d\n", a2(200), a3(300), a4(40000), a5(70000));
  printf("%d %d %d %d\n", g1(200), g2(70000), p1(&c, 255), p2(&s, 40000));
  printf("%d %d\n", a6(), a7());
  return 0;
}
