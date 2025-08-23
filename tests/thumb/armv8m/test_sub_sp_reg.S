.syntax unified
.thumb

.global _start
_start:

.global test_sub_sp_reg
test_sub_sp_reg:
   sub r0, sp, r0
   sub sp, r0

   ite eq
   subeq r1, sp, r1
   subne r1, sp, r2, rrx

   sub r9, sp, r1, rrx
   ite eq
   subeq r1, sp, r2, rrx
   subne r1, sp, r1, rrx

   sub.w r1, sp, r1

   sub r1, sp, r2, lsl #1
   sub r6, sp, r8, lsr #2
   sub r1, sp, r2, asr #3
   sub r6, sp, r8, ror #4

   ittee eq
   subeq r1, sp, r2, lsl #1
   subeq r6, sp, r8, lsr #2
   subne r1, sp, r2, asr #3
   subne r6, sp, r8, ror #4

   subs r1, sp, r2, rrx

   ite eq
   subseq r1, sp, r2, rrx
   subsne r5, sp, r1, rrx

   subs r1, sp, r2, lsl #1
   subs r6, sp, r8, lsr #2
   subs r1, sp, r2, asr #3
   subs r6, sp, r8, ror #4

   ittee eq
   subseq r1, sp, r2, lsl #1
   subseq r6, sp, r8, lsr #2
   subsne r1, sp, r2, asr #3
   subsne r6, sp, r8, ror #4


   sub r1, pc, 1234
   sub r3, pc, #0xfac

