/*
 * dcp_init.c - enable the RP2350 coprocessors.
 *
 * The DCP is a coprocessor on CP4, not a memory-mapped peripheral.  (The
 * previous version of this file drove an invented register block at
 * 0x50200000, which does not exist on this part.)  Access is gated by CPACR,
 * and any DCP instruction executed with CP4 disabled raises a UsageFault with
 * NOCP set.
 *
 * This is per-core state: each core must call this for itself.
 *
 * Pico SDK builds already do the equivalent in
 * runtime_init_per_core_enable_coprocessors(), so firmware built against the
 * SDK -- including tests/benchmarks -- needs no call here.  It exists for
 * bare-metal startup code that does not use the SDK.
 */

#include "tcc_stdint.h"

/* Coprocessor Access Control Register, ARMv8-M System Control Block. */
#define CPACR (*(volatile uint32_t *)0xE000ED88u)

#define CPACR_CP0_FULL (3u << 0)   /* GPIO coprocessor */
#define CPACR_CP4_FULL (3u << 8)   /* DCP */
#define CPACR_CP10_FULL (3u << 20) /* FPv5-SP */
#define CPACR_CP11_FULL (3u << 22)

void rp2350_dcp_init(void)
{
  CPACR |= CPACR_CP4_FULL | CPACR_CP10_FULL | CPACR_CP11_FULL;

  /* Ordering: the enable must be visible before any CP4 instruction issues. */
  __asm__ volatile("dsb" ::: "memory");
  __asm__ volatile("isb" ::: "memory");

  /* Clear the "engaged" flag left over from reset or from a previous owner.
   * RCMP is the engaging read, which is what clears it; the result is
   * deliberately discarded.  Mirrors the Pico SDK's runtime init. */
  __asm__ volatile("mrc p4, #0, r0, c0, c0, #1" ::: "r0");
}
