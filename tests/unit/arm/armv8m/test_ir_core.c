/*
 *  test_ir_core.c - suite for ir/core.c IR instruction building
 *
 *  Exercises instruction append, operand packing, leaf/call tracking,
 *  jump-chain backpatching, and the irop_config shape table.
 */

#define USING_GLOBALS
#include "ir.h"

#include "ut.h"

static SValue sv_var(int vreg)
{
  SValue sv;
  svalue_init(&sv);
  sv.vr = vreg;
  sv.type.t = VT_INT;
  return sv;
}

static SValue sv_const(int v)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_CONST;
  sv.c.i = v;
  sv.type.t = VT_INT;
  return sv;
}

static SValue sv_jump_target(int target_idx)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_CONST;
  sv.c.i = target_idx;
  sv.type.t = VT_INT;
  return sv;
}

/* A local variable value (vr < 0, VT_LOCAL|VT_LVAL): a *real* stack slot, not
 * tracked by the vreg/live-interval system.  svalue_to_iroperand() (Case 3)
 * turns this into an IROP_TAG_STACKOFF operand with vreg_type == 0. */
static SValue sv_stack_local(int frame_off)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_LOCAL | VT_LVAL;
  sv.vr = -1;
  sv.c.i = frame_off;
  sv.type.t = VT_INT;
  return sv;
}

static SValue sv_var_llong(int vreg)
{
  SValue sv;
  svalue_init(&sv);
  sv.vr = vreg;
  sv.type.t = VT_LLONG;
  return sv;
}

/* -------------------------------------------------------------------------- */
/* Lifecycle and basic counts                                                 */
/* -------------------------------------------------------------------------- */

UT_TEST(test_alloc_fresh_block_has_zero_instructions)
{
  TCCIRState *ir = tcc_ir_alloc();
  UT_ASSERT(ir != NULL);
  UT_ASSERT_EQ(tcc_ir_count(ir), 0);
  UT_ASSERT_EQ(ir->iroperand_pool_count, 0);
  UT_ASSERT_EQ(ir->next_instruction_index, 0);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_put_add_packs_operands)
{
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);
  int t0 = tcc_ir_vreg_alloc_temp(ir);

  SValue dest = sv_var(t0);
  SValue src1 = sv_var(v0);
  SValue src2 = sv_const(7);

  int idx = tcc_ir_put(ir, TCCIR_OP_ADD, &src1, &src2, &dest);
  UT_ASSERT_EQ(idx, 0);
  UT_ASSERT_EQ(tcc_ir_count(ir), 1);

  IRQuadCompact *q = &ir->compact_instructions[idx];
  UT_ASSERT_EQ(q->op, TCCIR_OP_ADD);

  IROperand d = tcc_ir_op_get_dest(ir, q);
  IROperand s1 = tcc_ir_op_get_src1(ir, q);
  IROperand s2 = tcc_ir_op_get_src2(ir, q);

  UT_ASSERT_EQ(irop_get_vreg(d), t0);
  UT_ASSERT_EQ(irop_get_vreg(s1), v0);
  UT_ASSERT_EQ(irop_get_tag(s2), IROP_TAG_IMM32);
  UT_ASSERT_EQ(irop_get_imm32(s2), 7);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_put_no_op_has_no_operands)
{
  TCCIRState *ir = tcc_ir_alloc();
  int before = ir->iroperand_pool_count;
  int idx = tcc_ir_put_no_op(ir, TCCIR_OP_NOP);
  UT_ASSERT_EQ(idx, 0);
  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  UT_ASSERT_EQ(ir->iroperand_pool_count, before);

  IRQuadCompact *q = &ir->compact_instructions[idx];
  IROperand d = tcc_ir_op_get_dest(ir, q);
  IROperand s1 = tcc_ir_op_get_src1(ir, q);
  IROperand s2 = tcc_ir_op_get_src2(ir, q);
  UT_ASSERT(irop_is_none(d));
  UT_ASSERT(irop_is_none(s1));
  UT_ASSERT(irop_is_none(s2));

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_count_and_current_idx)
{
  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);
  int t1 = tcc_ir_vreg_alloc_temp(ir);

  SValue s_t0 = sv_var(t0);
  SValue s_t1 = sv_var(t1);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_t0, NULL, &s_t1);
  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  UT_ASSERT_EQ(tcc_ir_current_idx(ir), 0);

  SValue r_t1 = sv_var(t1);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &r_t1, NULL, NULL);
  UT_ASSERT_EQ(tcc_ir_count(ir), 2);
  UT_ASSERT_EQ(tcc_ir_current_idx(ir), 1);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Leaf / call tracking                                                       */
/* -------------------------------------------------------------------------- */

UT_TEST(test_leaf_by_default)
{
  TCCIRState *ir = tcc_ir_alloc();
  UT_ASSERT(tcc_ir_is_leaf(ir));
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_call_marks_nonleaf)
{
  TCCIRState *ir = tcc_ir_alloc();
  SValue func = sv_const(0);
  SValue call_info = sv_const((int)TCCIR_ENCODE_CALL(0, 0));
  tcc_ir_put(ir, TCCIR_OP_FUNCCALLVOID, &func, &call_info, NULL);
  UT_ASSERT(!tcc_ir_is_leaf(ir));
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_nonleaf_mark_explicit)
{
  TCCIRState *ir = tcc_ir_alloc();
  tcc_ir_nonleaf_mark(ir);
  UT_ASSERT(!tcc_ir_is_leaf(ir));
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_call_id_next_monotonic)
{
  TCCIRState *ir = tcc_ir_alloc();
  UT_ASSERT_EQ(tcc_ir_call_id_next(ir), 0);
  UT_ASSERT_EQ(tcc_ir_call_id_next(ir), 1);
  UT_ASSERT_EQ(tcc_ir_call_id_next(ir), 2);
  UT_ASSERT_EQ(ir->next_call_id, 3);
  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Operand setters / getters                                                  */
/* -------------------------------------------------------------------------- */

UT_TEST(test_set_dest_roundtrip)
{
  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);
  int t1 = tcc_ir_vreg_alloc_temp(ir);

  SValue dest = sv_var(t0);
  SValue src = sv_const(1);
  int idx = tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src, NULL, &dest);

  IROperand new_dest = irop_make_vreg(t1, IROP_BTYPE_INT32);
  tcc_ir_set_dest(ir, idx, new_dest);

  IROperand d = tcc_ir_op_get_dest(ir, &ir->compact_instructions[idx]);
  UT_ASSERT_EQ(irop_get_vreg(d), t1);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* ASSIGN-coalescing optimization (tcc_ir_put, ir/core.c ~535-613)            */
/*                                                                            */
/* When an ASSIGN's src1 is the TEMP that was just produced as the dest of   */
/* the immediately preceding instruction, tcc_ir_put() redirects that prior  */
/* instruction's dest to the ASSIGN's dest and drops the ASSIGN itself       */
/* (returns pos - 1, does not bump next_instruction_index).  This is a real, */
/* narrowly-scoped peephole -- these tests pin down exactly when it does and */
/* does not fire.                                                            */
/* -------------------------------------------------------------------------- */

UT_TEST(test_coalesce_assign_from_prev_temp_dest)
{
  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);
  int v0 = tcc_ir_vreg_alloc_var(ir);

  /* ADD t0, v0, #1 -- basic_block_start consumed here, dest is a TEMP. */
  SValue add_src1 = sv_var(v0);
  SValue add_src2 = sv_const(1);
  SValue add_dest = sv_var(t0);
  int add_idx = tcc_ir_put(ir, TCCIR_OP_ADD, &add_src1, &add_src2, &add_dest);
  UT_ASSERT_EQ(add_idx, 0);
  UT_ASSERT_EQ(tcc_ir_count(ir), 1);

  /* ASSIGN v0, t0 -- src1 is exactly the TEMP produced above -> coalesce. */
  SValue assign_src1 = sv_var(t0);
  SValue assign_dest = sv_var(v0);
  int assign_idx = tcc_ir_put(ir, TCCIR_OP_ASSIGN, &assign_src1, NULL, &assign_dest);

  /* Coalescing returns the index of the *prior* instruction and does not
   * append a new one. */
  UT_ASSERT_EQ(assign_idx, add_idx);
  UT_ASSERT_EQ(tcc_ir_count(ir), 1);

  /* The ADD's dest has been redirected from t0 to v0. */
  IROperand d = tcc_ir_op_get_dest(ir, &ir->compact_instructions[add_idx]);
  UT_ASSERT_EQ(irop_get_vreg(d), v0);
  UT_ASSERT_EQ(ir->compact_instructions[add_idx].op, TCCIR_OP_ADD);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_coalesce_skipped_when_src1_is_var_not_temp)
{
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);
  int v1 = tcc_ir_vreg_alloc_var(ir);

  /* ADD v1, v0, #1 -- dest is a VAR (tcc_ir_vreg_alloc_var), not a TEMP.
   * The prior swarm-agent bug-fix note flags this as the specific exemption:
   * tcc_ir_vreg_alloc_var()-allocated vregs are never coalesce targets
   * because TCCIR_DECODE_VREG_TYPE(...) == TCCIR_VREG_TYPE_TEMP is required. */
  SValue add_src1 = sv_var(v0);
  SValue add_src2 = sv_const(1);
  SValue add_dest = sv_var(v1);
  int add_idx = tcc_ir_put(ir, TCCIR_OP_ADD, &add_src1, &add_src2, &add_dest);

  SValue assign_src1 = sv_var(v1);
  SValue assign_dest = sv_var(v0);
  int assign_idx = tcc_ir_put(ir, TCCIR_OP_ASSIGN, &assign_src1, NULL, &assign_dest);

  /* Not coalesced: a genuinely new instruction is appended. */
  UT_ASSERT_EQ(assign_idx, add_idx + 1);
  UT_ASSERT_EQ(tcc_ir_count(ir), 2);
  UT_ASSERT_EQ(ir->compact_instructions[assign_idx].op, TCCIR_OP_ASSIGN);

  /* The ADD's dest is untouched (still v1). */
  IROperand d = tcc_ir_op_get_dest(ir, &ir->compact_instructions[add_idx]);
  UT_ASSERT_EQ(irop_get_vreg(d), v1);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_coalesce_skipped_at_basic_block_start)
{
  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);
  int v0 = tcc_ir_vreg_alloc_var(ir);

  /* Fresh block: ir->basic_block_start == 1.  The very first instruction
   * always consumes basic_block_start (sets it to 0) rather than attempting
   * to coalesce with "the previous instruction" (of which there is none). */
  SValue assign_src1 = sv_var(t0);
  SValue assign_dest = sv_var(v0);
  int idx = tcc_ir_put(ir, TCCIR_OP_ASSIGN, &assign_src1, NULL, &assign_dest);

  UT_ASSERT_EQ(idx, 0);
  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  UT_ASSERT_EQ(ir->basic_block_start, 0);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_coalesce_skipped_when_prevent_coalescing_set)
{
  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);
  int v0 = tcc_ir_vreg_alloc_var(ir);

  SValue add_src1 = sv_var(v0);
  SValue add_src2 = sv_const(1);
  SValue add_dest = sv_var(t0);
  int add_idx = tcc_ir_put(ir, TCCIR_OP_ADD, &add_src1, &add_src2, &add_dest);

  ir->prevent_coalescing = 1;

  SValue assign_src1 = sv_var(t0);
  SValue assign_dest = sv_var(v0);
  int assign_idx = tcc_ir_put(ir, TCCIR_OP_ASSIGN, &assign_src1, NULL, &assign_dest);

  UT_ASSERT_EQ(assign_idx, add_idx + 1);
  UT_ASSERT_EQ(tcc_ir_count(ir), 2);

  /* ADD's dest is untouched (still t0). */
  IROperand d = tcc_ir_op_get_dest(ir, &ir->compact_instructions[add_idx]);
  UT_ASSERT_EQ(irop_get_vreg(d), t0);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_coalesce_skipped_when_src1_is_lval)
{
  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);
  int v0 = tcc_ir_vreg_alloc_var(ir);

  SValue add_src1 = sv_var(v0);
  SValue add_src2 = sv_const(1);
  SValue add_dest = sv_var(t0);
  int add_idx = tcc_ir_put(ir, TCCIR_OP_ADD, &add_src1, &add_src2, &add_dest);

  /* ASSIGN whose src1 is an lvalue reference to t0 (e.g. *t0 style
   * register-indirect load) rather than the plain temp value itself.
   * can_coalesce requires !src1_irop.is_lval, so this must NOT coalesce. */
  SValue assign_src1 = sv_var(t0);
  assign_src1.r = VT_LVAL;
  SValue assign_dest = sv_var(v0);
  int assign_idx = tcc_ir_put(ir, TCCIR_OP_ASSIGN, &assign_src1, NULL, &assign_dest);

  UT_ASSERT_EQ(assign_idx, add_idx + 1);
  UT_ASSERT_EQ(tcc_ir_count(ir), 2);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_coalesce_skipped_when_src1_vreg_mismatches_prev_dest)
{
  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);
  int t1 = tcc_ir_vreg_alloc_temp(ir);
  int v0 = tcc_ir_vreg_alloc_var(ir);

  /* Two temps in flight; ADD produces t0, but the ASSIGN reads t1 (a
   * different, unrelated temp) -- must not coalesce with the ADD. */
  SValue add_src1 = sv_var(v0);
  SValue add_src2 = sv_const(1);
  SValue add_dest = sv_var(t0);
  int add_idx = tcc_ir_put(ir, TCCIR_OP_ADD, &add_src1, &add_src2, &add_dest);

  SValue assign_src1 = sv_var(t1);
  SValue assign_dest = sv_var(v0);
  int assign_idx = tcc_ir_put(ir, TCCIR_OP_ASSIGN, &assign_src1, NULL, &assign_dest);

  UT_ASSERT_EQ(assign_idx, add_idx + 1);
  UT_ASSERT_EQ(tcc_ir_count(ir), 2);

  IROperand d = tcc_ir_op_get_dest(ir, &ir->compact_instructions[add_idx]);
  UT_ASSERT_EQ(irop_get_vreg(d), t0);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_coalesce_width_mismatch_blocks_coalescing)
{
  /* NOTE: an earlier version of this test asserted that a width mismatch
   * still coalesces via the irop_make_vreg() "else" rebuild branch at
   * ir/core.c ~587-601.  That was wrong: `width_match` is itself one of
   * the conjuncts of `can_coalesce` (ir/core.c:547), so inside
   * `if (can_coalesce)` the `if (width_match)` check at line 566 always
   * takes the true branch.  The "else" rebuild path is therefore dead
   * code as currently gated -- a width mismatch simply blocks coalescing
   * altogether and the ASSIGN is emitted as its own instruction.  This
   * test documents that actual, currently-observed behavior. */
  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);
  int v0 = tcc_ir_vreg_alloc_var(ir);

  /* ADD produces a plain 32-bit temp t0. */
  SValue add_src1 = sv_var(v0);
  SValue add_src2 = sv_const(1);
  SValue add_dest = sv_var(t0);
  int add_idx = tcc_ir_put(ir, TCCIR_OP_ADD, &add_src1, &add_src2, &add_dest);

  /* ASSIGN into a 64-bit-typed var dest.  prev_is_64bit (t0, 32-bit) !=
   * new_is_64bit (v0, VT_LLONG) so width_match is false, which makes
   * can_coalesce false too: coalescing does not fire. */
  SValue assign_src1 = sv_var(t0);
  SValue assign_dest = sv_var_llong(v0);
  int assign_idx = tcc_ir_put(ir, TCCIR_OP_ASSIGN, &assign_src1, NULL, &assign_dest);

  /* Not coalesced: a distinct instruction is appended. */
  UT_ASSERT_EQ(assign_idx, add_idx + 1);
  UT_ASSERT_EQ(tcc_ir_count(ir), 2);

  /* The ADD's dest is untouched (still t0, 32-bit). */
  IROperand add_d = tcc_ir_op_get_dest(ir, &ir->compact_instructions[add_idx]);
  UT_ASSERT_EQ(irop_get_vreg(add_d), t0);
  UT_ASSERT(!irop_is_64bit(add_d));

  /* The ASSIGN stands alone with its own 64-bit dest. */
  IROperand assign_d = tcc_ir_op_get_dest(ir, &ir->compact_instructions[assign_idx]);
  UT_ASSERT_EQ(irop_get_vreg(assign_d), v0);
  UT_ASSERT(irop_is_64bit(assign_d));

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_coalesce_into_real_stack_slot_sets_stackoff_tag)
{
  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);
  int v0 = tcc_ir_vreg_alloc_var(ir);

  SValue add_src1 = sv_var(v0);
  SValue add_src2 = sv_const(1);
  SValue add_dest = sv_var(t0);
  int add_idx = tcc_ir_put(ir, TCCIR_OP_ADD, &add_src1, &add_src2, &add_dest);

  /* ASSIGN into a *real* stack slot (vr == -1, VT_LOCAL|VT_LVAL) rather than
   * a tracked var vreg -- exercises the "Temp locals and concrete stack
   * slots ... need the STACKOFF tag" branch (new_dest_vr < 0 path). */
  SValue assign_src1 = sv_var(t0);
  SValue assign_dest = sv_stack_local(-12);
  int assign_idx = tcc_ir_put(ir, TCCIR_OP_ASSIGN, &assign_src1, NULL, &assign_dest);

  UT_ASSERT_EQ(assign_idx, add_idx);
  UT_ASSERT_EQ(tcc_ir_count(ir), 1);

  IROperand d = tcc_ir_op_get_dest(ir, &ir->compact_instructions[add_idx]);
  UT_ASSERT_EQ(irop_get_vreg(d), -1);
  UT_ASSERT_EQ(irop_get_tag(d), IROP_TAG_STACKOFF);
  UT_ASSERT_EQ(irop_get_imm32(d), -12);
  UT_ASSERT(d.is_local);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Jump-chain backpatching                                                    */
/* -------------------------------------------------------------------------- */

UT_TEST(test_backpatch_to_here)
{
  TCCIRState *ir = tcc_ir_alloc();

  /* JUMP with target 7, followed by a few NOPs. */
  SValue target = sv_jump_target(7);
  int head = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &target);
  tcc_ir_put_no_op(ir, TCCIR_OP_NOP);
  tcc_ir_put_no_op(ir, TCCIR_OP_NOP);

  /* Backpatch the jump chain to the current instruction position.
   * tcc_ir_backpatch_to_here stores ir->next_instruction_index as the target. */
  tcc_ir_backpatch_to_here(ir, head);

  IROperand d = tcc_ir_op_get_dest(ir, &ir->compact_instructions[head]);
  UT_ASSERT_EQ(irop_get_imm32(d), ir->next_instruction_index);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_backpatch_walks_multi_link_chain)
{
  TCCIRState *ir = tcc_ir_alloc();

  /* Build a 3-link jump chain by hand: JUMP(0)->1, JUMP(1)->2, JUMP(2)->-1
   * (sentinel end).  tcc_ir_backpatch(ir, 0, target) must walk all three,
   * rewriting every dest to `target` and stopping at the -1 sentinel. */
  SValue t1 = sv_jump_target(1);
  int j0 = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &t1);
  SValue t2 = sv_jump_target(2);
  int j1 = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &t2);
  SValue t3 = sv_jump_target(-1);
  int j2 = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &t3);
  tcc_ir_put_no_op(ir, TCCIR_OP_NOP); /* index 3, backpatch target */

  UT_ASSERT_EQ(j0, 0);
  UT_ASSERT_EQ(j1, 1);
  UT_ASSERT_EQ(j2, 2);

  tcc_ir_backpatch(ir, j0, 3);

  IROperand d0 = tcc_ir_op_get_dest(ir, &ir->compact_instructions[j0]);
  IROperand d1 = tcc_ir_op_get_dest(ir, &ir->compact_instructions[j1]);
  IROperand d2 = tcc_ir_op_get_dest(ir, &ir->compact_instructions[j2]);
  UT_ASSERT_EQ(irop_get_imm32(d0), 3);
  UT_ASSERT_EQ(irop_get_imm32(d1), 3);
  UT_ASSERT_EQ(irop_get_imm32(d2), 3);

  /* Target instruction must be marked as a jump target. */
  UT_ASSERT(ir->compact_instructions[3].is_jump_target);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_backpatch_negative_chain_is_noop)
{
  TCCIRState *ir = tcc_ir_alloc();
  /* t == -1 means "no chain": must return immediately without touching
   * next_instruction_index or crashing on an empty instruction array. */
  tcc_ir_backpatch(ir, -1, 5);
  UT_ASSERT_EQ(tcc_ir_count(ir), 0);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_backpatch_stops_at_non_jump_instruction)
{
  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);

  /* A non-jump instruction at the chain head must not be corrupted:
   * tcc_ir_backpatch breaks out without touching its operands. */
  SValue src = sv_const(1);
  SValue dest = sv_var(t0);
  int add_idx = tcc_ir_put(ir, TCCIR_OP_ADD, &src, &src, &dest);

  tcc_ir_backpatch(ir, add_idx, 99);

  IROperand d = tcc_ir_op_get_dest(ir, &ir->compact_instructions[add_idx]);
  /* dest is still the vreg t0 written by tcc_ir_put, not corrupted into an
   * imm32 jump target. */
  UT_ASSERT_EQ(irop_get_vreg(d), t0);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_backpatch_first_patches_only_last_link)
{
  TCCIRState *ir = tcc_ir_alloc();

  /* Chain: JUMP(0)->1, JUMP(1)-> -1 (end).  tcc_ir_backpatch_first walks to
   * the *last* link in the chain and rewrites only that one (via
   * tcc_ir_pool_jump_target_set), leaving earlier links untouched -- this is
   * the "first empty slot in the chain" append primitive used by gsym-style
   * single-link patching, distinct from tcc_ir_backpatch's walk-and-rewrite-all. */
  SValue t1 = sv_jump_target(1);
  int j0 = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &t1);
  SValue t2 = sv_jump_target(-1);
  int j1 = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &t2);

  tcc_ir_backpatch_first(ir, j0, 42);

  IROperand d0 = tcc_ir_op_get_dest(ir, &ir->compact_instructions[j0]);
  IROperand d1 = tcc_ir_op_get_dest(ir, &ir->compact_instructions[j1]);
  /* First link is unchanged (still points at j1). */
  UT_ASSERT_EQ(irop_get_imm32(d0), 1);
  /* Last link in the chain now points at the new target. */
  UT_ASSERT_EQ(irop_get_imm32(d1), 42);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_backpatch_first_negative_chain_is_noop)
{
  TCCIRState *ir = tcc_ir_alloc();
  /* t < 0 means "no chain": must return immediately. */
  tcc_ir_backpatch_first(ir, -1, 5);
  UT_ASSERT_EQ(tcc_ir_count(ir), 0);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_gjmp_append_links_new_chain_onto_existing)
{
  TCCIRState *ir = tcc_ir_alloc();

  SValue t1 = sv_jump_target(-1);
  int n = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &t1);

  /* tcc_ir_gjmp_append(ir, n, t): when n is a valid instruction index, links
   * chain `t` onto the end of chain `n` (via tcc_ir_backpatch_first) and
   * returns n (the head of the combined chain). */
  int result = tcc_ir_gjmp_append(ir, n, 77);
  UT_ASSERT_EQ(result, n);

  IROperand d = tcc_ir_op_get_dest(ir, &ir->compact_instructions[n]);
  UT_ASSERT_EQ(irop_get_imm32(d), 77);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_gjmp_append_returns_t_when_n_out_of_range)
{
  TCCIRState *ir = tcc_ir_alloc();
  /* n < 0 or n >= next_instruction_index: nothing to link onto, return t
   * unchanged (t becomes the new chain head). */
  UT_ASSERT_EQ(tcc_ir_gjmp_append(ir, -1, 55), 55);
  UT_ASSERT_EQ(tcc_ir_gjmp_append(ir, 0, 55), 55); /* no instructions yet */
  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Utility functions: NULL-safety                                             */
/* -------------------------------------------------------------------------- */

UT_TEST(test_utility_functions_null_safe)
{
  UT_ASSERT_EQ(tcc_ir_count(NULL), 0);
  UT_ASSERT_EQ(tcc_ir_current_idx(NULL), -1);
  UT_ASSERT_EQ(tcc_ir_is_leaf(NULL), 0);
  UT_ASSERT_EQ(tcc_ir_call_id_next(NULL), 0);
  /* Must not crash: */
  tcc_ir_nonleaf_mark(NULL);
  tcc_ir_backpatch_to_here(NULL, 0);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Token -> IR opcode mapping (tcc_irop_from_token)                           */
/* -------------------------------------------------------------------------- */

/* tcc_irop_from_token() is defined non-static in ir/core.c (called
 * internally by tcc_ir_gen_i()) but is not declared in ir/core.h or any
 * other header -- no production TU currently calls it from outside
 * ir/core.c.  Declare it locally here rather than editing a production
 * header (see swarm ground rules). */
extern TccIrOp tcc_irop_from_token(int token);

UT_TEST(test_irop_from_token_arithmetic)
{
  UT_ASSERT_EQ(tcc_irop_from_token('+'), TCCIR_OP_ADD);
  UT_ASSERT_EQ(tcc_irop_from_token('-'), TCCIR_OP_SUB);
  UT_ASSERT_EQ(tcc_irop_from_token('*'), TCCIR_OP_MUL);
  UT_ASSERT_EQ(tcc_irop_from_token('/'), TCCIR_OP_DIV);
  UT_ASSERT_EQ(tcc_irop_from_token('%'), TCCIR_OP_IMOD);
  UT_ASSERT_EQ(tcc_irop_from_token('&'), TCCIR_OP_AND);
  UT_ASSERT_EQ(tcc_irop_from_token('|'), TCCIR_OP_OR);
  UT_ASSERT_EQ(tcc_irop_from_token('^'), TCCIR_OP_XOR);
  return 0;
}

UT_TEST(test_irop_from_token_carry_and_wide_mul)
{
  UT_ASSERT_EQ(tcc_irop_from_token(TOK_ADDC1), TCCIR_OP_ADC_GEN);
  UT_ASSERT_EQ(tcc_irop_from_token(TOK_ADDC2), TCCIR_OP_ADC_USE);
  UT_ASSERT_EQ(tcc_irop_from_token(TOK_SUBC1), TCCIR_OP_SUBC_GEN);
  UT_ASSERT_EQ(tcc_irop_from_token(TOK_SUBC2), TCCIR_OP_SUBC_USE);
  UT_ASSERT_EQ(tcc_irop_from_token(TOK_UMULL), TCCIR_OP_UMULL);
  UT_ASSERT_EQ(tcc_irop_from_token(TOK_SMULL), TCCIR_OP_SMULL);
  UT_ASSERT_EQ(tcc_irop_from_token(TOK_UDIV), TCCIR_OP_UDIV);
  UT_ASSERT_EQ(tcc_irop_from_token(TOK_UMOD), TCCIR_OP_UMOD);
  /* TOK_PDIV maps to the same op as plain '/' (pointer-difference fast div). */
  UT_ASSERT_EQ(tcc_irop_from_token(TOK_PDIV), TCCIR_OP_DIV);
  return 0;
}

UT_TEST(test_irop_from_token_shifts)
{
  UT_ASSERT_EQ(tcc_irop_from_token(TOK_SHL), TCCIR_OP_SHL);
  UT_ASSERT_EQ(tcc_irop_from_token(TOK_SAR), TCCIR_OP_SAR);
  UT_ASSERT_EQ(tcc_irop_from_token(TOK_SHR), TCCIR_OP_SHR);
  return 0;
}

UT_TEST(test_irop_from_token_all_comparisons_map_to_cmp)
{
  /* Every relational/equality token collapses to a single TCCIR_OP_CMP; the
   * actual condition is carried separately (vtop->cmp_op), not in the opcode. */
  UT_ASSERT_EQ(tcc_irop_from_token(TOK_EQ), TCCIR_OP_CMP);
  UT_ASSERT_EQ(tcc_irop_from_token(TOK_NE), TCCIR_OP_CMP);
  UT_ASSERT_EQ(tcc_irop_from_token(TOK_LT), TCCIR_OP_CMP);
  UT_ASSERT_EQ(tcc_irop_from_token(TOK_GT), TCCIR_OP_CMP);
  UT_ASSERT_EQ(tcc_irop_from_token(TOK_LE), TCCIR_OP_CMP);
  UT_ASSERT_EQ(tcc_irop_from_token(TOK_GE), TCCIR_OP_CMP);
  UT_ASSERT_EQ(tcc_irop_from_token(TOK_ULT), TCCIR_OP_CMP);
  UT_ASSERT_EQ(tcc_irop_from_token(TOK_UGT), TCCIR_OP_CMP);
  UT_ASSERT_EQ(tcc_irop_from_token(TOK_ULE), TCCIR_OP_CMP);
  UT_ASSERT_EQ(tcc_irop_from_token(TOK_UGE), TCCIR_OP_CMP);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* tcc_ir_put: destination-type inference and side effects                    */
/* -------------------------------------------------------------------------- */

UT_TEST(test_put_infers_dest_type_from_untyped_dest_and_src1)
{
  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);
  int v0 = tcc_ir_vreg_alloc_var(ir);

  /* dest->type.t == 0 (untyped): tcc_ir_put must infer it from src1 when
   * src1 is 64-bit (VT_LLONG), setting dest->type = src1->type and flagging
   * the vreg as llong via tcc_ir_vreg_type_set_64bit(). */
  SValue src1 = sv_var_llong(v0);
  SValue dest;
  svalue_init(&dest);
  dest.vr = t0; /* type.t left at 0 by svalue_init */

  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src1, NULL, &dest);

  /* dest (the SValue passed in) was mutated in place with the inferred type. */
  UT_ASSERT_EQ(dest.type.t & VT_BTYPE, VT_LLONG);

  IRLiveInterval *iv = tcc_ir_vreg_live_interval(ir, t0);
  UT_ASSERT(iv->is_llong);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_put_assign_marks_dest_lvalue_when_src1_is_plain_value)
{
  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);
  int v0 = tcc_ir_vreg_alloc_var(ir);

  /* ASSIGN with a plain (non-lval, non-stack-addr) src1: dest_interval's
   * is_lvalue flag is set to 1 -- "this destination now holds a materialized
   * value copied straight from src1", per the comment in tcc_ir_put. */
  SValue src1 = sv_var(v0);
  SValue dest = sv_var(t0);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src1, NULL, &dest);

  IRLiveInterval *iv = tcc_ir_vreg_live_interval(ir, t0);
  UT_ASSERT_EQ(iv->is_lvalue, 1);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_put_assign_does_not_mark_lvalue_when_src1_is_stack_addr)
{
  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);

  /* src1 is a stack *address* (VT_LOCAL, no VT_LVAL, vr == -1): taking the
   * address of a local is not "loading a value", so dest_interval->is_lvalue
   * must stay 0 (ir_operand_is_stack_addr() gates this). */
  SValue src1;
  svalue_init(&src1);
  src1.r = VT_LOCAL;
  src1.vr = -1;
  src1.c.i = -8;
  src1.type.t = VT_INT;

  SValue dest = sv_var(t0);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src1, NULL, &dest);

  IRLiveInterval *iv = tcc_ir_vreg_live_interval(ir, t0);
  UT_ASSERT_EQ(iv->is_lvalue, 0);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_put_lea_marks_src1_addrtaken)
{
  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);
  int v0 = tcc_ir_vreg_alloc_var(ir);

  SValue src1 = sv_var(v0);
  SValue dest = sv_var(t0);
  tcc_ir_put(ir, TCCIR_OP_LEA, &src1, NULL, &dest);

  IRLiveInterval *iv = tcc_ir_vreg_live_interval(ir, v0);
  UT_ASSERT_EQ(iv->addrtaken, 1);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_put_funccallval_marks_nonleaf)
{
  TCCIRState *ir = tcc_ir_alloc();
  UT_ASSERT(tcc_ir_is_leaf(ir));

  int t0 = tcc_ir_vreg_alloc_temp(ir);
  SValue func = sv_const(0);
  SValue call_info = sv_const((int)TCCIR_ENCODE_CALL(0, 0));
  SValue dest = sv_var(t0);
  int idx = tcc_ir_put(ir, TCCIR_OP_FUNCCALLVAL, &func, &call_info, &dest);

  UT_ASSERT(!tcc_ir_is_leaf(ir));
  UT_ASSERT_EQ(idx, 0);
  UT_ASSERT_EQ(tcc_ir_count(ir), 1);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_put_suppressed_by_nocode_wanted)
{
  extern int nocode_wanted;
  int saved = nocode_wanted;

  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);

  /* Any bit other than CODE_OFF_BIT (0x20000000) suppresses IR emission
   * entirely: tcc_ir_put returns -1 without touching the instruction count. */
  nocode_wanted = 0x1;
  SValue src = sv_const(1);
  SValue dest = sv_var(t0);
  int idx = tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src, NULL, &dest);

  UT_ASSERT_EQ(idx, -1);
  UT_ASSERT_EQ(tcc_ir_count(ir), 0);

  nocode_wanted = saved;
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_put_not_suppressed_by_code_off_bit_alone)
{
  extern int nocode_wanted;
  int saved = nocode_wanted;

  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);

  /* CODE_OFF_BIT (0x20000000) alone must NOT suppress emission: dead-code
   * regions after return/break/goto still need IR for jump-target
   * backpatching (see the comment at the top of tcc_ir_put). */
  nocode_wanted = 0x20000000;
  SValue src = sv_const(1);
  SValue dest = sv_var(t0);
  int idx = tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src, NULL, &dest);

  UT_ASSERT_EQ(idx, 0);
  UT_ASSERT_EQ(tcc_ir_count(ir), 1);

  nocode_wanted = saved;
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_put_no_op_zero_line_num_when_file_is_null)
{
  /* stubs.c defines `struct BufferedFile *file = NULL;` for the unit-test
   * build.  tcc_ir_put does `cq->line_num = file ? file->line_num : 0;` --
   * with no file open, every emitted instruction must record line 0. */
  TCCIRState *ir = tcc_ir_alloc();
  int idx = tcc_ir_put_no_op(ir, TCCIR_OP_NOP);
  UT_ASSERT_EQ(ir->compact_instructions[idx].line_num, 0u);
  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Inline assembly bookkeeping (tcc_ir_asm_add / tcc_ir_asm_put)              */
/* -------------------------------------------------------------------------- */

#ifdef CONFIG_TCC_ASM
UT_TEST(test_asm_add_stores_operands_and_marks_nonleaf)
{
  TCCIRState *ir = tcc_ir_alloc();
  UT_ASSERT(tcc_ir_is_leaf(ir));

  SValue val;
  svalue_init(&val);
  val.r = VT_CONST;
  val.c.i = 5;
  val.type.t = VT_INT;

  ASMOperand operands[1];
  memset(&operands[0], 0, sizeof(operands[0]));
  operands[0].vt = &val;
  strcpy(operands[0].constraint, "r");

  const char asm_str[] = "nop";
  int id = tcc_ir_asm_add(ir, asm_str, (int)(sizeof(asm_str) - 1), 0, operands, 1, 0, 0, NULL);

  UT_ASSERT_EQ(id, 0);
  UT_ASSERT_EQ(ir->inline_asm_count, 1);
  UT_ASSERT_EQ(ir->inline_asms[0].asm_len, (int)(sizeof(asm_str) - 1));
  UT_ASSERT_EQ(ir->inline_asms[0].nb_operands, 1);
  UT_ASSERT_EQ(ir->inline_asms[0].values[0].c.i, 5);
  /* Inline asm is conservatively treated as call-like. */
  UT_ASSERT(!tcc_ir_is_leaf(ir));

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_asm_put_emits_inline_asm_instruction)
{
  TCCIRState *ir = tcc_ir_alloc();

  SValue val;
  svalue_init(&val);
  val.r = VT_CONST;
  val.type.t = VT_INT;

  ASMOperand operands[1];
  memset(&operands[0], 0, sizeof(operands[0]));
  operands[0].vt = &val;

  const char asm_str[] = "wfi";
  int id = tcc_ir_asm_add(ir, asm_str, (int)(sizeof(asm_str) - 1), 0, operands, 1, 0, 0, NULL);

  tcc_ir_asm_put(ir, id);

  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  UT_ASSERT_EQ(ir->compact_instructions[0].op, TCCIR_OP_INLINE_ASM);
  UT_ASSERT(!tcc_ir_is_leaf(ir));

  tcc_ir_free(ir);
  return 0;
}
#endif /* CONFIG_TCC_ASM */

/* -------------------------------------------------------------------------- */
/* irop_config shape                                                          */
/* -------------------------------------------------------------------------- */

UT_TEST(test_irop_config_shapes)
{
  UT_ASSERT(irop_config[TCCIR_OP_ADD].has_dest);
  UT_ASSERT(irop_config[TCCIR_OP_ADD].has_src1);
  UT_ASSERT(irop_config[TCCIR_OP_ADD].has_src2);

  UT_ASSERT(!irop_config[TCCIR_OP_NOP].has_dest);
  UT_ASSERT(!irop_config[TCCIR_OP_NOP].has_src1);
  UT_ASSERT(!irop_config[TCCIR_OP_NOP].has_src2);

  UT_ASSERT(!irop_config[TCCIR_OP_RETURNVALUE].has_dest);
  UT_ASSERT(irop_config[TCCIR_OP_RETURNVALUE].has_src1);
  UT_ASSERT(!irop_config[TCCIR_OP_RETURNVALUE].has_src2);

  UT_ASSERT(irop_config[TCCIR_OP_JUMP].has_dest);
  UT_ASSERT(!irop_config[TCCIR_OP_JUMP].has_src1);
  UT_ASSERT(!irop_config[TCCIR_OP_JUMP].has_src2);

  UT_ASSERT(irop_config[TCCIR_OP_STORE].has_dest);
  UT_ASSERT(irop_config[TCCIR_OP_STORE].has_src1);
  UT_ASSERT(!irop_config[TCCIR_OP_STORE].has_src2);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Suite                                                                      */
/* -------------------------------------------------------------------------- */

UT_SUITE(ir_core)
{
  UT_RUN(test_alloc_fresh_block_has_zero_instructions);
  UT_RUN(test_put_add_packs_operands);
  UT_RUN(test_put_no_op_has_no_operands);
  UT_RUN(test_count_and_current_idx);
  UT_RUN(test_leaf_by_default);
  UT_RUN(test_call_marks_nonleaf);
  UT_RUN(test_nonleaf_mark_explicit);
  UT_RUN(test_call_id_next_monotonic);
  UT_RUN(test_set_dest_roundtrip);
  UT_RUN(test_coalesce_assign_from_prev_temp_dest);
  UT_RUN(test_coalesce_skipped_when_src1_is_var_not_temp);
  UT_RUN(test_coalesce_skipped_at_basic_block_start);
  UT_RUN(test_coalesce_skipped_when_prevent_coalescing_set);
  UT_RUN(test_coalesce_skipped_when_src1_is_lval);
  UT_RUN(test_coalesce_skipped_when_src1_vreg_mismatches_prev_dest);
  UT_RUN(test_coalesce_width_mismatch_blocks_coalescing);
  UT_RUN(test_coalesce_into_real_stack_slot_sets_stackoff_tag);
  UT_RUN(test_backpatch_to_here);
  UT_RUN(test_backpatch_walks_multi_link_chain);
  UT_RUN(test_backpatch_negative_chain_is_noop);
  UT_RUN(test_backpatch_stops_at_non_jump_instruction);
  UT_RUN(test_backpatch_first_patches_only_last_link);
  UT_RUN(test_backpatch_first_negative_chain_is_noop);
  UT_RUN(test_gjmp_append_links_new_chain_onto_existing);
  UT_RUN(test_gjmp_append_returns_t_when_n_out_of_range);
  UT_RUN(test_utility_functions_null_safe);
  UT_RUN(test_irop_from_token_arithmetic);
  UT_RUN(test_irop_from_token_carry_and_wide_mul);
  UT_RUN(test_irop_from_token_shifts);
  UT_RUN(test_irop_from_token_all_comparisons_map_to_cmp);
  UT_RUN(test_put_infers_dest_type_from_untyped_dest_and_src1);
  UT_RUN(test_put_assign_marks_dest_lvalue_when_src1_is_plain_value);
  UT_RUN(test_put_assign_does_not_mark_lvalue_when_src1_is_stack_addr);
  UT_RUN(test_put_lea_marks_src1_addrtaken);
  UT_RUN(test_put_funccallval_marks_nonleaf);
  UT_RUN(test_put_suppressed_by_nocode_wanted);
  UT_RUN(test_put_not_suppressed_by_code_off_bit_alone);
  UT_RUN(test_put_no_op_zero_line_num_when_file_is_null);
#ifdef CONFIG_TCC_ASM
  UT_RUN(test_asm_add_stores_operands_and_marks_nonleaf);
  UT_RUN(test_asm_put_emits_inline_asm_instruction);
#endif
  UT_RUN(test_irop_config_shapes);
}
