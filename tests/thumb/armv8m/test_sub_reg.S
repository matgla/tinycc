.syntax unified
.thumb

.global _start
_start:

.global test_sub_reg
test_sub_reg:
   // T1
   subs r2, r2, r2
   subs r1, r2, r3
   subs r1, r4

   itte eq
   subeq r3, r2, r1
   subeq r3, r2, r1
   subne r1, r2, r3

   // T2
   sub r8, r9

   itte eq
   subeq r1, r9
   subeq r3, r3, r1
   subne r9, r8

   sub r1, r2, r3, rrx
   sub r10, r11, r12, rrx
   itte eq
   subeq r10, r11, r12
   subeq r10, r12
   subne r11, r1

   sub r1, r1, r2, lsl #1
   sub r6, r6, r8, lsr #2
   sub r1, r1, r2, asr #3
   sub r6, r6, r8, ror #4

   sub r1, r4, r2, lsl #1
   sub r6, r4, r8, lsr #2
   sub r1, r8, r2, asr #3
   sub r6, r7, r8, ror #4

   subs r1, r2, r3, rrx
   subs r10, r11, r12, rrx

   subs r1, r1, r2, lsl #1
   subs r6, r6, r8, lsr #2
   subs r1, r1, r2, asr #3
   subs r6, r6, r8, ror #4

   subs r1, r4, r2, lsl #1
   subs r6, r4, r8, lsr #2
   subs r1, r8, r2, asr #3
   subs r6, r7, r8, ror #4

   ittee eq
   subeq r1, r1, r2, lsl #1
   subeq r6, r6, r8, lsr #2
   subne r1, r1, r2, asr #3
   subne r6, r6, r8, ror #4

   ittee eq
   subseq r1, r4, r2, lsl #1
   subseq r6, r4, r8, lsr #2
   subsne r1, r8, r2, asr #3
   subsne r6, r7, r8, ror #4