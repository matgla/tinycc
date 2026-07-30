/*
 *  test_gen_prolog.c - suite for tcc_gen_machine_prolog()/epilog()/
 *  finish_noreturn() in arm-thumb-gen.c.
 *
 *  Same "call the real backend function directly, assert on the real
 *  emitted Thumb-2 bytes" style as test_gen_dispatch_smoke.c (backend/
 *  binary, build_backend/run_unit_tests_backend). No IR, no dispatch loop.
 *
 *  Oracles for the PUSH/POP register-list encodings are cross-checked
 *  against test_thop_block.c (the low-level th_push()/th_pop() encoder
 *  suite): a 16-bit T1 PUSH is 0xB400 | reglist (| 0x0100 if LR is in the
 *  list), and once any register above r7 is in the list (e.g. r10) it must
 *  use the 32-bit T2 encoding 0xE92D0000 | reglist (LR at bit 14). SUB
 *  SP,SP,#imm (T1, imm7*4) is 0xB080 | (imm/4), per test_thop_alu_imm.c's
 *  test_sub_imm_t16_sp_imm7.
 */

#define USING_GLOBALS
#include "ir.h"
#include "source/backend/arch/arm/arm.h"
#include "source/backend/arch/arm/thumb/thumb.h"
#include "ir/machine_op.h"
#include "codegen_backend_stubs.h"
#include "elfsec_stubs.h"

#include "ut.h"

/* ------------------------------------------------------------------ setup */

static void setup_gen(void)
{
  elfsec_reset();
  cgb_reset();
  arm_target_init("armv8-m.main", NULL, "cortex-m33", 0);
  cur_text_section = elfsec_new_section(".text");
  ind = 0;
  tcc_state->registers_for_allocator = 13;
  tcc_state->float_abi = ARM_HARD_FLOAT;

  /* tcc_gen_machine_prolog()/epilog() read several TCCState fields that
   * default to zero in BSS but may have been left mutated by an earlier
   * test in this same process -- be explicit. */
  tcc_state->ir = NULL;
  tcc_state->need_frame_pointer = 0;
  tcc_state->force_frame_pointer = 0;
  tcc_state->force_lr_save = 0;
  tcc_state->text_and_data_separation = 0;
  tcc_state->func_save_apply_args = 0;
}

static uint16_t read_le16(const unsigned char *p)
{
  return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t read_be_pair32(const unsigned char *p)
{
  /* Thumb-2 32-bit instructions are stored as two little-endian halfwords,
   * but the "opcode" value the th_* encoders/tests compare against (e.g.
   * 0xE92D5FFF in test_thop_block.c) is the two halfwords concatenated
   * big-endian-of-halfwords (first halfword in the high 16 bits). */
  uint32_t hi = read_le16(p);
  uint32_t lo = read_le16(p + 2);
  return (hi << 16) | lo;
}

/* ------------------------------------------------------------------ prolog */

UT_TEST(test_prolog_leaf_no_regs_no_stack_emits_nothing)
{
  setup_gen();

  /* leaf function, no callee-saved regs used, no locals: save_lr=0 (leaf),
   * need_fp=0 (default), registers_count=0 (even, no pad needed),
   * stack_size=0 -> no SUB SP. Nothing at all should be emitted. */
  tcc_gen_machine_prolog(/*leaffunc=*/1, /*used_registers=*/0, /*stack_size=*/0,
                          /*extra_prologue_regs=*/0);

  UT_ASSERT_EQ(ind, 0);

  return 0;
}

UT_TEST(test_prolog_nonleaf_no_regs_no_stack_pushes_lr_padded_with_r3)
{
  setup_gen();

  /* non-leaf -> save_lr=1. registers_count=1 (LR only) is odd; since
   * stack_size==0 and !need_fp, the dummy-pad path pushes R3 alongside LR
   * to keep PUSH count even instead of emitting a separate SUB/ADD SP. */
  tcc_gen_machine_prolog(/*leaffunc=*/0, /*used_registers=*/0, /*stack_size=*/0,
                          /*extra_prologue_regs=*/0);

  UT_ASSERT_EQ(ind, 2);
  /* PUSH {r3, lr}: T1 16-bit encoding 0xB500 | reglist(r3) | (1<<8 for lr)
   * per test_push_t1_with_lr's 0xB505 for {r0,r2,lr} -- reglist bit for R3
   * is bit 3 (0x08), lr flag is bit 8 (0x0100). */
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xB508);

  return 0;
}

UT_TEST(test_prolog_callee_saved_r4_r5_r10_emits_t2_push)
{
  setup_gen();

  /* used_registers bits 4, 5, 10 set (r4, r5, r10 -- all in the R4..R11
   * callee-saved scan range). leaffunc=1 so LR is not pushed. stack_size=8
   * (nonzero) routes the odd-count (3 regs) alignment pad through SUB SP
   * instead of a dummy pushed register, so the PUSH register list is
   * exactly {r4, r5, r10} -- unlike the previous test, nothing else is
   * folded into it. r10 is a high register (>r7), forcing the 32-bit T2
   * PUSH encoding 0xE92D0000 | reglist (no LR bit), matching test_push_t2's
   * 0xE92D0000-base formula in test_thop_block.c. */
  uint64_t used = (1ull << R4) | (1ull << R5) | (1ull << R10);
  tcc_gen_machine_prolog(/*leaffunc=*/1, used, /*stack_size=*/8,
                          /*extra_prologue_regs=*/0);

  /* PUSH.W (4 bytes) followed by SUB SP,SP,#12 (4=pad + 8=stack_size, T1
   * 2-byte encoding). */
  UT_ASSERT_EQ(ind, 6);
  uint32_t reglist = (1u << R4) | (1u << R5) | (1u << R10);
  UT_ASSERT_EQ(read_be_pair32(cur_text_section->data), 0xE92D0000u | reglist);

  /* SUB SP, SP, #12 -> T1 0xB080 | (12/4) = 0xB083, per
   * test_sub_imm_t16_sp_imm7's 0xb084 for #16 (16/4=4). */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 4), 0xB083);

  return 0;
}

UT_TEST(test_prolog_extra_prologue_regs_lr_forces_lr_save_even_leaf)
{
  setup_gen();

  /* Leaf function, but extra_prologue_regs requests LR explicitly (e.g. the
   * static-chain/nested-function path) -- save_lr must become 1 even though
   * leaffunc=1. No callee-saved regs, no stack -> same dummy-R3-pad PUSH
   * shape as the plain non-leaf case above. */
  tcc_gen_machine_prolog(/*leaffunc=*/1, /*used_registers=*/0, /*stack_size=*/0,
                          /*extra_prologue_regs=*/(1u << R_LR));

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xB508);

  return 0;
}

UT_TEST(test_prolog_stack_size_rounds_up_to_8_byte_alignment)
{
  setup_gen();

  /* leaf, no regs, stack_size=4 (not 8-byte aligned) -> prolog defensively
   * rounds up to 8 before allocating. No PUSH (registers_count==0, even);
   * only a single SUB SP,SP,#8. */
  tcc_gen_machine_prolog(/*leaffunc=*/1, /*used_registers=*/0, /*stack_size=*/4,
                          /*extra_prologue_regs=*/0);

  UT_ASSERT_EQ(ind, 2);
  /* SUB SP, SP, #8 -> 0xB080 | (8/4) = 0xB082. */
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xB082);

  return 0;
}

/* ------------------------------------------------------------------ epilog */

UT_TEST(test_epilog_leaf_no_regs_no_stack_emits_bx_lr)
{
  setup_gen();

  tcc_gen_machine_prolog(/*leaffunc=*/1, /*used_registers=*/0, /*stack_size=*/0,
                          /*extra_prologue_regs=*/0);
  int base = ind;

  /* No FP, epilogue_stack_dealloc==0, lr not saved, pushed_registers==0:
   * the "no frame pointer" branch's else-arm just emits BX LR. */
  tcc_gen_machine_epilog(/*leaffunc=*/1);

  UT_ASSERT_EQ(ind - base, 2);
  /* BX LR -> Thumb T1 0x4770 (bx r14: 0x4700 | (r14<<3), r14=14 -> 0x4700 |
   * 0x70 = 0x4770), matching the well-known fixed BX LR encoding used
   * throughout the codebase's leaf-function epilogues. */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + base), 0x4770);

  return 0;
}

UT_TEST(test_epilog_nonleaf_pops_r3_lr_as_pc)
{
  setup_gen();

  /* Mirror of test_prolog_nonleaf_no_regs_no_stack_pushes_lr_padded_with_r3:
   * pushed_registers is left at {r3, lr} by the prolog call. lr_saved is
   * true and there's no FP, so the epilog rewrites the LR bit to PC and
   * pops {r3, pc} directly (no separate BX). */
  tcc_gen_machine_prolog(/*leaffunc=*/0, /*used_registers=*/0, /*stack_size=*/0,
                          /*extra_prologue_regs=*/0);
  int base = ind;

  tcc_gen_machine_epilog(/*leaffunc=*/0);

  UT_ASSERT_EQ(ind - base, 2);
  /* POP {r3, pc}: T1 16-bit encoding 0xBC00 | reglist(r3=0x08) | (1<<8 for
   * pc), per test_pop_t1_with_pc's 0xBD05 for {r0,r2,pc} (pc flag bit 8). */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + base), 0xBD08);

  return 0;
}

UT_TEST(test_epilog_callee_saved_r4_r5_r10_pops_t2)
{
  setup_gen();

  uint64_t used = (1ull << R4) | (1ull << R5) | (1ull << R10);
  tcc_gen_machine_prolog(/*leaffunc=*/1, used, /*stack_size=*/8,
                          /*extra_prologue_regs=*/0);
  int base = ind;

  /* No FP (need_frame_pointer stayed 0 throughout, no va/force paths hit),
   * so this takes the "no frame pointer" epilog branch: SUB SP is undone by
   * an ADD SP,SP,#12 first (epilogue_stack_dealloc==12), then POP {r4, r5,
   * r10} (no LR was pushed, so no PC rewrite) followed by a separate BX LR.
   * Total 2 (ADD SP) + 4 (POP.W, r10 forces T2) + 2 (BX LR) = 8 bytes. */
  tcc_gen_machine_epilog(/*leaffunc=*/1);

  UT_ASSERT_EQ(ind - base, 8);
  /* ADD SP, SP, #12 -> T1 0xB000 | (12/4) = 0xB003 (mirrors the sub-imm
   * oracle: base 0xb000 for ADD (SP plus immediate), imm7 encodes imm/4). */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + base), 0xB003);

  uint32_t reglist = (1u << R4) | (1u << R5) | (1u << R10);
  UT_ASSERT_EQ(read_be_pair32(cur_text_section->data + base + 2), 0xE8BD0000u | reglist);

  /* Trailing BX LR. */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + base + 6), 0x4770);

  return 0;
}

/* ------------------------------------------------------------- noreturn */

UT_TEST(test_finish_noreturn_clears_generating_function_flag)
{
  setup_gen();

  tcc_gen_machine_prolog(/*leaffunc=*/1, /*used_registers=*/0, /*stack_size=*/0,
                          /*extra_prologue_regs=*/0);
  UT_ASSERT_EQ(thumb_gen_state.generating_function, 1);

  tcc_gen_machine_finish_noreturn();

  /* finish_noreturn() is the noreturn-path counterpart of epilog(): it does
   * not emit any register-restoring code (no BX/POP -- the call site is
   * unreachable, e.g. after a __builtin_unreachable()/abort()-like call),
   * it only tears down the per-function bookkeeping (clears the
   * generating_function flag, flushes the literal pool, frees call sites).
   * With no pending literal-pool entries and no call sites recorded, no
   * bytes are emitted here. */
  UT_ASSERT_EQ(thumb_gen_state.generating_function, 0);

  return 0;
}

UT_TEST(test_finish_noreturn_emits_no_pop_or_branch)
{
  setup_gen();

  tcc_gen_machine_prolog(/*leaffunc=*/0, /*used_registers=*/0, /*stack_size=*/0,
                          /*extra_prologue_regs=*/0);
  int base = ind;

  tcc_gen_machine_finish_noreturn();

  /* Unlike tcc_gen_machine_epilog(), finish_noreturn() never pops the
   * pushed {r3, lr} or emits a BX -- confirms it is not just "epilog() with
   * fewer asserts" but a genuinely code-emission-free teardown. */
  UT_ASSERT_EQ(ind, base);

  return 0;
}
