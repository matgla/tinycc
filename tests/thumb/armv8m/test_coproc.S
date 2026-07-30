.syntax unified
.thumb

.global _start
_start:

.global test_coproc
test_coproc:
    /* CDP / CDP2 — the RP2350 DCP compute ops live here */
    cdp   p4, #0, c0, c0, c0, #0        /* DCP INIT */
    cdp   p4, #0, c0, c0, c1, #0        /* DCP ADD0 */
    cdp   p4, #1, c0, c0, c1, #0        /* DCP ADD1 */
    cdp   p4, #1, c0, c0, c1, #1        /* DCP SUB1 */
    cdp   p4, #2, c0, c0, c1, #0        /* DCP SQR0 */
    cdp   p4, #8, c0, c0, c0, #1        /* DCP NRDD */
    cdp   p4, #8, c0, c0, c2, #1        /* DCP NRDF */
    cdp   p4, #8, c0, c0, c0, #2        /* DCP NTDC */
    cdp   p4, #8, c0, c0, c0, #3        /* DCP NRDC */
    cdp   p15, #15, c15, c15, c15, #7   /* every field saturated */
    cdp2  p4, #0, c0, c0, c0, #0

    /* MCRR / MCRR2 — 64-bit operand writes */
    mcrr  p4, #0, r0, r1, c0            /* DCP WXMD */
    mcrr  p4, #0, r2, r3, c1            /* DCP WYMD */
    mcrr  p4, #0, r4, r5, c2            /* DCP WEFD */
    mcrr  p4, #1, r0, r1, c0            /* DCP WXUP */
    mcrr  p4, #1, r2, r3, c1            /* DCP WYUP */
    mcrr  p4, #1, r6, r7, c2            /* DCP WXYU */
    mcrr  p4, #6, r0, r0, c0            /* DCP WXUC */
    mcrr  p4, #7, r0, r0, c0            /* DCP WXIC */
    mcrr  p4, #8, r0, r1, c0            /* DCP WXDC */
    mcrr  p4, #9, r6, r7, c2            /* DCP WXFC */
    mcrr  p4, #0, r12, r14, c0          /* high registers */
    mcrr2 p4, #0, r0, r1, c0

    /* MRRC / MRRC2 — 64-bit result reads */
    mrrc  p4, #1, r0, r1, c0            /* DCP RDDA */
    mrrc  p4, #3, r0, r1, c0            /* DCP RDDS */
    mrrc  p4, #5, r0, r1, c0            /* DCP RDDM */
    mrrc  p4, #11, r0, r1, c0           /* DCP RDDG */
    mrrc  p4, #0, r2, r3, c8            /* DCP RXMD */
    mrrc  p4, #0, r4, r5, c9            /* DCP RYMD */
    mrrc2 p4, #0, r0, r1, c8            /* DCP PXMD (non-engaging peek) */

    /* MRC / MRC2 — 32-bit result reads */
    mrc   p4, #0, r0, c0, c0, #0        /* DCP RXVD */
    mrc   p4, #0, r0, c0, c0, #1        /* DCP RCMP */
    mrc   p4, #0, r3, c0, c2, #0        /* DCP RDFA */
    mrc   p4, #0, r3, c0, c2, #1        /* DCP RDFS */
    mrc   p4, #0, r0, c0, c2, #5        /* DCP RDFG */
    mrc   p4, #0, r7, c0, c3, #0        /* DCP RDIC */
    mrc   p4, #0, r7, c0, c3, #1        /* DCP RDUC */
    mrc   p4, #0, apsr_nzcv, c0, c0, #1 /* DCP RCMP straight into the flags */
    mrc2  p4, #0, r15, c0, c0, #1       /* DCP PCMP (non-engaging peek) */

    /* MCR / MCR2 */
    mcr   p15, #0, r0, c7, c10, #5      /* the encoding lib/stdatomic.c open-codes */
    mcr   p4, #7, r9, c5, c11, #3       /* opc1 is 3 bits on MCR/MRC */
    mcr2  p4, #0, r0, c0, c0, #0

    /* GNU as also spells coprocessor registers crN */
    mcrr  p4, #1, r0, r1, cr0
    mrc   p4, #0, r0, cr0, cr0, #1

    bx lr
