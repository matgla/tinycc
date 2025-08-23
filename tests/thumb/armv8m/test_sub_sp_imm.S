.syntax unified
.thumb

.global _start
_start:

.global test_sub_sp_imm
test_sub_sp_imm:
   sub r0, sp, #12

   itte ge
   subge r6, sp, #1
   subge r7, sp, #2
   sublt r2, sp, #4

   sub sp, sp, #0xab
   sub sp, #12
   sub.n sp, #104

   itte ge
   subge sp, #1
   subge sp, sp, #2
   sublt sp, #4


   sub.w r0, sp, #0xab
   sub.w sp, #0x10
   sub r10, sp, #0xde

   itte ge
   subge.w r6, sp, #1
   subge r10, sp, #2
   sublt.w sp, #4

   subs r0, sp, #0xabababab
   subs sp, #0x10
   subs r10, sp, #0xde

   itte ge
   subsge r6, sp, #1
   subsge r10, sp, #2
   subslt sp, #4

   sub sp, #4095
   subw r1, sp, #1
   sub r9, sp, #0x202
   subw r8, sp, #1



