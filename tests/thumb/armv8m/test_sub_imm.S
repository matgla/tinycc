.syntax unified
.thumb

.global _start
_start:

.global test_sub_imm
test_sub_mm:
   // T1
   subs r1, r2, #3
   subs r4, r5, #1

   itte eq
   subeq r1, r2, #2
   subeq r3, r1, #3
   subne r2, r3, #1


   // T2
   subs r1, r1, #0xab
   subs r2, #0xcd
   subs r2, #3
   subs r3, #0xde

   itte eq
   subeq r1, #2
   subeq r3, #3
   subne r2, #1

   // T3
   sub.w r1, #0x2
   sub.w r1, r2, #3
   sub r2, #0xdededede
   sub r3, #0xab

   subs.w r1, #0x2
   subs.w r1, r2, #3
   subs r2, #0xdededede
   subs r3, #0xab

   ittee eq
   subeq.w r1, #0x2
   subeq.w r1, r2, #3
   subne r2, #0xdededede
   subne r3, #0xab

   ittee eq
   subseq r1, #0x2
   subseq r1, r2, #3
   subsne r2, #0xdededede
   subsne r1, r3, #0xff00ff00

   sub r10, r11, #0xfac
   subw r1, r2, #0xbe
   subw r2, r3, #0xfac
