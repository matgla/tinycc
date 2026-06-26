/*
 *  test_opt_licm.c - suite for ir/licm.c (loop-invariant code motion)
 *
 *  tcc_ir_opt_licm() runs the dominance-based LICM in tcc_ir_opt_licm_ex():
 *    1. tcc_ir_cfg_build() splits compact_instructions[] into basic blocks at
 *       jump targets / fall-through-after-jump boundaries.
 *    2. compute dominators, find natural loops via dominance-verified back-edges
 *       (an edge b->h where h dominates b).
 *    3. for each loop with a valid preheader (a unique out-of-loop predecessor
 *       of the header that ALSO dominates the header), mark side-effect-free
 *       arithmetic/assign whose operands are all loop-invariant, and hoist a
 *       CLONE of each such instruction into the preheader, NOP-ing the original.
 *
 *  Return value note: tcc_ir_opt_licm() returns loops->num_loops (the count of
 *  detected loops), NOT the count of hoisted instructions.  So a non-zero return
 *  means "a loop was found", not "the IR was rewritten".  To assert a real hoist
 *  we therefore also check the instruction stream directly: a hoist INSERTS one
 *  instruction at the preheader (next_instruction_index grows by 1) and NOPs the
 *  original in-loop copy.
 *
 *  These are isolated tests: a hand-built IR sequence is run through the bare
 *  pass entry point and the resulting instructions are inspected directly.
 *
 *  Building a loop the pass will actually transform requires a real back-edge:
 *  a preheader block that dominates the header, the header itself (target of a
 *  conditional back-edge), an invariant computation, and a JUMPIF latch whose
 *  dest immediate is the header index.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry point (declared in ir/opt.h / licm.h; forward-declared to avoid
 * pulling in the optimizer engine headers). */
int tcc_ir_opt_licm(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

/* utb_new() leaves iroperand_pool_capacity / compact_instructions_size at 0
 * (it pre-fills the buffers but not the capacity bookkeeping).  LICM hoisting
 * calls tcc_ir_pool_add() and insert_instruction_before(), both of which grow
 * via those fields.  Set them to the real allocated sizes so the existing
 * UTB_MAX_* buffers are used in place (our sequences are tiny, well under the
 * limits, so no reallocation is triggered). */
static TCCIRState *utb_loop_new(void)
{
  TCCIRState *ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;
  ir->compact_instructions_size = UTB_MAX_INSTR;
  return ir;
}

/* Build a JUMP/JUMPIF target operand the way licm/cfg decode it:
 * irop_make_imm32(-1, target, INT32) -> no vreg, imm32 = instruction index. */
static IROperand utb_jtarget(int target)
{
  return irop_make_imm32(-1, target, I32);
}

/* Count NOP instructions in [0, next_instruction_index). */
static int count_nops(TCCIRState *ir)
{
  int n = 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
      n++;
  return n;
}

/* Find the first instruction with the given op whose dest vreg matches `vreg`.
 * Returns its index, or -1. */
static int find_def(TCCIRState *ir, TccIrOp op, int vreg)
{
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != op)
      continue;
    if (!irop_config[op].has_dest)
      continue;
    if (utb_vreg(tcc_ir_op_get_dest(ir, q)) == vreg)
      return i;
  }
  return -1;
}

/* ------------------------------------------------------------------ tests */

/* POSITIVE: a real natural loop with a loop-invariant ADD that must be hoisted.
 *
 *   idx 0: T0 = #100              ; preheader (block 0, dominates header)
 *   idx 1: T1 = T0 + #5           ; loop header (block 1) -- INVARIANT
 *   idx 2: T2 = T2 + #1           ; loop body          -- varying (self def)
 *   idx 3: JUMPIF ->1  (cond T3)  ; latch / conditional back-edge to header
 *   idx 4: RETURNVOID             ; exit (block 2)
 *
 * Block 0 = {0}, block 1 = {1,2,3} (header=1, latch=3), block 2 = {4}.
 * back-edge block1->block1 with header dominating latch => natural loop.
 * preheader = block 0 (unique out-of-loop pred of header, dominates it).
 * T1 = T0 + #5 is invariant (T0 defined outside loop, #5 const, single def),
 * its block (1) dominates the only exit block (1) => SAFE to hoist.
 *
 * Effect: a clone of `T1 = T0 + #5` is inserted at the preheader insert point
 * (index 1), the original is NOP'd, instruction count grows by 1, and the
 * JUMPIF target (was 1) is bumped to 2 by insert_instruction_before. */
UT_TEST(test_licm_hoists_invariant_add)
{
  TCCIRState *ir = utb_loop_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(100, I32), UTB_NONE);  /* 0 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));/* 1 header, invariant */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(2, I32), utb_imm(1, I32));/* 2 varying */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(1), utb_temp(3, I32), UTB_NONE);      /* 3 back-edge */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                /* 4 exit */

  int n_before = ir->next_instruction_index;
  int nops_before = count_nops(ir);

  int loops = tcc_ir_opt_licm(ir);

  /* A loop was detected (return value is the loop count, not the hoist count). */
  UT_ASSERT(loops >= 1);

  /* The hoist inserted exactly one instruction at the preheader. */
  UT_ASSERT_EQ(ir->next_instruction_index, n_before + 1);

  /* ...and NOP'd the original in-loop copy: exactly one new NOP appeared. */
  UT_ASSERT_EQ(count_nops(ir), nops_before + 1);

  /* The hoisted ADD that defines T1 must now live BEFORE the loop header.
   * The original header was at index 1; after inserting one instruction at the
   * preheader (index 1), the hoisted ADD sits at index 1 and the (now-NOP'd)
   * loop body starts at index 2.  The live ADD defining T1 is the hoisted one. */
  int t1_def = find_def(ir, TCCIR_OP_ADD, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 1));
  UT_ASSERT(t1_def >= 0);
  /* It is the hoisted copy at the preheader insert position (index 1), which is
   * before the back-edge JUMPIF (now at index 4). */
  UT_ASSERT_EQ(t1_def, 1);
  UT_ASSERT_EQ(utb_op(ir, t1_def), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_vreg(tcc_ir_op_get_src1(ir, &ir->compact_instructions[t1_def])),
               TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 0));

  /* The back-edge JUMPIF target was rewritten from 1 to 2 (header shifted by
   * the inserted preheader instruction). */
  int jmp_idx = -1;
  for (int i = 0; i < ir->next_instruction_index; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_JUMPIF)
    {
      jmp_idx = i;
      break;
    }
  UT_ASSERT(jmp_idx >= 0);
  {
    IROperand dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[jmp_idx]);
    UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, dest), 2);
  }

  utb_free(ir);
  return 0;
}

/* NEGATIVE (no loop): a straight-line sequence has no back-edge, so no loop is
 * detected, nothing is hoisted, and the IR is left byte-for-byte intact. */
UT_TEST(test_licm_no_loop_no_change)
{
  TCCIRState *ir = utb_loop_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(100, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int n_before = ir->next_instruction_index;
  int nops_before = count_nops(ir);

  int loops = tcc_ir_opt_licm(ir);

  /* No back-edge => no loop detected => return 0, IR unchanged. */
  UT_ASSERT_EQ(loops, 0);
  UT_ASSERT_EQ(ir->next_instruction_index, n_before);
  UT_ASSERT_EQ(count_nops(ir), nops_before);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_ADD);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (loop, but nothing invariant): a loop whose only body computation is
 * a self-referencing accumulator (T1 = T1 + #1) has no hoistable instruction.
 * A loop IS detected (non-zero return), but the IR must NOT grow and no NOP must
 * appear (nothing was hoisted/replaced).
 *
 *   idx 0: T0 = #0          ; preheader (defines accumulator seed... outside loop)
 *   idx 1: T1 = T1 + #1     ; header -- VARYING (dest also a source, self def)
 *   idx 2: JUMPIF ->1 (T2)  ; back-edge
 *   idx 3: RETURNVOID       ; exit
 */
UT_TEST(test_licm_loop_no_invariant_no_hoist)
{
  TCCIRState *ir = utb_loop_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32), UTB_NONE);    /* 0 preheader */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(1, I32), utb_imm(1, I32));/* 1 header varying */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(1), utb_temp(2, I32), UTB_NONE);      /* 2 back-edge */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                /* 3 exit */

  int n_before = ir->next_instruction_index;
  int nops_before = count_nops(ir);

  int loops = tcc_ir_opt_licm(ir);

  /* Loop detected, but nothing invariant -> no instruction inserted, no NOP. */
  UT_ASSERT(loops >= 1);
  UT_ASSERT_EQ(ir->next_instruction_index, n_before);
  UT_ASSERT_EQ(count_nops(ir), nops_before);
  /* The self-referencing accumulator is left in place. */
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, 1)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 1));

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): an instruction whose source is a memory dereference (lvalue)
 * must NOT be hoisted even though it looks invariant, because it may read
 * volatile / changing memory.  Here `T1 = LOAD [T0(lval)]` where T0 is defined
 * outside the loop -- T0 is loop-invariant but the LOAD's src is a deref, so the
 * has_deref guard in dom-LICM blocks the hoist.
 *
 *   idx 0: T0 = #100            ; preheader
 *   idx 1: T1 = LOAD [T0]lval   ; header -- deref source, NOT hoistable
 *   idx 2: T2 = T2 + #1         ; varying
 *   idx 3: JUMPIF ->1 (T3)      ; back-edge
 *   idx 4: RETURNVOID           ; exit
 */
UT_TEST(test_licm_deref_source_not_hoisted)
{
  TCCIRState *ir = utb_loop_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(100, I32), UTB_NONE);   /* 0 */
  /* LOAD dest=T1, src1 = T0 marked as lvalue (deref). */
  IROperand load_src = utb_temp(0, I32);
  load_src.is_lval = 1;
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), load_src, UTB_NONE);              /* 1 deref */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(2, I32), utb_imm(1, I32));/* 2 varying */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(1), utb_temp(3, I32), UTB_NONE);      /* 3 back-edge */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                /* 4 exit */

  int n_before = ir->next_instruction_index;
  int nops_before = count_nops(ir);

  int loops = tcc_ir_opt_licm(ir);

  /* Loop detected, but the LOAD has a deref source -> not hoisted.  LOAD is also
   * not in the hoistable opcode set at all, so doubly guarded. */
  UT_ASSERT(loops >= 1);
  UT_ASSERT_EQ(ir->next_instruction_index, n_before);
  UT_ASSERT_EQ(count_nops(ir), nops_before);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_LOAD);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_licm)
{
  UT_COVERS("licm");
  UT_RUN(test_licm_hoists_invariant_add);
  UT_RUN(test_licm_no_loop_no_change);
  UT_RUN(test_licm_loop_no_invariant_no_hoist);
  UT_RUN(test_licm_deref_source_not_hoisted);
}
