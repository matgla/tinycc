/*
 *  Embedded Dereference Extraction - Simplified Implementation
 *
 *  Pattern:  V0 = V0 ADD T0***DEREF***
 *            where T0 was created by: ASSIGN T0, P0; ADD T1, T0, #4; STORE P0, T1
 *
 *  Transform: Extract the DEREF into an explicit LOAD_POSTINC that combines
 *             with the pointer update pattern.
 */

#include "ir.h"
#include "pool.h"
#include "vreg.h"

/* Check if operand is a TEMP vreg with DEREF flag */
static int is_temp_deref(IROperand op)
{
  if (op.vr == -1)
    return 0;
  if (op.vreg_type != TCCIR_VREG_TYPE_TEMP)
    return 0;
  return op.is_lval;
}

/* Find ASSIGN instruction that defines ptr_vr */
static int find_assign_defining(TCCIRState *ir, int32_t ptr_vr, int before_idx)
{
  for (int i = before_idx - 1; i >= 0 && i >= before_idx - 10; i--)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(dest) == ptr_vr)
      return i;
  }
  return -1;
}

/* Find ADD that uses ptr_vr and immediate, returning the ADD index and offset */
static int find_add_with_imm(TCCIRState *ir, int start_idx, int32_t ptr_vr, int *offset_out)
{
  int n = ir->next_instruction_index;
  for (int i = start_idx + 1; i < n && i < start_idx + 5; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ADD)
      continue;
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    int s1_vr = irop_get_vreg(src1);
    int s2_vr = irop_get_vreg(src2);
    if (s1_vr == ptr_vr && src2.is_const)
    {
      *offset_out = (int)src2.u.imm32;
      return i;
    }
    if (s2_vr == ptr_vr && src1.is_const)
    {
      *offset_out = (int)src1.u.imm32;
      return i;
    }
  }
  return -1;
}

/* Find STORE of add_result to orig_ptr_vr */
static int find_store_to_vreg(TCCIRState *ir, int start_idx, int32_t orig_ptr_vr, int32_t add_result_vr)
{
  int n = ir->next_instruction_index;
  for (int i = start_idx + 1; i < n && i < start_idx + 3; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (irop_get_vreg(dest) == orig_ptr_vr && irop_get_vreg(src1) == add_result_vr)
      return i;
  }
  return -1;
}

/* Main extraction function */
int tcc_ir_opt_extract_embedded_deref(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_src1)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    /* Find which operand (if any) is a TEMP with DEREF */
    int deref_src = 0;
    IROperand deref_op;
    if (is_temp_deref(src1))
    {
      deref_src = 1;
      deref_op = src1;
    }
    else if (is_temp_deref(src2))
    {
      deref_src = 2;
      deref_op = src2;
    }
    else
      continue;

    int32_t ptr_vr = irop_get_vreg(deref_op);
    if (ptr_vr < 0)
      continue;

    /* Find the ASSIGN that created this ptr_copy */
    int assign_idx = find_assign_defining(ir, ptr_vr, i);
    if (assign_idx < 0)
      continue;

    /* Get the original pointer from ASSIGN */
    IRQuadCompact *assign_q = &ir->compact_instructions[assign_idx];
    IROperand assign_src = tcc_ir_op_get_src1(ir, assign_q);
    if (!irop_has_vreg(assign_src))
      continue;
    int32_t orig_ptr_vr = irop_get_vreg(assign_src);

    /* Find the ADD that uses ptr_vr */
    int offset = 0;
    int add_idx = find_add_with_imm(ir, assign_idx, ptr_vr, &offset);
    if (add_idx < 0 || offset <= 0 || offset > 255)
      continue;

    /* Get the ADD result vreg */
    IRQuadCompact *add_q = &ir->compact_instructions[add_idx];
    IROperand add_dest = tcc_ir_op_get_dest(ir, add_q);
    int32_t add_result_vr = irop_get_vreg(add_dest);

    /* Find the STORE of ADD result to original pointer */
    int store_idx = find_store_to_vreg(ir, add_idx, orig_ptr_vr, add_result_vr);
    if (store_idx < 0)
      continue;

    /* We found the pattern! Now transform it:
     *
     * Before:
     *   ASSIGN ptr_copy, ptr
     *   ADD new_ptr, ptr_copy, #imm
     *   STORE ptr, new_ptr
     *   ...
     *   V0 = V0 ADD ptr_copy***DEREF***
     *
     * After:
     *   LOAD_POSTINC loaded, ptr, #imm
     *   ...
     *   V0 = V0 ADD loaded
     *
     * The ADD and STORE become NOP (dead), and the ASSIGN is converted to LOAD_POSTINC.
     */

    /* Allocate new temp for the loaded value */
    int32_t loaded_vreg = tcc_ir_vreg_alloc_temp(ir);
    if (loaded_vreg < 0)
      continue;

    /* Check operand pool capacity */
    if (ir->iroperand_pool_count + 4 > ir->iroperand_pool_capacity)
      continue;

    /* Convert ASSIGN to LOAD_POSTINC */
    int new_base = ir->iroperand_pool_count;

    /* LOAD_POSTINC operands: dest (loaded), ptr, unused, offset */
    IROperand loaded_op = irop_make_vreg(loaded_vreg, IROP_BTYPE_INT32);
    IROperand ptr_op = assign_src;
    ptr_op.is_lval = 0;
    ptr_op.is_llocal = 0;
    IROperand unused = IROP_NONE;
    IROperand offset_op = IROP_NONE;
    offset_op.is_const = 1;
    offset_op.u.imm32 = offset;

    tcc_ir_pool_add(ir, loaded_op);
    tcc_ir_pool_add(ir, ptr_op);
    tcc_ir_pool_add(ir, unused);
    tcc_ir_pool_add(ir, offset_op);

    assign_q->op = TCCIR_OP_LOAD_POSTINC;
    assign_q->operand_base = new_base;

    /* Mark ADD and STORE as NOP */
    ir->compact_instructions[add_idx].op = TCCIR_OP_NOP;
    ir->compact_instructions[store_idx].op = TCCIR_OP_NOP;

    /* Update the using instruction to use loaded_vreg without DEREF */
    IROperand new_op = loaded_op;
    if (deref_src == 1)
      tcc_ir_set_src1(ir, i, new_op);
    else
      tcc_ir_set_src2(ir, i, new_op);

    changes++;
  }

  return changes;
}
