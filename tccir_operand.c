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

/* Convert SValue to IROperand, adding to appropriate pool if needed.
 * The vreg field is ALWAYS preserved from sv->vr.
 */
IROperand svalue_to_iroperand(TCCIRState *ir, const SValue *sv)
{
  if (!sv)
    return irop_make_none();

  int32_t vr = sv->vr; /* Always preserve vreg */
  int val_kind = sv->r & VT_VALMASK;
  int is_lval = sv->r & VT_LVAL;
  int is_llocal = (val_kind == VT_LLOCAL);
  int has_sym = sv->r & VT_SYM;
  int vt_btype = sv->type.t & VT_BTYPE;
  int irop_bt = vt_btype_to_irop_btype(vt_btype);

  /* Build flags */
  uint8_t flags = 0;
  if (is_lval)
    flags |= IROP_FLAG_LVAL;
  if (is_llocal)
    flags |= IROP_FLAG_LLOCAL;

  /* Case 1: Pure vreg (no const, no sym, no lval, valid vr) */
  if (vr >= 0 && val_kind != VT_CONST && val_kind != VT_LOCAL && val_kind != VT_LLOCAL && !has_sym && !is_lval)
  {
    return irop_make_vreg(vr, irop_bt);
  }

  /* Case 2: Symbol reference - always goes to symref pool */
  if (has_sym)
  {
    uint32_t pool_flags = 0;
    if (is_lval)
      pool_flags |= IRPOOL_SYMREF_LVAL;
    if (val_kind == VT_LOCAL || val_kind == VT_LLOCAL)
      pool_flags |= IRPOOL_SYMREF_LOCAL;
    uint32_t idx = tcc_ir_pool_add_symref(ir, sv->sym, (int32_t)sv->c.i, pool_flags);
    return irop_make_symref(vr, idx, flags, irop_bt);
  }

  /* Case 3: VT_LOCAL or VT_LLOCAL stack offset (no symbol) */
  if (val_kind == VT_LOCAL || val_kind == VT_LLOCAL)
  {
    return irop_make_stackoff(vr, (int32_t)sv->c.i, flags, irop_bt);
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
    return irop_make_f32(vr, u.bits);
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
    return irop_make_f64(vr, idx);
  }

  /* Case 6: 64-bit integer constant - pool I64 */
  if (vt_btype == VT_LLONG && val_kind == VT_CONST)
  {
    uint32_t idx = tcc_ir_pool_add_i64(ir, (int64_t)sv->c.i);
    return irop_make_i64(vr, idx);
  }

  /* Case 7: 32-bit integer constant - inline IMM32 */
  if (val_kind == VT_CONST)
  {
    /* Check if value fits in signed 32-bit */
    int64_t val = (int64_t)sv->c.i;
    if (val >= INT32_MIN && val <= INT32_MAX)
    {
      return irop_make_imm32(vr, (int32_t)val, irop_bt);
    }
    /* Doesn't fit - use I64 pool */
    uint32_t idx = tcc_ir_pool_add_i64(ir, val);
    return irop_make_i64(vr, idx);
  }

  /* Fallback: use symref pool for complex cases */
  uint32_t pool_flags = 0;
  if (is_lval)
    pool_flags |= IRPOOL_SYMREF_LVAL;
  if (val_kind == VT_LOCAL || val_kind == VT_LLOCAL)
    pool_flags |= IRPOOL_SYMREF_LOCAL;
  uint32_t idx = tcc_ir_pool_add_symref(ir, sv->sym, (int32_t)sv->c.i, pool_flags);
  return irop_make_symref(vr, idx, flags, irop_bt);
}

/* Expand IROperand back to SValue (for backward compatibility).
 * The vreg field is always restored from op.vr (with tag/flags stripped).
 */
void iroperand_to_svalue(const TCCIRState *ir, IROperand op, SValue *out)
{
  svalue_init(out);

  /* Always restore vreg from IROperand (strip embedded tag/flags/btype) */
  out->vr = irop_get_vreg(op.vr);

  int tag = irop_get_tag(op.vr);
  int flags = irop_get_flags(op.vr);
  int irop_bt = irop_get_btype(op.vr);

  /* Restore type.t from compressed btype (unless overridden below) */
  out->type.t = irop_btype_to_vt_btype(irop_bt);

  switch (tag)
  {
  case IROP_TAG_NONE:
    /* Already initialized by svalue_init */
    break;

  case IROP_TAG_VREG:
    /* Pure vreg - value is in a register, not memory */
    out->r = 0; /* No VT_CONST, no VT_LOCAL - just a vreg */
    break;

  case IROP_TAG_IMM32:
    out->r = VT_CONST;
    out->c.i = (int64_t)op.u.imm32;
    break;

  case IROP_TAG_STACKOFF:
  {
    /* VT_LOCAL or VT_LLOCAL based on flags */
    if (flags & IROP_FLAG_LLOCAL)
      out->r = VT_LLOCAL;
    else
      out->r = VT_LOCAL;
    if (flags & IROP_FLAG_LVAL)
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
    out->c.f = u.f;
    out->type.t = VT_FLOAT; /* Override btype */
    break;
  }

  case IROP_TAG_I64:
  {
    uint32_t idx = op.u.pool_idx;
    out->r = VT_CONST;
    out->c.i = (int64_t)ir->pool_i64[idx];
    out->type.t = VT_LLONG; /* Override btype */
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
    out->c.d = u.d;
    out->type.t = VT_DOUBLE; /* Override btype */
    break;
  }

  case IROP_TAG_SYMREF:
  {
    uint32_t idx = op.u.pool_idx;
    IRPoolSymref *ref = &ir->pool_symref[idx];
    out->sym = ref->sym;
    out->c.i = (int64_t)ref->addend;

    if (ref->flags & IRPOOL_SYMREF_LOCAL)
      out->r = VT_LOCAL;
    else
      out->r = VT_CONST;

    if (ref->flags & IRPOOL_SYMREF_LVAL)
      out->r |= VT_LVAL;

    if (ref->sym)
      out->r |= VT_SYM;

    break;
  }

  default:
    /* Unknown tag - already initialized by svalue_init */
    break;
  }
}
