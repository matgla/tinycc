/* __atomic_* read-modify-write helpers for ARMv8-M (Thumb only).
 *
 * tcc inlines atomic loads and stores but calls these for read-modify-write.
 * atomic.S is the ARM (A32) version -- raw ARM opcodes and CP15 barriers that a
 * Cortex-M cannot execute -- so the armv8m runtime had none at all, and code
 * using C11 atomics (the YasOS kernel's SMP locks) could only link against a
 * hand-written copy.  LDREX/STREX loops, every memory order treated as seq_cst
 * (a full DMB on each side; M-profile has only the SY barrier).
 *
 *   T  __atomic_fetch_<op>_N(T *p, T v, int order)    old value
 *   T  __atomic_<op>_fetch_N(T *p, T v, int order)    new value
 *   T  __atomic_exchange_N(T *p, T v, int order)      old value
 *   bool __atomic_compare_exchange_N(T *p, T *expected, T desired, bool weak,
 *                                    int success, int failure)
 * N is 1, 2 or 4; narrow results come back zero-extended.
 *
 * TCC_ATOMICS_PLAIN (the YasOS user runtime, lib/Makefile): a plain load, the
 * operation and a plain store, barriers kept.  On the RP2350 an exclusive goes
 * through the global monitor (ACTLR.EXTEXCLALL, see the kernel's
 * source/arch/armv8-m/mpu.zig), which does not cover PSRAM: a STREX there never
 * succeeds, and the loop below never ends.  zig's lock-free ArenaAllocator ran
 * __atomic_exchange_4 on the maker's .data in PSRAM and the maker spun in it for
 * good (2026-11-11).  A YasOS process is one thread with memory no other process
 * maps (a vfork parent is suspended while its child runs), so there is nothing to
 * be atomic against -- except a signal handler touching the same word, which the
 * exclusive loop would have caught and this does not.  The kernel assembles its
 * own copy without the define (build/kernel_tcc.zig): its atomics are SMP locks
 * in SRAM. */

        .syntax unified
        .thumb
        .text
        .align  2

#ifdef TCC_ATOMICS_PLAIN
# define LD1 ldrb
# define ST1 strb
# define LD2 ldrh
# define ST2 strh
# define LD4 ldr
# define ST4 str
#else
# define LD1 ldrexb
# define ST1 strexb
# define LD2 ldrexh
# define ST2 strexh
# define LD4 ldrex
# define ST4 strex
#endif

#ifdef TCC_ATOMICS_PLAIN

.macro FETCH name, op, ld, st
        .global \name
        .type   \name, %function
        .thumb_func
\name:
        dmb
        \ld     r2, [r0]
        \op     r3, r2, r1
        \st     r3, [r0]
        dmb
        mov     r0, r2
        bx      lr
        .size   \name, . - \name
.endm

.macro OPFETCH name, op, ld, st
        .global \name
        .type   \name, %function
        .thumb_func
\name:
        dmb
        \ld     r2, [r0]
        \op     r3, r2, r1
        \st     r3, [r0]
        dmb
        mov     r0, r3
        bx      lr
        .size   \name, . - \name
.endm

.macro FETCH_NAND name, ld, st, ret
        .global \name
        .type   \name, %function
        .thumb_func
\name:
        dmb
        \ld     r2, [r0]
        and     r3, r2, r1
        mvn     r3, r3
        \st     r3, [r0]
        dmb
        mov     r0, \ret
        bx      lr
        .size   \name, . - \name
.endm

.macro EXCHANGE name, ld, st
        .global \name
        .type   \name, %function
        .thumb_func
\name:
        dmb
        \ld     r2, [r0]
        \st     r1, [r0]
        dmb
        mov     r0, r2
        bx      lr
        .size   \name, . - \name
.endm

.macro CMPXCHG name, ld, st, ldx, stx
        .global \name
        .type   \name, %function
        .thumb_func
\name:
        push    {r4, lr}
        \ldx    r4, [r1]
        dmb
        \ld     r3, [r0]
        cmp     r3, r4
        bne     2f
        \st     r2, [r0]
        dmb
        movs    r0, #1
        pop     {r4, pc}
2:      \stx    r3, [r1]
        dmb
        movs    r0, #0
        pop     {r4, pc}
        .size   \name, . - \name
.endm

#else

/* old = *p; *p = old <op> v; return old (FETCH) or the new value (OPFETCH) */
.macro FETCH name, op, ld, st
        .global \name
        .type   \name, %function
        .thumb_func
\name:
        dmb
1:      \ld     r2, [r0]
        \op     r3, r2, r1
        \st     ip, r3, [r0]
        cmp     ip, #0
        bne     1b
        dmb
        mov     r0, r2
        bx      lr
        .size   \name, . - \name
.endm

.macro OPFETCH name, op, ld, st
        .global \name
        .type   \name, %function
        .thumb_func
\name:
        dmb
1:      \ld     r2, [r0]
        \op     r3, r2, r1
        \st     ip, r3, [r0]
        cmp     ip, #0
        bne     1b
        dmb
        mov     r0, r3
        bx      lr
        .size   \name, . - \name
.endm

/* nand: ~(old & v) */
.macro FETCH_NAND name, ld, st, ret
        .global \name
        .type   \name, %function
        .thumb_func
\name:
        dmb
1:      \ld     r2, [r0]
        and     r3, r2, r1
        mvn     r3, r3
        \st     ip, r3, [r0]
        cmp     ip, #0
        bne     1b
        dmb
        mov     r0, \ret
        bx      lr
        .size   \name, . - \name
.endm

.macro EXCHANGE name, ld, st
        .global \name
        .type   \name, %function
        .thumb_func
\name:
        dmb
1:      \ld     r2, [r0]
        \st     ip, r1, [r0]
        cmp     ip, #0
        bne     1b
        dmb
        mov     r0, r2
        bx      lr
        .size   \name, . - \name
.endm

/* if (*p == *expected) { *p = desired; return 1; } *expected = *p; return 0;
   `ldx` reads *expected at the width (zero-extending like ldrex[bh]). */
.macro CMPXCHG name, ld, st, ldx, stx
        .global \name
        .type   \name, %function
        .thumb_func
\name:
        push    {r4, lr}
        \ldx    r4, [r1]
        dmb
1:      \ld     r3, [r0]
        cmp     r3, r4
        bne     2f
        \st     ip, r2, [r0]
        cmp     ip, #0
        bne     1b
        dmb
        movs    r0, #1
        pop     {r4, pc}
2:      clrex
        \stx    r3, [r1]
        dmb
        movs    r0, #0
        pop     {r4, pc}
        .size   \name, . - \name
.endm

#endif

/* 1-byte */
        FETCH   __atomic_fetch_add_1, add, LD1, ST1
        FETCH   __atomic_fetch_sub_1, sub, LD1, ST1
        FETCH   __atomic_fetch_and_1, and, LD1, ST1
        FETCH   __atomic_fetch_or_1, orr, LD1, ST1
        FETCH   __atomic_fetch_xor_1, eor, LD1, ST1
        OPFETCH __atomic_add_fetch_1, add, LD1, ST1
        OPFETCH __atomic_sub_fetch_1, sub, LD1, ST1
        OPFETCH __atomic_and_fetch_1, and, LD1, ST1
        OPFETCH __atomic_or_fetch_1, orr, LD1, ST1
        OPFETCH __atomic_xor_fetch_1, eor, LD1, ST1
        FETCH_NAND __atomic_fetch_nand_1, LD1, ST1, r2
        FETCH_NAND __atomic_nand_fetch_1, LD1, ST1, r3
        EXCHANGE __atomic_exchange_1, LD1, ST1
        CMPXCHG __atomic_compare_exchange_1, LD1, ST1, ldrb, strb

/* 2-byte */
        FETCH   __atomic_fetch_add_2, add, LD2, ST2
        FETCH   __atomic_fetch_sub_2, sub, LD2, ST2
        FETCH   __atomic_fetch_and_2, and, LD2, ST2
        FETCH   __atomic_fetch_or_2, orr, LD2, ST2
        FETCH   __atomic_fetch_xor_2, eor, LD2, ST2
        OPFETCH __atomic_add_fetch_2, add, LD2, ST2
        OPFETCH __atomic_sub_fetch_2, sub, LD2, ST2
        OPFETCH __atomic_and_fetch_2, and, LD2, ST2
        OPFETCH __atomic_or_fetch_2, orr, LD2, ST2
        OPFETCH __atomic_xor_fetch_2, eor, LD2, ST2
        FETCH_NAND __atomic_fetch_nand_2, LD2, ST2, r2
        FETCH_NAND __atomic_nand_fetch_2, LD2, ST2, r3
        EXCHANGE __atomic_exchange_2, LD2, ST2
        CMPXCHG __atomic_compare_exchange_2, LD2, ST2, ldrh, strh

/* 4-byte */
        FETCH   __atomic_fetch_add_4, add, LD4, ST4
        FETCH   __atomic_fetch_sub_4, sub, LD4, ST4
        FETCH   __atomic_fetch_and_4, and, LD4, ST4
        FETCH   __atomic_fetch_or_4, orr, LD4, ST4
        FETCH   __atomic_fetch_xor_4, eor, LD4, ST4
        OPFETCH __atomic_add_fetch_4, add, LD4, ST4
        OPFETCH __atomic_sub_fetch_4, sub, LD4, ST4
        OPFETCH __atomic_and_fetch_4, and, LD4, ST4
        OPFETCH __atomic_or_fetch_4, orr, LD4, ST4
        OPFETCH __atomic_xor_fetch_4, eor, LD4, ST4
        FETCH_NAND __atomic_fetch_nand_4, LD4, ST4, r2
        FETCH_NAND __atomic_nand_fetch_4, LD4, ST4, r3
        EXCHANGE __atomic_exchange_4, LD4, ST4
        CMPXCHG __atomic_compare_exchange_4, LD4, ST4, ldr, str
