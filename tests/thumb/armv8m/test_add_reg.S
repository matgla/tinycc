.syntax unified
.thumb

.global _start 
_start:

.global test_add_reg
test_add_reg:
   // T1
   adds r2, r2, r2 
   adds r1, r2, r3
   adds r1, r4
   
   itte eq
   addeq r3, r2, r1
   addeq r3, r2, r1
   addne r1, r2, r3
   
   // T2
   add r8, r9
    
   itte eq
   addeq r1, r9 
   addeq r3, r3, r1
   addne r9, r8 

   add r1, r2, r3, rrx 
   add r10, r11, r12, rrx 
   itte eq
   addeq r10, r11, r12 
   addeq r10, r12
   addne r11, r1

   add r1, r1, r2, lsl #1
   add r6, r6, r8, lsr #2
   add r1, r1, r2, asr #3
   add r6, r6, r8, ror #4
   
   add r1, r4, r2, lsl #1
   add r6, r4, r8, lsr #2
   add r1, r8, r2, asr #3
   add r6, r7, r8, ror #4

   adds r1, r2, r3, rrx 
   adds r10, r11, r12, rrx 

   adds r1, r1, r2, lsl #1
   adds r6, r6, r8, lsr #2
   adds r1, r1, r2, asr #3
   adds r6, r6, r8, ror #4
   
   adds r1, r4, r2, lsl #1
   adds r6, r4, r8, lsr #2
   adds r1, r8, r2, asr #3
   adds r6, r7, r8, ror #4

   ittee eq
   addeq r1, r1, r2, lsl #1
   addeq r6, r6, r8, lsr #2
   addne r1, r1, r2, asr #3
   addne r6, r6, r8, ror #4
   
   ittee eq
   addseq r1, r4, r2, lsl #1
   addseq r6, r4, r8, lsr #2
   addsne r1, r8, r2, asr #3
   addsne r6, r7, r8, ror #4