.syntax unified
.thumb

.global test_add_sp_reg
test_add_sp_reg:
   add r0, sp, r0
   add sp, r0
   
   ite eq
   addeq r1, sp, r1 
   addne sp, r9

   add sp, sp, r2
   add sp, sp

   ite ge 
   addge sp, sp, r2
   addlt sp, sp

   add r1, sp, r2, rrx
   add r9, sp, r1, rrx
   ite eq 
   addeq r1, sp, r2, rrx
   addne r1, sp, r1, rrx

   add.w r1, sp, r1

   add r1, sp, r2, lsl #1
   add r6, sp, r8, lsr #2
   add r1, sp, r2, asr #3
   add r6, sp, r8, ror #4
    
   ittee eq
   addeq r1, sp, r2, lsl #1
   addeq r6, sp, r8, lsr #2
   addne r1, sp, r2, asr #3
   addne r6, sp, r8, ror #4

   adds r1, sp, r2, rrx

   ite eq 
   addseq r1, sp, r2, rrx
   addsne r5, sp, r1, rrx

   adds r1, sp, r2, lsl #1
   adds r6, sp, r8, lsr #2
   adds r1, sp, r2, asr #3
   adds r6, sp, r8, ror #4
    
   ittee eq
   addseq r1, sp, r2, lsl #1
   addseq r6, sp, r8, lsr #2
   addsne r1, sp, r2, asr #3
   addsne r6, sp, r8, ror #4

