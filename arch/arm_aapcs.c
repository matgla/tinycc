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

  loc.size = (uint16_t)size;
  loc.reg_base = 0;
  loc.reg_count = 0;
  loc.stack_off = 0;

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

    /* AAPCS: Composite types > 4 words (16 bytes) are passed by invisible reference.
     * The caller passes a pointer in a register, callee dereferences. */
    if (size > 16)
    {
      /* Mark as invisible reference */
      if (layout->arg_flags)
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
    else if ((int)layout->next_reg + regs_needed <= 4)
    {
      loc.kind = TCC_ABI_LOC_REG;
      loc.reg_base = layout->next_reg;
      loc.reg_count = (uint8_t)regs_needed;
      layout->next_reg = (uint8_t)(layout->next_reg + regs_needed);
      fprintf(stderr, "DEBUG ABI: struct arg %d -> REG: base=%d count=%d\n", arg_index, loc.reg_base, loc.reg_count);
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
      loc.stack_size = (uint16_t)(words_on_stack * 4);
      layout->next_stack_off += words_on_stack * 4;
      layout->next_reg = 4;
      fprintf(stderr, "DEBUG ABI: struct arg %d -> REG_STACK: base=%d reg_count=%d stack_off=%d stack_size=%d\n",
              arg_index, loc.reg_base, loc.reg_count, loc.stack_off, loc.stack_size);
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
  else
  {
    if (layout->next_reg <= 3)
    {
      loc.kind = TCC_ABI_LOC_REG;
      loc.reg_base = layout->next_reg;
      loc.reg_count = 1;
      layout->next_reg++;
      fprintf(stderr, "DEBUG ABI: scalar arg %d -> REG: base=%d\n", arg_index, loc.reg_base);
    }
    else
    {
      layout->next_stack_off = tcc_abi_align_up_int(layout->next_stack_off, 4);
      loc.kind = TCC_ABI_LOC_STACK;
      loc.stack_off = layout->next_stack_off;
      layout->next_stack_off += 4;
      layout->next_reg = 4;
      fprintf(stderr, "DEBUG ABI: scalar arg %d -> STACK: off=%d\n", arg_index, loc.stack_off);
    }
  }

  layout->stack_size = tcc_abi_align_up_int(layout->next_stack_off, layout->stack_align ? layout->stack_align : 8);
  //   layout->locs[arg_index] = loc;
  return loc;
}

int tcc_abi_align_up_int(int v, int align)
{
  return (v + align - 1) & ~(align - 1);
}
