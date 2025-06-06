/* Regression guard for the two-shift-extract -> UBFX fusion
 * (tcc_ir_opt_shift_pair_to_ubfx, ir/opt_fusion.c).
 *
 * The canonical unsigned bitfield extract `(x << a) >> b` (b >= a, logical)
 * isolates the (32-b)-bit field at bit offset (b-a).  When the extracted value
 * feeds a consumer that can NOT absorb a shifted operand (a store, a multiply,
 * a value used twice), the pass rewrites the SHL+SHR pair to a single
 * `UBFX rd, rx, #(b-a), #(32-b)`.  When the consumer IS a shift-foldable ALU op
 * (add/sub/and/or/xor/cmp), the earlier barrel-shift fusion already folded the
 * shift, so the pass must leave it alone — either way the RESULT must be
 * identical to the plain shift arithmetic.  These checks pin the value for
 * fields at several offsets/widths and for each consumer shape.
 */
#include <stdio.h>

#define pck __attribute__((packed))

/* k at offset 5, width 11; m at offset 0, width 12; n at offset 13, width 19 */
struct pck B { unsigned int i : 5, k : 11, pad : 16; };
struct pck C { unsigned int m : 12, x : 20; };
struct pck D { unsigned int lo : 13, n : 19; };

struct B sB;
struct C sC;
struct D sD;

unsigned char obuf[8];

/* extract feeding a STORE (non-foldable) */
unsigned int store_k(void)
{
  obuf[0] = (unsigned char)sB.k;
  return obuf[0];
}

/* extract feeding a MULTIPLY (non-foldable) */
unsigned int mul_k(unsigned int x) { return sB.k * x; }

/* extract feeding an ADD (foldable -> stays a barrel-shifted add; result same) */
unsigned int add_k(unsigned int x) { return sB.k + x; }

/* extract used TWICE (non-foldable -> single UBFX, value reused) */
unsigned int twice_m(void) { unsigned int v = sC.m; return v + v; }

/* wide field near the top of the word */
unsigned int get_n(void) { return sD.n; }

int main(void)
{
  int ok = 1;

  sB.i = 0x1f; sB.k = 0x37a; sB.pad = 0xbeef;
  sC.m = 0xabc; sC.x = 0x55aa5;
  sD.lo = 0x1abc; sD.n = 0x5a5a5;

  if (store_k() != (0x37au & 0xff)) ok = 0;
  if (mul_k(3u) != ((0x37au) * 3u)) ok = 0;
  if (mul_k(0u) != 0u) ok = 0;
  if (add_k(7u) != (0x37au + 7u)) ok = 0;
  if (add_k(0xffffffffu) != (0x37au + 0xffffffffu)) ok = 0;
  if (twice_m() != (0xabcu + 0xabcu)) ok = 0;
  if (get_n() != 0x5a5a5u) ok = 0;

  printf("%s\n", ok ? "OK" : "FAIL");
  return 0;
}
