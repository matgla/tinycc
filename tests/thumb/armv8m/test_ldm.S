.syntax unified
.thumb

.global _start 
_start:

.global test_ldm
test_ldm:
    ldm r0!, {r1-r4, r5}
    ldm r1!, {r2}
    ldm r3, {r1, r2}
    ldm r9!, {r2-r3, r8}
    ldm r9!, {r2-r3, r8, pc}

    ittee eq
    ldmeq r0!, {r1-r4, r5}
    ldmeq r1!, {r2}
    ldmne r3, {r1, r2}
    ldmne r9!, {r2-r3, r8}

    ldm r3, {r1, r2, pc}

    ittee eq
    ldmiaeq r0!, {r1-r9, r10}
    ldmiaeq r1!, {r2}
    ldmiane r3, {r1, r2}
    ldmiane r9!, {r2-r3, r8}

    ittee eq 
    ldmfdeq r0!, {r1-r4, r5}
    ldmfdeq r1!, {r2}
    ldmfdne r3, {r1, r2}
    ldmfdne r9!, {r2-r3, r8, pc}


    ldm sp!, {r1-r4, r5}
    ldm.w sp!, {r1-r4, r5, pc}
    ldm sp!, {r2}
    ldm sp, {r1, r2}
    ldm sp!, {r2-r3, r8}

    ittee eq
    ldmeq sp!, {r1-r4, r5}
    ldmeq sp!, {r2}
    ldmne sp!, {r1, r2}
    ldmne sp!, {r2-r3, r8}
    
    ldm lr!, {r2-r3, r8}
    
       