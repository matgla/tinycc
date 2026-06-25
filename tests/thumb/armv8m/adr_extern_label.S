.syntax unified
.thumb

.global _start
_start:

prev_adr_label:
    movs r0, #1

.global test_adr_local
test_adr_local:
    adr r0, prev_adr_label
    adr r1, prev_adr_label
    adr.w r2, prev_adr_label
    adr.w r3, prev_adr_label