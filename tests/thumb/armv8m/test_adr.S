.syntax unified
.thumb

.global _start
_start:

prev_label:
   mov r1, #1 

.global test_adr
test_adr:
   adr r2, prev_label
   movs r1, #0 
   adr r1, prev_label
   adr r3, next_label 
   adr r1, prev_label
   adr.w r2, prev_label
   adr.w r3, next_label 
   
   ite eq
   adreq r3, prev_label 
   adrne r8, next_label

next_label:
  movs r2, #3
  adr r1, prev_label
  adr.w r4, far_label 
  ite eq
  adreq r3, test_adr 
  adrne r8, far_label 
  
  
.asciz "Helloasfh9ueawh9fhe9wahf79wah9efh9wahe97fh9w7ehf79haf"
    
far_label: 
   adr r1, next_label
   add r2, pc, #10
   add r3, pc, #8
   adr r1, last 
   movs r1, #1 
.global last
last: 
   movs r2, #3