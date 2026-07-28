/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2026 Mateusz Stadnik
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

#include "tcc.h"
#include "tccabi.h"
#include <stdio.h>
#include <string.h>

TCCAbiArgLoc tcc_abi_classify_argument(TCCAbiCallLayout *layout, int arg_index, const TCCAbiArgDesc *arg_desc)
{
  TCCAbiArgLoc loc;
  memset(&loc, 0, sizeof(loc));
  if (!layout || !arg_desc || arg_index < 0)
  {
    loc.kind = TCC_ABI_LOC_STACK;
    loc.stack_off = 0;
    loc.size = 0;
    return loc;
  }

  /* Grow recorded arrays if needed. */
  const int needed = arg_index + 1;
  if (layout->capacity < needed)
  {
    int new_cap = layout->capacity ? layout->capacity : 8;
    while (new_cap < needed)
      new_cap *= 2;
    // layout->locs = (TCCAbiArgLoc *)tcc_realloc(layout->locs, sizeof(TCCAbiArgLoc) * new_cap);
    /* Zero new tail for determinism. */
    // memset(&layout->locs[layout->capacity], 0, sizeof(TCCAbiArgLoc) * (new_cap - layout->capacity));
    layout->capacity = new_cap;
  }

  layout->argc = layout->argc < needed ? needed : layout->argc;

  /* Default AAPCS-ish alignment requirement at call boundary. */
  if (layout->stack_align == 0)
    layout->stack_align = 8;

  int size = arg_desc->size;
  int align = arg_desc->alignment;
  if (align < 4)
    align = 4;

  loc.size = (uint32_t)size;
  loc.reg_base = 0;
  loc.reg_count = 0;
  loc.stack_off = 0;

  /* Hard-float single-precision: pass in the next VFP argument register
   * (s0..s15), else on the stack.  Variadic callees use the base standard
   * (GPRs), so this only applies to non-variadic calls.  Doubles stay soft
   * (GPR pairs) for now and fall through to the SCALAR64 path. */
  if (layout->hard_float && !layout->is_variadic && arg_desc->is_float && size == 4)
  {
    if (layout->next_vfp_reg < 16)
    {
      loc.kind = TCC_ABI_LOC_VFP_REG;
      loc.reg_base = layout->next_vfp_reg;
      loc.reg_count = 1;
      layout->next_vfp_reg++;
    }
    else
    {
      layout->next_stack_off = tcc_abi_align_up_int(layout->next_stack_off, 4);
      loc.kind = TCC_ABI_LOC_STACK;
      loc.stack_off = layout->next_stack_off;
      layout->next_stack_off += 4;
    }
    layout->stack_size = tcc_abi_align_up_int(layout->next_stack_off, layout->stack_align ? layout->stack_align : 8);
    return loc;
  }

  if (arg_desc->kind == TCC_ABI_ARG_SCALAR64)
  {
    if (layout->next_reg & 1)
      layout->next_reg++;

    if (layout->next_reg <= 2)
    {
      loc.kind = TCC_ABI_LOC_REG;
      loc.reg_base = layout->next_reg;
      loc.reg_count = 2;
      layout->next_reg += 2;
    }
    else
    {
      layout->next_stack_off = tcc_abi_align_up_int(layout->next_stack_off, 8);
      loc.kind = TCC_ABI_LOC_STACK;
      loc.stack_off = layout->next_stack_off;
      layout->next_stack_off += 8;
      layout->next_reg = 4;
    }
  }
  else if (arg_desc->kind == TCC_ABI_ARG_STRUCT_BYVAL)
  {
    const int slot_sz = tcc_abi_align_up_int(size, 4);
    const int regs_needed = (slot_sz + 3) / 4;

    /* Invisible reference for large composites (> 16 bytes).
     *
     * This is used only on the callee side (where arg_flags is allocated
     * by tcc_abi_call_layout_ensure_capacity).  On the caller/call-site
     * side, arg_flags is NULL and large structs are classified as normal
     * by-value composites — the frontend (gfunc_param_typed) handles the
     * invisible-reference conversion for prototyped calls, while variadic
     * anonymous arguments must be passed by value for va_arg to work.
     *
     * NOTE: The invisible-reference check must come BEFORE the 8-byte
     * alignment padding below.  When passed by invisible reference the
     * argument is a 4-byte pointer, so the struct's natural alignment
     * is irrelevant for register assignment and must not cause the NCRN
     * to skip a register. */
    if (size > 16 && layout->arg_flags)
    {
      layout->arg_flags[arg_index] |= TCC_ABI_ARG_FLAG_INVISIBLE_REF;
      /* Pass the pointer in a register (like a scalar) */
      if (layout->next_reg <= 3)
      {
        loc.kind = TCC_ABI_LOC_REG;
        loc.reg_base = layout->next_reg;
        loc.reg_count = 1;
        loc.size = 4; /* pointer size */
        layout->next_reg++;
      }
      else
      {
        loc.kind = TCC_ABI_LOC_STACK;
        loc.stack_off = layout->next_stack_off;
        loc.size = 4; /* pointer size */
        layout->next_stack_off += 4;
      }
    }
    else
    {
      /* AAPCS: Composite types with 8-byte natural alignment require
       * double-word alignment — the NCRN must be rounded up to the
       * next even register number before allocation.  This only applies
       * to by-value composites, not invisible references (handled above). */
      if (align >= 8 && (layout->next_reg & 1))
        layout->next_reg++;

      if ((int)layout->next_reg + regs_needed <= 4)
      {
        loc.kind = TCC_ABI_LOC_REG;
        loc.reg_base = layout->next_reg;
        loc.reg_count = (uint8_t)regs_needed;
        layout->next_reg = (uint8_t)(layout->next_reg + regs_needed);
      }
      else if (layout->next_reg <= 3)
      {
        /* AAPCS: Struct straddles registers and stack.
         * Put first word(s) in remaining registers, rest on stack. */
        int regs_avail = 4 - layout->next_reg;
        int words_on_stack = regs_needed - regs_avail;
        loc.kind = TCC_ABI_LOC_REG_STACK;
        loc.reg_base = layout->next_reg;
        loc.reg_count = (uint8_t)regs_avail;
        layout->next_stack_off = tcc_abi_align_up_int(layout->next_stack_off, align);
        loc.stack_off = layout->next_stack_off;
        loc.stack_size = (uint32_t)(words_on_stack * 4);
        layout->next_stack_off += words_on_stack * 4;
        layout->next_reg = 4;
      }
      else
      {
        layout->next_stack_off = tcc_abi_align_up_int(layout->next_stack_off, align);
        loc.kind = TCC_ABI_LOC_STACK;
        loc.stack_off = layout->next_stack_off;
        layout->next_stack_off += slot_sz;
        layout->next_reg = 4;
      }
    }
  }
  else
  {
    if (layout->next_reg <= 3)
    {
      loc.kind = TCC_ABI_LOC_REG;
      loc.reg_base = layout->next_reg;
      loc.reg_count = 1;
      layout->next_reg++;
    }
    else
    {
      layout->next_stack_off = tcc_abi_align_up_int(layout->next_stack_off, 4);
      loc.kind = TCC_ABI_LOC_STACK;
      loc.stack_off = layout->next_stack_off;
      layout->next_stack_off += 4;
      layout->next_reg = 4;
    }
  }

  layout->stack_size = tcc_abi_align_up_int(layout->next_stack_off, layout->stack_align ? layout->stack_align : 8);
  return loc;
}

int tcc_abi_align_up_int(int v, int align)
{
  return (v + align - 1) & ~(align - 1);
}

void tcc_abi_call_layout_ensure_capacity(TCCAbiCallLayout *layout, int needed)
{
  if (!layout)
    return;
  if (needed <= 0)
    return;

  if (layout->capacity >= needed && layout->locs && layout->args_effective && layout->args_original &&
      layout->arg_flags)
    return;

  int new_capacity = layout->capacity ? layout->capacity : 8;
  while (new_capacity < needed)
    new_capacity *= 2;

  layout->locs = (TCCAbiArgLoc *)tcc_realloc(layout->locs, sizeof(TCCAbiArgLoc) * (size_t)new_capacity);
  layout->args_original =
      (TCCAbiArgDesc *)tcc_realloc(layout->args_original, sizeof(TCCAbiArgDesc) * (size_t)new_capacity);
  layout->args_effective =
      (TCCAbiArgDesc *)tcc_realloc(layout->args_effective, sizeof(TCCAbiArgDesc) * (size_t)new_capacity);
  layout->arg_flags = (uint8_t *)tcc_realloc(layout->arg_flags, (size_t)new_capacity);

  /* Zero-init the newly added tail. */
  if (new_capacity > layout->capacity)
  {
    const int old = layout->capacity;
    memset(&layout->locs[old], 0, sizeof(TCCAbiArgLoc) * (size_t)(new_capacity - old));
    memset(&layout->args_original[old], 0, sizeof(TCCAbiArgDesc) * (size_t)(new_capacity - old));
    memset(&layout->args_effective[old], 0, sizeof(TCCAbiArgDesc) * (size_t)(new_capacity - old));
    memset(&layout->arg_flags[old], 0, (size_t)(new_capacity - old));
  }

  layout->capacity = new_capacity;
}

void tcc_abi_call_layout_deinit(TCCAbiCallLayout *layout)
{
  if (!layout)
    return;
  if (layout->locs)
    tcc_free(layout->locs);
  if (layout->args_original)
    tcc_free(layout->args_original);
  if (layout->args_effective)
    tcc_free(layout->args_effective);
  if (layout->arg_flags)
    tcc_free(layout->arg_flags);
  memset(layout, 0, sizeof(*layout));
}
