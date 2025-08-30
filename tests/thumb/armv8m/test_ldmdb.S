.syntax unified
.thumb

.global _start 
_start:

.global test_ldmdb
test_ldmdb:
    ldmdb r0!, {r1-r4, r5}
    ldmdb r1!, {r2, r3}
    ldmdb r3, {r1, r10}
    ldmdb r9!, {r2-r3, r8}
    ldmdb r9!, {r2-r3, r8, pc}

    ittee eq
    ldmdbeq r0!, {r1-r4, r5}
    ldmdbeq r1!, {r2, r10}
    ldmdbne r3, {r1, r2}
    ldmdbne r9!, {r2-r3, r8}

    ldmdb sp!, {r1-r4, r5}
    ldmdb.w sp!, {r1-r4, r5, pc}
    ldmdb sp!, {r2, r11}
    ldmdb sp, {r1, r2}
    ldmdb sp!, {r2-r3, r8}

    ittee eq
    ldmdbeq sp!, {r1-r4, r5}
    ldmdbeq sp!, {r2, r12}
    ldmdbne sp!, {r1, r2}
    ldmdbne sp!, {r2-r3, r8}
    
    ldmdb lr!, {r2-r3, r8}

    ldmea r0!, {r1-r4, r5}
    ldmea r1!, {r2, r14}
    ldmea r3, {r1, r2}
    ldmea r9!, {r2-r3, r8}
    ldmea r9!, {r2-r3, r8, pc}

    ittee eq
    ldmeaeq r0!, {r1-r4, r5}
    ldmeaeq r1!, {r2, r3}
    ldmeane r3, {r1, r2}
    ldmeane r9!, {r2-r3, r8}

    ldmdb sp!, {r1-r4, r5}
    ldmdb.w sp!, {r1-r4, r5, pc}
    ldmdb sp!, {r2, r11}
    ldmdb sp, {r1, r2}
    ldmdb sp!, {r2-r3, r8}

    ittee eq
    ldmdbeq sp!, {r1-r4, r5}
    ldmdbeq sp!, {r2, r8}
    ldmdbne sp!, {r1, r2}
    ldmdbne sp!, {r2-r3, r8}
    
    ldmdb lr!, {r2-r3, r8}
    
    
    
       