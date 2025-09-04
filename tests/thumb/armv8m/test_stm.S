.syntax unified
.thumb

.global _start
_start:

.global test_stm
test_stm:
    stm r0!, {r1-r4, r5}
    stm r1!, {r2}
    stm r3, {r1, r2}
    stm r9!, {r2-r3, r8}
    stm r9!, {r2-r3, r8, lr}

    ittee eq
    stmeq r0!, {r1-r4, r5}
    stmeq r1!, {r2}
    stmne r3, {r1, r2}
    stmne r9!, {r2-r3, r8}

    stm r3, {r1, r2, lr}

    ittee eq
    stmiaeq r0!, {r1-r9, r10}
    stmiaeq r1!, {r2}
    stmiane r3, {r1, r2}
    stmiane r9!, {r2-r3, r8}

    ittee eq
    stmiaeq r0!, {r1-r4, r5}
    stmiaeq r1!, {r2}
    stmiane r3, {r1, r2}
    stmiane r9!, {r2-r3, r8, lr}


    stm.w sp!, {r1-r4, r5}
    stm.w sp!, {r1-r4, r5, lr}
    stm.w sp, {r1, r2}
    stm.w sp!, {r2-r3, r8}

    ittee eq
    stmeq.w sp!, {r1-r4, r5}
    stmeq.w sp!, {r2, r7}
    stmne.w sp!, {r1, r2}
    stmne.w sp!, {r2-r3, r8}

    stm lr!, {r2-r3, r8}

    stmea r0!, {r1-r4, r5}
    stmea r1!, {r2}
    stmea r3, {r1, r2}
    stmea r9!, {r2-r3, r8, lr}


