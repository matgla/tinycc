/* Regression guard for the value-read-operand extension to
 * tcc_ir_opt_memmove_global_load_fwd (ir/opt_memory.c).
 *
 * The base pass forwards `struct y = global; ... return y.field;` to a direct
 * global read only when the field read is a STANDALONE load.  A wide bitfield
 * (e.g. a 29-bit field of a 64-bit-bitfield struct) does not lower to a bare
 * LOAD: the deref is FUSED as an operand of the surrounding shift/mask/add
 * ((deref(y+4) >> 3) + x).  The extension forwards that fused-operand deref to
 * the global too, dropping the whole 8-byte memmove copy + stack frame -- this
 * is the 20040709-2 fn1 / fn2 idiom.
 *
 * Pins:
 *   (a) CORRECTNESS of the forwarded fused read across packed bitfield shapes
 *       (field at a byte/word offset, read inside +x / %15 arithmetic), and for
 *       BOTH the retme-identity (fn1) and no-call (fn2) shapes;
 *   (b) the source global is left untouched (no wild store-back to it), and a
 *       BSS canary array is not clobbered by a stray low-address store;
 *   (c) the snapshot-safety gate: a copy whose source is mutated by an
 *       intervening call must NOT forward (still observes the pre-mutation
 *       value).
 */
#include <stdio.h>

#define pck __attribute__((packed))

/* 64-bit bitfield: k is a 29-bit field whose read fuses into shift+mask. */
struct pck D { unsigned long long l : 6, i : 6, j : 23, k : 29; };
/* mixed: full u64 member then a 29-bit field at a word offset. */
struct pck E { unsigned long long l; unsigned long long i : 12, j : 23, k : 29; };
/* int + short-unit bitfield (k at byte offset 4). */
struct pck M { unsigned int l; unsigned short k : 6, j : 11, i : 15; };

struct D sD;
struct E sE;
struct M sM;

struct D retmeD(struct D x) { return x; }
struct E retmeE(struct E x) { return x; }
struct M retmeM(struct M x) { return x; }

/* fn1 shape: copy, modify field, identity round-trip, return field. */
unsigned int fn1D(unsigned int x) { struct D y = sD; y.k += x; y = retmeD(y); return y.k; }
unsigned int fn1E(unsigned int x) { struct E y = sE; y.k += x; y = retmeE(y); return y.k; }
unsigned int fn1M(unsigned int x) { struct M y = sM; y.k += x; y = retmeM(y); return y.k; }

/* fn2 shape: copy, modify field, mod, return field (no call). */
unsigned int fn2D(unsigned int x) { struct D y = sD; y.k += x; y.k %= 15; return y.k; }
unsigned int fn2M(unsigned int x) { struct M y = sM; y.k += x; y.k %= 15; return y.k; }

/* Canary: globals a stray low-address store could clobber. */
volatile unsigned int canary[8] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u,
                                   0x55555555u, 0x66666666u, 0x77777777u, 0x88888888u};

/* Snapshot must NOT be forwarded: bump() mutates sD between copy and read. */
void bumpD(void) { sD.k += 7; }
int snapshot_ok(void)
{
  sD.k = 0x100;
  struct D x = sD;   /* snapshot k == 0x100 */
  bumpD();           /* sD.k becomes 0x107 — a call after the copy */
  return (unsigned int)x.k == 0x100u;
}

int main(void)
{
  int ok = 1;
  unsigned int xs[] = {0u, 1u, 7u, 0x3ffu, 12345u, 0xffffu, 0xffffffffu};

  for (unsigned t = 0; t < sizeof(xs) / sizeof(xs[0]); t++)
  {
    unsigned int x = xs[t];

    sD.l = 0x2a; sD.i = 0x3b; sD.j = 0x5abcd; sD.k = 0x1abcdef;
    sE.l = 0xdeadbeefcafebabeull; sE.i = 0xabc; sE.j = 0x5abcd; sE.k = 0x1abcdef;
    sM.l = 0xfeedface; sM.k = 0x2a; sM.j = 0x5ab; sM.i = 0x7abc;

    if (fn1D(x) != ((0x1abcdefu + x) & 0x1fffffffu)) ok = 0;
    if (fn1E(x) != ((0x1abcdefu + x) & 0x1fffffffu)) ok = 0;
    if (fn1M(x) != ((0x2au + x) & 0x3fu)) ok = 0;
    if (fn2D(x) != ((((0x1abcdefu + x) & 0x1fffffffu) % 15u) & 0x1fffffffu)) ok = 0;
    if (fn2M(x) != ((((0x2au + x) & 0x3fu) % 15u) & 0x3fu)) ok = 0;

    /* The copy sources must be untouched — no wild store-back. */
    if (sD.l != 0x2a || sD.i != 0x3b || sD.j != 0x5abcd || sD.k != 0x1abcdef) ok = 0;
    if (sM.l != 0xfeedface || sM.k != 0x2a || sM.j != 0x5ab || sM.i != 0x7abc) ok = 0;
  }

  if (!snapshot_ok()) ok = 0;

  for (int i = 0; i < 8; i++)
    if (canary[i] != (unsigned int)(0x11111111u * (i + 1))) ok = 0;

  printf("%s\n", ok ? "OK" : "FAIL");
  return 0;
}
