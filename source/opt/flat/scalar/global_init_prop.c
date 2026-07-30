/*
 *  TCC IR - Global initializer constant propagation (flat pass)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* Relocated flat pass, Branch A [A*]; see docs/plan_legacy_flat_ir_ssa_retire.md */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "opt_du.h"

/* Fold LOAD/deref of a const-data global to #imm or &SymY+addend. */
int tcc_ir_opt_global_init_prop(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;
  if (!tcc_state)
    return 0;

  const int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* LOAD src1 deref becomes ASSIGN; other ops fold an is_sym&&is_lval deref in place. */
    for (int slot = 0; slot < 2; slot++)
    {
      IROperand opnd = (slot == 0) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      if (!opnd.is_sym || !opnd.is_lval)
        continue;

      IRPoolSymref *ref = irop_get_symref_ex(ir, opnd);
      if (!ref || !ref->sym)
        continue;
      Sym *sym = ref->sym;

      /* Linkage / attribute gates. */
      if (sym->a.weak || sym->a.dllimport)
        continue;

      const int ttype = sym->type.t;
      if (ttype & VT_VLA)
        continue;
      if (ttype & VT_VOLATILE)
        continue;

      int is_const_q = (ttype & VT_CONSTANT) != 0;
      /* For arrays, const qualifies the element type; check the pointed-to type. */
      if (!is_const_q && (ttype & VT_ARRAY) && sym->type.ref)
        is_const_q = (sym->type.ref->type.t & VT_CONSTANT) != 0;

      /* Const globals cannot be modified, so ignore possibly_written. */
      if (!is_const_q && sym->a.possibly_written)
        continue;

      if (!(ttype & VT_STATIC) && !is_const_q)
        continue;

      /* Pointer globals fold only when const-qualified (non-const symref fold unsafe). */
      if ((ttype & VT_BTYPE) == VT_PTR && !is_const_q)
        continue;

      /* Single-pass: non-const statics only fold in the TU-wide late_reopt phase, and only if addr never taken. */
      if (!is_const_q)
      {
        if (sym->a.addrtaken)
          continue;
        if (!tcc_state->ir_late_reopt_phase)
        {
          /* Defer: record function for end-of-TU re-optimization. */
          if (tcc_state->cur_func_sym && tcc_state->cur_func_sym->type.ref)
            tcc_state->cur_func_sym->type.ref->f.func_late_reopt = 1;
          continue;
        }
      }

      ElfSym *esym = elfsym(sym);
      if (!esym)
        continue;
      if (esym->st_shndx == SHN_UNDEF || esym->st_shndx == SHN_COMMON)
        continue;
      if (esym->st_shndx >= tcc_state->nb_sections)
        continue;

      Section *sec = tcc_state->sections[esym->st_shndx];
      if (!sec)
        continue;
      /* SHT_NOBITS (.bss): no data buffer, value is implicit zero. */
      int is_bss = (sec->sh_type == SHT_NOBITS);
      if (!is_bss && !sec->data)
        continue;

      /* Result btype: dest btype for LOAD, else the operand's own btype. */
      int result_btype;
      int result_is_unsigned;
      if (q->op == TCCIR_OP_LOAD && slot == 0)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        result_btype = irop_get_btype(dest);
        result_is_unsigned = dest.is_unsigned;
      }
      else
      {
        result_btype = irop_get_btype(opnd);
        result_is_unsigned = opnd.is_unsigned;
      }

      /* Map result btype to read size in bytes; reject undecodable types. */
      int read_size;
      switch (result_btype)
      {
        case IROP_BTYPE_INT8:  read_size = 1; break;
        case IROP_BTYPE_INT16: read_size = 2; break;
        case IROP_BTYPE_INT32: read_size = 4; break;
        case IROP_BTYPE_INT64: read_size = 8; break;
        default:               read_size = 0; break;
      }
      if (read_size == 0)
        continue;

      unsigned long off = (unsigned long)(esym->st_value + (unsigned long long)ref->addend);
      if (!is_bss && off + (unsigned long)read_size > sec->data_offset)
        continue;

      /* Only foldable overlap: a single R_ARM_ABS32 at exactly `off` with a 4-byte read. */
      int reloc_at_off = 0;
      int reloc_overlap = 0;
      Sym *reloc_target_sym = NULL;
      int64_t reloc_data_addend = 0;

      if (sec->reloc && sec->reloc->data && sec->reloc->data_offset)
      {
        ElfW_Rel *rel;
        for_each_elem(sec->reloc, 0, rel, ElfW_Rel)
        {
          uint32_t r_off = (uint32_t)rel->r_offset;
          int r_type = ELFW(R_TYPE)(rel->r_info);
          /* Non-ABS32 treated as single-byte cover to still reject partial overlaps. */
          uint32_t r_size = (r_type == R_ARM_ABS32) ? 4 : 1;

          if (r_off + r_size <= off)
            continue;
          if (r_off >= off + (unsigned long)read_size)
            continue;

          reloc_overlap = 1;
          /* Symref fold only safe when the source global is const-qualified. */
          if (r_off == off && read_size == 4 && r_type == R_ARM_ABS32 && is_const_q)
          {
            int r_sym_idx = ELFW(R_SYM)(rel->r_info);
            if (r_sym_idx > 0 && symtab_section && symtab_section->link)
            {
              ElfW(Sym) *tgt_esym = &((ElfW(Sym) *)symtab_section->data)[r_sym_idx];
              const char *tname = (const char *)symtab_section->link->data + tgt_esym->st_name;
              if (tname && *tname)
              {
                int tok = tok_alloc_const(tname);
                Sym *tsym = sym_find(tok);
                if (tsym)
                {
                  /* REL format: addend lives in the data at the reloc offset. */
                  int32_t a32 = 0;
                  if (!is_bss)
                    memcpy(&a32, sec->data + off, 4);
                  reloc_target_sym = tsym;
                  reloc_data_addend = a32;
                  reloc_at_off = 1;
                }
              }
            }
          }
          break;
        }
      }

      if (reloc_overlap && !reloc_at_off)
        continue;

      IROperand new_opnd;
      if (reloc_at_off)
      {
        /* Fold to a symref-by-value (address constant: &TargetSym + addend). */
        uint32_t pool_idx = tcc_ir_pool_add_symref(ir, reloc_target_sym, (int32_t)reloc_data_addend, 0);
        new_opnd = irop_make_symref(-1, pool_idx, 0 /* not lval */, 0, 1 /* is_const */, result_btype);
        new_opnd.is_unsigned = result_is_unsigned;
      }
      else
      {
        int64_t val = 0;
        if (!is_bss)
        {
          const unsigned char *ptr = sec->data + off;
          memcpy(&val, ptr, read_size);
          if (!result_is_unsigned && read_size < 8)
          {
            int shift = (8 - read_size) * 8;
            val = (int64_t)(val << shift) >> shift;
          }
        }

        if (result_btype == IROP_BTYPE_INT64 || val != (int64_t)(int32_t)val)
        {
          uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
          new_opnd = irop_make_i64(-1, pool_idx, result_btype);
        }
        else
        {
          new_opnd = irop_make_imm32(-1, (int32_t)val, result_btype);
        }
        new_opnd.is_unsigned = result_is_unsigned;
      }

      if (q->op == TCCIR_OP_LOAD && slot == 0)
      {
        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, i, new_opnd);
      }
      else if (slot == 0)
      {
        tcc_ir_set_src1(ir, i, new_opnd);
      }
      else
      {
        tcc_ir_set_src2(ir, i, new_opnd);
      }
      changes++;
    }
  }

  return changes;
}

