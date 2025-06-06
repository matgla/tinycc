/* Regression guard for the generalized bitfield insert/extract fold in
 * ir/opt_bitfield.c (tcc_ir_opt_bitfield_insert_extract).
 *
 * The "copy a struct to a local, poke one bitfield, return it" idiom expands to
 * an in-register insert (`(word & clearmask) | (V << off)`) immediately followed
 * by a re-extract.  For a field that is NOT at bit 0 the extract is the two-shift
 * form `(word << (32-(off+width))) >> (32-width)`, which the fold must recognize
 * (peeling the outer SHL and following the ASSIGN copy sl_forward leaves behind)
 * and collapse back to V.  These cases check that the RESULT is still correct for
 * fields at several offsets/widths, i.e. the fold does not drop high bits, read
 * the wrong window, or otherwise miscompile.
 *
 * Each fnX reads field k of a global struct, adds x, and returns k (masked to the
 * field width by C semantics).  main() compares against an explicit reference.
 */
#include <stdio.h>

#define pck __attribute__((packed))

/* k at offset 5, width 11 (the 20040709-2 fn1A shape) */
struct pck A { unsigned short i : 1, l : 1, j : 3, k : 11; };
/* k at offset 6, width 11 */
struct pck B { unsigned int j : 6, k : 11, i : 15; };
/* k at offset 12, width 13 */
struct pck C { unsigned int j : 12, k : 13, i : 7; };
/* k at the bottom (offset 0), width 12 */
struct pck D { unsigned int k : 12, j : 13, i : 7; };

struct A sA;
struct B sB;
struct C sC;
struct D sD;

unsigned int fnA(unsigned int x) { struct A y = sA; y.k += x; return y.k; }
unsigned int fnB(unsigned int x) { struct B y = sB; y.k += x; return y.k; }
unsigned int fnC(unsigned int x) { struct C y = sC; y.k += x; return y.k; }
unsigned int fnD(unsigned int x) { struct D y = sD; y.k += x; return y.k; }

int main(void)
{
  int ok = 1;

  sA.k = 0x37a; sA.i = 1; sA.l = 1; sA.j = 5;
  sB.k = 0x37a; sB.j = 0x2a; sB.i = 0x7abc;
  sC.k = 0x1abc; sC.j = 0xabc; sC.i = 0x55;
  sD.k = 0xabc; sD.j = 0x1abc; sD.i = 0x33;

  unsigned int xs[] = {0u, 1u, 7u, 0x3ffu, 0x7ffu, 12345u, 0xffffu, 0xffffffffu};
  for (unsigned i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
    unsigned int x = xs[i];
    if (fnA(x) != ((0x37au + x) & 0x7ff)) ok = 0;
    if (fnB(x) != ((0x37au + x) & 0x7ff)) ok = 0;
    if (fnC(x) != ((0x1abcu + x) & 0x1fff)) ok = 0;
    if (fnD(x) != ((0xabcu + x) & 0xfff)) ok = 0;
  }

  printf("%s\n", ok ? "OK" : "FAIL");
  return 0;
}
