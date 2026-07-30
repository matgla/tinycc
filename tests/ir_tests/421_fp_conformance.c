/*
 * 421_fp_conformance.c - IEEE-754 conformance for the FP runtime.
 *
 * Walks the host-generated bit-exact vector tables in tests/fp/ and checks
 * every add/sub/mul/div/compare/conversion result against the reference.
 *
 * Under QEMU (mps2-an505) this exercises whatever `-mfpu=` selected, which
 * today is libsoftfp. The same sources also build for real RP2350 hardware,
 * where they are the acceptance test for the DCP path -- QEMU has no RP2350
 * machine and cannot model the double coprocessor at all, so the hardware run
 * is the only place DCP codegen can be validated.
 *
 * Exit code is the number of failing vectors, capped at 120 so a total
 * breakage still produces a usable exit status rather than wrapping to 0.
 */

#include <stdio.h>

#include "../fp/fp_conformance.h"

/* Pulled in as a single translation unit: the QEMU harness compiles one .c
 * (plus an optional `-lib.c` sibling), and keeping the shared runner in
 * tests/fp/ means the identical source also builds for RP2350 hardware. */
#include "../fp/fp_conformance.c"

int main(void)
{
  int failures = fp_conformance_run();

  if (failures == 0)
    printf("FP conformance: PASS\n");
  else
    printf("FP conformance: FAIL (%d)\n", failures);

  return failures > 120 ? 120 : failures;
}
