/* SysTick cycle-count shim; see docs/qemu_cycle_profiling.md */

#include <stdio.h>

/* Not the usual *(volatile T *)0xADDR macro: tcc miscompiles a deref of an
   integer-constant address into the address itself (docs/bugs.md). */
static volatile unsigned *const SYST_CSR = (volatile unsigned *)0xE000E010;
static volatile unsigned *const SYST_RVR = (volatile unsigned *)0xE000E014;
static volatile unsigned *const SYST_CVR = (volatile unsigned *)0xE000E018;

#define SYST_MAX 0x00FFFFFFu
#define SYST_COUNTFLAG (1u << 16)

static unsigned long long cyc_base;
static unsigned long long cyc_wraps;

/* SysTick counts DOWN over 24 bits; COUNTFLAG (cleared by the CSR read) marks
   each reload, so sampling must outpace the wrap rate to stay exact. */
static unsigned long long cyc_now(void)
{
    unsigned csr = *SYST_CSR;
    unsigned cvr = *SYST_CVR;
    if (csr & SYST_COUNTFLAG)
        cyc_wraps++;
    return cyc_wraps * (unsigned long long)(SYST_MAX + 1)
           + (SYST_MAX - (cvr & SYST_MAX));
}

__attribute__((constructor)) static void cyc_start(void)
{
    *SYST_RVR = SYST_MAX;
    *SYST_CVR = 0;
    *SYST_CSR = 5;              /* enable | processor clock, no interrupt */
    /* Writing CVR reloads on the next tick, not now: sampling the intervening
       CVR==0 would read as a full SYST_MAX and underflow the delta.  Coarse
       shifts made that likely, fine ones merely hid it. */
    while (*SYST_CVR == 0)
        ;
    (void)*SYST_CSR;            /* swallow the reload's COUNTFLAG */
    cyc_wraps = 0;
    cyc_base = cyc_now();
}

__attribute__((destructor)) static void cyc_end(void)
{
    printf("\n##CYCLES %llu\n", cyc_now() - cyc_base);
}
