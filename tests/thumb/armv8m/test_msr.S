.syntax unified
.thumb

.global _start
_start:

.global test_msr
test_msr:
    msr ipsr, r5
    msr epsr, r6
    msr msp, r7
    msr psp, r8
    msr msplim, r9
    msr psplim, r10
    msr primask, r11
    msr basepri, r0
    msr faultmask, r1
    msr control, r2
    msr msp_ns, r3
    msr psp_ns, r4
    msr msplim_ns, r5
    msr psplim_ns, r6
    msr primask_ns, r7
    msr basepri_ns, r8
    msr faultmask_ns, r9
    msr control_ns, r10
    msr sp_ns, r11

    msr apsr_nzcvq, r1
    msr iapsr_nzcvq, r2
    msr eapsr_nzcvq, r3
    msr xpsr_nzcvq, r4

    msr apsr_g, r1
    msr iapsr_g, r2
    msr eapsr_g, r3
    msr xpsr_g, r4

    msr apsr_nzcvqg, r1
    msr iapsr_nzcvqg, r2
    msr eapsr_nzcvqg, r3
    msr xpsr_nzcvqg, r4