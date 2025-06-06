/* Regression guard for the precise vreg-deref dead-store elimination in
 * ir/opt_memory.c (dls_vreg_frame_off + the known-offset STORE elim).
 *
 * The 20040709-2 fn1/fn2 shape `struct y = g; y.f += x; return y.f;` leaves a
 * dead write-back to the local copy `y`.  Eliminating it lets the init copy
 * forward to a direct global load (GCC-level codegen) AND removes a latent WILD
 * STORE: the RA, treating the dead store as dead, otherwise reuses its base
 * register and writes `y` back to a garbage low address.
 *
 * This checks, across several field offsets/widths, that (a) the returned value
 * is correct and (b) NEIGHBOURING globals are not corrupted by a stray store
 * (the wild store landed at ~`result & 0x7ff`, a low address).  A canary array
 * placed in BSS catches such a stray write.
 */
#include <stdio.h>

#define pck __attribute__((packed))

struct pck A { unsigned short i : 1, j : 4, k : 11; };          /* k @5  w11 */
struct pck B { unsigned int l; unsigned short i : 4, k : 11; }; /* k @4(+4) w11, multi-unit */
struct pck C { unsigned int k : 6, j : 11, i : 15; };          /* k @0  w6  */

struct A gA;
struct B gB;
struct C gC;

unsigned int addA(unsigned int x) { struct A y = gA; y.k += x; return y.k; }
unsigned int addB(unsigned int x) { struct B y = gB; y.k += x; return y.k; }
unsigned int addC(unsigned int x) { struct C y = gC; y.k += x; return y.k; }

/* Canary: a global the wild store (to a low/garbage address) could clobber. */
volatile unsigned int canary[8] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u,
                                   0x55555555u, 0x66666666u, 0x77777777u, 0x88888888u};

int main(void)
{
  int ok = 1;
  unsigned int xs[] = {0u, 1u, 7u, 0x3ffu, 0x7ffu, 12345u, 0xffffu, 0xffffffffu};

  for (unsigned t = 0; t < sizeof(xs) / sizeof(xs[0]); t++)
  {
    unsigned int x = xs[t];
    gA.i = 1; gA.j = 9; gA.k = 0x37a;
    gB.l = 0xdeadbeefu; gB.i = 0xa; gB.k = 0x1c3;
    gC.k = 0x2a; gC.j = 0x5ab; gC.i = 0x7abc;

    if (addA(x) != ((0x37au + x) & 0x7ffu)) ok = 0;
    if (addB(x) != ((0x1c3u + x) & 0x7ffu)) ok = 0;
    if (addC(x) != ((0x2au + x) & 0x3fu)) ok = 0;

    /* Globals (the copy sources) must be untouched — no wild store-back. */
    if (gA.i != 1 || gA.j != 9 || gA.k != 0x37a) ok = 0;
    if (gB.l != 0xdeadbeefu || gB.i != 0xa || gB.k != 0x1c3) ok = 0;
    if (gC.k != 0x2a || gC.j != 0x5ab || gC.i != 0x7abc) ok = 0;
  }

  for (int i = 0; i < 8; i++)
    if (canary[i] != (unsigned int)(0x11111111u * (i + 1))) ok = 0;

  printf("%s\n", ok ? "OK" : "FAIL");
  return 0;
}
