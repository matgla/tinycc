/*
 *  test_codegen_mem.c - backend unit tests for memory IR ops
 *
 *  Covers LOAD/STORE/LEA/LOAD_INDEXED/STORE_INDEXED operand lowering through
 *  machine_op_from_ir() and the codegen helper backpatch routines.
 */

#define USING_GLOBALS
#include "ir.h"
#include "cfg.h"
#include "source/ir/ssa.h"
#include "source/ir/vreg.h"
#include "source/ir/regalloc.h"
#include "source/ir/codegen.h"
#include "source/ir/machine_op.h"
#include "source/backend/arch/arm/arm_regalloc.h"
#include "codegen_mop_stubs.h"
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

/* Hand-builds a byte-sized STORE_INDEXED with an immediate value and an
 * immediate (compile-time-constant) byte offset -- the shape the byte-to-word
 * coalescing peephole (ir/codegen.c ~3650-3792) looks for. Pool layout
 * mirrors test_indexed_memory_layout's STORE_INDEXED: address, value, index,
 * scale; here index doubles as the literal byte offset since it's an
 * immediate rather than a register, and scale=0 marks "no register scaling". */
static int emit_byte_store_indexed(TCCIRState *ir, int base_vreg, int byte_val, int32_t offset)
{
  int pool_base = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, irop_make_vreg(base_vreg, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_imm32(-1, byte_val, IROP_BTYPE_INT8));
  tcc_ir_pool_add(ir, irop_make_imm32(-1, offset, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_imm32(-1, 0, IROP_BTYPE_INT32));
  int idx = ir->next_instruction_index;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  q->op = TCCIR_OP_STORE_INDEXED;
  q->operand_base = pool_base;
  ir->next_instruction_index++;
  return idx;
}

/* Word-sized (32-bit) STORE_INDEXED with a register-resident value and an
 * immediate (compile-time-constant) offset -- the shape the REG-source STRD
 * pairing peephole (ir/codegen.c ~3494-3582) looks for. */
static int emit_reg_store_indexed(TCCIRState *ir, int base_vreg, int value_vreg, int32_t offset)
{
  int pool_base = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, irop_make_vreg(base_vreg, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_vreg(value_vreg, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_imm32(-1, offset, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_imm32(-1, 0, IROP_BTYPE_INT32));
  int idx = ir->next_instruction_index;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  q->op = TCCIR_OP_STORE_INDEXED;
  q->operand_base = pool_base;
  ir->next_instruction_index++;
  return idx;
}

/* Word-sized (32-bit) STORE_INDEXED with an immediate value and an immediate
 * offset -- the shape the IMM-source STRD pairing peephole (ir/codegen.c
 * ~3584-3648) looks for; distinct from emit_byte_store_indexed's INT8 value
 * (which instead trips the byte-to-word coalescing peephole below it). */
static int emit_imm32_store_indexed(TCCIRState *ir, int base_vreg, int32_t value, int32_t offset)
{
  int pool_base = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, irop_make_vreg(base_vreg, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_imm32(-1, value, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_imm32(-1, offset, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_imm32(-1, 0, IROP_BTYPE_INT32));
  int idx = ir->next_instruction_index;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  q->op = TCCIR_OP_STORE_INDEXED;
  q->operand_base = pool_base;
  ir->next_instruction_index++;
  return idx;
}

/* Hand-builds a raw stackoff operand representing a plain (non-llocal) spill
 * slot: vr=-1, is_lval=1 (so machine_op_from_ir yields MACH_OP_SPILL rather
 * than MACH_OP_FRAME_ADDR), is_llocal=0 (so needs_deref comes out false --
 * the double-indirection llocal case is test_codegen_atomic.c's concern, not
 * this one). Mirrors test_atomic_style_llocal_operand's construction, minus
 * the llocal flag. */
static IROperand spill_slot_op(int32_t offset)
{
  return irop_make_stackoff(-1, offset, 1, 0, 0, IROP_BTYPE_INT32);
}

/* dest = spill slot, src1 = plain register value -- the shape both the
 * TCCIR_OP_STORE spill-slot STRD peephole (ir/codegen.c ~3143-3202) and the
 * TCCIR_OP_ASSIGN register->spill STRD peephole (~3912-3967) look for. */
static int emit_reg_to_spill(TCCIRState *ir, TccIrOp op, int value_vreg, int32_t spill_offset)
{
  int pool_base = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, spill_slot_op(spill_offset));
  tcc_ir_pool_add(ir, irop_make_vreg(value_vreg, IROP_BTYPE_INT32));
  int idx = ir->next_instruction_index;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  q->op = op;
  q->operand_base = pool_base;
  ir->next_instruction_index++;
  return idx;
}

/* dest = spill slot, src1 = *ptr_vreg (a register operand with needs_deref
 * set) -- the shape docs/bugs.md #1b's fix guards against: fusing this into
 * STRD would feed ptr_vreg's register to try_strd_spill as if it were the
 * value, silently dropping the dereference. Only meaningful for
 * TCCIR_OP_STORE (ASSIGN has no "value = *ptr" construction). */
static int emit_deref_to_spill(TCCIRState *ir, int32_t spill_offset, int ptr_vreg)
{
  int pool_base = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, spill_slot_op(spill_offset));
  IROperand deref_src = irop_make_vreg(ptr_vreg, IROP_BTYPE_INT32);
  deref_src.is_lval = 1;
  tcc_ir_pool_add(ir, deref_src);
  int idx = ir->next_instruction_index;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  q->op = TCCIR_OP_STORE;
  q->operand_base = pool_base;
  ir->next_instruction_index++;
  return idx;
}

/* dest = spill slot, src1 = immediate -- the IMM-source STRD-to-spill shape
 * (TCCIR_OP_STORE ~3204-3258). Per docs/bugs.md, an IMM value operand can
 * never carry needs_deref (ir/machine_op.c's immediate tags return early
 * without ever setting it), so there's no deref-guard regression case here. */
static int emit_imm_to_spill(TCCIRState *ir, TccIrOp op, int32_t value, int32_t spill_offset)
{
  int pool_base = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, spill_slot_op(spill_offset));
  tcc_ir_pool_add(ir, irop_make_imm32(-1, value, IROP_BTYPE_INT32));
  int idx = ir->next_instruction_index;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  q->op = op;
  q->operand_base = pool_base;
  ir->next_instruction_index++;
  return idx;
}

/* dest = plain register, src1 = spill slot -- the ASSIGN-based
 * LDRD-from-spill peephole (ir/codegen.c ~3855-3910) looks for this shape. */
static int emit_spill_to_reg(TCCIRState *ir, int32_t spill_offset, int dest_vreg)
{
  int pool_base = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, irop_make_vreg(dest_vreg, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, spill_slot_op(spill_offset));
  int idx = ir->next_instruction_index;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  q->op = TCCIR_OP_ASSIGN;
  q->operand_base = pool_base;
  ir->next_instruction_index++;
  return idx;
}

/* dest, base, index(=offset immediate), scale=0 -- the LOAD_INDEXED
 * LDRD-pairing shape (ir/codegen.c ~3415-3486, try_ldrd_base). */
static int emit_reg_load_indexed(TCCIRState *ir, int dest_vreg, int base_vreg, int32_t offset)
{
  int pool_base = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, irop_make_vreg(dest_vreg, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_vreg(base_vreg, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_imm32(-1, offset, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_imm32(-1, 0, IROP_BTYPE_INT32));
  int idx = ir->next_instruction_index;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  q->op = TCCIR_OP_LOAD_INDEXED;
  q->operand_base = pool_base;
  ir->next_instruction_index++;
  return idx;
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

/* ============================================================================
 * Dispatch-level tests (tcc_ir_codegen_generate)
 *
 * See test_codegen_arith.c's dispatch-level section header for the overall
 * rationale. LOAD/STORE/LEA/LOAD_INDEXED/STORE_INDEXED each have their own
 * dedicated (non-fallthrough) case label in ir/codegen.c (~3013/3136/3990/
 * 3415/3490 respectively), each ending in exactly one mop call for the shapes
 * built here. BLOCK_COPY (needs a real Sym* this bare harness can't build --
 * see stubs.c's always-NULL sym_push2/external_global_sym) and
 * LOAD_POSTINC/STORE_POSTINC (post-increment opcodes still lowered by
 * ir/codegen.c but no longer produced by any optimizer pass) are documented
 * gaps, left uncovered here -- see docs/plan_codegen_unit_tests.md.
 * ============================================================================ */

UT_TEST(test_dispatch_load_store_route_to_mops)
{
  cgstub_reset();
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
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("store_mop"), 1);
  UT_ASSERT_EQ(cgstub_call_count("load_mop"), 1);
  const CgStubCall *st = cgstub_nth_call("store_mop", 0);
  const CgStubCall *ld = cgstub_nth_call("load_mop", 0);
  UT_ASSERT(st != NULL && ld != NULL);
  UT_ASSERT_EQ(st->ir_op, TCCIR_OP_STORE);
  UT_ASSERT_EQ(ld->ir_op, TCCIR_OP_LOAD);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_lea_routes_to_lea_mop)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int local = tcc_ir_vreg_alloc_var(ir);
  int addr = tcc_ir_vreg_alloc_temp(ir);

  SValue s_local = sv_var(local, VT_INT);
  SValue s_addr = sv_var(addr, VT_PTR);

  tcc_ir_put(ir, TCCIR_OP_LEA, &s_local, NULL, &s_addr);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_addr, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("lea_mop"), 1);
  const CgStubCall *c = cgstub_nth_call("lea_mop", 0);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->dest_kind, MACH_OP_REG);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_indexed_memory_routes_to_indexed_mops)
{
  cgstub_reset();
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

  /* LOAD_INDEXED: dest, base, index, scale. */
  int pool_base = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, irop_make_vreg(loaded, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_vreg(base, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_vreg(idx, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_imm32(-1, 2, IROP_BTYPE_INT32));
  int load_idx = ir->next_instruction_index;
  IRQuadCompact *lq = &ir->compact_instructions[load_idx];
  lq->op = TCCIR_OP_LOAD_INDEXED;
  lq->operand_base = pool_base;
  ir->next_instruction_index++;

  /* STORE_INDEXED: address, value, index, scale. */
  int pool_base2 = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, irop_make_vreg(base, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_vreg(val, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_vreg(idx, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_imm32(-1, 2, IROP_BTYPE_INT32));
  int store_idx = ir->next_instruction_index;
  IRQuadCompact *sq = &ir->compact_instructions[store_idx];
  sq->op = TCCIR_OP_STORE_INDEXED;
  sq->operand_base = pool_base2;
  ir->next_instruction_index++;

  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_loaded, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("load_indexed_mop"), 1);
  UT_ASSERT_EQ(cgstub_call_count("store_indexed_mop"), 1);
  const CgStubCall *lc = cgstub_nth_call("load_indexed_mop", 0);
  const CgStubCall *sc = cgstub_nth_call("store_indexed_mop", 0);
  UT_ASSERT(lc != NULL && sc != NULL);
  UT_ASSERT_EQ(lc->ir_op, TCCIR_OP_LOAD_INDEXED);
  UT_ASSERT_EQ(sc->ir_op, TCCIR_OP_STORE_INDEXED);

  tcc_ir_free(ir);
  return 0;
}

/* ============================================================================
 * Byte-to-word store coalescing peephole (ir/codegen.c ~3650-3792)
 *
 * Four consecutive byte STORE_INDEXEDs with immediate values, to word-aligned
 * consecutive offsets off the same base register, collapse into a single
 * 32-bit store_indexed_mop of the packed constant (little-endian: byte at
 * offset+0 is the LSB). Eight consecutive bytes collapse into two 32-bit
 * stores (NOT one STRD -- these originate from byte writes so the base may
 * be unaligned; STRD always faults on unaligned access, a plain STR
 * tolerates it, see the comment at ir/codegen.c ~3761-3769).
 * ============================================================================ */

UT_TEST(test_dispatch_store_indexed_four_bytes_coalesce_into_one_word_store)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int base = tcc_ir_vreg_alloc_var(ir);

  emit_byte_store_indexed(ir, base, 0x11, 0);
  emit_byte_store_indexed(ir, base, 0x22, 1);
  emit_byte_store_indexed(ir, base, 0x33, 2);
  emit_byte_store_indexed(ir, base, 0x44, 3);

  SValue s_base = sv_var(base, VT_PTR);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_base, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("store_indexed_mop"), 1);
  const CgStubCall *c = cgstub_nth_call("store_indexed_mop", 0);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->dest_kind, MACH_OP_REG);  /* base */
  UT_ASSERT_EQ(c->src1_kind, MACH_OP_IMM);  /* offset (0) */
  UT_ASSERT_EQ(c->src2_kind, MACH_OP_IMM);  /* merged word value */
  UT_ASSERT_EQ(c->aux0, 0x44332211);        /* bytes packed little-endian */

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_store_indexed_eight_bytes_coalesce_into_two_word_stores)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int base = tcc_ir_vreg_alloc_var(ir);

  emit_byte_store_indexed(ir, base, 0x11, 0);
  emit_byte_store_indexed(ir, base, 0x22, 1);
  emit_byte_store_indexed(ir, base, 0x33, 2);
  emit_byte_store_indexed(ir, base, 0x44, 3);
  emit_byte_store_indexed(ir, base, 0x55, 4);
  emit_byte_store_indexed(ir, base, 0x66, 5);
  emit_byte_store_indexed(ir, base, 0x77, 6);
  emit_byte_store_indexed(ir, base, 0x88, 7);

  SValue s_base = sv_var(base, VT_PTR);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_base, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("store_indexed_mop"), 2);
  const CgStubCall *c0 = cgstub_nth_call("store_indexed_mop", 0);
  const CgStubCall *c1 = cgstub_nth_call("store_indexed_mop", 1);
  UT_ASSERT(c0 != NULL && c1 != NULL);
  UT_ASSERT_EQ(c0->aux0, 0x44332211);
  /* 0x88776655 > INT32_MAX, so both sides must be cast the same way (as
   * `int`) before UT_ASSERT_EQ widens to `long long` -- otherwise the
   * unsigned literal zero-extends while aux0 (already `int`) sign-extends,
   * a spurious mismatch unrelated to the peephole itself. */
  UT_ASSERT_EQ(c1->aux0, (int)0x88776655u);

  tcc_ir_free(ir);
  return 0;
}

/* Three consecutive bytes (one short of the four needed) must NOT coalesce --
 * confirms the peephole requires the full run, not a partial merge. */
UT_TEST(test_dispatch_store_indexed_three_bytes_do_not_coalesce)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int base = tcc_ir_vreg_alloc_var(ir);

  emit_byte_store_indexed(ir, base, 0x11, 0);
  emit_byte_store_indexed(ir, base, 0x22, 1);
  emit_byte_store_indexed(ir, base, 0x33, 2);

  SValue s_base = sv_var(base, VT_PTR);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_base, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("store_indexed_mop"), 3);

  tcc_ir_free(ir);
  return 0;
}

/* ============================================================================
 * STRD pairing "attempt" for STORE_INDEXED (ir/codegen.c ~3494-3648)
 *
 * Two adjacent 32-bit STORE_INDEXEDs to the same base, word-aligned offsets
 * 4 apart, trigger a try_strd_base/try_strd_imm_base call. codegen_mop_stubs.c
 * stubs these to always return 0 (documented in
 * docs/plan_codegen_unit_tests.md §1/§7: no IR shape can exercise the real
 * fused encoding without reimplementing it inside the stub), so the pairing
 * never "lands" -- both stores still emit individually. These tests assert
 * the attempt itself: the peephole recognizes the shape and calls the
 * try_strd_* helper exactly once, distinct from testing a successful pairing.
 * ============================================================================ */

UT_TEST(test_dispatch_store_indexed_reg_pair_attempts_strd_base)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int base = tcc_ir_vreg_alloc_var(ir);
  int v1 = tcc_ir_vreg_alloc_temp(ir);
  int v2 = tcc_ir_vreg_alloc_temp(ir);
  SValue s_v1 = sv_var(v1, VT_INT);
  SValue s_v2 = sv_var(v2, VT_INT);
  SValue s_c1 = sv_const(0x1111);
  SValue s_c2 = sv_const(0x2222);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_c1, NULL, &s_v1);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_c2, NULL, &s_v2);

  emit_reg_store_indexed(ir, base, v1, 0);
  emit_reg_store_indexed(ir, base, v2, 4);

  SValue s_base = sv_var(base, VT_PTR);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_base, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("try_strd_base"), 1);
  /* Stub returns 0 -- pairing never lands, both stores still emit. */
  UT_ASSERT_EQ(cgstub_call_count("store_indexed_mop"), 2);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_store_indexed_imm32_pair_attempts_strd_imm_base)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int base = tcc_ir_vreg_alloc_var(ir);

  emit_imm32_store_indexed(ir, base, 0x11111111, 0);
  emit_imm32_store_indexed(ir, base, 0x22222222, 4);

  SValue s_base = sv_var(base, VT_PTR);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_base, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("try_strd_imm_base"), 1);
  UT_ASSERT_EQ(cgstub_call_count("store_indexed_mop"), 2);

  tcc_ir_free(ir);
  return 0;
}

/* ============================================================================
 * STRD/LDRD "attempt" tests for the remaining sites: TCCIR_OP_STORE's
 * spill-slot and deref-through-vreg forms, LOAD_INDEXED, and ASSIGN's
 * spill<->reg forms (ir/codegen.c ~3143-3202, ~3260-3355, ~3415-3486,
 * ~3855-3967). Same "record the attempt, stub returns 0, falls back to two
 * plain ops" discipline as the STORE_INDEXED attempt tests above.
 *
 * The STORE-opcode forms were the site of a real bug fixed in docs/bugs.md #1:
 * a missing `!src1.needs_deref` guard let a pending pointer dereference
 * (`slot = *ptr`) get silently fused as if the pointer register were the
 * value. The regression tests below assert the *fix*: a deref-valued store
 * must not even attempt the pairing.
 * ============================================================================ */

UT_TEST(test_dispatch_store_spill_reg_pair_attempts_strd_spill)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int v1 = tcc_ir_vreg_alloc_temp(ir);
  int v2 = tcc_ir_vreg_alloc_temp(ir);
  SValue s_v1 = sv_var(v1, VT_INT);
  SValue s_v2 = sv_var(v2, VT_INT);
  SValue s_c1 = sv_const(0x1111);
  SValue s_c2 = sv_const(0x2222);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_c1, NULL, &s_v1);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_c2, NULL, &s_v2);

  emit_reg_to_spill(ir, TCCIR_OP_STORE, v1, 0);
  emit_reg_to_spill(ir, TCCIR_OP_STORE, v2, 4);

  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_v1, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("try_strd_spill"), 1);
  UT_ASSERT_EQ(cgstub_call_count("store_mop"), 2);

  tcc_ir_free(ir);
  return 0;
}

/* Regression for docs/bugs.md #1b: the *second* store's value is `*ptr`
 * (needs_deref) -- must not even attempt the pairing. */
UT_TEST(test_dispatch_store_spill_second_deref_value_blocks_strd_spill)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int v1 = tcc_ir_vreg_alloc_temp(ir);
  int ptr = tcc_ir_vreg_alloc_temp(ir);
  SValue s_v1 = sv_var(v1, VT_INT);
  SValue s_ptr = sv_var(ptr, VT_PTR);
  SValue s_c1 = sv_const(0x1111);
  SValue s_addr = sv_const(0x2000);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_c1, NULL, &s_v1);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_addr, NULL, &s_ptr);

  emit_reg_to_spill(ir, TCCIR_OP_STORE, v1, 0);
  emit_deref_to_spill(ir, 4, ptr);

  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_v1, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("try_strd_spill"), 0);
  UT_ASSERT_EQ(cgstub_call_count("store_mop"), 2);

  tcc_ir_free(ir);
  return 0;
}

/* Regression, other side: the *first* store's value is `*ptr`. */
UT_TEST(test_dispatch_store_spill_first_deref_value_blocks_strd_spill)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int v2 = tcc_ir_vreg_alloc_temp(ir);
  int ptr = tcc_ir_vreg_alloc_temp(ir);
  SValue s_v2 = sv_var(v2, VT_INT);
  SValue s_ptr = sv_var(ptr, VT_PTR);
  SValue s_c2 = sv_const(0x2222);
  SValue s_addr = sv_const(0x2000);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_addr, NULL, &s_ptr);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_c2, NULL, &s_v2);

  emit_deref_to_spill(ir, 0, ptr);
  emit_reg_to_spill(ir, TCCIR_OP_STORE, v2, 4);

  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_v2, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("try_strd_spill"), 0);
  UT_ASSERT_EQ(cgstub_call_count("store_mop"), 2);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_store_spill_imm_pair_attempts_strd_imm_spill)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int base = tcc_ir_vreg_alloc_var(ir);
  emit_imm_to_spill(ir, TCCIR_OP_STORE, 0x11111111, 0);
  emit_imm_to_spill(ir, TCCIR_OP_STORE, 0x22222222, 4);

  SValue s_base = sv_var(base, VT_PTR);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_base, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("try_strd_imm_spill"), 1);
  UT_ASSERT_EQ(cgstub_call_count("store_mop"), 2);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_store_deref_vreg_reg_pair_attempts_strd_base)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int base = tcc_ir_vreg_alloc_var(ir);
  int v1 = tcc_ir_vreg_alloc_temp(ir);
  int v2 = tcc_ir_vreg_alloc_temp(ir);
  SValue s_v1 = sv_var(v1, VT_INT);
  SValue s_v2 = sv_var(v2, VT_INT);
  SValue s_c1 = sv_const(0x1111);
  SValue s_c2 = sv_const(0x2222);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_c1, NULL, &s_v1);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_c2, NULL, &s_v2);

  /* *base = v1 (offset 0, plain STORE through a dereferenced vreg) */
  SValue s_base = sv_var(base, VT_PTR);
  SValue s_base_lval = lval_of(s_base);
  tcc_ir_put(ir, TCCIR_OP_STORE, &s_v1, NULL, &s_base_lval);
  /* base[4] = v2 (STORE_INDEXED, same base register) */
  emit_reg_store_indexed(ir, base, v2, 4);

  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_base, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("try_strd_base"), 1);
  UT_ASSERT_EQ(cgstub_call_count("store_mop"), 1);
  UT_ASSERT_EQ(cgstub_call_count("store_indexed_mop"), 1);

  tcc_ir_free(ir);
  return 0;
}

/* Regression for docs/bugs.md #1b's sibling guard: *base = *ptr (the STORE's
 * own value is a deref) must not attempt pairing with the following
 * STORE_INDEXED. */
UT_TEST(test_dispatch_store_deref_vreg_deref_value_blocks_strd_base)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int base = tcc_ir_vreg_alloc_var(ir);
  int ptr = tcc_ir_vreg_alloc_temp(ir);
  int v2 = tcc_ir_vreg_alloc_temp(ir);
  SValue s_ptr = sv_var(ptr, VT_PTR);
  SValue s_v2 = sv_var(v2, VT_INT);
  SValue s_addr = sv_const(0x2000);
  SValue s_c2 = sv_const(0x2222);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_addr, NULL, &s_ptr);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_c2, NULL, &s_v2);

  SValue s_base = sv_var(base, VT_PTR);
  SValue s_base_lval = lval_of(s_base);
  SValue s_ptr_deref = lval_of(s_ptr); /* *base = *ptr */
  tcc_ir_put(ir, TCCIR_OP_STORE, &s_ptr_deref, NULL, &s_base_lval);
  emit_reg_store_indexed(ir, base, v2, 4);

  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_base, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("try_strd_base"), 0);
  UT_ASSERT_EQ(cgstub_call_count("store_mop"), 1);
  UT_ASSERT_EQ(cgstub_call_count("store_indexed_mop"), 1);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_load_indexed_reg_pair_attempts_ldrd_base)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int base = tcc_ir_vreg_alloc_var(ir);
  int d1 = tcc_ir_vreg_alloc_temp(ir);
  int d2 = tcc_ir_vreg_alloc_temp(ir);
  emit_reg_load_indexed(ir, d1, base, 0);
  emit_reg_load_indexed(ir, d2, base, 4);

  int sum = tcc_ir_vreg_alloc_temp(ir);
  SValue s_d1 = sv_var(d1, VT_INT);
  SValue s_d2 = sv_var(d2, VT_INT);
  SValue s_sum = sv_var(sum, VT_INT);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_d1, &s_d2, &s_sum);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_sum, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("try_ldrd_base"), 1);
  UT_ASSERT_EQ(cgstub_call_count("load_indexed_mop"), 2);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_assign_spill_to_reg_pair_attempts_ldrd_spill)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int d1 = tcc_ir_vreg_alloc_temp(ir);
  int d2 = tcc_ir_vreg_alloc_temp(ir);
  emit_spill_to_reg(ir, 0, d1);
  emit_spill_to_reg(ir, 4, d2);

  int sum = tcc_ir_vreg_alloc_temp(ir);
  SValue s_d1 = sv_var(d1, VT_INT);
  SValue s_d2 = sv_var(d2, VT_INT);
  SValue s_sum = sv_var(sum, VT_INT);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_d1, &s_d2, &s_sum);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_sum, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("try_ldrd_spill"), 1);
  UT_ASSERT_EQ(cgstub_call_count("assign_mop"), 2);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_assign_reg_to_spill_pair_attempts_strd_spill)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int v1 = tcc_ir_vreg_alloc_temp(ir);
  int v2 = tcc_ir_vreg_alloc_temp(ir);
  SValue s_v1 = sv_var(v1, VT_INT);
  SValue s_v2 = sv_var(v2, VT_INT);
  SValue s_c1 = sv_const(0x1111);
  SValue s_c2 = sv_const(0x2222);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_c1, NULL, &s_v1);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_c2, NULL, &s_v2);

  emit_reg_to_spill(ir, TCCIR_OP_ASSIGN, v1, 0);
  emit_reg_to_spill(ir, TCCIR_OP_ASSIGN, v2, 4);

  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_v1, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("try_strd_spill"), 1);
  /* 4, not 2: the two setup ASSIGNs (v1/v2 <- immediate) are themselves
   * dispatched through assign_mop too, in addition to the two ASSIGN-to-spill
   * instructions under test. */
  UT_ASSERT_EQ(cgstub_call_count("assign_mop"), 4);

  tcc_ir_free(ir);
  return 0;
}

/* ============================================================================
 * TCCIR_OP_BLOCK_COPY dispatch (ir/codegen.c ~4193-4202)
 *
 * Previously documented as out of scope in docs/plan_codegen_unit_tests.md
 * §9 ("needs a real Sym* this bare harness can't build -- stubs.c's always-
 * NULL sym_push2/external_global_sym"). That blocker applies to going through
 * the *frontend* symbol table; it doesn't apply here, since neither the
 * dispatch code nor the block_copy_mop stub ever dereferences the Sym*
 * pointer -- both just carry the raw IROperand/its vreg (-1, since a symref
 * has none) through untouched. tcc_ir_pool_add_symref() builds the pool
 * entry directly, bypassing sym_push2 entirely, the same "construct the IR
 * shape directly" technique used for MLA's accumulator and SELECT's
 * condition elsewhere in this project.
 * ============================================================================ */

UT_TEST(test_dispatch_block_copy_routes_to_block_copy_mop)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  /* NULL sym is safe: nothing on this dispatch path dereferences it. */
  uint32_t symref_idx = tcc_ir_pool_add_symref(ir, NULL, 0, 0);

  int pool_base = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, irop_make_stackoff(-1, -16, 0, 0, 0, IROP_BTYPE_INT32)); /* dest: stack offset */
  tcc_ir_pool_add(ir, irop_make_symref(-1, symref_idx, 0, 1, 1, IROP_BTYPE_INT32)); /* src1: symbol ref */
  tcc_ir_pool_add(ir, irop_make_imm32(-1, 64, IROP_BTYPE_INT32)); /* src2: size in bytes */
  int idx = ir->next_instruction_index;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  q->op = TCCIR_OP_BLOCK_COPY;
  q->operand_base = pool_base;
  ir->next_instruction_index++;

  tcc_ir_put(ir, TCCIR_OP_RETURNVOID, NULL, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("block_copy_mop"), 1);
  const CgStubCall *c = cgstub_nth_call("block_copy_mop", 0);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->dest_vreg, -1); /* stack offset, no vreg */
  UT_ASSERT_EQ(c->src1_vreg, -1); /* symref, no vreg */

  tcc_ir_free(ir);
  return 0;
}
