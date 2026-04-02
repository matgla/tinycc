.syntax unified
.thumb

.global _start
_start:

.global test_shift_imm
test_shift_imm:

    /* T1 forms - LSL */
    lsls r0, r1, #3
    lsls r2, r3, #31
    lsls r0, r1, #0

    /* T1 forms - LSR */
    lsrs r2, r3, #4
    lsrs r2, r3, #31

    /* T1 forms - ASR */
    asrs r3, r4, #5
    asrs r5, r6, #1

    /* T3 forms - LSL */
    lsl.w r8, r9, #3
    lsl.w r0, r1, #5

    /* T3 forms - LSR */
    lsr.w r8, r9, #4

    /* T3 forms - ASR */
    asr.w r8, r9, #4

    /* T3 forms - ROR (T32 only) */
    ror.w r0, r1, #5
    ror.w r8, r9, #7

    /* IT block variants */
    ittee eq
    lslseq r0, r1, #3
    lsreq.w r8, r9, #4
    lsrne r2, r3, #5
    asrne r4, r5, #6

    ite cs
    lsrcs r0, r1, #7
    lslcc r2, r3, #8

    itt ne
    rorne r0, r1, #9
    asrne r4, r5, #10

    /* High reg variants (T3 only) */
    lsls r8, r1, #3
    lsrs r8, r1, #4
    asrs r8, r1, #2
    rors r8, r1, #1

    /* Wide variants with S suffix */
    lsls.w r0, r1, #3
    lsrs.w r8, r9, #4
    asrs.w r8, r9, #2
    rors.w r0, r1, #5