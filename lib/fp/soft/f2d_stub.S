.syntax unified
.thumb

/*
 * Provide the standard EABI entrypoint name without reimplementing the logic.
 * In soft-float ABI, the float argument is passed in r0 as its raw 32-bit bits.
 * We tail-branch to the bits-based implementation.
 * Use b.w (32-bit branch) for larger range than b.n (16-bit).
 */

.global __aeabi_f2d
.type __aeabi_f2d, %function
__aeabi_f2d:
  b.w __aeabi_f2d_bits
