.syntax unified
.thumb

.global _start
_start:

.global test_mrs
test_mrs:
    mrs r0, apsr
    mrs r1, iapsr
    mrs r2, eapsr
    mrs r3, xpsr
    mrs r4, ipsr
    mrs r5, epsr
    mrs r5, msp
    mrs r6, psp
    mrs r7, msplim
    mrs r8, psplim
    mrs r9, primask
    mrs r10, basepri
    mrs r11, faultmask
    mrs r0, control
    mrs r0, msp_ns
    mrs r1, psp_ns
    mrs r2, msplim_ns
    mrs r3, psplim_ns
    mrs r4, primask_ns
    mrs r5, basepri_ns
    mrs r6, faultmask_ns
    mrs r7, control_ns
    mrs r8, sp_ns

