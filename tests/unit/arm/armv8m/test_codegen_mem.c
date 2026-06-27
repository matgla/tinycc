/*
 *  test_codegen_mem.c - backend unit tests for memory IR ops
 *
 *  Covers LOAD/STORE/LEA/LOAD_INDEXED/STORE_INDEXED operand lowering through
 *  machine_op_from_ir() and the codegen helper backpatch routines.
 */

#define USING_GLOBALS
#include "ir.h"
#include "cfg.h"
#include "ir/ssa.h"
#include "ir/vreg.h"
#include "ir/regalloc.h"
#include "ir/codegen.h"
#include "ir/machine_op.h"
#include "arch/arm/arm_regalloc.h"
#include "ut.h"

static SValue sv_var(int vreg, int vt)
{
  SValue sv;
  svalue_init(&sv);
  sv.vr = vreg;
  sv.type.t = vt;
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

static void setup_tcc_state(void)
{
  tcc_state->registers_for_allocator = 13;
  tcc_state->registers_map_for_allocator = (1ull << 13) - 1;
  tcc_state->float_abi = ARM_HARD_FLOAT;
  tcc_state->float_registers_for_allocator = 32;
  tcc_state->float_registers_map_for_allocator = (1ull << 32) - 1;
  tcc_state->optimize = 0;
}

static SValue lval_of(SValue sv)
{
  sv.r |= VT_LVAL;
  return sv;
}

/* -------------------------------------------------------------------------- */
/* LOAD / STORE lowering                                                      */
/* -------------------------------------------------------------------------- */

UT_TEST(test_load_store_lowering)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int ptr = tcc_ir_vreg_alloc_var(ir);
  int val = tcc_ir_vreg_alloc_temp(ir);
  int loaded = tcc_ir_vreg_alloc_temp(ir);

  SValue s_ptr = sv_var(ptr, VT_PTR);
  SValue s_val = sv_var(val, VT_INT);
  SValue s_loaded = sv_var(loaded, VT_INT);
  SValue s_const = sv_const(42);

  SValue s_ptr_lval = lval_of(s_ptr);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_const, NULL, &s_val);
  tcc_ir_put(ir, TCCIR_OP_STORE, &s_val, NULL, &s_ptr_lval);
  tcc_ir_put(ir, TCCIR_OP_LOAD, &s_ptr_lval, NULL, &s_loaded);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_loaded, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);

  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_LOAD)
    {
      IROperand src = tcc_ir_codegen_src1_get(ir, q);
      IROperand dst = tcc_ir_codegen_dest_get(ir, q);
      MachineOperand ms = machine_op_from_ir(ir, &src);
      MachineOperand md = machine_op_from_ir(ir, &dst);
      UT_ASSERT(ms.needs_deref);
      UT_ASSERT(md.kind == MACH_OP_REG || md.kind == MACH_OP_SPILL);
    }
    else if (q->op == TCCIR_OP_STORE)
    {
      IROperand src = tcc_ir_codegen_src1_get(ir, q);
      IROperand dst = tcc_ir_codegen_dest_get(ir, q);
      MachineOperand ms = machine_op_from_ir(ir, &src);
      MachineOperand md = machine_op_from_ir(ir, &dst);
      UT_ASSERT(ms.kind == MACH_OP_REG);
      UT_ASSERT(md.needs_deref);
    }
  }

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* LEA produces a frame/symbol address without dereference                    */
/* -------------------------------------------------------------------------- */

UT_TEST(test_lea_lowering)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int local = tcc_ir_vreg_alloc_var(ir);
  int addr = tcc_ir_vreg_alloc_temp(ir);

  SValue s_local = sv_var(local, VT_INT);
  SValue s_addr = sv_var(addr, VT_PTR);

  tcc_ir_put(ir, TCCIR_OP_LEA, &s_local, NULL, &s_addr);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_addr, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);

  int lea_idx = -1;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_LEA)
    {
      lea_idx = i;
      break;
    }
  }
  UT_ASSERT(lea_idx >= 0);

  IRQuadCompact *q = &ir->compact_instructions[lea_idx];
  IROperand src = tcc_ir_codegen_src1_get(ir, q);
  IROperand dst = tcc_ir_codegen_dest_get(ir, q);

  MachineOperand ms = machine_op_from_ir(ir, &src);
  MachineOperand md = machine_op_from_ir(ir, &dst);

  /* LEA source should be a local address; destination a register. */
  UT_ASSERT(md.kind == MACH_OP_REG);
  if (ms.kind == MACH_OP_FRAME_ADDR || ms.kind == MACH_OP_SPILL)
  {
    UT_ASSERT(!ms.needs_deref);
  }

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* LOAD_INDEXED / STORE_INDEXED operand layout                                */
/* -------------------------------------------------------------------------- */

UT_TEST(test_indexed_memory_layout)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int base = tcc_ir_vreg_alloc_var(ir);
  int idx = tcc_ir_vreg_alloc_temp(ir);
  int val = tcc_ir_vreg_alloc_temp(ir);
  int loaded = tcc_ir_vreg_alloc_temp(ir);

  SValue s_idx = sv_var(idx, VT_INT);
  SValue s_val = sv_var(val, VT_INT);
  SValue s_loaded = sv_var(loaded, VT_INT);
  SValue s_const2 = sv_const(2);
  SValue s_const99 = sv_const(99);

  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_const2, NULL, &s_idx);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_const99, NULL, &s_val);

  /* Build LOAD_INDEXED manually: operands are dest, base, index, scale. */
  int pool_base = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, irop_make_vreg(loaded, IROP_BTYPE_INT32));          /* dest */
  tcc_ir_pool_add(ir, irop_make_vreg(base, IROP_BTYPE_INT32));           /* base */
  tcc_ir_pool_add(ir, irop_make_vreg(idx, IROP_BTYPE_INT32));            /* index */
  tcc_ir_pool_add(ir, irop_make_imm32(-1, 2, IROP_BTYPE_INT32));         /* scale = 4 */

  int load_idx = ir->next_instruction_index;
  IRQuadCompact *q = &ir->compact_instructions[load_idx];
  q->op = TCCIR_OP_LOAD_INDEXED;
  q->operand_base = pool_base;
  ir->next_instruction_index++;

  /* Build STORE_INDEXED: operands are dest(address), src, index, scale. */
  int pool_base2 = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, irop_make_vreg(base, IROP_BTYPE_INT32));           /* address */
  tcc_ir_pool_add(ir, irop_make_vreg(val, IROP_BTYPE_INT32));            /* value */
  tcc_ir_pool_add(ir, irop_make_vreg(idx, IROP_BTYPE_INT32));            /* index */
  tcc_ir_pool_add(ir, irop_make_imm32(-1, 2, IROP_BTYPE_INT32));         /* scale = 4 */

  int store_idx = ir->next_instruction_index;
  IRQuadCompact *q2 = &ir->compact_instructions[store_idx];
  q2->op = TCCIR_OP_STORE_INDEXED;
  q2->operand_base = pool_base2;
  ir->next_instruction_index++;

  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_loaded, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);

  /* Inspect the indexed instructions. */
  IRQuadCompact *lq = &ir->compact_instructions[load_idx];
  IRQuadCompact *sq = &ir->compact_instructions[store_idx];

  UT_ASSERT_EQ(lq->op, TCCIR_OP_LOAD_INDEXED);
  UT_ASSERT_EQ(sq->op, TCCIR_OP_STORE_INDEXED);

  /* tcc_ir_codegen_src2_get should return the index operand. */
  IROperand lidx = tcc_ir_codegen_src2_get(ir, lq);
  IROperand sidx = tcc_ir_codegen_src2_get(ir, sq);
  UT_ASSERT(irop_has_vreg(lidx));
  UT_ASSERT(irop_has_vreg(sidx));

  /* Scale lives at operand_base + 3. */
  IROperand lscale = tcc_ir_op_get_scale(ir, lq);
  IROperand sscale = tcc_ir_op_get_scale(ir, sq);
  UT_ASSERT(irop_is_immediate(lscale));
  UT_ASSERT(irop_is_immediate(sscale));
  UT_ASSERT_EQ(irop_get_imm32(lscale), 2);
  UT_ASSERT_EQ(irop_get_imm32(sscale), 2);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Backpatch helpers for control-flow embedded in memory tests                */
/* -------------------------------------------------------------------------- */

UT_TEST(test_codegen_backpatch_roundtrip)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int v = tcc_ir_vreg_alloc_temp(ir);
  SValue s_v = sv_var(v, VT_INT);
  SValue s_one = sv_const(1);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_v);

  SValue jtarget;
  svalue_init(&jtarget);
  jtarget.vr = -1;
  jtarget.r = VT_CONST;
  jtarget.c.i = -1;

  /* Two independent unresolved jumps. */
  int j1 = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &jtarget);
  int j2 = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &jtarget);

  int here = ir->next_instruction_index;
  tcc_ir_codegen_backpatch(ir, j1, here);
  tcc_ir_codegen_backpatch_here(ir, j2);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_v, NULL, NULL);

  UT_ASSERT_EQ(tcc_ir_op_get_dest(ir, &ir->compact_instructions[j1]).u.imm32, here);
  UT_ASSERT_EQ(tcc_ir_op_get_dest(ir, &ir->compact_instructions[j2]).u.imm32, here);

  /* Build a separate chain j3 -> j4 -> -1 and patch through the head. */
  int j3 = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &jtarget);
  int j4 = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &jtarget);
  int chain = tcc_ir_codegen_jump_append(ir, j3, j4);
  tcc_ir_codegen_backpatch_first(ir, chain, here);
  UT_ASSERT_EQ(tcc_ir_op_get_dest(ir, &ir->compact_instructions[j4]).u.imm32, here);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Suite                                                                      */
/* -------------------------------------------------------------------------- */

UT_SUITE(codegen_mem)
{
  UT_RUN(test_load_store_lowering);
  UT_RUN(test_lea_lowering);
  UT_RUN(test_indexed_memory_layout);
  UT_RUN(test_codegen_backpatch_roundtrip);
}
