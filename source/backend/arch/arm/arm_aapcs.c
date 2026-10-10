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

  /* Hard-float co-processor register candidates (AAPCS-VFP C.1-C.5): a
   * scalar float/double, or a homogeneous aggregate of 1-4 of them (a struct
   * HFA, a _Complex float/double), takes the lowest run of consecutive free
   * s-registers in s0..s15 -- even-aligned d-registers for doubles.  Variadic
   * callees use the base standard (GPRs), so this only applies to
   * non-variadic calls.
   *
   * Allocation back-fills: a float can take a gap a double left below it, so
   * the bank is a per-register bitmap.  A candidate that finds no run closes
   * the bank for every later argument (C.3) and goes on the stack at its
   * natural alignment (C.5) -- never into core registers.
   *
   * Doubles ride the VFP bank even on a single-precision-only FPU: the ABI is
   * about where arguments live, not about which arithmetic exists, so the
   * callee unpacks d0 into a GPR pair to call __aeabi_dadd.  That is what
   * arm-none-eabi-gcc does for -mfpu=fpv5-sp-d16, and matching it is what makes
   * our objects interoperate with its libm. */
  const int cprc_count = arg_desc->is_float ? 1 : arg_desc->hfa_count;
  const int cprc_base = arg_desc->is_float ? size : arg_desc->hfa_base;
  if (layout->hard_float && !layout->is_variadic && cprc_count > 0 && (cprc_base == 4 || cprc_base == 8))
  {
    const int step = cprc_base / 4;        /* doubles must be even-aligned */
    const int slots = cprc_count * step;   /* s-registers consumed */
    int base = -1;

    if (!layout->vfp_exhausted)
    {
      for (int cand = 0; cand + slots <= 16; cand += step)
      {
        const unsigned mask = (unsigned)((1u << slots) - 1u) << cand;
        if (!(layout->vfp_used & mask))
        {
          layout->vfp_used |= (uint16_t)mask;
          base = cand;
          break;
        }
      }
      if (base < 0)
        layout->vfp_exhausted = 1; /* no more VFP for any later argument */
    }

    if (base >= 0)
    {
      loc.kind = TCC_ABI_LOC_VFP_REG;
      loc.reg_base = (uint8_t)base;
      loc.reg_count = (uint8_t)slots;
    }
    else
    {
      layout->next_stack_off = tcc_abi_align_up_int(layout->next_stack_off, align > 8 ? 8 : align);
      loc.kind = TCC_ABI_LOC_STACK;
      loc.stack_off = layout->next_stack_off;
      layout->next_stack_off += tcc_abi_align_up_int(size, 4);
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

    /* AAPCS32 passes a composite of any size by value: in the core registers
     * from the NCRN, then on the stack, split between the two when it
     * straddles r3 (rule C.5).  (Passing one larger than 16 bytes by a pointer
     * to a copy is the AArch64 rule, not this one.) */
    {
      /* AAPCS: Composite types with 8-byte natural alignment require
       * double-word alignment — the NCRN must be rounded up to the
       * next even register number before allocation.  A stack slot is
       * aligned to at most 8 (C.4 caps the NSAA rounding at 8), whatever
       * the composite's own alignment. */
      const int stack_align = align > 8 ? 8 : align;
      if (align >= 8 && (layout->next_reg & 1))
        layout->next_reg++;

      if ((int)layout->next_reg + regs_needed <= 4)
      {
        loc.kind = TCC_ABI_LOC_REG;
        loc.reg_base = layout->next_reg;
        loc.reg_count = (uint8_t)regs_needed;
        layout->next_reg = (uint8_t)(layout->next_reg + regs_needed);
      }
      else if (layout->next_reg <= 3 && layout->next_stack_off == 0)
      {
        /* AAPCS C.5: split only while no earlier argument has reached the
         * stack (NSAA is still SP).  Once a VFP argument has spilled, C.6
         * moves the whole composite to the stack instead. */
        int regs_avail = 4 - layout->next_reg;
        int words_on_stack = regs_needed - regs_avail;
        loc.kind = TCC_ABI_LOC_REG_STACK;
        loc.reg_base = layout->next_reg;
        loc.reg_count = (uint8_t)regs_avail;
        layout->next_stack_off = tcc_abi_align_up_int(layout->next_stack_off, stack_align);
        loc.stack_off = layout->next_stack_off;
        loc.stack_size = (uint32_t)(words_on_stack * 4);
        layout->next_stack_off += words_on_stack * 4;
        layout->next_reg = 4;
      }
      else
      {
        layout->next_stack_off = tcc_abi_align_up_int(layout->next_stack_off, stack_align);
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
