/* combo_num fuzz seed 84127 (O1; also ptr seed 80958 at O2): the linear-scan
 * register allocator double-booked a register between a loop counter and the
 * high half of a 64-bit value.
 *
 * `q7` is a loop-carried 64-bit value OR'd with a constant each iteration; the
 * outer counter `g16` is a loop-carried single-register value.  Loop-phi
 * coalescing gave `g16` and its increment a SHARED callee-saved register (say
 * R5) and marked the holder `loop_phi_locked` so the single-register spill path
 * would never evict it.  But the *64-bit pair* allocation has its own
 * call-crossing fallback (ra_linear_scan, ir/regalloc.c) that evicts single-INT
 * victims to free an aligned register pair for `q7 |= const` — and that victim
 * scan was missing the `loop_phi_locked` guard.  It spilled the locked counter
 * and freed R5, but the counter's coalesce partner still occupied R5, so the
 * 64-bit OR's high half was allocated R5 too.  The high-word `orr` then
 * clobbered the outer loop counter with ~0xffbfefbb, so the outer loop ran once
 * instead of 11 times and the checksum was wrong (O0/O2/Os correct).
 *
 * Fix: skip `loop_phi_locked` victims in the LLONG pair-fallback eviction, the
 * same guard the single-register spill victim scan already applies.
 */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}

int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u2 = 429893537u;
  unsigned u3 = 3280183169u;
  unsigned long long q7 = (((unsigned long long)(u2)) << 32) | (unsigned long long)(u3);

  for (unsigned g16 = 0u; g16 < 11u; g16++) {
    unsigned g18 = 0u;
    while (g18 < 4u) {
      q7 = (q7) | (17850273013748512779ull);
      cs = csmix(cs, (unsigned)(q7) ^ (unsigned)(q7 >> 32));
      g18++;
    }
  }

  printf("checksum=%08x\n", cs);
  return 0;
}
