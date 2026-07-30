/* global_deref_cse + bitfield_unit_narrow: a whole-word bitfield
 * read-modify-write whose field exactly fills one aligned byte/halfword is
 * rewritten into a narrow load and a narrow store of that unit.
 *
 * Every check must hold at every -O level, so a narrowing that picks the wrong
 * unit, drops a value bit, or clobbers a neighbouring field fails here rather
 * than silently shrinking code.
 *
 * The shapes, and the wrong-code hazards each one pins:
 *  1. byte/halfword-sized fields at each aligned offset of the storage word
 *     (bits 0, 16 and 24), with the NEIGHBOURING fields checked after every
 *     update — a wrong unit offset or a too-wide store shows up there and
 *     nowhere else.  The struct leads with a `long long` so the bitfield word
 *     sits at a non-zero addend, which is what makes the unit offset
 *     arithmetic (addend + lsb/8) load-bearing.
 *  2. values that overflow the field (`+= 0x1234` into 8 bits): the narrow
 *     store truncates, and the original word merge must agree, so the pass
 *     may only drop the mask when the value provably fits.
 *  3. `&=` / `|=` / `^=`, where the frontend leaves the field mask IMPLICIT
 *     (the operand is already field-width) — the provably-in-range path — and
 *     `/=` / `%=`, which route the value through a helper call.
 *  4. a read of the same field in the same expression as the write, so the
 *     narrowed load has to serve both the extraction and the merge.
 *  5. fields NOT filling a unit (6/11/15 bits, and a 1-bit field), which must
 *     be left alone: they are here to catch a unit test that matches too
 *     eagerly on the clear mask.
 *  6. a volatile bitfield, which must keep its declared word access.
 */
#include <stdio.h>

struct S {
  long long l;
  unsigned int i : 16, j : 8, k : 8;
};

struct N {
  long long l;
  unsigned int i : 6, j : 11, k : 15;
};

struct B {
  long long l;
  unsigned int i : 5, j : 1, k : 26;
};

struct S d;
struct N b;
struct B c;
volatile struct S vd;

static void reset(void)
{
  d.l = 0x1122334455667788LL;
  d.i = 26812; d.j = 156; d.k = 187;
  b.l = -1;
  b.i = 51; b.j = 636; b.k = 31278;
  c.l = 7;
  c.i = 21; c.j = 1; c.k = 33554432;
}

/* 1 + 2: unit-sized fields, each neighbour re-checked after the update. */
void add_i(unsigned x) { d.i += x; }
void add_j(unsigned x) { d.j += x; }
void add_k(unsigned x) { d.k += x; }
void set_j(unsigned x) { d.j = x; }

/* 3: implicit-mask forms and the helper-call forms. */
void and_j(unsigned x) { d.j &= x; }
void ior_j(unsigned x) { d.j |= x; }
void xor_j(unsigned x) { d.j ^= x; }
void div_k(unsigned x) { d.k /= x; }
void rem_i(unsigned x) { d.i %= x; }

/* 4: the field is read and written in one expression. */
void selfmix(unsigned x) { d.j = (d.j << 1) + x; }

/* 5: sub-unit fields that must NOT narrow. */
void add_ni(unsigned x) { b.i += x; }
void add_nj(unsigned x) { b.j += x; }
void add_nk(unsigned x) { b.k += x; }
void add_cj(unsigned x) { c.j += x; }

/* 6: volatile keeps its declared access. */
void add_vj(unsigned x) { vd.j += x; }

#define SHOW(tag) printf(tag "=%u %u %u %lld\n", d.i, d.j, d.k, d.l)

int main(void)
{
  reset(); add_i(713);    SHOW("ai");
  reset(); add_j(17);     SHOW("aj");
  reset(); add_k(199);    SHOW("ak");
  reset(); set_j(0xabcd); SHOW("sj");

  /* value wider than the field: the store must truncate, neighbours intact */
  reset(); add_j(0x1234); SHOW("oj");
  reset(); add_k(0xff01); SHOW("ok");
  reset(); add_i(0x30000);SHOW("oi");

  reset(); and_j(21);     SHOW("nj");
  reset(); ior_j(19);     SHOW("rj");
  reset(); xor_j(37);     SHOW("xj");
  reset(); div_k(17);     SHOW("dk");
  reset(); rem_i(19);     SHOW("mi");
  reset(); selfmix(3);    SHOW("mx");

  reset(); add_ni(3);  printf("ni=%u %u %u %lld\n", b.i, b.j, b.k, b.l);
  reset(); add_nj(251);printf("nj=%u %u %u %lld\n", b.i, b.j, b.k, b.l);
  reset(); add_nk(13279);printf("nk=%u %u %u %lld\n", b.i, b.j, b.k, b.l);
  reset(); add_cj(1);  printf("cj=%u %u %u %lld\n", c.i, c.j, c.k, c.l);

  vd.i = 1; vd.j = 2; vd.k = 3;
  add_vj(9);
  printf("vj=%u %u %u\n", vd.i, vd.j, vd.k);
  return 0;
}
