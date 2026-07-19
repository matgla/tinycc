/* Guard: const_prop_tmp must not record a constant it had to truncate to reach
 * the destination's declared width.
 *
 * The pass tracks "this temp currently holds constant K" and substitutes K into
 * later reads.  When it records K it fits the value to the DEST operand's btype
 * -- correct while that width describes the access, since a char slot really
 * does hold 8 bits.
 *
 * A `?:` breaks that assumption.  Both arms assign the ONE merge temp, so a
 * `wide_constant : narrow_variable` conditional produces a temp that can carry
 * the narrow arm's btype while holding the wide arm's value:
 *
 *     T6 <-- s3            [LOAD]      the char arm  -> T6 typed INT8
 *     T6 <-- #1767579187   [ASSIGN]    the wide arm  -> recorded as 51
 *     T12 <-- T6 SUB T22               folded to  #51 SUB T22
 *
 * 1767579187 & 0xFF == 51, and the subtraction then ran on the wrong number.
 * The fix skips recording when the immediate does not survive the dest's width,
 * so nothing is propagated instead of the wrong value being propagated.
 *
 * Both the char (8-bit) and short (16-bit) forms are covered -- the short one
 * truncated to a different wrong value through the identical path.
 *
 * Reduced from agg_deep fuzz seed 2343 (diverged at -O1 and -O2).  Expected
 * output is the -O0/gcc checksum.
 */
#include <stdio.h>

static unsigned cs_extra;
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + 1u + 1u;
  return h * 2654435761u;
}
static unsigned narrow16(void)
{
  unsigned cs = 0x12345678u;
  short s4 = (short)1u;
  unsigned u4 = 2680500729u;
  unsigned u6 = 804901209u;
  unsigned u7 = 4212568317u;
  unsigned arr8[8] = { 4101259284u, 3865130185u, 3056342746u, 2497024765u, 3598391132u, 2502926222u, 1145493425u, 1588927684u };
  unsigned *pa212 = &u7;
  unsigned **ppa213 = &pa212;
  if ((unsigned)1u & 1u) {
    for (unsigned g15 = 0u; g15 < 5u; g15++) {
    }
    if ((unsigned)(u4) & 1u) {
    }
  }
  u6 = (unsigned)(((unsigned)(((unsigned)((((unsigned)(u4) & 1u) ? (unsigned)(1767579187u) : (unsigned)((unsigned)(s4)))) - (unsigned)(((unsigned)(((unsigned)(arr8[((unsigned)(u7) & 7u)]) / 1u)) >> 1u)))) + (unsigned)1u)) & 0xffffffffu;
  u4 = (unsigned)(u7) & 0xffffffffu;
  cs = csmix(cs, u6);
  return cs;
}

int main(void)
{
  cs_extra = narrow16();
  unsigned cs = 0x12345678u;
  char s3 = (char)1u;
  unsigned u4 = 2680500729u;
  unsigned u6 = 804901209u;
  unsigned u7 = 4212568317u;
  unsigned arr8[8] = { 4101259284u, 3865130185u, 3056342746u, 2497024765u, 3598391132u, 2502926222u, 1145493425u, 1588927684u };
  unsigned *pa212 = &u7;
  unsigned **ppa213 = &pa212;
  if ((unsigned)1u & 1u) {
    for (unsigned g15 = 0u; g15 < 5u; g15++) {
    }
    if ((unsigned)(u4) & 1u) {
    }
  }
  u6 = (unsigned)(((unsigned)(((unsigned)((((unsigned)(u4) & 1u) ? (unsigned)(1767579187u) : (unsigned)((unsigned)(s3)))) - (unsigned)(((unsigned)(((unsigned)(arr8[((unsigned)(u7) & 7u)]) / 1u)) >> 1u)))) + (unsigned)1u)) & 0xffffffffu;
  u4 = (unsigned)(u7) & 0xffffffffu;
  cs = csmix(cs, u6);
  printf("checksum=%08x\n", csmix(cs, cs_extra));
  return 0;
}

