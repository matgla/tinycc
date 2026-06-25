/* Regression guard for the bitfield-insert -> ARM BFI lowering in
 * ir/opt_bitfield.c (tcc_ir_opt_bitfield_insert_to_bfi).
 *
 * The "modify one bitfield of a global, observe the result" idiom
 * (`s.k += x; return s.k;` — the 20040709-2 fn3 shape) leaves an in-register
 * insert `(word & clearmask) | (V << lsb)` whose result escapes to a store, so
 * the extract fold cannot collapse it.  This pass rewrites it to a single
 * `BFI Rd, V, #lsb, #width`.  The transform is a pure algebraic identity, so
 * these cases check that:
 *   - the inserted field gets the right value (no dropped high bits, right lsb);
 *   - the NEIGHBOURING fields are untouched (BFI replaces only [lsb,lsb+width));
 *   - lsb==0 (no SHL) and lsb>0 shapes both work;
 *   - the gate's non-firing cases (small field whose host word has no bits in
 *     the field region) still compute correctly.
 *
 * Fields chosen so the clearmask is NOT a Thumb-2 modified immediate (the gate),
 * i.e. the insert really lowers to BFI.
 */
#include <stdio.h>

#define pck __attribute__((packed))

/* k at offset 5, width 11 (clearmask 0xffff001f, non-encodable -> BFI) */
struct pck A { unsigned short i : 1, l : 1, j : 3, k : 11; };
/* k at offset 8, width 8 (clearmask 0xffff00ff -> BFI) */
struct pck B { unsigned int i : 6, j : 2, k : 8, l : 16; };
/* k at the bottom, offset 0, width 6 (clearmask 0xffffffc0 -> BFI, lsb==0) */
struct pck C { unsigned int k : 6, j : 11, i : 15; };
/* k at offset 12, width 13 (clearmask 0xfe000fff -> BFI) */
struct pck D { unsigned int j : 12, k : 13, i : 7; };

struct A sA;
struct B sB;
struct C sC;
struct D sD;

unsigned int fA(unsigned int x) { sA.k += x; return sA.k; }
unsigned int fB(unsigned int x) { sB.k += x; return sB.k; }
unsigned int fC(unsigned int x) { sC.k += x; return sC.k; }
unsigned int fD(unsigned int x) { sD.k += x; return sD.k; }

int main(void)
{
  int ok = 1;

  unsigned int xs[] = {0u, 1u, 7u, 0x3ffu, 0x7ffu, 12345u, 0xffffu, 0xffffffffu};
  for (unsigned t = 0; t < sizeof(xs) / sizeof(xs[0]); t++) {
    unsigned int x = xs[t];

    /* Reset to known field contents (with non-trivial neighbour values). */
    sA.i = 1; sA.l = 1; sA.j = 5; sA.k = 0x37a;
    sB.i = 0x2a; sB.j = 3; sB.k = 0xb7; sB.l = 0xabcd;
    sC.k = 0x2a; sC.j = 0x5ab; sC.i = 0x7abc;
    sD.j = 0xabc; sD.k = 0x1abc; sD.i = 0x55;

    /* Insert result correct. */
    if (fA(x) != ((0x37au + x) & 0x7ffu)) ok = 0;
    if (fB(x) != ((0xb7u + x) & 0xffu)) ok = 0;
    if (fC(x) != ((0x2au + x) & 0x3fu)) ok = 0;
    if (fD(x) != ((0x1abcu + x) & 0x1fffu)) ok = 0;

    /* Neighbour fields untouched by the insert. */
    if (sA.i != 1 || sA.l != 1 || sA.j != 5) ok = 0;
    if (sB.i != 0x2a || sB.j != 3 || sB.l != 0xabcd) ok = 0;
    if (sC.j != 0x5ab || sC.i != 0x7abc) ok = 0;
    if (sD.j != 0xabc || sD.i != 0x55) ok = 0;
  }

  printf("%s\n", ok ? "OK" : "FAIL");
  return 0;
}
