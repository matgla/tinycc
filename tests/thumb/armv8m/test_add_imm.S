.syntax unified
.thumb

.global _start 
_start:

.global test_add_imm
test_add_mm:
   // T1
   adds r1, r2, #3
   adds r4, r5, #1

   itte eq
   addeq r1, r2, #2
   addeq r3, r1, #3
   addne r2, r3, #1


   // T2
   adds r1, r1, #0xab
   adds r2, #0xcd
   adds r2, #3
   adds r3, #0xde

   itte eq
   addeq r1, #2
   addeq r3, #3
   addne r2, #1

   // T3
   add.w r1, #0x2
   add.w r1, r2, #3
   add r2, #0xdededede
   add r3, #0xab

   adds.w r1, #0x2
   adds.w r1, r2, #3
   adds r2, #0xdededede
   adds r3, #0xab

   ittee eq
   addeq.w r1, #0x2
   addeq.w r1, r2, #3
   addne r2, #0xdededede
   addne r3, #0xab

   ittee eq
   addseq r1, #0x2
   addseq r1, r2, #3
   addsne r2, #0xdededede
   addsne r1, r3, #0xff00ff00

   add r10, r11, #0xfac
   addw r1, r2, #0xbe
   addw r2, r3, #0xfac
