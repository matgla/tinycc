/*
 * dcp_aeabi.S - RP2350 AEABI floating point via the DCP (doubles) and the
 * FPv5-SP FPU (floats).
 *
 * Written in assembly rather than C with inline asm for two reasons:
 *
 *   1. The canned DCP multiply needs seven scratch registers on top of four
 *      argument registers and two results; tcc's inline-asm allocator cannot
 *      satisfy that many "r" constraints, and the Pico SDK reuses r0-r3 as
 *      both operands and scratch to fit.
 *   2. The AEABI already pins the arguments to r0-r3, so fixed registers cost
 *      nothing and avoid a round trip through the allocator.
 *
 * Sequences are transcribed from the vendored Pico SDK:
 *   tests/benchmarks/libs/pico-sdk/src/rp2_common/hardware_dcp/include/hardware/
 *     dcp_instr.inc.S, dcp_canned.inc.S
 * with the DCP mnemonics expanded inline (tcc's assembler has no .macro), and
 * each expansion commented with the SDK name so the two stay diffable.
 *
 * ABI: double a in r0:r1, double b in r2:r3, result in r0:r1.
 *      float a in r0, b in r1, result in r0.
 *
 * REENTRANCY: these entry points are the "preemptor saves" side of the
 * contract described in dcp.h -- an interrupt handler that touches doubles
 * must come through here.  The save/restore wrapper is in dcp_reent.c; the
 * sequences below are the raw operations.
 */

.syntax unified
.thumb

.text
.align 2

/* ═══════════════════════════════════════════════════════════════════
 *  Double precision -- DCP
 * ═══════════════════════════════════════════════════════════════════ */

/* dcp_dadd_m r0,r1, r0,r1, r2,r3 */
.global __rp2350_dadd
.type __rp2350_dadd, %function
.global __aeabi_dadd
.type __aeabi_dadd, %function
__rp2350_dadd:
__aeabi_dadd:
    mcrr p4, #1, r0, r1, c0         /* WXUP r0,r1 */
    mcrr p4, #1, r2, r3, c1         /* WYUP r2,r3 */
    cdp  p4, #0, c0, c0, c1, #0     /* ADD0 */
    cdp  p4, #1, c0, c0, c1, #0     /* ADD1 */
    cdp  p4, #8, c0, c0, c0, #1     /* NRDD */
    mrrc p4, #1, r0, r1, c0         /* RDDA r0,r1 */
    bx lr
.size __rp2350_dadd, .-__rp2350_dadd

/* dcp_dsub_m r0,r1, r0,r1, r2,r3 */
.global __rp2350_dsub
.type __rp2350_dsub, %function
.global __aeabi_dsub
.type __aeabi_dsub, %function
__rp2350_dsub:
__aeabi_dsub:
    mcrr p4, #1, r0, r1, c0         /* WXUP r0,r1 */
    mcrr p4, #1, r2, r3, c1         /* WYUP r2,r3 */
    cdp  p4, #0, c0, c0, c1, #0     /* ADD0 */
    cdp  p4, #1, c0, c0, c1, #1     /* SUB1 */
    cdp  p4, #8, c0, c0, c0, #1     /* NRDD */
    mrrc p4, #3, r0, r1, c0         /* RDDS r0,r1 */
    bx lr
.size __rp2350_dsub, .-__rp2350_dsub

/* dcp_dsub_m r0,r1, r2,r3, r0,r1  -- reverse subtract, b - a */
.global __rp2350_drsub
.type __rp2350_drsub, %function
.global __aeabi_drsub
.type __aeabi_drsub, %function
__rp2350_drsub:
__aeabi_drsub:
    mcrr p4, #1, r2, r3, c0         /* WXUP r2,r3 */
    mcrr p4, #1, r0, r1, c1         /* WYUP r0,r1 */
    cdp  p4, #0, c0, c0, c1, #0     /* ADD0 */
    cdp  p4, #1, c0, c0, c1, #1     /* SUB1 */
    cdp  p4, #8, c0, c0, c0, #1     /* NRDD */
    mrrc p4, #3, r0, r1, c0         /* RDDS r0,r1 */
    bx lr
.size __rp2350_drsub, .-__rp2350_drsub

/* dcp_dmul_m r0,r1, r0,r1, r2,r3, r0,r1,r2,r3,r4,r12,r14
 *
 * The DCP hands back the split significands (RXMS/RYMS) and does the exponent
 * work; the 53x53 partial products run on the core and are fed back in with
 * WXMS/WXMO.  r4 is callee-saved so it is stacked.
 */
.global __rp2350_dmul
.type __rp2350_dmul, %function
.global __aeabi_dmul
.type __aeabi_dmul, %function
__rp2350_dmul:
__aeabi_dmul:
    push {r4, lr}
    mcrr p4, #1, r0, r1, c0         /* WXUP r0,r1 */
    mcrr p4, #1, r2, r3, c1         /* WYUP r2,r3 */
    mrrc p4, #0, r0, r1, c4         /* RXMS r0,r1,0 */
    mrrc p4, #0, r2, r3, c5         /* RYMS r2,r3,0 */
    umull r4, r12, r0, r2           /* re,rf = ra*rc */
    movs r14, #0                    /* rg = 0 */
    umlal r12, r14, r0, r3          /* rf,rg += ra*rd */
    umlal r12, r14, r1, r2          /* rf,rg += rb*rc */
    mcrr p4, #2, r4, r12, c0        /* WXMS re,rf */
    movs r4, #0                     /* re = 0 */
    umlal r14, r4, r1, r3           /* rg,re += rb*rd */
    mcrr p4, #3, r14, r4, c0        /* WXMO rg,re */
    cdp  p4, #8, c0, c0, c0, #1     /* NRDD */
    mrrc p4, #5, r0, r1, c0         /* RDDM r0,r1 */
    pop {r4, pc}
.size __rp2350_dmul, .-__rp2350_dmul

/* dcp_dcmp_m r0, r0,r1, r2,r3  -- raw compare register into r0.
 * Bit 31 is "engaged"; the ordering bits are decoded by the C wrappers. */
.global __rp2350_dcmp_raw
.type __rp2350_dcmp_raw, %function
__rp2350_dcmp_raw:
    mcrr p4, #1, r0, r1, c0         /* WXUP r0,r1 */
    mcrr p4, #1, r2, r3, c1         /* WYUP r2,r3 */
    cdp  p4, #0, c0, c0, c1, #0     /* ADD0 */
    mrc  p4, #0, r0, c0, c0, #1     /* RCMP r0 */
    bx lr
.size __rp2350_dcmp_raw, .-__rp2350_dcmp_raw

/* ═══════════════════════════════════════════════════════════════════
 *  Conversions -- DCP
 * ═══════════════════════════════════════════════════════════════════ */

/* dcp_int2double_m r0,r1, r0 */
.global __rp2350_i2d
.type __rp2350_i2d, %function
.global __aeabi_i2d
.type __aeabi_i2d, %function
__rp2350_i2d:
__aeabi_i2d:
    mcrr p4, #7, r0, r0, c0         /* WXIC r0,r0 */
    cdp  p4, #0, c0, c0, c1, #0     /* ADD0 */
    cdp  p4, #1, c0, c0, c1, #1     /* SUB1 */
    cdp  p4, #8, c0, c0, c0, #1     /* NRDD */
    mrrc p4, #3, r0, r1, c0         /* RDDS r0,r1 */
    bx lr
.size __rp2350_i2d, .-__rp2350_i2d

/* dcp_uint2double_m r0,r1, r0 */
.global __rp2350_ui2d
.type __rp2350_ui2d, %function
.global __aeabi_ui2d
.type __aeabi_ui2d, %function
__rp2350_ui2d:
__aeabi_ui2d:
    mcrr p4, #6, r0, r0, c0         /* WXUC r0,r0 */
    cdp  p4, #0, c0, c0, c1, #0     /* ADD0 */
    cdp  p4, #1, c0, c0, c1, #1     /* SUB1 */
    cdp  p4, #8, c0, c0, c0, #1     /* NRDD */
    mrrc p4, #3, r0, r1, c0         /* RDDS r0,r1 */
    bx lr
.size __rp2350_ui2d, .-__rp2350_ui2d

/* dcp_double2int_m r0, r0,r1  -- truncating (NTDC), which is what d2iz wants */
.global __rp2350_d2iz
.type __rp2350_d2iz, %function
.global __aeabi_d2iz
.type __aeabi_d2iz, %function
__rp2350_d2iz:
__aeabi_d2iz:
    mcrr p4, #8, r0, r1, c0         /* WXDC r0,r1 */
    cdp  p4, #0, c0, c0, c1, #0     /* ADD0 */
    cdp  p4, #1, c0, c0, c1, #0     /* ADD1 */
    cdp  p4, #8, c0, c0, c0, #2     /* NTDC */
    mrc  p4, #0, r0, c0, c3, #0     /* RDIC r0 */
    bx lr
.size __rp2350_d2iz, .-__rp2350_d2iz

/* dcp_double2uint_m r0, r0,r1 */
.global __rp2350_d2uiz
.type __rp2350_d2uiz, %function
.global __aeabi_d2uiz
.type __aeabi_d2uiz, %function
__rp2350_d2uiz:
__aeabi_d2uiz:
    mcrr p4, #8, r0, r1, c0         /* WXDC r0,r1 */
    cdp  p4, #0, c0, c0, c1, #0     /* ADD0 */
    cdp  p4, #1, c0, c0, c1, #0     /* ADD1 */
    cdp  p4, #8, c0, c0, c0, #2     /* NTDC */
    mrc  p4, #0, r0, c0, c3, #1     /* RDUC r0 */
    bx lr
.size __rp2350_d2uiz, .-__rp2350_d2uiz

/* dcp_float2double_m r0,r1, r0 */
.global __rp2350_f2d
.type __rp2350_f2d, %function
__rp2350_f2d:
    mcrr p4, #1, r0, r0, c2         /* WXYU r0,r0 */
    cdp  p4, #8, c0, c0, c0, #1     /* NRDD */
    mrrc p4, #11, r0, r1, c0        /* RDDG r0,r1 */
    bx lr
.size __rp2350_f2d, .-__rp2350_f2d

/* dcp_double2float_m r0, r0,r1
 *
 * NRDF flushes to zero any result that does not land in the *normal* float
 * range, so every double whose float image is subnormal came back 0 -- and
 * since tccpp.c narrows hex float literals with `tokc.f = (float)d`, the
 * self-hosted compiler silently zeroed them at parse time as well (see
 * d2f_subnormal.c).  Screen the biased exponent first and hand that one band
 * to software; everything else keeps the three-instruction DCP path.
 *
 *   exp == 0        double zero or subnormal -- float image is +-0, NRDF right
 *   1 <= exp <= 896 float image is subnormal or 0  -- NRDF wrong, go software
 *   exp >= 897      normal float, overflow to Inf, or Inf/NaN -- NRDF right
 *
 * 897 is the smallest biased exponent whose value is >= 2^-126, the smallest
 * normal float.
 *
 * Written with shifts and a movw rather than `ubfx`/`cbz`/`cmp rN,#897`: this
 * file is assembled by tcc's own assembler, which has no ubfx (see dcmpun
 * above) or cbz, and 897 is not a Thumb modified immediate.
 */
.global __rp2350_d2f
.type __rp2350_d2f, %function
.global __aeabi_d2f
.type __aeabi_d2f, %function
__rp2350_d2f:
__aeabi_d2f:
    lsls r2, r1, #1                 /* drop the sign bit */
    lsrs r2, r2, #21                /* r2 = biased exponent of the double */
    cmp  r2, #0
    beq  1f                         /* zero/subnormal double -> +-0 */
    movw r3, #897
    cmp  r2, r3
    bhs  1f                         /* normal, overflow, Inf or NaN */
    b    __rp2350_d2f_subnormal     /* tail call; argument stays in r0,r1 */
1:
    mcrr p4, #1, r0, r1, c0         /* WXUP r0,r1 */
    cdp  p4, #8, c0, c0, c2, #1     /* NRDF */
    mrc  p4, #0, r0, c0, c2, #5     /* RDFG r0 */
    bx lr
.size __rp2350_d2f, .-__rp2350_d2f

/* ═══════════════════════════════════════════════════════════════════
 *  DCP state save / restore
 *
 *  Used by the reentrancy wrapper.  Layout matches `dcp_state` in dcp.h:
 *  { xl, xh, yl, yh, el, eh, engaged }.
 * ═══════════════════════════════════════════════════════════════════ */

/* void __rp2350_dcp_save(dcp_state *s) -- r0 = s */
.global __rp2350_dcp_save
.type __rp2350_dcp_save, %function
__rp2350_dcp_save:
    push {r4, lr}
    mrrc2 p4, #0, r2, r3, c8        /* PXMD (peek: does not engage) */
    strd r2, r3, [r0, #0]
    mrrc2 p4, #0, r2, r3, c9        /* PYMD */
    strd r2, r3, [r0, #8]
    mrrc  p4, #0, r2, r3, c10       /* REFD (engaging read; also clears) */
    strd r2, r3, [r0, #16]
    pop {r4, pc}
.size __rp2350_dcp_save, .-__rp2350_dcp_save

/* void __rp2350_dcp_restore(const dcp_state *s) -- r0 = s */
.global __rp2350_dcp_restore
.type __rp2350_dcp_restore, %function
__rp2350_dcp_restore:
    ldrd r2, r3, [r0, #0]
    mcrr p4, #0, r2, r3, c0         /* WXMD */
    ldrd r2, r3, [r0, #8]
    mcrr p4, #0, r2, r3, c1         /* WYMD */
    ldrd r2, r3, [r0, #16]
    mcrr p4, #0, r2, r3, c2         /* WEFD */
    bx lr
.size __rp2350_dcp_restore, .-__rp2350_dcp_restore

/* int __rp2350_dcp_engaged(void) -- non-zero if a computation is in flight */
.global __rp2350_dcp_engaged
.type __rp2350_dcp_engaged, %function
__rp2350_dcp_engaged:
    mrc2 p4, #0, r0, c0, c0, #1     /* PCMP r0 (peek) */
    lsrs r0, r0, #31
    bx lr
.size __rp2350_dcp_engaged, .-__rp2350_dcp_engaged

/* ═══════════════════════════════════════════════════════════════════
 *  Single precision -- FPv5-SP hardware
 *
 *  RP2350's Cortex-M33 has a real single-precision FPU, so floats never touch
 *  the DCP.  Arguments arrive in GPRs (soft-float ABI).
 * ═══════════════════════════════════════════════════════════════════ */

.global __aeabi_fadd
.type __aeabi_fadd, %function
__aeabi_fadd:
    vmov s0, r0
    vmov s1, r1
    vadd.f32 s0, s0, s1
    vmov r0, s0
    bx lr
.size __aeabi_fadd, .-__aeabi_fadd

.global __aeabi_fsub
.type __aeabi_fsub, %function
__aeabi_fsub:
    vmov s0, r0
    vmov s1, r1
    vsub.f32 s0, s0, s1
    vmov r0, s0
    bx lr
.size __aeabi_fsub, .-__aeabi_fsub

.global __aeabi_frsub
.type __aeabi_frsub, %function
__aeabi_frsub:
    vmov s0, r1
    vmov s1, r0
    vsub.f32 s0, s0, s1
    vmov r0, s0
    bx lr
.size __aeabi_frsub, .-__aeabi_frsub

.global __aeabi_fmul
.type __aeabi_fmul, %function
__aeabi_fmul:
    vmov s0, r0
    vmov s1, r1
    vmul.f32 s0, s0, s1
    vmov r0, s0
    bx lr
.size __aeabi_fmul, .-__aeabi_fmul

.global __aeabi_fdiv
.type __aeabi_fdiv, %function
__aeabi_fdiv:
    vmov s0, r0
    vmov s1, r1
    vdiv.f32 s0, s0, s1
    vmov r0, s0
    bx lr
.size __aeabi_fdiv, .-__aeabi_fdiv

.global __aeabi_i2f
.type __aeabi_i2f, %function
__aeabi_i2f:
    vmov s0, r0
    vcvt.f32.s32 s0, s0
    vmov r0, s0
    bx lr
.size __aeabi_i2f, .-__aeabi_i2f

.global __aeabi_ui2f
.type __aeabi_ui2f, %function
__aeabi_ui2f:
    vmov s0, r0
    vcvt.f32.u32 s0, s0
    vmov r0, s0
    bx lr
.size __aeabi_ui2f, .-__aeabi_ui2f

.global __aeabi_f2iz
.type __aeabi_f2iz, %function
__aeabi_f2iz:
    vmov s0, r0
    vcvt.s32.f32 s0, s0
    vmov r0, s0
    bx lr
.size __aeabi_f2iz, .-__aeabi_f2iz

.global __aeabi_f2uiz
.type __aeabi_f2uiz, %function
__aeabi_f2uiz:
    vmov s0, r0
    vcvt.u32.f32 s0, s0
    vmov r0, s0
    bx lr
.size __aeabi_f2uiz, .-__aeabi_f2uiz

/* ═══════════════════════════════════════════════════════════════════
 *  Double comparison -- DCP
 *
 *  RCMP returns the relation in the NZCV bit positions, so reading it into
 *  APSR_nzcv (mrc with Rt == PC) puts it straight into the flags.  The
 *  ordering is then tested with *unsigned* conditions: C set means >=, Z set
 *  means ==, V set means unordered.  Recipes follow the Pico SDK's
 *  double_aeabi_dcp.S exactly -- the NaN behaviour here is subtle and the
 *  conformance vectors (all six relations over every NaN/Inf/zero pairing)
 *  are what validate it.
 * ═══════════════════════════════════════════════════════════════════ */

.global __aeabi_dcmpun
.type __aeabi_dcmpun, %function
__aeabi_dcmpun:
    mcrr p4, #1, r0, r1, c0
    mcrr p4, #1, r2, r3, c1
    cdp  p4, #0, c0, c0, c1, #0
    mrc  p4, #0, r0, c0, c0, #1     /* RCMP r0 */
    lsls r0, r0, #3                 /* V (bit 28) -> bit 31 */
    lsrs r0, r0, #31                /* ...and down to 0/1 (no ubfx in tcc's asm) */
    bx lr
.size __aeabi_dcmpun, .-__aeabi_dcmpun

.global __aeabi_dcmpeq
.type __aeabi_dcmpeq, %function
__aeabi_dcmpeq:
    mcrr p4, #1, r0, r1, c0
    mcrr p4, #1, r2, r3, c1
    cdp  p4, #0, c0, c0, c1, #0
    mrc  p4, #0, r0, c0, c0, #1
    lsls r0, r0, #1                 /* Z (bit 30) -> bit 31 */
    lsrs r0, r0, #31                /* ...and down to 0/1 (no ubfx in tcc's asm) */
    bx lr
.size __aeabi_dcmpeq, .-__aeabi_dcmpeq

/* a < b  is tested as the reversed compare with `hi` */
.global __aeabi_dcmplt
.type __aeabi_dcmplt, %function
__aeabi_dcmplt:
    mcrr p4, #1, r2, r3, c0
    mcrr p4, #1, r0, r1, c1
    cdp  p4, #0, c0, c0, c1, #0
    mrc  p4, #0, r15, c0, c0, #1    /* RCMP apsr_nzcv */
    ite hi
    movhi r0, #1
    movls r0, #0
    bx lr
.size __aeabi_dcmplt, .-__aeabi_dcmplt

.global __aeabi_dcmple
.type __aeabi_dcmple, %function
__aeabi_dcmple:
    mcrr p4, #1, r2, r3, c0
    mcrr p4, #1, r0, r1, c1
    cdp  p4, #0, c0, c0, c1, #0
    mrc  p4, #0, r15, c0, c0, #1
    ite hs
    movhs r0, #1
    movlo r0, #0
    bx lr
.size __aeabi_dcmple, .-__aeabi_dcmple

.global __aeabi_dcmpge
.type __aeabi_dcmpge, %function
__aeabi_dcmpge:
    mcrr p4, #1, r0, r1, c0
    mcrr p4, #1, r2, r3, c1
    cdp  p4, #0, c0, c0, c1, #0
    mrc  p4, #0, r15, c0, c0, #1
    ite hs
    movhs r0, #1
    movlo r0, #0
    bx lr
.size __aeabi_dcmpge, .-__aeabi_dcmpge

.global __aeabi_dcmpgt
.type __aeabi_dcmpgt, %function
__aeabi_dcmpgt:
    mcrr p4, #1, r0, r1, c0
    mcrr p4, #1, r2, r3, c1
    cdp  p4, #0, c0, c0, c1, #0
    mrc  p4, #0, r15, c0, c0, #1
    ite hi
    movhi r0, #1
    movls r0, #0
    bx lr
.size __aeabi_dcmpgt, .-__aeabi_dcmpgt

/* ═══════════════════════════════════════════════════════════════════
 *  Flag-setting compares
 *
 *  These do NOT follow the AEABI's own ZCV encoding (Z=C=1 equal, Z=C=0 less,
 *  Z=0/C=1 greater-or-unordered, tested with *unsigned* conditions).  They
 *  follow libsoftfp's convention instead: flags as if `cmp r, #0` on the
 *  three-way result -1/0/1(/2), tested with *signed* conditions.  That is
 *  what tcc emits -- ir/gen/float.c swaps the operands for >/>= and then
 *  `tcc_gen_machine_fp_mop()` tests lt/le/eq/ne:
 *
 *      bl __aeabi_cdcmple ; ite lt ; movlt r0,#1 ; movge r0,#0
 *
 *  and it is what lib/fp/soft/{fcmp,dcmp}_asm.S produce.  Diverging here
 *  would break the plan's premise that portable-mode and fast-mode objects
 *  are link-compatible, since a translation unit compiled either way emits
 *  the same call and the same condition code.  pico-sdk's double_aeabi_dcp.S
 *  is AEABI-conformant instead because GCC tests unsigned conditions.
 *
 *  RCMP reports, in the NZCV bit positions:
 *      bit 30 (Z) = ordered and a == b
 *      bit 29 (C) = ordered and a >= b
 *      bit 28 (V) = unordered
 *  so C and V are both clear exactly when a < b.
 *
 *  libsoftfp gives cdcmplt/cdcmpgt/cdcmpge the same body as cdcmple (the
 *  three-way flags already answer every relation); they are aliases here for
 *  the same reason, and so that no soft dcmp object is ever pulled in.
 * ═══════════════════════════════════════════════════════════════════ */
.global __aeabi_cdcmple
.type __aeabi_cdcmple, %function
.global __aeabi_cdcmpeq
.type __aeabi_cdcmpeq, %function
.global __aeabi_cdcmplt
.type __aeabi_cdcmplt, %function
.global __aeabi_cdcmpgt
.type __aeabi_cdcmpgt, %function
.global __aeabi_cdcmpge
.type __aeabi_cdcmpge, %function
__aeabi_cdcmple:
__aeabi_cdcmpeq:
__aeabi_cdcmplt:
__aeabi_cdcmpgt:
__aeabi_cdcmpge:
    mcrr p4, #1, r0, r1, c0         /* WXUP r0,r1 */
    mcrr p4, #1, r2, r3, c1         /* WYUP r2,r3 */
    cdp  p4, #0, c0, c0, c1, #0     /* ADD0 */
    mrc  p4, #0, r0, c0, c0, #1     /* RCMP r0 */
    b    .Ldcmp_flags
.size __aeabi_cdcmple, .-__aeabi_cdcmple

.global __aeabi_cdrcmple
.type __aeabi_cdrcmple, %function
__aeabi_cdrcmple:
    mcrr p4, #1, r2, r3, c0         /* WXUP r2,r3 -- operands reversed */
    mcrr p4, #1, r0, r1, c1         /* WYUP r0,r1 */
    cdp  p4, #0, c0, c0, c1, #0     /* ADD0 */
    mrc  p4, #0, r0, c0, c0, #1     /* RCMP r0 */
    /* fall through */

/* Translate the RCMP word in r0 into `cmp <three-way>, #0` flags.  Each tail
 * ends in cmp/subs, which define V; movs alone would leave it stale and the
 * signed conditions test N against V.
 *
 * The `%function` types are load-bearing, not decoration: tcc's assembler
 * leaves branches to local labels as relocations and emits no $t mapping
 * symbols, so ld has nothing else to tell it the target is Thumb and rejects
 * the R_ARM_THM_CALL from `b` with "Unknown destination type (ARM/Thumb)".
 * %function makes it set the Thumb bit in st_value. */
.type .Ldcmp_flags, %function
.Ldcmp_flags:
    lsls r1, r0, #1                 /* Z -> bit 31 (no ubfx in tcc's asm) */
    bmi  .Ldcmp_eq
    lsls r1, r0, #2                 /* C -> bit 31 */
    bmi  .Ldcmp_gt
    lsls r1, r0, #3                 /* V -> bit 31 */
    bmi  .Ldcmp_gt                  /* unordered scores like "greater": only ne */
    movs r0, #0
    subs r0, r0, #1                 /* -1: N=1 Z=0 V=0 -> lt, le, ne */
    bx lr
.type .Ldcmp_eq, %function
.Ldcmp_eq:
    movs r0, #0
    cmp  r0, #0                     /*  0: Z=1 N=0 V=0 -> le, eq */
    bx lr
.type .Ldcmp_gt, %function
.Ldcmp_gt:
    movs r0, #1
    cmp  r0, #0                     /*  1: N=0 Z=0 V=0 -> ne */
    bx lr
.size __aeabi_cdrcmple, .-__aeabi_cdrcmple

/* ═══════════════════════════════════════════════════════════════════
 *  Sign flip -- no coprocessor needed
 * ═══════════════════════════════════════════════════════════════════ */

.global __aeabi_dneg
.type __aeabi_dneg, %function
__aeabi_dneg:
    eor r1, r1, #0x80000000         /* toggle the sign bit of the high word */
    bx lr
.size __aeabi_dneg, .-__aeabi_dneg

/* __aeabi_f2d: the soft ABI passes the float's bits in r0, which is exactly
 * what the DCP float->double sequence wants -- so __aeabi_f2d_bits, whose
 * argument *is* those bits, is the same code and not just a tail call. */
.global __aeabi_f2d
.type __aeabi_f2d, %function
.global __aeabi_f2d_bits
.type __aeabi_f2d_bits, %function
__aeabi_f2d:
__aeabi_f2d_bits:
    mcrr p4, #1, r0, r0, c2         /* WXYU r0,r0 */
    cdp  p4, #8, c0, c0, c0, #1     /* NRDD */
    mrrc p4, #11, r0, r1, c0        /* RDDG r0,r1 */
    bx lr
.size __aeabi_f2d, .-__aeabi_f2d
