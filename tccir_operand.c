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
#define USING_GLOBALS
#include "tcc.h"
#include "tccir.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef UINT32_MAX
#define UINT32_MAX 0xffffffffU
#endif
#ifndef INT32_MAX
#define INT32_MAX 0x7fffffff
#endif
#ifndef INT32_MIN
#define INT32_MIN (-INT32_MAX - 1)
#endif

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

  /* CType pool for struct/array types */
  ir->pool_ctype_capacity = IRPOOL_INIT_SIZE;
  ir->pool_ctype_count = 0;
  ir->pool_ctype = (CType *)tcc_mallocz(sizeof(CType) * ir->pool_ctype_capacity);

  /* IROperand pool - parallel to svalue_pool */
  ir->iroperand_pool_capacity = IRPOOL_INIT_SIZE;
  ir->iroperand_pool_count = 0;
  ir->iroperand_pool = (IROperand *)tcc_mallocz(sizeof(IROperand) * ir->iroperand_pool_capacity);

  if (!ir->pool_i64 || !ir->pool_f64 || !ir->pool_symref || !ir->pool_ctype || !ir->iroperand_pool)
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

  if (ir->pool_ctype)
  {
    tcc_free(ir->pool_ctype);
    ir->pool_ctype = NULL;
  }
  ir->pool_ctype_count = 0;
  ir->pool_ctype_capacity = 0;

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

/* Pool read accessors */
int64_t *tcc_ir_pool_get_i64_ptr(const TCCIRState *ir, uint32_t idx)
{
  if (!ir || idx >= (uint32_t)ir->pool_i64_count)
    return NULL;
  return &ir->pool_i64[idx];
}

uint64_t *tcc_ir_pool_get_f64_ptr(const TCCIRState *ir, uint32_t idx)
{
  if (!ir || idx >= (uint32_t)ir->pool_f64_count)
    return NULL;
  return &ir->pool_f64[idx];
}

IRPoolSymref *tcc_ir_pool_get_symref_ptr(const TCCIRState *ir, uint32_t idx)
{
  if (!ir || idx >= (uint32_t)ir->pool_symref_count)
    return NULL;
  return &ir->pool_symref[idx];
}

uint32_t tcc_ir_pool_add_ctype(TCCIRState *ir, const CType *ctype)
{
  if (ir->pool_ctype_count >= ir->pool_ctype_capacity)
  {
    ir->pool_ctype_capacity *= 2;
    ir->pool_ctype = (CType *)tcc_realloc(ir->pool_ctype, sizeof(CType) * ir->pool_ctype_capacity);
    if (!ir->pool_ctype)
    {
      fprintf(stderr, "tcc_ir_pool_add_ctype: out of memory\n");
      exit(1);
    }
  }
  ir->pool_ctype[ir->pool_ctype_count] = *ctype;
  return (uint32_t)ir->pool_ctype_count++;
}

CType *tcc_ir_pool_get_ctype_ptr(const TCCIRState *ir, uint32_t idx)
{
  if (!ir || idx >= (uint32_t)ir->pool_ctype_count)
    return NULL;
  return &ir->pool_ctype[idx];
}

/* Public wrapper: get symbol from IROperand using the global tcc_state->ir. */
ST_FUNC struct Sym *irop_get_sym(IROperand op)
{
  return irop_get_sym_ex(tcc_state->ir, op);
}

/* Get CType for struct operands using global tcc_state->ir */
CType *irop_get_ctype(IROperand op)
{
  if (op.btype != IROP_BTYPE_STRUCT)
    return NULL;
  return tcc_ir_pool_get_ctype_ptr(tcc_state->ir, op.u.s.ctype_idx);
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
  case VT_BOOL:
  case VT_BYTE:
    return IROP_BTYPE_INT8;
  case VT_SHORT:
    return IROP_BTYPE_INT16;
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
    /* VT_VOID, VT_INT, VT_PTR -> INT32 */
    return IROP_BTYPE_INT32;
  }
}

/* Convert compressed IROP_BTYPE back to VT_BTYPE for SValue reconstruction */
int irop_btype_to_vt_btype(int irop_btype)
{
  switch (irop_btype)
  {
  case IROP_BTYPE_INT8:
    return VT_BYTE;
  case IROP_BTYPE_INT16:
    return VT_SHORT;
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
 * NOTE: This does NOT set is_const, is_sym, or is_param - those are semantic flags that
 * should be set by the irop_make_* functions based on the operand type.
 */
static inline void irop_copy_svalue_info(IROperand *op, const SValue *sv)
{
  op->pr0_reg = sv->pr0_reg;
  op->pr0_spilled = sv->pr0_spilled;
  op->pr1_reg = sv->pr1_reg;
  op->pr1_spilled = sv->pr1_spilled;
  op->is_unsigned = (sv->type.t & VT_UNSIGNED) ? 1 : 0;
  /* _Bool is always unsigned (0 or 1) */
  if ((sv->type.t & VT_BTYPE) == VT_BOOL)
    op->is_unsigned = 1;
  op->is_static = (sv->type.t & VT_STATIC) ? 1 : 0;
  /* Don't overwrite is_sym, is_const, or is_param - those are set by irop_make_* */
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
  int is_complex = (sv->type.t & VT_COMPLEX) ? 1 : 0;  /* DONE: Phase 2 */

  IROperand result;

  /* Case 1: vreg (possibly with lval for register-indirect access)
   * Handles both pure vregs and register-indirect lvalues.
   * val_kind being a physical register (< VT_CONST) means the value is in/through that register. */
  if (vr >= 0 && val_kind != VT_CONST && val_kind != VT_LOCAL && val_kind != VT_LLOCAL && !has_sym)
  {
    int is_reg_param = (sv->r & VT_PARAM) && !is_local && !is_llocal;
    result = irop_make_vreg(vr, irop_bt);
    /* For register parameters, the value is directly in the register - no dereferencing needed.
     * Clear is_lval for register params since they're already values, not addresses. */
    result.is_lval = is_reg_param ? 0 : is_lval;
    result.is_param = (sv->r & VT_PARAM) ? 1 : 0; /* Preserve VT_PARAM for register params */
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
    int is_reg_param = (sv->r & VT_PARAM) && !is_local && !is_llocal;
    result = irop_make_vreg(vr, irop_bt);
    /* For register parameters, the value is directly in the register - no dereferencing needed.
     * Clear is_lval for register params since they're already values, not addresses. */
    result.is_lval = is_reg_param ? 0 : is_lval;
    result.is_param = (sv->r & VT_PARAM) ? 1 : 0; /* Preserve VT_PARAM for register params */
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
    int is_param = (sv->r & VT_PARAM) ? 1 : 0;
    int offset_val = (int32_t)sv->c.i;
    result = irop_make_stackoff(vr, offset_val, is_lval, is_llocal, is_param, irop_bt);
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

  /* Case 5: Double/Long Double constant - pool F64 */
  if ((vt_btype == VT_DOUBLE || vt_btype == VT_LDOUBLE) && val_kind == VT_CONST)
  {
    union
    {
      double d;
      uint64_t bits;
    } u;
    /* Handle cross-compilation where host and target have different long double sizes.
     * If host's long double is larger than target's, cast to double first. */
    if (vt_btype == VT_LDOUBLE && sizeof(long double) != LDOUBLE_SIZE)
      u.d = (double)sv->c.ld;
    else if (vt_btype == VT_LDOUBLE)
      u.d = (double)sv->c.ld;  /* Same size, but access through double for bit extraction */
    else
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
  /* DONE: Phase 2 - Set complex type flag in IROperand */
  result.is_complex = is_complex;
  
  /* For STRUCT types, encode CType pool index + preserve original data in split format */
  if (irop_bt == IROP_BTYPE_STRUCT)
  {
    uint32_t ctype_idx = tcc_ir_pool_add_ctype(ir, &sv->type);
    int tag = irop_get_tag(result);

    if (tag == IROP_TAG_STACKOFF)
    {
      /* Stack offset: store directly in aux_data (±32KB range) */
      int32_t offset = result.u.imm32;
      result.u.s.ctype_idx = (uint16_t)ctype_idx;
      result.u.s.aux_data = (int16_t)offset; /* store offset directly, no alignment assumption */
    }
    else if (tag == IROP_TAG_SYMREF)
    {
      /* Symbol ref: store symref pool index in aux_data (max 64K symbols) */
      uint32_t symref_idx = result.u.pool_idx;
      result.u.s.ctype_idx = (uint16_t)ctype_idx;
      result.u.s.aux_data = (int16_t)symref_idx;
    }
    else if (tag == IROP_TAG_VREG)
    {
      /* Pure vreg: u is unused, just store ctype_idx */
      result.u.s.ctype_idx = (uint16_t)ctype_idx;
      result.u.s.aux_data = 0;
    }
    else
    {
      tcc_error("UNHANDLED TAG=%d! u.imm32=%d u.pool_idx=%u\n", tag, result.u.imm32, result.u.pool_idx);
    }
    /* Other tags (IMM32, etc.) - shouldn't happen for structs, leave as-is */
  }

  /* DONE: Phase 2 - Set complex flag for all paths */
  result.is_complex = is_complex;

  /* Debug: verify round-trip conversion preserves data */
  // irop_compare_svalue(ir, sv, result, "svalue_to_iroperand");
  return result;
}

/* Expand IROperand back to SValue (for backward compatibility).
 * The vreg field is always restored from op (with tag/flags stripped).
 */
void iroperand_to_svalue(const TCCIRState *ir, IROperand op, SValue *out)
{
  svalue_init(out);

  /* Always restore vreg from IROperand (strip embedded tag/flags/btype) */
  out->vr = irop_get_vreg(op);

  int tag = irop_get_tag(op);
  int irop_bt = irop_get_btype(op);

  /* Restore type.t from compressed btype (unless overridden below) */
  out->type.t = irop_btype_to_vt_btype(irop_bt);
  
  /* DONE: Phase 2 - Restore complex type flag from IROperand to SValue */
  if (op.is_complex)
    out->type.t |= VT_COMPLEX;

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
    /* Restore VT_PARAM from explicit is_param flag */
    if (op.is_param)
      out->r |= VT_PARAM;
    /* For STRUCT types, offset is stored directly in aux_data */
    if (irop_bt == IROP_BTYPE_STRUCT)
      out->c.i = (int64_t)op.u.s.aux_data; /* offset stored directly */
    else
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
    /* For STRUCT types, symref index is stored in aux_data */
    uint32_t idx = (irop_bt == IROP_BTYPE_STRUCT) ? (uint32_t)(uint16_t)op.u.s.aux_data : op.u.pool_idx;
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

  /* For STRUCT types, restore full CType from pool (including type.ref) */
  if (irop_bt == IROP_BTYPE_STRUCT)
  {
    CType *ct = tcc_ir_pool_get_ctype_ptr(ir, op.u.s.ctype_idx);
    if (ct)
    {
      out->type = *ct; /* Restore full CType including ref pointer */
      /* Re-apply any type flags that were set above */
      if (op.is_unsigned)
        out->type.t |= VT_UNSIGNED;
      if (op.is_static)
        out->type.t |= VT_STATIC;
    }
  }
}

/* Debug: compare SValue with IROperand by converting IROperand back to SValue
 * and comparing critical fields. Returns 1 if mismatch found, 0 if OK.
 */
int irop_compare_svalue(const TCCIRState *ir, const SValue *sv, IROperand op, const char *context)
{
  SValue reconstructed;
  iroperand_to_svalue(ir, op, &reconstructed);

  int mismatch = 0;

  /* Compare individual fields and report differences */
  if (reconstructed.pr0_reg != sv->pr0_reg)
  {
    fprintf(stderr, "%s: pr0_reg mismatch: reconstructed=%d, expected=%d\n", context, reconstructed.pr0_reg,
            sv->pr0_reg);
    mismatch = 1;
  }

  if (reconstructed.pr0_spilled != sv->pr0_spilled)
  {
    fprintf(stderr, "%s: pr0_spilled mismatch: reconstructed=%d, expected=%d\n", context, reconstructed.pr0_spilled,
            sv->pr0_spilled);
    mismatch = 1;
  }

  if (reconstructed.pr1_reg != sv->pr1_reg)
  {
    fprintf(stderr, "%s: pr1_reg mismatch: reconstructed=%d, expected=%d\n", context, reconstructed.pr1_reg,
            sv->pr1_reg);
    mismatch = 1;
  }

  if (reconstructed.pr1_spilled != sv->pr1_spilled)
  {
    fprintf(stderr, "%s: pr1_spilled mismatch: reconstructed=%d, expected=%d\n", context, reconstructed.pr1_spilled,
            sv->pr1_spilled);
    mismatch = 1;
  }

  if (reconstructed.r != sv->r)
  {
    fprintf(stderr, "%s: r mismatch: reconstructed=0x%04x, expected=0x%04x\n", context, reconstructed.r, sv->r);
    mismatch = 1;
  }

  if (reconstructed.vr != sv->vr)
  {
    fprintf(stderr, "%s: vr mismatch: reconstructed=%d, expected=%d\n", context, reconstructed.vr, sv->vr);
    mismatch = 1;
  }

  if (reconstructed.type.t != sv->type.t)
  {
    fprintf(stderr, "%s: type.t mismatch: reconstructed=0x%08x, expected=0x%08x\n", context, reconstructed.type.t,
            sv->type.t);
    mismatch = 1;
  }

  if (reconstructed.type.ref != sv->type.ref)
  {
    fprintf(stderr, "%s: type.ref mismatch: reconstructed=%p, expected=%p\n", context, (void *)reconstructed.type.ref,
            (void *)sv->type.ref);
    mismatch = 1;
  }

  /* Compare CValue (c union) - compare multiple members for better diagnosis */
  if (reconstructed.c.i != sv->c.i)
  {
    fprintf(stderr, "%s: c.i mismatch: reconstructed=0x%016llx, expected=0x%016llx\n", context,
            (unsigned long long)reconstructed.c.i, (unsigned long long)sv->c.i);
    mismatch = 1;
  }
  else if (memcmp(&reconstructed.c, &sv->c, sizeof(CValue)) != 0)
  {
    /* Check string members if i matches but bytes differ (likely padding or str variant) */
    if (reconstructed.c.str.data != sv->c.str.data || reconstructed.c.str.size != sv->c.str.size)
    {
      fprintf(stderr, "%s: c.str mismatch: data=%p/%p, size=%d/%d\n", context, (void *)reconstructed.c.str.data,
              (void *)sv->c.str.data, reconstructed.c.str.size, sv->c.str.size);
    }
    else
    {
      fprintf(stderr, "%s: c mismatch: bytes differ (likely padding)\n", context);
      fprintf(stderr, "  reconstructed.c.i = 0x%016llx\n", (unsigned long long)reconstructed.c.i);
      fprintf(stderr, "  expected.c.i = 0x%016llx\n", (unsigned long long)sv->c.i);
    }
    mismatch = 1;
  }

  /* Compare sym pointer */
  if (reconstructed.sym != sv->sym)
  {
    fprintf(stderr, "%s: sym mismatch: reconstructed=%p, expected=%p\n", context, (void *)reconstructed.sym,
            (void *)sv->sym);
    mismatch = 1;
  }

  return mismatch;
}

int irop_type_size(IROperand op)
{
  switch (op.btype)
  {
  case IROP_BTYPE_INT8:
    return 1;
  case IROP_BTYPE_INT16:
    return 2;
  case IROP_BTYPE_INT32:
  case IROP_BTYPE_FLOAT32:
    return 4;
  case IROP_BTYPE_INT64:
  case IROP_BTYPE_FLOAT64:
    return 8;
  case IROP_BTYPE_STRUCT:
    /* For structs, get CType from pool using split ctype_idx field */
    {
      CType *ct = tcc_ir_pool_get_ctype_ptr(tcc_state->ir, op.u.s.ctype_idx);
      if (ct)
      {
        int align;
        return type_size(ct, &align);
      }
    }
    break;
  default:
    break;
  }
  return 0; // Unknown size
}

/* Get type size and alignment from IROperand.
 * For structs, uses the CType pool to compute actual size/alignment.
 * Returns size in bytes, writes alignment to *align_out if non-NULL. */
int irop_type_size_align(IROperand op, int *align_out)
{
  int align = 4; /* default alignment */

  switch (op.btype)
  {
  case IROP_BTYPE_INT8:
    align = 1;
    if (align_out)
      *align_out = align;
    return 1;
  case IROP_BTYPE_INT16:
    align = 2;
    if (align_out)
      *align_out = align;
    return 2;
  case IROP_BTYPE_INT32:
  case IROP_BTYPE_FLOAT32:
    align = 4;
    if (align_out)
      *align_out = align;
    return 4;
  case IROP_BTYPE_INT64:
  case IROP_BTYPE_FLOAT64:
    align = 8;
    if (align_out)
      *align_out = align;
    return 8;
  case IROP_BTYPE_STRUCT:
    /* For structs, get CType from pool using split ctype_idx field */
    {
      CType *ct = tcc_ir_pool_get_ctype_ptr(tcc_state->ir, op.u.s.ctype_idx);
      if (ct)
      {
        int size = type_size(ct, &align);
        if (align_out)
          *align_out = align;
        return size;
      }
    }
    break;
  default:
    break;
  }
  if (align_out)
    *align_out = align;
  return 0; // Unknown size
}