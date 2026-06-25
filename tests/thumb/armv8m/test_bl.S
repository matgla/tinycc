.syntax unified
.thumb

.global _start 
_start:

.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"

.global jump_target
.thumb_func
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


.global test_bl
test_bl:
bl jump_target
