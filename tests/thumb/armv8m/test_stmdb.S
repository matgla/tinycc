.syntax unified
.thumb

.global _start
_start:

.global test_stmdb
test_stmdb:
    stmdb r0!, {r1-r4, r5}
    stmdb r1!, {r2, r3}
    stmdb r3, {r1, r2}
    stmdb r9!, {r2-r3, r8}
    stmdb r9!, {r2-r3, r8, lr}

    ittee eq
    stmdbeq r0!, {r1-r4, r5}
    stmdbeq r1!, {r2, r3}
    stmdbne r3, {r1, r2}
    stmdbne r9!, {r2-r3, r8}

    stmfd r3, {r1, r2, lr}

    ittee eq
    stmfdeq r0!, {r1-r9, r10}
    stmfdeq r1!, {r2, r4}
    stmfdne r3, {r1, r2}
    stmfdne r9!, {r2-r3, r8}

    stmfd.w sp!, {r1-r4, r5}
    stmfd.w sp!, {r1-r4, r5, lr}
    stmfd.w sp, {r1, r2}
    stmfd.w sp!, {r2-r3, r8}

    ittee eq
    stmfdeq.w sp!, {r1-r4, r5}
    stmfdeq.w sp!, {r2, r7}
    stmfdne.w sp!, {r1, r2}
    stmfdne.w sp!, {r2-r3, r8}

    stmfd lr!, {r2-r3, r8}

