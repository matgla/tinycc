/*
 *  TCC IR - Constant & Value Propagation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "opt_du.h"

int tcc_ir_opt_const_var_prop(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  int max_var_pos = 0;
  int i;

  if (n == 0)
    return 0;

  /* Phase 1: Find constant VAR vregs (assigned exactly once with immediate) */
  typedef struct
  {
    uint8_t is_constant : 1;
    uint8_t def_count : 7;
    int64_t value;
    int btype;
    int is_unsigned;
  } VarInfo;

  /* Combined pass: find max_var_pos and build var_info in one O(n) scan.
   * var_info grows dynamically as new VAR positions are discovered. */
  VarInfo *var_info = NULL;
  int var_info_cap = 0;
  int has_var = 0;

  for (i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_VAR)
      continue;

    int pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
    has_var = 1;
    if (pos > max_var_pos)
      max_var_pos = pos;

    if (pos >= var_info_cap)
    {
      int new_cap = var_info_cap ? var_info_cap * 2 : 16;
      while (new_cap <= pos)
        new_cap *= 2;
      var_info = tcc_realloc(var_info, sizeof(VarInfo) * new_cap);
      memset(var_info + var_info_cap, 0, sizeof(VarInfo) * (new_cap - var_info_cap));
      var_info_cap = new_cap;
    }

    /* If the variable's address is taken, it can be modified through aliases
     * (e.g. passed as an out-parameter to a function).  Not safe for
     * constant propagation. */
    IRLiveInterval *interval = tcc_ir_get_live_interval(ir, dest_vr);
    if (interval && interval->addrtaken)
    {
      var_info[pos].def_count++;
      var_info[pos].is_constant = 0;
      continue;
    }

    var_info[pos].def_count++;

    if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_STORE)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      if (irop_is_immediate(src1) && !src1.is_sym && !src1.is_lval && !src1.is_local && var_info[pos].def_count == 1)
      {
        var_info[pos].is_constant = 1;
        var_info[pos].value = irop_get_imm64_ex(ir, src1);
        var_info[pos].btype = irop_get_btype(src1);
        var_info[pos].is_unsigned = src1.is_unsigned;
      }
    }
    else
    {
      var_info[pos].is_constant = 0;
    }
  }

  if (!has_var)
  {
    if (var_info)
      tcc_free(var_info);
    return 0;
  }

  /* Mark multiply-defined vars as non-constant */
  for (i = 0; i <= max_var_pos; i++)
  {
    if (var_info[i].def_count > 1)
      var_info[i].is_constant = 0;
  }

  /* Phase 2: Replace uses of constant VARs with immediates */
  for (i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Check src1.
     * Don't propagate if src1 is a local without lval — that's an address-of
     * (LEA), not a value load.  Replacing it with the variable's value would
     * be incorrect. */
    if (irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t src1_vr = irop_get_vreg(src1);
      if (src1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR && !(src1.is_local && !src1.is_lval))
      {
        int pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
        if (pos <= max_var_pos && var_info[pos].is_constant)
        {
          int64_t val = var_info[pos].value;
          IROperand new_src1;
          if (val == (int32_t)val)
            new_src1 = irop_make_imm32(-1, (int32_t)val, var_info[pos].btype);
          else
          {
            uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
            new_src1 = irop_make_i64(-1, pool_idx, var_info[pos].btype);
          }
          new_src1.is_unsigned = var_info[pos].is_unsigned;
          tcc_ir_set_src1(ir, i, new_src1);

          /* LOAD with constant src means the address was a local variable that
           * is now known to be a constant value — convert to ASSIGN. */
          if (q->op == TCCIR_OP_LOAD)
            q->op = TCCIR_OP_ASSIGN;

          changes++;
        }
      }
    }

    /* Check src2 (same LEA guard as src1) */
    if (irop_config[q->op].has_src2)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int32_t src2_vr = irop_get_vreg(src2);
      if (src2_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src2_vr) == TCCIR_VREG_TYPE_VAR && !(src2.is_local && !src2.is_lval))
      {
        int pos = TCCIR_DECODE_VREG_POSITION(src2_vr);
        if (pos <= max_var_pos && var_info[pos].is_constant)
        {
          int64_t val = var_info[pos].value;
          IROperand new_src2;
          if (val == (int32_t)val)
            new_src2 = irop_make_imm32(-1, (int32_t)val, var_info[pos].btype);
          else
          {
            uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
            new_src2 = irop_make_i64(-1, pool_idx, var_info[pos].btype);
          }
          new_src2.is_unsigned = var_info[pos].is_unsigned;
          tcc_ir_set_src2(ir, i, new_src2);
          changes++;
        }
      }
    }
  }

  /* Phase 3: Eliminate dead VAR ASSIGNs whose uses were all replaced.
   * Scan for remaining uses of each constant VAR; if none found, NOP
   * the defining ASSIGN. */
  if (changes > 0)
  {
    /* Reset use counts for constant VARs */
    uint8_t *has_use = tcc_mallocz((max_var_pos + 8) / 8);

    for (i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[q->op].has_src1)
      {
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        int32_t vr = irop_get_vreg(src1);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (pos <= max_var_pos)
            has_use[pos / 8] |= (1 << (pos % 8));
        }
      }
      if (irop_config[q->op].has_src2)
      {
        IROperand src2 = tcc_ir_op_get_src2(ir, q);
        int32_t vr = irop_get_vreg(src2);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (pos <= max_var_pos)
            has_use[pos / 8] |= (1 << (pos % 8));
        }
      }
    }

    /* NOP dead ASSIGN instructions for constant VARs with no remaining uses */
    for (i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_ASSIGN)
        continue;
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos > max_var_pos)
        continue;
      if (var_info[pos].is_constant && !(has_use[pos / 8] & (1 << (pos % 8))))
      {
        q->op = TCCIR_OP_NOP;
        changes++;
      }
    }

    tcc_free(has_use);
  }

  tcc_free(var_info);
  return changes;
}

/* ---------------------------------------------------------------------------
 * Global-initializer constant propagation
 *
 * Replace `LOAD dest <-- GlobalSym(X) [deref]` with `ASSIGN dest <-- #imm`
 * when X is a static global whose initial value is still valid — i.e. no
 * store or non-const pointer escape has been seen in this TU.  The
 * initializer bytes are read out of the symbol's ELF section (same source
 * the inline-eval path at tccgen.c:11965 uses for `*&g` folding).  After
 * this pass runs, the iterative const_prop + branch_folding + DCE pipeline
 * picks up the newly-visible constants and collapses comparisons / dead
 * error arms that the runtime CMP previously kept live.
 *
 * Safety gates (mirror the checks already used in try_inline_const_eval):
 *   - The sym must exist and carry a known type.
 *   - possibly_written == 0 (no stores / no non-const pointer escape).
 *   - Not volatile, not array / VLA, not aggregate.
 *   - Primitive type (VT_BYTE / VT_SHORT / VT_INT / VT_LLONG / VT_BOOL / VT_PTR).
 *   - Linkage: static / file-local only — non-static extern-visible globals
 *     may be written from other translation units, which possibly_written
 *     cannot observe.
 *   - Weak / dllimport / undefined symbols are skipped.
 *   - The initializer range must fit inside the section's emitted data.
 */
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
    if (q->op != TCCIR_OP_LOAD)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (!src1.is_sym || !src1.is_lval)
      continue;

    IRPoolSymref *ref = irop_get_symref_ex(ir, src1);
    if (!ref || !ref->sym)
      continue;
    Sym *sym = ref->sym;

    /* Linkage / attribute gates. */
    if (sym->a.weak || sym->a.dllimport)
      continue;
    if (sym->a.possibly_written)
      continue;
    if (!(sym->type.t & VT_STATIC))
      continue; /* extern-visible: other TUs may write it */
    /* TCC is single-pass: when this function is optimized, stores in
     * later-declared functions have not yet been seen, so possibly_written
     * may be 0 for a global that is in fact written elsewhere in the TU
     * (see 20001111-1.c).  Restrict the fold to const-qualified globals,
     * which the language guarantees are not modified. */
    if (!(sym->type.t & VT_CONSTANT))
      continue;

    const int ttype = sym->type.t;
    if (ttype & (VT_ARRAY | VT_VLA))
      continue;
    if (ttype & VT_VOLATILE)
      continue;

    const int btype = ttype & VT_BTYPE;
    if (btype != VT_BYTE && btype != VT_SHORT && btype != VT_INT && btype != VT_LLONG && btype != VT_BOOL &&
        btype != VT_PTR)
      continue;

    /* Pointer globals whose initializer is another symbol (e.g. `static T *p = &x;`)
     * live in .data as zero bytes plus a relocation.  Reading the raw bytes
     * would yield a bogus null pointer — skip pointer types entirely. */
    if (btype == VT_PTR)
      continue;

    ElfSym *esym = elfsym(sym);
    if (!esym)
      continue;
    if (esym->st_shndx == SHN_UNDEF || esym->st_shndx == SHN_COMMON)
      continue;
    if (esym->st_shndx >= tcc_state->nb_sections)
      continue;

    Section *sec = tcc_state->sections[esym->st_shndx];
    if (!sec || !sec->data)
      continue;

    int align;
    int sz = type_size(&sym->type, &align);
    if (sz <= 0 || sz > 8)
      continue;

    unsigned long off = (unsigned long)(esym->st_value + (unsigned long long)ref->addend);
    if (off + (unsigned long)sz > sec->data_offset)
      continue;

    /* Read the initializer bytes.  Sign-extend narrow signed types so the
     * IR constant carries the correct high bits. */
    const unsigned char *ptr = sec->data + off;
    int64_t val = 0;
    if (sz == 8)
    {
      memcpy(&val, ptr, 8);
    }
    else
    {
      memcpy(&val, ptr, sz);
      if (!(ttype & VT_UNSIGNED) && sz < 8)
      {
        int shift = (8 - sz) * 8;
        val = (int64_t)(val << shift) >> shift;
      }
    }

    /* Build the new immediate operand.  Preserve the LOAD's result btype
     * (the destination) rather than deriving from Sym, so later passes see
     * a consistent shape. */
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int dest_btype = irop_get_btype(dest);
    IROperand new_src1;
    if (dest_btype == IROP_BTYPE_INT64 || val != (int64_t)(int32_t)val)
    {
      uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
      new_src1 = irop_make_i64(-1, pool_idx, dest_btype);
    }
    else
    {
      new_src1 = irop_make_imm32(-1, (int32_t)val, dest_btype);
    }
    new_src1.is_unsigned = (ttype & VT_UNSIGNED) ? 1 : 0;

    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, i, new_src1);
    changes++;
  }

  return changes;
}

/* Complex Constant Param Folding — compile-time evaluation/folding for
 * _Complex float locals passed by value to a call.
 *
 * Pattern:
 *   StackLoc[-N]   <-- #C_real [STORE]         (4-byte float constant)
 *   StackLoc[-N+4] <-- #C_imag [STORE]         (4-byte float constant)
 *   FUNCPARAMVAL  src1 = StackLoc[-N]          (8-byte read, complex lval)
 *
 * When the 8-byte slot [-N, -N+8) is touched by exactly these three ops —
 * no other read, write, or address-of references it — pack {real,imag}
 * into a 64-bit complex-float immediate (real in low 32 bits, imag in
 * high 32 bits) and rewrite the PARAM source as that immediate.  The
 * two component stores become dead and are NOP'd; the codegen path for
 * complex constants then materializes the value directly into the
 * callee's argument registers, skipping the stack round-trip.
 */
int tcc_ir_opt_complex_const_param_fold(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  const int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCPARAMVAL)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (irop_get_tag(src1) != IROP_TAG_STACKOFF)
      continue;
    if (!src1.is_lval || !src1.is_complex)
      continue;
    /* Only handle _Complex float (8 bytes packed: real_u32 | imag_u32 << 32) */
    if (src1.btype != IROP_BTYPE_FLOAT32)
      continue;
    /* Skip if this stack offset carries a vreg (spill slot for a vreg) — those
     * are not raw stack locals and need different treatment. */
    if (irop_get_vreg(src1) != -1)
      continue;
    /* Skip incoming-arg stack slots; they alias caller-allocated memory. */
    if (src1.is_param)
      continue;

    int real_off = (int)irop_get_stack_offset(src1);
    int imag_off = real_off + 4;

    int real_store_idx = -1;
    int imag_store_idx = -1;
    uint32_t real_bits = 0;
    uint32_t imag_bits = 0;
    int conflict = 0;

    for (int j = 0; j < n && !conflict; j++)
    {
      if (j == i)
        continue;
      IRQuadCompact *p = &ir->compact_instructions[j];
      if (p->op == TCCIR_OP_NOP)
        continue;

      /* dest: detect a 4-byte constant STORE/ASSIGN to either component slot. */
      if (irop_config[p->op].has_dest)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, p);
        if (irop_get_tag(dest) == IROP_TAG_STACKOFF && irop_get_vreg(dest) == -1 && dest.is_lval && !dest.is_param)
        {
          int doff = (int)irop_get_stack_offset(dest);
          if (doff == real_off || doff == imag_off)
          {
            if ((p->op != TCCIR_OP_STORE && p->op != TCCIR_OP_ASSIGN) || dest.is_complex ||
                dest.btype != IROP_BTYPE_FLOAT32)
            {
              conflict = 1;
              break;
            }
            IROperand sv = tcc_ir_op_get_src1(ir, p);
            int tag = irop_get_tag(sv);
            if (sv.is_sym || sv.is_lval || sv.is_complex)
            {
              conflict = 1;
              break;
            }
            if (tag != IROP_TAG_IMM32 && tag != IROP_TAG_F32)
            {
              conflict = 1;
              break;
            }
            uint32_t bits = (uint32_t)irop_get_imm64_ex(ir, sv);
            if (doff == real_off)
            {
              if (real_store_idx != -1 || j >= i)
              {
                conflict = 1;
                break;
              }
              real_store_idx = j;
              real_bits = bits;
            }
            else
            {
              if (imag_store_idx != -1 || j >= i)
              {
                conflict = 1;
                break;
              }
              imag_store_idx = j;
              imag_bits = bits;
            }
            continue;
          }
        }
      }

      /* Any other reference to the 8-byte slot disqualifies the fold. */
      for (int s = 0; s < 2; s++)
      {
        if (s == 0 && !irop_config[p->op].has_src1)
          continue;
        if (s == 1 && !irop_config[p->op].has_src2)
          continue;
        IROperand src = s ? tcc_ir_op_get_src2(ir, p) : tcc_ir_op_get_src1(ir, p);
        if (irop_get_tag(src) != IROP_TAG_STACKOFF)
          continue;
        if (irop_get_vreg(src) != -1)
          continue;
        int soff = (int)irop_get_stack_offset(src);
        if (soff >= real_off && soff < imag_off + 4)
        {
          conflict = 1;
          break;
        }
      }
    }

    if (conflict || real_store_idx < 0 || imag_store_idx < 0)
      continue;

    /* Pack {real, imag} into a 64-bit complex-float immediate. */
    uint64_t packed = (uint64_t)real_bits | ((uint64_t)imag_bits << 32);
    uint32_t pool_idx = tcc_ir_pool_add_i64(ir, (int64_t)packed);
    IROperand new_src1 = irop_make_i64(-1, pool_idx, IROP_BTYPE_FLOAT32);
    new_src1.is_complex = 1;
    new_src1.is_lval = 0;

    tcc_ir_set_src1(ir, i, new_src1);
    ir->compact_instructions[real_store_idx].op = TCCIR_OP_NOP;
    ir->compact_instructions[imag_store_idx].op = TCCIR_OP_NOP;

    LOG_IR_GEN("=== COMPLEX CONST PARAM FOLD: stack[%d..%d] folded into PARAM at i=%d ===", real_off, real_off + 7, i);
    changes++;
  }

  return changes;
}

/* Dead Call Result Elimination — convert FUNCCALLVAL → FUNCCALLVOID when
 * the call's destination vreg has no remaining uses.  Without this, the
 * codegen emits dead `mov rN, r0` (and `mov rN+1, r1` for 8-byte returns)
 * to copy the AAPCS return registers into the destination's allocated
 * registers, even though nothing will read them.
 *
 * Typical trigger: `_Complex float z = pure_callee(...);` where z is then
 * unused — after constant folding eats the args, the call still happens
 * (we can't prove purity), but its result is dead.
 *
 * The dest of a complex-returning FUNCCALLVAL is typically encoded as a
 * "temp local" (negative vreg sentinel) rather than a regular TEMP, so we
 * compare full vreg values rather than restricting to the TEMP type.
 */

/* Locate the sret-pointer parameter spill at the prolog: the first non-NOP
 * instruction should be `STORE LocalSlot[X] <-- P0`.  Returns 1 and fills
 * *out_param_vr / *out_slot if found; 0 otherwise. */
/* Analyze whether the current function is "pure via sret": its only
 * observable side effects are writes through the sret pointer (the first
 * parameter, which holds the caller's destination for a struct/complex
 * return).  Sets func_sym->f.func_pure_via_sret on success.
 *
 * Allowed operations:
 *   - Reads (LOAD, ASSIGN reading params/locals)
 *   - Writes to local stack slots (.is_local + IROP_TAG_STACKOFF)
 *   - Writes through pointers derived from the sret pointer
 *   - Calls to functions marked pure / const / pure_via_sret, or to known-
 *     pure aeabi runtime helpers
 *
 * Disallowed:
 *   - Writes to globals, volatile, or arbitrary pointers
 *   - Calls to unknown functions (could have side effects)
 *   - Inline asm, setjmp/longjmp, VLA SP manipulation
 */

int tcc_ir_opt_const_prop(TCCIRState *ir)
{
  /* VarConstInfo: track constant variables */
  typedef struct
  {
    uint8_t is_constant : 1;
    uint8_t def_count : 7;
    int64_t value;
    int def_idx;   /* instruction index of the defining STORE/ASSIGN */
    int use_count; /* count of source-operand uses (capped at 255) */
  } VarConstInfo;

  /* Returns 1 if materializing `val` into a register requires a multi-instruction
   * sequence (e.g. PC-relative pool load) on Thumb-2.  Small unsigned (≤0xFFFF
   * via MOVW) and small negative (≥-0xFFFF via MVN) fit in a single instruction;
   * other patterns generally don't.  Conservative — misses some "modified
   * immediate" encodings but that just means we propagate a few constants we
   * could have hoisted.  Used to suppress propagation of large constants that
   * would otherwise be loaded redundantly at each use site. */
  #define VAR_CONST_NEEDS_POOL_LOAD(val_)                                       \
    ({ uint32_t uv_ = (uint32_t)(val_); uv_ > 0xFFFFu && uv_ < 0xFFFF0001u; })

  int n = ir->next_instruction_index;
  int changes = 0;
  int max_var_pos = 0;
  int i;
  IRQuadCompact *q;
  VarConstInfo *var_info;

  if (n == 0)
    return 0;

  int dc_stride = 0;
  uint8_t *dc = (n <= 4000) ? ir_opt_build_def_count(ir, n, &dc_stride) : NULL;

  /* Combined pass: find max_var_pos AND fold identity comparisons in a single
   * scan.  The two concerns are orthogonal — one looks at VAR dests, the other
   * looks at CMP instructions followed by JUMPIF/SETIF.
   *
   * Identity comparison folding: fold CMP+JUMPIF and CMP+SETIF when both CMP
   * operands are the same vreg.  Comparing a value to itself always yields
   * equality, so == is true, != is false, <= and >= are true, etc.
   * Runs before the VAR-centric passes so it works even when there are no VAR
   * vregs (e.g. functions that only use parameters). */
  for (i = 0; i < n; i++)
  {
    q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Track max VAR position from destinations */
    if (irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dest_vr = irop_get_vreg(dest);
      if (TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_VAR)
      {
        const int pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
        if (pos > max_var_pos)
          max_var_pos = pos;
      }
    }

    /* Identity comparison folding — only for CMP followed by another instr */
    if (q->op != TCCIR_OP_CMP || i + 1 >= n)
      continue;

    IRQuadCompact *cmp_q = q;
    IROperand cmp_src1 = tcc_ir_op_get_src1(ir, cmp_q);
    IROperand cmp_src2 = tcc_ir_op_get_src2(ir, cmp_q);

    /* CMP commute peephole: CMP #imm, Vreg → CMP Vreg, #imm.
     * The backend can encode `cmp Rn, #imm8` as a 16-bit T1 instruction
     * (or `cmp.w Rn, #imm12` as T2). Keeping the immediate on the RHS
     * avoids materializing the constant into a register first.
     * When swapping operands, the consumer's comparison condition must be
     * swapped accordingly (LT<->GT, LE<->GE, EQ/NE unchanged).
     * Restricted to 32-bit integer comparisons against a register-resident
     * vreg: 64-bit CMPs decompose into hi/lo handled specially by codegen,
     * and DEREF/symbol/float operands have backend-specific encoding rules
     * that can be broken by a naive swap. */
    if (irop_is_immediate(cmp_src1) && !irop_is_immediate(cmp_src2)
        && irop_get_vreg(cmp_src2) >= 0
        && !cmp_src1.is_lval && !cmp_src2.is_lval
        && !cmp_src1.is_sym && !cmp_src2.is_sym
        && !cmp_src1.is_complex && !cmp_src2.is_complex)
    {
      int b1 = irop_get_btype(cmp_src1);
      int b2 = irop_get_btype(cmp_src2);
      int is_32bit_int = (b1 != IROP_BTYPE_INT64 && b1 != IROP_BTYPE_FLOAT32
                          && b1 != IROP_BTYPE_FLOAT64 && b2 != IROP_BTYPE_INT64
                          && b2 != IROP_BTYPE_FLOAT32 && b2 != IROP_BTYPE_FLOAT64);
      if (is_32bit_int)
      {
        IRQuadCompact *cons_q = &ir->compact_instructions[i + 1];
        int cond_pool_off = -1; /* offset into operand pool where cond is stored */
        if (cons_q->op == TCCIR_OP_JUMPIF || cons_q->op == TCCIR_OP_SETIF)
          cond_pool_off = irop_config[cons_q->op].has_dest; /* src1 slot */
        else if (cons_q->op == TCCIR_OP_SELECT)
          cond_pool_off = 3; /* dest, src1=then, src2=else, cond at +3 */

        if (cond_pool_off >= 0)
        {
          IROperand cur_cond = ir->iroperand_pool[cons_q->operand_base + cond_pool_off];
          int tok = (int)irop_get_imm64_ex(ir, cur_cond);
          int swapped = vrp_swap_cmp_tok(tok);
          if (swapped > 0)
          {
            tcc_ir_op_set_src1(ir, cmp_q, cmp_src2);
            tcc_ir_op_set_src2(ir, cmp_q, cmp_src1);
            int btype = irop_get_btype(cur_cond);
            ir->iroperand_pool[cons_q->operand_base + cond_pool_off] =
                irop_make_imm32(-1, swapped, btype);
            changes++;
            continue;
          }
        }
      }
    }

    /* Check if both operands are provably identical (identity comparison).
     * First check: same vreg with same is_lval flag.
     * Second check: different vregs but structurally equal expressions
     * (e.g. both compute "base + 5" via independent ADD instructions). */
    {
      int is_identity = 0;
      int32_t vr1 = irop_get_vreg(cmp_src1);
      int32_t vr2 = irop_get_vreg(cmp_src2);

      if (vr1 >= 0 && vr2 >= 0 && vr1 == vr2 && cmp_src1.is_lval == cmp_src2.is_lval)
      {
        /* Same vreg — check symbol refs for struct field disambiguation */
        if (cmp_src1.is_sym || cmp_src2.is_sym)
        {
          if (cmp_src1.is_sym == cmp_src2.is_sym)
          {
            IRPoolSymref *ref1 = irop_get_symref_ex(ir, cmp_src1);
            IRPoolSymref *ref2 = irop_get_symref_ex(ir, cmp_src2);
            if (ref1 && ref2 && ref1->sym == ref2->sym && ref1->addend == ref2->addend)
              is_identity = 1;
          }
        }
        else
          is_identity = 1;
      }

      /* Try definition-level equality for different vregs.
       * Compares the defining instructions directly (same op, same
       * operands), including cross-tag comparisons (VAR vs TEMP).
       * Guarded: find_defining_instruction is O(n) per CMP. */
      if (!is_identity && n <= 4000 && vr1 >= 0 && vr2 >= 0 && vr1 != vr2 &&
          DC_IS_SINGLE_DEF(dc, dc_stride, vr1) && DC_IS_SINGLE_DEF(dc, dc_stride, vr2))
      {
        int def1 = tcc_ir_find_defining_instruction(ir, vr1, i);
        int def2 = tcc_ir_find_defining_instruction(ir, vr2, i);
        if (def1 >= 0 && def2 >= 0 && ir_opt_pure_def_equal(ir, def1, def2, 0))
          is_identity = 1;
      }

      if (!is_identity)
        continue;
    }

    IRQuadCompact *next_q = &ir->compact_instructions[i + 1];

    if (next_q->op == TCCIR_OP_JUMPIF)
    {
      IROperand cond = tcc_ir_op_get_src1(ir, next_q);
      int tok = (int)irop_get_imm64_ex(ir, cond);
      /* evaluate_compare_condition(x, x, cond) — use 0,0 as representative */
      int result = evaluate_compare_condition(0, 0, tok);
      if (result < 0)
        continue;

      IROperand jmp_dest = tcc_ir_op_get_dest(ir, next_q);
      if (result)
      {
        /* Branch always taken — convert CMP to NOP, JUMPIF to unconditional JUMP */
        cmp_q->op = TCCIR_OP_NOP;
        next_q->op = TCCIR_OP_JUMP;
        tcc_ir_set_dest(ir, i + 1, jmp_dest);
      }
      else
      {
        /* Branch never taken — eliminate both */
        cmp_q->op = TCCIR_OP_NOP;
        next_q->op = TCCIR_OP_NOP;
      }
      changes++;
    }
    else if (next_q->op == TCCIR_OP_SETIF)
    {
      IROperand setif_src1 = tcc_ir_op_get_src1(ir, next_q);
      int tok = (int)irop_get_imm64_ex(ir, setif_src1);
      int result = evaluate_compare_condition(0, 0, tok);
      if (result < 0)
        continue;

      int btype = irop_get_btype(setif_src1);
      cmp_q->op = TCCIR_OP_NOP;
      next_q->op = TCCIR_OP_ASSIGN;
      IROperand new_src1 = irop_make_imm32(-1, result, btype);
      tcc_ir_set_src1(ir, i + 1, new_src1);
      tcc_ir_set_src2(ir, i + 1, IROP_NONE);
      changes++;
    }
  }

  /* max_var_pos tracks the highest VAR position seen.  When no VAR dests
   * exist at all, the subsequent VAR-only passes have nothing to do, but
   * the two-const fold and algebraic simplifications at the end of the
   * function are still needed (they're op-level, not VAR-level).
   * Use `has_var_dests` to distinguish "no VARs" from "only V0@pos=0". */
  int has_var_dests = 0;
  for (i = 0; i < n && !has_var_dests; i++)
  {
    q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (irop_config[q->op].has_dest)
    {
      int32_t dv = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
      if (TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR)
        has_var_dests = 1;
    }
  }

  var_info = has_var_dests ? tcc_mallocz(sizeof(VarConstInfo) * (max_var_pos + 1)) : NULL;

  /* First pass: identify constant variables (skip if no VAR dests) */
  if (has_var_dests)
    for (i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];

      if (q->op == TCCIR_OP_NOP)
        continue;

      /* Track definitions of VAR vregs */
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dest_vr = irop_get_vreg(dest);
      if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_VAR)
      {
        const int pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
        if (pos <= max_var_pos)
        {
          /* If the address of a local is taken, it can be modified through aliases
           * (e.g. passed as an out-parameter). Such variables are not safe for
           * constant propagation even if they are only assigned once.
           *
           * Complex types (_Complex float/double) are stored as register pairs
           * (real, imag) but the constant tracker only records a single scalar
           * value. Propagating that scalar would replace both halves with the
           * same value, corrupting the imaginary part.
           */
          IRLiveInterval *interval = tcc_ir_get_live_interval(ir, dest_vr);
          if (interval && (interval->addrtaken || interval->is_complex))
          {
            var_info[pos].def_count++;
            var_info[pos].is_constant = 0;
            continue;
          }

          var_info[pos].def_count++;

          /* Check if this is a constant assignment */
          IROperand src1 = tcc_ir_op_get_src1(ir, q);
          if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_STORE) && irop_is_immediate(src1))
          {
            if (var_info[pos].def_count == 1)
            {
              var_info[pos].is_constant = 1;
              var_info[pos].value = irop_get_imm64_ex(ir, src1);
              var_info[pos].def_idx = i;
            }
          }
          else
          {
            /* Non-constant assignment - mark as non-constant */
            var_info[pos].is_constant = 0;
          }
        }
      }
    }

  /* Mark variables with multiple definitions as non-constant */
  if (var_info)
    for (i = 0; i <= max_var_pos; i++)
    {
      if (var_info[i].def_count > 1)
        var_info[i].is_constant = 0;
    }

  /* Count source-operand uses of each VAR.  Used below to suppress
   * propagation of large constants with multiple uses — propagating a
   * pool-loaded value into N uses creates N materializations that the
   * regalloc can't undo, whereas keeping the VAR alive lets a single
   * load satisfy all reads. */
  if (var_info)
    for (i = 0; i < n; i++)
    {
      IRQuadCompact *uq = &ir->compact_instructions[i];
      if (uq->op == TCCIR_OP_NOP) continue;
      for (int oi = 0; oi < 2; oi++) {
        if (oi == 0 && !irop_config[uq->op].has_src1) continue;
        if (oi == 1 && !irop_config[uq->op].has_src2) continue;
        IROperand op = oi == 0 ? tcc_ir_op_get_src1(ir, uq) : tcc_ir_op_get_src2(ir, uq);
        int32_t vr = irop_get_vreg(op);
        if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR) continue;
        if (op.is_local && !op.is_lval) continue; /* address-of, not value */
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var_pos && var_info[pos].use_count < 255)
          var_info[pos].use_count++;
      }
    }

  /* Second pass: propagate constants and apply algebraic simplifications */
  for (i = 0; i < n; i++)
  {
    int src1_is_const, src2_is_const;
    int64_t result;
    int can_fold;
    int skip_bool_prop;

    q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_NOP)
      continue;

    /* For BOOL_AND/BOOL_OR, don't propagate constants unless both become constants.
     * The code generator can't handle mixed const/reg operands for these ops. */
    skip_bool_prop = 0;
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    if (q->op == TCCIR_OP_BOOL_AND || q->op == TCCIR_OP_BOOL_OR)
    {
      int src1_can_be_const = 0, src2_can_be_const = 0;
      /* Check if both would become constants */
      int32_t src1_vr = irop_get_vreg(src1);
      if (var_info && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
        if (pos <= max_var_pos && var_info[pos].is_constant)
          src1_can_be_const = 1;
      }
      else if (irop_is_immediate(src1))
        src1_can_be_const = 1;

      int32_t src2_vr = irop_get_vreg(src2);
      if (var_info && TCCIR_DECODE_VREG_TYPE(src2_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(src2_vr);
        if (pos <= max_var_pos && var_info[pos].is_constant)
          src2_can_be_const = 1;
      }
      else if (irop_is_immediate(src2))
        src2_can_be_const = 1;

      /* Skip propagation if only ONE would become constant (can't generate code) */
      if (src1_can_be_const != src2_can_be_const)
        skip_bool_prop = 1;
    }

    /* Propagate constant VAR vregs to immediate values.
     * IMPORTANT: Don't propagate if src1 is local without lval - that means
     * "address of local variable", not its value. The address must be computed at runtime. */
    int32_t src1_vr = irop_get_vreg(src1);
    if (var_info && !skip_bool_prop && irop_config[q->op].has_src1 &&
        TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
      if (src1.is_local && !src1.is_lval)
        continue;
      if (pos <= max_var_pos && var_info[pos].is_constant)
      {
        int64_t val = var_info[pos].value;
        /* Suppress propagation of large (pool-loaded) constants with
         * multiple uses — keep the VAR alive so a single load suffices. */
        if (var_info[pos].use_count > 1 && VAR_CONST_NEEDS_POOL_LOAD(val))
          continue;
        IROperand new_src1;
        int btype = irop_get_btype(src1);
        if (val == (int32_t)val)
        {
          new_src1 = irop_make_imm32(-1, (int32_t)val, btype);
        }
        else
        {
          uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
          new_src1 = irop_make_i64(-1, pool_idx, btype);
        }
        /* Preserve type flags but NOT memory-access flags.
         * is_lval/is_llocal/is_local describe stack-slot semantics that
         * don't apply to an immediate constant value. */
        new_src1.is_unsigned = src1.is_unsigned;
        new_src1.is_static = src1.is_static;
        tcc_ir_set_src1(ir, i, new_src1);
        if (q->op == TCCIR_OP_LOAD)
        {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          if (!d.is_lval && (btype == IROP_BTYPE_INT64 || val == (int32_t)val))
            q->op = TCCIR_OP_ASSIGN;
        }
        changes++;
      }
    }

    int32_t src2_vr = irop_get_vreg(src2);
    if (var_info && !skip_bool_prop && irop_config[q->op].has_src2 &&
        TCCIR_DECODE_VREG_TYPE(src2_vr) == TCCIR_VREG_TYPE_VAR && !(src2.is_local && !src2.is_lval))
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(src2_vr);
      if (pos <= max_var_pos && var_info[pos].is_constant)
      {
        int64_t val = var_info[pos].value;
        /* Same suppression as for src1 above. */
        if (var_info[pos].use_count > 1 && VAR_CONST_NEEDS_POOL_LOAD(val))
          continue;
        IROperand new_src2;
        int btype = irop_get_btype(src2);
        if (val == (int32_t)val)
        {
          new_src2 = irop_make_imm32(-1, (int32_t)val, btype);
        }
        else
        {
          uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
          new_src2 = irop_make_i64(-1, pool_idx, btype);
        }
        /* Preserve type flags but NOT memory-access flags. */
        new_src2.is_unsigned = src2.is_unsigned;
        new_src2.is_static = src2.is_static;
        tcc_ir_set_src2(ir, i, new_src2);
        changes++;
      }
    }

    /* Re-read operands after propagation to get updated values */
    src1 = tcc_ir_op_get_src1(ir, q);
    src2 = tcc_ir_op_get_src2(ir, q);

    /* Algebraic simplifications */
    src1_is_const = irop_config[q->op].has_src1 ? irop_is_immediate(src1) : 0;
    src2_is_const = irop_config[q->op].has_src2 ? irop_is_immediate(src2) : 0;

    /* For commutative operations, if src1 is const and src2 is not, swap them.
     * This ensures constants end up in src2 where the code generator expects them.
     * Note: BOOL_AND/BOOL_OR are not included because the code generator doesn't
     * handle constants in either operand - they require both to be registers. */
    if (irop_config[q->op].has_src1 && irop_config[q->op].has_src2 && src1_is_const && !src2_is_const)
    {
      int is_commutative = 0;
      switch (q->op)
      {
      case TCCIR_OP_ADD:
      case TCCIR_OP_MUL:
      case TCCIR_OP_AND:
      case TCCIR_OP_OR:
      case TCCIR_OP_XOR:
        is_commutative = 1;
        break;
      default:
        break;
      }
      if (is_commutative)
      {
        IROperand tmp;
        LOG_IR_GEN("OPTIMIZE: Swap operands for commutative %s (const in src1) at i=%d", tcc_ir_get_op_name(q->op), i);
        tmp = src1;
        src1 = src2;
        src2 = tmp;
        tcc_ir_set_src1(ir, i, src1);
        tcc_ir_set_src2(ir, i, src2);
        /* Update flags after swap */
        src1_is_const = 0;
        src2_is_const = 1;
      }
    }

    /* Full constant folding: C1 OP C2 = result */
    result = 0;
    can_fold = 1;

    if (irop_config[q->op].has_src1 && irop_config[q->op].has_src2 && src1_is_const && src2_is_const)
    {
      int64_t val1 = irop_get_imm64_ex(ir, src1);
      int64_t val2 = irop_get_imm64_ex(ir, src2);
      int btype = irop_get_btype(src1);

      switch (q->op)
      {
      case TCCIR_OP_ADD:
        result = (int64_t)((uint64_t)val1 + (uint64_t)val2);
        break;
      case TCCIR_OP_SUB:
        result = (int64_t)((uint64_t)val1 - (uint64_t)val2);
        break;
      case TCCIR_OP_MUL:
        result = (int64_t)((uint64_t)val1 * (uint64_t)val2);
        break;
      case TCCIR_OP_AND:
        result = val1 & val2;
        break;
      case TCCIR_OP_OR:
        result = val1 | val2;
        break;
      case TCCIR_OP_XOR:
        result = val1 ^ val2;
        break;
      case TCCIR_OP_SHL:
        result = (int64_t)((uint64_t)val1 << val2);
        break;
      case TCCIR_OP_SHR:
        if (btype == IROP_BTYPE_INT64)
          result = (uint64_t)val1 >> val2;
        else
          result = (uint32_t)val1 >> val2;
        break;
      case TCCIR_OP_SAR:
        result = val1 >> val2;
        break;
      case TCCIR_OP_ROR:
      {
        uint32_t v = (uint32_t)val1;
        uint32_t n = (uint32_t)val2 & 31;
        result = (int64_t)(int32_t)((v >> n) | (v << (32 - n)));
        break;
      }
      case TCCIR_OP_BOOL_AND:
        result = (val1 != 0) && (val2 != 0) ? 1 : 0;
        break;
      case TCCIR_OP_BOOL_OR:
        result = (val1 != 0) || (val2 != 0) ? 1 : 0;
        break;
      case TCCIR_OP_IMOD:
        if (val2 != 0)
        {
          result = val1 % val2;
        }
        else
        {
          can_fold = 0; /* Division by zero - don't fold */
        }
        break;
      case TCCIR_OP_DIV:
        if (val2 != 0)
        {
          result = val1 / val2;
        }
        else
        {
          can_fold = 0; /* Division by zero - don't fold */
        }
        break;
      case TCCIR_OP_UDIV:
        if (val2 != 0)
        {
          if (btype == IROP_BTYPE_INT64)
            result = (uint64_t)val1 / (uint64_t)val2;
          else
            result = (uint32_t)val1 / (uint32_t)val2;
        }
        else
        {
          can_fold = 0; /* Division by zero - don't fold */
        }
        break;
      case TCCIR_OP_UMOD:
        if (val2 != 0)
        {
          if (btype == IROP_BTYPE_INT64)
            result = (uint64_t)val1 % (uint64_t)val2;
          else
            result = (uint32_t)val1 % (uint32_t)val2;
        }
        else
        {
          can_fold = 0; /* Division by zero - don't fold */
        }
        break;
      case TCCIR_OP_UMULL:
      {
        uint64_t uresult = (uint64_t)(uint32_t)val1 * (uint64_t)(uint32_t)val2;
        result = (int64_t)uresult;
        btype = IROP_BTYPE_INT64;
        break;
      }
      case TCCIR_OP_UBFX:
      {
        int lsb = (int)val2 & 0x1F;
        int width = ((int)val2 >> 5) & 0x1F;
        if (width > 0 && width <= 32)
          result = ((uint32_t)val1 >> lsb) & ((1u << width) - 1);
        else
          can_fold = 0;
        break;
      }
      default:
        can_fold = 0;
        break;
      }

      /* Truncate to the operand's natural width so that 32-bit wrapping
       * arithmetic is modeled correctly (e.g. 0x80000000 + 0x80000000 wraps
       * to 0 in 32-bit).
       * Exception: SHL by >= 32 on a 32-bit type.  64-bit multiply chains
       * use 32-bit-typed temps with SHL #32 to position values in the upper
       * half of a register pair; truncating that to 0 is incorrect. */
      if (can_fold && btype != IROP_BTYPE_INT64 && btype != IROP_BTYPE_FLOAT64)
      {
        if (q->op == TCCIR_OP_SHL && val2 >= 32)
        {
          IROperand dest = tcc_ir_op_get_dest(ir, q);
          if (irop_get_btype(dest) == IROP_BTYPE_INT64)
            btype = IROP_BTYPE_INT64;
          else
            can_fold = 0;
        }
        else
          result = (int64_t)(int32_t)(uint32_t)result;
      }

      if (can_fold)
      {
        LOG_IR_GEN("OPTIMIZE: Constant fold %s(%lld, %lld) = %lld at i=%d", tcc_ir_get_op_name(q->op), (long long)val1,
                   (long long)val2, (long long)result, i);
        q->op = TCCIR_OP_ASSIGN;
        IROperand new_src1;
        if (result == (int32_t)result)
        {
          new_src1 = irop_make_imm32(-1, (int32_t)result, btype);
        }
        else
        {
          uint32_t pool_idx = tcc_ir_pool_add_i64(ir, result);
          new_src1 = irop_make_i64(-1, pool_idx, btype);
        }
        tcc_ir_set_src1(ir, i, new_src1);
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
        continue;
      }
    }

    /* Algebraic simplifications with one constant operand */
    if (irop_config[q->op].has_src2 && src2_is_const)
    {
      int64_t c = irop_get_imm64_ex(ir, src2);
      int simplify;
      int replace_with_zero;
      int replace_with_const;
      int64_t const_value;
      int btype = irop_get_btype(src1);

      simplify = 0;
      replace_with_zero = 0;
      replace_with_const = 0;
      const_value = 0;

      switch (q->op)
      {
      case TCCIR_OP_ADD:
      case TCCIR_OP_SUB:
        if (c == 0)
          simplify = 1; /* X + 0 = X, X - 0 = X */
        break;
      case TCCIR_OP_OR:
        if (c == 0)
          simplify = 1; /* X | 0 = X */
        else if (c == -1 || (btype != IROP_BTYPE_INT64 && c == 0xFFFFFFFF))
        {
          replace_with_const = 1; /* X | -1 = -1 */
          const_value = -1;
        }
        break;
      case TCCIR_OP_SHL:
      case TCCIR_OP_SHR:
      case TCCIR_OP_SAR:
      case TCCIR_OP_ROR:
        if (c == 0)
          simplify = 1; /* X << 0 = X, X >> 0 = X, X ror 0 = X */
        break;
      case TCCIR_OP_MUL:
        if (c == 1)
          simplify = 1; /* X * 1 = X */
        else if (c == 0)
          replace_with_zero = 1; /* X * 0 = 0 */
        break;
      case TCCIR_OP_DIV:
      case TCCIR_OP_UDIV:
        if (c == 1)
          simplify = 1; /* X / 1 = X */
        break;
      case TCCIR_OP_AND:
        if (c == 0)
          replace_with_zero = 1; /* X & 0 = 0 */
        else if (c == -1 || (btype != IROP_BTYPE_INT64 && c == 0xFFFFFFFF))
          simplify = 1; /* X & -1 = X */
        break;
      case TCCIR_OP_XOR:
        if (c == 0)
          simplify = 1; /* X ^ 0 = X */
        break;
      default:
        break;
      }

      if (simplify)
      {
        LOG_IR_GEN("OPTIMIZE: Algebraic simplify %s(x, %lld) = x at i=%d", tcc_ir_get_op_name(q->op), (long long)c, i);
        q->op = TCCIR_OP_ASSIGN;
        /* src1 stays as-is, clear src2 */
        tcc_ir_set_src1(ir, i, src1);
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
      }
      else if (replace_with_zero)
      {
        LOG_IR_GEN("OPTIMIZE: Algebraic simplify %s(x, %lld) = 0 at i=%d", tcc_ir_get_op_name(q->op), (long long)c, i);
        q->op = TCCIR_OP_ASSIGN;
        IROperand new_src1 = irop_make_imm32(-1, 0, btype);
        tcc_ir_set_src1(ir, i, new_src1);
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
      }
      else if (replace_with_const)
      {
        LOG_IR_GEN("OPTIMIZE: Algebraic simplify %s(x, %lld) = %lld at i=%d", tcc_ir_get_op_name(q->op), (long long)c,
                   (long long)const_value, i);
        q->op = TCCIR_OP_ASSIGN;
        IROperand new_src1;
        if (const_value == (int32_t)const_value)
        {
          new_src1 = irop_make_imm32(-1, (int32_t)const_value, btype);
        }
        else
        {
          uint32_t pool_idx = tcc_ir_pool_add_i64(ir, const_value);
          new_src1 = irop_make_i64(-1, pool_idx, btype);
        }
        tcc_ir_set_src1(ir, i, new_src1);
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
      }
    }

    /* Handle commutative operations: 0 + X = X, 0 << X = 0 */
    if (irop_config[q->op].has_src1 && src1_is_const)
    {
      const int64_t c = irop_get_imm64_ex(ir, src1);

      switch (q->op)
      {
      case TCCIR_OP_ADD:
      case TCCIR_OP_OR:
      case TCCIR_OP_XOR:
        if (c == 0)
        {
          /* 0 + X = X, 0 | X = X, 0 ^ X = X (commutative, swap operands) */
          LOG_IR_GEN("OPTIMIZE: Algebraic simplify %s(0, x) = x at i=%d", tcc_ir_get_op_name(q->op), i);
          q->op = TCCIR_OP_ASSIGN;
          tcc_ir_set_src1(ir, i, src2);
          tcc_ir_set_src2(ir, i, IROP_NONE);
          changes++;
        }
        break;
      case TCCIR_OP_MUL:
        if (c == 0)
        {
          /* 0 * X = 0 */
          LOG_IR_GEN("OPTIMIZE: Algebraic simplify %s(0, x) = 0 at i=%d", tcc_ir_get_op_name(q->op), i);
          q->op = TCCIR_OP_ASSIGN;
          /* src1 is already 0 */
          tcc_ir_set_src1(ir, i, src1);
          tcc_ir_set_src2(ir, i, IROP_NONE);
          changes++;
        }
        break;
      case TCCIR_OP_SHL:
      case TCCIR_OP_SHR:
      case TCCIR_OP_SAR:
      case TCCIR_OP_ROR:
        if (c == 0)
        {
          /* 0 << X = 0, 0 >> X = 0 */
          LOG_IR_GEN("OPTIMIZE: Algebraic simplify %s(0, x) = 0 at i=%d", tcc_ir_get_op_name(q->op), i);
          q->op = TCCIR_OP_ASSIGN;
          /* src1 is already 0 */
          tcc_ir_set_src1(ir, i, src1);
          tcc_ir_set_src2(ir, i, IROP_NONE);
          changes++;
        }
        break;
      default:
        break;
      }
    }
  }

  /* Byte-cast folding: SHL #N → SHR #N → AND #mask.
   * TCC emits (byte)x as SHL #24, SHR #24 (shift up then unsigned shift down).
   * Fold to AND #0xFF which the backend can emit as UXTB or UBFX.
   * Also fold SHL #16, SHR #16 → AND #0xFFFF (halfword cast). */
  for (i = 0; i < n - 1; i++)
  {
    IRQuadCompact *shl_q = &ir->compact_instructions[i];
    IRQuadCompact *shr_q = &ir->compact_instructions[i + 1];
    if (shl_q->op != TCCIR_OP_SHL || shr_q->op != TCCIR_OP_SHR)
      continue;
    IROperand shl_src2 = tcc_ir_op_get_src2(ir, shl_q);
    IROperand shr_src2 = tcc_ir_op_get_src2(ir, shr_q);
    if (!irop_is_immediate(shl_src2) || !irop_is_immediate(shr_src2))
      continue;
    int64_t shl_amt = irop_get_imm64_ex(ir, shl_src2);
    int64_t shr_amt = irop_get_imm64_ex(ir, shr_src2);
    if (shl_amt != shr_amt || shl_amt <= 0 || shl_amt >= 32)
      continue;
    /* Verify the SHR reads the SHL's dest */
    IROperand shl_dest = tcc_ir_op_get_dest(ir, shl_q);
    IROperand shr_src1 = tcc_ir_op_get_src1(ir, shr_q);
    if (irop_get_vreg(shl_dest) != irop_get_vreg(shr_src1))
      continue;
    /* Skip 64-bit types: the mask computation assumes 32-bit width.
     * For INT64, SHL #16 → SHR #16 masks 48 bits, not 16. */
    IROperand shl_orig_src1_chk = tcc_ir_op_get_src1(ir, shl_q);
    if (shl_orig_src1_chk.btype == IROP_BTYPE_INT64 || shl_orig_src1_chk.btype == IROP_BTYPE_FLOAT64)
      continue;
    /* SHL #N then SHR #N = AND with mask of (32-N) low bits */
    uint32_t mask = (shl_amt == 32) ? 0 : ((1u << (32 - shl_amt)) - 1);
    /* Replace SHL with AND, NOP the SHR */
    IROperand shl_orig_src1 = tcc_ir_op_get_src1(ir, shl_q);
    IROperand shr_dest = tcc_ir_op_get_dest(ir, shr_q);
    shr_q->op = TCCIR_OP_AND;
    tcc_ir_set_dest(ir, i + 1, shr_dest);
    tcc_ir_set_src1(ir, i + 1, shl_orig_src1);
    tcc_ir_set_src2(ir, i + 1, irop_make_imm32(-1, (int32_t)mask, IROP_BTYPE_INT32));
    shl_q->op = TCCIR_OP_NOP;
    changes++;
  }

  /* XOR cancellation: (x ^ C) ^ C = x.
   * Two consecutive XORs with the same constant cancel out. */
  for (i = 0; i < n - 1; i++)
  {
    IRQuadCompact *xor1_q = &ir->compact_instructions[i];
    IRQuadCompact *xor2_q = &ir->compact_instructions[i + 1];
    if (xor1_q->op != TCCIR_OP_XOR || xor2_q->op != TCCIR_OP_XOR)
      continue;
    IROperand xor1_src2 = tcc_ir_op_get_src2(ir, xor1_q);
    IROperand xor2_src2 = tcc_ir_op_get_src2(ir, xor2_q);
    if (!irop_is_immediate(xor1_src2) || !irop_is_immediate(xor2_src2))
      continue;
    if (irop_get_imm64_ex(ir, xor1_src2) != irop_get_imm64_ex(ir, xor2_src2))
      continue;
    IROperand xor1_dest = tcc_ir_op_get_dest(ir, xor1_q);
    IROperand xor2_src1 = tcc_ir_op_get_src1(ir, xor2_q);
    if (irop_get_vreg(xor1_dest) != irop_get_vreg(xor2_src1))
      continue;
    LOG_IR_GEN("OPTIMIZE: XOR cancel (x ^ %lld) ^ %lld = x at i=%d,%d", (long long)irop_get_imm64_ex(ir, xor1_src2),
               (long long)irop_get_imm64_ex(ir, xor2_src2), i, i + 1);
    IROperand xor1_src1 = tcc_ir_op_get_src1(ir, xor1_q);
    IROperand xor2_dest = tcc_ir_op_get_dest(ir, xor2_q);
    if (irop_get_vreg(xor1_src1) == irop_get_vreg(xor2_dest))
    {
      xor2_q->op = TCCIR_OP_NOP;
    }
    else
    {
      xor2_q->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_dest(ir, i + 1, xor2_dest);
      tcc_ir_set_src1(ir, i + 1, xor1_src1);
      tcc_ir_set_src2(ir, i + 1, IROP_NONE);
    }
    xor1_q->op = TCCIR_OP_NOP;
    changes++;
  }

  /* SHR+AND → UBFX fusion: SHR #N then AND #((1<<W)-1) → UBFX #N,#W.
   * This fuses two instructions into one ARM UBFX instruction. */
  for (i = 0; i < n - 1; i++)
  {
    IRQuadCompact *shr_q = &ir->compact_instructions[i];
    IRQuadCompact *and_q = &ir->compact_instructions[i + 1];
    if (shr_q->op != TCCIR_OP_SHR || and_q->op != TCCIR_OP_AND)
      continue;
    IROperand shr_src2 = tcc_ir_op_get_src2(ir, shr_q);
    IROperand and_src2 = tcc_ir_op_get_src2(ir, and_q);
    if (!irop_is_immediate(shr_src2) || !irop_is_immediate(and_src2))
      continue;
    int64_t shift = irop_get_imm64_ex(ir, shr_src2);
    int64_t mask = irop_get_imm64_ex(ir, and_src2);
    if (shift <= 0 || shift >= 32)
      continue;
    /* Check mask is (1<<W)-1 for W in {8,16} */
    int width = 0;
    if (mask == 0xFF)
      width = 8;
    else if (mask == 0xFFFF)
      width = 16;
    else
      continue;
    if (shift + width > 32)
      continue;
    /* Verify AND reads SHR's dest */
    IROperand shr_dest = tcc_ir_op_get_dest(ir, shr_q);
    IROperand and_src1 = tcc_ir_op_get_src1(ir, and_q);
    if (irop_get_vreg(shr_dest) != irop_get_vreg(and_src1))
      continue;
    /* UBFX can handle lval sources — the backend loads to a scratch register
     * first, then applies UBFX. This saves 1 instruction vs SHR+AND. */
    /* Verify SHR dest is single-use (only the AND) */
    if (!tcc_ir_vreg_has_single_use(ir, irop_get_vreg(shr_dest), i))
      continue;
    /* Fuse: NOP the SHR, change AND to UBFX with src2 = lsb|(width<<5) */
    IROperand shr_orig_src1 = tcc_ir_op_get_src1(ir, shr_q);
    IROperand and_dest = tcc_ir_op_get_dest(ir, and_q);
    int32_t ubfx_param = (int32_t)shift | (width << 5);
    and_q->op = TCCIR_OP_UBFX;
    tcc_ir_set_dest(ir, i + 1, and_dest);
    tcc_ir_set_src1(ir, i + 1, shr_orig_src1);
    tcc_ir_set_src2(ir, i + 1, irop_make_imm32(-1, ubfx_param, IROP_BTYPE_INT32));
    shr_q->op = TCCIR_OP_NOP;
    changes++;
  }

  /* Redundant AND elimination: SHR #N (N>=24) + AND #255 → just SHR #N.
   * After shifting right by 24+ bits on a 32-bit value, the result is
   * already 0-255, making AND #255 redundant.  This catches cases the
   * UBFX fusion skips (DEREF sources). */
  for (i = 0; i < n - 1; i++)
  {
    IRQuadCompact *shr_q = &ir->compact_instructions[i];
    IRQuadCompact *and_q = &ir->compact_instructions[i + 1];
    if (shr_q->op != TCCIR_OP_SHR || and_q->op != TCCIR_OP_AND)
      continue;
    IROperand shr_src2 = tcc_ir_op_get_src2(ir, shr_q);
    IROperand and_src2 = tcc_ir_op_get_src2(ir, and_q);
    if (!irop_is_immediate(shr_src2) || !irop_is_immediate(and_src2))
      continue;
    int64_t shift = irop_get_imm64_ex(ir, shr_src2);
    int64_t mask = irop_get_imm64_ex(ir, and_src2);
    if (shift < 24 || shift >= 32 || mask != 0xFF)
      continue;
    IROperand shr_dest = tcc_ir_op_get_dest(ir, shr_q);
    IROperand and_src1 = tcc_ir_op_get_src1(ir, and_q);
    if (irop_get_vreg(shr_dest) != irop_get_vreg(and_src1))
      continue;
    if (!tcc_ir_vreg_has_single_use(ir, irop_get_vreg(shr_dest), i))
      continue;
    /* Redirect AND's dest to SHR's dest and NOP the AND */
    IROperand and_dest = tcc_ir_op_get_dest(ir, and_q);
    tcc_ir_set_dest(ir, i, and_dest);
    and_q->op = TCCIR_OP_NOP;
    changes++;
  }

  /* Redundant AND elimination: AND #M + AND #M → single AND #M.
   * Also: AND #M1 + AND #M2 where M2 is a superset of M1 → AND #M1.
   * Common pattern from C casts: (uint8_t)x generates AND #255 twice. */
  for (i = 0; i < n - 1; i++)
  {
    IRQuadCompact *and1_q = &ir->compact_instructions[i];
    IRQuadCompact *and2_q = &ir->compact_instructions[i + 1];
    if (and1_q->op != TCCIR_OP_AND || and2_q->op != TCCIR_OP_AND)
      continue;
    IROperand and1_src2 = tcc_ir_op_get_src2(ir, and1_q);
    IROperand and2_src2 = tcc_ir_op_get_src2(ir, and2_q);
    if (!irop_is_immediate(and1_src2) || !irop_is_immediate(and2_src2))
      continue;
    int64_t mask1 = irop_get_imm64_ex(ir, and1_src2);
    int64_t mask2 = irop_get_imm64_ex(ir, and2_src2);
    /* Second AND is redundant if mask1 is a subset of mask2 (mask1 & mask2 == mask1) */
    if ((mask1 & mask2) != mask1)
      continue;
    IROperand and1_dest = tcc_ir_op_get_dest(ir, and1_q);
    IROperand and2_src1 = tcc_ir_op_get_src1(ir, and2_q);
    if (irop_get_vreg(and1_dest) != irop_get_vreg(and2_src1))
      continue;
    if (!tcc_ir_vreg_has_single_use(ir, irop_get_vreg(and1_dest), i))
      continue;
    IROperand and2_dest = tcc_ir_op_get_dest(ir, and2_q);
    tcc_ir_set_dest(ir, i, and2_dest);
    and2_q->op = TCCIR_OP_NOP;
    changes++;
  }

  /* Redundant AND after UBFX: UBFX produces a value already within
   * the extracted range, so a following AND with a superset mask is
   * redundant.  E.g. UBFX #8,#8 (result 0-255) + AND #255 → UBFX. */
  for (i = 0; i < n - 1; i++)
  {
    IRQuadCompact *ubfx_q = &ir->compact_instructions[i];
    IRQuadCompact *and_q = &ir->compact_instructions[i + 1];
    if (ubfx_q->op != TCCIR_OP_UBFX || and_q->op != TCCIR_OP_AND)
      continue;
    IROperand ubfx_src2 = tcc_ir_op_get_src2(ir, ubfx_q);
    IROperand and_src2 = tcc_ir_op_get_src2(ir, and_q);
    if (!irop_is_immediate(ubfx_src2) || !irop_is_immediate(and_src2))
      continue;
    int64_t ubfx_param = irop_get_imm64_ex(ir, ubfx_src2);
    int width = (ubfx_param >> 5) & 0x1F;
    if (width <= 0 || width > 31)
      continue;
    uint32_t ubfx_range = (1u << width) - 1;
    int64_t and_mask = irop_get_imm64_ex(ir, and_src2);
    if ((ubfx_range & and_mask) != ubfx_range)
      continue;
    IROperand ubfx_dest = tcc_ir_op_get_dest(ir, ubfx_q);
    IROperand and_src1 = tcc_ir_op_get_src1(ir, and_q);
    if (irop_get_vreg(ubfx_dest) != irop_get_vreg(and_src1))
      continue;
    if (!tcc_ir_vreg_has_single_use(ir, irop_get_vreg(ubfx_dest), i))
      continue;
    IROperand and_dest = tcc_ir_op_get_dest(ir, and_q);
    tcc_ir_set_dest(ir, i, and_dest);
    and_q->op = TCCIR_OP_NOP;
    changes++;
  }

  /* Third pass: Fold CMP+SETIF patterns when CMP has constant operands */
  for (i = 0; i < n - 1; i++)
  {
    IRQuadCompact *cmp_q = &ir->compact_instructions[i];
    IRQuadCompact *setif_q = &ir->compact_instructions[i + 1];
    int cmp_src1_const, cmp_src2_const;
    int64_t val1, val2;
    int cond, result;
    int btype;

    if (cmp_q->op != TCCIR_OP_CMP)
      continue;
    if (setif_q->op != TCCIR_OP_SETIF)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, cmp_q);
    IROperand src2 = tcc_ir_op_get_src2(ir, cmp_q);
    cmp_src1_const = irop_is_immediate(src1);
    cmp_src2_const = irop_is_immediate(src2);

    if (!cmp_src1_const || !cmp_src2_const)
      continue;

    val1 = irop_get_imm64_ex(ir, src1);
    val2 = irop_get_imm64_ex(ir, src2);
    IROperand setif_src1 = tcc_ir_op_get_src1(ir, setif_q);
    cond = (int)irop_get_imm64_ex(ir, setif_src1); /* Condition code stored as immediate (TCC token) */

    /* Evaluate the comparison based on TCC token values */
    result = 0;
    switch (cond)
    {
    case 0x94: /* TOK_EQ */
      result = (val1 == val2) ? 1 : 0;
      break;
    case 0x95: /* TOK_NE */
      result = (val1 != val2) ? 1 : 0;
      break;
    case 0x9c: /* TOK_LT */
      result = (val1 < val2) ? 1 : 0;
      break;
    case 0x9d: /* TOK_GE */
      result = (val1 >= val2) ? 1 : 0;
      break;
    case 0x9e: /* TOK_LE */
      result = (val1 <= val2) ? 1 : 0;
      break;
    case 0x9f: /* TOK_GT */
      result = (val1 > val2) ? 1 : 0;
      break;
    case 0x92: /* TOK_ULT (unsigned <) */
      result = ((uint64_t)val1 < (uint64_t)val2) ? 1 : 0;
      break;
    case 0x93: /* TOK_UGE (unsigned >=) */
      result = ((uint64_t)val1 >= (uint64_t)val2) ? 1 : 0;
      break;
    case 0x96: /* TOK_ULE (unsigned <=) */
      result = ((uint64_t)val1 <= (uint64_t)val2) ? 1 : 0;
      break;
    case 0x97: /* TOK_UGT (unsigned >) */
      result = ((uint64_t)val1 > (uint64_t)val2) ? 1 : 0;
      break;
    default:
      /* Unknown condition, don't fold */
      continue;
    }

    LOG_IR_GEN("OPTIMIZE: Fold CMP+SETIF const (%lld cmp %lld, cond=0x%x) = %d at i=%d", (long long)val1,
               (long long)val2, cond, result, i);

    /* Convert CMP to NOP and SETIF to ASSIGN with constant result.
     * Dead store elimination will remove the NOP. */
    cmp_q->op = TCCIR_OP_NOP;
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    setif_q->op = TCCIR_OP_ASSIGN;
    ir->compact_instructions[i + 1].op = TCCIR_OP_ASSIGN;

    btype = irop_get_btype(setif_src1);
    IROperand new_setif_src1 = irop_make_imm32(-1, result, btype);
    tcc_ir_set_src1(ir, i + 1, new_setif_src1);
    tcc_ir_set_src2(ir, i + 1, IROP_NONE);
    changes++;
  }

  /* Fourth pass: eliminate dead STORE/ASSIGN to constant VARs whose values
   * were fully propagated (no remaining vreg references as sources).
   * Only safe when the variable's address is not taken (no aliased reads). */
  if (var_info)
    for (i = 0; i <= max_var_pos; i++)
    {
      if (!var_info[i].is_constant)
        continue;

      int32_t vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, i);
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vr);
      if (!interval || interval->addrtaken || interval->is_complex)
        continue;

      /* Scan all instructions for any remaining use of this VAR as a source */
      int still_used = 0;
      for (int j = 0; j < n; j++)
      {
        IRQuadCompact *jq = &ir->compact_instructions[j];
        if (jq->op == TCCIR_OP_NOP)
          continue;

        if (irop_config[jq->op].has_src1)
        {
          int32_t src_vr = irop_get_vreg(tcc_ir_op_get_src1(ir, jq));
          if (src_vr == vr)
          {
            still_used = 1;
            break;
          }
        }
        if (irop_config[jq->op].has_src2)
        {
          int32_t src_vr = irop_get_vreg(tcc_ir_op_get_src2(ir, jq));
          if (src_vr == vr)
          {
            still_used = 1;
            break;
          }
        }
      }

      if (!still_used)
      {
        int di = var_info[i].def_idx;
        if (di >= 0 && di < n && ir->compact_instructions[di].op != TCCIR_OP_NOP)
        {
          LOG_IR_GEN("OPTIMIZE: Dead constant VAR store at i=%d (V%d=#%lld, no remaining uses)", di, i,
                     (long long)var_info[i].value);
          ir->compact_instructions[di].op = TCCIR_OP_NOP;
          changes++;
        }
      }
    }

  tcc_free(dc);
  tcc_free(var_info);

  return changes;
}

/* ============================================================================
 * Phase 2: Value Tracking through Arithmetic
 * ============================================================================
 *
 * Track constant values through arithmetic operations (ADD, SUB) to enable
 * folding of comparisons where a vreg has a known constant value.
 *
 * Example:
 *   V0 <- #1234 [ASSIGN]           ; V0 = 1234
 *   V0 <- V0 SUB #42               ; V0 = 1192 (still constant!)
 *   CMP V0, #1000000               ; 1192 <= 1000000, always true
 *   JMP to X if "<=S"              ; Can fold to unconditional JUMP
 */

/* Track constant values for vregs through arithmetic.
 * Uses generation counters for O(1) bulk invalidation instead of O(max_vreg)
 * loops.  This makes the pass O(n) instead of O(n × max_vreg). */
typedef struct
{
  int gen;       /* entry valid when gen == current_gen */
  int def_gen;   /* def_idx valid when def_gen == current_def_gen */
  int64_t value; /* The constant value */
  int def_idx;   /* instruction index of last constant def (-1 = none/read) */
} VRegConstState;

/* LEA map entry with generation counter */
typedef struct
{
  int gen;     /* valid when gen == current_lea_gen */
  int var_pos; /* VAR position this TMP points to */
} LeaMapGenEntry;

/* Helper: check if state entry is a known constant in current generation */
#define VT_IS_CONST(st, pos) ((st)[pos].gen == vt_gen)
/* Helper: check if def_idx is valid in current def generation */
#define VT_HAS_DEF(st, pos) ((st)[pos].def_gen == vt_def_gen && (st)[pos].def_idx >= 0)
/* Helper: set state as constant */
#define VT_SET_CONST(st, pos, val_)                                                                                    \
  do                                                                                                                   \
  {                                                                                                                    \
    (st)[pos].gen = vt_gen;                                                                                            \
    (st)[pos].def_gen = vt_def_gen;                                                                                    \
    (st)[pos].value = (val_);                                                                                          \
    (st)[pos].def_idx = -1;                                                                                            \
  } while (0)
/* Helper: set state as constant with def tracking */
#define VT_SET_CONST_DEF(st, pos, val_, idx_)                                                                          \
  do                                                                                                                   \
  {                                                                                                                    \
    (st)[pos].gen = vt_gen;                                                                                            \
    (st)[pos].def_gen = vt_def_gen;                                                                                    \
    (st)[pos].value = (val_);                                                                                          \
    (st)[pos].def_idx = (idx_);                                                                                        \
  } while (0)
/* Helper: invalidate constant state for a position */
#define VT_INVALIDATE(st, pos)                                                                                         \
  do                                                                                                                   \
  {                                                                                                                    \
    (st)[pos].gen = 0;                                                                                                 \
  } while (0)
/* Helper: invalidate def_idx only (keep constant value) */
#define VT_CLEAR_DEF(st, pos)                                                                                          \
  do                                                                                                                   \
  {                                                                                                                    \
    (st)[pos].def_gen = 0;                                                                                             \
  } while (0)

/* Maximum number of addrtaken vregs to track for fast STORE/CALL invalidation.
 * Beyond this limit, falls back to full scan. */
#define VT_MAX_ADDRTAKEN 64

int tcc_ir_opt_value_tracking(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  int max_vreg = 0;
  int max_tmp = 0;

  if (n == 0)
    return 0;

  /* Single pre-scan: build merge-point bitmap AND find max vreg/tmp positions.
   * Merges 3 separate O(n) scans into 1. */
  uint8_t *is_merge = tcc_mallocz((n + 7) / 8);
  int *pred_count = tcc_mallocz(n * sizeof(int));

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* Track max vreg/tmp positions while scanning */
    if (q->op != TCCIR_OP_NOP)
    {
      IROperand ops[3];
      ops[0] = tcc_ir_op_get_dest(ir, q);
      ops[1] = tcc_ir_op_get_src1(ir, q);
      ops[2] = tcc_ir_op_get_src2(ir, q);
      for (int k = 0; k < 3; k++)
      {
        int32_t vr = irop_get_vreg(ops[k]);
        if (vr >= 0)
        {
          int type = TCCIR_DECODE_VREG_TYPE(vr);
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (type == TCCIR_VREG_TYPE_VAR && pos > max_vreg)
            max_vreg = pos;
          else if (type == TCCIR_VREG_TYPE_TEMP && pos > max_tmp)
            max_tmp = pos;
        }
      }
    }

    /* Build pred_count and is_merge */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)dest.u.imm32;
      if (target >= 0 && target < n)
      {
        pred_count[target]++;
        /* Back-edge: jump from later instruction to earlier one - always a merge point */
        if (i > target)
          is_merge[target / 8] |= (1 << (target % 8));
      }
    }
    /* SWITCH_TABLE: all case targets are merge points */
    if (q->op == TCCIR_OP_SWITCH_TABLE)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int table_id = (int)irop_get_imm64_ex(ir, src2);
      if (table_id >= 0 && table_id < ir->num_switch_tables)
      {
        TCCIRSwitchTable *table = &ir->switch_tables[table_id];
        for (int j = 0; j < table->num_entries; j++)
        {
          int t = table->targets[j];
          if (t >= 0 && t < n)
            pred_count[t]++;
        }
        if (table->default_target >= 0 && table->default_target < n)
          pred_count[table->default_target]++;
      }
    }
    /* Fall-through predecessor (SWITCH_TABLE is a terminator — no fall-through) */
    if (i + 1 < n && q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_NOP && q->op != TCCIR_OP_RETURNVALUE &&
        q->op != TCCIR_OP_RETURNVOID && q->op != TCCIR_OP_SWITCH_TABLE)
    {
      pred_count[i + 1]++;
    }
  }
  /* Mark instructions with multiple predecessors as merge points */
  for (int i = 0; i < n; i++)
  {
    if (pred_count[i] > 1)
      is_merge[i / 8] |= (1 << (i % 8));
  }
  tcc_free(pred_count);

  /* Detect VLA — SHL folding is unsafe in functions with VLA because
   * it can disrupt VLA stack save/restore patterns in nested scopes. */
  int has_vla = 0;
  for (int vi = 0; vi < n && !has_vla; vi++)
  {
    TccIrOp vop = ir->compact_instructions[vi].op;
    if (vop == TCCIR_OP_VLA_ALLOC || vop == TCCIR_OP_VLA_SP_SAVE || vop == TCCIR_OP_VLA_SP_RESTORE)
      has_vla = 1;
  }

  /* Note: do NOT return early when max_vreg == 0.  The loop also
   * constant-folds __aeabi_lcmp/ulcmp calls with immediate args,
   * which doesn't require any tracked VARs. */

  VRegConstState *state = tcc_mallocz(sizeof(VRegConstState) * (max_vreg + 1));

  /* LEA tracking with generation counters */
  LeaMapGenEntry *lea_map = tcc_mallocz(sizeof(LeaMapGenEntry) * (max_tmp + 1));
  LeaMapGenEntry *lea_var_map = tcc_mallocz(sizeof(LeaMapGenEntry) * (max_vreg + 1));

  /* Generation counters — bumping invalidates all entries in O(1) */
  int vt_gen = 1;     /* state[].gen must match for is_constant to be valid */
  int vt_def_gen = 1; /* state[].def_gen must match for def_idx to be valid */
  int vt_lea_gen = 1; /* lea_map[].gen must match for entry to be valid */
  int vt_in_dead_zone = 0;

  /* Track addrtaken constant vregs for fast STORE/CALL invalidation.
   * Instead of scanning all max_vreg entries, we only iterate this small list. */
  int addrtaken_list[VT_MAX_ADDRTAKEN];
  int num_addrtaken = 0;
  int addrtaken_overflow = 0; /* 1 = list full, must fall back to full scan */

  /* Pre-build addrtaken bitmap for quick lookup during constant tracking */
  uint8_t *is_addrtaken = tcc_mallocz((max_vreg + 8) / 8);
  for (int v = 0; v <= max_vreg; v++)
  {
    int32_t vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, v);
    IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vr);
    if (interval && interval->addrtaken)
      is_addrtaken[v / 8] |= (1 << (v % 8));
  }

  /* Forward pass: track values through the IR */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* Clear state at merge points — O(1) via generation bump */
    if (is_merge[i / 8] & (1 << (i % 8)))
    {
      vt_gen++;
      vt_def_gen++;
      vt_lea_gen++;
      num_addrtaken = 0;
      addrtaken_overflow = 0;
    }

    /* After a terminator, the next instruction is NOT a fall-through successor.
     * Clear state — O(1) via generation bump.
     * Exception: after RETURNVALUE/RETURNVOID, if the next instruction is NOT a
     * merge point it has exactly one predecessor (a JUMPIF).  The JUMPIF preserves
     * constant state, so we keep propagating.  A dead unconditional JUMP between
     * RETURNVALUE and the target (emitted but unreachable) is harmless — it
     * doesn't modify any VARs, so we stay in the "post-return" zone until we hit
     * a merge point or the dead code ends. */
    if (i > 0)
    {
      IRQuadCompact *prev = &ir->compact_instructions[i - 1];
      if (prev->op == TCCIR_OP_JUMP || prev->op == TCCIR_OP_RETURNVALUE || prev->op == TCCIR_OP_RETURNVOID ||
          prev->op == TCCIR_OP_SWITCH_TABLE)
      {
        int skip_clear = 0;
        if (prev->op == TCCIR_OP_RETURNVALUE || prev->op == TCCIR_OP_RETURNVOID)
          vt_in_dead_zone = 1;
        if (vt_in_dead_zone && !(is_merge[i / 8] & (1 << (i % 8))))
          skip_clear = 1;
        else
          vt_in_dead_zone = 0;
        if (!skip_clear)
        {
          vt_gen++;
          vt_def_gen++;
          vt_lea_gen++;
          num_addrtaken = 0;
          addrtaken_overflow = 0;
        }
      }
      else
        vt_in_dead_zone = 0;
    }

    if (q->op == TCCIR_OP_NOP)
      continue;

    /* A conditional branch creates an alternative path where current defs
     * may still be live.  Clear def_idx only — O(1) via def generation bump.
     * Constant values remain valid (is_constant preserved). */
    if (q->op == TCCIR_OP_JUMPIF)
    {
      vt_def_gen++;
      continue;
    }

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    int32_t dest_vr = irop_get_vreg(dest);
    int dest_pos = (dest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_VAR)
                       ? TCCIR_DECODE_VREG_POSITION(dest_vr)
                       : -1;

    /* LEA tracking: T = &V — record that TMP T points to VAR V */
    if (q->op == TCCIR_OP_LEA)
    {
      int32_t src1_vr = irop_get_vreg(src1);
      if (dest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP && src1_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int tmp_pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
        int var_pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
        if (tmp_pos <= max_tmp && var_pos <= max_vreg)
        {
          lea_map[tmp_pos].gen = vt_lea_gen;
          lea_map[tmp_pos].var_pos = var_pos;
          LOG_IR_GEN("VALUE_TRACK LEA: i=%d T%d -> V%d", i, tmp_pos, var_pos);
        }
      }
      LOG_IR_GEN("VALUE_TRACK LEA SKIP: i=%d dest_vr=0x%x dest_type=%d src1_vr=0x%x src1_type=%d", i, dest_vr,
                 dest_vr >= 0 ? TCCIR_DECODE_VREG_TYPE(dest_vr) : -1, irop_get_vreg(src1),
                 irop_get_vreg(src1) >= 0 ? TCCIR_DECODE_VREG_TYPE(irop_get_vreg(src1)) : -1);
      continue;
    }

    /* STORE through LEA: *T = value — if T = &V, propagate value to V. */
    if (q->op == TCCIR_OP_STORE)
    {
      int32_t addr_vr = irop_get_vreg(dest);
      if (addr_vr >= 0 && TCCIR_DECODE_VREG_TYPE(addr_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int tmp_pos = TCCIR_DECODE_VREG_POSITION(addr_vr);
        if (tmp_pos <= max_tmp && lea_map[tmp_pos].gen == vt_lea_gen)
        {
          int var_pos = lea_map[tmp_pos].var_pos;
          if (var_pos <= max_vreg)
          {
            if (irop_is_immediate(src1))
            {
              VT_SET_CONST(state, var_pos, irop_get_imm64_ex(ir, src1));
              /* Track addrtaken for fast invalidation */
              if (is_addrtaken[var_pos / 8] & (1 << (var_pos % 8)))
              {
                if (!addrtaken_overflow && num_addrtaken < VT_MAX_ADDRTAKEN)
                  addrtaken_list[num_addrtaken++] = var_pos;
                else
                  addrtaken_overflow = 1;
              }
              LOG_IR_GEN("VALUE_TRACK STORE: i=%d V%d = %lld (via T%d)", i, var_pos, (long long)state[var_pos].value,
                         tmp_pos);
            }
            else
            {
              /* Non-constant store → invalidate tracked value */
              VT_INVALIDATE(state, var_pos);
            }
          }
        }
      }
      /* Direct VAR store: V = T — propagate LEA if src is a LEA result */
      else if (dest_pos >= 0)
      {
        int lea_propagated = 0;
        int32_t src_vr = irop_get_vreg(src1);
        if (src_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src_vr) == TCCIR_VREG_TYPE_TEMP)
        {
          int src_tmp = TCCIR_DECODE_VREG_POSITION(src_vr);
          if (src_tmp <= max_tmp && lea_map[src_tmp].gen == vt_lea_gen)
          {
            lea_var_map[dest_pos].gen = vt_lea_gen;
            lea_var_map[dest_pos].var_pos = lea_map[src_tmp].var_pos;
            lea_propagated = 1;
            LOG_IR_GEN("VALUE_TRACK LEA-VAR: i=%d V%d -> V%d (via T%d)", i, dest_pos, lea_map[src_tmp].var_pos,
                       src_tmp);
          }
        }
        /* src1 VAR is read here — mark its def as consumed so the
         * dead-def elimination won't kill the defining instruction. */
        if (src_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src_vr) == TCCIR_VREG_TYPE_VAR)
        {
          int src_pos = TCCIR_DECODE_VREG_POSITION(src_vr);
          if (src_pos >= 0 && src_pos <= max_vreg)
            VT_CLEAR_DEF(state, src_pos);
        }
        if (!lea_propagated && dest_pos <= max_vreg)
          lea_var_map[dest_pos].gen = 0;
      }
      /* Any STORE through an unknown pointer could alias any address-taken var.
       * Iterate only the tracked addrtaken list — O(k) instead of O(max_vreg). */
      else
      {
        if (!addrtaken_overflow)
        {
          for (int a = 0; a < num_addrtaken; a++)
          {
            int v = addrtaken_list[a];
            if (VT_IS_CONST(state, v))
              VT_INVALIDATE(state, v);
          }
        }
        else
        {
          /* Overflow fallback: scan all vregs (rare) */
          for (int v = 0; v <= max_vreg; v++)
          {
            if (VT_IS_CONST(state, v) && (is_addrtaken[v / 8] & (1 << (v % 8))))
              VT_INVALIDATE(state, v);
          }
        }
      }
      continue;
    }

    /* LEA propagation through VAR: T = V where V holds a LEA result */
    if (q->op == TCCIR_OP_ASSIGN && dest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int32_t src_vr = irop_get_vreg(src1);
      if (src_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int src_var = TCCIR_DECODE_VREG_POSITION(src_vr);
        if (src_var <= max_vreg && lea_var_map[src_var].gen == vt_lea_gen)
        {
          int dest_tmp = TCCIR_DECODE_VREG_POSITION(dest_vr);
          if (dest_tmp <= max_tmp)
          {
            lea_map[dest_tmp].gen = vt_lea_gen;
            lea_map[dest_tmp].var_pos = lea_var_map[src_var].var_pos;
            LOG_IR_GEN("VALUE_TRACK LEA-TMP: i=%d T%d -> V%d (via V%d)", i, dest_tmp, lea_var_map[src_var].var_pos,
                       src_var);
          }
        }
      }
    }

    /* Pattern 1: Direct constant assignment: Vx <- #const */
    if (q->op == TCCIR_OP_ASSIGN && irop_is_immediate(src1))
    {
      if (dest_pos >= 0 && dest_pos <= max_vreg)
      {
        /* If the address of this variable is taken, it can be modified
         * through aliases.  Do not track it as constant. */
        if (is_addrtaken[dest_pos / 8] & (1 << (dest_pos % 8)))
        {
          VT_INVALIDATE(state, dest_pos);
        }
        else
        {
          /* Previous unread constant def is dead — NOP it */
          if (VT_IS_CONST(state, dest_pos) && VT_HAS_DEF(state, dest_pos))
          {
            ir->compact_instructions[state[dest_pos].def_idx].op = TCCIR_OP_NOP;
            changes++;
          }
          VT_SET_CONST_DEF(state, dest_pos, irop_get_imm64_ex(ir, src1), i);
        }
      }
      continue;
    }

    /* Pattern 2: Arithmetic/bitwise with constant operand: Vx <- Vy op #const
     * SHL/SHR/SAR/MUL included: merge-point invalidation at loop headers
     * prevents constant folding of live IVs inside loops, so straight-line
     * folds are safe. */
    if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_XOR || q->op == TCCIR_OP_AND ||
         q->op == TCCIR_OP_OR || (!has_vla && q->op == TCCIR_OP_SHL) || q->op == TCCIR_OP_SHR ||
         q->op == TCCIR_OP_SAR || q->op == TCCIR_OP_MUL) &&
        irop_is_immediate(src2))
    {
      int32_t src1_vr = irop_get_vreg(src1);
      int src1_pos = (src1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR)
                         ? TCCIR_DECODE_VREG_POSITION(src1_vr)
                         : -1;

      /* Check if src1 is a known constant AND src2 is immediate */
      if (src1_pos >= 0 && src1_pos <= max_vreg && VT_IS_CONST(state, src1_pos))
      {
        int64_t val1 = state[src1_pos].value;
        int64_t val2 = irop_get_imm64_ex(ir, src2);
        int btype = irop_get_btype(src1);
        int is_64 = (btype == IROP_BTYPE_INT64 || btype == IROP_BTYPE_FLOAT64);
        int64_t result;
        int shift_mask = is_64 ? 63 : 31;
        switch (q->op)
        {
        case TCCIR_OP_ADD:
          result = val1 + val2;
          break;
        case TCCIR_OP_SUB:
          result = val1 - val2;
          break;
        case TCCIR_OP_XOR:
          result = val1 ^ val2;
          break;
        case TCCIR_OP_AND:
          result = val1 & val2;
          break;
        case TCCIR_OP_OR:
          result = val1 | val2;
          break;
        case TCCIR_OP_MUL:
          result = val1 * val2;
          break;
        case TCCIR_OP_SHL:
          result = (int64_t)((uint64_t)val1 << (val2 & shift_mask));
          break;
        case TCCIR_OP_SHR:
          if (is_64)
            result = (int64_t)((uint64_t)val1 >> (val2 & 63));
          else
            result = (int64_t)((uint32_t)val1 >> (val2 & 31));
          break;
        case TCCIR_OP_SAR:
          if (is_64)
            result = val1 >> (val2 & 63);
          else
            result = (int64_t)((int32_t)val1 >> (val2 & 31));
          break;
        case TCCIR_OP_ROR:
        {
          uint32_t v = (uint32_t)val1;
          uint32_t n = (uint32_t)val2 & 31;
          result = (int64_t)(int32_t)((v >> n) | (v << (32 - n)));
          break;
        }
        default:
          result = 0;
          break;
        }
        if (!is_64 && q->op != TCCIR_OP_SHR && q->op != TCCIR_OP_SAR)
          result = (int64_t)(int32_t)(uint32_t)result;

        LOG_IR_GEN("OPTIMIZE: Constant fold %s(%lld, %lld) = %lld at i=%d", tcc_ir_get_op_name(q->op), (long long)val1,
                   (long long)val2, (long long)result, i);

        /* Fold: replace op with constant ASSIGN */
        q->op = TCCIR_OP_ASSIGN;
        if (result == (int32_t)result)
          tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)result, btype));
        else
        {
          uint32_t pool_idx = tcc_ir_pool_add_i64(ir, result);
          tcc_ir_set_src1(ir, i, irop_make_i64(-1, pool_idx, btype));
        }
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;

        if (dest_pos >= 0 && dest_pos <= max_vreg)
        {
          /* Do not propagate constant through address-taken variables */
          if (is_addrtaken[dest_pos / 8] & (1 << (dest_pos % 8)))
          {
            VT_INVALIDATE(state, dest_pos);
          }
          else
          {
            /* Previous unread constant def is dead — NOP it */
            if (VT_IS_CONST(state, dest_pos) && VT_HAS_DEF(state, dest_pos))
            {
              ir->compact_instructions[state[dest_pos].def_idx].op = TCCIR_OP_NOP;
              changes++;
            }
            VT_SET_CONST_DEF(state, dest_pos, result, i);
          }
        }
      }
      else
      {
        /* src1 is read but not folded — mark its def as live */
        if (src1_pos >= 0 && src1_pos <= max_vreg)
          VT_CLEAR_DEF(state, src1_pos);
        /* Destination no longer has known constant value */
        if (dest_pos >= 0 && dest_pos <= max_vreg)
          VT_INVALIDATE(state, dest_pos);
      }
      continue;
    }

    /* Pattern 2a: Arithmetic where src2 is a known-constant VAR.
     * Handles `T ADD V0` where V0 is tracked as constant — substitute src2
     * with the immediate value.  If src1 is also immediate, fold entirely. */
    if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_XOR || q->op == TCCIR_OP_AND ||
         q->op == TCCIR_OP_OR || (!has_vla && q->op == TCCIR_OP_SHL) || q->op == TCCIR_OP_SHR ||
         q->op == TCCIR_OP_SAR || q->op == TCCIR_OP_MUL) &&
        !irop_is_immediate(src2))
    {
      int32_t src2_vr = irop_get_vreg(src2);
      int src2_pos = (src2_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src2_vr) == TCCIR_VREG_TYPE_VAR)
                         ? TCCIR_DECODE_VREG_POSITION(src2_vr)
                         : -1;

      if (src2_pos >= 0 && src2_pos <= max_vreg && VT_IS_CONST(state, src2_pos))
      {
        int64_t val2 = state[src2_pos].value;
        int btype = irop_get_btype(src2);
        int is_64 = (btype == IROP_BTYPE_INT64 || btype == IROP_BTYPE_FLOAT64);

        /* Check if src1 is also a known constant (immediate or tracked VAR) */
        int src1_const = 0;
        int64_t val1 = 0;
        if (irop_is_immediate(src1))
        {
          src1_const = 1;
          val1 = irop_get_imm64_ex(ir, src1);
        }
        else
        {
          int32_t src1_vr2 = irop_get_vreg(src1);
          int s1_pos = (src1_vr2 >= 0 && TCCIR_DECODE_VREG_TYPE(src1_vr2) == TCCIR_VREG_TYPE_VAR)
                           ? TCCIR_DECODE_VREG_POSITION(src1_vr2)
                           : -1;
          if (s1_pos >= 0 && s1_pos <= max_vreg && VT_IS_CONST(state, s1_pos))
          {
            src1_const = 1;
            val1 = state[s1_pos].value;
          }
        }

        if (src1_const)
        {
          /* Both operands are constants — fold entirely */
          int64_t result;
          int shift_mask = is_64 ? 63 : 31;
          switch (q->op)
          {
          case TCCIR_OP_ADD:
            result = val1 + val2;
            break;
          case TCCIR_OP_SUB:
            result = val1 - val2;
            break;
          case TCCIR_OP_XOR:
            result = val1 ^ val2;
            break;
          case TCCIR_OP_AND:
            result = val1 & val2;
            break;
          case TCCIR_OP_OR:
            result = val1 | val2;
            break;
          case TCCIR_OP_MUL:
            result = val1 * val2;
            break;
          case TCCIR_OP_SHL:
            result = (int64_t)((uint64_t)val1 << (val2 & shift_mask));
            break;
          case TCCIR_OP_SHR:
            if (is_64)
              result = (int64_t)((uint64_t)val1 >> (val2 & 63));
            else
              result = (int64_t)((uint32_t)val1 >> (val2 & 31));
            break;
          case TCCIR_OP_SAR:
            if (is_64)
              result = val1 >> (val2 & 63);
            else
              result = (int64_t)((int32_t)val1 >> (val2 & 31));
            break;
          case TCCIR_OP_ROR:
          {
            uint32_t v = (uint32_t)val1;
            uint32_t n = (uint32_t)val2 & 31;
            result = (int64_t)(int32_t)((v >> n) | (v << (32 - n)));
            break;
          }
          default:
            result = 0;
            break;
          }
          if (!is_64 && q->op != TCCIR_OP_SHR && q->op != TCCIR_OP_SAR)
            result = (int64_t)(int32_t)(uint32_t)result;

          LOG_IR_GEN("VALUE_TRACK 2a FOLD: i=%d %s(%lld, %lld) = %lld", i, tcc_ir_get_op_name(q->op), (long long)val1,
                     (long long)val2, (long long)result);

          q->op = TCCIR_OP_ASSIGN;
          if (result == (int32_t)result)
            tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)result, btype));
          else
          {
            uint32_t pool_idx = tcc_ir_pool_add_i64(ir, result);
            tcc_ir_set_src1(ir, i, irop_make_i64(-1, pool_idx, btype));
          }
          tcc_ir_set_src2(ir, i, IROP_NONE);
          changes++;

          if (dest_pos >= 0 && dest_pos <= max_vreg)
          {
            if (is_addrtaken[dest_pos / 8] & (1 << (dest_pos % 8)))
              VT_INVALIDATE(state, dest_pos);
            else
            {
              if (VT_IS_CONST(state, dest_pos) && VT_HAS_DEF(state, dest_pos))
              {
                ir->compact_instructions[state[dest_pos].def_idx].op = TCCIR_OP_NOP;
                changes++;
              }
              VT_SET_CONST_DEF(state, dest_pos, result, i);
            }
          }
        }
        else
        {
          /* Only src2 is constant — substitute it with immediate */
          LOG_IR_GEN("VALUE_TRACK 2a SUBST: i=%d src2 V%d -> #%lld", i, src2_pos, (long long)val2);
          if (val2 == (int32_t)val2)
            tcc_ir_set_src2(ir, i, irop_make_imm32(-1, (int32_t)val2, btype));
          else
          {
            uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val2);
            tcc_ir_set_src2(ir, i, irop_make_i64(-1, pool_idx, btype));
          }
          changes++;

          if (dest_pos >= 0 && dest_pos <= max_vreg)
            VT_INVALIDATE(state, dest_pos);
        }
        /* Mark src2 def as consumed */
        VT_CLEAR_DEF(state, src2_pos);
        continue;
      }
    }

    /* Pattern 2b: LOAD of known-constant VAR → ASSIGN #const.
     * Propagates constants tracked through LEA+STORE into TMPs. */
    if (q->op == TCCIR_OP_LOAD && !dest.is_lval)
    {
      int32_t src1_vr = irop_get_vreg(src1);
      if (src1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int src1_pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
        if (src1_pos >= 0 && src1_pos <= max_vreg && VT_IS_CONST(state, src1_pos))
        {
          int64_t val = state[src1_pos].value;
          int btype = irop_get_btype(src1);
          q->op = TCCIR_OP_ASSIGN;
          if (val == (int32_t)val)
            tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)val, btype));
          else
          {
            uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
            tcc_ir_set_src1(ir, i, irop_make_i64(-1, pool_idx, btype));
          }
          tcc_ir_set_src2(ir, i, IROP_NONE);
          LOG_IR_GEN("VALUE_TRACK LOAD-FOLD: i=%d V%d -> #%lld", i, src1_pos, (long long)val);
          changes++;
        }
      }
    }

    /* Pattern 3: CMP with constant vreg - FOLD IT */
    if (q->op == TCCIR_OP_CMP && i + 1 < n)
    {
      IRQuadCompact *jump_q = &ir->compact_instructions[i + 1];
      if (jump_q->op == TCCIR_OP_JUMPIF)
      {
        int32_t src1_vr = irop_get_vreg(src1);
        int src1_pos = (src1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR)
                           ? TCCIR_DECODE_VREG_POSITION(src1_vr)
                           : -1;

        /* Check if src1 is known constant AND src2 is immediate */
        int src1_const = (src1_pos >= 0 && src1_pos <= max_vreg && VT_IS_CONST(state, src1_pos));
        int src2_const = irop_is_immediate(src2);

        if (src1_const && src2_const)
        {
          int64_t val1 = state[src1_pos].value;
          int64_t val2 = irop_get_imm64_ex(ir, src2);

          IROperand cond = tcc_ir_op_get_src1(ir, jump_q);
          int tok = (int)irop_get_imm64_ex(ir, cond);

          int result = evaluate_compare_condition(val1, val2, tok);

          if (result >= 0)
          {
            IROperand jmp_dest = tcc_ir_op_get_dest(ir, jump_q);

            if (result)
            {
              q->op = TCCIR_OP_NOP;
              jump_q->op = TCCIR_OP_JUMP;
              tcc_ir_set_dest(ir, i + 1, jmp_dest);
              LOG_IR_GEN("VALUE_TRACK: CMP vreg=%lld,#%lld -> always taken, JUMP to %d", (long long)val1,
                         (long long)val2, (int)jmp_dest.u.imm32);
            }
            else
            {
              q->op = TCCIR_OP_NOP;
              jump_q->op = TCCIR_OP_NOP;
              LOG_IR_GEN("VALUE_TRACK: CMP vreg=%lld,#%lld -> never taken, eliminated", (long long)val1,
                         (long long)val2);
            }
            changes++;
          }
        }
      }
      else if (jump_q->op == TCCIR_OP_SETIF)
      {
        int32_t src1_vr = irop_get_vreg(src1);
        int src1_pos = (src1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR)
                           ? TCCIR_DECODE_VREG_POSITION(src1_vr)
                           : -1;

        int src1_const = (src1_pos >= 0 && src1_pos <= max_vreg && VT_IS_CONST(state, src1_pos));
        int src2_const = irop_is_immediate(src2);

        if (src1_const && src2_const)
        {
          int64_t val1 = state[src1_pos].value;
          int64_t val2 = irop_get_imm64_ex(ir, src2);

          IROperand setif_src1 = tcc_ir_op_get_src1(ir, jump_q);
          int cond = (int)irop_get_imm64_ex(ir, setif_src1);
          int result = evaluate_compare_condition(val1, val2, cond);

          if (result >= 0)
          {
            int btype = irop_get_btype(setif_src1);
            q->op = TCCIR_OP_NOP;
            jump_q->op = TCCIR_OP_ASSIGN;
            tcc_ir_set_src1(ir, i + 1, irop_make_imm32(-1, result, btype));
            tcc_ir_set_src2(ir, i + 1, IROP_NONE);
            LOG_IR_GEN("VALUE_TRACK: CMP+SETIF vreg=%lld,#%lld cond=0x%x -> %d at i=%d", (long long)val1,
                       (long long)val2, cond, result, i);
            changes++;
          }
        }
      }
      /* CMP reads src1 — mark its def as live */
      {
        int32_t s1_vr = irop_get_vreg(src1);
        if (s1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_VAR)
        {
          int s1_pos = TCCIR_DECODE_VREG_POSITION(s1_vr);
          if (s1_pos >= 0 && s1_pos <= max_vreg)
            VT_CLEAR_DEF(state, s1_pos);
        }
      }
      continue;
    }

    /* Mark source operand reads — preserve their defining instructions */
    {
      int32_t s1_vr = irop_get_vreg(src1);
      if (s1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int s1_pos = TCCIR_DECODE_VREG_POSITION(s1_vr);
        if (s1_pos >= 0 && s1_pos <= max_vreg)
          VT_CLEAR_DEF(state, s1_pos);
      }
      int32_t s2_vr = irop_get_vreg(src2);
      if (s2_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s2_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int s2_pos = TCCIR_DECODE_VREG_POSITION(s2_vr);
        if (s2_pos >= 0 && s2_pos <= max_vreg)
          VT_CLEAR_DEF(state, s2_pos);
      }
    }

    /* Constant-fold __aeabi_lcmp/__aeabi_ulcmp calls when both arguments are
     * known constants (tracked through LEA+STORE). */
    if (q->op == TCCIR_OP_FUNCCALLVAL)
    {
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (callee)
      {
        const char *fname = get_tok_str(callee->v, NULL);
        LOG_IR_GEN("VALUE_TRACK CALL: i=%d fname=%s", i, fname ? fname : "(null)");
        int is_lcmp = (fname && strcmp(fname, "__aeabi_lcmp") == 0);
        int is_ulcmp = (fname && strcmp(fname, "__aeabi_ulcmp") == 0);
        if (is_lcmp || is_ulcmp)
        {
          IROperand arg0, arg1;
          if (ir_opt_get_call_param_operand(ir, i, 0, &arg0) && ir_opt_get_call_param_operand(ir, i, 1, &arg1))
          {
            int arg0_known = 0, arg1_known = 0;
            int64_t val0 = 0, val1 = 0;
            uint64_t uval;

            if (irop_is_immediate(arg0))
            {
              val0 = irop_get_imm64_ex(ir, arg0);
              arg0_known = 1;
            }
            else
            {
              int32_t vr0 = irop_get_vreg(arg0);
              if (vr0 >= 0 && TCCIR_DECODE_VREG_TYPE(vr0) == TCCIR_VREG_TYPE_VAR)
              {
                int pos0 = TCCIR_DECODE_VREG_POSITION(vr0);
                if (pos0 >= 0 && pos0 <= max_vreg && VT_IS_CONST(state, pos0))
                {
                  val0 = state[pos0].value;
                  arg0_known = 1;
                }
              }
              if (!arg0_known && ir_opt_eval_const_u64(ir, arg0, i, &uval, 0))
              {
                val0 = (int64_t)uval;
                arg0_known = 1;
              }
            }

            if (irop_is_immediate(arg1))
            {
              val1 = irop_get_imm64_ex(ir, arg1);
              arg1_known = 1;
            }
            else
            {
              int32_t vr1 = irop_get_vreg(arg1);
              if (vr1 >= 0 && TCCIR_DECODE_VREG_TYPE(vr1) == TCCIR_VREG_TYPE_VAR)
              {
                int pos1 = TCCIR_DECODE_VREG_POSITION(vr1);
                if (pos1 >= 0 && pos1 <= max_vreg && VT_IS_CONST(state, pos1))
                {
                  val1 = state[pos1].value;
                  arg1_known = 1;
                }
              }
              if (!arg1_known && ir_opt_eval_const_u64(ir, arg1, i, &uval, 0))
              {
                val1 = (int64_t)uval;
                arg1_known = 1;
              }
            }

            if (arg0_known && arg1_known)
            {
              int result;
              if (is_ulcmp)
              {
                uint64_t u0 = (uint64_t)val0, u1 = (uint64_t)val1;
                result = (u0 > u1) - (u0 < u1);
              }
              else
              {
                result = (val0 > val1) - (val0 < val1);
              }

              IROperand call_dest = tcc_ir_op_get_dest(ir, q);
              ir_opt_nop_call_params(ir, i);
              q->op = TCCIR_OP_ASSIGN;
              tcc_ir_set_dest(ir, i, call_dest);
              tcc_ir_set_src1(ir, i, irop_make_imm32(-1, result, IROP_BTYPE_INT32));
              tcc_ir_set_src2(ir, i, IROP_NONE);
              LOG_IR_GEN("VALUE_TRACK: %s(%lld, %lld) = %d at i=%d -> folded", fname, (long long)val0, (long long)val1,
                         result, i);
              changes++;
              continue; /* Skip call invalidation — call was eliminated */
            }

            /* Same-vreg fold: lcmp(x, x) == 0 regardless of the value.
             * Catches cases where global LOAD CSE or copy propagation
             * made both arguments refer to the same virtual register.
             * Traces through ASSIGN chains (T5←T4←T0) to find the root. */
            {
              int32_t vr0 = irop_get_vreg(arg0);
              int32_t vr1 = irop_get_vreg(arg1);
              /* Resolve copy chains: follow ASSIGN and single-def STORE
               * to find root vreg.  Covers patterns like:
               *   T4 <-- T0 [ASSIGN]  (from SL_FWD)
               *   V3 <-- T5 [STORE]   (inlined parameter)
               *   T5 <-- T4 [ASSIGN]  (from SL_FWD) */
              for (int depth = 0; depth < 8 && vr0 >= 0; depth++)
              {
                int def = tcc_ir_find_defining_instruction(ir, vr0, i);
                if (def < 0)
                  break;
                IRQuadCompact *dq = &ir->compact_instructions[def];
                if (dq->op != TCCIR_OP_ASSIGN && dq->op != TCCIR_OP_STORE)
                  break;
                IROperand dsrc = tcc_ir_op_get_src1(ir, dq);
                int32_t svr = irop_get_vreg(dsrc);
                if (svr < 0 || dsrc.is_lval)
                  break;
                vr0 = svr;
              }
              for (int depth = 0; depth < 8 && vr1 >= 0; depth++)
              {
                int def = tcc_ir_find_defining_instruction(ir, vr1, i);
                if (def < 0)
                  break;
                IRQuadCompact *dq = &ir->compact_instructions[def];
                if (dq->op != TCCIR_OP_ASSIGN && dq->op != TCCIR_OP_STORE)
                  break;
                IROperand dsrc = tcc_ir_op_get_src1(ir, dq);
                int32_t svr = irop_get_vreg(dsrc);
                if (svr < 0 || dsrc.is_lval)
                  break;
                vr1 = svr;
              }
              LOG_IR_GEN("VALUE_TRACK: %s resolved at i=%d: vr0=%d vr1=%d (orig %d %d)", fname, i, vr0, vr1,
                         irop_get_vreg(arg0), irop_get_vreg(arg1));
              if (vr0 >= 0 && vr0 == vr1 && !arg0.is_lval && !arg1.is_lval)
              {
                IROperand call_dest = tcc_ir_op_get_dest(ir, q);
                ir_opt_nop_call_params(ir, i);
                q->op = TCCIR_OP_ASSIGN;
                tcc_ir_set_dest(ir, i, call_dest);
                tcc_ir_set_src1(ir, i, irop_make_imm32(-1, 0, IROP_BTYPE_INT32));
                tcc_ir_set_src2(ir, i, IROP_NONE);
                LOG_IR_GEN("VALUE_TRACK: %s(vreg%d, vreg%d) = 0 at i=%d -> same-vreg fold", fname, vr0, vr1, i);
                changes++;
                continue;
              }
            }
          }
        }
        /* Constant-fold __aeabi_ldivmod/__aeabi_uldivmod with constant args. */
        {
          int is_ldivmod = (fname && strcmp(fname, "__aeabi_ldivmod") == 0);
          int is_uldivmod = (fname && strcmp(fname, "__aeabi_uldivmod") == 0);
          if (is_ldivmod || is_uldivmod)
          {
            IROperand arg0, arg1;
            if (ir_opt_get_call_param_operand(ir, i, 0, &arg0) && ir_opt_get_call_param_operand(ir, i, 1, &arg1))
            {
              int arg0_known = irop_is_immediate(arg0);
              int arg1_known = irop_is_immediate(arg1);
              int64_t val0 = arg0_known ? irop_get_imm64_ex(ir, arg0) : 0;
              int64_t val1 = arg1_known ? irop_get_imm64_ex(ir, arg1) : 0;

              if (arg0_known && arg1_known && val1 != 0)
              {
                int64_t result;
                if (is_uldivmod)
                  result = (int64_t)((uint64_t)val0 / (uint64_t)val1);
                else
                  result = val0 / val1;

                IROperand call_dest = tcc_ir_op_get_dest(ir, q);
                ir_opt_nop_call_params(ir, i);
                q->op = TCCIR_OP_ASSIGN;
                tcc_ir_set_dest(ir, i, call_dest);
                if (result == (int32_t)result)
                  tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)result, IROP_BTYPE_INT64));
                else
                {
                  uint32_t pool_idx = tcc_ir_pool_add_i64(ir, result);
                  tcc_ir_set_src1(ir, i, irop_make_i64(-1, pool_idx, IROP_BTYPE_INT64));
                }
                tcc_ir_set_src2(ir, i, IROP_NONE);
                LOG_IR_GEN("VALUE_TRACK: %s(%lld, %lld) = %lld at i=%d -> folded", fname, (long long)val0,
                           (long long)val1, (long long)result, i);
                changes++;
                continue;
              }
            }
          }
        }
        /* Constant-fold __bswapsi2/__bswapdi3 calls with known constant arg. */
        {
          int is_bswap32 = (fname && strcmp(fname, "__bswapsi2") == 0);
          int is_bswap64 = (fname && strcmp(fname, "__bswapdi3") == 0);
          if (is_bswap32 || is_bswap64)
          {
            IROperand arg0;
            if (ir_opt_get_call_param_operand(ir, i, 0, &arg0))
            {
              int arg0_known = 0;
              int64_t val0 = 0;
              if (irop_is_immediate(arg0))
              {
                val0 = irop_get_imm64_ex(ir, arg0);
                arg0_known = 1;
              }
              else
              {
                int32_t vr0 = irop_get_vreg(arg0);
                if (vr0 >= 0 && TCCIR_DECODE_VREG_TYPE(vr0) == TCCIR_VREG_TYPE_VAR)
                {
                  int pos0 = TCCIR_DECODE_VREG_POSITION(vr0);
                  if (pos0 >= 0 && pos0 <= max_vreg && VT_IS_CONST(state, pos0))
                  {
                    val0 = state[pos0].value;
                    arg0_known = 1;
                  }
                }
              }
              if (arg0_known)
              {
                int64_t result;
                if (is_bswap32)
                {
                  uint32_t x = (uint32_t)val0;
                  result = (int64_t)(int32_t)(((x >> 24) & 0xFFU) | ((x >> 8) & 0xFF00U) | ((x << 8) & 0xFF0000U) |
                                              ((x << 24) & 0xFF000000U));
                }
                else
                {
                  uint64_t x = (uint64_t)val0;
                  result = (int64_t)(((x >> 56) & 0xFFULL) | ((x >> 40) & 0xFF00ULL) | ((x >> 24) & 0xFF0000ULL) |
                                     ((x >> 8) & 0xFF000000ULL) | ((x << 8) & 0xFF00000000ULL) |
                                     ((x << 24) & 0xFF0000000000ULL) | ((x << 40) & 0xFF000000000000ULL) |
                                     ((x << 56) & 0xFF00000000000000ULL));
                }
                IROperand call_dest = tcc_ir_op_get_dest(ir, q);
                ir_opt_nop_call_params(ir, i);
                q->op = TCCIR_OP_ASSIGN;
                tcc_ir_set_dest(ir, i, call_dest);
                if (result == (int32_t)result)
                  tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)result, IROP_BTYPE_INT32));
                else
                {
                  uint32_t pool_idx = tcc_ir_pool_add_i64(ir, result);
                  tcc_ir_set_src1(ir, i, irop_make_i64(-1, pool_idx, IROP_BTYPE_INT64));
                }
                tcc_ir_set_src2(ir, i, IROP_NONE);
                {
                  int32_t dv = irop_get_vreg(call_dest);
                  if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR)
                  {
                    int dp = TCCIR_DECODE_VREG_POSITION(dv);
                    if (dp >= 0 && dp <= max_vreg)
                      VT_SET_CONST(state, dp, result);
                  }
                }
                LOG_IR_GEN("VALUE_TRACK: %s(%lld) = %lld at i=%d -> folded", fname, (long long)val0, (long long)result,
                           i);
                changes++;
                continue;
              }
            }
          }
        }
        /* Constant-fold __aeabi_llsl/__aeabi_llsr/__aeabi_lasr/__aeabi_lmul
         * calls when both arguments are compile-time constants. */
        {
          int is_llsl = (fname && strcmp(fname, "__aeabi_llsl") == 0);
          int is_llsr = (fname && strcmp(fname, "__aeabi_llsr") == 0);
          int is_lasr = (fname && strcmp(fname, "__aeabi_lasr") == 0);
          int is_lmul = (fname && strcmp(fname, "__aeabi_lmul") == 0);
          if (is_llsl || is_llsr || is_lasr || is_lmul)
          {
            IROperand arg0, arg1;
            if (ir_opt_get_call_param_operand(ir, i, 0, &arg0) && ir_opt_get_call_param_operand(ir, i, 1, &arg1))
            {
              int arg0_known = irop_is_immediate(arg0);
              int arg1_known = irop_is_immediate(arg1);
              int64_t val0 = arg0_known ? irop_get_imm64_ex(ir, arg0) : 0;
              int64_t val1 = arg1_known ? irop_get_imm64_ex(ir, arg1) : 0;

              if (!arg0_known)
              {
                int32_t vr0 = irop_get_vreg(arg0);
                if (vr0 >= 0 && TCCIR_DECODE_VREG_TYPE(vr0) == TCCIR_VREG_TYPE_VAR)
                {
                  int pos0 = TCCIR_DECODE_VREG_POSITION(vr0);
                  if (pos0 >= 0 && pos0 <= max_vreg && VT_IS_CONST(state, pos0))
                  {
                    val0 = state[pos0].value;
                    arg0_known = 1;
                  }
                }
                if (!arg0_known)
                {
                  uint64_t uval;
                  if (ir_opt_eval_const_u64(ir, arg0, i, &uval, 0))
                  {
                    val0 = (int64_t)uval;
                    arg0_known = 1;
                  }
                }
              }
              if (!arg1_known)
              {
                int32_t vr1 = irop_get_vreg(arg1);
                if (vr1 >= 0 && TCCIR_DECODE_VREG_TYPE(vr1) == TCCIR_VREG_TYPE_VAR)
                {
                  int pos1 = TCCIR_DECODE_VREG_POSITION(vr1);
                  if (pos1 >= 0 && pos1 <= max_vreg && VT_IS_CONST(state, pos1))
                  {
                    val1 = state[pos1].value;
                    arg1_known = 1;
                  }
                }
              }

              if (arg0_known && arg1_known)
              {
                int64_t result;
                if (is_llsl)
                  result = (int64_t)((uint64_t)val0 << (val1 & 63));
                else if (is_llsr)
                  result = (int64_t)((uint64_t)val0 >> (val1 & 63));
                else if (is_lasr)
                  result = val0 >> (val1 & 63);
                else /* is_lmul */
                  result = val0 * val1;

                IROperand call_dest = tcc_ir_op_get_dest(ir, q);
                ir_opt_nop_call_params(ir, i);
                q->op = TCCIR_OP_ASSIGN;
                tcc_ir_set_dest(ir, i, call_dest);
                if (result == (int32_t)result)
                  tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)result, IROP_BTYPE_INT64));
                else
                {
                  uint32_t pool_idx = tcc_ir_pool_add_i64(ir, result);
                  tcc_ir_set_src1(ir, i, irop_make_i64(-1, pool_idx, IROP_BTYPE_INT64));
                }
                tcc_ir_set_src2(ir, i, IROP_NONE);
                {
                  int32_t dv = irop_get_vreg(call_dest);
                  if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR)
                  {
                    int dp = TCCIR_DECODE_VREG_POSITION(dv);
                    if (dp >= 0 && dp <= max_vreg)
                      VT_SET_CONST(state, dp, result);
                  }
                }
                LOG_IR_GEN("VALUE_TRACK: %s(%lld, %lld) = %lld at i=%d -> folded", fname, (long long)val0,
                           (long long)val1, (long long)result, i);
                changes++;
                continue;
              }

              /* Lower shift calls with immediate shift amount to IR
               * instructions so subsequent passes can optimize them. */
              if (!is_lmul && arg1_known)
              {
                TccIrOp ir_op = is_llsl ? TCCIR_OP_SHL : is_llsr ? TCCIR_OP_SHR : TCCIR_OP_SAR;
                IROperand call_dest = tcc_ir_op_get_dest(ir, q);
                ir_opt_nop_call_params(ir, i);
                q->op = ir_op;
                tcc_ir_set_dest(ir, i, call_dest);
                arg0.btype = IROP_BTYPE_INT64;
                tcc_ir_set_src1(ir, i, arg0);
                tcc_ir_set_src2(ir, i, irop_make_imm32(-1, (int32_t)(val1 & 63), IROP_BTYPE_INT32));
                LOG_IR_GEN("VALUE_TRACK: %s(vreg, %lld) at i=%d -> lowered to IR shift", fname, (long long)val1, i);
                changes++;
                continue;
              }
            }
          }
        }
      }
    }

    /* Constant-fold soft-float arithmetic calls (__aeabi_fadd/fsub/fmul/fdiv,
     * __aeabi_f2iz, __aeabi_dadd/dsub/dmul/ddiv, __aeabi_d2iz, conversions)
     * when all arguments are compile-time constants.  Uses host FPU. */
    if (q->op == TCCIR_OP_FUNCCALLVAL)
    {
      Sym *sf_callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (sf_callee)
      {
        const char *sf = get_tok_str(sf_callee->v, NULL);
        if (sf)
        {
          /* Classify: nargs=1 or 2, float or double */
          int sf_nargs = 0, sf_kind = 0;
          /* kinds: 1=add 2=sub 3=mul 4=div 5=f2iz 6=f2uiz 7=i2f 8=ui2f
           *        9=f2d 10=d2f 11=d2iz 12=d2uiz 13=i2d 14=ui2d */
          if (strcmp(sf, "__aeabi_fadd") == 0)
          {
            sf_nargs = 2;
            sf_kind = 1;
          }
          else if (strcmp(sf, "__aeabi_fsub") == 0)
          {
            sf_nargs = 2;
            sf_kind = 2;
          }
          else if (strcmp(sf, "__aeabi_fmul") == 0)
          {
            sf_nargs = 2;
            sf_kind = 3;
          }
          else if (strcmp(sf, "__aeabi_fdiv") == 0)
          {
            sf_nargs = 2;
            sf_kind = 4;
          }
          /* Double-returning operations (dadd/dsub/dmul/ddiv, f2d): result is
           * 64-bit and is materialized via the F64 immediate pool below. */
          else if (strcmp(sf, "__aeabi_dadd") == 0)
          {
            sf_nargs = 2;
            sf_kind = 1 | 0x80;
          }
          else if (strcmp(sf, "__aeabi_dsub") == 0)
          {
            sf_nargs = 2;
            sf_kind = 2 | 0x80;
          }
          else if (strcmp(sf, "__aeabi_dmul") == 0)
          {
            sf_nargs = 2;
            sf_kind = 3 | 0x80;
          }
          else if (strcmp(sf, "__aeabi_ddiv") == 0)
          {
            sf_nargs = 2;
            sf_kind = 4 | 0x80;
          }
          else if (strcmp(sf, "__aeabi_f2iz") == 0)
          {
            sf_nargs = 1;
            sf_kind = 5;
          }
          else if (strcmp(sf, "__aeabi_f2uiz") == 0)
          {
            sf_nargs = 1;
            sf_kind = 6;
          }
          else if (strcmp(sf, "__aeabi_i2f") == 0)
          {
            sf_nargs = 1;
            sf_kind = 7;
          }
          else if (strcmp(sf, "__aeabi_ui2f") == 0)
          {
            sf_nargs = 1;
            sf_kind = 8;
          }
          /* __aeabi_f2d and __aeabi_d2f are NOT folded here: the float_narrowing
           * pass pattern-matches f2d → double-math → d2f and rewrites it to a
           * single float-precision math call (e.g. floor → floorf).  Folding
           * f2d/d2f to constants prevents narrowing from firing. */
          else if (0 && strcmp(sf, "__aeabi_f2d") == 0)
          {
            sf_nargs = 1;
            sf_kind = 9;
          }
          else if (0 && strcmp(sf, "__aeabi_d2f") == 0)
          {
            sf_nargs = 1;
            sf_kind = 10;
          }
          else if (strcmp(sf, "__aeabi_i2d") == 0)
          {
            sf_nargs = 1;
            sf_kind = 13;
          }
          else if (strcmp(sf, "__aeabi_ui2d") == 0)
          {
            sf_nargs = 1;
            sf_kind = 14;
          }
          else if (strcmp(sf, "__aeabi_d2iz") == 0)
          {
            sf_nargs = 1;
            sf_kind = 11;
          }
          else if (strcmp(sf, "__aeabi_d2uiz") == 0)
          {
            sf_nargs = 1;
            sf_kind = 12;
          }

          if (sf_kind)
          {
            /* Resolve arguments */
            int64_t a0 = 0, a1 = 0;
            int a0_ok = 0, a1_ok = 0;
            IROperand op0;
            if (ir_opt_get_call_param_operand(ir, i, 0, &op0))
            {
              if (irop_is_immediate(op0))
              {
                a0 = irop_get_imm64_ex(ir, op0);
                a0_ok = 1;
              }
              else
              {
                int32_t vr = irop_get_vreg(op0);
                if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
                {
                  int p = TCCIR_DECODE_VREG_POSITION(vr);
                  if (p >= 0 && p <= max_vreg && VT_IS_CONST(state, p))
                  {
                    a0 = state[p].value;
                    a0_ok = 1;
                  }
                }
              }
            }
            if (sf_nargs >= 2)
            {
              IROperand op1;
              if (ir_opt_get_call_param_operand(ir, i, 1, &op1))
              {
                if (irop_is_immediate(op1))
                {
                  a1 = irop_get_imm64_ex(ir, op1);
                  a1_ok = 1;
                }
                else
                {
                  int32_t vr = irop_get_vreg(op1);
                  if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
                  {
                    int p = TCCIR_DECODE_VREG_POSITION(vr);
                    if (p >= 0 && p <= max_vreg && VT_IS_CONST(state, p))
                    {
                      a1 = state[p].value;
                      a1_ok = 1;
                    }
                  }
                }
              }
            }
            else
              a1_ok = 1;

            if (a0_ok && a1_ok)
            {
              int64_t result = 0;
              int folded = 0;
              int is_dbl = (sf_kind & 0x80) != 0;
              int op = sf_kind & 0x7F;

              if (!is_dbl && op >= 1 && op <= 4)
              {
                /* Float binary: fadd/fsub/fmul/fdiv */
                union
                {
                  float f;
                  uint32_t u;
                } fa, fb, fr;
                fa.u = (uint32_t)a0;
                fb.u = (uint32_t)a1;
                switch (op)
                {
                case 1:
                  fr.f = fa.f + fb.f;
                  folded = 1;
                  break;
                case 2:
                  fr.f = fa.f - fb.f;
                  folded = 1;
                  break;
                case 3:
                  fr.f = fa.f * fb.f;
                  folded = 1;
                  break;
                case 4:
                  if (fb.u != 0)
                  {
                    fr.f = fa.f / fb.f;
                    folded = 1;
                  }
                  break;
                }
                if (folded)
                  result = (int64_t)(int32_t)fr.u;
              }
              else if (is_dbl && op >= 1 && op <= 4)
              {
                /* Double binary: dadd/dsub/dmul/ddiv */
                union
                {
                  double d;
                  uint64_t u;
                } da, db, dr;
                da.u = (uint64_t)a0;
                db.u = (uint64_t)a1;
                switch (op)
                {
                case 1:
                  dr.d = da.d + db.d;
                  folded = 1;
                  break;
                case 2:
                  dr.d = da.d - db.d;
                  folded = 1;
                  break;
                case 3:
                  dr.d = da.d * db.d;
                  folded = 1;
                  break;
                case 4:
                  if (db.u != 0)
                  {
                    dr.d = da.d / db.d;
                    folded = 1;
                  }
                  break;
                }
                if (folded)
                  result = (int64_t)dr.u;
              }
              else
                switch (sf_kind)
                {
                case 5:
                { /* f2iz */
                  union
                  {
                    float f;
                    uint32_t u;
                  } fa;
                  fa.u = (uint32_t)a0;
                  result = (int32_t)fa.f;
                  folded = 1;
                }
                break;
                case 6:
                { /* f2uiz */
                  union
                  {
                    float f;
                    uint32_t u;
                  } fa;
                  fa.u = (uint32_t)a0;
                  result = (int64_t)(uint32_t)fa.f;
                  folded = 1;
                }
                break;
                case 7:
                { /* i2f */
                  union
                  {
                    float f;
                    uint32_t u;
                  } fr;
                  fr.f = (float)(int32_t)a0;
                  result = (int64_t)(int32_t)fr.u;
                  folded = 1;
                }
                break;
                case 8:
                { /* ui2f */
                  union
                  {
                    float f;
                    uint32_t u;
                  } fr;
                  fr.f = (float)(uint32_t)a0;
                  result = (int64_t)(int32_t)fr.u;
                  folded = 1;
                }
                break;
                case 9:
                { /* f2d */
                  union
                  {
                    float f;
                    uint32_t u;
                  } fa;
                  fa.u = (uint32_t)a0;
                  union
                  {
                    double d;
                    uint64_t u;
                  } dr;
                  dr.d = (double)fa.f;
                  result = (int64_t)dr.u;
                  folded = 1;
                }
                break;
                case 10:
                { /* d2f */
                  union
                  {
                    double d;
                    uint64_t u;
                  } da;
                  da.u = (uint64_t)a0;
                  union
                  {
                    float f;
                    uint32_t u;
                  } fr;
                  fr.f = (float)da.d;
                  result = (int64_t)(int32_t)fr.u;
                  folded = 1;
                }
                break;
                case 11:
                { /* d2iz */
                  union
                  {
                    double d;
                    uint64_t u;
                  } da;
                  da.u = (uint64_t)a0;
                  result = (int32_t)da.d;
                  folded = 1;
                }
                break;
                case 12:
                { /* d2uiz */
                  union
                  {
                    double d;
                    uint64_t u;
                  } da;
                  da.u = (uint64_t)a0;
                  result = (int64_t)(uint32_t)da.d;
                  folded = 1;
                }
                break;
                case 13:
                { /* i2d */
                  union
                  {
                    double d;
                    uint64_t u;
                  } dr;
                  dr.d = (double)(int32_t)a0;
                  result = (int64_t)dr.u;
                  folded = 1;
                }
                break;
                case 14:
                { /* ui2d */
                  union
                  {
                    double d;
                    uint64_t u;
                  } dr;
                  dr.d = (double)(uint32_t)a0;
                  result = (int64_t)dr.u;
                  folded = 1;
                }
                break;
                }

              if (folded)
              {
                IROperand call_dest = tcc_ir_op_get_dest(ir, q);
                /* 64-bit results (double-returning ops, f2d) need to be
                 * materialized via the F64 immediate pool because imm32 only
                 * holds 32 bits. */
                int dest_is_64 = irop_is_64bit(call_dest);
                IROperand imm_src;
                if (dest_is_64)
                {
                  uint32_t pool_idx = tcc_ir_pool_add_f64(ir, (uint64_t)result);
                  imm_src = irop_make_f64(-1, pool_idx);
                }
                else
                {
                  imm_src = irop_make_imm32(-1, (int32_t)result, IROP_BTYPE_INT32);
                }
                ir_opt_nop_call_params(ir, i);
                q->op = TCCIR_OP_ASSIGN;
                tcc_ir_set_dest(ir, i, call_dest);
                tcc_ir_set_src1(ir, i, imm_src);
                tcc_ir_set_src2(ir, i, IROP_NONE);
                /* Update value tracking for the dest so subsequent folds
                 * see the correct value (the continue skips normal processing).
                 * state[].value is int64_t so it can carry the full result. */
                {
                  int32_t dv = irop_get_vreg(call_dest);
                  if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR)
                  {
                    int dp = TCCIR_DECODE_VREG_POSITION(dv);
                    if (dp >= 0 && dp <= max_vreg)
                      VT_SET_CONST(state, dp, dest_is_64 ? result : (int64_t)(int32_t)result);
                  }
                }
                LOG_IR_GEN("VALUE_TRACK: %s -> %lld at i=%d (soft-float fold)", sf, (long long)result, i);
                changes++;
                continue;
              }
            }
          }
        }
      }
    }

    /* Function calls can modify any address-taken variable through pointers.
     * Invalidate only tracked addrtaken constants — O(k) instead of O(max_vreg). */
    if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL)
    {
      if (!addrtaken_overflow)
      {
        for (int a = 0; a < num_addrtaken; a++)
        {
          int v = addrtaken_list[a];
          if (VT_IS_CONST(state, v))
            VT_INVALIDATE(state, v);
        }
      }
      else
      {
        /* Overflow fallback: scan all vregs (rare) */
        for (int v = 0; v <= max_vreg; v++)
        {
          if (VT_IS_CONST(state, v) && (is_addrtaken[v / 8] & (1 << (v % 8))))
            VT_INVALIDATE(state, v);
        }
      }
    }

    /* Any other instruction that defines a VAR vreg invalidates the constant */
    if (dest_pos >= 0 && dest_pos <= max_vreg && irop_config[q->op].has_dest)
      VT_INVALIDATE(state, dest_pos);
  }

  tcc_free(is_addrtaken);
  tcc_free(lea_var_map);
  tcc_free(lea_map);
  tcc_free(state);
  tcc_free(is_merge);

  /* Run DCE to remove code after eliminated branches */
  if (changes)
    changes += tcc_ir_opt_dce(ir);

  return changes;
}

#undef VT_IS_CONST
#undef VT_HAS_DEF
#undef VT_SET_CONST
#undef VT_SET_CONST_DEF
#undef VT_INVALIDATE
#undef VT_CLEAR_DEF

/* ============================================================================
 * VRP (Value Range Propagation)
 * ============================================================================
 *
 * Tracks integer value ranges for PARAM and TEMP vregs through the IR.
 * Derives range constraints from conditional branch fall-through paths,
 * propagates constraints through arithmetic, and folds subsequent comparisons
 * when the range fully determines the outcome.
 *
 * Example:
 *   CMP P0, #0
 *   JMP to X if "<=S"     ; fall-through: P0 > 0, i.e. P0 in [1, INT32_MAX]
 *   T0 = P0 - #1          ; T0 in [0, INT32_MAX-1]
 *   CMP T0, #-1           ; -1 == UINT32_MAX as unsigned
 *   JMP to X if "<U"      ; T0 <U UINT32_MAX always true → fold to unconditional JUMP
 *
 * The second branch is always taken (T0 >= 0 implies T0 <U UINT32_MAX),
 * enabling dead code elimination of the otherwise-unreachable block.
 */

/* Maximum vreg positions tracked per type */
#define VRP_MAX_POS 256

/* Range state for a single vreg slot */

int tcc_ir_opt_const_prop_tmp(TCCIRState *ir)
{
  typedef struct
  {
    int gen; /* Generation when this entry is valid */
    int64_t value;
  } TmpConstInfo;

  /* Stack buffers for common case */
#define TMP_CONST_STACK_SIZE 64
#define TMP_CONST_STACK_N 256
  TmpConstInfo tmp_info_stack[TMP_CONST_STACK_SIZE];
  int block_start_seen_stack[TMP_CONST_STACK_N];

  int n = ir->next_instruction_index;
  int changes = 0;
  int max_tmp_pos = 0;
  int current_gen = 1; /* Generation counter, 0 means invalid */
  int i;
  IRQuadCompact *q;
  TmpConstInfo *tmp_info;
  int *block_start_seen;
  int block_start_gen = 1;
  void *heap_alloc = NULL;

  if (n == 0)
    return 0;

  /* Find max TMP position */
  for (i = 0; i < n; i++)
  {
    q = &ir->compact_instructions[i];
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
      if (pos > max_tmp_pos)
        max_tmp_pos = pos;
    }
  }

  if (max_tmp_pos == 0)
    return 0;

  /* Use stack buffers if possible */
  if (max_tmp_pos < TMP_CONST_STACK_SIZE && n <= TMP_CONST_STACK_N)
  {
    tmp_info = tmp_info_stack;
    block_start_seen = block_start_seen_stack;
    memset(tmp_info, 0, sizeof(TmpConstInfo) * (max_tmp_pos + 1));
    memset(block_start_seen, 0, sizeof(int) * n);
  }
  else
  {
    size_t tmp_size = sizeof(TmpConstInfo) * (max_tmp_pos + 1);
    size_t block_size = sizeof(int) * n;
    heap_alloc = tcc_mallocz(tmp_size + block_size);
    tmp_info = (TmpConstInfo *)heap_alloc;
    block_start_seen = (int *)((char *)heap_alloc + tmp_size);
  }

  /* Mark block starts (shared helper) */
  ir_opt_mark_block_starts(ir, block_start_seen, block_start_gen, n);

  /* Single pass: track TMP constants and propagate */
  for (i = 0; i < n; i++)
  {
    q = &ir->compact_instructions[i];

    /* Clear at basic block entry (jump targets) - O(1) via generation bump */
    if (i != 0 && block_start_seen[i] == block_start_gen)
    {
      current_gen++;
    }

    if (q->op == TCCIR_OP_NOP)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    int32_t src1_vr = irop_get_vreg(src1);

    /* Resolve SWITCH_TABLE when the index TMP is a known constant:
     * replace with a direct JUMP to the appropriate case target. */
    if (q->op == TCCIR_OP_SWITCH_TABLE && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_TEMP)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
      if (pos <= max_tmp_pos && tmp_info[pos].gen == current_gen)
      {
        int64_t index_val = tmp_info[pos].value;
        IROperand src2 = tcc_ir_op_get_src2(ir, q);
        int table_id = (int)irop_get_imm64_ex(ir, src2);
        if (table_id >= 0 && table_id < ir->num_switch_tables)
        {
          TCCIRSwitchTable *table = &ir->switch_tables[table_id];
          int target;
          if (index_val >= 0 && index_val < table->num_entries)
            target = table->targets[(int)index_val];
          else
            target = table->default_target;
          LOG_IR_GEN("OPTIMIZE: Constant SWITCH_TABLE index=%lld -> JUMP to %d", (long long)index_val, target);
          q->op = TCCIR_OP_JUMP;
          tcc_ir_set_dest(ir, i, irop_make_imm32(-1, target, 0));
          tcc_ir_set_src1(ir, i, IROP_NONE);
          tcc_ir_set_src2(ir, i, IROP_NONE);
          changes++;
          current_gen++;
          continue;
        }
      }
    }

    /* Propagate TMP constants to src1.
     * Skip SWITCH_TABLE and IJUMP: their src1 (the index / target address)
     * must remain in a register — the ARM code generator cannot handle an
     * immediate operand there. */
    if (irop_config[q->op].has_src1 && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_TEMP &&
        q->op != TCCIR_OP_SWITCH_TABLE && q->op != TCCIR_OP_IJUMP)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
      if (pos <= max_tmp_pos && tmp_info[pos].gen == current_gen)
      {
        int btype = irop_get_btype(src1);
        IROperand new_src1;
        int64_t val = tmp_info[pos].value;
        if (val == (int32_t)val)
        {
          new_src1 = irop_make_imm32(-1, (int32_t)val, btype);
        }
        else
        {
          uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
          new_src1 = irop_make_i64(-1, pool_idx, btype);
        }
        /* Preserve type flags but NOT memory-access flags.
         * is_lval/is_llocal/is_local describe stack-slot semantics that
         * don't apply to an immediate constant value. */
        new_src1.is_unsigned = src1.is_unsigned;
        new_src1.is_static = src1.is_static;
        tcc_ir_set_src1(ir, i, new_src1);
        changes++;
      }
    }

    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    int32_t src2_vr = irop_get_vreg(src2);
    /* Propagate TMP constants to src2 */
    if (irop_config[q->op].has_src2 && TCCIR_DECODE_VREG_TYPE(src2_vr) == TCCIR_VREG_TYPE_TEMP)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(src2_vr);
      if (pos <= max_tmp_pos && tmp_info[pos].gen == current_gen)
      {
        LOG_IR_GEN("OPTIMIZE: TMP const propagate TMP:%d = %lld to src2 at i=%d", pos, (long long)tmp_info[pos].value,
                   i);
        int btype = irop_get_btype(src2);
        int64_t val = tmp_info[pos].value;
        /* When propagating a narrow constant into a wider bitwise op,
         * widen it to INT64 with zero-extension so the code generator
         * doesn't sign-extend the immediate into the upper register. */
        int src1_bt = irop_get_btype(src1);
        if (src1_bt == IROP_BTYPE_INT64 && btype != IROP_BTYPE_INT64 &&
            (q->op == TCCIR_OP_OR || q->op == TCCIR_OP_AND || q->op == TCCIR_OP_XOR))
        {
          val = (int64_t)(uint32_t)val;
          btype = IROP_BTYPE_INT64;
        }
        IROperand new_src2;
        if (val == (int32_t)val)
        {
          new_src2 = irop_make_imm32(-1, (int32_t)val, btype);
        }
        else
        {
          uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
          new_src2 = irop_make_i64(-1, pool_idx, btype);
        }
        /* Preserve type flags but NOT memory-access flags. */
        new_src2.is_unsigned = src2.is_unsigned;
        new_src2.is_static = src2.is_static;
        tcc_ir_set_src2(ir, i, new_src2);
        changes++;
      }
    }

    /* After propagation, fold if both operands are now immediate.
     * This cascades within a single pass: the result is tracked and
     * feeds the next instruction, avoiding multi-iteration ping-pong. */
    if (irop_config[q->op].has_src1 && irop_config[q->op].has_src2)
    {
      IROperand fs1 = tcc_ir_op_get_src1(ir, q);
      IROperand fs2 = tcc_ir_op_get_src2(ir, q);
      if (irop_is_immediate(fs1) && irop_is_immediate(fs2))
      {
        int64_t v1 = irop_get_imm64_ex(ir, fs1);
        int64_t v2 = irop_get_imm64_ex(ir, fs2);
        int btype = irop_get_btype(fs1);
        int64_t res = 0;
        int ok = 1;
        switch (q->op)
        {
        case TCCIR_OP_ADD:
          res = (int64_t)((uint64_t)v1 + (uint64_t)v2);
          break;
        case TCCIR_OP_SUB:
          res = (int64_t)((uint64_t)v1 - (uint64_t)v2);
          break;
        case TCCIR_OP_AND:
          res = v1 & v2;
          break;
        case TCCIR_OP_OR:
          res = v1 | v2;
          break;
        case TCCIR_OP_XOR:
          res = v1 ^ v2;
          break;
        case TCCIR_OP_SHL:
          res = (int64_t)((uint64_t)v1 << v2);
          break;
        case TCCIR_OP_SHR:
          if (btype == IROP_BTYPE_INT64)
            res = (int64_t)((uint64_t)v1 >> v2);
          else
            res = (int64_t)((uint32_t)v1 >> v2);
          break;
        case TCCIR_OP_SAR:
          res = v1 >> v2;
          break;
        case TCCIR_OP_ROR:
        {
          uint32_t v = (uint32_t)v1;
          uint32_t n = (uint32_t)v2 & 31;
          res = (int64_t)(int32_t)((v >> n) | (v << (32 - n)));
          break;
        }
        case TCCIR_OP_MUL:
          res = (int64_t)((uint64_t)v1 * (uint64_t)v2);
          break;
        case TCCIR_OP_UMULL:
        {
          uint64_t uresult = (uint64_t)(uint32_t)v1 * (uint64_t)(uint32_t)v2;
          res = (int64_t)uresult;
          btype = IROP_BTYPE_INT64;
          break;
        }
        case TCCIR_OP_UBFX:
        {
          int lsb = (int)v2 & 0x1F;
          int width = ((int)v2 >> 5) & 0x1F;
          if (width > 0 && width <= 32)
            res = ((uint32_t)v1 >> lsb) & ((1u << width) - 1);
          else
            ok = 0;
          break;
        }
        default:
          ok = 0;
          break;
        }
        if (ok)
        {
          if (btype != IROP_BTYPE_INT64 && btype != IROP_BTYPE_FLOAT64)
          {
            if (q->op == TCCIR_OP_SHL && v2 >= 32)
            {
              IROperand dest = tcc_ir_op_get_dest(ir, q);
              if (irop_get_btype(dest) == IROP_BTYPE_INT64)
                btype = IROP_BTYPE_INT64;
              else
                ok = 0;
            }
            else
              res = (int64_t)(int32_t)(uint32_t)res;
          }
        }
        if (ok)
        {
          q->op = TCCIR_OP_ASSIGN;
          IROperand nr;
          if (res == (int32_t)res)
            nr = irop_make_imm32(-1, (int32_t)res, btype);
          else
          {
            uint32_t pool_idx = tcc_ir_pool_add_i64(ir, res);
            nr = irop_make_i64(-1, pool_idx, btype);
          }
          tcc_ir_set_src1(ir, i, nr);
          tcc_ir_set_src2(ir, i, IROP_NONE);
          changes++;
        }
      }
    }

    /* CMP+SETIF fold: when TMP propagation makes both CMP operands immediate,
     * fold the CMP+SETIF pair in-place so TEST_ZERO+JUMPIF can be folded
     * within the same pass rather than waiting for the next const_prop round. */
    if (q->op == TCCIR_OP_CMP && i + 1 < n)
    {
      IRQuadCompact *next_q = &ir->compact_instructions[i + 1];
      if (next_q->op == TCCIR_OP_SETIF)
      {
        IROperand cs1 = tcc_ir_op_get_src1(ir, q);
        IROperand cs2 = tcc_ir_op_get_src2(ir, q);
        if (irop_is_immediate(cs1) && irop_is_immediate(cs2))
        {
          int64_t cv1 = irop_get_imm64_ex(ir, cs1);
          int64_t cv2 = irop_get_imm64_ex(ir, cs2);
          IROperand setif_src1 = tcc_ir_op_get_src1(ir, next_q);
          int cond = (int)irop_get_imm64_ex(ir, setif_src1);
          int result = evaluate_compare_condition(cv1, cv2, cond);
          if (result >= 0)
          {
            q->op = TCCIR_OP_NOP;
            next_q->op = TCCIR_OP_ASSIGN;
            int btype = irop_get_btype(setif_src1);
            tcc_ir_set_src1(ir, i + 1, irop_make_imm32(-1, result, btype));
            tcc_ir_set_src2(ir, i + 1, IROP_NONE);
            changes++;
          }
        }
      }
    }

    /* Clear all at basic block boundaries - O(1) via generation bump */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
    {
      current_gen++;
      continue;
    }

    /* Track TMP <- constant assignments (re-fetch src1 since fold may have changed it) */
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP &&
        q->op == TCCIR_OP_ASSIGN)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
      IROperand cur_src1 = tcc_ir_op_get_src1(ir, q);
      if (pos <= max_tmp_pos && irop_is_immediate(cur_src1))
      {
        tmp_info[pos].gen = current_gen;
        tmp_info[pos].value = irop_get_imm64_ex(ir, cur_src1);
      }
    }
  }

  if (heap_alloc)
    tcc_free(heap_alloc);

  return changes;
#undef TMP_CONST_STACK_SIZE
#undef TMP_CONST_STACK_N
}

/* ADD/SUB Constant Reassociation
 *
 * Normalizes ADD/SUB chains with constant operands so that cascaded
 * pointer arithmetic collapses into a single ADD from the original base:
 *
 *   ADD(ADD(base, c1), c2)  →  ADD(base, c1+c2)
 *
 * This enables downstream CMP identity folding to recognize that two
 * independently computed "base + N" values are identical.
 */

int tcc_ir_opt_add_reassoc(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2 || n > 4000)
    return 0;

  uint8_t *is_merge = ir_opt_build_merge_bitmap(ir, n);
  int dc_stride = 0;
  uint8_t *dc = ir_opt_build_def_count(ir, n, &dc_stride);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    if (!irop_is_immediate(src2))
      continue;

    if (src1.is_lval)
      continue;

    int32_t src1_vr = irop_get_vreg(src1);
    if (src1_vr < 0)
      continue;

    if (!DC_IS_SINGLE_DEF(dc, dc_stride, src1_vr))
      continue;

    int def_idx = tcc_ir_find_defining_instruction(ir, src1_vr, i);
    if (def_idx < 0)
      continue;

    /* Verify no merge points between def and use */
    {
      int safe = 1;
      for (int j = def_idx + 1; j <= i; j++)
      {
        if (is_merge[j / 8] & (1 << (j % 8)))
        {
          safe = 0;
          break;
        }
      }
      if (!safe)
        continue;
    }

    IRQuadCompact *def_q = &ir->compact_instructions[def_idx];
    if (def_q->op != TCCIR_OP_ADD && def_q->op != TCCIR_OP_SUB)
      continue;

    IROperand def_src2 = tcc_ir_op_get_src2(ir, def_q);
    if (!irop_is_immediate(def_src2))
      continue;
    IROperand def_src1 = tcc_ir_op_get_src1(ir, def_q);

    /* The reassociation replaces src1_vr with def_src1 at the use point.
     * If def_src1 is a vreg, it must not be redefined between def_idx and i,
     * otherwise the substituted value would read a stale/wrong version. */
    int32_t inner_vr = irop_get_vreg(def_src1);
    if (inner_vr >= 0 && !DC_IS_SINGLE_DEF(dc, dc_stride, inner_vr))
    {
      int redefined = 0;
      for (int j = def_idx + 1; j < i; j++)
      {
        IRQuadCompact *jq = &ir->compact_instructions[j];
        if (jq->op == TCCIR_OP_NOP)
          continue;
        IROperand jdst = tcc_ir_op_get_dest(ir, jq);
        if (irop_get_vreg(jdst) == inner_vr)
        {
          redefined = 1;
          break;
        }
      }
      if (redefined)
        continue;
    }

    int64_t c1 = irop_get_imm64_ex(ir, def_src2);
    int64_t c2 = irop_get_imm64_ex(ir, src2);
    int64_t eff_c1 = (def_q->op == TCCIR_OP_SUB) ? -c1 : c1;
    int64_t eff_c2 = (q->op == TCCIR_OP_SUB) ? -c2 : c2;
    int64_t combined = eff_c1 + eff_c2;

    if (combined != (int32_t)combined)
      continue;

    int btype = irop_get_btype(src2);
    LOG_IR_GEN("OPTIMIZE: ADD reassoc at i=%d: (%lld) + (%lld) = %lld",
               i, (long long)eff_c1, (long long)eff_c2, (long long)combined);

    if (combined == 0)
    {
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i, def_src1);
      tcc_ir_set_src2(ir, i, IROP_NONE);
    }
    else
    {
      q->op = TCCIR_OP_ADD;
      tcc_ir_set_src1(ir, i, def_src1);
      tcc_ir_set_src2(ir, i, irop_make_imm32(-1, (int32_t)combined, btype));
    }
    changes++;
  }

  tcc_free(dc);
  tcc_free(is_merge);
  return changes;
}

/* CMP Expression-Equality Fold
 *
 * Fold CMP+JUMPIF/SELECT when both CMP operands compute the same
 * expression (e.g. both are ADD(GlobalSym, 5) via different vregs).
 * Handles cross-type comparisons (VAR vs TEMP) by comparing at the
 * definition level, bypassing the STACKOFF/VREG tag mismatch that
 * ir_opt_pure_expr_equal cannot handle.
 */
int tcc_ir_opt_cmp_expr_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2 || n > 4000)
    return 0;

  int dc_stride = 0;
  uint8_t *dc = ir_opt_build_def_count(ir, n, &dc_stride);

  for (int i = 0; i < n - 1; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_CMP)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    int32_t vr1 = irop_get_vreg(src1);
    int32_t vr2 = irop_get_vreg(src2);
    if (vr1 < 0 || vr2 < 0 || vr1 == vr2)
      continue;

    /* Both operands must have a single reaching definition */
    int def1 = tcc_ir_find_defining_instruction(ir, vr1, i);
    int def2 = tcc_ir_find_defining_instruction(ir, vr2, i);
    if (def1 < 0 || def2 < 0 || def1 == def2)
      continue;

    /* Try standard def equality (works for single-def vregs) */
    int is_equal = 0;
    if (DC_IS_SINGLE_DEF(dc, dc_stride, vr1) && DC_IS_SINGLE_DEF(dc, dc_stride, vr2))
      is_equal = ir_opt_pure_def_equal(ir, def1, def2, 0);

    /* Pattern match: both defs are ADD/SUB with the same immediate, and
     * their base operands resolve to the same value (e.g. both are
     * ASSIGN(GlobalSym) or LOAD of the same source). */
    if (!is_equal)
    {
      IRQuadCompact *dq1 = &ir->compact_instructions[def1];
      IRQuadCompact *dq2 = &ir->compact_instructions[def2];
      if (dq1->op == dq2->op && (dq1->op == TCCIR_OP_ADD || dq1->op == TCCIR_OP_SUB))
      {
        IROperand ds2_1 = tcc_ir_op_get_src2(ir, dq1);
        IROperand ds2_2 = tcc_ir_op_get_src2(ir, dq2);
        if (irop_is_immediate(ds2_1) && irop_is_immediate(ds2_2) &&
            irop_get_imm64_ex(ir, ds2_1) == irop_get_imm64_ex(ir, ds2_2))
        {
          IROperand base1 = tcc_ir_op_get_src1(ir, dq1);
          IROperand base2 = tcc_ir_op_get_src1(ir, dq2);
          int32_t bvr1 = irop_get_vreg(base1);
          int32_t bvr2 = irop_get_vreg(base2);

          if (bvr1 >= 0 && bvr2 >= 0)
          {
            /* Same base vreg → equal */
            if (bvr1 == bvr2)
              is_equal = 1;
            /* Different base vregs: check if they resolve to the same value */
            if (!is_equal)
            {
              int bd1 = tcc_ir_find_defining_instruction(ir, bvr1, def1);
              int bd2 = tcc_ir_find_defining_instruction(ir, bvr2, def2);
              if (bd1 >= 0 && bd2 >= 0)
              {
                IRQuadCompact *bdq1 = &ir->compact_instructions[bd1];
                IRQuadCompact *bdq2 = &ir->compact_instructions[bd2];
                /* Both ASSIGN/LOAD of the same source operand */
                if ((bdq1->op == TCCIR_OP_ASSIGN || bdq1->op == TCCIR_OP_LOAD) &&
                    (bdq2->op == TCCIR_OP_ASSIGN || bdq2->op == TCCIR_OP_LOAD))
                {
                  IROperand bs1 = tcc_ir_op_get_src1(ir, bdq1);
                  IROperand bs2 = tcc_ir_op_get_src1(ir, bdq2);
                  int32_t bsvr1 = irop_get_vreg(bs1);
                  int32_t bsvr2 = irop_get_vreg(bs2);
                  /* Same vreg source (e.g. both LOAD from V0) */
                  if (bsvr1 >= 0 && bsvr1 == bsvr2)
                    is_equal = 1;
                  /* Both non-vreg: compare structurally (e.g. same GlobalSym) */
                  if (!is_equal && bsvr1 < 0 && bsvr2 < 0)
                    is_equal = ir_opt_nonvreg_expr_equal(ir, bs1, bs2);
                  /* One is vreg (LOAD(V0)), other is constant (ASSIGN(GlobalSym)):
                   * resolve the vreg's value and compare with the constant. */
                  if (!is_equal && ((bsvr1 >= 0) != (bsvr2 >= 0)))
                  {
                    int vreg_side = (bsvr1 >= 0) ? bsvr1 : bsvr2;
                    IROperand const_side = (bsvr1 >= 0) ? bs2 : bs1;
                    int vreg_def_at = (bsvr1 >= 0) ? bd1 : bd2;
                    int vdef = tcc_ir_find_defining_instruction(ir, vreg_side, vreg_def_at);
                    if (vdef >= 0)
                    {
                      IRQuadCompact *vdq = &ir->compact_instructions[vdef];
                      if (vdq->op == TCCIR_OP_ASSIGN)
                      {
                        IROperand vs = tcc_ir_op_get_src1(ir, vdq);
                        if (irop_get_vreg(vs) < 0)
                          is_equal = ir_opt_nonvreg_expr_equal(ir, vs, const_side);
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }

    if (!is_equal)
      continue;

    /* Both operands compute the same expression — fold the CMP */
    IRQuadCompact *next = &ir->compact_instructions[i + 1];
    if (next->op == TCCIR_OP_JUMPIF)
    {
      IROperand cond = tcc_ir_op_get_src1(ir, next);
      int tok = (int)irop_get_imm64_ex(ir, cond);
      int result = evaluate_compare_condition(0, 0, tok);
      if (result < 0)
        continue;
      IROperand jmp_dest = tcc_ir_op_get_dest(ir, next);
      if (result)
      {
        q->op = TCCIR_OP_NOP;
        next->op = TCCIR_OP_JUMP;
        tcc_ir_set_dest(ir, i + 1, jmp_dest);
      }
      else
      {
        q->op = TCCIR_OP_NOP;
        next->op = TCCIR_OP_NOP;
      }
      changes++;
    }
    else if (next->op == TCCIR_OP_SELECT)
    {
      IROperand select_cond = ir->iroperand_pool[next->operand_base + 3];
      int tok = (int)irop_get_imm64_ex(ir, select_cond);
      int result = evaluate_compare_condition(0, 0, tok);
      if (result < 0)
        continue;
      IROperand then_val = tcc_ir_op_get_src1(ir, next);
      IROperand else_val = tcc_ir_op_get_src2(ir, next);
      IROperand chosen = result ? then_val : else_val;
      q->op = TCCIR_OP_NOP;
      next->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i + 1, chosen);
      tcc_ir_set_src2(ir, i + 1, IROP_NONE);
      changes++;
    }
  }

  tcc_free(dc);

  if (changes)
    changes += tcc_ir_opt_dce(ir);

  return changes;
}

int tcc_ir_opt_const_prop_ex(IROptCtx *ctx) { return tcc_ir_opt_const_prop(ctx->ir); }
int tcc_ir_opt_const_prop_tmp_ex(IROptCtx *ctx) { return tcc_ir_opt_const_prop_tmp(ctx->ir); }
int tcc_ir_opt_const_var_prop_ex(IROptCtx *ctx) { return tcc_ir_opt_const_var_prop(ctx->ir); }
int tcc_ir_opt_global_init_prop_ex(IROptCtx *ctx) { return tcc_ir_opt_global_init_prop(ctx->ir); }
int tcc_ir_opt_value_tracking_ex(IROptCtx *ctx) { return tcc_ir_opt_value_tracking(ctx->ir); }
int tcc_ir_opt_add_reassoc_ex(IROptCtx *ctx) { return tcc_ir_opt_add_reassoc(ctx->ir); }
int tcc_ir_opt_cmp_expr_fold_ex(IROptCtx *ctx) { return tcc_ir_opt_cmp_expr_fold(ctx->ir); }
int tcc_ir_opt_const_string_calls_ex(IROptCtx *ctx) { return tcc_ir_opt_const_string_calls(ctx->ir); }
