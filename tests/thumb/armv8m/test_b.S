.syntax unified
.thumb

.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"

.global jump_target
jump_target:
    movs r0, #0
    cmp r1, #1

.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"


.global test_b
test_b:
b.n jump_target
b.w next_target 
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"

next_target:
    movs r1, #1
    b.n jump_target
    it eq
    beq.n test_b

    it ne 
    bne.n next_target

    it gt 
    bgt.n jump_target

    cmp r0, #1 
    bgt.n test_b

    cmp r0, #2
    bgt.w test_b 




