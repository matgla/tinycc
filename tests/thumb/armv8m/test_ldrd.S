.syntax unified
.thumb

.global _start 
_start:

.global test_ldrbt
test_ldrbt:
    ldrd r1, r2, [r3, #32]
    ldrd r3, r4, [r3, #-32]
    ldrd r10, r11, [r3]
    ldrd r10, r11, [r3], #8
    ldrd r2, r3, [r0], #-8
    ldrd r1, r2, [r3, #32]!
    ldrd r3, r4, [r5, #-32]!
    ldrd r10, r11, [r3]!

    ittee eq
    ldrdeq r1, r2, [r3, #32]
    ldrdeq r3, r4, [r5, #-32]
    ldrdne r10, r11, [r3]
    ldrdne r10, r11, [r3], #8

    ittee ne 
    ldrdne r2, r3, [r0], #-8
    ldrdne r1, r2, [r3, #32]!
    ldrdeq r3, r4, [r5, #-32]!
    ldrdeq r10, r11, [r3]!   