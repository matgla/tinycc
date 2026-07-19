/* Guard: copy-source load forwarding (source/opt/flat/memory/copy_source_load_fwd.c).
 *
 * After `x = G`, a read of `x.f` may be redirected to read `G.f` directly, which
 * is what collapses the `if (x.f != G.f) abort();` self-checks in the
 * 20040709-2 drivers.  The pass now forwards a lot more than it used to, and
 * every extension below is a place where a too-eager rewrite silently returns
 * the WRONG value rather than merely slower code:
 *
 *  - inline memory operands, not just LOADs: `CMP StackLoc[s], G` and
 *    `T <- StackLoc[s] SHL #k` name the local's bytes directly;
 *  - store-forwarded copies, `G.f = v; x = G;`, where no LOAD exists at all;
 *  - per-field disjointness: a write to a DIFFERENT field of the same global
 *    must not block the forward, but a write to the SAME field must;
 *  - demanded bits: a bitfield update is a read-modify-write that leaves the
 *    other fields of the word alone, so a read of those fields still forwards —
 *    but only the bits it does not touch.
 *
 * Each check below copies the global, mutates part of it, and compares the copy
 * against the live global.  The expected answers are computed from the C
 * semantics, so a forward that reads post-mutation state shows up as a wrong
 * number, not as a missing optimization.
 */
#include <stdio.h>

#define pck __attribute__((packed))

/* 64-bit bitfield unit: j straddles the 32-bit boundary (bits 12..34) while k
 * (bits 35..63) lives entirely in the high word — the shape where a write to k
 * overlaps j's BYTES but none of its BITS. */
struct pck W64 { unsigned long long l, i : 12, j : 23, k : 29; };
struct W64 g64;

/* Register-sized struct: `g32 = ...; x = g32;` is store-forwarded, no LOAD. */
struct pck W32 { unsigned int k : 6, l : 1, j : 10, i : 15; };
struct W32 g32;

/* Separate whole-word member next to a bitfield word. */
struct pck Wsep { unsigned int k : 6, j : 11, i : 15; unsigned int l; };
struct Wsep gsep;

__attribute__((noinline)) unsigned bump64(unsigned a) { g64.k += a; return g64.k; }
__attribute__((noinline)) unsigned bump32(unsigned a) { g32.k += a; return g32.k; }
__attribute__((noinline)) unsigned bumpsep(unsigned a) { gsep.k += a; return gsep.k; }

/* Untouched fields must still compare equal after the callee writes only k. */
__attribute__((noinline)) int t64_other_fields_survive(unsigned a)
{
  struct W64 x = g64;
  unsigned r = bump64(a);
  int eq = (x.i == g64.i) + (x.j == g64.j) + (x.l == g64.l);
  return eq * 10 + (r == g64.k);
}

/* k itself changed, so the compare must NOT fold to "equal". */
__attribute__((noinline)) int t64_written_field_differs(unsigned a)
{
  struct W64 x = g64;
  bump64(a);
  return x.k != g64.k;
}

__attribute__((noinline)) int t32_other_fields_survive(unsigned a)
{
  struct W32 x = g32;
  unsigned r = bump32(a);
  int eq = (x.i == g32.i) + (x.j == g32.j) + (x.l == g32.l);
  return eq * 10 + (r == g32.k);
}

__attribute__((noinline)) int t32_written_field_differs(unsigned a)
{
  struct W32 x = g32;
  bump32(a);
  return x.k != g32.k;
}

__attribute__((noinline)) int tsep_other_fields_survive(unsigned a)
{
  struct Wsep x = gsep;
  unsigned r = bumpsep(a);
  int eq = (x.i == gsep.i) + (x.j == gsep.j) + (x.l == gsep.l);
  return eq * 10 + (r == gsep.k);
}

/* Store-forwarded copy: the value written to the global is reused as the copy
 * source without a reload, and the whole word (not just k) is then compared. */
__attribute__((noinline)) int t32_store_forwarded(unsigned v)
{
  g32.k = v;
  struct W32 x = g32;
  return (x.k == g32.k) + (x.j == g32.j) + (x.i == g32.i) + (x.l == g32.l);
}

/* Writing the copy itself must stop the forward: x.j is no longer G.j. */
__attribute__((noinline)) int t64_local_write_wins(unsigned v)
{
  struct W64 x = g64;
  x.j = v;
  return (x.j == g64.j);
}

int main(void)
{
  g64.l = 0x0123456789abcdefULL;
  g64.i = 0xabc;
  g64.j = 0x123456;
  g64.k = 0x1234567;
  g32.k = 0x2a; g32.l = 1; g32.j = 0x123; g32.i = 0x4321;
  gsep.k = 0x15; gsep.j = 0x456; gsep.i = 0x1234; gsep.l = 0xdeadbeef;

  /* Every call that mutates a global is sequenced into its own statement before
   * anything reads that global again: argument evaluation order is unspecified,
   * so a call and a read of what it wrote must never share one printf. */
  unsigned before = g64.k;
  int r = t64_other_fields_survive(7);
  int ok = (unsigned)((before + 7) & 0x1fffffff) == g64.k;
  printf("t64=%d %d\n", r, ok);
  r = t64_written_field_differs(9);
  printf("t64d=%d\n", r);

  before = g32.k;
  r = t32_other_fields_survive(3);
  ok = (unsigned)((before + 3) & 0x3f) == g32.k;
  printf("t32=%d %d\n", r, ok);
  r = t32_written_field_differs(5);
  printf("t32d=%d\n", r);

  before = gsep.k;
  r = tsep_other_fields_survive(4);
  ok = (unsigned)((before + 4) & 0x3f) == gsep.k;
  printf("tsep=%d %d\n", r, ok);
  printf("sepl=%x\n", gsep.l);

  r = t32_store_forwarded(0x11);
  printf("fwd=%d %x\n", r, g32.k);

  unsigned newj = g64.j ^ 1u;
  r = t64_local_write_wins(newj);
  printf("lw=%d\n", r);
  return 0;
}
