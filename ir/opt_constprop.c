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
#include "opt.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "opt_du.h"

/* When a soft-FP __aeabi_cfcmple/cdcmple is called with at least one NaN
 * operand, the (>)-(<) integer "cmp_result" we compute from the host's
 * IEEE comparison degenerates to 0 — indistinguishable from "equal" — so
 * evaluate_compare_condition() would mis-fold ordered predicates as true.
 * This helper returns the correct IEEE boolean for the JUMPIF/SETIF token
 * directly: ordered predicates are FALSE for NaN, NE is TRUE.
 *
 * Returns -1 to mean "don't fold" for tokens where the soft-FP runtime
 * (fcmp_core returns 2 for unordered, then `cmp r0, #0` makes flags say
 * "greater") would disagree with IEEE — GT/GE/UGT/UGE.  The IR generator
 * swaps operands so these tokens don't normally appear after cfcmple, but
 * if they ever do, folding to the IEEE answer would silently diverge from
 * runtime; leave the call so the (buggy-for-NaN) runtime answer stands. */
int nan_compare_branch_result(int cond_token)
{
  switch (cond_token)
  {
  case TOK_EQ:
  case TOK_LT:
  case TOK_LE:
  case TOK_ULT:
  case TOK_ULE:
    return 0;
  case TOK_NE:
    return 1;
  default:
    return -1;
  }
}

static int cmp_operand_is_unsigned_int(IROperand op)
{
  int btype = irop_get_btype(op);
  return op.is_unsigned &&
         (irop_get_tag(op) == IROP_TAG_I64 ||
          btype == IROP_BTYPE_INT8 || btype == IROP_BTYPE_INT16 ||
          btype == IROP_BTYPE_INT32 || btype == IROP_BTYPE_INT64);
}

static int cmp_operands_unsigned_width(IROperand src1, IROperand src2)
{
  return (irop_get_tag(src1) == IROP_TAG_I64 ||
          irop_get_tag(src2) == IROP_TAG_I64 ||
          irop_get_btype(src1) == IROP_BTYPE_INT64 ||
          irop_get_btype(src2) == IROP_BTYPE_INT64)
             ? 64
             : 32;
}

static int unsigned_cond_for_cmp_operands(int cond, IROperand src1, IROperand src2)
{
  if (!cmp_operand_is_unsigned_int(src1) && !cmp_operand_is_unsigned_int(src2))
    return cond;

  switch (cond)
  {
  case TOK_LT:
    return TOK_ULT;
  case TOK_GE:
    return TOK_UGE;
  case TOK_LE:
    return TOK_ULE;
  case TOK_GT:
    return TOK_UGT;
  default:
    return cond;
  }
}

int evaluate_compare_condition_cmp_operands(int64_t val1, int64_t val2, int cond,
                                            IROperand src1, IROperand src2)
{
  cond = unsigned_cond_for_cmp_operands(cond, src1, src2);
  if (cmp_operands_unsigned_width(src1, src2) != 64)
  {
    int32_t s1 = (int32_t)(uint32_t)val1;
    int32_t s2 = (int32_t)(uint32_t)val2;
    switch (cond)
    {
    case TOK_EQ:
      return (uint32_t)val1 == (uint32_t)val2;
    case TOK_NE:
      return (uint32_t)val1 != (uint32_t)val2;
    case TOK_LT:
      return s1 < s2;
    case TOK_GE:
      return s1 >= s2;
    case TOK_LE:
      return s1 <= s2;
    case TOK_GT:
      return s1 > s2;
    default:
      break;
    }
  }
  switch (cond)
  {
  case TOK_ULT:
  case TOK_UGE:
  case TOK_ULE:
  case TOK_UGT:
  {
    if (cmp_operands_unsigned_width(src1, src2) == 64)
      return evaluate_compare_condition(val1, val2, cond);
    uint32_t u1 = (uint32_t)val1;
    uint32_t u2 = (uint32_t)val2;
    switch (cond)
    {
    case TOK_ULT:
      return u1 < u2;
    case TOK_UGE:
      return u1 >= u2;
    case TOK_ULE:
      return u1 <= u2;
    case TOK_UGT:
      return u1 > u2;
    default:
      break;
    }
  }
  default:
    return evaluate_compare_condition(val1, val2, cond);
  }
}

/* MLA carries a 4th (accumulator) operand at pool[operand_base+3] that is a
 * real USE of its vreg but is invisible to the has_src1/has_src2 operand
 * config.  Every use-scan that decides whether a def is dead must include
 * it, or a value consumed only as an MLA accumulator is treated as unread
 * and its def deleted (ptr seed 6869).  Returns -1 when there is none. */
int32_t ir_opt_mla_accum_vreg(const TCCIRState *ir, const IRQuadCompact *q)
{
  if (q->op != TCCIR_OP_MLA)
    return -1;
  return irop_get_vreg(tcc_ir_op_get_accum(ir, q));
}

/* ---------------------------------------------------------------------------
 * Global-initializer constant propagation
 *
 * Replace `LOAD dest <-- GlobalSym(X)+addend [deref]` with either:
 *   - `ASSIGN dest <-- #imm` when the initializer byte range is plain data,
 *   - `ASSIGN dest <-- &SymY+addend` when the byte range is covered by a
 *     single R_ARM_ABS32 relocation (e.g. a const pointer initialised to
 *     the address of another global, or a function-pointer entry inside a
 *     const struct).
 *
 * The pass handles primitive scalar globals, arrays, and aggregates: the
 * read width is taken from the operand's btype, not the symbol's full size,
 * so a 1-byte byte-access into a const struct at offset N folds to that
 * specific byte.  After this pass runs, the iterative const_prop +
 * branch_folding + DCE pipeline picks up the newly-visible constants /
 * symrefs and collapses downstream comparisons and dead arms.
 *
 * Safety gates (mirror the checks already used in try_inline_const_eval):
 *   - The sym must exist and carry a known type.
 *   - possibly_written == 0 (no stores / no non-const pointer escape).
 *   - Not volatile, not VLA.
 *   - Linkage: VT_STATIC, or VT_CONSTANT (the C language forbids writing
 *     to a const object even via another TU, so an extern-visible const
 *     global cannot be mutated legally).
 *   - Weak / dllimport / undefined symbols are skipped.
 *   - The initializer range must fit inside the section's emitted data.
 *   - If a relocation overlaps the read range, only a clean R_ARM_ABS32 at
 *     exactly `off` with a 4-byte read folds (to a symref); any partial
 *     overlap rejects the fold.
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

    /* Consider both src1 and src2 operands.  For LOAD, src1's deref is the
     * load location and the whole op becomes ASSIGN.  For other ops (CMP,
     * JUMPIF, ASSIGN, arithmetic), an is_sym && is_lval operand is a
     * read-side deref that can be folded in place. */
    for (int slot = 0; slot < 2; slot++)
    {
      IROperand opnd = (slot == 0) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      if (!opnd.is_sym || !opnd.is_lval)
        continue;
      /* STORE's address is in dest, not in src1/src2 — both srcs are values.
       * For LOAD, src1 carries the address; folding it converts to ASSIGN. */

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
      /* For array types, const qualifies the element type, not the
       * array itself.  Check the pointed-to type for VT_CONSTANT. */
      if (!is_const_q && (ttype & VT_ARRAY) && sym->type.ref)
        is_const_q = (sym->type.ref->type.t & VT_CONSTANT) != 0;

      /* For non-const globals, possibly_written means another function
       * may have stored to this symbol.  For const-qualified globals the
       * C standard forbids modification, so ignore the flag. */
      if (!is_const_q && sym->a.possibly_written)
        continue;

      if (!(ttype & VT_STATIC) && !is_const_q)
        continue;

      /* Pointer-typed globals are foldable only when const-qualified — the
       * non-const late_reopt path can't safely emit a symref fold (re-emit
       * shrinks the function and the literal-pool placement isn't kept
       * aligned).  Skip non-const pointer globals before we'd otherwise
       * flag the function for late_reopt and trigger that re-emit. */
      if ((ttype & VT_BTYPE) == VT_PTR && !is_const_q)
        continue;

      /* TCC is single-pass: when this function is optimized, stores in
       * later-declared functions have not yet been seen, so possibly_written
       * may be 0 for a global that is in fact written elsewhere in the TU
       * (see 20001111-1.c).  Restrict the fold to const-qualified globals,
       * which the language guarantees are not modified.
       *
       * BYPASS: during the end-of-TU late_reopt phase, possibly_written
       * reflects the entire TU, so non-const statics can also be folded —
       * but only if the symbol's address was never taken (otherwise an
       * alias write could have updated it without poisoning possibly_written;
       * static initializers that capture `&sym` don't go through the regular
       * store path that sets the flag).  See pr22237.c for the alias case. */
      if (!is_const_q)
      {
        if (sym->a.addrtaken)
          continue;
        if (!tcc_state->ir_late_reopt_phase)
        {
          /* Record the function for end-of-TU re-optimization: at that
           * point possibly_written will be final TU-wide and we can fold
           * safely. */
          if (tcc_state->cur_func_sym && tcc_state->cur_func_sym->type.ref)
            tcc_state->cur_func_sym->type.ref->f.func_late_reopt = 1;
          continue;
        }
        /* else: late phase — fall through, fold this non-const static. */
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

      /* Result btype: for LOAD, use dest btype; for read-side deref operands
       * on non-LOAD ops, use the operand's own btype so consumers keep their
       * expected operand width. */
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

      /* Map result btype to read size in bytes; reject types we can't decode. */
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

      /* Scan section relocations for any overlap with [off, off+read_size).
       * The only foldable overlap is a single R_ARM_ABS32 at exactly `off`
       * with a 4-byte read — that becomes a symref to the target.  Any
       * other overlap (partial reloc inside the read range, smaller read
       * over a 4-byte reloc, etc.) rejects the fold. */
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
          /* Be conservative: anything other than ABS32 we treat as a
           * single-byte cover so we still reject partial overlaps. */
          uint32_t r_size = (r_type == R_ARM_ABS32) ? 4 : 1;

          if (r_off + r_size <= off)
            continue;
          if (r_off >= off + (unsigned long)read_size)
            continue;

          reloc_overlap = 1;
          /* Symref fold is only safe when the *source* global is
           * const-qualified.  The non-const late_reopt path can shrink the
           * caller (LOAD+DEREF → ASSIGN-sym is one fewer Thumb-2 instruction)
           * and the re-emit's literal-pool placement isn't kept aligned
           * across that size change (see pr22237).  Restricting to const
           * sources avoids that path entirely. */
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

/* ---------------------------------------------------------------------------
 * Symref-constant propagation
 *
 * Propagate `ASSIGN T <-- &S+addend` (a symref-by-value, not is_lval) into
 * subsequent uses of T.  Each use is replaced with a fresh symref operand
 * carrying the same sym + addend, preserving the use's is_lval / is_unsigned
 * flags so that `T***DEREF***` becomes `&S+addend***DEREF***` — a lval-symref
 * that downstream global-init-prop can then read out of the section data.
 *
 * Scope is per straight-line basic block: any jump / merge / function call
 * clears the tracked map.  Tmps must be single-defined within the block
 * (no later redef).  Restricted to TMP vregs (not VAR/PARAM) because VAR
 * lifetimes span blocks and PARAM values are owned by the caller.
 */
int tcc_ir_opt_symref_const_prop(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  const int n = ir->next_instruction_index;
  int changes = 0;

  int max_tmp_pos = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(dest);
    if (TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    const int pos = TCCIR_DECODE_VREG_POSITION(dvr);
    if (pos > max_tmp_pos)
      max_tmp_pos = pos;
  }
  if (max_tmp_pos == 0)
    return 0;

  /* Per-tmp tracked symref. gen 0 means invalid; bumps on block boundaries. */
  typedef struct
  {
    int gen;
    uint32_t pool_idx;
    int btype;
    uint8_t is_local;
    uint8_t is_const;
    uint8_t is_unsigned;
  } SymrefTmp;

  SymrefTmp *map = tcc_mallocz(sizeof(SymrefTmp) * (max_tmp_pos + 1));
  int current_gen = 1;
  int *block_start_seen = tcc_mallocz(sizeof(int) * n);
  int block_start_gen = 1;
  ir_opt_mark_block_starts(ir, block_start_seen, block_start_gen, n);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (i != 0 && block_start_seen[i] == block_start_gen)
      current_gen++;

    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Control-flow instructions clear the tracked map (lightweight tracker
     * doesn't model cross-block flow) and we do NOT substitute their
     * operands — JUMP/JUMPIF carry the branch target in `dest` and a
     * condition token in `src1`; neither should be rewritten. */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP ||
        q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
    {
      current_gen++;
      continue;
    }
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
    {
      current_gen++;
      /* fall through to substitute call argument operands */
    }

    /* Substitute symref into operand uses (src1, src2).  Skip the dest. */
    for (int slot = 0; slot < 2; slot++)
    {
      int has = (slot == 0) ? irop_config[q->op].has_src1 : irop_config[q->op].has_src2;
      if (!has)
        continue;
      IROperand opnd = (slot == 0) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      /* Operand must be a plain vreg (not already a sym/imm operand). */
      if (opnd.is_sym)
        continue;
      int32_t opnd_vr = irop_get_vreg(opnd);
      if (TCCIR_DECODE_VREG_TYPE(opnd_vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(opnd_vr);
      if (pos > max_tmp_pos || map[pos].gen != current_gen)
        continue;

      /* Replace with a fresh symref operand carrying use-site flags. */
      IROperand new_opnd = irop_make_symref(-1, map[pos].pool_idx, opnd.is_lval, map[pos].is_local,
                                            map[pos].is_const, irop_get_btype(opnd));
      new_opnd.is_unsigned = opnd.is_unsigned;

      if (slot == 0)
        tcc_ir_set_src1(ir, i, new_opnd);
      else
        tcc_ir_set_src2(ir, i, new_opnd);
      changes++;
    }

    /* Record new ASSIGN(symref) definitions for downstream substitution, and
     * invalidate any tracked tmp redefined by a write that does NOT record a
     * fresh copy.  Both cases share the dest-decode prologue, so they live in
     * one branch: an ASSIGN whose source is not a non-lval symref must still
     * fall through to invalidation (it redefines the tmp), which an
     * `if/else if` split would have skipped. */
    if (irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dest);
      if (TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(dvr);
        int recorded = 0;
        if (q->op == TCCIR_OP_ASSIGN)
        {
          IROperand src1 = tcc_ir_op_get_src1(ir, q);
          if (src1.is_sym && !src1.is_lval && pos <= max_tmp_pos)
          {
            map[pos].gen = current_gen;
            map[pos].pool_idx = (uint32_t)src1.u.pool_idx;
            map[pos].btype = irop_get_btype(src1);
            map[pos].is_local = src1.is_local;
            map[pos].is_const = src1.is_const;
            map[pos].is_unsigned = src1.is_unsigned;
            recorded = 1;
          }
        }
        /* Not a fresh copy record → this write kills any tracked symref. */
        if (!recorded && pos <= max_tmp_pos && map[pos].gen == current_gen)
          map[pos].gen = 0;
      }
    }
  }

  tcc_free(map);
  tcc_free(block_start_seen);
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

/* When CMP V_a,V_b is folded because pure_def_equal proved V_a==V_b via the
 * SETIF def-equality path, the SETIFs that produced V_a/V_b — and the CMPs
 * that produced flags for those SETIFs — become dead in the same step.  DCE
 * removes the SETIFs, but cannot reason about flag liveness across CMPs, so
 * the orphan CMPs survive unless we NOP them here. */
static int ir_opt_vreg_use_count(TCCIRState *ir, int32_t vreg)
{
  if (!ir || vreg < 0)
    return -1;
  int n = ir->next_instruction_index;
  int count = 0;
  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == vreg ||
        irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == vreg ||
        ir_opt_mla_accum_vreg(ir, q) == vreg)
      count++;
  }
  return count;
}

static void ir_opt_setif_chain_cleanup(TCCIRState *ir, int def1, int def2, int32_t vr1, int32_t vr2)
{
  if (def1 < 0 || def2 < 0)
    return;
  IRQuadCompact *dq1 = &ir->compact_instructions[def1];
  IRQuadCompact *dq2 = &ir->compact_instructions[def2];
  if (dq1->op != TCCIR_OP_SETIF || dq2->op != TCCIR_OP_SETIF)
    return;
  /* Caller has just NOPped the CMP that consumed V_a/V_b — if no other live
   * use remains, the SETIFs become dead and so do their flag-producing CMPs. */
  if (ir_opt_vreg_use_count(ir, vr1) != 0 || ir_opt_vreg_use_count(ir, vr2) != 0)
    return;

  int cmp_a_idx = def1 - 1;
  while (cmp_a_idx >= 0 && ir->compact_instructions[cmp_a_idx].op == TCCIR_OP_NOP)
    cmp_a_idx--;
  int cmp_b_idx = def2 - 1;
  while (cmp_b_idx >= 0 && ir->compact_instructions[cmp_b_idx].op == TCCIR_OP_NOP)
    cmp_b_idx--;

  dq1->op = TCCIR_OP_NOP;
  dq2->op = TCCIR_OP_NOP;
  if (cmp_a_idx >= 0 && ir->compact_instructions[cmp_a_idx].op == TCCIR_OP_CMP &&
      !ir->compact_instructions[cmp_a_idx].is_jump_target)
    ir->compact_instructions[cmp_a_idx].op = TCCIR_OP_NOP;
  if (cmp_b_idx >= 0 && ir->compact_instructions[cmp_b_idx].op == TCCIR_OP_CMP &&
      !ir->compact_instructions[cmp_b_idx].is_jump_target)
    ir->compact_instructions[cmp_b_idx].op = TCCIR_OP_NOP;
}

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
  if (tcc_ir_opt_pass_disabled("const_prop_tmp")) return 0;
  tcc_pass_timing_init();
  if (!tcc_pass_timing_on) return tcc_ir_opt_const_prop_tmp_core(ir);
  unsigned long _t = tcc_pass_clk_us();
  int _r = tcc_ir_opt_const_prop_tmp_core(ir);
  tcc_pass_timing_add("const_prop_tmp", tcc_pass_clk_us() - _t);
  return _r;
}

/* True if `vreg` is a local VAR whose address is taken anywhere (it appears as
 * a LEA src1).  Such a VAR is memory-resident and can be mutated behind the
 * compiler's back by a store through an aliasing pointer, so its value at an
 * arithmetic def does not necessarily still hold at a later use. */
static int ir_reassoc_var_addr_taken(TCCIRState *ir, int32_t vreg)
{
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) != TCCIR_VREG_TYPE_VAR)
    return 0;
  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_LEA)
      continue;
    if (irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == vreg)
      return 1;
  }
  return 0;
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

    /* Bail on real memory dereferences only. Register-promoted locals
     * (is_lval && is_local) and llocals carry is_lval as a tag but
     * read from a register, so substituting their def-value is sound.
     * Matches the does_memory_deref predicate elsewhere in this file. */
    if (src1.is_lval && !src1.is_const && !src1.is_local && !src1.is_llocal)
      continue;

    /* Direct case: src1 is itself a symref-by-value (the prior symref-prop
     * pass folded the vreg use into a sym operand).  Combine directly. */
    if (src1.is_sym && !src1.is_lval)
    {
      IRPoolSymref *sref = irop_get_symref_ex(ir, src1);
      if (!sref || !sref->sym)
        continue;
      int64_t c2_d = irop_get_imm64_ex(ir, src2);
      int64_t eff_c2_d = (q->op == TCCIR_OP_SUB) ? -c2_d : c2_d;
      int64_t new_addend_d = (int64_t)sref->addend + eff_c2_d;
      if (new_addend_d != (int32_t)new_addend_d)
        continue;
      Sym *target_sym = sref->sym;
      uint32_t sref_flags = sref->flags;
      int btype_d = irop_get_btype(src1);
      uint8_t local_d = src1.is_local;
      uint8_t const_d = src1.is_const;
      uint8_t uns_d = src1.is_unsigned;
      uint32_t pool_idx_d = tcc_ir_pool_add_symref(ir, target_sym, (int32_t)new_addend_d, sref_flags);
      IROperand new_src_d = irop_make_symref(-1, pool_idx_d, 0, local_d, const_d, btype_d);
      new_src_d.is_unsigned = uns_d;
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i, new_src_d);
      changes++;
      continue;
    }

    int32_t src1_vr = irop_get_vreg(src1);
    if (src1_vr < 0)
      continue;

    int def_idx = tcc_ir_find_defining_instruction(ir, src1_vr, i);
    if (def_idx < 0)
      continue;

    /* Verify def_idx and i are in the same straight-line basic block.
     * The merge bitmap only flags multi-predecessor instructions; a
     * forward-jump target with a single (non-fall-through) predecessor
     * is NOT a merge but IS a block boundary, and the linear-scan def
     * lookup will incorrectly find a def that doesn't actually reach i.
     * Bail on any merge OR any prior JUMP/RETURN that breaks linearity. */
    {
      int safe = 1;
      for (int j = def_idx + 1; j <= i; j++)
      {
        if (is_merge[j / 8] & (1 << (j % 8)))
        {
          safe = 0;
          break;
        }
        if (j > 0)
        {
          int prev_op = ir->compact_instructions[j - 1].op;
          if (prev_op == TCCIR_OP_JUMP || prev_op == TCCIR_OP_RETURNVALUE ||
              prev_op == TCCIR_OP_RETURNVOID)
          {
            safe = 0;
            break;
          }
        }
      }
      if (!safe)
        continue;
    }

    IRQuadCompact *def_q = &ir->compact_instructions[def_idx];

    /* Accept either ADD/SUB-with-immediate (the chain case) or an ASSIGN
     * whose source is an address-constant (a symref-by-value).  The latter
     * lets `ADD(T, imm)` collapse into a single `ASSIGN T2 = &S+(addend+imm)`
     * when an earlier pass (e.g. global-init-prop) replaced a const pointer
     * load with a symref. */
    int def_is_assign_symref = 0;
    IROperand def_src1;
    int64_t eff_c1 = 0;

    if (def_q->op == TCCIR_OP_ADD || def_q->op == TCCIR_OP_SUB)
    {
      IROperand def_src2 = tcc_ir_op_get_src2(ir, def_q);
      if (!irop_is_immediate(def_src2))
        continue;
      def_src1 = tcc_ir_op_get_src1(ir, def_q);
      int64_t c1 = irop_get_imm64_ex(ir, def_src2);
      eff_c1 = (def_q->op == TCCIR_OP_SUB) ? -c1 : c1;
    }
    else if (def_q->op == TCCIR_OP_ASSIGN)
    {
      def_src1 = tcc_ir_op_get_src1(ir, def_q);
      if (!def_src1.is_sym || def_src1.is_lval)
        continue;
      def_is_assign_symref = 1;
      eff_c1 = 0;
    }
    else
    {
      continue;
    }

    /* def_src1 becomes the new src1 at the *later* use point `i`.  If it is a
     * real memory dereference (a global/pointer load — is_lval but not a
     * register-promoted local or llocal), moving it forward is unsound: an
     * intervening STORE/CALL between def_idx and i may have changed the
     * memory.  e.g. `T0 = cnt*** + 2; cnt*** = T0; T7 = T0 + 1` must NOT
     * become `T7 = cnt*** + 3` — the second load reads the post-store value.
     *
     * Unlike the use's src1 (handled above, which also bails at src1_vr<0 for
     * a deref carrying no backing vreg), def_src1 here is reached via the inner
     * ADD/SUB whose src1 IS a memory deref, so we must reject it explicitly.
     * is_const is intentionally NOT part of the predicate: a global symref
     * deref is flagged is_const (the *address* is constant) yet its *value*
     * still changes across stores, so a const-permitting check would let the
     * miscompile through.  Register-promoted locals/llocals (is_local/is_llocal)
     * read from a register and stay safe via the inner_vr redefinition scan. */
    if (def_src1.is_lval && !def_src1.is_local && !def_src1.is_llocal)
      continue;

    /* The reassociation replaces src1_vr with def_src1 at the use point.
     * If def_src1 is a vreg, it must not be redefined between def_idx and i
     * (inclusive of def_idx, since def_q itself may write inner_vr, e.g.
     * self-update chains like V0 = V0 + 200 where def_src1 and def_dst are
     * both V0 — substituting reads V0 at the wrong point). */
    int32_t inner_vr = irop_get_vreg(def_src1);
    if (inner_vr >= 0 && !DC_IS_SINGLE_DEF(dc, dc_stride, inner_vr))
    {
      int redefined = 0;
      for (int j = def_idx; j < i; j++)
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

    /* The linear def lookup that found def_idx only sees *explicit* defs of
     * src1_vr; it is blind to a redefinition of an address-taken local's memory
     * through an aliasing pointer store.  If src1_vr (whose def `src1_vr =
     * def_src1 + eff_c1` we are about to forward) — or def_src1 itself — is such
     * an address-taken VAR, an intervening STORE/CALL between def_idx and the
     * use i may have silently changed it, so the `src1_vr == def_src1 + eff_c1`
     * relation no longer holds and folding to `def_src1 + combined` would read a
     * stale value.  (ptr fuzz seed 85636: `u4 = u3 + C; *p = ...; x = u4 + C2`
     * with p == &u4 — the store redefines u4.)  Bail when a memory-clobbering op
     * sits in the gap and either base is an aliasable address-taken VAR.
     *
     * The same hazard applies when def_src1 is a *direct stack-slot load*
     * (is_lval && is_local with no backing vreg, inner_vr < 0): the guard at the
     * top only rejected non-local derefs, and the address-taken-VAR check below
     * never fires for a raw StackLoc (it has no vreg).  Yet an aliasing pointer
     * store in the gap — e.g. `u4 = arr[7] + C; *p = ...; x = u4 + C2` with
     * p == &arr[7] — overwrites the slot, so forwarding `arr[7] + combined` reads
     * the post-store value (ptr fuzz seed 409667).  Reject that too. */
    {
      int gap_clobbers_memory = 0;
      for (int j = def_idx + 1; j < i && !gap_clobbers_memory; j++)
      {
        switch (ir->compact_instructions[j].op)
        {
        case TCCIR_OP_STORE:
        case TCCIR_OP_STORE_INDEXED:
        case TCCIR_OP_STORE_POSTINC:
        case TCCIR_OP_BLOCK_COPY:
        case TCCIR_OP_FUNCCALLVAL:
        case TCCIR_OP_FUNCCALLVOID:
        case TCCIR_OP_INLINE_ASM:
          gap_clobbers_memory = 1;
          break;
        default:
          break;
        }
      }
      if (gap_clobbers_memory &&
          (ir_reassoc_var_addr_taken(ir, src1_vr) ||
           (inner_vr >= 0 && ir_reassoc_var_addr_taken(ir, inner_vr)) ||
           (inner_vr < 0 && def_src1.is_lval)))
        continue;
    }

    int64_t c2 = irop_get_imm64_ex(ir, src2);
    int64_t eff_c2 = (q->op == TCCIR_OP_SUB) ? -c2 : c2;
    int64_t combined = eff_c1 + eff_c2;

    if (combined != (int32_t)combined)
      continue;

    int btype = irop_get_btype(src2);
    LOG_IR_GEN("OPTIMIZE: ADD reassoc at i=%d: (%lld) + (%lld) = %lld",
               i, (long long)eff_c1, (long long)eff_c2, (long long)combined);

    if (def_is_assign_symref)
    {
      /* Fold `T <- symref(S,+A); T2 <- T ± imm` into `T2 <- symref(S,+A±imm)`.
       * Builds a new symref pool entry with the combined addend. */
      IRPoolSymref *sref = irop_get_symref_ex(ir, def_src1);
      if (!sref || !sref->sym)
        continue;
      int64_t new_addend = (int64_t)sref->addend + combined;
      if (new_addend != (int32_t)new_addend)
        continue;
      uint32_t pool_idx = tcc_ir_pool_add_symref(ir, sref->sym, (int32_t)new_addend, sref->flags);
      IROperand new_src = irop_make_symref(-1, pool_idx, 0, def_src1.is_local, def_src1.is_const,
                                           irop_get_btype(def_src1));
      new_src.is_unsigned = def_src1.is_unsigned;
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i, new_src);
      tcc_ir_set_src2(ir, i, IROP_NONE);
    }
    else if (combined == 0)
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

    int def1 = -1, def2 = -1;
    int is_equal = 0;
    int both_nonvreg = (vr1 < 0 && vr2 < 0);

    if (both_nonvreg)
    {
      /* Both operands are immediates or symrefs.  Compare structurally;
       * const-var-prop may leave behind `CMP symref(X), symref(X)` that the
       * vreg-based path below would skip because vr1 == vr2 == -1. */
      is_equal = ir_opt_nonvreg_expr_equal(ir, src1, src2);
      /* Two integer immediates compare equal by value (e.g. `CMP #7, #7`).
       * Scoped to the CMP-operand site (mirroring the asymmetric branch's
       * manual check) rather than broadening the shared
       * `ir_opt_nonvreg_expr_equal` helper, which would perturb its ADD/SUB
       * base-equality callers.  Floats excluded (NaN != NaN). */
      if (!is_equal && irop_is_immediate(src1) && irop_is_immediate(src2) &&
          !src1.is_sym && !src2.is_sym &&
          irop_get_btype(src1) != IROP_BTYPE_FLOAT32 && irop_get_btype(src1) != IROP_BTYPE_FLOAT64 &&
          irop_get_btype(src2) != IROP_BTYPE_FLOAT32 && irop_get_btype(src2) != IROP_BTYPE_FLOAT64)
        is_equal = irop_get_imm64_ex(ir, src1) == irop_get_imm64_ex(ir, src2);
      /* Fallback for symref-vs-symref: the strict check requires every flag
       * to match, but the two operands at a CMP can carry different
       * unsigned/is_lval encodings from how the frontend lowered each side
       * even though both resolve to the same sym+addend.  For an equality
       * comparison the value-identity is enough — comparison of the same
       * symbol's address to itself is always equal. */
      if (!is_equal && src1.is_sym && src2.is_sym && !src1.is_lval && !src2.is_lval)
      {
        IRPoolSymref *a_ref = irop_get_symref_ex(ir, src1);
        IRPoolSymref *b_ref = irop_get_symref_ex(ir, src2);
        if (a_ref && b_ref && a_ref->sym == b_ref->sym && a_ref->addend == b_ref->addend)
          is_equal = 1;
      }
      /* Same-symbol deref read: CMP *sym, *sym where both operands load
       * from the same non-volatile global.  The reads see the same value,
       * so the comparison result is known (x==x, x>=x, etc.).  Safe for
       * integer types; skip floats (NaN != NaN). */
      if (!is_equal && src1.is_sym && src2.is_sym && src1.is_lval && src2.is_lval)
      {
        IRPoolSymref *a_ref = irop_get_symref_ex(ir, src1);
        IRPoolSymref *b_ref = irop_get_symref_ex(ir, src2);
        if (a_ref && b_ref && a_ref->sym == b_ref->sym &&
            a_ref->addend == b_ref->addend)
        {
          Sym *sym = a_ref->sym;
          int ttype = sym->type.t;
          int btype = ttype & VT_BTYPE;
          if (!(ttype & VT_VOLATILE) &&
              btype != VT_FLOAT && btype != VT_DOUBLE && btype != VT_LDOUBLE)
            is_equal = 1;
        }
      }
      if (!is_equal)
        continue;
    }
    else if ((vr1 >= 0) != (vr2 >= 0))
    {
      /* Asymmetric: one side is a vreg, the other is a non-vreg literal.
       * Resolve the vreg by chasing its single defining ASSIGN to its
       * literal value, then compare against the other side.  Without this,
       * const-var-prop's symref propagation produces e.g. `CMP V0, &f+5`
       * which the strict both-vregs path below would reject, even though
       * V0's only def is `ASSIGN V0 <-- &f+5`.
       *
       * Skip address-taken VARs: the value at the CMP may differ from the
       * defining ASSIGN's source if a store-through-pointer happened in
       * between. */
      int32_t v_vr = (vr1 >= 0) ? vr1 : vr2;
      IROperand other = (vr1 >= 0) ? src2 : src1;
      if (DC_IS_SINGLE_DEF(dc, dc_stride, v_vr))
      {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, v_vr);
        if (!interval || !interval->addrtaken)
        {
          int vdef = tcc_ir_find_defining_instruction(ir, v_vr, i);
          if (vdef >= 0)
          {
            IRQuadCompact *vdq = &ir->compact_instructions[vdef];
            if (vdq->op == TCCIR_OP_ASSIGN)
            {
              IROperand vs = tcc_ir_op_get_src1(ir, vdq);
              if (irop_get_vreg(vs) < 0)
              {
                /* Both literals now: compare exactly the same way the
                 * both_nonvreg branch above does. */
                is_equal = ir_opt_nonvreg_expr_equal(ir, vs, other);
                if (!is_equal && vs.is_sym && other.is_sym &&
                    !vs.is_lval && !other.is_lval)
                {
                  IRPoolSymref *a_ref = irop_get_symref_ex(ir, vs);
                  IRPoolSymref *b_ref = irop_get_symref_ex(ir, other);
                  if (a_ref && b_ref && a_ref->sym == b_ref->sym &&
                      a_ref->addend == b_ref->addend)
                    is_equal = 1;
                }
                if (!is_equal && irop_is_immediate(vs) && irop_is_immediate(other) &&
                    !vs.is_sym && !other.is_sym)
                {
                  is_equal = irop_get_imm64_ex(ir, vs) == irop_get_imm64_ex(ir, other);
                }
              }
            }
          }
        }
      }
      if (!is_equal)
        continue;
    }
    else
    {
      if (vr1 < 0 || vr2 < 0)
        continue;

      /* Operand value-identity requires matching lval-ness: `*(p)` (a load
       * through p) and `p` (the address) are different values even when p's
       * defining expression is identical.  Without this, `ptr >= base + N`
       * (where &base[N] aliases &ptr) mis-folds to a constant. */
      if (src1.is_lval != src2.is_lval)
        continue;

      if (vr1 == vr2)
      {
        /* x OP x: a value compared against itself.  CMP is an integer compare
         * (floats lower to FCMP), so a plain register value is always
         * determinate — evaluate_compare_condition(0,0,tok) gives the result.
         * Require matching width and signedness: `CMP x:I8, x:I32` compares a
         * truncation against the full value and is NOT always equal.  A
         * dereference *(V) OP *(V) could read a volatile location twice, so
         * only fold the non-lval (register-value) form. */
        if (src1.is_lval ||
            irop_get_btype(src1) != irop_get_btype(src2) ||
            src1.is_unsigned != src2.is_unsigned)
          continue;
        is_equal = 1;
      }
      else
      {
        /* Both operands must have a single reaching definition */
        def1 = tcc_ir_find_defining_instruction(ir, vr1, i);
        def2 = tcc_ir_find_defining_instruction(ir, vr2, i);
        if (def1 < 0 || def2 < 0 || def1 == def2)
          continue;

        /* Try standard def equality (works for single-def vregs) */
        if (DC_IS_SINGLE_DEF(dc, dc_stride, vr1) && DC_IS_SINGLE_DEF(dc, dc_stride, vr2))
          is_equal = ir_opt_pure_def_equal(ir, def1, def2, 0);
      }
    }

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

          /* A dereferenced base `*(V)` (is_lval) and a plain address base `V`
           * are different values even when V resolves to the same definition.
           * Without this, `*(p) + K` (loaded value + K) is equated with
           * `p + K` (an address), mis-folding `(c->field0 + K) > c->fieldK`
           * (K == field offset) to a constant. */
          if (base1.is_lval == base2.is_lval && bvr1 >= 0 && bvr2 >= 0)
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
    int folded = 0;
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
      folded = 1;
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
      folded = 1;
    }
    else if (next->op == TCCIR_OP_SETIF)
    {
      IROperand cond = tcc_ir_op_get_src1(ir, next);
      int tok = (int)irop_get_imm64_ex(ir, cond);
      int result = evaluate_compare_condition(0, 0, tok);
      if (result < 0)
        continue;
      IROperand setif_dest = tcc_ir_op_get_dest(ir, next);
      q->op = TCCIR_OP_NOP;
      next->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_dest(ir, i + 1, setif_dest);
      tcc_ir_set_src1(ir, i + 1, irop_make_imm32(-1, result, irop_get_btype(setif_dest)));
      tcc_ir_set_src2(ir, i + 1, IROP_NONE);
      changes++;
      folded = 1;
    }

    if (folded)
      ir_opt_setif_chain_cleanup(ir, def1, def2, vr1, vr2);
  }

  tcc_free(dc);

  if (changes)
    changes += tcc_ir_opt_dce(ir);

  return changes;
}

typedef struct StackAddrValue
{
  int off;
  int is_param;
} StackAddrValue;

static int ir_resolve_stack_addr_value_ex(TCCIRState *ir, IROperand op, int at_idx,
                                          StackAddrValue *out, int depth);

static int ir_has_backward_control_flow(TCCIRState *ir)
{
  int n = ir ? ir->next_instruction_index : 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int target = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
      if (target >= 0 && target <= i)
        return 1;
    }
    else if (q->op == TCCIR_OP_IJUMP)
    {
      return 1;
    }
  }

  return 0;
}

/* Resolve an operand at instruction `at_idx` to a stack-frame offset, if
 * provably constant.  Recognized shapes:
 *   - direct address operand:  `Addr[StackLoc[X]]` → X
 *   - vreg V with same-BB defs of the form `V = Addr[StackLoc[X]]` followed
 *     by zero or more `V = V ± const` self-updates → X + sum(const).
 * Returns 1 and writes *out_off on success, 0 otherwise.
 *
 * Conservative: stops at any other def of V or at any jump_target between
 * the def and `at_idx` (don't cross BB boundaries / merge points).  A vreg
 * operand read at an instruction that is ITSELF a jump target is never
 * resolved: its value depends on which edge entered (docs/bugs.md #2 — a
 * loop-exit `CMP ptr,end` at a back-edge target resolved ptr through the
 * preheader init only, missing the in-loop `ptr += stride` redefinition,
 * and the fold deleted the loop's only exit test). */
static int ir_resolve_stack_addr_value(TCCIRState *ir, IROperand op, int at_idx, int *out_off)
{
  StackAddrValue value;
  if (!ir_resolve_stack_addr_value_ex(ir, op, at_idx, &value, 0))
    return 0;
  *out_off = value.off;
  return 1;
}

/* Presence map of vregs that have at least one "def" as seen by the backward
 * walk in ir_resolve_stack_addr_value_ex (any has_dest op other than
 * STORE/STORE_INDEXED/STORE_POSTINC/FUNCPARAMVAL writing that vreg).  Built
 * once per driving pass so the resolver can answer "this vreg has no def — the
 * walk would scan to the start and return 0" in O(1).  Indexed by
 * pos*3 + (type-1); a vreg outside the map's range is treated as absent.
 * Returning early only for the no-def case keeps the walk's merge-crossing
 * semantics intact (a no-def walk can only ever return 0). */
static uint8_t *sav_def_present;
static int sav_def_present_maxpos = -1;

static int sav_is_def_op(int op)
{
  return op != TCCIR_OP_STORE && op != TCCIR_OP_STORE_INDEXED &&
         op != TCCIR_OP_STORE_POSTINC && op != TCCIR_OP_FUNCPARAMVAL;
}

static void sav_build_def_map(TCCIRState *ir)
{
  int n = ir ? ir->next_instruction_index : 0;
  sav_def_present = NULL;
  sav_def_present_maxpos = -1;
  int maxpos = -1;
  for (int j = 0; j < n; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest || !sav_is_def_op(q->op))
      continue;
    int32_t dvr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    if (dvr < 0)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(dvr);
    if (pos > maxpos)
      maxpos = pos;
  }
  if (maxpos < 0)
    return;
  sav_def_present = (uint8_t *)tcc_mallocz((size_t)(maxpos + 1) * 3);
  sav_def_present_maxpos = maxpos;
  for (int j = 0; j < n; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest || !sav_is_def_op(q->op))
      continue;
    int32_t dvr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    if (dvr < 0)
      continue;
    int type = TCCIR_DECODE_VREG_TYPE(dvr);
    int pos = TCCIR_DECODE_VREG_POSITION(dvr);
    if (type >= 1 && type <= 3)
      sav_def_present[pos * 3 + (type - 1)] = 1;
  }
}

static void sav_free_def_map(void)
{
  tcc_free(sav_def_present);
  sav_def_present = NULL;
  sav_def_present_maxpos = -1;
}

/* Returns 1 if vr definitely has no qualifying def (walk would return 0). */
static int sav_vreg_has_no_def(int32_t vr)
{
  if (!sav_def_present)
    return 0; /* map not built — be safe, let the walk run */
  int type = TCCIR_DECODE_VREG_TYPE(vr);
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (type < 1 || type > 3 || pos > sav_def_present_maxpos)
    return 1; /* outside any recorded def */
  return sav_def_present[pos * 3 + (type - 1)] == 0;
}

static int ir_resolve_stack_addr_value_ex(TCCIRState *ir, IROperand op, int at_idx,
                                          StackAddrValue *out, int depth)
{
  if (!ir || !out || depth > 12)
    return 0;

  /* Direct stack address (Addr[StackLoc[X]], i.e. STACKOFF tag, no vreg, not lval). */
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && irop_get_vreg(op) == -1 && !op.is_lval)
  {
    out->off = (int)irop_get_imm64_ex(ir, op);
    out->is_param = op.is_param;
    return 1;
  }

  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;

  /* Fast reject: a vreg with no def anywhere can only make the walk below
   * scan to the start and return 0 — skip the scan. */
  if (sav_vreg_has_no_def(vr))
    return 0;

  /* A vreg read at a merge point (jump target) has an edge-dependent value:
   * a def found by the linear backward walk holds only for the fall-through
   * path, not for the jumped-in edge(s).  This check also covers recursive
   * calls: resolving a def instruction's own operands uses at_idx = def_j,
   * so a def sitting at a merge point refuses to resolve its inputs. */
  if (at_idx >= 0 && at_idx < ir->next_instruction_index &&
      ir->compact_instructions[at_idx].is_jump_target)
    return 0;

  for (int j = at_idx - 1; j >= 0; j--)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];

    /* Determine whether this instruction is a real def of vr.  STORE-style
     * ops carry an address-of-write in dest (a use, not a def); FUNCPARAMVAL
     * dest carries the param value (also a use). */
    int is_def_of_vr = 0;
    if (q->op != TCCIR_OP_NOP && irop_config[q->op].has_dest && sav_is_def_op(q->op))
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (irop_get_vreg(dest) == vr)
        is_def_of_vr = 1;
    }

    /* Never cross a merge point (instruction with is_jump_target set, NOPs
     * included — a NOPed jump target still merges control flow): a value
     * flowing in over the jumped-in edge may differ from the fall-through
     * value.  The found def itself being a jump target is fine — its RESULT
     * dominates the straight-line range down to at_idx (no entries between);
     * its own operands are guarded by the at_idx check in the recursion. */
    if (!is_def_of_vr)
    {
      if (q->is_jump_target)
        return 0;
      continue;
    }

    if (q->op == TCCIR_OP_ASSIGN)
    {
      IROperand src = tcc_ir_op_get_src1(ir, q);
      return ir_resolve_stack_addr_value_ex(ir, src, j, out, depth + 1);
    }
    if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      StackAddrValue base;
      int64_t c;
      if (!ir_resolve_stack_addr_value_ex(ir, s1, j, &base, depth + 1))
        return 0;
      if (!irop_is_immediate(s2))
        return 0;
      c = irop_get_imm64_ex(ir, s2);
      if (q->op == TCCIR_OP_SUB)
        c = -c;
      c += base.off;
      if (c != (int32_t)c)
        return 0;
      out->off = (int)c;
      out->is_param = base.is_param;
      return 1;
    }
    /* Some other op writes vr — give up. */
    return 0;
  }
  return 0;
}

/* Canonicalize dereferences through pointers whose value is a known stack
 * address, and fold simple stack-address arithmetic.  This exposes ordinary
 * StackLoc STORE/LOAD/CMP patterns to the existing store-load forwarding and
 * branch folders.
 *
 * Example:
 *   T0 = Addr[StackLoc[-16]]
 *   T0***DEREF*** = #10       -> StackLoc[-16] = #10
 *   T1 = (T0 + 1) - T0        -> T1 = #1
 */
int tcc_ir_opt_stack_addr_simplify(TCCIRState *ir)
{
  int n = ir ? ir->next_instruction_index : 0;
  int changes = 0;

  if (ir_has_backward_control_flow(ir))
    return 0;

  sav_build_def_map(ir);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (q->op == TCCIR_OP_STORE)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (dest.is_lval &&
          !(irop_get_tag(dest) == IROP_TAG_STACKOFF && dest.is_local && !dest.is_llocal))
      {
        StackAddrValue addr;
        if (ir_resolve_stack_addr_value_ex(ir, dest, i, &addr, 0))
        {
          IROperand direct = irop_make_stackoff(-1, addr.off, 1, 0, addr.is_param, irop_get_btype(dest));
          direct.is_unsigned = dest.is_unsigned;
          tcc_ir_set_dest(ir, i, direct);
          changes++;
        }
      }
      continue;
    }

    if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) && irop_config[q->op].has_src1)
    {
      IROperand src = tcc_ir_op_get_src1(ir, q);
      if (src.is_lval &&
          !(irop_get_tag(src) == IROP_TAG_STACKOFF && src.is_local && !src.is_llocal))
      {
        StackAddrValue addr;
        if (ir_resolve_stack_addr_value_ex(ir, src, i, &addr, 0))
        {
          IROperand direct = irop_make_stackoff(-1, addr.off, 1, 0, addr.is_param, irop_get_btype(src));
          direct.is_unsigned = src.is_unsigned;
          tcc_ir_set_src1(ir, i, direct);
          changes++;
        }
      }
    }

    if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && irop_config[q->op].has_dest)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      StackAddrValue a1, a2;
      int folded = 0;
      int64_t result = 0;

      if (q->op == TCCIR_OP_SUB &&
          ir_resolve_stack_addr_value_ex(ir, src1, i, &a1, 0) &&
          ir_resolve_stack_addr_value_ex(ir, src2, i, &a2, 0) &&
          a1.is_param == a2.is_param)
      {
        result = (int64_t)a1.off - (int64_t)a2.off;
        folded = 1;
      }

      if (folded && result == (int32_t)result)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)result, irop_get_btype(dest)));
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
      }
    }
  }

  sav_free_def_map();
  return changes;
}

/* Fold CMP whose two operands provably resolve to the same stack-frame
 * offset (one side a vreg holding `Addr[StackLoc[X]] + N`, the other side
 * a literal `Addr[StackLoc[X+N]]`).  Rewrites the following JUMPIF/SELECT
 * by precomputing the comparison result, matching `cmp_expr_fold`'s
 * downstream logic. */
int tcc_ir_opt_cmp_stack_addr_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 2)
    return 0;

  /* IJUMP safety: address-taken labels (`&&label`) aren't marked
   * is_jump_target, so the backward def-walk could cross a target
   * unaware. See [[project_global_sl_fwd_ijump_safety]]. */
  for (int i = 0; i < n; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return 0;
  }

  sav_build_def_map(ir);

  for (int i = 0; i < n - 1; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_CMP)
      continue;

    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    IROperand s2 = tcc_ir_op_get_src2(ir, q);

    int off1, off2;
    if (!ir_resolve_stack_addr_value(ir, s1, i, &off1))
      continue;
    if (!ir_resolve_stack_addr_value(ir, s2, i, &off2))
      continue;
    if (off1 != off2)
      continue; /* could fold to !equal too, but be conservative */

    IRQuadCompact *next = &ir->compact_instructions[i + 1];
    if (next->op == TCCIR_OP_JUMPIF)
    {
      IROperand cond = tcc_ir_op_get_src1(ir, next);
      int tok = (int)irop_get_imm64_ex(ir, cond);
      int result = evaluate_compare_condition(0, 0, tok); /* equal-equal */
      if (result < 0)
        continue;
      IROperand jmp_dest = tcc_ir_op_get_dest(ir, next);
      LOG_IR_GEN("OPTIMIZE: CMP stack-addr fold at %d (off=%d, %s)",
                 i, off1, result ? "taken" : "not taken");
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
  sav_free_def_map();
  return changes;
}

int tcc_ir_opt_cmp_stack_addr_fold_ex(IROptCtx *ctx) { return tcc_ir_opt_cmp_stack_addr_fold(ctx->ir); }

/* Single-Value Temp Propagation
 *
 * If ALL definitions of a TEMP are ASSIGN/LOAD of the same immediate
 * constant, replace every use of that TEMP with the constant.  This
 * handles phi-like merges where both arms assign the same value
 * (e.g. after VRP folds a conditional set). */
int tcc_ir_opt_single_value_tmp(TCCIRState *ir)
{
#define SVT_MAX_TEMPS 128
  int n = ir->next_instruction_index;
  if (n < 2)
    return 0;

  int max_tmp = -1;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(d);
    if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP) {
      int pos = TCCIR_DECODE_VREG_POSITION(dvr);
      if (pos > max_tmp)
        max_tmp = pos;
    }
  }
  if (max_tmp < 0 || max_tmp >= SVT_MAX_TEMPS)
    return 0;

  int count = max_tmp + 1;
  uint8_t state[SVT_MAX_TEMPS];
  int32_t vals[SVT_MAX_TEMPS];
  memset(state, 0, count);

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(d);
    if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(dvr);
    if (pos >= count) continue;

    if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) &&
        state[pos] != 2) {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      if (irop_is_immediate(s) && !s.is_lval &&
          irop_get_btype(s) == IROP_BTYPE_INT32) {
        int32_t v = (int32_t)irop_get_imm64_ex(ir, s);
        if (state[pos] == 0) {
          state[pos] = 1;
          vals[pos] = v;
        } else if (vals[pos] != v) {
          state[pos] = 2;
        }
        continue;
      }
    }
    state[pos] = 2;
  }

  int changes = 0;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op != TCCIR_OP_RETURNVALUE)
      continue;
    for (int k = 0; k < 2; k++) {
      IROperand op = k == 0 ? tcc_ir_op_get_src1(ir, q)
                            : tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(op);
      if (vr < 0 || op.is_lval)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos >= count || state[pos] != 1)
        continue;
      IROperand imm = irop_make_imm32(-1, vals[pos], IROP_BTYPE_INT32);
      if (k == 0)
        tcc_ir_set_src1(ir, i, imm);
      else
        tcc_ir_set_src2(ir, i, imm);
      changes++;
    }
  }

  if (changes) {
    /* Let DCE reclaim the now-dead constant defs.  Do NOT NOP them directly by
     * state[pos] == 1: a single-value temp may still have uses OTHER than the
     * RETURNVALUE we just folded (e.g. `OR T, #const` in a bitfield store),
     * because Phase 2 only propagates into RETURNVALUE operands.  Blindly
     * removing such a def leaves a dangling use → a use-before-def miscompile.
     * DCE removes a def only when it has no remaining uses, which is exactly
     * the condition we need. */
    changes += tcc_ir_opt_dce(ir);
  }

  if (changes) {
    n = ir->next_instruction_index;
    int64_t ret_val = 0;
    int ret_btype = 0;
    int ret_idx = -1;
    int all_same_ret = 1;
    int has_side_effect = 0;
    for (int i = 0; i < n && all_same_ret && !has_side_effect; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      switch (q->op) {
      case TCCIR_OP_RETURNVALUE: {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (!irop_is_immediate(s)) { all_same_ret = 0; break; }
        int64_t v = irop_get_imm64_ex(ir, s);
        if (ret_idx < 0) {
          ret_val = v; ret_btype = irop_get_btype(s); ret_idx = i;
        } else if (v != ret_val) { all_same_ret = 0; }
        break;
      }
      case TCCIR_OP_STORE: case TCCIR_OP_STORE_INDEXED:
      case TCCIR_OP_FUNCCALLVAL: case TCCIR_OP_FUNCCALLVOID:
      case TCCIR_OP_VLA_ALLOC: case TCCIR_OP_VLA_SP_SAVE:
      case TCCIR_OP_VLA_SP_RESTORE: case TCCIR_OP_TRAP:
      case TCCIR_OP_RETURNVOID:
        has_side_effect = 1; break;
      default: break;
      }
    }
    if (all_same_ret && !has_side_effect && ret_idx >= 0) {
      for (int i = 0; i < n; i++) {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_RETURNVALUE)
          continue;
        q->op = TCCIR_OP_NOP;
        changes++;
      }
      if (ret_idx > 0) {
        ir->compact_instructions[0].op = TCCIR_OP_RETURNVALUE;
        IROperand rv = irop_make_imm32(-1, (int32_t)ret_val, ret_btype);
        tcc_ir_set_src1(ir, 0, rv);
        ir->compact_instructions[ret_idx].op = TCCIR_OP_NOP;
        changes++;
      }
      changes += tcc_ir_opt_dce(ir);
    }
  }
  return changes;
#undef SVT_MAX_TEMPS
}

int tcc_ir_opt_single_value_tmp_ex(IROptCtx *ctx) { return tcc_ir_opt_single_value_tmp(ctx->ir); }

int tcc_ir_opt_const_prop_tmp_ex(IROptCtx *ctx) { return tcc_ir_opt_const_prop_tmp(ctx->ir); }
int tcc_ir_opt_const_var_prop_ex(IROptCtx *ctx) { return tcc_ir_opt_const_var_prop(ctx->ir); }
int tcc_ir_opt_global_init_prop_ex(IROptCtx *ctx) { return tcc_ir_opt_global_init_prop(ctx->ir); }
int tcc_ir_opt_symref_const_prop_ex(IROptCtx *ctx) { return tcc_ir_opt_symref_const_prop(ctx->ir); }
int tcc_ir_opt_value_tracking_ex(IROptCtx *ctx) { return tcc_ir_opt_value_tracking(ctx->ir); }
int tcc_ir_opt_add_reassoc_ex(IROptCtx *ctx) { return tcc_ir_opt_add_reassoc(ctx->ir); }
int tcc_ir_opt_cmp_expr_fold_ex(IROptCtx *ctx) { return tcc_ir_opt_cmp_expr_fold(ctx->ir); }
int tcc_ir_opt_const_string_calls_ex(IROptCtx *ctx) { return tcc_ir_opt_const_string_calls(ctx->ir); }
