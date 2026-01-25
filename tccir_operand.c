/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "tccir_operand.h"
#include "tcc.h"
#include "tccir.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * IROperand pool management - separate pools for cache efficiency
 * ============================================================================
 */
#define IRPOOL_INIT_SIZE 64

void tcc_ir_pools_init(TCCIRState *ir)
{
  /* I64 pool */
  ir->pool_i64_capacity = IRPOOL_INIT_SIZE;
  ir->pool_i64_count = 0;
  ir->pool_i64 = (int64_t *)tcc_mallocz(sizeof(int64_t) * ir->pool_i64_capacity);

  /* F64 pool */
  ir->pool_f64_capacity = IRPOOL_INIT_SIZE;
  ir->pool_f64_count = 0;
  ir->pool_f64 = (uint64_t *)tcc_mallocz(sizeof(uint64_t) * ir->pool_f64_capacity);

  /* Symref pool */
  ir->pool_symref_capacity = IRPOOL_INIT_SIZE;
  ir->pool_symref_count = 0;
  ir->pool_symref = (IRPoolSymref *)tcc_mallocz(sizeof(IRPoolSymref) * ir->pool_symref_capacity);

  /* IROperand pool - parallel to svalue_pool */
  ir->iroperand_pool_capacity = IRPOOL_INIT_SIZE;
  ir->iroperand_pool_count = 0;
  ir->iroperand_pool = (IROperand *)tcc_mallocz(sizeof(IROperand) * ir->iroperand_pool_capacity);

  if (!ir->pool_i64 || !ir->pool_f64 || !ir->pool_symref || !ir->iroperand_pool)
  {
    fprintf(stderr, "tcc_ir_pools_init: out of memory\n");
    exit(1);
  }
}

void tcc_ir_pools_free(TCCIRState *ir)
{
  if (ir->pool_i64)
  {
    tcc_free(ir->pool_i64);
    ir->pool_i64 = NULL;
  }
  ir->pool_i64_count = 0;
  ir->pool_i64_capacity = 0;

  if (ir->pool_f64)
  {
    tcc_free(ir->pool_f64);
    ir->pool_f64 = NULL;
  }
  ir->pool_f64_count = 0;
  ir->pool_f64_capacity = 0;

  if (ir->pool_symref)
  {
    tcc_free(ir->pool_symref);
    ir->pool_symref = NULL;
  }
  ir->pool_symref_count = 0;
  ir->pool_symref_capacity = 0;

  if (ir->iroperand_pool)
  {
    tcc_free(ir->iroperand_pool);
    ir->iroperand_pool = NULL;
  }
  ir->iroperand_pool_count = 0;
  ir->iroperand_pool_capacity = 0;
}

uint32_t tcc_ir_pool_add_i64(TCCIRState *ir, int64_t val)
{
  if (ir->pool_i64_count >= ir->pool_i64_capacity)
  {
    ir->pool_i64_capacity *= 2;
    ir->pool_i64 = (int64_t *)tcc_realloc(ir->pool_i64, sizeof(int64_t) * ir->pool_i64_capacity);
    if (!ir->pool_i64)
    {
      fprintf(stderr, "tcc_ir_pool_add_i64: out of memory\n");
      exit(1);
    }
  }
  ir->pool_i64[ir->pool_i64_count] = val;
  return (uint32_t)ir->pool_i64_count++;
}

uint32_t tcc_ir_pool_add_f64(TCCIRState *ir, uint64_t bits)
{
  if (ir->pool_f64_count >= ir->pool_f64_capacity)
  {
    ir->pool_f64_capacity *= 2;
    ir->pool_f64 = (uint64_t *)tcc_realloc(ir->pool_f64, sizeof(uint64_t) * ir->pool_f64_capacity);
    if (!ir->pool_f64)
    {
      fprintf(stderr, "tcc_ir_pool_add_f64: out of memory\n");
      exit(1);
    }
  }
  ir->pool_f64[ir->pool_f64_count] = bits;
  return (uint32_t)ir->pool_f64_count++;
}

uint32_t tcc_ir_pool_add_symref(TCCIRState *ir, Sym *sym, int32_t addend, uint32_t flags)
{
  if (ir->pool_symref_count >= ir->pool_symref_capacity)
  {
    ir->pool_symref_capacity *= 2;
    ir->pool_symref = (IRPoolSymref *)tcc_realloc(ir->pool_symref, sizeof(IRPoolSymref) * ir->pool_symref_capacity);
    if (!ir->pool_symref)
    {
      fprintf(stderr, "tcc_ir_pool_add_symref: out of memory\n");
      exit(1);
    }
  }
  IRPoolSymref *entry = &ir->pool_symref[ir->pool_symref_count];
  entry->sym = sym;
  entry->addend = addend;
  entry->flags = flags;
  return (uint32_t)ir->pool_symref_count++;
}

/* ============================================================================
 * IROperand <-> SValue conversion functions
 * ============================================================================
 * These form the synchronization layer between the old SValue-based system
 * and the new IROperand-based system during the migration period.
 */

/* Convert VT_BTYPE to compressed IROP_BTYPE for storage in vr field */
static int vt_btype_to_irop_btype(int vt_btype)
{
  switch (vt_btype)
  {
  case VT_LLONG:
    return IROP_BTYPE_INT64;
  case VT_FLOAT:
    return IROP_BTYPE_FLOAT32;
  case VT_DOUBLE:
  case VT_LDOUBLE:
    return IROP_BTYPE_FLOAT64;
  case VT_STRUCT:
    return IROP_BTYPE_STRUCT;
  case VT_FUNC:
    return IROP_BTYPE_FUNC;
  default:
    /* VT_VOID, VT_BYTE, VT_SHORT, VT_INT, VT_PTR, VT_BOOL -> INT32 */
    return IROP_BTYPE_INT32;
  }
}

/* Convert compressed IROP_BTYPE back to VT_BTYPE for SValue reconstruction */
static int irop_btype_to_vt_btype(int irop_btype)
{
  switch (irop_btype)
  {
  case IROP_BTYPE_INT64:
    return VT_LLONG;
  case IROP_BTYPE_FLOAT32:
    return VT_FLOAT;
  case IROP_BTYPE_FLOAT64:
    return VT_DOUBLE;
  case IROP_BTYPE_STRUCT:
    return VT_STRUCT;
  case IROP_BTYPE_FUNC:
    return VT_FUNC;
  default:
    return VT_INT; /* Default for INT32 */
  }
}

/* Helper to copy physical register info and type flags from SValue to IROperand.
 * NOTE: This does NOT set is_const or is_sym - those are semantic flags that
 * should be set by the irop_make_* functions based on the operand type.
 */
static inline void irop_copy_svalue_info(IROperand *op, const SValue *sv)
{
  op->pr0_reg = sv->pr0_reg;
  op->pr0_spilled = sv->pr0_spilled;
  op->pr1_reg = sv->pr1_reg;
  op->pr1_spilled = sv->pr1_spilled;
  op->is_unsigned = (sv->type.t & VT_UNSIGNED) ? 1 : 0;
  op->is_static = (sv->type.t & VT_STATIC) ? 1 : 0;
  /* Don't overwrite is_sym or is_const - those are set by irop_make_* */
  op->reserved = 0;
}

/* Convert SValue to IROperand, adding to appropriate pool if needed.
 * The vreg field is ALWAYS preserved from sv->vr.
 * Physical register allocation and type flags are also preserved.
 */
IROperand svalue_to_iroperand(TCCIRState *ir, const SValue *sv)
{
  if (!sv)
    return irop_make_none();

  int32_t vr = sv->vr; /* Always preserve vreg */
  int val_kind = sv->r & VT_VALMASK;
  int is_lval = (sv->r & VT_LVAL) ? 1 : 0;
  int is_llocal = (val_kind == VT_LLOCAL) ? 1 : 0;
  int is_local = (val_kind == VT_LOCAL || val_kind == VT_LLOCAL) ? 1 : 0;
  int is_const = (val_kind == VT_CONST) ? 1 : 0;
  int has_sym = (sv->r & VT_SYM) ? 1 : 0;
  int vt_btype = sv->type.t & VT_BTYPE;
  int irop_bt = vt_btype_to_irop_btype(vt_btype);

  IROperand result;

  /* Case 1: vreg (possibly with lval for register-indirect access)
   * Handles both pure vregs and register-indirect lvalues.
   * val_kind being a physical register (< VT_CONST) means the value is in/through that register. */
  if (vr >= 0 && val_kind != VT_CONST && val_kind != VT_LOCAL && val_kind != VT_LLOCAL && !has_sym)
  {
    result = irop_make_vreg(vr, irop_bt);
    result.is_lval = is_lval;
    irop_copy_svalue_info(&result, sv);
    /* Capture physical register from VT_VALMASK if it's a register number */
    if (val_kind < VT_CONST && val_kind < 32) /* Physical register in VT_VALMASK */
      result.pr0_reg = val_kind;
    goto done;
  }

  /* Case 1b: Physical register with no vreg (vr < 0)
   * Value is purely in a physical register, not tracked by IR vreg system. */
  if (vr < 0 && val_kind < VT_CONST && val_kind < 32 && !has_sym)
  {
    result = irop_make_vreg(vr, irop_bt);
    result.is_lval = is_lval;
    irop_copy_svalue_info(&result, sv);
    result.pr0_reg = val_kind; /* Physical register in VT_VALMASK */
    goto done;
  }

  /* Case 2: Symbol reference - always goes to symref pool */
  if (has_sym)
  {
    uint32_t pool_flags = 0;
    if (is_lval)
      pool_flags |= IRPOOL_SYMREF_LVAL;
    if (is_local)
      pool_flags |= IRPOOL_SYMREF_LOCAL;
    uint32_t idx = tcc_ir_pool_add_symref(ir, sv->sym, (int32_t)sv->c.i, pool_flags);
    result = irop_make_symref(vr, idx, is_lval, is_local, is_const, irop_bt);
    irop_copy_svalue_info(&result, sv);
    goto done;
  }

  /* Case 3: VT_LOCAL or VT_LLOCAL stack offset (no symbol) */
  if (val_kind == VT_LOCAL || val_kind == VT_LLOCAL)
  {
    result = irop_make_stackoff(vr, (int32_t)sv->c.i, is_lval, is_llocal, irop_bt);
    irop_copy_svalue_info(&result, sv);
    goto done;
  }

  /* Case 4: Float constant - inline F32 */
  if (vt_btype == VT_FLOAT && val_kind == VT_CONST)
  {
    union
    {
      float f;
      uint32_t bits;
    } u;
    u.f = sv->c.f;
    result = irop_make_f32(vr, u.bits);
    result.is_lval = is_lval;
    irop_copy_svalue_info(&result, sv);
    goto done;
  }

  /* Case 5: Double constant - pool F64 */
  if (vt_btype == VT_DOUBLE && val_kind == VT_CONST)
  {
    union
    {
      double d;
      uint64_t bits;
    } u;
    u.d = sv->c.d;
    uint32_t idx = tcc_ir_pool_add_f64(ir, u.bits);
    result = irop_make_f64(vr, idx);
    result.is_lval = is_lval;
    irop_copy_svalue_info(&result, sv);
    goto done;
  }

  /* Case 6: 64-bit integer constant - pool I64 */
  if (vt_btype == VT_LLONG && val_kind == VT_CONST)
  {
    uint32_t idx = tcc_ir_pool_add_i64(ir, (int64_t)sv->c.i);
    result = irop_make_i64(vr, idx, irop_bt);
    result.is_lval = is_lval;
    irop_copy_svalue_info(&result, sv);
    goto done;
  }

  /* Case 7: 32-bit integer constant - inline IMM32 */
  if (val_kind == VT_CONST)
  {
    /* Check if value fits in 32-bit (signed or unsigned depending on type) */
    int64_t val = (int64_t)sv->c.i;
    int is_unsigned = (sv->type.t & VT_UNSIGNED) ? 1 : 0;
    int fits_32bit = is_unsigned ? (val >= 0 && val <= (int64_t)UINT32_MAX) : (val >= INT32_MIN && val <= INT32_MAX);
    if (fits_32bit)
    {
      result = irop_make_imm32(vr, (int32_t)val, irop_bt);
      result.is_lval = is_lval;
      irop_copy_svalue_info(&result, sv);
      goto done;
    }
    /* Doesn't fit - use I64 pool */
    uint32_t idx = tcc_ir_pool_add_i64(ir, val);
    result = irop_make_i64(vr, idx, irop_bt);
    result.is_lval = is_lval;
    irop_copy_svalue_info(&result, sv);
    goto done;
  }

  /* Fallback: use symref pool for complex cases */
  {
    uint32_t pool_flags = 0;
    if (is_lval)
      pool_flags |= IRPOOL_SYMREF_LVAL;
    if (is_local)
      pool_flags |= IRPOOL_SYMREF_LOCAL;
    uint32_t idx = tcc_ir_pool_add_symref(ir, sv->sym, (int32_t)sv->c.i, pool_flags);
    result = irop_make_symref(vr, idx, is_lval, is_local, is_const, irop_bt);
    result.is_sym = has_sym; /* Only set if original had VT_SYM */
    irop_copy_svalue_info(&result, sv);
  }

done:
  /* Debug: verify round-trip conversion preserves data */
  assert(irop_compare_svalue(ir, sv, result, "svalue_to_iroperand") == 0);
  return result;
}

/* Expand IROperand back to SValue (for backward compatibility).
 * The vreg field is always restored from op (with tag/flags stripped).
 */
void iroperand_to_svalue(const TCCIRState *ir, IROperand op, SValue *out)
{
  svalue_init(out);

  /* Always restore vreg from IROperand (strip embedded tag/flags/btype) */
  out->vr = irop_get_vreg(&op);

  int tag = irop_get_tag(&op);
  int irop_bt = irop_get_btype(&op);

  /* Restore type.t from compressed btype (unless overridden below) */
  out->type.t = irop_btype_to_vt_btype(irop_bt);

  switch (tag)
  {
  case IROP_TAG_NONE:
    /* Already initialized by svalue_init */
    break;

  case IROP_TAG_VREG:
    /* vreg - value is in a register, or register-indirect if lval set */
    /* Restore physical register from pr0_reg if allocated (non-zero or explicitly r0) */
    out->r = op.pr0_reg; /* Physical register in VT_VALMASK */
    if (op.is_lval)
      out->r |= VT_LVAL;
    break;

  case IROP_TAG_IMM32:
    out->r = op.is_const ? VT_CONST : 0;
    if (op.is_lval)
      out->r |= VT_LVAL;
    /* Zero-extend for unsigned types, sign-extend for signed */
    if (op.is_unsigned)
      out->c.i = (int64_t)(uint32_t)op.u.imm32;
    else
      out->c.i = (int64_t)op.u.imm32;
    break;

  case IROP_TAG_STACKOFF:
  {
    /* VT_LOCAL or VT_LLOCAL based on bitfields */
    if (op.is_llocal)
      out->r = VT_LLOCAL;
    else
      out->r = VT_LOCAL;
    if (op.is_lval)
      out->r |= VT_LVAL;
    /* Derive VT_PARAM from vreg type - PARAM vregs represent parameter locations */
    if (TCCIR_DECODE_VREG_TYPE(out->vr) == TCCIR_VREG_TYPE_PARAM)
      out->r |= VT_PARAM;
    out->c.i = (int64_t)op.u.imm32; /* stack offset stored in imm32 */
    break;
  }

  case IROP_TAG_F32:
  {
    union
    {
      uint32_t bits;
      float f;
    } u;
    u.bits = op.u.f32_bits;
    out->r = VT_CONST;
    if (op.is_lval)
      out->r |= VT_LVAL;
    out->c.f = u.f;
    out->type.t = VT_FLOAT; /* Override btype */
    break;
  }

  case IROP_TAG_I64:
  {
    uint32_t idx = op.u.pool_idx;
    out->r = VT_CONST;
    if (op.is_lval)
      out->r |= VT_LVAL;
    out->c.i = (int64_t)ir->pool_i64[idx];
    /* Use stored btype - don't override to VT_LLONG, could be VT_INT with large value */
    break;
  }

  case IROP_TAG_F64:
  {
    uint32_t idx = op.u.pool_idx;
    union
    {
      uint64_t bits;
      double d;
    } u;
    u.bits = ir->pool_f64[idx];
    out->r = VT_CONST;
    if (op.is_lval)
      out->r |= VT_LVAL;
    out->c.d = u.d;
    /* Use stored btype - don't override to VT_DOUBLE, could be VT_LDOUBLE */
    break;
  }

  case IROP_TAG_SYMREF:
  {
    uint32_t idx = op.u.pool_idx;
    IRPoolSymref *ref = &ir->pool_symref[idx];
    out->sym = ref->sym;
    out->c.i = (int64_t)ref->addend;

    /* Use bitfields from op to restore r value */
    if (op.is_local)
      out->r = VT_LOCAL;
    else if (op.is_const)
      out->r = VT_CONST;
    else
      out->r = 0; /* Register */

    if (op.is_lval)
      out->r |= VT_LVAL;

    if (op.is_sym)
      out->r |= VT_SYM;

    break;
  }

  default:
    /* Unknown tag - already initialized by svalue_init */
    break;
  }

  /* Restore physical register allocation from IROperand */
  out->pr0_reg = op.pr0_reg;
  out->pr0_spilled = op.pr0_spilled;
  out->pr1_reg = op.pr1_reg;
  out->pr1_spilled = op.pr1_spilled;

  /* Restore type flags */
  if (op.is_unsigned)
    out->type.t |= VT_UNSIGNED;
  if (op.is_static)
    out->type.t |= VT_STATIC;
}

/* Debug: compare SValue with IROperand by converting IROperand back to SValue
 * and comparing critical fields. Returns 1 if mismatch found, 0 if OK.
 */
int irop_compare_svalue(const TCCIRState *ir, const SValue *sv, IROperand op, const char *context)
{
  SValue reconstructed;
  iroperand_to_svalue(ir, op, &reconstructed);

  int mismatch = 0;

  /* Compare vr (vreg) */
  if (sv->vr != reconstructed.vr)
  {
    fprintf(stderr, "IROP_MISMATCH[%s]: vr: orig=%d reconstructed=%d\n", context, sv->vr, reconstructed.vr);
    mismatch = 1;
  }

  /* Compare r (storage class/flags) - mask out bits that aren't preserved */
  int sv_valmask = sv->r & VT_VALMASK;
  int rec_valmask = reconstructed.r & VT_VALMASK;
  if (sv_valmask != rec_valmask)
  {
    fprintf(stderr, "IROP_MISMATCH[%s]: r&VT_VALMASK: orig=0x%x reconstructed=0x%x\n", context, sv_valmask,
            rec_valmask);
    mismatch = 1;
  }

  int sv_lval = (sv->r & VT_LVAL) ? 1 : 0;
  int rec_lval = (reconstructed.r & VT_LVAL) ? 1 : 0;
  if (sv_lval != rec_lval)
  {
    fprintf(stderr, "IROP_MISMATCH[%s]: VT_LVAL: orig=%d reconstructed=%d\n", context, sv_lval, rec_lval);
    mismatch = 1;
  }

  int sv_sym = (sv->r & VT_SYM) ? 1 : 0;
  int rec_sym = (reconstructed.r & VT_SYM) ? 1 : 0;
  if (sv_sym != rec_sym)
  {
    fprintf(stderr, "IROP_MISMATCH[%s]: VT_SYM: orig=%d reconstructed=%d\n", context, sv_sym, rec_sym);
    mismatch = 1;
  }

  /* Compare type.t basic type - allow equivalent compressed types */
  int sv_btype = sv->type.t & VT_BTYPE;
  int rec_btype = reconstructed.type.t & VT_BTYPE;
  /* VT_BYTE, VT_SHORT, VT_INT, VT_PTR, VT_BOOL all compress to INT32 -> VT_INT
   * This is acceptable lossy compression since they're all <= 32 bits */
  int sv_btype_class = (sv_btype == VT_BYTE || sv_btype == VT_SHORT || sv_btype == VT_INT || sv_btype == VT_PTR ||
                        sv_btype == VT_BOOL || sv_btype == VT_VOID)
                           ? VT_INT
                           : sv_btype;
  int rec_btype_class = (rec_btype == VT_BYTE || rec_btype == VT_SHORT || rec_btype == VT_INT || rec_btype == VT_PTR ||
                         rec_btype == VT_BOOL || rec_btype == VT_VOID)
                            ? VT_INT
                            : rec_btype;
  if (sv_btype_class != rec_btype_class)
  {
    fprintf(stderr, "IROP_MISMATCH[%s]: VT_BTYPE: orig=0x%x reconstructed=0x%x\n", context, sv_btype, rec_btype);
    mismatch = 1;
  }

  int sv_unsigned = (sv->type.t & VT_UNSIGNED) ? 1 : 0;
  int rec_unsigned = (reconstructed.type.t & VT_UNSIGNED) ? 1 : 0;
  if (sv_unsigned != rec_unsigned)
  {
    fprintf(stderr, "IROP_MISMATCH[%s]: VT_UNSIGNED: orig=%d reconstructed=%d\n", context, sv_unsigned, rec_unsigned);
    mismatch = 1;
  }

  /* Compare c.i for non-float types, only when it's meaningful */
  /* c.i matters for: VT_CONST, VT_LOCAL, VT_LLOCAL (offsets), VT_SYM (offsets) */
  int sv_valkind = sv->r & VT_VALMASK;
  int c_i_matters = (sv_valkind == VT_CONST || sv_valkind == VT_LOCAL || sv_valkind == VT_LLOCAL || (sv->r & VT_SYM));
  if (c_i_matters && sv_btype != VT_FLOAT && sv_btype != VT_DOUBLE && sv_btype != VT_LDOUBLE)
  {
    if (sv->c.i != reconstructed.c.i)
    {
      fprintf(stderr, "IROP_MISMATCH[%s]: c.i: orig=0x%llx reconstructed=0x%llx\n", context,
              (unsigned long long)sv->c.i, (unsigned long long)reconstructed.c.i);
      mismatch = 1;
    }
  }

  /* Compare sym pointer - only if VT_SYM is set (otherwise sym is garbage) */
  if (sv_sym && sv->sym != reconstructed.sym)
  {
    fprintf(stderr, "IROP_MISMATCH[%s]: sym: orig=%p reconstructed=%p\n", context, (void *)sv->sym,
            (void *)reconstructed.sym);
    mismatch = 1;
  }

  if (mismatch)
  {
    fprintf(stderr, "IROP_MISMATCH[%s]: Original SValue: vr=%d r=0x%x type.t=0x%x c.i=0x%llx sym=%p\n", context, sv->vr,
            sv->r, sv->type.t, (unsigned long long)sv->c.i, (void *)sv->sym);
    fprintf(stderr,
            "IROP_MISMATCH[%s]: IROperand: tag=%d is_lval=%d is_llocal=%d is_local=%d is_const=%d is_sym=%d "
            "btype=%d position=%d vreg_type=%d\n",
            context, op.tag, op.is_lval, op.is_llocal, op.is_local, op.is_const, op.is_sym, op.btype, op.position,
            op.vreg_type);
    fprintf(stderr, "IROP_MISMATCH[%s]: Reconstructed: vr=%d r=0x%x type.t=0x%x c.i=0x%llx sym=%p\n", context,
            reconstructed.vr, reconstructed.r, reconstructed.type.t, (unsigned long long)reconstructed.c.i,
            (void *)reconstructed.sym);
  }

  return mismatch;
}
