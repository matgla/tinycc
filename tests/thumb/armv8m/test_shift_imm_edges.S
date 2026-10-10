.syntax unified
.thumb

.global _start
_start:

.global test_shift_imm_edges
test_shift_imm_edges:
    @ 16-bit flag-setting forms: #0 / #1 / #31 / #32
    lsls r0, r1, #1
    lsls r0, r1, #31
    lsrs r0, r1, #1
    lsrs r0, r1, #31
    lsrs r0, r1, #32
    asrs r0, r1, #1
    asrs r0, r1, #31
    asrs r0, r1, #32
    lsls r2, r3, #0

    @ wide forms
    lsl.w r0, r1, #1
    lsl.w r0, r1, #31
    lsr.w r0, r1, #1
    lsr.w r0, r1, #31
    lsr.w r0, r1, #32
    asr.w r0, r1, #1
    asr.w r0, r1, #31
    asr.w r0, r1, #32
    ror.w r0, r1, #1
    ror.w r0, r1, #31
    lsls.w r8, r9, #32 - 1
    lsrs.w r8, r9, #32
    asrs.w r8, r9, #32

    @ #0 means a plain move
    lsr r0, r1, #0
    asr r0, r1, #0
    lsl r0, r1, #0
    ror r0, r1, #0
    lsr r8, r9, #0
    asr r8, r9, #0

    @ unflagged 32-bit-only forms with high registers
    lsr r8, r9, #32
    asr r8, r9, #32

    @ inside an IT block (16-bit non-flag-setting)
    ite eq
    lsreq r0, r1, #32
    asrne r0, r1, #32
    it eq
    asreq r0, r1, #31
    bx lr
