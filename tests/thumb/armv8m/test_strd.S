.syntax unified
.thumb

.global _start
_start:

.global test_strd
test_strd:
    strd r1, r2, [r3, #32]
    strd r3, r4, [r3, #-32]
    strd r10, r11, [r3]
    strd r10, r11, [r3], #8
    strd r2, r3, [r0], #-8
    strd r1, r2, [r3, #32]
    strd r3, r4, [r5, #-32]
    strd r10, r11, [r3]

    ittee eq
    strdeq r1, r2, [r3, #32]
    strdeq r3, r4, [r5, #-32]
    strdne r10, r11, [r3]
    strdne r10, r11, [r3], #8

    ittee ne
    strdne r2, r3, [r0], #-8
    strdne r1, r2, [r3, #32]
    strdeq r3, r4, [r5, #-32]
    strdeq r10, r11, [r3]