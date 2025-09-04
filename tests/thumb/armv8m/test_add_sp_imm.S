.syntax unified
.thumb

.global _start 
_start:

.global test_add_sp_imm
test_add_sp_imm:
   add r0, sp, #12
   add.n r1, sp, #20

   itte ge
   addge r6, sp, #1 
   addge r7, sp, #2
   addlt r2, sp, #4

   add sp, sp, #0xab
   add sp, #12
   add.n r1, sp, #508
   add.n sp, #104

   itte ge
   addge sp, #1 
   addge sp, sp, #2
   addlt sp, #4


   add.w r0, sp, #0xab
   add.w sp, #0x10
   add r10, sp, #0xde

   itte ge
   addge.w r6, sp, #1 
   addge r10, sp, #2
   addlt.w sp, #4

   adds r0, sp, #0xabababab
   adds sp, #0x10
   adds r10, sp, #0xde

   itte ge
   addsge r6, sp, #1 
   addsge r10, sp, #2
   addslt sp, #4

   add sp, #4095
   addw r1, sp, #1
   add r9, sp, #0x202
   addw r8, sp, #1



    