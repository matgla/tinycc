/* A function big enough that its bytes fill the linker's piece buffer before
 * .text.dup (13_spill_gap_a.c, 13_spill_gap_b.c) is read into it. */
extern unsigned ext_mix(unsigned);

unsigned spill_big(unsigned x)
{
#define STEP(k) x = ext_mix(x ^ (0x9E3779B9u * (k)));
#define STEP8(k) STEP(k) STEP(k + 1) STEP(k + 2) STEP(k + 3) STEP(k + 4) STEP(k + 5) STEP(k + 6) STEP(k + 7)
  STEP8(1) STEP8(9) STEP8(17) STEP8(25) STEP8(33) STEP8(41) STEP8(49) STEP8(57)
  return x;
}
