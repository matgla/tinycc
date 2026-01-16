.syntax unified
.thumb

/*
 * Provide the standard EABI entrypoint name without reimplementing the logic.
 * In soft-float ABI, the float argument is passed in r0 as its raw 32-bit bits.
 * We tail-branch to the bits-based implementation.
 */

.global __aeabi_f2d
.type __aeabi_f2d, %function
__aeabi_f2d:
  b __aeabi_f2d_bits
