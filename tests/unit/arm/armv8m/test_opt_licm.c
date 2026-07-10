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
 *
 *  Beyond tcc_ir_opt_licm()/tcc_ir_opt_licm_ex() (the dominance-based hoist
 *  entry points above), this suite also covers the rest of licm.h's public
 *  API directly, since tcc_ir_opt_licm() never surfaces its intermediate
 *  results to a caller:
 *    - tcc_ir_detect_loops()/tcc_ir_is_in_loop()/tcc_ir_free_loops(): the
 *      standalone pattern-based (non-dominance) loop detector.
 *    - tcc_ir_estimate_hoist_budget(): the sliding-window register-pressure
 *      estimator that caps per-loop hoist count.
 *    - tcc_ir_cache_func_purity()/tcc_ir_lookup_func_purity(): the TCCState
 *      function-purity cache.
 *    - tcc_ir_get_func_purity(): purity resolution for a call-site symbol
 *      (well-known table / attributes / cache / conservative default).
 *    - tcc_ir_infer_func_purity(): purity inference from a function's own
 *      IR body (stack-only stores/loads, calls, opaque ops, VLA_ALLOC).
 *  tcc_ir_hoist_pure_calls() (re-enabled by default 2026-07-02 after the
 *  ninth defect fix — docs/bugs.md #7, resolved) is exercised end-to-end
 *  via tcc_ir_opt_licm_ex() by test_licm_hoists_const_call_* and
 *  test_licm_no_hoist_pure_call_when_loop_writes_memory below.  The disabled
 *  pattern-based hoist_from_loop()/hoist_const_exprs_from_loop() internals are
 *  still dead (unreachable early-return) and not covered.
 */

#include "ir_build.h"

#include "ut.h"

#include "licm.h"

/* Pass entry point (declared in ir/opt.h / licm.h; forward-declared to avoid
 * pulling in the optimizer engine headers). */
int tcc_ir_opt_licm(TCCIRState *ir);

/* Defined further down with the purity-resolution tests; forward-declared here
 * so the pure-call hoist tests below can build a function Sym. */
static void ut_init_func_sym(Sym *s, int tok);

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

/* ssa:licm — the regalloc-time driver (ssa_opt_licm) over the same engine.
 * Pins that the relocated pass entry still hoists the invariant ADD (one new
 * NOP; the def moves ahead of the header) and reports a change. */
UT_TEST(test_ssa_opt_licm_hoists_invariant)
{
  TCCIRState *ir = utb_loop_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(100, I32), UTB_NONE);   /* 0 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));/* 1 header, invariant */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(2, I32), utb_imm(1, I32));/* 2 varying */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(1), utb_temp(3, I32), UTB_NONE);      /* 3 back-edge */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                /* 4 exit */

  int n_before = ir->next_instruction_index;
  int nops_before = count_nops(ir);

  int changed = ssa_opt_licm(ir);

  UT_ASSERT_EQ(changed, 1);
  UT_ASSERT_EQ(ir->next_instruction_index, n_before + 1);
  UT_ASSERT_EQ(count_nops(ir), nops_before + 1);

  int t1_def = find_def(ir, TCCIR_OP_ADD, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 1));
  UT_ASSERT_EQ(t1_def, 1);

  utb_free(ir);
  return 0;
}

/* POSITIVE (docs/bugs.md #7, re-enabled): a CONST function call in a loop whose
 * argument is loop-invariant is hoisted into the preheader by
 * tcc_ir_hoist_pure_calls (run first inside tcc_ir_opt_licm_ex).  The original
 * call site becomes `T1 = ASSIGN <hoisted temp>`; the surviving FUNCCALLVAL is
 * lifted ahead of the loop header.  ("abs" is CONST in the pure-func table, so
 * the loop's `T2 += 1` store/update does not block it.) */
UT_TEST(test_licm_hoists_const_call_with_invariant_arg)
{
  static Sym fn;
  ut_init_func_sym(&fn, TOK_IDENT + 40);
  utb_set_tok_str(fn.v, "abs"); /* abs is CONST in pure_func_table */

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);                          /* needed for the callee SYMREF */
  ir->compact_instructions_size = UTB_MAX_INSTR;
  ir->next_temporary_variable = 10;            /* room for the hoister's fresh temp */
  ir->next_call_id = 2;

  IROperand callee = utb_symref(ir, &fn, 0, 0, 0, I32);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(42, I32), UTB_NONE);      /* 0 preheader */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));                        /* 1 header */
  utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, I32), callee,
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));                         /* 2 call */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(2, I32), utb_imm(1, I32));  /* 3 varying */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(1), utb_temp(3, I32), UTB_NONE);        /* 4 back-edge */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                  /* 5 exit */

  int n_before = ir->next_instruction_index;
  int loops = tcc_ir_opt_licm(ir);
  UT_ASSERT(loops >= 1);
  UT_ASSERT(ir->next_instruction_index > n_before);

  int t1_assign = find_def(ir, TCCIR_OP_ASSIGN, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 1));
  UT_ASSERT(t1_assign >= 0);

  int num_calls = 0, call_idx = -1, jmp_idx = -1;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_FUNCCALLVAL) { num_calls++; call_idx = i; }
    if (ir->compact_instructions[i].op == TCCIR_OP_JUMPIF && jmp_idx < 0) jmp_idx = i;
  }
  UT_ASSERT_EQ(num_calls, 1);
  UT_ASSERT(jmp_idx >= 0);
  int header = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, &ir->compact_instructions[jmp_idx]));
  UT_ASSERT(call_idx < header); /* call lifted ahead of the loop header */

  utb_free(ir);
  return 0;
}

/* NEGATIVE (docs/bugs.md #7 / PR20100): a merely-PURE function (reads memory)
 * must NOT be hoisted when the loop modifies memory it could read.  Here the
 * loop contains a STORE, so the PURE call stays in the loop body (its
 * FUNCCALLVAL remains after the loop header). */
UT_TEST(test_licm_no_hoist_pure_call_when_loop_writes_memory)
{
  static Sym fn;
  ut_init_func_sym(&fn, TOK_IDENT + 41);
  utb_set_tok_str(fn.v, "some_pure_reader");
  fn.f.func_pure = 1; /* PURE (reads memory), not CONST */

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  ir->next_temporary_variable = 10;
  ir->next_call_id = 2;

  IROperand callee = utb_symref(ir, &fn, 0, 0, 0, I32);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32), UTB_NONE);       /* 0 preheader */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVOID, UTB_NONE, UTB_NONE,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));                        /* 1 header: void arg marker */
  utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, I32), callee,
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));                         /* 2 PURE call, no args */
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_stackoff(100, 1, 0, 0, I32)),
           utb_temp(1, I32), UTB_NONE);                                            /* 3 STORE in loop */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(1), utb_temp(3, I32), UTB_NONE);        /* 4 back-edge */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                  /* 5 exit */

  int loops = tcc_ir_opt_licm(ir);
  UT_ASSERT(loops >= 1);

  /* The PURE call must remain inside the loop (after the header), not hoisted. */
  int call_idx = -1, jmp_idx = -1;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_FUNCCALLVAL && call_idx < 0) call_idx = i;
    if (ir->compact_instructions[i].op == TCCIR_OP_JUMPIF && jmp_idx < 0) jmp_idx = i;
  }
  UT_ASSERT(call_idx >= 0);
  UT_ASSERT(jmp_idx >= 0);
  int header = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, &ir->compact_instructions[jmp_idx]));
  UT_ASSERT(call_idx >= header); /* NOT hoisted: call is still in the loop body */

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

/* ------------------------------------------------------------------ tests */

/* NEGATIVE (safety): an instruction that uses a vreg defined inside the loop
 * must NOT be hoisted, even if the other operand is a constant.  Here T1 is a
 * self-referencing accumulator in the outer header; T2 = T1 + #5 depends on it
 * and therefore stays in the loop body.
 *
 *   idx 0: T0 = #0
 *   idx 1: T1 = T1 + #1     ; header -- varying (self def)
 *   idx 2: T2 = T1 + #5     ; body   -- NOT invariant (T1 defined in loop)
 *   idx 3: JUMPIF ->1
 *   idx 4: RETURNVOID
 */
UT_TEST(test_licm_in_loop_def_blocks_hoist)
{
  TCCIRState *ir = utb_loop_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32), UTB_NONE);    /* 0 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(1, I32), utb_imm(1, I32));/* 1 varying */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32), utb_imm(5, I32));/* 2 not invariant */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(1), utb_temp(3, I32), UTB_NONE);      /* 3 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                /* 4 */

  int n_before = ir->next_instruction_index;
  int nops_before = count_nops(ir);

  int loops = tcc_ir_opt_licm(ir);

  UT_ASSERT(loops >= 1);
  UT_ASSERT_EQ(ir->next_instruction_index, n_before);
  UT_ASSERT_EQ(count_nops(ir), nops_before);
  /* The dependent ADD is left untouched at index 2. */
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, 2)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 2));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, 2)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 1));

  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (side effects): a STORE must never be hoisted, even when both the
 * address and the stored value are loop-invariant.  STORE is not in the
 * hoistable opcode set and has an observable memory effect.
 *
 *   idx 0: T0 = #100
 *   idx 1: T1 = #200
 *   idx 2: STORE [T0]lval <- T1   ; header
 *   idx 3: T2 = T2 + #1
 *   idx 4: JUMPIF ->2
 *   idx 5: RETURNVOID
 */
UT_TEST(test_licm_store_not_hoisted)
{
  TCCIRState *ir = utb_loop_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(100, I32), UTB_NONE);   /* 0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(200, I32), UTB_NONE);   /* 1 */
  /* STORE dest is the address operand marked as lvalue. */
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_temp(1, I32), UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(2, I32), utb_imm(1, I32));/* 3 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(2), utb_temp(3, I32), UTB_NONE);      /* 4 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                /* 5 */

  int n_before = ir->next_instruction_index;
  int nops_before = count_nops(ir);

  int loops = tcc_ir_opt_licm(ir);

  UT_ASSERT(loops >= 1);
  UT_ASSERT_EQ(ir->next_instruction_index, n_before);
  UT_ASSERT_EQ(count_nops(ir), nops_before);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_STORE);

  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: LEA of a loop-invariant stack address is in the hoistable opcode
 * set and should be moved to the preheader.
 *
 *   idx 0: T0 = LEA Addr[StackLoc[-4]]  ; preheader
 *   idx 1: T1 = LEA T0                  ; header -- invariant
 *   idx 2: T2 = T2 + #1
 *   idx 3: JUMPIF ->1
 *   idx 4: RETURNVOID
 */
UT_TEST(test_licm_lea_stack_addr_hoisted)
{
  TCCIRState *ir = utb_loop_new();

  utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_stackoff(-4, 0, 0, 0, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);        /* 1 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(2, I32), utb_imm(1, I32));/* 2 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(1), utb_temp(3, I32), UTB_NONE);      /* 3 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                /* 4 */

  int n_before = ir->next_instruction_index;

  int loops = tcc_ir_opt_licm(ir);

  UT_ASSERT(loops >= 1);
  UT_ASSERT_EQ(ir->next_instruction_index, n_before + 1);
  UT_ASSERT_EQ(count_nops(ir), 1);

  int t1_def = find_def(ir, TCCIR_OP_LEA, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 1));
  UT_ASSERT_EQ(t1_def, 1);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, t1_def)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 0));

  /* The back-edge target was bumped past the inserted preheader instruction. */
  int jmp_idx = -1;
  for (int i = 0; i < ir->next_instruction_index; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_JUMPIF)
    {
      jmp_idx = i;
      break;
    }
  UT_ASSERT(jmp_idx >= 0);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, &ir->compact_instructions[jmp_idx])), 2);

  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (no preheader): when the loop header is at function entry there is
 * no out-of-loop predecessor to hoist into.  A loop is detected but no
 * instruction may be inserted.
 *
 *   idx 0: T1 = T0 + #5     ; would be invariant, but header is the entry
 *   idx 1: T2 = T2 + #1
 *   idx 2: JUMPIF ->0
 *   idx 3: RETURNVOID
 */
UT_TEST(test_licm_header_at_entry_no_hoist)
{
  TCCIRState *ir = utb_loop_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));/* 0 header */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(2, I32), utb_imm(1, I32));/* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(0), utb_temp(3, I32), UTB_NONE);      /* 2 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                /* 3 */

  int n_before = ir->next_instruction_index;
  int nops_before = count_nops(ir);

  int loops = tcc_ir_opt_licm(ir);

  UT_ASSERT(loops >= 1);
  UT_ASSERT_EQ(ir->next_instruction_index, n_before);
  UT_ASSERT_EQ(count_nops(ir), nops_before);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_ADD);

  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (merge preheader): when the loop header has more than one
 * out-of-loop predecessor, no single preheader dominates every entry path, so
 * invariant code must stay in the loop.
 *
 *   idx 0: T0 = #0
 *   idx 1: JUMPIF ->4 (T1)   ; branch over alternate entry
 *   idx 2: T2 = #100
 *   idx 3: JMP 4
 *   idx 4: T3 = T2 + #5      ; header
 *   idx 5: T4 = T4 + #1
 *   idx 6: JUMPIF ->4 (T5)
 *   idx 7: RETURNVOID
 */
UT_TEST(test_licm_merge_preheader_no_hoist)
{
  TCCIRState *ir = utb_loop_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32), UTB_NONE);    /* 0 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(4), utb_temp(1, I32), UTB_NONE);     /* 1 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(100, I32), UTB_NONE);  /* 2 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_jtarget(4), UTB_NONE, UTB_NONE);               /* 3 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(2, I32), utb_imm(5, I32));/* 4 header */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(4, I32), utb_temp(4, I32), utb_imm(1, I32));/* 5 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(4), utb_temp(5, I32), UTB_NONE);     /* 6 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);               /* 7 */

  int n_before = ir->next_instruction_index;
  int nops_before = count_nops(ir);

  int loops = tcc_ir_opt_licm(ir);

  UT_ASSERT(loops >= 1);
  UT_ASSERT_EQ(ir->next_instruction_index, n_before);
  UT_ASSERT_EQ(count_nops(ir), nops_before);
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_ADD);

  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE / ROBUSTNESS: a loop with two distinct back-edges to the same header
 * is detected as one natural loop and the invariant is hoisted exactly once.
 * Both jump targets are updated consistently.
 *
 *   idx 0: T0 = #100
 *   idx 1: T1 = T0 + #5     ; header
 *   idx 2: T2 = T2 + #1
 *   idx 3: JUMPIF ->1
 *   idx 4: JMP 1
 *   idx 5: RETURNVOID
 */
UT_TEST(test_licm_multiple_back_edges_hoisted_once)
{
  TCCIRState *ir = utb_loop_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(100, I32), UTB_NONE);   /* 0 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));/* 1 header */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(2, I32), utb_imm(1, I32));/* 2 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(1), utb_temp(3, I32), UTB_NONE);      /* 3 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_jtarget(1), UTB_NONE, UTB_NONE);                /* 4 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                /* 5 */

  int n_before = ir->next_instruction_index;

  int loops = tcc_ir_opt_licm(ir);

  UT_ASSERT(loops >= 1);
  UT_ASSERT_EQ(ir->next_instruction_index, n_before + 1);
  UT_ASSERT_EQ(count_nops(ir), 1);

  int t1_def = find_def(ir, TCCIR_OP_ADD, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 1));
  UT_ASSERT_EQ(t1_def, 1);

  /* Both jumps to the header now target the post-insertion header index. */
  int branch_targets[2] = {-1, -1};
  int found = 0;
  for (int i = 0; i < ir->next_instruction_index && found < 2; i++)
  {
    TccIrOp op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_JUMP || op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[i]);
      branch_targets[found++] = (int)irop_get_imm64_ex(ir, dest);
    }
  }
  UT_ASSERT_EQ(found, 2);
  UT_ASSERT_EQ(branch_targets[0], 2);
  UT_ASSERT_EQ(branch_targets[1], 2);

  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ★ SEMI-ORACLE: nested loops.  An invariant that is only invariant in the
 * inner loop must be hoisted to the inner preheader (inside the outer loop),
 * while an invariant in the outer loop must be hoisted to the outer
 * preheader.
 *
 *   idx 0: T0 = #100
 *   idx 1: T5 = #0          ; outer accumulator seed
 *   idx 2: T1 = T0 + #5     ; outer header -- invariant in outer
 *   idx 3: T2 = T5 + #1     ; outer body  -- varies in outer (depends on T5)
 *   idx 4: T3 = T2 + #7     ; inner header -- invariant in inner only
 *   idx 5: T4 = T4 + #1
 *   idx 6: JUMPIF ->4
 *   idx 7: T5 = T5 + #1
 *   idx 8: JUMPIF ->2
 *   idx 9: RETURNVOID
 */
UT_TEST(test_licm_nested_loop_hoists_to_right_preheader)
{
  TCCIRState *ir = utb_loop_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(100, I32), UTB_NONE);   /* 0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(5, I32), utb_imm(0, I32), UTB_NONE);     /* 1 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));/* 2 outer header */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(5, I32), utb_imm(1, I32));/* 3 outer body */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(2, I32), utb_imm(7, I32));/* 4 inner header */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(4, I32), utb_temp(4, I32), utb_imm(1, I32));/* 5 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(4), utb_temp(6, I32), UTB_NONE);      /* 6 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(5, I32), utb_temp(5, I32), utb_imm(1, I32));/* 7 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(2), utb_temp(8, I32), UTB_NONE);      /* 8 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                /* 9 */

  int n_before = ir->next_instruction_index;

  int loops = tcc_ir_opt_licm(ir);

  UT_ASSERT(loops >= 1);
  UT_ASSERT_EQ(ir->next_instruction_index, n_before + 2);
  UT_ASSERT_EQ(count_nops(ir), 2);

  /* T1 (invariant in outer loop) lands in the outer preheader. */
  int t1_def = find_def(ir, TCCIR_OP_ADD, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 1));
  UT_ASSERT(t1_def >= 0);
  UT_ASSERT_EQ(t1_def, 2);

  /* T3 (invariant in inner loop only) lands in the inner preheader, which is
   * strictly after the outer preheader and before the remaining inner body. */
  int t3_def = find_def(ir, TCCIR_OP_ADD, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 3));
  UT_ASSERT(t3_def >= 0);
  UT_ASSERT_EQ(t3_def, 5);

  /* The outer-body computation that is not invariant must stay put. */
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, 4)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 2));

  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* IDEMPOTENCE: after the first LICM run the IR is already transformed; a
 * second run must not insert additional instructions or create new NOPs.  The
 * pass returns the loop count both times, not zero, because its return value is
 * the number of detected loops rather than the number of changes. */
UT_TEST(test_licm_idempotent_no_new_hoists)
{
  TCCIRState *ir = utb_loop_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(100, I32), UTB_NONE);   /* 0 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));/* 1 header */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(2, I32), utb_imm(1, I32));/* 2 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(1), utb_temp(3, I32), UTB_NONE);      /* 3 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                /* 4 */

  int loops1 = tcc_ir_opt_licm(ir);
  int n_after_first = ir->next_instruction_index;
  int nops_after_first = count_nops(ir);

  UT_ASSERT(loops1 >= 1);
  UT_ASSERT_EQ(n_after_first, 6);
  UT_ASSERT_EQ(nops_after_first, 1);

  int loops2 = tcc_ir_opt_licm(ir);
  UT_ASSERT_EQ(loops2, loops1);
  UT_ASSERT_EQ(ir->next_instruction_index, n_after_first);
  UT_ASSERT_EQ(count_nops(ir), nops_after_first);

  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (side effects / UB risk): DIV is not in the hoistable opcode set,
 * so an invariant division must stay in the loop even though both operands are
 * loop-invariant.  This also guards the "div-by-maybe-0" corner: hoisting a
 * division that turns out to trap would change observable behavior.
 *
 *   idx 0: T0 = #100
 *   idx 1: T1 = #7
 *   idx 2: T2 = T0 / T1     ; header
 *   idx 3: T3 = T3 + #1
 *   idx 4: JUMPIF ->2
 *   idx 5: RETURNVOID
 */
UT_TEST(test_licm_div_not_hoisted)
{
  TCCIRState *ir = utb_loop_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(100, I32), UTB_NONE);   /* 0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(7, I32), UTB_NONE);     /* 1 */
  utb_emit(ir, TCCIR_OP_DIV, utb_temp(2, I32), utb_temp(0, I32), utb_temp(1, I32));/* 2 header */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(3, I32), utb_imm(1, I32));/* 3 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(2), utb_temp(4, I32), UTB_NONE);      /* 4 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                /* 5 */

  int n_before = ir->next_instruction_index;
  int nops_before = count_nops(ir);

  int loops = tcc_ir_opt_licm(ir);

  UT_ASSERT(loops >= 1);
  UT_ASSERT_EQ(ir->next_instruction_index, n_before);
  UT_ASSERT_EQ(count_nops(ir), nops_before);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_DIV);

  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ==================================================================
 * tcc_ir_detect_loops / tcc_ir_is_in_loop / tcc_ir_free_loops
 *
 * These are the standalone pattern-based loop-detection primitives that sit
 * underneath tcc_ir_opt_licm_ex() (which uses its own CFG/dominator-based
 * detector internally and only calls tcc_ir_detect_loops() again at the very
 * end to refresh indices for callers).  They are public API (licm.h) and are
 * exercised directly here since tcc_ir_opt_licm() never surfaces the IRLoops*
 * it computes internally.
 * ================================================================== */

/* POSITIVE: a single backward JUMP creates exactly one detected loop whose
 * header/preheader/body fields match the simple pattern-based rule (no
 * dominance check here -- that lives in tcc_ir_opt_licm_ex, not in
 * tcc_ir_detect_loops itself):
 *   header_idx  = jump target
 *   preheader_idx = nearest non-jump instruction walking back from header
 *   body_instrs = [target .. jump_idx] inclusive
 *
 *   idx 0: T0 = #100         ; preheader
 *   idx 1: T1 = T0 + #5      ; header
 *   idx 2: T2 = T2 + #1
 *   idx 3: JUMP ->1          ; backward jump: target(1) < i(3)
 *   idx 4: RETURNVOID
 */
UT_TEST(test_detect_loops_finds_single_backward_jump)
{
  TCCIRState *ir = utb_loop_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(100, I32), UTB_NONE);   /* 0 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));/* 1 header */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(2, I32), utb_imm(1, I32));/* 2 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_jtarget(1), UTB_NONE, UTB_NONE);                /* 3 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                /* 4 */

  IRLoops *loops = tcc_ir_detect_loops(ir);
  UT_ASSERT(loops != NULL);
  UT_ASSERT_EQ(loops->num_loops, 1);
  UT_ASSERT_EQ(loops->loops[0].header_idx, 1);
  UT_ASSERT_EQ(loops->loops[0].start_idx, 1);
  UT_ASSERT_EQ(loops->loops[0].end_idx, 3);
  UT_ASSERT_EQ(loops->loops[0].preheader_idx, 0);
  UT_ASSERT_EQ(loops->loops[0].num_body_instrs, 3);
  UT_ASSERT_EQ(loops->loops[0].body_instrs[0], 1);
  UT_ASSERT_EQ(loops->loops[0].body_instrs[1], 2);
  UT_ASSERT_EQ(loops->loops[0].body_instrs[2], 3);
  UT_ASSERT_EQ(loops->loops[0].depth, 1);

  /* tcc_ir_is_in_loop: instructions inside the body vs. outside it. */
  UT_ASSERT_EQ(tcc_ir_is_in_loop(&loops->loops[0], 1), 1);
  UT_ASSERT_EQ(tcc_ir_is_in_loop(&loops->loops[0], 2), 1);
  UT_ASSERT_EQ(tcc_ir_is_in_loop(&loops->loops[0], 3), 1);
  UT_ASSERT_EQ(tcc_ir_is_in_loop(&loops->loops[0], 0), 0);  /* preheader is not in body */
  UT_ASSERT_EQ(tcc_ir_is_in_loop(&loops->loops[0], 4), 0);  /* exit is not in body */
  UT_ASSERT_EQ(tcc_ir_is_in_loop(NULL, 1), 0);              /* NULL loop -> 0, no crash */

  tcc_ir_free_loops(loops);
  utb_free(ir);
  return 0;
}

/* NEGATIVE: a straight-line sequence with no backward JUMP/JUMPIF still
 * returns a valid (non-NULL) IRLoops* with num_loops == 0 -- tcc_ir_detect_loops
 * only returns NULL for a NULL/empty `ir` (see the guard at the top of the
 * function in ir/licm.c). It's the *caller* (tcc_ir_opt_licm_ex) that treats
 * "!loops || loops->num_loops == 0" as the "no loops" signal, not
 * tcc_ir_detect_loops itself. */
UT_TEST(test_detect_loops_no_backward_jump_returns_null)
{
  TCCIRState *ir = utb_loop_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(100, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  IRLoops *loops = tcc_ir_detect_loops(ir);
  UT_ASSERT(loops != NULL);
  UT_ASSERT_EQ(loops->num_loops, 0);

  tcc_ir_free_loops(loops);
  /* Freeing a NULL IRLoops* must also be a safe no-op. */
  tcc_ir_free_loops(NULL);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): tcc_ir_detect_loops on an IR with zero instructions
 * returns NULL immediately (next_instruction_index == 0 guard). */
UT_TEST(test_detect_loops_empty_ir_returns_null)
{
  TCCIRState *ir = utb_loop_new();
  UT_ASSERT_EQ(ir->next_instruction_index, 0);

  IRLoops *loops = tcc_ir_detect_loops(ir);
  UT_ASSERT(loops == NULL);

  utb_free(ir);
  return 0;
}

/* POSITIVE (switch-break filtering): tcc_ir_detect_loops discards a "loop"
 * that is a strict subset of another loop sharing the same header -- the
 * documented switch-break artifact filter (licm.c's "Filter out spurious
 * loops" pass).  Two backward jumps target the same header; the shorter one
 * (a JUMP whose source is earlier) is a subset of the longer one and must be
 * dropped, leaving exactly one loop -- the larger range.
 *
 *   idx 0: T0 = #0             ; preheader
 *   idx 1: T1 = T1 + #1        ; header
 *   idx 2: JUMP ->1             ; inner/shorter back-edge (subset, end=2)
 *   idx 3: T2 = T2 + #1
 *   idx 4: JUMP ->1             ; outer/longer back-edge (end=4)
 *   idx 5: RETURNVOID
 */
UT_TEST(test_detect_loops_filters_switch_break_subset)
{
  TCCIRState *ir = utb_loop_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32), UTB_NONE);     /* 0 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(1, I32), utb_imm(1, I32));/* 1 header */
  utb_emit(ir, TCCIR_OP_JUMP, utb_jtarget(1), UTB_NONE, UTB_NONE);                /* 2 subset back-edge */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(2, I32), utb_imm(1, I32));/* 3 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_jtarget(1), UTB_NONE, UTB_NONE);                /* 4 outer back-edge */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                /* 5 */

  IRLoops *loops = tcc_ir_detect_loops(ir);
  UT_ASSERT(loops != NULL);
  /* Only the larger-range loop (header=1, end=4) survives; the header=1,
   * end=2 subset was filtered out. */
  UT_ASSERT_EQ(loops->num_loops, 1);
  UT_ASSERT_EQ(loops->loops[0].header_idx, 1);
  UT_ASSERT_EQ(loops->loops[0].end_idx, 4);

  tcc_ir_free_loops(loops);
  utb_free(ir);
  return 0;
}

/* ==================================================================
 * tcc_ir_estimate_hoist_budget
 *
 * Sliding-window register-pressure estimator used by tcc_ir_opt_licm_ex to
 * cap how many values get hoisted into the preheader per loop, so hoisting
 * doesn't starve the loop body of registers.  budget = total_regs -
 * num_params - max_pressure, floored at 1; max_pressure is floored at 3.
 *
 * tcc_ir_vreg_is_valid() (consulted per operand) requires a real, non-zero
 * temporary_variables_live_intervals_size -- utb_loop_new() leaves it 0, which
 * would make every TEMP vreg reference invalid and silently zero out the
 * pressure count.  utb_budget_new() gives it real backing storage, mirroring
 * test_opt_copyprop.c's utb_new_sym() pattern. */
static TCCIRState *utb_budget_new(void)
{
  TCCIRState *ir = utb_loop_new();
  ir->temporary_variables_live_intervals_size = 64;
  ir->temporary_variables_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * 64);
  return ir;
}

/* A loop body with very few distinct vregs (well under the pressure floor of
 * 3) yields budget = total_regs - num_params - 3 exactly, when that is >= 1. */
UT_TEST(test_hoist_budget_low_pressure_floor_of_three)
{
  TCCIRState *ir = utb_budget_new();
  tcc_state->registers_for_allocator = 11;

  /* Single instruction referencing 2 distinct vregs (T0 dest, T0 src1 -- same
   * vreg counted once) + one immediate: well under the pressure floor. */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_temp(0, I32), utb_imm(1, I32)); /* 0 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 1 */

  int budget = tcc_ir_estimate_hoist_budget(ir, 0, 0, /*num_params=*/0);
  /* max_pressure floors at 3 even though only 1 distinct vreg is referenced. */
  UT_ASSERT_EQ(budget, 11 - 0 - 3);

  utb_free(ir);
  return 0;
}

/* More function parameters directly reduce the budget by the same amount
 * (budget = total_regs - num_params - max_pressure), all else equal. */
UT_TEST(test_hoist_budget_shrinks_with_more_params)
{
  TCCIRState *ir = utb_budget_new();
  tcc_state->registers_for_allocator = 11;

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_temp(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int budget_0p = tcc_ir_estimate_hoist_budget(ir, 0, 0, 0);
  int budget_4p = tcc_ir_estimate_hoist_budget(ir, 0, 0, 4);
  UT_ASSERT_EQ(budget_0p - budget_4p, 4);

  utb_free(ir);
  return 0;
}

/* A window with many distinct vregs raises max_pressure above the floor of
 * 3, so the budget for a high-pressure body is strictly smaller than for a
 * low-pressure one with the same register count/params. */
UT_TEST(test_hoist_budget_shrinks_with_more_distinct_vregs)
{
  TCCIRState *ir = utb_budget_new();
  tcc_state->registers_for_allocator = 11;

  /* 6 distinct TEMP vregs (T0..T5) all referenced within one WINDOW_SIZE(8)
   * window of non-NOP instructions -> max_pressure = 6, above the floor. */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_temp(1, I32), utb_temp(2, I32)); /* 0 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(4, I32), utb_temp(5, I32)); /* 1 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                  /* 2 */

  int budget_highpressure = tcc_ir_estimate_hoist_budget(ir, 0, 1, 0);
  UT_ASSERT_EQ(budget_highpressure, 11 - 0 - 6);
  UT_ASSERT(budget_highpressure < 11 - 0 - 3);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard/floor): when total_regs - num_params - max_pressure would
 * go non-positive, the budget floors at 1 (never 0 or negative -- the
 * caller always gets to hoist at least one value). */
UT_TEST(test_hoist_budget_floors_at_one)
{
  TCCIRState *ir = utb_budget_new();
  tcc_state->registers_for_allocator = 4;

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_temp(1, I32), utb_temp(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  /* total_regs(4) - num_params(20) - max_pressure(>=3) is deeply negative. */
  int budget = tcc_ir_estimate_hoist_budget(ir, 0, 0, 20);
  UT_ASSERT_EQ(budget, 1);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (default register count): when tcc_state->registers_for_allocator
 * is <= 0 (unset), the estimator falls back to 11 total regs. */
UT_TEST(test_hoist_budget_defaults_total_regs_when_unset)
{
  TCCIRState *ir = utb_budget_new();
  tcc_state->registers_for_allocator = 0;

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_temp(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int budget = tcc_ir_estimate_hoist_budget(ir, 0, 0, 0);
  UT_ASSERT_EQ(budget, 11 - 0 - 3);

  tcc_state->registers_for_allocator = 11; /* restore for subsequent tests */
  utb_free(ir);
  return 0;
}

/* ==================================================================
 * tcc_ir_cache_func_purity / tcc_ir_lookup_func_purity
 *
 * A small linear cache on TCCState keyed by function token, consulted by
 * tcc_ir_get_func_purity() before falling back to the conservative IMPURE
 * default.  Uses the shared tcc_state global (tcc_state_stub.c); tests reset
 * func_purity_cache_count first since the storage persists across the whole
 * UT binary run.
 * ================================================================== */

#define UT_TOK_A (TOK_IDENT + 100)
#define UT_TOK_B (TOK_IDENT + 101)

/* POSITIVE: a cached token round-trips through lookup. */
UT_TEST(test_purity_cache_add_then_lookup_roundtrips)
{
  tcc_state->func_purity_cache_count = 0;

  tcc_ir_cache_func_purity(tcc_state, UT_TOK_A, TCC_FUNC_PURITY_CONST);
  int got = tcc_ir_lookup_func_purity(tcc_state, UT_TOK_A);
  UT_ASSERT_EQ(got, (int)TCC_FUNC_PURITY_CONST);

  tcc_state->func_purity_cache_count = 0;
  return 0;
}

/* NEGATIVE: a token that was never cached returns -1 (not found sentinel). */
UT_TEST(test_purity_cache_lookup_miss_returns_minus_one)
{
  tcc_state->func_purity_cache_count = 0;

  int got = tcc_ir_lookup_func_purity(tcc_state, UT_TOK_B);
  UT_ASSERT_EQ(got, -1);

  return 0;
}

/* NEGATIVE (guard): caching the same token twice keeps the FIRST value --
 * tcc_ir_cache_func_purity's "already cached" scan returns early without
 * overwriting. */
UT_TEST(test_purity_cache_duplicate_token_keeps_first_value)
{
  tcc_state->func_purity_cache_count = 0;

  tcc_ir_cache_func_purity(tcc_state, UT_TOK_A, TCC_FUNC_PURITY_CONST);
  tcc_ir_cache_func_purity(tcc_state, UT_TOK_A, TCC_FUNC_PURITY_IMPURE);

  UT_ASSERT_EQ(tcc_state->func_purity_cache_count, 1);
  UT_ASSERT_EQ(tcc_ir_lookup_func_purity(tcc_state, UT_TOK_A), (int)TCC_FUNC_PURITY_CONST);

  tcc_state->func_purity_cache_count = 0;
  return 0;
}

/* NEGATIVE (guard): a token below TOK_IDENT (not a real identifier token) is
 * silently rejected by both cache and lookup -- neither crashes nor caches
 * garbage. */
UT_TEST(test_purity_cache_rejects_token_below_tok_ident)
{
  tcc_state->func_purity_cache_count = 0;

  tcc_ir_cache_func_purity(tcc_state, 5 /* < TOK_IDENT */, TCC_FUNC_PURITY_CONST);
  UT_ASSERT_EQ(tcc_state->func_purity_cache_count, 0);
  UT_ASSERT_EQ(tcc_ir_lookup_func_purity(tcc_state, 5), -1);
  UT_ASSERT_EQ(tcc_ir_lookup_func_purity(NULL, UT_TOK_A), -1);
  UT_ASSERT_EQ(tcc_ir_lookup_func_purity(tcc_state, -1), -1);

  return 0;
}

#undef UT_TOK_A
#undef UT_TOK_B

/* ==================================================================
 * tcc_ir_get_func_purity
 *
 * Resolution order: not-a-function -> IMPURE; well-known table name match;
 * func_noreturn attr -> IMPURE; func_const attr -> CONST; func_pure attr ->
 * PURE; purity cache; conservative IMPURE default.
 * ================================================================== */

/* Build a minimal function Sym with token `tok` and the given FuncAttr bits
 * (via a caller-supplied lambda-like setup is overkill here -- callers set
 * fields directly after this helper zero-inits and marks it VT_FUNC). */
static void ut_init_func_sym(Sym *s, int tok)
{
  memset(s, 0, sizeof(*s));
  s->v = tok;
  s->type.t = VT_FUNC;
}

/* NEGATIVE (guard): NULL symbol -> UNKNOWN (not a crash). */
UT_TEST(test_get_func_purity_null_sym_is_unknown)
{
  UT_ASSERT_EQ(tcc_ir_get_func_purity(NULL, NULL), (int)TCC_FUNC_PURITY_UNKNOWN);
  return 0;
}

/* NEGATIVE (guard): a symbol whose type is not VT_FUNC is never pure.
 * Regression lock for bugs.md #8 (fixed): tcc_ir_get_func_purity now masks
 * VT_BTYPE first (`(t & VT_BTYPE) == VT_FUNC`). Before the fix its guard was
 * the raw `sym->type.t & VT_FUNC`, and VT_FUNC==6 shares set bits with other
 * basic types (VT_INT==3, 3 & 6 == 2 != 0), so a VT_INT symbol wrongly passed
 * the guard and fell through to the purity lookup. VT_INT below now exercises
 * the corrected guard directly (it would have returned non-IMPURE before). */
UT_TEST(test_get_func_purity_non_function_sym_is_impure)
{
  static Sym s;
  memset(&s, 0, sizeof(s));
  s.v = TOK_IDENT + 1;
  utb_set_tok_str(s.v, "not_a_function");

  /* VT_VOID (0): zero bitwise-AND with VT_FUNC — impure under old and new. */
  s.type.t = VT_VOID;
  UT_ASSERT_EQ(tcc_ir_get_func_purity(NULL, &s), (int)TCC_FUNC_PURITY_IMPURE);

  /* VT_INT (3): 3 & 6 == 2 (non-zero) wrongly passed the old raw guard; the
   * VT_BTYPE mask now correctly classifies it as a non-function -> IMPURE. */
  s.type.t = VT_INT;
  UT_ASSERT_EQ(tcc_ir_get_func_purity(NULL, &s), (int)TCC_FUNC_PURITY_IMPURE);
  return 0;
}

/* POSITIVE: a name matching the well-known pure-function table (e.g.
 * "strlen") returns that table's purity level regardless of attributes. */
UT_TEST(test_get_func_purity_well_known_table_hit)
{
  static Sym s;
  ut_init_func_sym(&s, TOK_IDENT + 2);
  utb_set_tok_str(s.v, "strlen");

  /* strlen is PURE (purity level 2) in pure_func_table. */
  UT_ASSERT_EQ(tcc_ir_get_func_purity(NULL, &s), (int)TCC_FUNC_PURITY_PURE);
  return 0;
}

/* POSITIVE: a CONST-level well-known name (e.g. "abs") returns CONST. */
UT_TEST(test_get_func_purity_well_known_table_const_hit)
{
  static Sym s;
  ut_init_func_sym(&s, TOK_IDENT + 3);
  utb_set_tok_str(s.v, "abs");

  UT_ASSERT_EQ(tcc_ir_get_func_purity(NULL, &s), (int)TCC_FUNC_PURITY_CONST);
  return 0;
}

/* NEGATIVE: func_noreturn overrides everything else (checked before
 * func_const/func_pure) -> IMPURE even for an otherwise-unknown name. */
UT_TEST(test_get_func_purity_noreturn_attr_is_impure)
{
  static Sym s;
  ut_init_func_sym(&s, TOK_IDENT + 4);
  utb_set_tok_str(s.v, "some_noreturn_fn");
  s.f.func_noreturn = 1;
  s.f.func_const = 1; /* would otherwise be CONST -- noreturn wins */

  UT_ASSERT_EQ(tcc_ir_get_func_purity(NULL, &s), (int)TCC_FUNC_PURITY_IMPURE);
  return 0;
}

/* POSITIVE: explicit __attribute__((const)) -> CONST. */
UT_TEST(test_get_func_purity_const_attr)
{
  static Sym s;
  ut_init_func_sym(&s, TOK_IDENT + 5);
  utb_set_tok_str(s.v, "some_const_fn");
  s.f.func_const = 1;

  UT_ASSERT_EQ(tcc_ir_get_func_purity(NULL, &s), (int)TCC_FUNC_PURITY_CONST);
  return 0;
}

/* POSITIVE: explicit __attribute__((pure)) -> PURE. */
UT_TEST(test_get_func_purity_pure_attr)
{
  static Sym s;
  ut_init_func_sym(&s, TOK_IDENT + 6);
  utb_set_tok_str(s.v, "some_pure_fn");
  s.f.func_pure = 1;

  UT_ASSERT_EQ(tcc_ir_get_func_purity(NULL, &s), (int)TCC_FUNC_PURITY_PURE);
  return 0;
}

/* POSITIVE: attributes stored on the function TYPE symbol (sym->type.ref->f)
 * are OR'd in, not just the declaration symbol's own sym->f. */
UT_TEST(test_get_func_purity_attr_from_type_ref_propagates)
{
  static Sym s, type_sym;
  ut_init_func_sym(&s, TOK_IDENT + 7);
  utb_set_tok_str(s.v, "some_fn_via_type_ref");
  memset(&type_sym, 0, sizeof(type_sym));
  type_sym.f.func_const = 1;
  s.type.ref = &type_sym;

  UT_ASSERT_EQ(tcc_ir_get_func_purity(NULL, &s), (int)TCC_FUNC_PURITY_CONST);
  return 0;
}

/* POSITIVE: an unknown name with no attributes falls back to the purity
 * cache when a prior inference cached its token. */
UT_TEST(test_get_func_purity_cache_hit)
{
  static Sym s;
  ut_init_func_sym(&s, TOK_IDENT + 8);
  utb_set_tok_str(s.v, "some_cached_fn");

  tcc_state->func_purity_cache_count = 0;
  tcc_ir_cache_func_purity(tcc_state, s.v, TCC_FUNC_PURITY_PURE);

  UT_ASSERT_EQ(tcc_ir_get_func_purity(NULL, &s), (int)TCC_FUNC_PURITY_PURE);

  tcc_state->func_purity_cache_count = 0;
  return 0;
}

/* NEGATIVE: an unknown name, no attributes, no cache entry -> conservative
 * IMPURE default. */
UT_TEST(test_get_func_purity_unknown_defaults_impure)
{
  static Sym s;
  ut_init_func_sym(&s, TOK_IDENT + 9);
  utb_set_tok_str(s.v, "totally_unknown_fn");

  tcc_state->func_purity_cache_count = 0;
  UT_ASSERT_EQ(tcc_ir_get_func_purity(NULL, &s), (int)TCC_FUNC_PURITY_IMPURE);
  return 0;
}

/* ==================================================================
 * tcc_ir_infer_func_purity
 *
 * Scans a function's own IR body (not the caller's) to infer purity:
 * STORE to non-stack memory -> IMPURE; LOAD from non-stack/param memory ->
 * not CONST (but still PURE); calls to impure/unknown/indirect callees ->
 * IMPURE; certain opaque ops (INLINE_ASM, IJUMP, ...) -> IMPURE; VLA_ALLOC
 * -> IMPURE; otherwise CONST if every load was stack/param-only, else PURE.
 * ================================================================== */

/* Symref operands are built with ir_build.h's utb_symref(ir, sym, is_lval,
 * is_local, is_const, btype); it internally calls tcc_ir_pool_add_symref(),
 * which requires the symref pool to be allocated first
 * (pool_symref_capacity > 0) -- so every test below that builds a SYMREF
 * operand uses utb_new()+utb_pools_init() rather than utb_loop_new() (which
 * skips pool init; fine for the LICM hoist tests above, which never touch
 * SYMREFs, but not for these). */

/* POSITIVE: a function whose body only touches stack-local memory (STORE to
 * a stack offset, no calls) is inferred CONST -- the strongest purity level. */
UT_TEST(test_infer_purity_stack_only_store_is_const)
{
  static Sym fn;
  ut_init_func_sym(&fn, TOK_IDENT + 20);
  utb_set_tok_str(fn.v, "stack_only_fn");

  TCCIRState *ir = utb_loop_new();
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_stackoff(-4, 0, 0, 0, I32)), utb_imm(7, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  TCCFuncPurity p = tcc_ir_infer_func_purity(ir, &fn);
  UT_ASSERT_EQ((int)p, (int)TCC_FUNC_PURITY_CONST);

  utb_free(ir);
  return 0;
}

/* POSITIVE: a LOAD from a non-stack address (a global SYMREF) downgrades the
 * result from CONST to PURE (still no observable side effects, but it does
 * read memory outside the local frame). */
UT_TEST(test_infer_purity_global_load_is_pure_not_const)
{
  static Sym fn, g;
  ut_init_func_sym(&fn, TOK_IDENT + 21);
  utb_set_tok_str(fn.v, "reads_global_fn");
  memset(&g, 0, sizeof(g));

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_symref(ir, &g, /*is_lval*/ 1, 0, 0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  TCCFuncPurity p = tcc_ir_infer_func_purity(ir, &fn);
  UT_ASSERT_EQ((int)p, (int)TCC_FUNC_PURITY_PURE);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: a STORE to a non-stack address (global SYMREF dest) makes the
 * function IMPURE outright. */
UT_TEST(test_infer_purity_global_store_is_impure)
{
  static Sym fn, g;
  ut_init_func_sym(&fn, TOK_IDENT + 22);
  utb_set_tok_str(fn.v, "writes_global_fn");
  memset(&g, 0, sizeof(g));

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_STORE, utb_symref(ir, &g, /*is_lval*/ 1, 0, 0, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  TCCFuncPurity p = tcc_ir_infer_func_purity(ir, &fn);
  UT_ASSERT_EQ((int)p, (int)TCC_FUNC_PURITY_IMPURE);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: an indirect call (FUNCCALLVOID whose src1 is not a SYMREF, so
 * irop_get_sym_ex returns NULL) cannot be analyzed for purity and is
 * conservatively IMPURE. */
UT_TEST(test_infer_purity_indirect_call_is_impure)
{
  static Sym fn;
  ut_init_func_sym(&fn, TOK_IDENT + 23);
  utb_set_tok_str(fn.v, "indirect_caller_fn");

  TCCIRState *ir = utb_loop_new();
  /* src1 is a plain vreg (a function pointer held in a temp), not a SYMREF. */
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, utb_temp(1, I32),
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  TCCFuncPurity p = tcc_ir_infer_func_purity(ir, &fn);
  UT_ASSERT_EQ((int)p, (int)TCC_FUNC_PURITY_IMPURE);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: a call to a callee found in the well-known pure_func_table (e.g.
 * "strlen", purity PURE) keeps the caller analyzable but downgrades CONST to
 * PURE (a PURE callee means the caller can't be CONST either). */
UT_TEST(test_infer_purity_call_to_known_pure_callee_downgrades_to_pure)
{
  static Sym fn, callee;
  ut_init_func_sym(&fn, TOK_IDENT + 24);
  utb_set_tok_str(fn.v, "calls_strlen_fn");
  memset(&callee, 0, sizeof(callee));
  callee.v = TOK_IDENT + 25;
  utb_set_tok_str(callee.v, "strlen");

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, utb_symref(ir, &callee, 0, 0, 0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  TCCFuncPurity p = tcc_ir_infer_func_purity(ir, &fn);
  UT_ASSERT_EQ((int)p, (int)TCC_FUNC_PURITY_PURE);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: a call to a callee found in the table at CONST level (e.g.
 * "abs") does NOT downgrade -- the caller can remain CONST if nothing else
 * disqualifies it. */
UT_TEST(test_infer_purity_call_to_known_const_callee_stays_const)
{
  static Sym fn, callee;
  ut_init_func_sym(&fn, TOK_IDENT + 26);
  utb_set_tok_str(fn.v, "calls_abs_fn");
  memset(&callee, 0, sizeof(callee));
  callee.v = TOK_IDENT + 27;
  utb_set_tok_str(callee.v, "abs");

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, utb_symref(ir, &callee, 0, 0, 0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  TCCFuncPurity p = tcc_ir_infer_func_purity(ir, &fn);
  UT_ASSERT_EQ((int)p, (int)TCC_FUNC_PURITY_CONST);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: a call to an unrecognized callee (not in the table, no
 * pure/const attribute) is conservatively treated as impure -> caller is
 * IMPURE. */
UT_TEST(test_infer_purity_call_to_unknown_callee_is_impure)
{
  static Sym fn, callee;
  ut_init_func_sym(&fn, TOK_IDENT + 28);
  utb_set_tok_str(fn.v, "calls_unknown_fn");
  memset(&callee, 0, sizeof(callee));
  callee.v = TOK_IDENT + 29;
  utb_set_tok_str(callee.v, "totally_unrecognized_callee");

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, utb_symref(ir, &callee, 0, 0, 0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  TCCFuncPurity p = tcc_ir_infer_func_purity(ir, &fn);
  UT_ASSERT_EQ((int)p, (int)TCC_FUNC_PURITY_IMPURE);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: an opaque side-effecting op (TRAP is in the switch's opaque-op
 * list) makes the function IMPURE regardless of the rest of the body. */
UT_TEST(test_infer_purity_opaque_op_trap_is_impure)
{
  static Sym fn;
  ut_init_func_sym(&fn, TOK_IDENT + 30);
  utb_set_tok_str(fn.v, "traps_fn");

  TCCIRState *ir = utb_loop_new();
  utb_emit(ir, TCCIR_OP_TRAP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  TCCFuncPurity p = tcc_ir_infer_func_purity(ir, &fn);
  UT_ASSERT_EQ((int)p, (int)TCC_FUNC_PURITY_IMPURE);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: VLA_ALLOC makes the function IMPURE (non-trivial stack
 * adjustment at runtime). */
UT_TEST(test_infer_purity_vla_alloc_is_impure)
{
  static Sym fn;
  ut_init_func_sym(&fn, TOK_IDENT + 31);
  utb_set_tok_str(fn.v, "vla_fn");

  TCCIRState *ir = utb_loop_new();
  utb_emit(ir, TCCIR_OP_VLA_ALLOC, UTB_NONE, utb_imm(16, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  TCCFuncPurity p = tcc_ir_infer_func_purity(ir, &fn);
  UT_ASSERT_EQ((int)p, (int)TCC_FUNC_PURITY_IMPURE);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): NULL ir or NULL func_sym -> IMPURE, no crash. */
UT_TEST(test_infer_purity_null_args_are_impure)
{
  static Sym fn;
  ut_init_func_sym(&fn, TOK_IDENT + 32);

  TCCIRState *ir = utb_loop_new();
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  UT_ASSERT_EQ((int)tcc_ir_infer_func_purity(NULL, &fn), (int)TCC_FUNC_PURITY_IMPURE);
  UT_ASSERT_EQ((int)tcc_ir_infer_func_purity(ir, NULL), (int)TCC_FUNC_PURITY_IMPURE);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_licm)
{
  UT_COVERS("licm");
  UT_RUN(test_licm_hoists_invariant_add);
  UT_RUN(test_ssa_opt_licm_hoists_invariant);
  UT_RUN(test_licm_hoists_const_call_with_invariant_arg);
  UT_RUN(test_licm_no_hoist_pure_call_when_loop_writes_memory);
  UT_RUN(test_licm_no_loop_no_change);
  UT_RUN(test_licm_loop_no_invariant_no_hoist);
  UT_RUN(test_licm_deref_source_not_hoisted);
  UT_RUN(test_licm_in_loop_def_blocks_hoist);
  UT_RUN(test_licm_store_not_hoisted);
  UT_RUN(test_licm_div_not_hoisted);
  UT_RUN(test_licm_lea_stack_addr_hoisted);
  UT_RUN(test_licm_header_at_entry_no_hoist);
  UT_RUN(test_licm_merge_preheader_no_hoist);
  UT_RUN(test_licm_multiple_back_edges_hoisted_once);
  UT_RUN(test_licm_nested_loop_hoists_to_right_preheader);
  UT_RUN(test_licm_idempotent_no_new_hoists);

  UT_RUN(test_detect_loops_finds_single_backward_jump);
  UT_RUN(test_detect_loops_no_backward_jump_returns_null);
  UT_RUN(test_detect_loops_empty_ir_returns_null);
  UT_RUN(test_detect_loops_filters_switch_break_subset);

  UT_RUN(test_hoist_budget_low_pressure_floor_of_three);
  UT_RUN(test_hoist_budget_shrinks_with_more_params);
  UT_RUN(test_hoist_budget_shrinks_with_more_distinct_vregs);
  UT_RUN(test_hoist_budget_floors_at_one);
  UT_RUN(test_hoist_budget_defaults_total_regs_when_unset);

  UT_RUN(test_purity_cache_add_then_lookup_roundtrips);
  UT_RUN(test_purity_cache_lookup_miss_returns_minus_one);
  UT_RUN(test_purity_cache_duplicate_token_keeps_first_value);
  UT_RUN(test_purity_cache_rejects_token_below_tok_ident);

  UT_RUN(test_get_func_purity_null_sym_is_unknown);
  UT_RUN(test_get_func_purity_non_function_sym_is_impure);
  UT_RUN(test_get_func_purity_well_known_table_hit);
  UT_RUN(test_get_func_purity_well_known_table_const_hit);
  UT_RUN(test_get_func_purity_noreturn_attr_is_impure);
  UT_RUN(test_get_func_purity_const_attr);
  UT_RUN(test_get_func_purity_pure_attr);
  UT_RUN(test_get_func_purity_attr_from_type_ref_propagates);
  UT_RUN(test_get_func_purity_cache_hit);
  UT_RUN(test_get_func_purity_unknown_defaults_impure);

  UT_RUN(test_infer_purity_stack_only_store_is_const);
  UT_RUN(test_infer_purity_global_load_is_pure_not_const);
  UT_RUN(test_infer_purity_global_store_is_impure);
  UT_RUN(test_infer_purity_indirect_call_is_impure);
  UT_RUN(test_infer_purity_call_to_known_pure_callee_downgrades_to_pure);
  UT_RUN(test_infer_purity_call_to_known_const_callee_stays_const);
  UT_RUN(test_infer_purity_call_to_unknown_callee_is_impure);
  UT_RUN(test_infer_purity_opaque_op_trap_is_impure);
  UT_RUN(test_infer_purity_vla_alloc_is_impure);
  UT_RUN(test_infer_purity_null_args_are_impure);
}
