/*
 *  TCC IR - Dead Code & Cleanup Passes
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
#include "opt_xform.h"
#include "opt_alias.h"
#include "opt_utils.h"
#include "opt_du.h"
#include "opt_loop_utils.h"
#include "cfg.h"
#include "licm.h"

static int tcc_ir_callee_is_noreturn(Sym *callee)
{
  if (!callee)
    return 0;
  if (callee->type.ref && callee->type.ref->f.func_noreturn)
    return 1;

  ElfSym *esym = elfsym(callee);
  if (esym && esym->st_shndx != SHN_UNDEF)
    return 0;

  const char *name = get_tok_str(callee->asm_label ? callee->asm_label : callee->v, NULL);
  return name && (!strcmp(name, "abort") || !strcmp(name, "exit") || !strcmp(name, "_Exit") ||
                  !strcmp(name, "quick_exit"));
}

/* Dead Code Elimination pass
 * Removes unreachable instructions by following control flow from entry.
 * Returns 1 if any instructions were eliminated, 0 otherwise.
 */
int tcc_ir_opt_dce(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* If the function contains any IJUMP (computed goto / indirect jump),
   * skip DCE entirely.  The targets of an IJUMP are determined at runtime
   * (typically via labels-as-values stored in arrays), so we cannot
   * statically determine which basic blocks are reachable from them.
   * Attempting to do DCE would incorrectly eliminate label target blocks
   * that are only reachable through the computed goto. */
  for (int i = 0; i < n; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return 0;
  }

  uint8_t *reachable = tcc_mallocz((n + 7) / 8);
  int *worklist = tcc_malloc(n * sizeof(int));
  int worklist_head = 0, worklist_tail = 0;

/* Mark instruction as reachable if not already marked */
#define MARK_REACHABLE(idx)                                                                                            \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((idx) >= 0 && (idx) < n && !(reachable[(idx) / 8] & (1 << ((idx) % 8))))                                       \
    {                                                                                                                  \
      reachable[(idx) / 8] |= (1 << ((idx) % 8));                                                                      \
      worklist[worklist_tail++] = (idx);                                                                               \
    }                                                                                                                  \
  } while (0)

  /* Start from instruction 0 */
  MARK_REACHABLE(0);

  while (worklist_head < worklist_tail)
  {
    int i = worklist[worklist_head++];
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    switch (q->op)
    {
    case TCCIR_OP_JUMP:
      /* Unconditional jump - only the target is reachable */
      MARK_REACHABLE((int)dest.u.imm32);
      break;
    case TCCIR_OP_JUMPIF:
      /* Conditional jump - both target and fall-through are reachable */
      MARK_REACHABLE((int)dest.u.imm32);
      MARK_REACHABLE(i + 1);
      break;
    case TCCIR_OP_SWITCH_TABLE:
    {
      /* Switch table - all targets are reachable */
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int table_id = (int)irop_get_imm64_ex(ir, src2);
      if (table_id >= 0 && table_id < ir->num_switch_tables)
      {
        TCCIRSwitchTable *table = &ir->switch_tables[table_id];
        for (int j = 0; j < table->num_entries; j++)
          MARK_REACHABLE(table->targets[j]);
        /* Also mark the default target */
        MARK_REACHABLE(table->default_target);
      }
      /* SWITCH_TABLE is a terminator - no fall-through */
      break;
    }
    case TCCIR_OP_IJUMP:
      /* Indirect jump (computed goto).
         The successor set is not statically known, but in typical patterns
         (like GCC's labels-as-values jump tables) targets are within the same
         function and code continues at/after those labels.
         Conservatively keep fall-through reachable to avoid deleting label
         blocks and subsequent code. */
      MARK_REACHABLE(i + 1);
      break;
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_TRAP:
      /* Return/trap - no successor (epilogue is implicit, trap never returns) */
      break;
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    {
      /* If the callee is provably noreturn (either attributed or inferred by
       * the inter-procedural noreturn_collapse / infinite_self_recursion /
       * uninit_dom_return passes — they set sym->f.func_noreturn at end of
       * gen_function), the call never returns and code after it is dead. */
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (tcc_ir_callee_is_noreturn(callee))
        break; /* terminator: no fall-through */
      MARK_REACHABLE(i + 1);
      break;
    }
    default:
      /* All other instructions fall through to the next */
      MARK_REACHABLE(i + 1);
      break;
    }
  }

#undef MARK_REACHABLE

  /* Mark unreachable instructions as NOP (no array compaction needed) */
  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    if (!(reachable[i / 8] & (1 << (i % 8))))
    {
      ir->compact_instructions[i].op = TCCIR_OP_NOP;
      changes++;
    }
  }

  tcc_free(reachable);
  tcc_free(worklist);

  return changes;
}

int tcc_ir_opt_dce_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_dce(ctx->ir);
}

/* Orphan CMP elimination - NOP CMP/TEST_ZERO (and FUNCCALLVOID to flag-setting
 * soft-float compare helpers __aeabi_cfcmple / __aeabi_cdcmple) whose flag
 * result is not consumed by a SETIF or JUMPIF before the next flag-clobbering
 * op or basic-block boundary.  Various folding passes can leave orphan flag
 * setters behind when their SETIF/JUMPIF consumers get folded into constants
 * or get NOPed by degenerate-branch elimination — the flag setter itself
 * looks "essential" to plain DCE (it sets flags as a side effect) but is
 * observably dead.
 *
 * Flag semantics on ARM (and modeled in this IR): JUMP does not clobber
 * flags; an unconditional JUMP after a CMP propagates the flags to the
 * target block, where they may be consumed by a SETIF.  We follow JUMPs
 * (with a visited bitmap to bound work) but stop at JUMPIF on the safe
 * side — it consumes our flags so the CMP is live anyway. */
static int orphan_cmp_scan(TCCIRState *ir, int from_idx, uint8_t *visited)
{
  int n = ir->next_instruction_index;
  int j = from_idx;
  while (j < n)
  {
    if (visited[j / 8] & (1 << (j % 8)))
      return 0; /* loop — conservatively LIVE */
    visited[j / 8] |= (1 << (j % 8));

    IRQuadCompact *nq = &ir->compact_instructions[j];
    if (nq->op == TCCIR_OP_NOP)
    {
      j++;
      continue;
    }
    /* A join point (jump_target) is reached by alternate predecessors that
     * may not have executed our flag setter.  We still continue scanning:
     * if no SETIF/JUMPIF consumer is found before the next flag clobber or
     * function exit, our flag setter is observably dead.  (Finding a
     * consumer downstream means our setter IS read on our path, regardless
     * of what alternate predecessors did.) */

    switch (nq->op)
    {
    case TCCIR_OP_SETIF:
    case TCCIR_OP_JUMPIF:
      /* Consumer of our flags - CMP is live. */
      return 0;
    case TCCIR_OP_JUMP:
    {
      /* Flags propagate across unconditional JUMPs.  Follow the target. */
      IROperand dest = tcc_ir_op_get_dest(ir, nq);
      int target = (int)dest.u.imm32;
      if (target < 0)
        return 0; /* defensive: malformed JUMP — keep CMP */
      if (target >= n)
        return 1; /* JUMP past end (implicit return) — no consumer */
      j = target;
      continue;
    }
    case TCCIR_OP_CMP:
    case TCCIR_OP_TEST_ZERO:
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_TRAP:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
      /* Flag-clobbering or terminator before any consumer. */
      return 1;
    default:
      break;
    }
    j++;
  }
  /* End of function with no consumer found. */
  return 1;
}

int tcc_ir_opt_orphan_cmp_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  int changes = 0;
  int bytes = (n + 7) / 8;
  uint8_t *visited = tcc_mallocz(bytes);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int is_flag_cmp_call = 0;

    if (q->op == TCCIR_OP_FUNCCALLVOID)
    {
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      const char *name = callee ? get_tok_str(callee->v, NULL) : NULL;
      if (!ir_opt_is_flag_cmp_helper_name(name))
        continue;
      is_flag_cmp_call = 1;
    }
    else if (q->op != TCCIR_OP_CMP && q->op != TCCIR_OP_TEST_ZERO)
      continue;

    if (q->is_jump_target)
      continue;

    for (int b = 0; b < bytes; b++)
      visited[b] = 0;

    if (orphan_cmp_scan(ir, i + 1, visited))
    {
      if (is_flag_cmp_call)
        ir_opt_nop_call_params(ir, i);
      q->op = TCCIR_OP_NOP;
      changes++;
    }
  }
  tcc_free(visited);
  return changes;
}

int tcc_ir_opt_orphan_cmp_elim_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_orphan_cmp_elim(ctx->ir);
}

/* ============================================================================
 * Useless Function Body - if no instruction in the function has an observable
 * side effect, NOP the entire body.
 * ============================================================================
 *
 * After all the data-flow folds (const prop, store-load fwd, DCE/DSE) run,
 * a function body can be reduced to a forest of LOAD/CMP/JUMPIF chains whose
 * eventual consumer (a STORE, CALL, RETURNVALUE, ...) has been killed.  None
 * of the surviving ops can be eliminated individually:
 *   - LOAD/ASSIGN from a non-volatile sym: still has a "use" via the temp
 *   - CMP: uses the temp from the LOAD
 *   - JUMPIF: keeps the CMP alive (control op, no temp dest)
 * Each link of the chain is alive because the next link reads it, but the
 * tail of the chain (the JUMPIF) leads nowhere observable.
 *
 * If we can prove the *entire function* has no observable side effect, we
 * can drop the whole body.  This is the GCC -O2 behavior for things like
 * compile/20040304-2.c — a void function whose only "work" is comparing
 * cast-folded zeros and conditionally storing into a dead path.
 *
 * Essential ops (function has observable behavior; skip the pass):
 *   - STORE / STORE_INDEXED / STORE_POSTINC, except late-reopt direct writes
 *     to non-volatile PARAM vregs after their caller-visible consumers died
 *   - FUNCCALLVAL / FUNCCALLVOID unless the callee is a curated pure aeabi helper
 *   - FUNCPARAMVAL / FUNCPARAMVOID unless it belongs to such a pure call
 *   - RETURNVALUE / TRAP / IJUMP
 *   - INLINE_ASM / ASM_INPUT / ASM_OUTPUT
 *   - CALLSEQ_BEGIN / CALLARG_REG / CALLARG_STACK / CALLSEQ_END
 *   - INIT_CHAIN_SLOT (writes nested-fn chain slot)
 *   - PREFETCH (architectural hint — keep conservatively)
 *   - SETJMP / LONGJMP / NL_SETJMP / NL_LONGJMP
 *   - BUILTIN_APPLY_ARGS / BUILTIN_APPLY / BUILTIN_RETURN
 *   - VLA_ALLOC / VLA_SP_SAVE / VLA_SP_RESTORE (stack manipulation)
 *   - BLOCK_COPY (memory write)
 *   - SWITCH_TABLE / SWITCH_LOAD (jumps + loads)
 *   - any LOAD/ASSIGN/CMP/... whose src1/src2 references a sym with VT_VOLATILE
 *
 * Everything else is "non-essential": pure arithmetic, comparisons, jumps,
 * loads from non-volatile memory, etc.  When the entire body is non-essential,
 * the function is observationally a no-op and we NOP everything.
 */
static int ir_opt_pure_call_id_test(const uint8_t *pure_call_ids, int pure_call_id_bytes, int call_id)
{
  return call_id >= 0 && call_id / 8 < pure_call_id_bytes &&
         (pure_call_ids[call_id / 8] & (uint8_t)(1u << (call_id & 7)));
}

static void ir_opt_pure_call_id_mark(uint8_t **pure_call_ids, int *pure_call_id_bytes, int call_id)
{
  if (call_id < 0)
    return;

  int needed_bytes = call_id / 8 + 1;
  if (needed_bytes > *pure_call_id_bytes)
  {
    int old_bytes = *pure_call_id_bytes;
    int new_bytes = old_bytes ? old_bytes * 2 : 32;
    while (new_bytes < needed_bytes)
      new_bytes *= 2;
    *pure_call_ids = tcc_realloc(*pure_call_ids, new_bytes);
    memset(*pure_call_ids + old_bytes, 0, new_bytes - old_bytes);
    *pure_call_id_bytes = new_bytes;
  }

  (*pure_call_ids)[call_id / 8] |= (uint8_t)(1u << (call_id & 7));
}

static int ir_opt_callee_is_body_elidable(TCCIRState *ir, Sym *callee)
{
  if (!callee)
    return 0;

  const char *name = get_tok_str(callee->v, NULL);
  if (name && tcc_ir_is_pure_aeabi(name))
    return 1;

  /* Flag-setting soft-float compares (__aeabi_cfcmple / __aeabi_cdcmple) and
   * float negation helpers (__aeabi_fneg / __aeabi_dneg) have no observable
   * side effects beyond their result (CPSR flags or return value), so they
   * are elidable when the surrounding body is otherwise side-effect-free. */
  if (name && ir_opt_is_flag_cmp_helper_name(name))
    return 1;
  if (name && (strcmp(name, "__aeabi_fneg") == 0 || strcmp(name, "__aeabi_dneg") == 0))
    return 1;

  return tcc_ir_get_func_purity(ir, callee) >= TCC_FUNC_PURITY_PURE;
}

static int ir_opt_param_vreg_is_volatile(int param_pos)
{
  int32_t param_vreg = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, param_pos);
  if (tcc_state && tcc_state->ir && tcc_ir_vreg_is_valid(tcc_state->ir, param_vreg))
  {
    IRLiveInterval *iv = tcc_ir_vreg_live_interval(tcc_state->ir, param_vreg);
    if (iv)
      return iv->is_volatile != 0;
  }

  for (Sym *sym = local_stack; sym; sym = sym->prev)
  {
    if (sym->vreg == param_vreg)
      return (sym->type.t & VT_VOLATILE) != 0;
  }

  if (!tcc_state || !tcc_state->cur_func_sym || !tcc_state->cur_func_sym->type.ref)
    return 1;

  Sym *param = tcc_state->cur_func_sym->type.ref->next;
  for (int i = 0; param && i < param_pos; i++)
    param = param->next;

  if (!param)
    return 1;
  return (param->type.t & VT_VOLATILE) != 0;
}

static int ir_opt_vreg_sym_is_volatile(int32_t vr)
{
  if (tcc_state && tcc_state->ir && tcc_ir_vreg_is_valid(tcc_state->ir, vr))
  {
    IRLiveInterval *iv = tcc_ir_vreg_live_interval(tcc_state->ir, vr);
    if (iv)
      return iv->is_volatile != 0;
  }

  for (Sym *sym = local_stack; sym; sym = sym->prev)
  {
    if (sym->vreg == vr)
      return (sym->type.t & VT_VOLATILE) != 0;
  }
  return 0;
}

static int ir_opt_direct_auto_vreg_store_is_local(IROperand op)
{
  int32_t vr;
  int vt;

  if (op.is_lval || op.is_sym || op.is_llocal)
    return 0;

  vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;

  vt = TCCIR_DECODE_VREG_TYPE(vr);
  if (vt == TCCIR_VREG_TYPE_VAR)
    return !ir_opt_vreg_sym_is_volatile(vr);
  if (vt == TCCIR_VREG_TYPE_PARAM)
    return !ir_opt_param_vreg_is_volatile(TCCIR_DECODE_VREG_POSITION(vr));
  return 0;
}

static int ir_opt_op_is_essential(TCCIRState *ir, IRQuadCompact *q, int idx,
                                  const uint8_t *pure_call_ids, int pure_call_id_bytes)
{
  switch (q->op)
  {
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
  {
    /* Backward / self-targeting jumps form loops whose non-termination is
     * itself observable.  uninit_local_ub deliberately collapses functions
     * that read uninit locals to a single JUMP-to-self ("b ."); forward
     * jumps just route over NOPs and can be safely dropped. */
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int target = (int)dest.u.imm32;
    if (target >= 0 && target <= idx)
      return 1;
    return 0;
  }
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_TRAP:
  case TCCIR_OP_IJUMP:
  case TCCIR_OP_INLINE_ASM:
  case TCCIR_OP_ASM_INPUT:
  case TCCIR_OP_ASM_OUTPUT:
  case TCCIR_OP_CALLSEQ_BEGIN:
  case TCCIR_OP_CALLARG_REG:
  case TCCIR_OP_CALLARG_STACK:
  case TCCIR_OP_CALLSEQ_END:
  case TCCIR_OP_INIT_CHAIN_SLOT:
  case TCCIR_OP_PREFETCH:
  case TCCIR_OP_SETJMP:
  case TCCIR_OP_LONGJMP:
  case TCCIR_OP_NL_SETJMP:
  case TCCIR_OP_NL_LONGJMP:
  case TCCIR_OP_BUILTIN_APPLY_ARGS:
  case TCCIR_OP_BUILTIN_APPLY:
  case TCCIR_OP_BUILTIN_RETURN:
  case TCCIR_OP_VLA_ALLOC:
  case TCCIR_OP_VLA_SP_SAVE:
  case TCCIR_OP_VLA_SP_RESTORE:
  case TCCIR_OP_BLOCK_COPY:
  case TCCIR_OP_SWITCH_TABLE:
  case TCCIR_OP_SWITCH_LOAD:
    return 1;
  case TCCIR_OP_STORE:
  {
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    int dest_vt = TCCIR_DECODE_VREG_TYPE(dest_vr);
    /* A direct write to a non-volatile parameter is not observable once the
     * whole body is otherwise side-effect-free.  Pointer writes, volatile
     * parameter writes, and parent-frame writes remain essential. */
    if (tcc_state && tcc_state->ir_late_reopt_phase &&
        !dest.is_lval && !dest.is_sym && !dest.is_llocal &&
        dest_vt == TCCIR_VREG_TYPE_PARAM &&
        !ir_opt_param_vreg_is_volatile(TCCIR_DECODE_VREG_POSITION(dest_vr)))
      return 0;
    return 1;
  }
  case TCCIR_OP_STORE_INDEXED:
  case TCCIR_OP_STORE_POSTINC:
    return 1;
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCCALLVOID:
  {
    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    return !ir_opt_callee_is_body_elidable(ir, callee);
  }
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_FUNCPARAMVOID:
  {
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    int call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, src2));
    return !ir_opt_pure_call_id_test(pure_call_ids, pure_call_id_bytes, call_id);
  }
  default:
    break;
  }

  /* Volatile sym read on any source: keep the function alive. */
  if (irop_config[q->op].has_src1)
  {
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (s.is_sym)
    {
      Sym *sym = irop_get_sym_ex(ir, s);
      if (sym && (sym->type.t & VT_VOLATILE))
        return 1;
    }
  }
  if (irop_config[q->op].has_src2)
  {
    IROperand s = tcc_ir_op_get_src2(ir, q);
    if (s.is_sym)
    {
      Sym *sym = irop_get_sym_ex(ir, s);
      if (sym && (sym->type.t & VT_VOLATILE))
        return 1;
    }
  }
  return 0;
}

static int ir_opt_vreg_has_def_in_range(TCCIRState *ir, int32_t vreg, int start, int end)
{
  if (vreg < 0)
    return 0;
  for (int i = start; i <= end; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!dest.is_lval && irop_get_vreg(dest) == vreg)
      return 1;
  }
  return 0;
}

static int ir_opt_vreg_has_iv_update_in_range(TCCIRState *ir, int32_t vreg, int start, int end, int depth)
{
  if (vreg < 0 || depth > 2)
    return 0;

  for (int i = start; i <= end; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (dest.is_lval || irop_get_vreg(dest) != vreg)
      continue;

    if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) &&
        irop_config[q->op].has_src1 && irop_config[q->op].has_src2 &&
        irop_is_immediate(tcc_ir_op_get_src2(ir, q)))
    {
      int32_t s1 = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
      if (ir_opt_vreg_has_def_in_range(ir, s1, start, end))
        return 1;
    }

    if (q->op == TCCIR_OP_ASSIGN && irop_config[q->op].has_src1)
    {
      int32_t src = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
      if (ir_opt_vreg_has_iv_update_in_range(ir, src, start, end, depth + 1))
        return 1;
    }
  }

  return 0;
}

static int ir_opt_jumpif_uses_iv_update(TCCIRState *ir, int jif_idx, int start, int end)
{
  int scan_floor = start < jif_idx ? start : 0;
  for (int i = jif_idx - 1; i >= scan_floor; i--)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      break;
    if (q->op != TCCIR_OP_CMP && q->op != TCCIR_OP_TEST_ZERO)
      continue;

    if (irop_config[q->op].has_src1)
    {
      int32_t s1 = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
      if (ir_opt_vreg_has_iv_update_in_range(ir, s1, start, end, 0))
        return 1;
    }
    if (irop_config[q->op].has_src2)
    {
      int32_t s2 = irop_get_vreg(tcc_ir_op_get_src2(ir, q));
      if (ir_opt_vreg_has_iv_update_in_range(ir, s2, start, end, 0))
        return 1;
    }
    return 0;
  }

  return 0;
}

static int ir_opt_range_has_iv_update(TCCIRState *ir, int start, int end)
{
  for (int i = start; i <= end; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    if (q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB)
      continue;
    if (!irop_config[q->op].has_src2 || !irop_is_immediate(tcc_ir_op_get_src2(ir, q)))
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (dest.is_lval)
      continue;

    int32_t dest_vr = irop_get_vreg(dest);
    int dest_vt = TCCIR_DECODE_VREG_TYPE(dest_vr);
    if (dest_vt != TCCIR_VREG_TYPE_TEMP && dest_vt != TCCIR_VREG_TYPE_VAR)
      continue;

    int32_t src_vr = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
    if (ir_opt_vreg_has_def_in_range(ir, src_vr, start, end))
      return 1;
  }
  return 0;
}

static int ir_opt_successor_enters_range(TCCIRState *ir, int succ, int start, int end)
{
  int n = ir->next_instruction_index;
  if (succ >= start && succ <= end)
    return 1;
  while (succ >= 0 && succ < n && ir->compact_instructions[succ].op == TCCIR_OP_NOP)
    succ++;
  if (succ >= start && succ <= end)
    return 1;
  if (succ >= 0 && succ < n && ir->compact_instructions[succ].op == TCCIR_OP_JUMP)
  {
    IROperand dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[succ]);
    int target = (int)irop_get_imm64_ex(ir, dest);
    return target >= start && target <= end;
  }
  return 0;
}

static int ir_opt_backward_jump_has_cond_exit(TCCIRState *ir, int idx)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  int target = (int)irop_get_imm64_ex(ir, dest);

  if (target < 0 || target > idx)
    return 0;

  /* A conditional back-edge driven by an in-loop IV update is a finite
   * side-effect-free loop for our late whole-body elision purposes.
   * Unconditional infinite loops (`for (;;)` lowered to a bare self/back
   * jump), and loops whose only exit depends on an unchanged parameter/load,
   * remain essential. */
  if (q->op == TCCIR_OP_JUMPIF)
  {
    if (ir_opt_jumpif_uses_iv_update(ir, idx, target, idx))
      return 1;
    /* Secondary conditional back-edges can be driven by pure work inside
     * an otherwise finite IV loop.  Once the whole function is proven
     * non-observable, those edges should not keep the body alive. */
    return ir_opt_range_has_iv_update(ir, target, idx);
  }

  if (q->op != TCCIR_OP_JUMP)
    return 0;

  for (int i = 0; i <= idx; i++)
  {
    IRQuadCompact *iq = &ir->compact_instructions[i];
    if (iq->op != TCCIR_OP_JUMPIF)
      continue;

    IROperand idest = tcc_ir_op_get_dest(ir, iq);
    int itarget = (int)irop_get_imm64_ex(ir, idest);
    int target_enters = ir_opt_successor_enters_range(ir, itarget, target, idx);
    int fallthrough_enters = ir_opt_successor_enters_range(ir, i + 1, target, idx);
    if (!target_enters && fallthrough_enters &&
        ir_opt_jumpif_uses_iv_update(ir, i, target, idx))
      return 1;
    if (target_enters && !fallthrough_enters &&
        ir_opt_jumpif_uses_iv_update(ir, i, target, idx))
      return 1;
  }

  return 0;
}

int tcc_ir_opt_useless_function_body(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  uint8_t *pure_call_ids = NULL;
  int pure_call_id_bytes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
      continue;

    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!ir_opt_callee_is_body_elidable(ir, callee))
      continue;

    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    int call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, src2));
    ir_opt_pure_call_id_mark(&pure_call_ids, &pure_call_id_bytes, call_id);
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (ir_opt_op_is_essential(ir, q, i, pure_call_ids, pure_call_id_bytes))
    {
      if ((q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) &&
          ir_opt_backward_jump_has_cond_exit(ir, i))
        continue;
      if (pure_call_ids)
        tcc_free(pure_call_ids);
      return 0;
    }
  }

  if (pure_call_ids)
    tcc_free(pure_call_ids);

  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    if (ir->compact_instructions[i].op != TCCIR_OP_NOP)
    {
      ir->compact_instructions[i].op = TCCIR_OP_NOP;
      changes++;
    }
  }

  /* Regalloc has already run by the time this pass fires, so the dirty
   * register bitmap reflects the pre-NOP IR.  With every op now NOP, no
   * register is actually live — clearing the bitmap (and per-instruction
   * live-reg map, if any) lets the prologue skip the otherwise-spurious
   * `push/pop {rN}` for a "saved" register the body never touches.  Also
   * mark the function as a leaf so save_lr drops away. */
  ir->ls.dirty_registers = 0;
  ir->ls.dirty_float_registers = 0;
  if (ir->ls.live_regs_by_instruction && ir->ls.live_regs_by_instruction_size > 0)
    memset(ir->ls.live_regs_by_instruction, 0,
           ir->ls.live_regs_by_instruction_size * sizeof(ir->ls.live_regs_by_instruction[0]));
  ir->leaffunc = 1;
  for (int p = 0; p < ir->next_parameter; p++)
  {
    IRLiveInterval *iv = &ir->parameters_live_intervals[p];
    iv->allocation.r0 = PREG_NONE;
    iv->allocation.r1 = PREG_NONE;
    iv->allocation.offset = 0;
  }
  /* The body had every essential-op already NOPed away (e.g. dead_vla_struct
   * removed the VLA dance for a never-read local).  Drop the frame-pointer
   * forcing so the prologue collapses to a single `bx lr` rather than the
   * VLA-era push/setup/sub/teardown. */
  tcc_state->need_frame_pointer = 0;
  tcc_state->force_frame_pointer = 0;

  LOG_IR_GEN("USELESS-BODY: NOPed %d instructions (no observable side effects)", changes);
  /* Always return 1 once we've proved the body is observationally empty —
   * even when an earlier pass already NOPed everything (changes == 0), the
   * caller still needs to reset `loc` so the prologue doesn't allocate
   * frame space for now-dead locals. */
  return changes > 0 ? changes : 1;
}

int tcc_ir_opt_useless_function_body_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_useless_function_body(ctx->ir);
}

/* No-Return Function Collapse
 *
 * If a function provably never returns (no RETURNVALUE/RETURNVOID anywhere)
 * and has no other observable-from-outside effects (no FUNCCALL whose callee
 * could publish state, no inline asm, no volatile sym access, no setjmp/
 * longjmp/trap/VLA primitives, no computed goto), then no caller can observe
 * any of its writes — the function's effect is indistinguishable from
 * `b .`.  Collapse the whole body to a single self-jump, matching GCC's
 * -O2 behavior on patterns like gcc.c-torture/compile/pr70916.c where every
 * path bottoms out in an infinite loop.
 *
 * Pre-condition: useless_function_body left this body alone because it
 * contains STOREs (or other essential-but-elidable ops); we accept those
 * here because the function's non-return makes them unobservable.
 */
int tcc_ir_opt_noreturn_collapse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  /* O2-only: aggressive elision of observable-but-unreachable side effects
   * (caller can't see them because we never return).  Matches the gating
   * philosophy of uninit_local_ub. */
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;

  int has_jump = 0;
  int has_store = 0; /* observable work — gate for publishing func_noreturn */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_STORE_POSTINC || q->op == TCCIR_OP_BLOCK_COPY)
      has_store = 1;

    switch (q->op)
    {
    /* Any return op means the function CAN return; collapse would change
     * semantics by skipping the side effects on the returning path. */
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    /* Calls publish state we can't elide: the callee may write through
     * pointers we passed, run signal handlers, etc. */
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_CALLSEQ_BEGIN:
    case TCCIR_OP_CALLARG_REG:
    case TCCIR_OP_CALLARG_STACK:
    case TCCIR_OP_CALLSEQ_END:
    /* Inline asm could do anything (including exit / sync). */
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    /* Non-local control flow can return to the caller through a longjmp. */
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_RETURN:
    /* Trap may dispatch to a fault/signal handler that observes state. */
    case TCCIR_OP_TRAP:
    /* VLA / SP juggling: stack-pointer side effects the prologue tracks. */
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
    /* Computed goto: unknown-at-compile-time target — defensive bail. */
    case TCCIR_OP_IJUMP:
      return 0;
    case TCCIR_OP_JUMP:
    case TCCIR_OP_JUMPIF:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_SWITCH_LOAD:
      has_jump = 1;
      break;
    default:
      break;
    }

    /* Volatile sym read/write on any operand: keep the function alive — the
     * write is observable through the volatile memory model regardless of
     * whether we return. */
    for (int k = 0; k <= 2; k++)
    {
      IROperand op;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        if (!irop_config[q->op].has_src2)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }
      if (op.is_sym)
      {
        Sym *sym = irop_get_sym_ex(ir, op);
        if (sym && (sym->type.t & VT_VOLATILE))
          return 0;
      }
    }
  }

  /* Need at least one JUMP — otherwise the function would fall off the end,
   * which is an implicit return. */
  if (!has_jump)
    return 0;

  /* Implicit-return detection: TCC does not always emit an explicit
   * RETURNVOID for void functions — control simply falls off the end of the
   * IR, and the backend emits a `bx lr` after the last op.  If the last
   * non-NOP instruction can fall through (anything other than an
   * unconditional JUMP), the function still returns and we must not
   * collapse.  SWITCH_TABLE / IJUMP could in principle exit cleanly, but
   * have unknown-at-this-pass targets; we already bailed on IJUMP above,
   * and we conservatively also refuse to collapse when the last op is
   * SWITCH_TABLE. */
  int last_idx = -1;
  for (int i = n - 1; i >= 0; i--)
  {
    if (ir->compact_instructions[i].op != TCCIR_OP_NOP)
    {
      last_idx = i;
      break;
    }
  }
  if (last_idx < 0)
    return 0;
  if (ir->compact_instructions[last_idx].op != TCCIR_OP_JUMP)
    return 0;

  /* If the final JUMP targets past-end (n) or any NOP at/after last_idx, the
   * function actually returns implicitly — the backend will emit `bx lr` at
   * the epilogue.  Only collapse when the JUMP demonstrably loops back to a
   * live earlier instruction. */
  {
    IROperand jdest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[last_idx]);
    int jt = (int)irop_get_imm64_ex(ir, jdest);
    if (jt < 0 || jt >= n || jt > last_idx)
      return 0;
    /* Target could also land on a NOP that compaction would push past the end. */
    int t = jt;
    while (t < n && ir->compact_instructions[t].op == TCCIR_OP_NOP)
      t++;
    if (t >= n || t > last_idx)
      return 0;
  }

  /* The last op looping back is not enough: an *earlier* conditional branch
   * can still exit the loop to the epilogue.  Scan every JUMP/JUMPIF — if any
   * targets past the last live instruction (i.e. the implicit `bx lr`
   * epilogue), the function has a reachable return path and must not be
   * collapsed.  Without this, a bottom-tested loop like
   *   for (...; --i < ~0u; ) ...   // exit branch jumps to past-end
   * whose body ends in an unconditional back-edge JUMP was wrongly treated as
   * noreturn and replaced with `b .` (miscompile: loop-2d/pr27073 spun
   * forever).  Branches that stay within the body (target <= last_idx) are
   * internal control flow and don't count as exits. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    int jt = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
    if (jt < 0)
      continue;
    int t = jt;
    while (t < n && ir->compact_instructions[t].op == TCCIR_OP_NOP)
      t++;
    if (t >= n || t > last_idx)
      return 0;
  }

  LOG_IR_GEN("NORETURN-COLLAPSE: collapsing function body to infinite loop "
             "(no RETURN, no calls/asm/volatile — side effects unobservable)");

  for (int i = 0; i < n; i++)
  {
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[i].is_jump_target = 0;
  }

  ir->compact_instructions[0].op = TCCIR_OP_JUMP;
  ir->compact_instructions[0].is_jump_target = 1;
  IROperand self = irop_make_imm32(-1, 0, IROP_BTYPE_INT32);
  tcc_ir_set_dest(ir, 0, self);
  tcc_ir_set_src1(ir, 0, IROP_NONE);
  tcc_ir_set_src2(ir, 0, IROP_NONE);

  ir->ls.dirty_registers = 0;
  ir->ls.dirty_float_registers = 0;
  if (ir->ls.live_regs_by_instruction && ir->ls.live_regs_by_instruction_size > 0)
    memset(ir->ls.live_regs_by_instruction, 0,
           ir->ls.live_regs_by_instruction_size * sizeof(ir->ls.live_regs_by_instruction[0]));
  ir->leaffunc = 1;
  /* Suppress the unreachable `bx lr` after the self-jump: control never
   * reaches the epilogue, so emitting it just wastes 2 bytes. */
  ir->noreturn = 1;
  /* Publish func_noreturn to callers ONLY when the body had at least one
   * STORE — that's our heuristic for "this function was doing genuine work
   * in an infinite loop" (e.g. gcc.c-torture pc44485.c::func_21).  Without
   * the gate we also publish for bodies that were effectively no-ops the
   * other opts NOP'd down to a trailing self-JUMP (e.g. string-opt-18's
   * test1, where memcpy(p,p,8) self-copy folds away, leaving nothing).
   * Publishing for those mislabels regular helpers as noreturn and breaks
   * downstream purity/LICM/inlining analyses. */
  if (has_store && tcc_state && tcc_state->cur_func_sym &&
      tcc_state->cur_func_sym->type.ref)
    tcc_state->cur_func_sym->type.ref->f.func_noreturn = 1;

  return 1;
}

int tcc_ir_opt_noreturn_collapse_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_noreturn_collapse(ctx->ir);
}

/* ============================================================================
 * Infinite Loop Body Simplification
 * ============================================================================
 *
 * Detect infinite loops (no exit) whose body has no externally-observable side
 * effects, and collapse them to a tight self-jump.
 *
 * A store is considered dead within an infinite loop when:
 *   - It writes to a local/parameter whose address is not taken, OR
 *   - It writes a loop-invariant constant to a non-volatile global (hoisted)
 *
 * The pass also hoists constant global stores to a preheader position so
 * the store executes once rather than being eliminated entirely.
 */
int tcc_ir_opt_infinite_loop_simplify(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 3)
    return 0;
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;

  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops || loops->num_loops == 0)
  {
    tcc_ir_free_loops(loops);
    return 0;
  }

  int changes = 0;

  for (int li = 0; li < loops->num_loops; li++)
  {
    IRLoop *loop = &loops->loops[li];

    int back_edge_idx = -1;
    int is_infinite = 1;
    int has_call = 0;
    int has_volatile = 0;

    for (int bi = 0; bi < loop->num_body_instrs; bi++)
    {
      int idx = loop->body_instrs[bi];
      IRQuadCompact *q = &ir->compact_instructions[idx];
      if (q->op == TCCIR_OP_NOP)
        continue;

      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID ||
          q->op == TCCIR_OP_CALLSEQ_BEGIN || q->op == TCCIR_OP_INLINE_ASM)
      {
        has_call = 1;
        break;
      }
      if (q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
      {
        is_infinite = 0;
        break;
      }
      if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE)
      {
        is_infinite = 0;
        break;
      }

      /* Check for volatile operands */
      for (int k = 0; k <= 2; k++)
      {
        IROperand op;
        if (k == 0 && irop_config[q->op].has_dest)
          op = tcc_ir_op_get_dest(ir, q);
        else if (k == 1 && irop_config[q->op].has_src1)
          op = tcc_ir_op_get_src1(ir, q);
        else if (k == 2 && irop_config[q->op].has_src2)
          op = tcc_ir_op_get_src2(ir, q);
        else
          continue;
        if (op.is_sym)
        {
          Sym *sym = irop_get_sym_ex(ir, op);
          if (sym && (sym->type.t & VT_VOLATILE))
            has_volatile = 1;
        }
      }

      if (q->op == TCCIR_OP_JUMPIF)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        int target = (int)dest.u.imm32;
        if (target < loop->start_idx || target > loop->end_idx)
        {
          is_infinite = 0;
          break;
        }
      }
      if (q->op == TCCIR_OP_JUMP)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        int target = (int)dest.u.imm32;
        if (target == loop->header_idx)
          back_edge_idx = idx;
        else if (target < loop->start_idx || target > loop->end_idx)
        {
          is_infinite = 0;
          break;
        }
      }
    }

    if (!is_infinite || has_call || has_volatile || back_edge_idx < 0)
      continue;

    /* Analyze stores in the loop body. Check if all are dead or hoistable. */
    int all_stores_dead = 1;

    /* Track which globals get constant stores (for hoisting) */
#define MAX_HOIST 8
    struct { Sym *sym; int64_t addend; IROperand value; int store_idx; } hoist[MAX_HOIST];
    int nhoist = 0;

    for (int bi = 0; bi < loop->num_body_instrs; bi++)
    {
      int idx = loop->body_instrs[bi];
      IRQuadCompact *q = &ir->compact_instructions[idx];

      if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED &&
          q->op != TCCIR_OP_STORE_POSTINC)
        continue;

      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dest_vr = irop_get_vreg(dest);

      /* Store to a local or parameter (direct vreg store, not pointer deref) */
      if (q->op == TCCIR_OP_STORE && dest_vr >= 0 && !dest.is_lval && !dest.is_sym)
      {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, dest_vr);
        if (interval && interval->addrtaken)
          {
            /* Address taken — check if the LEA is within the loop
             * (reachable) or outside (unreachable from infinite loop). */
            int lea_in_loop = 0;
            for (int j = 0; j < n; j++)
            {
              IRQuadCompact *lq = &ir->compact_instructions[j];
              if (lq->op == TCCIR_OP_LEA || lq->op == TCCIR_OP_ASSIGN)
              {
                if (irop_config[lq->op].has_src1)
                {
                  IROperand s1 = tcc_ir_op_get_src1(ir, lq);
                  if (!s1.is_lval && irop_get_vreg(s1) == dest_vr)
                  {
                    /* Check if this LEA is in the loop body */
                    for (int bk = 0; bk < loop->num_body_instrs; bk++)
                    {
                      if (loop->body_instrs[bk] == j)
                      {
                        lea_in_loop = 1;
                        break;
                      }
                    }
                  }
                }
              }
            }
            if (lea_in_loop)
            {
              all_stores_dead = 0;
              break;
            }
          }
        continue;
      }

      if (q->op == TCCIR_OP_STORE && dest.is_sym && dest.is_lval)
      {
        /* Store to global. Check if value is loop-invariant (constant). */
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        IRPoolSymref *sr = irop_get_symref_ex(ir, dest);
        if (!sr || !sr->sym)
        {
          all_stores_dead = 0;
          break;
        }
        if (sr->sym->type.t & VT_VOLATILE)
        {
          all_stores_dead = 0;
          break;
        }
        if (irop_is_immediate(src1) && !src1.is_sym)
        {
          /* Constant store to non-volatile global → hoistable */
          if (nhoist < MAX_HOIST)
          {
            hoist[nhoist].sym = sr->sym;
            hoist[nhoist].addend = sr->addend;
            hoist[nhoist].value = src1;
            hoist[nhoist].store_idx = idx;
            nhoist++;
          }
          continue;
        }
        /* Non-constant store: check if it's a signed int read-modify-write
         * (++m pattern) where eventual overflow is UB → body can be removed. */
        int32_t val_vr = irop_get_vreg(src1);
        int is_signed_rmw = 0;
        int dbtype = irop_get_btype(dest);
        if (val_vr >= 0 && !src1.is_lval && !src1.is_sym &&
            (dbtype == IROP_BTYPE_INT32 || dbtype == IROP_BTYPE_INT16 ||
             dbtype == IROP_BTYPE_INT8) &&
            !dest.is_unsigned)
        {
          for (int bj = 0; bj < loop->num_body_instrs; bj++)
          {
            int didx = loop->body_instrs[bj];
            IRQuadCompact *dq = &ir->compact_instructions[didx];
            if (dq->op != TCCIR_OP_ADD && dq->op != TCCIR_OP_SUB)
              continue;
            if (!irop_config[dq->op].has_dest)
              continue;
            IROperand dd = tcc_ir_op_get_dest(ir, dq);
            if (irop_get_vreg(dd) != val_vr)
              continue;
            IROperand ds1 = tcc_ir_op_get_src1(ir, dq);
            IROperand ds2 = tcc_ir_op_get_src2(ir, dq);
            if (ds1.is_sym && ds1.is_lval && irop_is_immediate(ds2) && !ds2.is_sym)
            {
              IRPoolSymref *dsr = irop_get_symref_ex(ir, ds1);
              if (dsr && dsr->sym == sr->sym && dsr->addend == sr->addend)
                is_signed_rmw = 1;
            }
            break;
          }
        }
        if (!is_signed_rmw)
        {
          all_stores_dead = 0;
          break;
        }
        continue;
      }

      /* STORE_INDEXED, STORE_POSTINC, or unknown STORE pattern */
      all_stores_dead = 0;
      break;
    }

    if (!all_stores_dead)
      continue;

    /* All stores are dead or hoistable. Simplify the loop. */

    /* Step 1: Convert hoisted constant stores to execute before the loop.
     * We rewrite the store instructions in-place: move them to just before
     * the loop header, and NOP the originals. */
    for (int h = 0; h < nhoist; h++)
    {
      /* Find the preheader position: the instruction just before the loop
       * header.  If the loop has a preheader_idx, use it. Otherwise,
       * we can't safely hoist (would need to insert instructions). */
      int preheader = loop->preheader_idx;
      if (preheader < 0)
      {
        /* Try to find a NOP slot before the header */
        for (int j = loop->header_idx - 1; j >= 0; j--)
        {
          if (ir->compact_instructions[j].op == TCCIR_OP_NOP)
          {
            preheader = j;
            break;
          }
          break;
        }
      }
      if (preheader >= 0 && ir->compact_instructions[preheader].op == TCCIR_OP_NOP)
      {
        /* Copy the store to the preheader slot */
        ir->compact_instructions[preheader] = ir->compact_instructions[hoist[h].store_idx];
        ir->compact_instructions[preheader].is_jump_target =
          ir->compact_instructions[hoist[h].store_idx].is_jump_target ? 1 : 0;
        /* Copy operands */
        int src_base = ir->compact_instructions[hoist[h].store_idx].operand_base;
        int dst_base = ir->compact_instructions[preheader].operand_base;
        int nops_count = (irop_config[TCCIR_OP_STORE].has_dest ? 1 : 0) +
                         (irop_config[TCCIR_OP_STORE].has_src1 ? 1 : 0) +
                         (irop_config[TCCIR_OP_STORE].has_src2 ? 1 : 0);
        for (int k = 0; k < nops_count; k++)
          ir->iroperand_pool[dst_base + k] = ir->iroperand_pool[src_base + k];
      }
      /* NOP the original store */
      ir->compact_instructions[hoist[h].store_idx].op = TCCIR_OP_NOP;
    }

    /* Step 2: NOP all remaining non-NOP instructions in the loop body
     * except the back-edge jump. Then convert the back-edge to a
     * self-jump at the header. */
    for (int bi = 0; bi < loop->num_body_instrs; bi++)
    {
      int idx = loop->body_instrs[bi];
      IRQuadCompact *q = &ir->compact_instructions[idx];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (idx == back_edge_idx)
        continue;
      q->op = TCCIR_OP_NOP;
    }

    /* Convert the back-edge to a self-jump at the loop header */
    ir->compact_instructions[loop->header_idx].op = TCCIR_OP_JUMP;
    ir->compact_instructions[loop->header_idx].is_jump_target = 1;
    IROperand self = irop_make_imm32(-1, loop->header_idx, IROP_BTYPE_INT32);
    tcc_ir_set_dest(ir, loop->header_idx, self);
    tcc_ir_set_src1(ir, loop->header_idx, IROP_NONE);
    tcc_ir_set_src2(ir, loop->header_idx, IROP_NONE);

    /* NOP the old back-edge if it's not the header */
    if (back_edge_idx != loop->header_idx)
      ir->compact_instructions[back_edge_idx].op = TCCIR_OP_NOP;

    changes++;
#undef MAX_HOIST
  }

  tcc_ir_free_loops(loops);
  return changes;
}

int tcc_ir_opt_infinite_loop_simplify_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_infinite_loop_simplify(ctx->ir);
}

/* ============================================================================
 * Dead-Code-Before-Infinite-Loop Elimination
 * ============================================================================
 *
 * After infinite_loop_simplify collapses a side-effect-free infinite loop to a
 * tight self-jump (`JMP to self`), the code that *precedes* the loop on the
 * never-returning path is still emitted: stores to globals, the address-take
 * that feeds them, the dominating `if` tests, etc.  GCC removes all of it.
 *
 * pr106433.c::bar is the motivating case:
 *
 *     if (x) {
 *       if (m < 1) for (m = 0; m < 1; ++m) ++x;
 *       p = &x;
 *       for (;;) ++m;          // never returns
 *     }
 *     return 0;
 *
 * Once control enters the non-terminating, side-effect-free `for(;;)`, the
 * function never resumes its caller, so the writes to m and p (and the
 * address-take of x that forces a stack spill) can never be observed.
 *
 * An instruction is dead under this rule when, following the CFG, it cannot
 * reach any observable program effect — a RETURN, a (non-pure) call, a
 * volatile access, inline asm, a trap, a longjmp, etc.  Its only destiny is to
 * spin forever in an empty loop.  A plain store to non-volatile memory is NOT
 * such an effect: it is observable only if the function eventually returns so
 * the value can be read, which on these paths never happens.
 *
 * We keep the self-jump sink itself (the program must still hang) and redirect
 * every edge entering the dead region straight to the loop, NOPing the rest.
 * DCE / jump-threading downstream cleans up the redirected hops.  Removing the
 * address-take of a parameter/local also lets us clear its now-stale
 * `addrtaken` flag, dropping the spill it would otherwise force. */

/* An op is an "anchor": observable even if the function never returns.
 * Differs from ir_opt_op_is_essential only in that a store to non-volatile
 * memory is NOT an anchor (it needs a return to be observed) and plain
 * control flow (JUMP/JUMPIF) is not an anchor. */
static int ir_opt_op_is_inf_dead_anchor(TCCIRState *ir, IRQuadCompact *q, int idx)
{
  switch (q->op)
  {
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
    return 0; /* pure control flow */
  case TCCIR_OP_STORE:
  {
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    /* Store to a global (sym lval): observable-without-return only if volatile. */
    if (dest.is_sym && dest.is_lval)
    {
      Sym *s = irop_get_sym_ex(ir, dest);
      return (s && (s->type.t & VT_VOLATILE)) ? 1 : 0;
    }
    /* Direct store to a local/param vreg: dead unless volatile. */
    if (!dest.is_lval && !dest.is_sym && !dest.is_llocal)
    {
      int32_t dvr = irop_get_vreg(dest);
      int dvt = TCCIR_DECODE_VREG_TYPE(dvr);
      if (dvt == TCCIR_VREG_TYPE_VAR)
        return ir_opt_vreg_sym_is_volatile(dvr);
      if (dvt == TCCIR_VREG_TYPE_PARAM)
        return ir_opt_param_vreg_is_volatile(TCCIR_DECODE_VREG_POSITION(dvr));
    }
    /* Pointer / unknown store target: keep conservatively. */
    return 1;
  }
  default:
    break;
  }
  /* Everything else (calls, asm, traps, returns, volatile reads, VLA, …) keeps
   * its ir_opt_op_is_essential classification. */
  return ir_opt_op_is_essential(ir, q, idx, NULL, 0);
}

/* Forward-walk from `start` over CFG successors, staying within the dead/sink
 * set, until an empty-infinite-loop sink is reached.  Returns its index, or -1
 * if no sink is reachable (the caller then leaves the region untouched). */
static int ir_inf_dead_find_sink(TCCIRState *ir, int start, const uint8_t *dead,
                                 const uint8_t *is_sink, int n)
{
  uint8_t *vis = tcc_mallocz((n + 7) / 8);
  int *stk = tcc_malloc(n * sizeof(int));
  int sp = 0, found = -1;
  stk[sp++] = start;
  vis[start / 8] |= (1 << (start % 8));
  while (sp > 0)
  {
    int i = stk[--sp];
    if (is_sink[i / 8] & (1 << (i % 8)))
    {
      found = i;
      break;
    }
    IRQuadCompact *q = &ir->compact_instructions[i];
    int succ[2], ns = 0;
    switch (q->op)
    {
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_TRAP:
      break;
    case TCCIR_OP_JUMP:
      succ[ns++] = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
      break;
    case TCCIR_OP_JUMPIF:
      succ[ns++] = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
      succ[ns++] = i + 1;
      break;
    default:
      succ[ns++] = i + 1;
      break;
    }
    for (int k = 0; k < ns; k++)
    {
      int s = succ[k];
      if (s < 0 || s >= n)
        continue;
      int sdead = dead[s / 8] & (1 << (s % 8));
      int ssink = is_sink[s / 8] & (1 << (s % 8));
      if (!sdead && !ssink)
        continue; /* escapes the dead region — cannot happen for a dead node */
      if (!(vis[s / 8] & (1 << (s % 8))))
      {
        vis[s / 8] |= (1 << (s % 8));
        stk[sp++] = s;
      }
    }
  }
  tcc_free(vis);
  tcc_free(stk);
  return found;
}

int tcc_ir_opt_dead_before_infinite_loop(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 2)
    return 0;
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;

  /* Indirect jumps have statically-unknown successors — bail. */
  for (int i = 0; i < n; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return 0;

#define GETBIT(arr, k) ((arr)[(k) / 8] & (1 << ((k) % 8)))
#define SETBIT(arr, k) ((arr)[(k) / 8] |= (1 << ((k) % 8)))

  /* Empty infinite-loop sinks: a JUMP whose target is itself. */
  uint8_t *is_sink = tcc_mallocz((n + 7) / 8);
  int have_sink = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP)
      continue;
    if ((int)tcc_ir_op_get_dest(ir, q).u.imm32 == i)
    {
      SETBIT(is_sink, i);
      have_sink = 1;
    }
  }
  if (!have_sink)
  {
    tcc_free(is_sink);
    return 0;
  }

  /* anchor[i]: instruction has an effect observable without returning. */
  uint8_t *anchor = tcc_mallocz((n + 7) / 8);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (ir_opt_op_is_inf_dead_anchor(ir, q, i))
      SETBIT(anchor, i);
  }

  /* can_reach[i]: from i, control can reach an anchor (backward fixpoint). */
  uint8_t *can_reach = tcc_mallocz((n + 7) / 8);
  int changed = 1;
  while (changed)
  {
    changed = 0;
    for (int i = n - 1; i >= 0; i--)
    {
      if (GETBIT(can_reach, i))
        continue;
      IRQuadCompact *q = &ir->compact_instructions[i];
      int reach = GETBIT(anchor, i) ? 1 : 0;
      if (!reach)
      {
        switch (q->op)
        {
        case TCCIR_OP_RETURNVALUE:
        case TCCIR_OP_RETURNVOID:
        case TCCIR_OP_TRAP:
        case TCCIR_OP_SWITCH_TABLE:
          break; /* anchors / no fall-through */
        case TCCIR_OP_JUMP:
        {
          int t = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
          if (t >= 0 && t < n && GETBIT(can_reach, t))
            reach = 1;
          break;
        }
        case TCCIR_OP_JUMPIF:
        {
          int t = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
          if (t >= 0 && t < n && GETBIT(can_reach, t))
            reach = 1;
          if (i + 1 < n && GETBIT(can_reach, i + 1))
            reach = 1;
          break;
        }
        default:
          if (i + 1 < n && GETBIT(can_reach, i + 1))
            reach = 1;
          break;
        }
      }
      if (reach)
      {
        SETBIT(can_reach, i);
        changed = 1;
      }
    }
  }

  /* reach[i]: executed on some path from entry (forward BFS). */
  uint8_t *reach = tcc_mallocz((n + 7) / 8);
  int *wl = tcc_malloc(n * sizeof(int));
  int wh = 0, wt = 0;
  SETBIT(reach, 0);
  wl[wt++] = 0;
  while (wh < wt)
  {
    int i = wl[wh++];
    IRQuadCompact *q = &ir->compact_instructions[i];
#define PUSH(k)                                                                                                        \
  do                                                                                                                   \
  {                                                                                                                    \
    int _k = (k);                                                                                                      \
    if (_k >= 0 && _k < n && !GETBIT(reach, _k))                                                                       \
    {                                                                                                                  \
      SETBIT(reach, _k);                                                                                               \
      wl[wt++] = _k;                                                                                                   \
    }                                                                                                                  \
  } while (0)
    switch (q->op)
    {
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_TRAP:
      break;
    case TCCIR_OP_JUMP:
      PUSH((int)tcc_ir_op_get_dest(ir, q).u.imm32);
      break;
    case TCCIR_OP_JUMPIF:
      PUSH((int)tcc_ir_op_get_dest(ir, q).u.imm32);
      PUSH(i + 1);
      break;
    case TCCIR_OP_SWITCH_TABLE:
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int table_id = (int)irop_get_imm64_ex(ir, src2);
      if (table_id >= 0 && table_id < ir->num_switch_tables)
      {
        TCCIRSwitchTable *table = &ir->switch_tables[table_id];
        for (int j = 0; j < table->num_entries; j++)
          PUSH(table->targets[j]);
        PUSH(table->default_target);
      }
      break;
    }
    default:
      PUSH(i + 1);
      break;
    }
#undef PUSH
  }

  /* dead[i]: reachable, cannot reach an anchor, and not itself a sink. */
  uint8_t *dead = tcc_mallocz((n + 7) / 8);
  int any_dead = 0;
  for (int i = 0; i < n; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
      continue;
    if (GETBIT(is_sink, i))
      continue;
    if (GETBIT(reach, i) && !GETBIT(can_reach, i))
    {
      SETBIT(dead, i);
      any_dead = 1;
    }
  }

  int changes = 0;
  if (!any_dead)
    goto done;

  /* entry[d]: a dead instr reached by an edge from a kept (non-dead, non-NOP)
   * instruction.  Such edges must be rerouted to the loop sink. */
  uint8_t *entry = tcc_mallocz((n + 7) / 8);
  for (int p = 0; p < n; p++)
  {
    if (!GETBIT(reach, p) || GETBIT(dead, p))
      continue;
    IRQuadCompact *q = &ir->compact_instructions[p];
    if (q->op == TCCIR_OP_NOP)
      continue;
#define MARKENTRY(s)                                                                                                   \
  do                                                                                                                   \
  {                                                                                                                    \
    int _s = (s);                                                                                                      \
    if (_s >= 0 && _s < n && GETBIT(dead, _s))                                                                         \
      SETBIT(entry, _s);                                                                                               \
  } while (0)
    switch (q->op)
    {
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_TRAP:
      break;
    case TCCIR_OP_JUMP:
      MARKENTRY((int)tcc_ir_op_get_dest(ir, q).u.imm32);
      break;
    case TCCIR_OP_JUMPIF:
      MARKENTRY((int)tcc_ir_op_get_dest(ir, q).u.imm32);
      MARKENTRY(p + 1);
      break;
    case TCCIR_OP_SWITCH_TABLE:
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int table_id = (int)irop_get_imm64_ex(ir, src2);
      if (table_id >= 0 && table_id < ir->num_switch_tables)
      {
        TCCIRSwitchTable *table = &ir->switch_tables[table_id];
        for (int j = 0; j < table->num_entries; j++)
          MARKENTRY(table->targets[j]);
        MARKENTRY(table->default_target);
      }
      break;
    }
    default:
      MARKENTRY(p + 1);
      break;
    }
#undef MARKENTRY
  }

  /* Resolve a loop sink for every entry up front; if any entry cannot reach a
   * sink (a non-self-jump cycle), abort without touching the IR. */
  int *entry_sink = tcc_malloc(n * sizeof(int));
  int abort_pass = 0;
  for (int d = 0; d < n; d++)
  {
    entry_sink[d] = -1;
    if (!GETBIT(entry, d))
      continue;
    int sink = ir_inf_dead_find_sink(ir, d, dead, is_sink, n);
    if (sink < 0)
    {
      abort_pass = 1;
      break;
    }
    entry_sink[d] = sink;
  }
  if (abort_pass)
  {
    tcc_free(entry);
    tcc_free(entry_sink);
    goto done;
  }

  /* Apply: reroute entries to their sink, NOP the rest of the dead region.
   * Track address-takes we remove so their spill-forcing flag can be cleared. */
  int32_t *cleared_vr = tcc_malloc(n * sizeof(int32_t));
  int ncleared = 0;
  for (int d = 0; d < n; d++)
  {
    if (!GETBIT(dead, d))
      continue;
    IRQuadCompact *q = &ir->compact_instructions[d];
    if (q->op == TCCIR_OP_LEA)
    {
      int32_t lea_src = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
      if (lea_src >= 0)
        cleared_vr[ncleared++] = lea_src;
    }
    if (GETBIT(entry, d))
    {
      q->op = TCCIR_OP_JUMP;
      tcc_ir_set_dest(ir, d, irop_make_imm32(-1, entry_sink[d], IROP_BTYPE_INT32));
      tcc_ir_set_src1(ir, d, IROP_NONE);
      tcc_ir_set_src2(ir, d, IROP_NONE);
    }
    else
    {
      q->op = TCCIR_OP_NOP;
    }
    changes++;
  }

  /* Clear `addrtaken` on any param/local/temp whose last surviving LEA we just
   * removed — drops the now-unnecessary stack spill. */
  for (int c = 0; c < ncleared; c++)
  {
    int32_t vr = cleared_vr[c];
    int still = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_LEA)
        continue;
      if (irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == vr)
      {
        still = 1;
        break;
      }
    }
    if (!still && tcc_ir_vreg_is_valid(ir, vr))
    {
      IRLiveInterval *iv = tcc_ir_get_live_interval(ir, vr);
      if (iv)
        iv->addrtaken = 0;
    }
  }

  LOG_IR_GEN("DEAD-BEFORE-INF-LOOP: rerouted/NOPed %d instructions", changes);

  tcc_free(cleared_vr);
  tcc_free(entry);
  tcc_free(entry_sink);

done:
  tcc_free(is_sink);
  tcc_free(anchor);
  tcc_free(can_reach);
  tcc_free(reach);
  tcc_free(wl);
  tcc_free(dead);
#undef GETBIT
#undef SETBIT
  return changes;
}

int tcc_ir_opt_dead_before_infinite_loop_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_dead_before_infinite_loop(ctx->ir);
}

/* ============================================================================
 * Return-Constant Register Reuse
 * ============================================================================
 *
 * A `RETURNVALUE C` (C an integer immediate) whose block is entered *only*
 * through the equality edge of a `TEST_ZERO V` (C == 0) or `CMP V, #C` returns
 * the very constant the comparison already proved V holds on that edge.
 * Returning V instead of C lets the backend reuse the register V already lives
 * in — typically r0 for a leading parameter or a prior result — and drop the
 * constant materialization entirely.
 *
 * This never increases instruction count: in the worst case (V in some other
 * register or spilled) the reused value costs the same single mov/ldr the
 * constant would have cost; when V is already in the return register it costs
 * nothing.  Matches GCC -O2 on pr106433.c::bar, eliminating the `movs r0, #0`
 * that left us one instruction above GCC (cbnz/bx/b). */
int tcc_ir_opt_return_const_reuse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 2)
    return 0;
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;

  int changes = 0;
  for (int r = 0; r < n; r++)
  {
    IRQuadCompact *R = &ir->compact_instructions[r];
    if (R->op != TCCIR_OP_RETURNVALUE)
      continue;
    IROperand rv = tcc_ir_op_get_src1(ir, R);
    if (rv.is_sym || rv.is_lval || !irop_is_immediate(rv))
      continue;
    int64_t cval = irop_get_imm64_ex(ir, rv);

    /* (1) No fall-through into r: the instruction just before r must be an
     * unconditional diversion, otherwise r has a second (non-equality)
     * predecessor on which V may not equal C. */
    int p = r - 1;
    while (p >= 0 && ir->compact_instructions[p].op == TCCIR_OP_NOP)
      p--;
    if (p >= 0)
    {
      TccIrOp pop = ir->compact_instructions[p].op;
      if (pop != TCCIR_OP_JUMP && pop != TCCIR_OP_RETURNVALUE && pop != TCCIR_OP_RETURNVOID &&
          pop != TCCIR_OP_TRAP && pop != TCCIR_OP_IJUMP && pop != TCCIR_OP_SWITCH_TABLE)
        continue;
    }

    /* (2) Exactly one branch predecessor, an equality JUMPIF targeting r. */
    int jif = -1, npred = 0, bad = 0;
    for (int j = 0; j < n && !bad; j++)
    {
      if (j == r)
        continue;
      IRQuadCompact *q = &ir->compact_instructions[j];
      int targets_r = 0;
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      {
        if ((int)tcc_ir_op_get_dest(ir, q).u.imm32 == r)
          targets_r = 1;
      }
      else if (q->op == TCCIR_OP_SWITCH_TABLE)
      {
        IROperand s2 = tcc_ir_op_get_src2(ir, q);
        int tid = (int)irop_get_imm64_ex(ir, s2);
        if (tid >= 0 && tid < ir->num_switch_tables)
        {
          TCCIRSwitchTable *t = &ir->switch_tables[tid];
          for (int e = 0; e < t->num_entries; e++)
            if (t->targets[e] == r)
              targets_r = 1;
          if (t->default_target == r)
            targets_r = 1;
        }
      }
      if (!targets_r)
        continue;
      npred++;
      if (q->op == TCCIR_OP_JUMPIF && (int)tcc_ir_op_get_src1(ir, q).u.imm32 == TOK_EQ)
        jif = j;
      else
        bad = 1;
    }
    if (bad || npred != 1 || jif < 0)
      continue;

    /* (3) The flag-setter just before the JUMPIF proves V == C, with V a plain
     * register value (no memory / sym deref) of the same width as the return. */
    int t = jif - 1;
    while (t >= 0 && ir->compact_instructions[t].op == TCCIR_OP_NOP)
      t--;
    if (t < 0)
      continue;
    IRQuadCompact *T = &ir->compact_instructions[t];
    IROperand vop = IROP_NONE;
    int matched = 0;
    if (T->op == TCCIR_OP_TEST_ZERO && cval == 0)
    {
      vop = tcc_ir_op_get_src1(ir, T);
      matched = 1;
    }
    else if (T->op == TCCIR_OP_CMP)
    {
      IROperand a = tcc_ir_op_get_src1(ir, T);
      IROperand b = tcc_ir_op_get_src2(ir, T);
      if (!a.is_sym && !a.is_lval && irop_get_vreg(a) >= 0 && irop_is_immediate(b) && !b.is_sym &&
          irop_get_imm64_ex(ir, b) == cval)
      {
        vop = a;
        matched = 1;
      }
      else if (!b.is_sym && !b.is_lval && irop_get_vreg(b) >= 0 && irop_is_immediate(a) && !a.is_sym &&
               irop_get_imm64_ex(ir, a) == cval)
      {
        vop = b;
        matched = 1;
      }
    }
    if (!matched || vop.is_sym || vop.is_lval || irop_get_vreg(vop) < 0)
      continue;
    int vt = TCCIR_DECODE_VREG_TYPE(irop_get_vreg(vop));
    if (vt != TCCIR_VREG_TYPE_PARAM && vt != TCCIR_VREG_TYPE_VAR && vt != TCCIR_VREG_TYPE_TEMP)
      continue;
    if (irop_get_btype(vop) != irop_get_btype(rv))
      continue;

    /* Return the register the comparison proved equals C. */
    tcc_ir_set_src1(ir, r, vop);
    changes++;
    LOG_IR_GEN("RETURN-CONST-REUSE: return #%lld -> reg at instr %d", (long long)cval, r);
  }
  return changes;
}

int tcc_ir_opt_return_const_reuse_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_return_const_reuse(ctx->ir);
}

/* Trap-Only Body Suppression
 *
 * After constprop converts a constant `x / 0` or `x % 0` into TCCIR_OP_TRAP,
 * DCE NOPs out every following op.  The resulting IR has a single TRAP at
 * the top and nothing else.  Without this pass codegen still emits a full
 * prologue (push, frame setup, SUB SP) for the unreachable post-trap world,
 * even though the TRAP never returns.  Suppress the prologue/epilogue by
 * resetting the relevant frame state — caller resets `loc` to drop the
 * stack-size contribution from now-dead locals. */
int tcc_ir_opt_trap_only_body_suppress(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;
  if (ir->naked)
    return 0;

  int trap_idx = -1;
  for (int i = 0; i < n; i++)
  {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_NOP)
      continue;
    if (op == TCCIR_OP_TRAP && trap_idx < 0)
    {
      trap_idx = i;
      continue;
    }
    /* Any other live op (or a second TRAP) — not a pure trap-only body. */
    return 0;
  }
  if (trap_idx < 0)
    return 0;

  LOG_IR_GEN("TRAP-ONLY-BODY: body collapsed to a single TRAP at i=%d — "
             "suppressing prologue/epilogue", trap_idx);
  ir->ls.dirty_registers = 0;
  ir->ls.dirty_float_registers = 0;
  if (ir->ls.live_regs_by_instruction && ir->ls.live_regs_by_instruction_size > 0)
    memset(ir->ls.live_regs_by_instruction, 0,
           ir->ls.live_regs_by_instruction_size * sizeof(ir->ls.live_regs_by_instruction[0]));
  ir->leaffunc = 1;
  ir->noreturn = 1;
  tcc_state->need_frame_pointer = 0;
  tcc_state->force_frame_pointer = 0;
  return 1;
}

int tcc_ir_opt_trap_only_body_suppress_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_trap_only_body_suppress(ctx->ir);
}

/* Zero-Size VLA Elimination
 *
 * A VLA_ALLOC whose size operand resolves to a compile-time 0 (e.g. an array
 * with a zero-length inner dimension: `T a[n][0]`) doesn't actually change
 * SP — the runtime size expression has already been folded to `0` and stored
 * into the VLA size slot.  We turn such VLA_ALLOC ops into ASSIGN(dest <- #0)
 * so that:
 *   (a) downstream DCE sees the result as a constant rather than a side-
 *       effecting allocation, and
 *   (b) `dead_lea_store_elim` (which bails on any VLA_ALLOC) can now run and
 *       clean up the dead local stores around the eliminated allocation.
 *
 * After eliminating zero-size VLA_ALLOCs, the surrounding VLA_SP_SAVE /
 * VLA_SP_RESTORE pair becomes redundant when no remaining op between them
 * changes SP — we NOP those too.
 *
 * Conservative bails: any IJUMP / SETJMP / LONGJMP / INLINE_ASM in the
 * function (control flow we don't reason about cleanly).  The size-source
 * scan is straight-line — it stops at the first prior write to the slot
 * and bails on intervening calls or indirect stores that could clobber it.
 */
int tcc_ir_opt_zero_vla_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  for (int i = 0; i < n; i++)
  {
    TccIrOp op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SETJMP || op == TCCIR_OP_LONGJMP ||
        op == TCCIR_OP_INLINE_ASM)
      return 0;
  }

  int changed = 0;
  int eliminated_any = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_VLA_ALLOC)
      continue;

    IROperand size_op = tcc_ir_op_get_src1(ir, q);
    int size_is_zero = 0;

    if (irop_is_immediate(size_op) && irop_get_imm64_ex(ir, size_op) == 0)
    {
      size_is_zero = 1;
    }
    else if (irop_get_tag(size_op) == IROP_TAG_STACKOFF)
    {
      int32_t slot = irop_get_stack_offset(size_op);
      for (int j = i - 1; j >= 0; j--)
      {
        IRQuadCompact *qj = &ir->compact_instructions[j];
        TccIrOp jop = qj->op;
        if (jop == TCCIR_OP_NOP)
          continue;

        if (jop == TCCIR_OP_STORE)
        {
          IROperand dest = tcc_ir_op_get_dest(ir, qj);
          if (irop_get_tag(dest) == IROP_TAG_STACKOFF &&
              irop_get_stack_offset(dest) == slot)
          {
            IROperand src = tcc_ir_op_get_src1(ir, qj);
            if (irop_is_immediate(src) && irop_get_imm64_ex(ir, src) == 0)
              size_is_zero = 1;
            break;
          }
        }

        /* Calls / indirect stores / block ops could clobber the slot via
         * aliasing — stop the backward scan conservatively. */
        if (jop == TCCIR_OP_FUNCCALLVOID || jop == TCCIR_OP_FUNCCALLVAL ||
            jop == TCCIR_OP_STORE_INDEXED || jop == TCCIR_OP_BLOCK_COPY)
          break;
      }
    }

    if (!size_is_zero)
      continue;

    /* Convert VLA_ALLOC to ASSIGN(#0).  VLA_ALLOC has no dest in its op
     * config — converting to ASSIGN (which has dest, src1) requires the
     * operand pool to provide a dest slot.  We rely on the pool layout to
     * have an entry at operand_base[0]; for VLA_ALLOC that slot is unused.
     *
     * To stay safe, just NOP the VLA_ALLOC.  The codegen will then skip
     * any SP adjustment for it.  Consumers that store its (now-undefined)
     * result are dead-store-eliminated downstream since their dest slots
     * are never read in the zero-size case. */
    q->op = TCCIR_OP_NOP;
    changed = 1;
    eliminated_any = 1;
  }

  /* The SP_SAVE/RESTORE cleanup below is safe to run independently of whether
   * we just eliminated a VLA_ALLOC: after a previous pass call NOPed the
   * VLA_ALLOC, later passes (e.g. dead_lea_store) may have cleaned up the
   * LEAs that read the VLA's address slot, leaving a now-dead lone SP_SAVE
   * we couldn't see on the first call. */
  (void)eliminated_any;

  /* Helper: does any op in the function read the slot at `slot`? */
#define SLOT_USED_BY(_op, _is_read)                                                                                    \
  ({                                                                                                                   \
    IROperand _s1 = tcc_ir_op_get_src1(ir, (_op));                                                                      \
    IROperand _s2 = tcc_ir_op_get_src2(ir, (_op));                                                                      \
    int _used = 0;                                                                                                     \
    if (irop_get_tag(_s1) == IROP_TAG_STACKOFF && irop_get_stack_offset(_s1) == slot)                                  \
      _used = 1;                                                                                                       \
    if (irop_get_tag(_s2) == IROP_TAG_STACKOFF && irop_get_stack_offset(_s2) == slot)                                  \
      _used = 1;                                                                                                       \
    if (!(_is_read))                                                                                                   \
    {                                                                                                                  \
      IROperand _d = tcc_ir_op_get_dest(ir, (_op));                                                                    \
      if (irop_get_tag(_d) == IROP_TAG_STACKOFF && irop_get_stack_offset(_d) == slot)                                  \
        _used = 1;                                                                                                     \
    }                                                                                                                  \
    _used;                                                                                                             \
  })

  /* NOP redundant VLA_SP_SAVE / VLA_SP_RESTORE pairs whose enclosed region
   * no longer contains any SP-changing op.  Also NOP lone VLA_SP_SAVEs whose
   * dest slot is never read anywhere (the captured SP isn't used). */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_VLA_SP_SAVE)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_tag(dest) != IROP_TAG_STACKOFF)
      continue;
    int32_t slot = irop_get_stack_offset(dest);

    /* Scan the whole function for uses of this slot. */
    int restore_idx = -1;
    int other_reader = 0;
    int other_writer = 0;
    for (int j = 0; j < n; j++)
    {
      if (j == i)
        continue;
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op == TCCIR_OP_NOP)
        continue;

      if (qj->op == TCCIR_OP_VLA_SP_RESTORE)
      {
        IROperand src = tcc_ir_op_get_src1(ir, qj);
        if (irop_get_tag(src) == IROP_TAG_STACKOFF &&
            irop_get_stack_offset(src) == slot)
        {
          if (restore_idx >= 0)
          {
            other_reader = 1;
            break;
          }
          restore_idx = j;
        }
      }
      else if (qj->op == TCCIR_OP_VLA_SP_SAVE)
      {
        IROperand d = tcc_ir_op_get_dest(ir, qj);
        if (irop_get_tag(d) == IROP_TAG_STACKOFF &&
            irop_get_stack_offset(d) == slot)
          other_writer = 1;
      }
      else
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, qj);
        IROperand s2 = tcc_ir_op_get_src2(ir, qj);
        IROperand d = tcc_ir_op_get_dest(ir, qj);
        if ((irop_get_tag(s1) == IROP_TAG_STACKOFF &&
             irop_get_stack_offset(s1) == slot) ||
            (irop_get_tag(s2) == IROP_TAG_STACKOFF &&
             irop_get_stack_offset(s2) == slot))
          other_reader = 1;
        if (irop_get_tag(d) == IROP_TAG_STACKOFF &&
            irop_get_stack_offset(d) == slot)
          other_writer = 1;
      }
    }

    if (other_reader || other_writer)
      continue;

    if (restore_idx < 0)
    {
      /* Lone SP_SAVE with no reader anywhere — pure dead store. */
      ir->compact_instructions[i].op = TCCIR_OP_NOP;
      changed = 1;
      continue;
    }

    /* Paired SAVE/RESTORE — require no SP-changing op between them. */
    int sp_changed = 0;
    for (int j = i + 1; j < restore_idx; j++)
    {
      TccIrOp jop = ir->compact_instructions[j].op;
      if (jop == TCCIR_OP_VLA_ALLOC)
      {
        sp_changed = 1;
        break;
      }
    }
    if (sp_changed)
      continue;

    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[restore_idx].op = TCCIR_OP_NOP;
    changed = 1;
  }

#undef SLOT_USED_BY

  return changed;
}

int tcc_ir_opt_zero_vla_elim_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_zero_vla_elim(ctx->ir);
}

/* Infinite Self-Recursion Collapse
 *
 * If a function unconditionally calls itself before any return path is
 * reachable, every invocation must call itself before returning — by
 * induction the function can never return.  Caller-visible side effects
 * after such a call are unreachable; side effects before it are
 * unobservable (the caller never resumes).  Collapse the body to `b .`,
 * matching GCC -O2 on patterns like gcc.c-torture/compile/pr10153-1.c
 * (`V foo(void) { V v = {}; return v - foo(); }`).
 *
 * Conservative dominance: walk the IR from entry; require the first
 * FUNCCALL{VAL,VOID} encountered to be a self-call, and require nothing
 * preceding it to be able to exit the function (RETURN, JUMP/JUMPIF,
 * SWITCH, IJUMP, asm, setjmp, trap, VLA juggling) or perform an
 * observable volatile access.  This handles linear bodies cleanly; richer
 * dominance can be layered on later.
 */
int tcc_ir_opt_infinite_self_recursion(TCCIRState *ir, Sym *func_sym)
{
  int n = ir->next_instruction_index;
  if (n == 0 || !func_sym)
    return 0;
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;

  int self_call_idx = -1;
  for (int i = 0; i < n && self_call_idx < 0; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    switch (q->op)
    {
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    {
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (callee == func_sym)
      {
        self_call_idx = i;
        break;
      }
      /* Non-self call reached before any self-call: it may return,
       * after which control could fall through to a RETURN.  We can no
       * longer prove non-return — bail. */
      return 0;
    }
    /* Any early exit or branch before the self-call breaks the
     * "unconditionally reached" guarantee. */
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_JUMP:
    case TCCIR_OP_JUMPIF:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_SWITCH_LOAD:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_RETURN:
    case TCCIR_OP_TRAP:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
      return 0;
    default:
      break;
    }

    /* Volatile sym access is observable regardless of return — preserve. */
    for (int k = 0; k <= 2; k++)
    {
      IROperand op;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        if (!irop_config[q->op].has_src2)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }
      if (op.is_sym)
      {
        Sym *sym = irop_get_sym_ex(ir, op);
        if (sym && (sym->type.t & VT_VOLATILE))
          return 0;
      }
    }
  }

  if (self_call_idx < 0)
    return 0;

  LOG_IR_GEN("INFINITE-RECURSION-COLLAPSE: function unconditionally self-calls "
             "at i=%d; collapsing body to `b .`", self_call_idx);

  for (int i = 0; i < n; i++)
  {
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[i].is_jump_target = 0;
  }

  ir->compact_instructions[0].op = TCCIR_OP_JUMP;
  ir->compact_instructions[0].is_jump_target = 1;
  IROperand self = irop_make_imm32(-1, 0, IROP_BTYPE_INT32);
  tcc_ir_set_dest(ir, 0, self);
  tcc_ir_set_src1(ir, 0, IROP_NONE);
  tcc_ir_set_src2(ir, 0, IROP_NONE);

  ir->ls.dirty_registers = 0;
  ir->ls.dirty_float_registers = 0;
  if (ir->ls.live_regs_by_instruction && ir->ls.live_regs_by_instruction_size > 0)
    memset(ir->ls.live_regs_by_instruction, 0,
           ir->ls.live_regs_by_instruction_size * sizeof(ir->ls.live_regs_by_instruction[0]));
  ir->leaffunc = 1;
  ir->noreturn = 1;
  if (func_sym && func_sym->type.ref)
    func_sym->type.ref->f.func_noreturn = 1;

  return 1;
}

/* Noreturn-Call Epilogue Suppress
 *
 * Companion to the DCE extension that treats FUNCCALL-to-noreturn as a
 * terminator (no fall-through).  After that DCE runs, every RETURN op in
 * the function may be unreachable.  When the surviving (non-NOP) IR ends
 * at a FUNCCALL-to-noreturn (i.e., the last live instruction is the call,
 * or only NOP/CALLSEQ_END follow it), the function itself can never
 * return — set ir->noreturn = 1 so codegen omits the dead epilogue.
 *
 * This does NOT publish caller-side noreturn-ness (we'd need to also prove
 * the surviving pre-call body has no observable side effects, which is a
 * stronger check than this pass performs).  It only suppresses the unused
 * `bx lr` / `pop {pc}` tail.
 */
int tcc_ir_opt_noreturn_call_epilogue_suppress(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;
  if (ir->noreturn)
    return 0; /* already set by a stronger pass */

  /* Scan the function: every non-NOP RETURN must be unreachable (was NOP'd
   * by DCE) for us to declare the function noreturn from codegen's POV.
   * A single surviving RETURNVALUE/RETURNVOID anywhere means at least one
   * path exits cleanly.
   *
   * We additionally require at least one FUNCCALL-to-noreturn op to exist
   * (otherwise the function with no RETURN and no noreturn call is either
   * empty, already handled by noreturn_collapse, or has implicit
   * fall-through which the prologue MUST emit `bx lr` for). */
  int has_return = 0;
  int has_noreturn_call = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
    {
      has_return = 1;
      break;
    }
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
    {
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (tcc_ir_callee_is_noreturn(callee))
        has_noreturn_call = 1;
    }
  }
  if (has_return || !has_noreturn_call)
    return 0;

  /* Find the last non-NOP op.  If it's a FUNCCALL-to-noreturn (possibly
   * followed only by CALLSEQ_END), the function ends there and never
   * returns; the epilogue is dead. */
  int last_idx = -1;
  for (int i = n - 1; i >= 0; i--)
  {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_NOP)
      continue;
    if (op == TCCIR_OP_CALLSEQ_END)
      continue; /* harmless trailing frame cleanup */
    last_idx = i;
    break;
  }
  if (last_idx < 0)
    return 0;

  int last_op = ir->compact_instructions[last_idx].op;
  if (last_op != TCCIR_OP_FUNCCALLVAL && last_op != TCCIR_OP_FUNCCALLVOID)
    return 0;

  /* An implicit-return function can have its final live op be a noreturn call
   * on only one branch, e.g. `if (bad) abort();` with the non-abort path
   * jumping to the function end.  In that shape there is no explicit
   * RETURNVOID/RETURNVALUE in the IR, but the backend epilogue is still the
   * target for the other path.  Do not suppress the epilogue if any live jump
   * can land after the final noreturn call (possibly through trailing NOPs). */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int target = -1;
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      target = (int)irop_get_imm64_ex(ir, dest);
    }
    else
      continue;

    while (target >= 0 && target < n && ir->compact_instructions[target].op == TCCIR_OP_NOP)
      target++;
    if (target < 0 || target >= n || target > last_idx)
      return 0;
  }

  Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, &ir->compact_instructions[last_idx]));
  if (!tcc_ir_callee_is_noreturn(callee))
    return 0;

  LOG_IR_GEN("NORETURN-CALL-EPILOGUE-SUPPRESS: function ends at noreturn call "
             "(i=%d) — setting ir->noreturn to skip dead epilogue", last_idx);
  ir->noreturn = 1;
  return 1;
}

/* ============================================================================
 * NOP Compaction - remove NOP instructions from the instruction array
 * ============================================================================
 *
 * After multiple optimization passes, many instructions are marked as NOP.
 * Every subsequent pass still iterates over them.  This pass removes NOPs
 * in a single O(n) sweep, shrinks the array, and fixes all jump targets.
 *
 * Invariants preserved:
 *   - orig_index on each IRQuadCompact is NOT modified (codegen needs it)
 *   - operand_base indices into iroperand_pool are stable (pool is append-only)
 *   - switch_table targets are remapped
 *   - is_jump_target flags are re-derived from remapped jumps
 */
int tcc_ir_opt_compact_nops(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  IRQuadCompact *instr = ir->compact_instructions;

  /* Quick check: any NOPs at all? */
  int has_nops = 0;
  for (int i = 0; i < n; i++)
  {
    if (instr[i].op == TCCIR_OP_NOP)
    {
      has_nops = 1;
      break;
    }
  }
  if (!has_nops)
    return 0;

  /* Build old_to_new mapping and compact in one forward pass */
  int *old_to_new = tcc_malloc(n * sizeof(int));
  int write_pos = 0;

  for (int i = 0; i < n; i++)
  {
    if (instr[i].op == TCCIR_OP_NOP)
    {
      old_to_new[i] = -1;
    }
    else
    {
      old_to_new[i] = write_pos;
      if (write_pos != i)
        instr[write_pos] = instr[i];
      write_pos++;
    }
  }

  int removed = n - write_pos;
  if (removed == 0)
  {
    tcc_free(old_to_new);
    return 0;
  }

  /* Fix jump targets in JUMP / JUMPIF instructions.
   * Targets can be in [0, n] — target == n means "epilogue" (one past the
   * last instruction), set by tcc_ir_backpatch_to_here for return jumps. */
  for (int i = 0; i < write_pos; i++)
  {
    IRQuadCompact *q = &instr[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int old_target = (int)irop_get_imm64_ex(ir, dest);
      if (old_target < 0)
        continue;

      int new_target;
      if (old_target >= n)
      {
        /* Epilogue target (past-end): remap to new past-end */
        new_target = write_pos + (old_target - n);
      }
      else
      {
        new_target = old_to_new[old_target];
        if (new_target < 0)
        {
          /* Target was a NOP — find the next non-NOP instruction after it.
           * This shouldn't normally happen (DCE + jump threading should have
           * fixed dangling targets), but handle it defensively. */
          for (int j = old_target + 1; j < n; j++)
          {
            if (old_to_new[j] >= 0)
            {
              new_target = old_to_new[j];
              break;
            }
          }
          if (new_target < 0)
            new_target = write_pos; /* fall through to epilogue */
        }
      }
      if (new_target != old_target)
      {
        IROperand new_dest = irop_make_imm32(-1, new_target, IROP_BTYPE_INT32);
        tcc_ir_op_set_dest(ir, q, new_dest);
      }
    }
  }

  /* Fix switch table targets — same epilogue-aware remapping as jumps */
  for (int t = 0; t < ir->num_switch_tables; t++)
  {
    TCCIRSwitchTable *table = &ir->switch_tables[t];
    for (int j = 0; j < table->num_entries; j++)
    {
      int old_t = table->targets[j];
      if (old_t < 0)
        continue;
      if (old_t >= n)
      {
        table->targets[j] = write_pos + (old_t - n);
      }
      else
      {
        int new_t = old_to_new[old_t];
        if (new_t < 0)
        {
          for (int k = old_t + 1; k < n; k++)
          {
            if (old_to_new[k] >= 0)
            {
              new_t = old_to_new[k];
              break;
            }
          }
          if (new_t < 0)
            new_t = write_pos;
        }
        table->targets[j] = new_t;
      }
    }
    {
      int old_dt = table->default_target;
      if (old_dt >= 0)
      {
        if (old_dt >= n)
        {
          table->default_target = write_pos + (old_dt - n);
        }
        else
        {
          int new_dt = old_to_new[old_dt];
          if (new_dt < 0)
          {
            for (int k = old_dt + 1; k < n; k++)
            {
              if (old_to_new[k] >= 0)
              {
                new_dt = old_to_new[k];
                break;
              }
            }
            if (new_dt < 0)
              new_dt = write_pos;
          }
          table->default_target = new_dt;
        }
      }
    }
  }

  /* Re-derive is_jump_target flags from scratch */
  for (int i = 0; i < write_pos; i++)
    instr[i].is_jump_target = 0;

  for (int i = 0; i < write_pos; i++)
  {
    IRQuadCompact *q = &instr[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, dest);
      if (target >= 0 && target < write_pos)
        instr[target].is_jump_target = 1;
    }
  }
  for (int t = 0; t < ir->num_switch_tables; t++)
  {
    TCCIRSwitchTable *table = &ir->switch_tables[t];
    for (int j = 0; j < table->num_entries; j++)
    {
      if (table->targets[j] >= 0 && table->targets[j] < write_pos)
        instr[table->targets[j]].is_jump_target = 1;
    }
    if (table->default_target >= 0 && table->default_target < write_pos)
      instr[table->default_target].is_jump_target = 1;
  }

  ir->next_instruction_index = write_pos;

  tcc_free(old_to_new);

  return removed;
}

int tcc_ir_opt_compact_nops_ex(IROptCtx *ctx)
{
  int removed = tcc_ir_opt_compact_nops(ctx->ir);
  if (removed > 0)
    tcc_ir_opt_ctx_invalidate(ctx);
  return removed;
}

/* Constant VAR Propagation - propagate constant-assigned VAR vregs into uses.
 * Designed to run after store-load forwarding which may convert stack loads
 * into constant assignments (e.g. V0 <-- #34 [ASSIGN]) that the main
 * optimization loop's const_prop never saw.
 *
 * Unlike const_prop, this pass:
 *   - Does not check is_local/is_lval flags (safe since we verified single def)
 *   - Converts LOAD→ASSIGN when replacing a local-variable source with a constant
 *   - Handles both src1 and src2 operands
 *
 * This enables the register allocator to avoid callee-saved registers for
 * values that are cheap to rematerialize (small immediates).
 */

int tcc_ir_opt_dse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* Orphaned PARAM elimination: NOP FUNCPARAMVAL/FUNCPARAMVOID instructions
   * whose call_id has no matching FUNCCALLVAL/FUNCCALLVOID.
   * This happens when a function is inlined: the CALL is replaced with inline
   * code but the PARAM (e.g. struct return buffer address) is left behind.
   * Eliminating them removes false address-takes that prevent dead store elim.
   *
   * Combined pass 1+2: single scan that tracks max_call_id AND builds has_call[]
   * (grows dynamically as new call_ids are observed). */
  {
    uint8_t *has_call = NULL;
    int has_call_bytes = 0;
    int max_call_id = 0;
    int saw_any = 0;

    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID && q->op != TCCIR_OP_FUNCCALLVAL &&
          q->op != TCCIR_OP_FUNCCALLVOID)
        continue;

      saw_any = 1;
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int cid = TCCIR_DECODE_CALL_ID((int32_t)irop_get_imm64_ex(ir, src2));
      if (cid > max_call_id)
        max_call_id = cid;

      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
      {
        int needed_bytes = (cid / 8) + 1;
        if (needed_bytes > has_call_bytes)
        {
          int new_bytes = has_call_bytes ? has_call_bytes * 2 : 32;
          while (new_bytes < needed_bytes)
            new_bytes *= 2;
          has_call = tcc_realloc(has_call, new_bytes);
          memset(has_call + has_call_bytes, 0, new_bytes - has_call_bytes);
          has_call_bytes = new_bytes;
        }
        has_call[cid / 8] |= (1 << (cid % 8));
      }
    }

    if (saw_any && max_call_id > 0)
    {
      /* NOP orphaned PARAMs */
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID)
        {
          IROperand src2 = tcc_ir_op_get_src2(ir, q);
          int cid = TCCIR_DECODE_CALL_ID((int32_t)irop_get_imm64_ex(ir, src2));
          int byte_idx = cid / 8;
          int has = (byte_idx < has_call_bytes) && (has_call[byte_idx] & (1 << (cid % 8)));
          if (cid <= max_call_id && !has)
          {
            LOG_IR_GEN("OPTIMIZE: Orphaned PARAM at i=%d (call_id=%d has no CALL)", i, cid);
            q->op = TCCIR_OP_NOP;
          }
        }
      }
    }

    if (has_call)
      tcc_free(has_call);
  }

  /* Pre-scan: eliminate pure FUNCCALLVOIDs and their PARAMs.  After
   * dead_call_result demotes an unused-result FUNCCALLVAL → FUNCCALLVOID,
   * the call has no dest TMP and therefore won't be seeded by the
   * use_count-driven loop below.  Doing this before use_count[] is built
   * means the cascading loop will see the lowered use counts of any TMPs
   * that fed PARAMs and naturally eliminate them. */
  int pure_call_changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVOID)
      continue;
    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee)
      continue;
    /* Only the curated aeabi soft-float/long-int helpers qualify here.
     * They are known to return by value (in registers), so a FUNCCALLVOID
     * — produced by dead_call_result when the result TMP was unused —
     * is safely dead.  We deliberately exclude attribute((pure)) user
     * functions because they may return a struct/_Complex via an sret
     * pointer arg; the sret target may still be live.  Those cases are
     * the dedicated dead_sret_call pass's job. */
    const char *name = get_tok_str(callee->v, NULL);
    /* Pure aeabi soft-float/long-int helpers + the curated set of
     * side-effect-free libc helpers (isnan, etc.) — see
     * ir_opt_is_pure_helper_name.  Both classes return by value (in regs)
     * with no observable side effects, so an unused-result FUNCCALLVOID
     * is safely dead.  The read-only __tcc_str* helpers (strcmp, strlen,
     * ...) only read memory through their pointer args, so an unused-result
     * call is likewise dead — safe here because we are removing the call
     * entirely, not value-numbering it against another call. */
    if (!name || (!tcc_ir_is_pure_aeabi(name) && !ir_opt_is_pure_helper_name(name) &&
                  !ir_opt_is_readonly_str_helper_name(name)))
      continue;
    LOG_IR_GEN("DCE PURE-CALL: nop FUNCCALLVOID at i=%d (callee=%s)", i,
               get_tok_str(callee->v, NULL) ? get_tok_str(callee->v, NULL) : "?");
    ir_opt_nop_call_params(ir, i);
    q->op = TCCIR_OP_NOP;
    pure_call_changes++;
  }

  /* Worklist-based cascading DSE.
   * Single O(n) combined pass: find max_tmp_pos, build use_count[] and
   * def_idx[] simultaneously, with dynamic growth of the per-TMP tables. */
  int max_tmp_pos = 0;
  int table_cap = 32;
  uint16_t *use_count = tcc_mallocz(table_cap * sizeof(uint16_t));
  int *def_idx = tcc_malloc(table_cap * sizeof(int));
  for (int i = 0; i < table_cap; i++)
    def_idx[i] = -1;

  /* Ensure use_count[] / def_idx[] can index position _p (grows on demand) */
#define DSE_ENSURE_CAP(_p)                                                                                             \
  do                                                                                                                   \
  {                                                                                                                    \
    int _pp = (_p);                                                                                                    \
    if (_pp >= table_cap)                                                                                              \
    {                                                                                                                  \
      int _new_cap = table_cap * 2;                                                                                    \
      while (_new_cap <= _pp)                                                                                          \
        _new_cap *= 2;                                                                                                 \
      use_count = tcc_realloc(use_count, _new_cap * sizeof(uint16_t));                                                 \
      memset(use_count + table_cap, 0, (_new_cap - table_cap) * sizeof(uint16_t));                                     \
      def_idx = tcc_realloc(def_idx, _new_cap * sizeof(int));                                                          \
      for (int _k = table_cap; _k < _new_cap; _k++)                                                                    \
        def_idx[_k] = -1;                                                                                              \
      table_cap = _new_cap;                                                                                            \
    }                                                                                                                  \
    if (_pp > max_tmp_pos)                                                                                             \
      max_tmp_pos = _pp;                                                                                               \
  } while (0)

#define DSE_INC_USE(_pos)                                                                                              \
  do                                                                                                                   \
  {                                                                                                                    \
    int _p = (_pos);                                                                                                   \
    if (_p >= 0)                                                                                                       \
    {                                                                                                                  \
      DSE_ENSURE_CAP(_p);                                                                                              \
      if (use_count[_p] < 0xFFFF)                                                                                      \
        use_count[_p]++;                                                                                               \
    }                                                                                                                  \
  } while (0)

  /* Single O(n) pass: build use_count[] and def_idx[] */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (irop_config[q->op].has_src1)
    {
      const IROperand s = tcc_ir_op_get_src1(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
        DSE_INC_USE(TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s)));
    }
    if (irop_config[q->op].has_src2)
    {
      const IROperand s = tcc_ir_op_get_src2(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
        DSE_INC_USE(TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s)));
    }

    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    /* STORE/STORE_INDEXED dest is a pointer use, not a def */
    if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) &&
        TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP)
      DSE_INC_USE(TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest)));
    /* FUNCPARAMVAL dest carries the parameter value — it's a use */
    if (q->op == TCCIR_OP_FUNCPARAMVAL && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP)
      DSE_INC_USE(TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest)));
    /* MLA accumulator is a use */
    if (q->op == TCCIR_OP_MLA)
    {
      const IROperand acc = tcc_ir_op_get_accum(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(acc)) == TCCIR_VREG_TYPE_TEMP)
        DSE_INC_USE(TCCIR_DECODE_VREG_POSITION(irop_get_vreg(acc)));
    }

    /* Record def site for TEMP-destination ops where dest is a real def */
    if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP &&
        q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_FUNCPARAMVAL)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest));
      if (pos >= 0)
      {
        DSE_ENSURE_CAP(pos);
        def_idx[pos] = i; /* last def wins; OK since we only eliminate when use_count hits 0 */
      }
    }
  }

  if (max_tmp_pos == 0)
  {
    tcc_free(use_count);
    tcc_free(def_idx);
    return pure_call_changes;
  }

  int changes = pure_call_changes;
  LOG_IR_GEN("=== DEAD STORE ELIMINATION START ===");

  /* True iff this CALL targets a by-value-returning, side-effect-free
   * callee.  Restricted to the curated aeabi soft-float/long-int helper
   * list — those are known to never use an sret arg.  See the matching
   * comment in the FUNCCALLVOID pre-scan above. */
#define DSE_IS_PURE_CALL(_q)                                                                                           \
  ({                                                                                                                   \
    int _pure = 0;                                                                                                     \
    if ((_q)->op == TCCIR_OP_FUNCCALLVAL || (_q)->op == TCCIR_OP_FUNCCALLVOID)                                         \
    {                                                                                                                  \
      Sym *_callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, (_q)));                                                \
      if (_callee)                                                                                                     \
      {                                                                                                                \
        const char *_name = get_tok_str(_callee->v, NULL);                                                             \
        if (_name && tcc_ir_is_pure_aeabi(_name))                                                                      \
          _pure = 1;                                                                                                   \
      }                                                                                                                \
    }                                                                                                                  \
    _pure;                                                                                                             \
  })

  /* Reusable dead-eligibility predicate.
   *
   * A LOAD is eligible (dead-result kills the op) when its source has no
   * possible side effect of being read: immediates, symref reads, reads
   * from anonymous TEMP_LOCAL stack slots (vreg in [-9,-2]), and reads
   * from any LOCAL stack address (IROP_TAG_STACKOFF + is_local) all
   * qualify — none can be observed by another agent.  Without the local
   * stack case, a chain of write → LOAD-local + dead consumer leaves the
   * LOAD undead, which keeps the source slot artificially alive in the
   * downstream dead_local_slot_elim pass. */
#define DSE_IS_DEAD_ELIGIBLE(_q)                                                                                       \
  (((_q)->op != TCCIR_OP_STORE && (_q)->op != TCCIR_OP_STORE_INDEXED && (_q)->op != TCCIR_OP_STORE_POSTINC &&          \
    (_q)->op != TCCIR_OP_LOAD_POSTINC && (_q)->op != TCCIR_OP_LOAD && (_q)->op != TCCIR_OP_FUNCCALLVAL &&              \
    (_q)->op != TCCIR_OP_FUNCCALLVOID && (_q)->op != TCCIR_OP_FUNCPARAMVAL && (_q)->op != TCCIR_OP_FUNCPARAMVOID) ||   \
   ((_q)->op == TCCIR_OP_LOAD &&                                                                                       \
    (irop_is_immediate(tcc_ir_op_get_src1(ir, (_q))) || tcc_ir_op_get_src1(ir, (_q)).is_sym ||                         \
     (irop_get_vreg(tcc_ir_op_get_src1(ir, (_q))) <= -2 && irop_get_vreg(tcc_ir_op_get_src1(ir, (_q))) >= -9) ||       \
     (irop_get_tag(tcc_ir_op_get_src1(ir, (_q))) == IROP_TAG_STACKOFF &&                                               \
      tcc_ir_op_get_src1(ir, (_q)).is_local))) ||                                                                      \
   DSE_IS_PURE_CALL(_q))

  /* When NOP'ing a pure call, also NOP its matching PARAMs and decrement
   * use_count for any TMPs they consumed. Pushes newly-dead TMPs to the
   * worklist so the cascade can continue. */
#define DSE_CASCADE_PURE_CALL_PARAMS(_call_idx)                                                                        \
  do                                                                                                                   \
  {                                                                                                                    \
    int _ci = (_call_idx);                                                                                             \
    IRQuadCompact *_cq = &ir->compact_instructions[_ci];                                                               \
    int _cid = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, _cq)));                     \
    for (int _pi = _ci - 1; _pi >= 0; --_pi)                                                                           \
    {                                                                                                                  \
      IRQuadCompact *_pq = &ir->compact_instructions[_pi];                                                             \
      if (_pq->op == TCCIR_OP_NOP)                                                                                     \
        continue;                                                                                                      \
      if (_pq->op != TCCIR_OP_FUNCPARAMVAL && _pq->op != TCCIR_OP_FUNCPARAMVOID)                                       \
        continue;                                                                                                      \
      int _pid = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, _pq)));                   \
      if (_pid != _cid)                                                                                                \
        continue;                                                                                                      \
      if (irop_config[_pq->op].has_src1)                                                                               \
      {                                                                                                                \
        const IROperand _s = tcc_ir_op_get_src1(ir, _pq);                                                              \
        if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(_s)) == TCCIR_VREG_TYPE_TEMP)                                         \
        {                                                                                                              \
          int _pp = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(_s));                                                     \
          if (_pp >= 0 && _pp <= max_tmp_pos && use_count[_pp] > 0)                                                    \
            if (--use_count[_pp] == 0)                                                                                 \
              worklist[wl_top++] = _pp;                                                                                \
        }                                                                                                              \
      }                                                                                                                \
      if (_pq->op == TCCIR_OP_FUNCPARAMVAL)                                                                            \
      {                                                                                                                \
        const IROperand _d = tcc_ir_op_get_dest(ir, _pq);                                                              \
        if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(_d)) == TCCIR_VREG_TYPE_TEMP)                                         \
        {                                                                                                              \
          int _pp = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(_d));                                                     \
          if (_pp >= 0 && _pp <= max_tmp_pos && use_count[_pp] > 0)                                                    \
            if (--use_count[_pp] == 0)                                                                                 \
              worklist[wl_top++] = _pp;                                                                                \
        }                                                                                                              \
      }                                                                                                                \
      LOG_IR_GEN("DCE PURE-CALL: nop PARAM i=%d (call_id=%d)", _pi, _cid);                                             \
      _pq->op = TCCIR_OP_NOP;                                                                                          \
      changes++;                                                                                                       \
    }                                                                                                                  \
  } while (0)

  /* Worklist of TMP positions whose use_count just dropped to 0 */
  int *worklist = tcc_malloc((max_tmp_pos + 1) * sizeof(int));
  int wl_top = 0;

  /* Seed the worklist by eliminating all initially-dead instructions */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!irop_config[q->op].has_dest)
      continue;
    if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest));
    if (pos > max_tmp_pos || use_count[pos] != 0)
      continue;
    if (!DSE_IS_DEAD_ELIGIBLE(q))
      continue;

    /* Decrement use_count for this instruction's sources before NOP'ing it */
    if (irop_config[q->op].has_src1)
    {
      const IROperand s = tcc_ir_op_get_src1(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
      {
        int p = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s));
        if (p >= 0 && p <= max_tmp_pos && use_count[p] > 0)
        {
          if (--use_count[p] == 0)
            worklist[wl_top++] = p;
        }
      }
    }
    if (irop_config[q->op].has_src2)
    {
      const IROperand s = tcc_ir_op_get_src2(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
      {
        int p = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s));
        if (p >= 0 && p <= max_tmp_pos && use_count[p] > 0)
        {
          if (--use_count[p] == 0)
            worklist[wl_top++] = p;
        }
      }
    }
    if (q->op == TCCIR_OP_MLA)
    {
      const IROperand acc = tcc_ir_op_get_accum(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(acc)) == TCCIR_VREG_TYPE_TEMP)
      {
        int p = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(acc));
        if (p >= 0 && p <= max_tmp_pos && use_count[p] > 0)
        {
          if (--use_count[p] == 0)
            worklist[wl_top++] = p;
        }
      }
    }
    if (q->op == TCCIR_OP_FUNCCALLVAL)
    {
      LOG_IR_GEN("DCE PURE-CALL: nop CALL at i=%d (result tmp dead)", i);
      DSE_CASCADE_PURE_CALL_PARAMS(i);
    }
    q->op = TCCIR_OP_NOP;
    def_idx[pos] = -1;
    changes++;
  }

  /* Drain the worklist: cascade eliminations */
  while (wl_top > 0)
  {
    int pos = worklist[--wl_top];
    int di = def_idx[pos];
    if (di < 0 || di >= n)
      continue;
    IRQuadCompact *q = &ir->compact_instructions[di];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (use_count[pos] != 0)
      continue; /* someone used it again via a different def */
    if (!DSE_IS_DEAD_ELIGIBLE(q))
      continue;
    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!irop_config[q->op].has_dest || TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) != TCCIR_VREG_TYPE_TEMP)
      continue;

    if (irop_config[q->op].has_src1)
    {
      const IROperand s = tcc_ir_op_get_src1(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
      {
        int p = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s));
        if (p >= 0 && p <= max_tmp_pos && use_count[p] > 0)
        {
          if (--use_count[p] == 0)
            worklist[wl_top++] = p;
        }
      }
    }
    if (irop_config[q->op].has_src2)
    {
      const IROperand s = tcc_ir_op_get_src2(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
      {
        int p = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s));
        if (p >= 0 && p <= max_tmp_pos && use_count[p] > 0)
        {
          if (--use_count[p] == 0)
            worklist[wl_top++] = p;
        }
      }
    }
    if (q->op == TCCIR_OP_MLA)
    {
      const IROperand acc = tcc_ir_op_get_accum(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(acc)) == TCCIR_VREG_TYPE_TEMP)
      {
        int p = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(acc));
        if (p >= 0 && p <= max_tmp_pos && use_count[p] > 0)
        {
          if (--use_count[p] == 0)
            worklist[wl_top++] = p;
        }
      }
    }
    if (q->op == TCCIR_OP_FUNCCALLVAL)
    {
      LOG_IR_GEN("DCE PURE-CALL: nop CALL at i=%d (cascade)", di);
      DSE_CASCADE_PURE_CALL_PARAMS(di);
    }
    q->op = TCCIR_OP_NOP;
    def_idx[pos] = -1;
    changes++;
  }

#undef DSE_INC_USE
#undef DSE_ENSURE_CAP
#undef DSE_IS_DEAD_ELIGIBLE
#undef DSE_IS_PURE_CALL
#undef DSE_CASCADE_PURE_CALL_PARAMS

  LOG_IR_GEN("=== DEAD STORE ELIMINATION END (marked %d as NOP) ===", changes);
  tcc_free(worklist);
  tcc_free(def_idx);
  tcc_free(use_count);

  /* Also eliminate dead VAR vreg definitions.
   * A VAR that is defined (ASSIGN) but never used as a source operand
   * anywhere in the function is dead — provided it's not address-taken. */
  {
    int max_var_pos = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      const IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
      if (irop_config[q->op].has_dest && vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos > max_var_pos)
          max_var_pos = pos;
      }
    }

    if (max_var_pos > 0)
    {
      uint8_t *var_used = tcc_mallocz((max_var_pos + 8) / 8);

      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;

        /* Check src1 */
        if (irop_config[q->op].has_src1)
        {
          const IROperand s = tcc_ir_op_get_src1(ir, q);
          int32_t vr = irop_get_vreg(s);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
          {
            int pos = TCCIR_DECODE_VREG_POSITION(vr);
            if (pos <= max_var_pos)
              var_used[pos / 8] |= (1 << (pos % 8));
          }
        }

        /* Check src2 */
        if (irop_config[q->op].has_src2)
        {
          const IROperand s = tcc_ir_op_get_src2(ir, q);
          int32_t vr = irop_get_vreg(s);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
          {
            int pos = TCCIR_DECODE_VREG_POSITION(vr);
            if (pos <= max_var_pos)
              var_used[pos / 8] |= (1 << (pos % 8));
          }
        }

        /* STORE dest: only a use when it's a pointer dereference (non-local),
         * not when it's a direct local store (which is a define).
         * STORE_INDEXED dest is always a pointer use (base of indexed access),
         * even when the variable is local — the indexed store reads the base
         * address, it doesn't define it. */
        if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED)
        {
          const IROperand d = tcc_ir_op_get_dest(ir, q);
          if (!d.is_local || q->op == TCCIR_OP_STORE_INDEXED)
          {
            int32_t vr = irop_get_vreg(d);
            if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
            {
              int pos = TCCIR_DECODE_VREG_POSITION(vr);
              if (pos <= max_var_pos)
                var_used[pos / 8] |= (1 << (pos % 8));
            }
          }
        }

        /* FUNCPARAMVAL dest carries the parameter value — it's a use, not a def */
        if (q->op == TCCIR_OP_FUNCPARAMVAL)
        {
          const IROperand d = tcc_ir_op_get_dest(ir, q);
          int32_t vr = irop_get_vreg(d);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
          {
            int pos = TCCIR_DECODE_VREG_POSITION(vr);
            if (pos <= max_var_pos)
              var_used[pos / 8] |= (1 << (pos % 8));
          }
        }
      }

      /* NOP ASSIGN/STORE to unused VARs (skip address-taken) */
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_STORE)
          continue;
        const IROperand dest = tcc_ir_op_get_dest(ir, q);
        /* For STORE, only eliminate local stores (not pointer dereferences) */
        if (q->op == TCCIR_OP_STORE && !dest.is_local)
          continue;
        /* Skip stores to parent frame via static chain — externally visible */
        if (dest.is_llocal)
          continue;
        int32_t vr = irop_get_vreg(dest);
        if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
          continue;
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos > max_var_pos)
          continue;
        if (var_used[pos / 8] & (1 << (pos % 8)))
          continue; /* VAR is used */
        /* Skip address-taken variables */
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vr);
        if (interval && interval->addrtaken)
          continue;
        q->op = TCCIR_OP_NOP;
        changes++;
      }

      tcc_free(var_used);
    }
  }

  /* Dead StackLoc store elimination.
   * STORE to anonymous StackLoc offsets (not VAR vregs) that are never read
   * and whose addresses are never taken can be safely eliminated.
   * Uses a hash set of (sym, offset) pairs to track read/address-taken locations.
   *
   * Skip if function uses static chain — two cases:
   * 1. Function is a nested function (has_static_chain=1): it may write to
   *    the parent's frame via the chain pointer, and those writes are
   *    externally visible even though they look dead within this function.
   * 2. Function contains SET_CHAIN instructions: it is a parent function
   *    that sets up the chain for nested function calls, meaning its own
   *    stack variables may be read by the nested functions. */
  {
    if (ir->has_static_chain)
      goto skip_dead_stackloc;

    for (int i = 0; i < n; i++)
    {
      if (ir->compact_instructions[i].op == TCCIR_OP_SET_CHAIN)
        goto skip_dead_stackloc;
    }
#define STACKLOC_HASH_SIZE 256
    uint8_t stackloc_read[STACKLOC_HASH_SIZE];
    memset(stackloc_read, 0, sizeof(stackloc_read));

    /* Pre-scan: find the maximum StackLoc offset used in any STORE.
     * This determines how far an address-of range needs to extend. */
    int64_t max_stackloc_off = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_STORE)
        continue;
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (!dest.is_local || irop_get_vreg(dest) >= 0)
        continue;
      int64_t off;
      if (irop_get_tag(dest) == IROP_TAG_SYMREF)
      {
        IRPoolSymref *sr = irop_get_symref_ex(ir, dest);
        off = sr ? sr->addend : 0;
      }
      else
      {
        off = irop_get_stack_offset(dest);
      }
      if (off > max_stackloc_off)
        max_stackloc_off = off;
    }

    /* Helper: hash a (sym, offset) pair to a bit index */
#define STACKLOC_HASH(sym, off) (((uintptr_t)(sym) * 31 + (uint32_t)(off) * 17) % (STACKLOC_HASH_SIZE * 8))
#define STACKLOC_SET(sym, off)                                                                                         \
  do                                                                                                                   \
  {                                                                                                                    \
    uint32_t _h = STACKLOC_HASH(sym, off);                                                                             \
    stackloc_read[_h / 8] |= (1 << (_h % 8));                                                                          \
  } while (0)
#define STACKLOC_TEST(sym, off) (stackloc_read[STACKLOC_HASH(sym, off) / 8] & (1 << (STACKLOC_HASH(sym, off) % 8)))

    /* Pre-scan: identify write-only address-of TEMPs.
     * An addr-TMP is "write-only" if the entire chain from the Addr[StackLoc]
     * through VAR intermediaries down to final uses consists only of:
     *   - STORE addr-prop-TMP → VAR  (address pipeline flow)
     *   - ASSIGN addr-prop-VAR → TMP (address pipeline flow)
     *   - ADD addr-prop-TMP, offset → TMP (pointer arithmetic)
     *   - STORE value → *addr-prop-TMP  (deref write — safe)
     * Any other use (LOAD, FUNCPARAM, TEST_ZERO, CMP, etc.) means the address
     * or pointed-to data is observable, so the addr-TMP is marked "read". */
    int max_tmp_stackloc = 0;
    int max_var_stackloc = -1;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      IROperand ops[3];
      int nops = 0;
      if (irop_config[q->op].has_dest)
        ops[nops++] = tcc_ir_op_get_dest(ir, q);
      if (irop_config[q->op].has_src1)
        ops[nops++] = tcc_ir_op_get_src1(ir, q);
      if (irop_config[q->op].has_src2)
        ops[nops++] = tcc_ir_op_get_src2(ir, q);
      for (int k = 0; k < nops; k++)
      {
        int32_t vr = irop_get_vreg(ops[k]);
        if (vr >= 0)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && pos > max_tmp_stackloc)
            max_tmp_stackloc = pos;
          else if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && pos > max_var_stackloc)
            max_var_stackloc = pos;
        }
      }
    }

    /* addr_tmp[pos] = 1 if TMP pos was defined from an Addr[StackLoc] */
    /* addr_tmp_read[pos] = 1 if the addr or pointed-to data is observable */
    uint8_t *addr_tmp = NULL;
    uint8_t *addr_tmp_read = NULL;

    if (max_tmp_stackloc > 0)
    {
      addr_tmp = tcc_mallocz((max_tmp_stackloc + 8) / 8);
      addr_tmp_read = tcc_mallocz((max_tmp_stackloc + 8) / 8);

      /* Phase 1: Find TEMPs defined from Addr[StackLoc] (is_local=1, is_lval=0) */
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        if (irop_config[q->op].has_src1)
        {
          IROperand s = tcc_ir_op_get_src1(ir, q);
          if (s.is_local && !s.is_lval && irop_get_vreg(s) < 0)
          {
            IROperand d = tcc_ir_op_get_dest(ir, q);
            int32_t dvr = irop_get_vreg(d);
            if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
            {
              int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
              if (dpos <= max_tmp_stackloc)
                addr_tmp[dpos / 8] |= (1 << (dpos % 8));
            }
          }
        }
      }

      /* Phase 2: Propagate addr-prop through STORE→VAR, ASSIGN→TMP, ADD→TMP.
       * prop_tmp[pos] and prop_var[pos] record the origin addr-TMP position,
       * or -1 (not addr-prop) or -2 (ambiguous / multiple origins). */
      int *prop_tmp = tcc_mallocz((max_tmp_stackloc + 1) * sizeof(int));
      int *prop_var = max_var_stackloc >= 0 ? tcc_mallocz((max_var_stackloc + 1) * sizeof(int)) : NULL;
      memset(prop_tmp, 0xFF, (max_tmp_stackloc + 1) * sizeof(int)); /* -1 */
      if (prop_var)
        memset(prop_var, 0xFF, (max_var_stackloc + 1) * sizeof(int));

      /* Seed from addr-TMPs */
      for (int pos = 0; pos <= max_tmp_stackloc; pos++)
        if (addr_tmp[pos / 8] & (1 << (pos % 8)))
          prop_tmp[pos] = pos;

      /* Propagate through the chain */
      int prop_changed = 1;
      while (prop_changed)
      {
        prop_changed = 0;
        for (int i = 0; i < n; i++)
        {
          IRQuadCompact *q = &ir->compact_instructions[i];
          if (q->op == TCCIR_OP_NOP)
            continue;

          if (!irop_config[q->op].has_src1 || !irop_config[q->op].has_dest)
            continue;
          IROperand src = tcc_ir_op_get_src1(ir, q);
          IROperand dest = tcc_ir_op_get_dest(ir, q);
          int32_t svr = irop_get_vreg(src);
          int32_t dvr = irop_get_vreg(dest);
          if (svr < 0 || dvr < 0)
            continue;

          int stype = TCCIR_DECODE_VREG_TYPE(svr);
          int dtype = TCCIR_DECODE_VREG_TYPE(dvr);
          int spos = TCCIR_DECODE_VREG_POSITION(svr);
          int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
          int origin = -1;

          /* STORE/ASSIGN TMP→VAR: addr flows from TMP to VAR.
           * ASSIGN case arises after copy propagation rewrites STORE to ASSIGN
           * or propagates an addr-TMP through a TMP→VAR assignment chain. */
          if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_ASSIGN) && stype == TCCIR_VREG_TYPE_TEMP &&
              dtype == TCCIR_VREG_TYPE_VAR && spos <= max_tmp_stackloc && prop_var && dpos <= max_var_stackloc)
            origin = prop_tmp[spos];
          /* ASSIGN VAR→TMP: addr flows from VAR to TMP */
          else if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) && stype == TCCIR_VREG_TYPE_VAR &&
                   dtype == TCCIR_VREG_TYPE_TEMP && prop_var && spos <= max_var_stackloc && dpos <= max_tmp_stackloc)
            origin = prop_var[spos];
          /* ASSIGN/LOAD VAR→VAR: addr flows from one VAR to another */
          else if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) && stype == TCCIR_VREG_TYPE_VAR &&
                   dtype == TCCIR_VREG_TYPE_VAR && prop_var && spos <= max_var_stackloc && dpos <= max_var_stackloc)
            origin = prop_var[spos];
          /* ADD/SUB/ASSIGN TMP→TMP: pointer arithmetic and copies preserve addr-prop */
          else if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_ASSIGN) &&
                   stype == TCCIR_VREG_TYPE_TEMP && dtype == TCCIR_VREG_TYPE_TEMP && spos <= max_tmp_stackloc &&
                   dpos <= max_tmp_stackloc)
            origin = prop_tmp[spos];

          if (origin < 0)
            continue;

          /* Propagate to dest */
          if (dtype == TCCIR_VREG_TYPE_TEMP && dpos <= max_tmp_stackloc)
          {
            if (prop_tmp[dpos] == -1)
            {
              prop_tmp[dpos] = origin;
              prop_changed = 1;
            }
            else if (prop_tmp[dpos] != origin && prop_tmp[dpos] != -2)
            {
              prop_tmp[dpos] = -2; /* ambiguous */
              prop_changed = 1;
            }
          }
          else if (dtype == TCCIR_VREG_TYPE_VAR && prop_var && dpos <= max_var_stackloc)
          {
            if (prop_var[dpos] == -1)
            {
              prop_var[dpos] = origin;
              prop_changed = 1;
            }
            else if (prop_var[dpos] != origin && prop_var[dpos] != -2)
            {
              prop_var[dpos] = -2;
              prop_changed = 1;
            }
          }
        }
      }

      /* Phase 3: Check uses of all addr-prop values. Mark origin addr-TMP
       * as "read" if any propagated value is used outside the write pipeline. */
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;

        /* Helper: get origin of a vreg, or -1 if not addr-prop */
#define GET_ORIGIN(vr)                                                                                                 \
  ({                                                                                                                   \
    int _o = -1;                                                                                                       \
    int _t = TCCIR_DECODE_VREG_TYPE(vr);                                                                               \
    int _p = TCCIR_DECODE_VREG_POSITION(vr);                                                                           \
    if (_t == TCCIR_VREG_TYPE_TEMP && _p <= max_tmp_stackloc)                                                          \
      _o = prop_tmp[_p];                                                                                               \
    else if (_t == TCCIR_VREG_TYPE_VAR && prop_var && _p <= max_var_stackloc)                                          \
      _o = prop_var[_p];                                                                                               \
    _o;                                                                                                                \
  })

#define MARK_ORIGIN_READ(origin)                                                                                       \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((origin) == -2)                                                                                                \
      memset(addr_tmp_read, 0xFF, (max_tmp_stackloc + 8) / 8);                                                         \
    else if ((origin) >= 0 && (origin) <= max_tmp_stackloc)                                                            \
      addr_tmp_read[(origin) / 8] |= (1 << ((origin) % 8));                                                            \
  } while (0)

        /* Check src1 */
        if (irop_config[q->op].has_src1)
        {
          IROperand s = tcc_ir_op_get_src1(ir, q);
          int32_t vr = irop_get_vreg(s);
          if (vr >= 0)
          {
            int origin = GET_ORIGIN(vr);
            if (origin != -1)
            {
              /* Is this use safe (within the write pipeline)? */
              int safe = 0;
              if (q->op == TCCIR_OP_STORE && (!s.is_lval || TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR))
              {
                /* Storing addr value into a VAR = pipeline flow.
                 * For TMP src, is_lval=1 means deref read (*ptr) — NOT safe.
                 * For VAR src, is_lval=1 just means "load variable" — safe. */
                IROperand d = tcc_ir_op_get_dest(ir, q);
                int32_t dvr = irop_get_vreg(d);
                if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR)
                  safe = 1;
              }
              else if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
              {
                /* Pipeline flow or pointer arithmetic — but a deref read
                 * (is_lval=1 on a TMP) means memory at the address is being
                 * read, making the pointed-to data observable.
                 * For VAR sources, is_lval=1 just means "load the variable's
                 * value" (always set for VAR reads) — this is a pointer copy,
                 * NOT a dereference through the pointed-to data. */
                int stype = TCCIR_DECODE_VREG_TYPE(vr);
                if (!s.is_lval || stype == TCCIR_VREG_TYPE_VAR)
                  safe = 1;
              }
              if (!safe)
              {
                LOG_IR_GEN("DSE-SL: Phase3 MARK READ origin=%d at i=%d op=%d src1 is_lval=%d", origin, i, q->op,
                           s.is_lval);
                MARK_ORIGIN_READ(origin);
              }
            }
          }
        }

        /* Check src2: addr-prop as src2 is unusual; conservatively mark read */
        if (irop_config[q->op].has_src2)
        {
          IROperand s = tcc_ir_op_get_src2(ir, q);
          int32_t vr = irop_get_vreg(s);
          if (vr >= 0)
          {
            int origin = GET_ORIGIN(vr);
            if (origin != -1)
            {
              LOG_IR_GEN("DSE-SL: Phase3 MARK READ origin=%d at i=%d op=%d src2", origin, i, q->op);
              MARK_ORIGIN_READ(origin);
            }
          }
        }

        /* STORE dest: if dest is an addr-prop TMP (deref write), that's safe.
         * No marking needed — this is a write through the pointer. */

#undef GET_ORIGIN
#undef MARK_ORIGIN_READ
      }

      tcc_free(prop_tmp);
      if (prop_var)
        tcc_free(prop_var);
    }

    /* Pass 1: Mark StackLoc offsets that are read or address-taken */
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;

      /* Helper: mark a StackLoc operand as read or address-taken.
       * is_lval=true means direct memory read → mark exact offset.
       * is_lval=false means address-of → mark range up to max store offset.
       * Write-only address-of (pointer only used for writes) is skipped. */
#define MARK_STACKLOC_OP(op)                                                                                           \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((op).is_local && irop_get_vreg(op) < 0)                                                                        \
    {                                                                                                                  \
      const Sym *_sym = NULL;                                                                                          \
      int64_t _off;                                                                                                    \
      if (irop_get_tag(op) == IROP_TAG_SYMREF)                                                                         \
      {                                                                                                                \
        IRPoolSymref *_sr = irop_get_symref_ex(ir, op);                                                                \
        _sym = _sr ? _sr->sym : NULL;                                                                                  \
        _off = _sr ? _sr->addend : 0;                                                                                  \
      }                                                                                                                \
      else                                                                                                             \
      {                                                                                                                \
        _off = irop_get_stack_offset(op);                                                                              \
      }                                                                                                                \
      if ((op).is_lval)                                                                                                \
      {                                                                                                                \
        /* Mark all byte offsets within the access width so that stores                                                \
         * at sub-offsets (e.g. _Complex short imag part at _off+2)                                                    \
         * are not incorrectly eliminated as dead. */                                                                  \
        int _width;                                                                                                    \
        switch ((op).btype)                                                                                            \
        {                                                                                                              \
        case IROP_BTYPE_INT8:                                                                                          \
          _width = 1;                                                                                                  \
          break;                                                                                                       \
        case IROP_BTYPE_INT16:                                                                                         \
          _width = 2;                                                                                                  \
          break;                                                                                                       \
        case IROP_BTYPE_FLOAT32:                                                                                       \
          _width = 4;                                                                                                  \
          break;                                                                                                       \
        case IROP_BTYPE_INT64:                                                                                         \
        case IROP_BTYPE_FLOAT64:                                                                                       \
          _width = 8;                                                                                                  \
          break;                                                                                                       \
        case IROP_BTYPE_STRUCT:                                                                                        \
        {                                                                                                              \
          /* Struct access — conservatively mark range up to max store offset */                                       \
          int64_t _send = max_stackloc_off + 4;                                                                        \
          for (int64_t _s = _off; _s <= _send; _s++)                                                                   \
            STACKLOC_SET(_sym, _s);                                                                                    \
          _width = 0; /* already handled */                                                                            \
          break;                                                                                                       \
        }                                                                                                              \
        default:                                                                                                       \
          _width = 4;                                                                                                  \
          break;                                                                                                       \
        }                                                                                                              \
        /* Complex types implicitly read both real and imag halves —                                                   \
         * double the width so the imag store at +elem_size isn't DSE'd. */                                            \
        if ((op).is_complex)                                                                                           \
          _width *= 2;                                                                                                 \
        for (int _b = 0; _b < _width; _b++)                                                                            \
          STACKLOC_SET(_sym, _off + _b);                                                                               \
      }                                                                                                                \
      else                                                                                                             \
      {                                                                                                                \
        /* Address-of: mark from base offset to max store offset (+ margin for field access).                          \
         * Cap range to avoid excessive iteration; if too large, mark all bits. */                                     \
        int64_t _range_end = max_stackloc_off + 4;                                                                     \
        int64_t _range_len = _range_end - _off + 1;                                                                    \
        if (_range_len > STACKLOC_HASH_SIZE * 8)                                                                       \
        {                                                                                                              \
          memset(stackloc_read, 0xFF, sizeof(stackloc_read));                                                          \
        }                                                                                                              \
        else if (_range_len > 0)                                                                                       \
        {                                                                                                              \
          for (int64_t _k = _off; _k <= _range_end; _k++)                                                              \
            STACKLOC_SET(_sym, _k);                                                                                    \
        }                                                                                                              \
      }                                                                                                                \
    }                                                                                                                  \
  } while (0)

      /* Check all operands for StackLoc reads / address-taken */
      if (irop_config[q->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        /* Skip range marking for address-of StackLoc that feeds a write-only TMP.
         * If the TMP is only used for STORE destinations (pointer writes), the
         * address-of doesn't constitute a "read" of the StackLoc range. */
        if (s.is_local && !s.is_lval && irop_get_vreg(s) < 0 && addr_tmp != NULL)
        {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          int32_t dvr = irop_get_vreg(d);
          if (TCC_LOG_IR_GEN)
          {
            fprintf(stderr, "[IR_GEN] DSE-SL: addr-of check i=%d dvr=0x%x type=%d pos=%d max=%d", i, (unsigned)dvr,
                    dvr >= 0 ? TCCIR_DECODE_VREG_TYPE(dvr) : -1, dvr >= 0 ? TCCIR_DECODE_VREG_POSITION(dvr) : -1,
                    max_tmp_stackloc);
            if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
            {
              int _dp = TCCIR_DECODE_VREG_POSITION(dvr);
              fprintf(stderr, " addr_tmp=%d addr_tmp_read=%d",
                      !!(_dp <= max_tmp_stackloc && (addr_tmp[_dp / 8] & (1 << (_dp % 8)))),
                      !!(_dp <= max_tmp_stackloc && (addr_tmp_read[_dp / 8] & (1 << (_dp % 8)))));
            }
            fprintf(stderr, "\n");
          }
          if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
          {
            int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
            if (dpos <= max_tmp_stackloc && (addr_tmp[dpos / 8] & (1 << (dpos % 8))) &&
                !(addr_tmp_read[dpos / 8] & (1 << (dpos % 8))))
            {
              LOG_IR_GEN("DSE-SL: SKIP addr-of at i=%d (write-only T%d)", i, dpos);
              goto after_src1_mark;
            }
          }
        }
        LOG_IR_GEN("DSE-SL: MARK src1 at i=%d op=%d is_lval=%d is_local=%d", i, q->op, s.is_lval, s.is_local);
        /* FUNCPARAMVAL/FUNCPARAMVOID src1 passing a STRUCT from a StackLoc
         * base may span multiple consecutive words (size not encoded in
         * btype) — force range marking for that case only. Scalar value
         * params (INT/FLOAT) have a well-defined width and use the normal
         * is_lval=1 width-based marking; over-marking them as full-range
         * masks legitimately dead stores at higher offsets. */
        if ((q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID) && s.is_local &&
            irop_get_vreg(s) < 0 && s.btype == IROP_BTYPE_STRUCT)
        {
          IROperand s_range = s;
          s_range.is_lval = 0;
          MARK_STACKLOC_OP(s_range);
        }
        else
        {
          MARK_STACKLOC_OP(s);
        }
      after_src1_mark:;
      }
      if (irop_config[q->op].has_src2)
      {
        IROperand s = tcc_ir_op_get_src2(ir, q);
        LOG_IR_GEN("DSE-SL: MARK src2 at i=%d op=%d is_lval=%d is_local=%d", i, q->op, s.is_lval, s.is_local);
        MARK_STACKLOC_OP(s);
      }
      /* dest operands are not uses for most instructions (they are defines).
       * Special cases (FUNCPARAMVAL src1) are handled above. */
      /* MLA accumulator (4th operand) may reference a StackLoc */
      if (q->op == TCCIR_OP_MLA)
      {
        IROperand acc = tcc_ir_op_get_accum(ir, q);
        MARK_STACKLOC_OP(acc);
      }
#undef MARK_STACKLOC_OP
    }

    /* Pass 2: Eliminate STORE to unread StackLoc offsets */
#if TCC_LOG_IR_GEN
    {
      int any_set = 0;
      for (int bi = 0; bi <= max_stackloc_off / 8; bi++)
        if (stackloc_read[bi])
        {
          any_set = 1;
          break;
        }
      LOG_IR_GEN("DSE-SL: stackloc_read has %s bits set, max_stackloc_off=%lld", any_set ? "some" : "NO",
                 (long long)max_stackloc_off);
    }
#endif
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_STORE)
        continue;
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (!dest.is_local || irop_get_vreg(dest) >= 0)
        continue; /* Not an anonymous StackLoc */
      if (dest.is_llocal)
        continue; /* Store to parent frame via static chain — externally visible */

      const Sym *sym = NULL;
      int64_t off;
      if (irop_get_tag(dest) == IROP_TAG_SYMREF)
      {
        IRPoolSymref *sr = irop_get_symref_ex(ir, dest);
        sym = sr ? sr->sym : NULL;
        off = sr ? sr->addend : 0;
      }
      else
      {
        off = irop_get_stack_offset(dest);
      }

      LOG_IR_GEN("DSE-SL: i=%d off=%lld sym=%p test=%d", i, (long long)off, (void *)sym, !!STACKLOC_TEST(sym, off));
      if (!STACKLOC_TEST(sym, off))
      {
        q->op = TCCIR_OP_NOP;
        changes++;
      }
    }

    /* Pass 3: Eliminate dead pointer chains from write-only addr-of.
     * If a write-only addr-TMP produced by Addr[StackLoc] feeds only
     * pointer writes, and the StackLoc range has no surviving reads,
     * NOP the LEA and forward-propagate: NOP instructions that use
     * only dead TMPs/VARs as sources or pointer bases (deref stores). */
    if (addr_tmp != NULL)
    {
      /* Find max VAR position for dead_var tracking */
      int max_var_pos = -1;
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        if (irop_config[q->op].has_dest)
        {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          int32_t vr = irop_get_vreg(d);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
          {
            int pos = TCCIR_DECODE_VREG_POSITION(vr);
            if (pos > max_var_pos)
              max_var_pos = pos;
          }
        }
      }

      uint8_t *dead_tmp = tcc_mallocz((max_tmp_stackloc + 8) / 8);
      uint8_t *dead_var = max_var_pos >= 0 ? tcc_mallocz((max_var_pos + 8) / 8) : NULL;

      /* Step 1: NOP LEA/Addr instructions for write-only addr-TMPs whose
       * StackLoc range is actually dead (no surviving reads). */
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_src1)
          continue;
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (!s.is_local || s.is_lval || irop_get_vreg(s) >= 0)
          continue;
        IROperand d = tcc_ir_op_get_dest(ir, q);
        int32_t dvr = irop_get_vreg(d);
        if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
          continue;
        int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
        if (dpos > max_tmp_stackloc)
          continue;
        if (!(addr_tmp[dpos / 8] & (1 << (dpos % 8))))
          continue;
        if (addr_tmp_read[dpos / 8] & (1 << (dpos % 8)))
          continue;

        /* Verify the pointed-to StackLoc range is dead: check if any offset
         * in [base_off, max_stackloc_off+4] is still marked as read.
         * This catches cases where different Addr instructions to the same
         * StackLoc produce both read and write-only TMPs. */
        const Sym *addr_sym = NULL;
        int64_t addr_off;
        if (irop_get_tag(s) == IROP_TAG_SYMREF)
        {
          IRPoolSymref *sr = irop_get_symref_ex(ir, s);
          addr_sym = sr ? sr->sym : NULL;
          addr_off = sr ? sr->addend : 0;
        }
        else
        {
          addr_off = irop_get_stack_offset(s);
        }
        int range_is_read = 0;
        int64_t range_end = max_stackloc_off + 4;
        int64_t range_len = range_end - addr_off + 1;
        if (range_len > STACKLOC_HASH_SIZE * 8)
        {
          range_is_read = 1; /* Too large — conservatively assume read */
        }
        else
        {
          for (int64_t k = addr_off; k <= range_end; k++)
          {
            if (STACKLOC_TEST(addr_sym, k))
            {
              range_is_read = 1;
              break;
            }
          }
        }
        if (range_is_read)
          continue;

        q->op = TCCIR_OP_NOP;
        changes++;
        dead_tmp[dpos / 8] |= (1 << (dpos % 8));
      }

      /* Step 2: Forward-propagate dead values through the pointer chain.
       * NOP instructions whose source values are all dead (defined only
       * by NOP'd instructions), then mark their dests as dead too. */
      int prop_changed = 1;
      while (prop_changed)
      {
        prop_changed = 0;
        for (int i = 0; i < n; i++)
        {
          IRQuadCompact *q = &ir->compact_instructions[i];
          if (q->op == TCCIR_OP_NOP)
            continue;
          /* Only propagate through data-flow instructions */
          if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP ||
              q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID ||
              q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID || q->op == TCCIR_OP_SWITCH_TABLE)
            continue;

          int has_dead_src = 0;

          if (irop_config[q->op].has_src1)
          {
            IROperand s = tcc_ir_op_get_src1(ir, q);
            int32_t vr = irop_get_vreg(s);
            if (vr >= 0)
            {
              int pos = TCCIR_DECODE_VREG_POSITION(vr);
              if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && pos <= max_tmp_stackloc &&
                  (dead_tmp[pos / 8] & (1 << (pos % 8))))
                has_dead_src = 1;
              else if (dead_var && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && pos <= max_var_pos &&
                       (dead_var[pos / 8] & (1 << (pos % 8))))
                has_dead_src = 1;
            }
          }

          /* For STORE / STORE_INDEXED: check if dest is a dead TMP/VAR
           * used as pointer base.  STORE_INDEXED has the same dest-as-base
           * semantics as STORE — the dest carries the address, src1 the
           * value — so the same dead-base check kills the store. */
          if (!has_dead_src && (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED))
          {
            IROperand d = tcc_ir_op_get_dest(ir, q);
            int32_t vr = irop_get_vreg(d);
            if (vr >= 0)
            {
              int pos = TCCIR_DECODE_VREG_POSITION(vr);
              if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && pos <= max_tmp_stackloc &&
                  (dead_tmp[pos / 8] & (1 << (pos % 8))))
                has_dead_src = 1;
            }
          }

          if (has_dead_src)
          {
            /* Mark dest as dead */
            if (irop_config[q->op].has_dest)
            {
              IROperand d = tcc_ir_op_get_dest(ir, q);
              int32_t dvr = irop_get_vreg(d);
              if (dvr >= 0)
              {
                int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
                if (TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP && dpos <= max_tmp_stackloc)
                  dead_tmp[dpos / 8] |= (1 << (dpos % 8));
                else if (dead_var && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR && dpos <= max_var_pos)
                  dead_var[dpos / 8] |= (1 << (dpos % 8));
              }
            }
            q->op = TCCIR_OP_NOP;
            changes++;
            prop_changed = 1;
          }
        }
      }

      tcc_free(dead_tmp);
      if (dead_var)
        tcc_free(dead_var);
    }

    if (addr_tmp)
      tcc_free(addr_tmp);
    if (addr_tmp_read)
      tcc_free(addr_tmp_read);

#undef STACKLOC_HASH_SIZE
#undef STACKLOC_HASH
#undef STACKLOC_SET
#undef STACKLOC_TEST
  skip_dead_stackloc:;
  }

  /* Second pass: dead TMP elimination after VAR + StackLoc store elimination.
   * The first TMP pass (above) cannot eliminate e.g. T0 in:
   *   T0 <-- #7 [LOAD]          (defines T0)
   *   StackLoc[-32] <-- T0      (only use of T0)
   * because the STORE still uses T0.  After StackLoc store elimination removes
   * the STORE, T0 is dead but the first TMP pass already finished.
   * Re-run the same iterative elimination to catch these cascading deaths. */
  {
    uint8_t *used2 = tcc_mallocz((max_tmp_pos + 8) / 8);
    int iter2_changes;
    do
    {
      iter2_changes = 0;
      memset(used2, 0, (max_tmp_pos + 8) / 8);
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        if (irop_config[q->op].has_src1)
        {
          const IROperand s = tcc_ir_op_get_src1(ir, q);
          if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
        }
        if (irop_config[q->op].has_src2)
        {
          const IROperand s = tcc_ir_op_get_src2(ir, q);
          if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
        }
        /* STORE/STORE_INDEXED dest is a use (pointer deref) */
        {
          const IROperand d = tcc_ir_op_get_dest(ir, q);
          if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) &&
              TCCIR_DECODE_VREG_TYPE(irop_get_vreg(d)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(d));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
          if (q->op == TCCIR_OP_FUNCPARAMVAL && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(d)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(d));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
        }
        if (q->op == TCCIR_OP_MLA)
        {
          const IROperand acc = tcc_ir_op_get_accum(ir, q);
          if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(acc)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(acc));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
        }
      }

      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        const IROperand dest = tcc_ir_op_get_dest(ir, q);
        if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP)
        {
          int is_dead_eligible =
              (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_STORE_POSTINC &&
               q->op != TCCIR_OP_LOAD_POSTINC && q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_FUNCCALLVAL &&
               q->op != TCCIR_OP_FUNCCALLVOID && q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID);
          if (!is_dead_eligible && q->op == TCCIR_OP_LOAD)
          {
            const IROperand src1 = tcc_ir_op_get_src1(ir, q);
            if (irop_is_immediate(src1))
              is_dead_eligible = 1;
          }
          if (is_dead_eligible)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest));
            if (pos <= max_tmp_pos && !(used2[pos / 8] & (1 << (pos % 8))))
            {
              q->op = TCCIR_OP_NOP;
              iter2_changes++;
            }
          }
        }
      }
      changes += iter2_changes;
    } while (iter2_changes > 0);
    tcc_free(used2);
  }

  return changes;
}

int tcc_ir_opt_dse_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_dse(ctx->ir);
}

/* Returns 1 if the only effect of `q` is the def of its dest — safe to NOP
 * when the dest is unread.  Mirrors DSE_IS_DEAD_ELIGIBLE in tcc_ir_opt_dse,
 * but tuned for VAR-dest ops where the dead-var consumer of a CALL/LOAD
 * chain is often a narrowing arithmetic op (AND/SHL/SHR/etc.) rather than
 * a plain ASSIGN.  Without this, e.g. `V = T & 0xFF` keeps T alive even
 * when V is unread, blocking the cascading kill of the producer call. */
static int ir_op_pure_for_dead_var_dest(TCCIRState *ir, IRQuadCompact *q)
{
  switch (q->op) {
  /* Side-effecting / control-flow / call-related — never safe to drop here. */
  case TCCIR_OP_NOP:
  case TCCIR_OP_STORE_INDEXED:
  case TCCIR_OP_STORE_POSTINC:
  case TCCIR_OP_LOAD_POSTINC:
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_FUNCPARAMVOID:
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
  case TCCIR_OP_IJUMP:
  case TCCIR_OP_SWITCH_TABLE:
  case TCCIR_OP_RETURNVOID:
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_SET_CHAIN:
  case TCCIR_OP_INIT_CHAIN_SLOT:
  case TCCIR_OP_INLINE_ASM:
  case TCCIR_OP_ASM_INPUT:
  case TCCIR_OP_ASM_OUTPUT:
  case TCCIR_OP_VLA_ALLOC:
  case TCCIR_OP_VLA_SP_SAVE:
  case TCCIR_OP_VLA_SP_RESTORE:
  case TCCIR_OP_PREFETCH:
  case TCCIR_OP_TRAP:
  case TCCIR_OP_SETJMP:
  case TCCIR_OP_LONGJMP:
  case TCCIR_OP_CALLSEQ_BEGIN:
  case TCCIR_OP_CALLARG_REG:
  case TCCIR_OP_CALLARG_STACK:
  case TCCIR_OP_CALLSEQ_END:
  case TCCIR_OP_CMP:
    return 0;
  case TCCIR_OP_LOAD: {
    /* Same safe-source set as DSE_IS_DEAD_ELIGIBLE: no MMIO / volatile risk. */
    IROperand s = tcc_ir_op_get_src1(ir, q);
    return irop_is_immediate(s) || s.is_sym ||
           (irop_get_vreg(s) <= -2 && irop_get_vreg(s) >= -9) ||
           (irop_get_tag(s) == IROP_TAG_STACKOFF && s.is_local);
  }
  case TCCIR_OP_STORE: {
    /* Only local stores (direct register writes) are safe — pointer stores
     * are observable. */
    IROperand d = tcc_ir_op_get_dest(ir, q);
    return d.is_local;
  }
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCCALLVOID: {
    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee) return 0;
    const char *name = get_tok_str(callee->v, NULL);
    return name && tcc_ir_is_pure_aeabi(name);
  }
  default:
    /* Pure arithmetic/bitwise/conversion: ADD, SUB, MUL, AND, OR, XOR, SHL,
     * SHR, SAR, ZEXT, UBFX, FADD/FSUB/..., CVT_*, LEA, SETIF, etc. */
    return 1;
  }
}

/* Dead address-taken VAR elimination.
 * After value tracking folds branches that read address-taken VARs (e.g.,
 * overflow builtin results), the ASSIGN + LEA + STORE sequences writing to
 * those VARs become dead. The regular DSE skips address-taken VARs, but this
 * pass can safely eliminate them by proving no live reads remain.
 *
 * A VAR is "dead" if:
 * 1. It's never read directly (not src1/src2 of any non-NOP, non-LEA instruction)
 * 2. All LEA pointers to it are only used as STORE destinations (write-only)
 */
int tcc_ir_opt_dead_var_store_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  for (int i = 0; i < n; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return 0;

  int max_var = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 0; k < 3; k++)
    {
      IROperand op = (k == 0)   ? tcc_ir_op_get_dest(ir, q)
                     : (k == 1) ? tcc_ir_op_get_src1(ir, q)
                                : tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(op);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos > max_var)
          max_var = pos;
      }
    }
  }
  if (max_var == 0)
    return 0;

  uint8_t *var_read = tcc_mallocz((max_var + 8) / 8);
  uint8_t *var_has_lea = tcc_mallocz((max_var + 8) / 8);
  int has_set_chain = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_SET_CHAIN || q->op == TCCIR_OP_INIT_CHAIN_SLOT)
      has_set_chain = 1;
    /* Track LEA instructions that take the address of a VAR */
    if (q->op == TCCIR_OP_LEA)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(src1);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          var_has_lea[pos / 8] |= (1 << (pos % 8));
      }
    }
    if (irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(src1);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          var_read[pos / 8] |= (1 << (pos % 8));
      }
    }
    if (irop_config[q->op].has_src2)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(src2);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          var_read[pos / 8] |= (1 << (pos % 8));
      }
    }
    /* STORE_INDEXED/STORE_POSTINC dest is a pointer read (base address),
     * not a definition.  Count it as a VAR read so the VAR's definition
     * isn't incorrectly eliminated. */
    if (q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(d);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          var_read[pos / 8] |= (1 << (pos % 8));
      }
    }
  }

  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    /* Any pure op writing a VAR may be dropped when the VAR is unread.
     * Broader than just ASSIGN/STORE so e.g. `V = T & 0xFF` (byte truncation
     * of a CALL result into a dead local) gets killed, which lets the
     * downstream call_result→DCE cascade reach the producer call. */
    if (!ir_op_pure_for_dead_var_dest(ir, q))
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(dest);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos > max_var)
      continue;
    if (var_read[pos / 8] & (1 << (pos % 8)))
      continue;
    /* If addrtaken, check whether the VAR could actually be read through
     * a pointer.  Two cases:
     * 1) LEA exists for this VAR — pointer alias is live, skip.
     * 2) SET_CHAIN exists — explicit nested call in this function makes
     *    captured VARs reachable via the static chain.
     * If neither applies, the addrtaken flag is a stale frontend
     * annotation (e.g. capture by a now-fully-inlined nested function)
     * and the VAR is safe to eliminate. */
    {
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vr);
      if (interval && interval->addrtaken &&
          ((var_has_lea[pos / 8] & (1 << (pos % 8))) || has_set_chain))
        continue;
    }
    LOG_IR_GEN("=== DEAD VAR STORE: eliminating V%d at i=%d (op=%d) ===", pos, i, q->op);
    q->op = TCCIR_OP_NOP;
    changes++;
  }

  LOG_IR_GEN("=== DEAD VAR STORE ELIM: eliminated %d dead VAR stores ===", changes);
  tcc_free(var_has_lea);
  tcc_free(var_read);
  return changes;
}

/* Fold ADD-immediate + DEREF into LOAD_INDEXED with offset.
 *
 * Pattern:  T = base ADD #imm          (T is single-use TEMP)
 *           ... T***DEREF***            (only use, as lval/deref)
 *
 * Becomes:  T = LOAD_INDEXED base, #imm, scale=0   (T = *(base + imm))
 *           ... T                       (plain value, deref cleared)
 *
 * This lets the codegen emit `ldr Rd, [Rbase, #imm]` instead of
 * `add Rt, Rbase, #imm; ldr Rd, [Rt, #0]`, saving one instruction
 * and one register. */

int tcc_ir_opt_dead_addrvar_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* Find max VAR and TMP positions */
  int max_var = 0, max_tmp = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
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
        if (type == TCCIR_VREG_TYPE_VAR && pos > max_var)
          max_var = pos;
        else if (type == TCCIR_VREG_TYPE_TEMP && pos > max_tmp)
          max_tmp = pos;
      }
    }
  }

  if (max_var == 0)
    return 0;

  uint8_t *var_read = tcc_mallocz((max_var + 8) / 8);
  uint8_t *var_has_lea = tcc_mallocz((max_var + 8) / 8);
  int *lea_map = tcc_mallocz(sizeof(int) * (max_tmp + 1));
  int *var_lea = tcc_mallocz(sizeof(int) * (max_var + 1));
  for (int i = 0; i <= max_tmp; i++)
    lea_map[i] = -1;
  for (int i = 0; i <= max_var; i++)
    var_lea[i] = -1;

  /* Pass 1: Build LEA map and mark directly-read VARs.
   * LEA src1 (address-take) is NOT a value read.
   * STORE dest (pointer) is NOT a value read of the pointed-to VAR.
   * Everything else that references a VAR as src1/src2 is a value read. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (q->op == TCCIR_OP_LEA)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t d_vr = irop_get_vreg(dest);
      int32_t s_vr = irop_get_vreg(src1);
      if (s_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int var_pos = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
        {
          /* LEA with TMP dest: trackable — record in lea_map */
          int tmp_pos = TCCIR_DECODE_VREG_POSITION(d_vr);
          if (tmp_pos <= max_tmp && var_pos <= max_var)
          {
            lea_map[tmp_pos] = var_pos;
            var_has_lea[var_pos / 8] |= (1 << (var_pos % 8));
          }
        }
        else if (var_pos <= max_var)
        {
          /* LEA with VAR dest: pointer escapes into a VAR, conservatively mark as read */
          var_read[var_pos / 8] |= (1 << (var_pos % 8));
        }
      }
      continue;
    }

    /* LEA propagation through VARs: STORE V = T where T is LEA result */
    if (q->op == TCCIR_OP_STORE)
    {
      IROperand dest_op = tcc_ir_op_get_dest(ir, q);
      IROperand src1_op = tcc_ir_op_get_src1(ir, q);
      int32_t d_vr = irop_get_vreg(dest_op);
      int32_t s_vr = irop_get_vreg(src1_op);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR && s_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int d_pos = TCCIR_DECODE_VREG_POSITION(d_vr);
        int s_tmp = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (d_pos <= max_var && s_tmp <= max_tmp && lea_map[s_tmp] >= 0)
          var_lea[d_pos] = lea_map[s_tmp];
      }
    }

    /* LEA propagation: ASSIGN T = V where V holds a LEA result */
    if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD)
    {
      IROperand dest_op = tcc_ir_op_get_dest(ir, q);
      IROperand src1_op = tcc_ir_op_get_src1(ir, q);
      int32_t d_vr = irop_get_vreg(dest_op);
      int32_t s_vr = irop_get_vreg(src1_op);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP && s_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int d_tmp = TCCIR_DECODE_VREG_POSITION(d_vr);
        int s_pos = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (d_tmp <= max_tmp && s_pos <= max_var && var_lea[s_pos] >= 0)
          lea_map[d_tmp] = var_lea[s_pos];
      }
    }

    /* Mark VARs read as src1 (for all instructions including STORE) */
    if (irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(src1);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          var_read[pos / 8] |= (1 << (pos % 8));
      }
    }

    /* Mark VARs read as src2 */
    if (irop_config[q->op].has_src2)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(src2);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          var_read[pos / 8] |= (1 << (pos % 8));
      }
    }
  }

  /* Pass 2: Mark VARs whose LEA pointers escape (used outside STORE dest).
   * If a LEA TMP appears as src1/src2 of any instruction, the VAR's address
   * may be used to read the VAR elsewhere — mark it as read.
   * Exception: STORE V = T (pointer copy to VAR) is tracked in Pass 1 and
   * does not constitute a read of the pointed-to VAR. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(src1);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int tmp_pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (tmp_pos <= max_tmp && lea_map[tmp_pos] >= 0)
        {
          int is_ptr_copy = 0;
          if (q->op == TCCIR_OP_STORE)
          {
            IROperand d = tcc_ir_op_get_dest(ir, q);
            int32_t d_vr = irop_get_vreg(d);
            if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR)
              is_ptr_copy = 1;
          }
          if (!is_ptr_copy)
          {
            int var_pos = lea_map[tmp_pos];
            if (var_pos <= max_var)
              var_read[var_pos / 8] |= (1 << (var_pos % 8));
          }
        }
      }
    }

    if (irop_config[q->op].has_src2)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(src2);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int tmp_pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (tmp_pos <= max_tmp && lea_map[tmp_pos] >= 0)
        {
          int var_pos = lea_map[tmp_pos];
          if (var_pos <= max_var)
            var_read[var_pos / 8] |= (1 << (var_pos % 8));
        }
      }
    }
  }

  /* Pass 2b: Mark VARs as read when their pointer escapes via FUNCPARAMVAL.
   * If a VAR in var_lea is passed as a function argument, the pointer
   * escapes to the callee which may read through it. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCPARAMVAL)
      continue;
    IROperand param_val = tcc_ir_op_get_src1(ir, q);
    int32_t vr = irop_get_vreg(param_val);
    if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos <= max_var && var_lea[pos] >= 0)
      {
        int target = var_lea[pos];
        if (target <= max_var)
          var_read[target / 8] |= (1 << (target % 8));
      }
    }
    if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int tmp_pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (tmp_pos <= max_tmp && lea_map[tmp_pos] >= 0)
      {
        int target = lea_map[tmp_pos];
        if (target <= max_var)
          var_read[target / 8] |= (1 << (target % 8));
      }
    }
  }

  /* Pass 3: Eliminate dead writes to unread VARs */
  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* ASSIGN to dead VAR -> NOP (only if VAR has LEA, proving we track all accesses) */
    if (q->op == TCCIR_OP_ASSIGN && irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var && (var_has_lea[pos / 8] & (1 << (pos % 8))) && !(var_read[pos / 8] & (1 << (pos % 8))))
        {
          q->op = TCCIR_OP_NOP;
          changes++;
        }
      }
    }

    /* LEA from dead VAR -> NOP (only for TMP destinations; VAR dests handled by regular DSE) */
    if (q->op == TCCIR_OP_LEA)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t d_vr = irop_get_vreg(dest);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        int32_t vr = irop_get_vreg(src1);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (pos <= max_var && (var_has_lea[pos / 8] & (1 << (pos % 8))) && !(var_read[pos / 8] & (1 << (pos % 8))))
          {
            q->op = TCCIR_OP_NOP;
            changes++;
          }
        }
      }
    }

    /* STORE through LEA to dead VAR -> NOP */
    if (q->op == TCCIR_OP_STORE)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int tmp_pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (tmp_pos <= max_tmp && lea_map[tmp_pos] >= 0)
        {
          int var_pos = lea_map[tmp_pos];
          if (var_pos <= max_var && !(var_read[var_pos / 8] & (1 << (var_pos % 8))))
          {
            q->op = TCCIR_OP_NOP;
            changes++;
          }
        }
      }
    }
  }

  /* STORE to dead VAR (non-deref): V = val where V is unread */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(dest);
    if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos <= max_var && (var_has_lea[pos / 8] & (1 << (pos % 8))) && !(var_read[pos / 8] & (1 << (pos % 8))))
      {
        q->op = TCCIR_OP_NOP;
        changes++;
      }
    }
  }

  LOG_IR_GEN("=== DEAD ADDRVAR ELIM: eliminated %d dead writes ===", changes);

  tcc_free(var_lea);
  tcc_free(lea_map);
  tcc_free(var_has_lea);
  tcc_free(var_read);
  return changes;
}

/* Trailing-dead-store elimination for addr-taken VARs.
 *
 * `dead_addrvar_elim` only fires when a VAR has *zero* reads anywhere.  This
 * misses the common pattern of a final write that happens after the last
 * read — e.g. `*p = n` at function tail where `*p` is never re-read.
 *
 * For each addr-taken VAR V, computes last_read_pos[V] = max position of a
 * read of V (direct VAR-src use, or LOAD/CMP/... via a TEMP T where T = &V).
 * Then NOPs any write to V (direct ASSIGN/STORE V=x, or STORE through a LEA
 * TEMP T where lea_map[T] = V) at position > last_read_pos[V].
 *
 * Conservative function-wide bails:
 *   - any CALL / PARAM: callee may dereference a leaked &V.
 *   - any IJUMP / SETJMP / LONGJMP / NL_SETJMP / NL_LONGJMP / INLINE_ASM /
 *     SET_CHAIN / INIT_CHAIN_SLOT / SWITCH_TABLE.
 *   - any back-edge JUMP/JUMPIF (target <= origin): a write past last_read
 *     could be re-executed via a loop before V is re-read.
 *
 * Per-VAR bails:
 *   - LEA temp escapes via STORE-as-value / VAR-dest / etc. (var_escaped).
 */
int tcc_ir_opt_dead_trailing_addrvar_store_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  for (int i = 0; i < n; i++)
  {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_FUNCCALLVOID ||
        op == TCCIR_OP_FUNCPARAMVAL || op == TCCIR_OP_FUNCPARAMVOID ||
        op == TCCIR_OP_IJUMP || op == TCCIR_OP_SETJMP || op == TCCIR_OP_LONGJMP ||
        op == TCCIR_OP_NL_SETJMP || op == TCCIR_OP_NL_LONGJMP ||
        op == TCCIR_OP_INLINE_ASM || op == TCCIR_OP_SET_CHAIN ||
        op == TCCIR_OP_INIT_CHAIN_SLOT || op == TCCIR_OP_SWITCH_TABLE)
      return 0;
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int target = (int)irop_get_imm64_ex(ir, dest);
    if (target <= i)
      return 0;
  }

  int max_var = 0, max_tmp = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    IROperand ops[3];
    ops[0] = tcc_ir_op_get_dest(ir, q);
    ops[1] = tcc_ir_op_get_src1(ir, q);
    ops[2] = tcc_ir_op_get_src2(ir, q);
    for (int k = 0; k < 3; k++)
    {
      int32_t vr = irop_get_vreg(ops[k]);
      if (vr < 0)
        continue;
      int t = TCCIR_DECODE_VREG_TYPE(vr);
      int p = TCCIR_DECODE_VREG_POSITION(vr);
      if (t == TCCIR_VREG_TYPE_VAR && p > max_var) max_var = p;
      else if (t == TCCIR_VREG_TYPE_TEMP && p > max_tmp) max_tmp = p;
    }
  }
  if (max_var == 0)
    return 0;

  int *lea_map = tcc_malloc(sizeof(int) * (max_tmp + 1));
  for (int i = 0; i <= max_tmp; i++) lea_map[i] = -1;
  int *var_lea = tcc_malloc(sizeof(int) * (max_var + 1));
  for (int i = 0; i <= max_var; i++) var_lea[i] = -1;
  uint8_t *var_escaped = tcc_mallocz((max_var + 8) / 8);
  int *var_last_read = tcc_malloc(sizeof(int) * (max_var + 1));
  for (int i = 0; i <= max_var; i++) var_last_read[i] = -1;

  /* Pass 1: build lea_map (T = &V → lea_map[T] = V) and var_lea (V' = &V →
   * var_lea[V'] = V).  Also propagate through TEMP↔VAR copies (STORE V=T,
   * ASSIGN/LOAD T=V). */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (q->op == TCCIR_OP_LEA)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t s_vr = irop_get_vreg(src1);
      int32_t d_vr = irop_get_vreg(dest);
      if (s_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_VAR && d_vr >= 0)
      {
        int v = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (v <= max_var)
        {
          if (TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
          {
            int t = TCCIR_DECODE_VREG_POSITION(d_vr);
            if (t <= max_tmp)
            {
              if (lea_map[t] >= 0 && lea_map[t] != v)
                var_escaped[v / 8] |= (1 << (v % 8));
              lea_map[t] = v;
            }
          }
          else if (TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR)
          {
            int d_pos = TCCIR_DECODE_VREG_POSITION(d_vr);
            if (d_pos <= max_var)
            {
              if (var_lea[d_pos] >= 0 && var_lea[d_pos] != v)
                var_escaped[v / 8] |= (1 << (v % 8));
              var_lea[d_pos] = v;
            }
          }
        }
      }
      continue;
    }

    /* STORE V = T (TEMP src holding LEA) → propagate lea_map → var_lea.
     * (mirrors dead_addrvar_elim's STORE V=T-from-LEA-result propagation;
     * is_lval flags are intentionally not checked — see same pass for
     * rationale: the ASSIGN/STORE/LOAD ops use is_lval to indicate fetch
     * semantics, not pointer/value distinction.) */
    if (q->op == TCCIR_OP_STORE)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t d_vr = irop_get_vreg(dest);
      int32_t s_vr = irop_get_vreg(src1);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR &&
          s_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int d_pos = TCCIR_DECODE_VREG_POSITION(d_vr);
        int s_tmp = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (d_pos <= max_var && s_tmp <= max_tmp && lea_map[s_tmp] >= 0)
        {
          if (var_lea[d_pos] >= 0 && var_lea[d_pos] != lea_map[s_tmp])
            var_escaped[lea_map[s_tmp] / 8] |= (1 << (lea_map[s_tmp] % 8));
          var_lea[d_pos] = lea_map[s_tmp];
        }
      }
    }

    /* ASSIGN/LOAD T = V (VAR src holding LEA) → propagate var_lea → lea_map. */
    if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t d_vr = irop_get_vreg(dest);
      int32_t s_vr = irop_get_vreg(src1);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP &&
          s_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int d_tmp = TCCIR_DECODE_VREG_POSITION(d_vr);
        int s_pos = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (d_tmp <= max_tmp && s_pos <= max_var && var_lea[s_pos] >= 0)
        {
          if (lea_map[d_tmp] >= 0 && lea_map[d_tmp] != var_lea[s_pos])
            var_escaped[var_lea[s_pos] / 8] |= (1 << (var_lea[s_pos] % 8));
          lea_map[d_tmp] = var_lea[s_pos];
        }
      }
    }
  }

  /* Pass 2: scan uses of LEA TEMPs to detect non-tracked escapes.
   * Allowed uses of a TEMP T with lea_map[T] = V:
   *   - STORE/STORE_INDEXED/STORE_POSTINC dest = T-deref  (write to V)
   *   - LOAD src1 = T-deref                                (read of V)
   *   - CMP/TEST_ZERO src1/src2 with T or T-deref          (read of V)
   *   - ASSIGN-into-TEMP/LEA-into-TEMP propagation         (handled later)
   * Anything else (STORE src1 = T as value, ASSIGN-into-VAR src1 = T, etc.)
   * is an escape. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* STORE: src1 = T (non-lval) where T is a LEA temp → escape (storing
     * the pointer value). */
    if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
         q->op == TCCIR_OP_STORE_POSTINC) && irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      if (!src1.is_lval)
      {
        int32_t vr = irop_get_vreg(src1);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
        {
          int t = TCCIR_DECODE_VREG_POSITION(vr);
          if (t <= max_tmp && lea_map[t] >= 0)
            var_escaped[lea_map[t] / 8] |= (1 << (lea_map[t] % 8));
        }
      }
    }
    /* RETURNVALUE src1 = T → escape (returning pointer to local) */
    if (q->op == TCCIR_OP_RETURNVALUE)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(src1);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int t = TCCIR_DECODE_VREG_POSITION(vr);
        if (t <= max_tmp && lea_map[t] >= 0)
          var_escaped[lea_map[t] / 8] |= (1 << (lea_map[t] % 8));
      }
    }
  }

  /* Pass 3: record last_read[V].  Direct VAR src reads, and LOADs/CMPs
   * via a known LEA TEMP, both count. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    for (int k = 0; k < 2; k++)
    {
      int has = (k == 0) ? irop_config[q->op].has_src1 : irop_config[q->op].has_src2;
      if (!has)
        continue;
      IROperand s = (k == 0) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(s);
      if (vr < 0)
        continue;
      int vt = TCCIR_DECODE_VREG_TYPE(vr);
      int vp = TCCIR_DECODE_VREG_POSITION(vr);
      if (vt == TCCIR_VREG_TYPE_VAR && vp <= max_var)
      {
        /* Direct VAR read. Even STORE src1 of `STORE dest <- V` reads V. */
        if (i > var_last_read[vp])
          var_last_read[vp] = i;
      }
      else if (vt == TCCIR_VREG_TYPE_TEMP && vp <= max_tmp && lea_map[vp] >= 0 && s.is_lval)
      {
        /* lval deref of LEA TEMP → read of V's memory. */
        int v = lea_map[vp];
        if (v <= max_var && i > var_last_read[v])
          var_last_read[v] = i;
      }
    }
  }

  /* Pass 4: NOP writes to V at position > last_read[V].
   * Two forms:
   *   - direct write: dest = V (non-lval), op pure (ASSIGN, etc.)
   *   - STORE dest = T-deref where T is a LEA TEMP, lea_map[T] = V */
  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    int written_var = -1;
    if (irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && !dest.is_lval)
      {
        /* Direct write. Conservative: only ASSIGN/STORE shapes — skip ops
         * with possible side effects. */
        if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_STORE ||
            q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ADD ||
            q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_AND || q->op == TCCIR_OP_OR ||
            q->op == TCCIR_OP_XOR || q->op == TCCIR_OP_MUL || q->op == TCCIR_OP_SHL ||
            q->op == TCCIR_OP_SHR || q->op == TCCIR_OP_SAR || q->op == TCCIR_OP_ZEXT)
          written_var = TCCIR_DECODE_VREG_POSITION(vr);
      }
      else if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
                q->op == TCCIR_OP_STORE_POSTINC) &&
               vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        /* STORE always has dest.is_lval=1.  STORE_INDEXED/POSTINC may have
         * is_lval cleared by disp_fusion — the op itself implies a memory
         * write so treat the dest as a write address regardless. */
        int t = TCCIR_DECODE_VREG_POSITION(vr);
        if (t <= max_tmp && lea_map[t] >= 0)
          written_var = lea_map[t];
      }
    }

    if (written_var < 0 || written_var > max_var)
      continue;
    if (var_escaped[written_var / 8] & (1 << (written_var % 8)))
      continue;
    int last_read = var_last_read[written_var];
    if (last_read < 0)
      continue; /* never read — dead_addrvar handles full elimination */
    if (i <= last_read)
      continue;

    LOG_IR_GEN("=== DEAD TRAILING ADDRVAR STORE: NOP i=%d (V=%d, last_read=%d) ===", i,
               written_var, last_read);
    q->op = TCCIR_OP_NOP;
    changes++;
  }

  tcc_free(var_last_read);
  tcc_free(var_escaped);
  tcc_free(var_lea);
  tcc_free(lea_map);
  return changes;
}

int tcc_ir_opt_dead_trailing_addrvar_store_elim_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_dead_trailing_addrvar_store_elim(ctx->ir);
}

/* Redundant VAR ASSIGN elimination.
 * Forward scan within basic blocks: if a VAR is assigned and then assigned
 * again without being read in between, the first assign is dead → NOP it.
 * This catches patterns like repeated overflow flag stores where earlier
 * values are overwritten before use.
 */
int tcc_ir_opt_redundant_var_assign(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* Find max VAR position */
  int max_var = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    IROperand ops[3];
    ops[0] = tcc_ir_op_get_dest(ir, q);
    ops[1] = tcc_ir_op_get_src1(ir, q);
    ops[2] = tcc_ir_op_get_src2(ir, q);
    for (int k = 0; k < 3; k++)
    {
      int32_t vr = irop_get_vreg(ops[k]);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos > max_var)
          max_var = pos;
      }
    }
  }

  if (max_var == 0)
    return 0;

  /* Mark jump targets as merge points — must flush pending at these */
  uint8_t *is_target = tcc_mallocz((n + 7) / 8);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)dest.u.imm32;
      if (target >= 0 && target < n)
        is_target[target / 8] |= (1 << (target % 8));
    }
  }

  /* pending[v] = instruction index of last unread ASSIGN to VAR v, or -1 */
  int *pending = tcc_malloc(sizeof(int) * (max_var + 1));
  for (int v = 0; v <= max_var; v++)
    pending[v] = -1;

  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Flush at merge points (jump targets) */
    if (is_target[i / 8] & (1 << (i % 8)))
    {
      for (int v = 0; v <= max_var; v++)
        pending[v] = -1;
    }

    /* Block boundary: flush all pending */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID ||
        q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_SWITCH_TABLE)
    {
      /* First process reads in this instruction (src1/src2) */
      if (irop_config[q->op].has_src1)
      {
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        int32_t vr = irop_get_vreg(src1);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (pos <= max_var)
            pending[pos] = -1;
        }
      }
      /* Flush all */
      for (int v = 0; v <= max_var; v++)
        pending[v] = -1;
      continue;
    }

    /* Process reads: src1 and src2 clear pending for read VARs */
    if (irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(src1);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          pending[pos] = -1;
      }
    }
    if (irop_config[q->op].has_src2)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(src2);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          pending[pos] = -1;
      }
    }

    /* STORE dest is a pointer USE — if it's a VAR, count as read */
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          pending[pos] = -1;
      }
      continue;
    }

    /* Process write: if dest is VAR, check for redundant prior assign */
    if (irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
        {
          if (pending[pos] >= 0)
          {
            /* Previous assign to this VAR is dead — overwritten before read */
            ir->compact_instructions[pending[pos]].op = TCCIR_OP_NOP;
            changes++;
          }
          pending[pos] = i;
        }
      }
    }
  }

  LOG_IR_GEN("=== REDUNDANT VAR ASSIGN: eliminated %d dead assigns ===", changes);

  tcc_free(pending);
  tcc_free(is_target);
  return changes;
}

/* vrp_swap_cmp_tok now in opt_utils.h */


int tcc_ir_opt_redundant_init_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n <= 1)
    return 0;

  /* Bail if function has indirect jumps (setjmp/longjmp, computed goto).
   * These create hidden control flow that our BFS doesn't follow. */
  for (int i = 0; i < n; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return 0;
  }

  /* Find function-entry VAR inits: instructions before any jump target
   * that assign a constant to a VAR. */
  for (int init_idx = 0; init_idx < n; init_idx++)
  {
    IRQuadCompact *q = &ir->compact_instructions[init_idx];
    if (q->is_jump_target)
      break;
    if (q->op != TCCIR_OP_ASSIGN)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(dest);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
      continue;

    IROperand src = tcc_ir_op_get_src1(ir, q);
    if (!irop_is_immediate(src))
      continue;

    IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vr);
    if (interval && interval->addrtaken)
      continue;

    /* Forward reachability: check if V is killed on all paths before use.
     * State per instruction: 0=unvisited, 1=V-alive (init not yet killed),
     * 2=V-killed (redef seen on this path). */
    uint8_t *state = tcc_mallocz(n);
    int found_use_before_kill = 0;

    /* Worklist-based BFS from init_idx+1 */
    int *worklist = tcc_malloc(n * sizeof(int));
    int wl_head = 0, wl_tail = 0;
    worklist[wl_tail++] = init_idx + 1;
    state[init_idx] = 1;

    while (wl_head < wl_tail && !found_use_before_kill)
    {
      int idx = worklist[wl_head++];
      if (idx < 0 || idx >= n)
        continue;
      if (state[idx] == 2)
        continue; /* already killed on this path */
      if (state[idx] == 1)
        continue;     /* already queued as alive */
      state[idx] = 1; /* mark as V-alive */

      IRQuadCompact *iq = &ir->compact_instructions[idx];
      if (iq->op == TCCIR_OP_NOP)
      {
        if (idx + 1 < n)
          worklist[wl_tail++] = idx + 1;
        continue;
      }

      /* Check if this instruction USES V */
      if (irop_config[iq->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, iq)) == vr)
      {
        found_use_before_kill = 1;
        break;
      }
      if (irop_config[iq->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, iq)) == vr)
      {
        found_use_before_kill = 1;
        break;
      }
      /* STORE/FUNCPARAMVAL dest is a use */
      if ((iq->op == TCCIR_OP_STORE || iq->op == TCCIR_OP_STORE_INDEXED || iq->op == TCCIR_OP_FUNCPARAMVAL) &&
          irop_get_vreg(tcc_ir_op_get_dest(ir, iq)) == vr)
      {
        found_use_before_kill = 1;
        break;
      }

      /* Check if this instruction DEFINES (kills) V.
       * Only treat as kill if the source is explicit (immediate or vreg).
       * Bare defs like ASM outputs (V <-- with no visible source) may
       * implicitly depend on V's prior value via register constraints. */
      if (irop_config[iq->op].has_dest && iq->op != TCCIR_OP_STORE && iq->op != TCCIR_OP_STORE_INDEXED &&
          iq->op != TCCIR_OP_FUNCPARAMVAL)
      {
        IROperand d = tcc_ir_op_get_dest(ir, iq);
        if (irop_get_vreg(d) == vr)
        {
          int has_explicit_src = 0;
          if (irop_config[iq->op].has_src1)
          {
            IROperand s = tcc_ir_op_get_src1(ir, iq);
            if (irop_is_immediate(s) || irop_has_vreg(s))
              has_explicit_src = 1;
          }
          if (has_explicit_src)
          {
            state[idx] = 2; /* killed */
            continue;       /* don't follow successors — V is dead on this path */
          }
        }
      }

      /* Follow successors */
      if (iq->op == TCCIR_OP_JUMP)
      {
        IROperand jd = tcc_ir_op_get_dest(ir, iq);
        int target = (int)irop_get_imm64_ex(ir, jd);
        if (target >= 0 && target < n && state[target] == 0)
          worklist[wl_tail++] = target;
      }
      else if (iq->op == TCCIR_OP_JUMPIF)
      {
        IROperand jd = tcc_ir_op_get_dest(ir, iq);
        int target = (int)irop_get_imm64_ex(ir, jd);
        if (target >= 0 && target < n && state[target] == 0)
          worklist[wl_tail++] = target;
        if (idx + 1 < n && state[idx + 1] == 0)
          worklist[wl_tail++] = idx + 1;
      }
      else if (iq->op == TCCIR_OP_RETURNVALUE)
      {
        /* Return with V alive but unused = V is dead (return doesn't use V
         * as an argument here since we checked src1 above) */
      }
      else
      {
        if (idx + 1 < n && state[idx + 1] == 0)
          worklist[wl_tail++] = idx + 1;
      }
    }

    tcc_free(worklist);
    tcc_free(state);

    if (!found_use_before_kill)
    {
      q->op = TCCIR_OP_NOP;
      changes++;
    }
  }

  return changes;
}


int tcc_ir_opt_dead_loop_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n == 0)
    return 0;

  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops || loops->num_loops == 0)
  {
    tcc_ir_free_loops(loops);
    return 0;
  }

  for (int li = 0; li < loops->num_loops; li++)
  {
    IRLoop *loop = &loops->loops[li];
    if (loop->num_body_instrs == 0)
      continue;

    int has_side_effects = 0;
    int num_const_assigns = 0; (void)num_const_assigns;
    int has_loop_counter = 0;
    int has_self_stores = 0;

    /* Track which VARs get constant assignments inside the loop body */
    typedef struct
    {
      int var_pos;
      int64_t value;
      int btype;
    } ConstVar;
    ConstVar const_vars[8];
    int num_const_vars = 0;

    for (int idx = loop->start_idx; idx <= loop->end_idx && idx < n; idx++)
    {
      IRQuadCompact *q = &ir->compact_instructions[idx];

      if (q->op == TCCIR_OP_NOP)
        continue;
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
        continue;
      if (q->op == TCCIR_OP_CMP || q->op == TCCIR_OP_TEST_ZERO)
        continue;

      /* Calls are side effects */
      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
      {
        has_side_effects = 1;
        break;
      }

      /* Stores to memory are side effects, with one exception: a local stack
       * STORE that copies a value back to the slot it was just loaded from
       * (`T = *p; *p = T;`) is observably a no-op.  This pattern appears in
       * inlined struct copies (e.g. CCID's `a = x` after CPOW's body has been
       * DCE'd) — the alias-conservative DSE passes won't kill it on their
       * own, so handle it here. */
      if (q->op == TCCIR_OP_STORE)
      {
        IROperand sdest = tcc_ir_op_get_dest(ir, q);
        IROperand ssrc = tcc_ir_op_get_src1(ir, q);
        int is_self_store = 0;
        if (sdest.is_local && sdest.is_lval && irop_get_tag(ssrc) == IROP_TAG_VREG && !ssrc.is_lval)
        {
          int32_t src_temp = irop_get_vreg(ssrc);
          if (src_temp >= 0 && TCCIR_DECODE_VREG_TYPE(src_temp) == TCCIR_VREG_TYPE_TEMP)
          {
            int dest_off = (int)irop_get_imm64_ex(ir, sdest);
            for (int j = idx - 1; j >= loop->start_idx; j--)
            {
              IRQuadCompact *p = &ir->compact_instructions[j];
              if (p->op == TCCIR_OP_NOP)
                continue;
              if (p->op == TCCIR_OP_JUMP || p->op == TCCIR_OP_JUMPIF)
                break;
              if (p->op == TCCIR_OP_FUNCCALLVAL || p->op == TCCIR_OP_FUNCCALLVOID)
                break;
              if (p->op == TCCIR_OP_STORE || p->op == TCCIR_OP_STORE_INDEXED)
              {
                IROperand pd = tcc_ir_op_get_dest(ir, p);
                if (!pd.is_local)
                  break;
                int po = (int)irop_get_imm64_ex(ir, pd);
                if (po == dest_off)
                  break;
                continue;
              }
              if (p->op == TCCIR_OP_LOAD)
              {
                IROperand pd = tcc_ir_op_get_dest(ir, p);
                IROperand ps = tcc_ir_op_get_src1(ir, p);
                if (irop_get_vreg(pd) == src_temp && ps.is_local && ps.is_lval)
                {
                  int po = (int)irop_get_imm64_ex(ir, ps);
                  if (po == dest_off)
                  {
                    is_self_store = 1;
                    break;
                  }
                }
              }
            }
          }
        }
        if (is_self_store)
        {
          has_self_stores = 1;
          continue;
        }
        has_side_effects = 1;
        break;
      }
      if (q->op == TCCIR_OP_STORE_INDEXED)
      {
        has_side_effects = 1;
        break;
      }

      /* PARAM instructions (function call setup) */
      if (q->op == TCCIR_OP_FUNCPARAMVOID || q->op == TCCIR_OP_FUNCPARAMVAL)
        continue;

      /* VAR <- immediate constant assignment: safe, track it */
      if (q->op == TCCIR_OP_ASSIGN)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        int32_t dest_vr = irop_get_vreg(dest);

        if (TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_VAR && irop_is_immediate(src1))
        {
          int var_pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
          int64_t val = irop_get_imm64_ex(ir, src1);
          int btype = irop_get_btype(src1);

          /* Check if we already track this VAR */
          int found = 0;
          for (int vi = 0; vi < num_const_vars; vi++)
          {
            if (const_vars[vi].var_pos == var_pos)
            {
              if (const_vars[vi].value != val)
              {
                has_side_effects = 1; /* Different values on different paths */
              }
              found = 1;
              break;
            }
          }
          if (has_side_effects)
            break;
          if (!found && num_const_vars < 8)
          {
            const_vars[num_const_vars].var_pos = var_pos;
            const_vars[num_const_vars].value = val;
            const_vars[num_const_vars].btype = btype;
            num_const_vars++;
          }
          num_const_assigns++;
          continue;
        }

        /* TMP <- anything (loop counter, etc): OK, no side effect */
        if (TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP)
          continue;
      }

      /* ADD/SUB on TMPs or dead VARs (loop counters): safe.
       * A VAR modified by ADD/SUB is safe only if not used after the loop
       * (just a dead counter). If used after the loop, it's meaningful
       * accumulation (like sum += 1) and the loop is NOT dead. */
      if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && irop_is_immediate(tcc_ir_op_get_src2(ir, q)))
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        int32_t dest_vr = irop_get_vreg(dest);
        if (TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP)
        {
          has_loop_counter = 1;
          continue;
        }
        if (TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_VAR)
        {
          int var_used_after = 0;
          for (int post = loop->end_idx + 1; post < n; post++)
          {
            IRQuadCompact *pq = &ir->compact_instructions[post];
            if (pq->op == TCCIR_OP_NOP)
              continue;
            if (irop_config[pq->op].has_src1)
            {
              int32_t s1 = irop_get_vreg(tcc_ir_op_get_src1(ir, pq));
              if (s1 == dest_vr)
              {
                var_used_after = 1;
                break;
              }
            }
            if (irop_config[pq->op].has_src2)
            {
              int32_t s2 = irop_get_vreg(tcc_ir_op_get_src2(ir, pq));
              if (s2 == dest_vr)
              {
                var_used_after = 1;
                break;
              }
            }
          }
          if (var_used_after)
          {
            has_side_effects = 1;
            break;
          }
          has_loop_counter = 1;
          continue;
        }
      }

      /* LOAD of a VAR (reading the result): safe */
      if (q->op == TCCIR_OP_LOAD)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        if (!dest.is_lval)
          continue;
      }

      /* Anything else is a potential side effect */
      has_side_effects = 1;
      break;
    }

    /* Require either a constant VAR assignment in the body OR at least one
     * self-store (load-then-store-back of the same slot, no observable effect).
     * A bare loop with only a counter and forward jumps doesn't qualify — it
     * may be a switch-bounds-check loop or other non-loop CFG quirk that
     * happens to look like a back-edge to the loop detector. */
    if (has_side_effects || !has_loop_counter)
      continue;
    if (num_const_vars == 0 && !has_self_stores)
      continue;

    /* The loop body only contains constant VAR assignments, counter updates,
     * and/or self-stores (`*p = *p`).  NOP all body instructions and place
     * any constant assignments in the preheader. */
    LOG_IR_GEN("OPTIMIZE: Dead loop elimination at header=%d (%d const vars)", loop->header_idx, num_const_vars);

    /* NOP loop body instructions within [start_idx, end_idx] only.
     * Instructions outside this range (exit targets, returns) must not be touched. */
    for (int idx = loop->start_idx; idx <= loop->end_idx && idx < n; idx++)
    {
      ir->compact_instructions[idx].op = TCCIR_OP_NOP;
    }

    /* Place constant assignments in the preheader (or at loop header).
     * Use the first available NOP slot at or before the header. */
    int insert_at = loop->preheader_idx >= 0 ? loop->preheader_idx : loop->header_idx;
    for (int vi = 0; vi < num_const_vars; vi++)
    {
      /* Find a NOP slot at or after insert_at */
      int slot = -1;
      for (int j = insert_at; j < n; j++)
      {
        if (ir->compact_instructions[j].op == TCCIR_OP_NOP)
        {
          slot = j;
          break;
        }
      }
      if (slot < 0)
        continue;

      int32_t dest_vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, const_vars[vi].var_pos);
      ir->compact_instructions[slot].op = TCCIR_OP_ASSIGN;
      IROperand dest_op = irop_make_vreg(dest_vr, const_vars[vi].btype);
      tcc_ir_set_dest(ir, slot, dest_op);
      if (const_vars[vi].value == (int32_t)const_vars[vi].value)
        tcc_ir_set_src1(ir, slot, irop_make_imm32(-1, (int32_t)const_vars[vi].value, const_vars[vi].btype));
      else
      {
        uint32_t pool_idx = tcc_ir_pool_add_i64(ir, const_vars[vi].value);
        tcc_ir_set_src1(ir, slot, irop_make_i64(-1, pool_idx, const_vars[vi].btype));
      }
      tcc_ir_set_src2(ir, slot, IROP_NONE);
    }

    changes++;
  }

  tcc_ir_free_loops(loops);
  return changes;
}

int tcc_ir_opt_dead_loop_elim_ex(IROptCtx *ctx) { return tcc_ir_opt_dead_loop_elim(ctx->ir); }
int tcc_ir_opt_redundant_var_assign_ex(IROptCtx *ctx) { return tcc_ir_opt_redundant_var_assign(ctx->ir); }
int tcc_ir_opt_dead_var_store_elim_ex(IROptCtx *ctx) { return tcc_ir_opt_dead_var_store_elim(ctx->ir); }
int tcc_ir_opt_dead_addrvar_elim_ex(IROptCtx *ctx) { return tcc_ir_opt_dead_addrvar_elim(ctx->ir); }

/* Observable side-effect guard shared by the uninit-UB collapse passes.
 *
 * Those passes exploit a dominating uninit read to declare the whole function
 * UB and collapse it to `b .`.  That is only sound when the function does no
 * observable work BEFORE returning — otherwise the side effects sequenced
 * before the UB read (which GCC keeps) would be wrongly discarded, breaking
 * code that relies on them (e.g. a result written through a pointer parameter,
 * or a call whose effects the caller depends on).  Returns 1 if the function
 * has any such observable effect: a call, inline asm, non-local control flow,
 * a trap, a VLA op, a volatile access, or a STORE that escapes the frame
 * (through a pointer or to a global).  Stores to the function's own locals are
 * unobservable once it returns, so they don't count. */
static int udr_has_observable_side_effects(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    switch (q->op)
    {
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_CALLSEQ_BEGIN:
    case TCCIR_OP_CALLARG_REG:
    case TCCIR_OP_CALLARG_STACK:
    case TCCIR_OP_CALLSEQ_END:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_RETURN:
    case TCCIR_OP_TRAP:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
      return 1;
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    case TCCIR_OP_BLOCK_COPY:
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (!d.is_local)
        return 1;
      break;
    }
    default:
      break;
    }
    for (int k = 0; k <= 2; k++)
    {
      IROperand op;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        if (!irop_config[q->op].has_src2)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }
      if (op.is_sym)
      {
        Sym *vs = irop_get_sym_ex(ir, op);
        if (vs && (vs->type.t & VT_VOLATILE))
          return 1;
      }
    }
  }
  return 0;
}

/* Unconditional Uninitialized Local UB Exploit
 *
 * If the entry basic block unconditionally reads a TCCIR_VREG_TYPE_VAR (local C
 * variable) before any write to it, that read is undefined behavior per C11.
 * Under UB the implementation may do anything; we choose to collapse the entire
 * function body to a single self-jump (`b .`), matching GCC's behavior on
 * gcc.c-torture/compile/931102-1.c.
 *
 * Conservative: scan stops at the first branch/call/return or any subsequent
 * jump-target, so conditional reads (where some path may not exercise the UB)
 * do not trigger the fold.
 */
int tcc_ir_opt_uninit_local_ub(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  /* O2-only: UB exploitation is too aggressive for lower levels where users
   * may rely on "whatever happens to be in the stack slot" semantics. */
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;

  /* Bail on any function containing inline asm / computed goto.  Inline asm
   * operand semantics (outputs written, inputs read, clobbers) aren't modeled
   * with simple has_dest/has_src1/src2 tracking, so a UB read could be hidden
   * inside an asm output and we'd mistakenly collapse the body.  IJUMP has
   * unknown-at-compile-time targets, same risk. */
  for (int i = 0; i < n; i++)
  {
    TccIrOp op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_ASM_INPUT || op == TCCIR_OP_ASM_OUTPUT || op == TCCIR_OP_INLINE_ASM || op == TCCIR_OP_IJUMP)
      return 0;
  }

#define UNINIT_MAX_VAR_POS 1024
  uint8_t written[(UNINIT_MAX_VAR_POS + 7) / 8] = {0};
  uint8_t addr_taken[(UNINIT_MAX_VAR_POS + 7) / 8] = {0};

  /* Pre-scan: identify any VAR whose address is taken anywhere in the function.
   * Address-taken VARs may be written through pointer aliases we cannot
   * statically track, so we conservatively exclude them. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 0; k <= 2; k++)
    {
      IROperand op;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        if (!irop_config[q->op].has_src2)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }
      int32_t vr = irop_get_vreg(op);
      if (vr < 0)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
        continue;
      /* Address-of pattern: is_local=1, is_lval=0 (the operand carries the
       * VAR's address, not its value). */
      if (op.is_local && !op.is_lval)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos >= 0 && pos < UNINIT_MAX_VAR_POS)
          addr_taken[pos >> 3] |= (uint8_t)(1u << (pos & 7));
      }
    }
    /* LEA on a VAR is also address-of even if the operand encoding isn't the
     * direct is_local+!is_lval pattern. */
    if (q->op == TCCIR_OP_LEA && irop_config[q->op].has_src1)
    {
      IROperand op = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(op);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos >= 0 && pos < UNINIT_MAX_VAR_POS)
          addr_taken[pos >> 3] |= (uint8_t)(1u << (pos & 7));
      }
    }
  }

  int found_uninit = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    /* Subsequent jump targets end the entry block. */
    if (i > 0 && q->is_jump_target)
      break;

    /* Check src1 / src2 for read of an unwritten VAR. */
    for (int k = 1; k <= 2 && !found_uninit; k++)
    {
      if (k == 1 && !irop_config[q->op].has_src1)
        continue;
      if (k == 2 && !irop_config[q->op].has_src2)
        continue;
      IROperand sop = (k == 1) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      int32_t svr = irop_get_vreg(sop);
      if (svr < 0)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR)
        continue;
      /* Pure address-of (is_local && !is_lval) is not a value read. */
      if (sop.is_local && !sop.is_lval)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(svr);
      if (pos < 0 || pos >= UNINIT_MAX_VAR_POS)
        continue;
      /* Skip address-taken VARs — pointer writes may have initialized them. */
      if (addr_taken[pos >> 3] & (uint8_t)(1u << (pos & 7)))
        continue;
      if (!(written[pos >> 3] & (uint8_t)(1u << (pos & 7))))
      {
        found_uninit = 1;
        break;
      }
    }
    if (found_uninit)
      break;

    /* Apply this op's WRITE after the read check (program order). */
    if (irop_config[q->op].has_dest)
    {
      IROperand dop = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dop);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(dvr);
        if (pos >= 0 && pos < UNINIT_MAX_VAR_POS)
          written[pos >> 3] |= (uint8_t)(1u << (pos & 7));
      }
    }

    /* Entry-block terminators: anything that splits control flow or may not
     * return.  CALL ends the scan because the callee may initialize VARs whose
     * addresses escaped before the call (we don't track aliasing here). */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP ||
        q->op == TCCIR_OP_SWITCH_TABLE || q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
      break;
  }
#undef UNINIT_MAX_VAR_POS

  if (!found_uninit)
    return 0;

  /* See udr_has_observable_side_effects: only collapse a side-effect-free
   * body.  va-arg-14's main reads an uninitialised `va_list t` but then calls
   * vat(t,1) and exit(0) — observable work that must not be discarded. */
  if (udr_has_observable_side_effects(ir))
    return 0;

  LOG_IR_GEN("UNINIT-UB: collapsing function body to infinite loop (read of uninit local in entry block)");

  /* Replace the whole IR with a single self-jump.  Other passes (compact_nops,
   * codegen) will see only the JUMP and emit `b .` with a minimal prologue. */
  for (int i = 0; i < n; i++)
  {
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[i].is_jump_target = 0;
  }

  ir->compact_instructions[0].op = TCCIR_OP_JUMP;
  ir->compact_instructions[0].is_jump_target = 1;
  IROperand self = irop_make_imm32(-1, 0, IROP_BTYPE_INT32);
  tcc_ir_set_dest(ir, 0, self);
  tcc_ir_set_src1(ir, 0, IROP_NONE);
  tcc_ir_set_src2(ir, 0, IROP_NONE);

  /* Function is now a leaf (no calls left). */
  ir->leaffunc = 1;

  return 1;
}

int tcc_ir_opt_uninit_local_ub_ex(IROptCtx *ctx) { return tcc_ir_opt_uninit_local_ub(ctx->ir); }

/* Uninit-Read Dominates Return — extended UB exploit
 *
 * Generalises uninit_local_ub from "entry block" to "any read of an uninit
 * local that dominates every RETURN op".  If such a read exists, every
 * execution that reaches a return first executes the UB read; per C11 we may
 * legally choose any behaviour, including non-termination.  Collapse to
 * `b .`, matching GCC -O2 on patterns like gcc.c-torture/compile/pc44485.c
 * `func_21` (20→1): the only RETURN is past a TEST of an uninitialised
 * `unsigned short l_53`, so GCC treats the whole function as noreturn.
 *
 * "Uninit at the read" is approximated by a linear-order pre-scan: a VAR
 * whose first read in IR linear order precedes any write of that VAR.  This
 * misses conditional-uninit patterns (where a path with a prior write also
 * exists) but is sound — we never claim uninit where the variable has been
 * written.  Address-taken VARs are excluded since pointer writes may
 * initialize them invisibly.
 */
int tcc_ir_opt_uninit_dominates_return(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;

  /* Inline asm / computed goto: same conservative bail as uninit_local_ub. */
  for (int i = 0; i < n; i++)
  {
    TccIrOp op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_ASM_INPUT || op == TCCIR_OP_ASM_OUTPUT || op == TCCIR_OP_INLINE_ASM || op == TCCIR_OP_IJUMP)
      return 0;
  }

  /* Check for explicit or implicit returns.  An implicit return is a
   * JUMP/JUMPIF whose target is past-end (>= n) — the backend emits
   * `bx lr` at the epilogue for these.  When neither exists,
   * noreturn_collapse handles the function. */
  int has_return = 0;
  int has_implicit_return = 0;
  for (int i = 0; i < n; i++)
  {
    TccIrOp op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_RETURNVALUE || op == TCCIR_OP_RETURNVOID)
    {
      has_return = 1;
      break;
    }
    if (op == TCCIR_OP_JUMP || op == TCCIR_OP_JUMPIF)
    {
      IROperand d = tcc_ir_op_get_dest(ir, &ir->compact_instructions[i]);
      int t = (int)irop_get_imm64_ex(ir, d);
      if (t >= n)
        has_implicit_return = 1;
    }
  }
  if (!has_return && !has_implicit_return)
    return 0;

  /* Don't exploit the UB when the function does observable work before
   * returning — collapsing to `b .` would discard side effects GCC keeps
   * (e.g. 920726-1's `first()` writes its result through a `char *buf`
   * parameter before `return dummy;`). */
  if (udr_has_observable_side_effects(ir))
    return 0;

#define UDR_MAX_VAR_POS 1024
  uint8_t written[(UDR_MAX_VAR_POS + 7) / 8] = {0};
  uint8_t addr_taken[(UDR_MAX_VAR_POS + 7) / 8] = {0};

  /* Pre-scan address-taken VARs. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 0; k <= 2; k++)
    {
      IROperand op;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        if (!irop_config[q->op].has_src2)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }
      int32_t vr = irop_get_vreg(op);
      if (vr < 0)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
        continue;
      if (op.is_local && !op.is_lval)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos >= 0 && pos < UDR_MAX_VAR_POS)
          addr_taken[pos >> 3] |= (uint8_t)(1u << (pos & 7));
      }
    }
    if (q->op == TCCIR_OP_LEA && irop_config[q->op].has_src1)
    {
      IROperand op = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(op);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos >= 0 && pos < UDR_MAX_VAR_POS)
          addr_taken[pos >> 3] |= (uint8_t)(1u << (pos & 7));
      }
    }
  }

  /* Linear scan: find the FIRST instruction that reads a VAR not yet written
   * (in IR order).  Track only one candidate — the earliest qualifying read.
   * This is conservative (we only catch reads where no prior linear-order
   * write exists), but matches what the entry-block pass already does on
   * straight-line code. */
  int uninit_read_idx = -1;
  for (int i = 0; i < n && uninit_read_idx < 0; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    for (int k = 1; k <= 2; k++)
    {
      if (k == 1 && !irop_config[q->op].has_src1)
        continue;
      if (k == 2 && !irop_config[q->op].has_src2)
        continue;
      IROperand sop = (k == 1) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      int32_t svr = irop_get_vreg(sop);
      if (svr < 0)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR)
        continue;
      if (sop.is_local && !sop.is_lval)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(svr);
      if (pos < 0 || pos >= UDR_MAX_VAR_POS)
        continue;
      if (addr_taken[pos >> 3] & (uint8_t)(1u << (pos & 7)))
        continue;
      if (!(written[pos >> 3] & (uint8_t)(1u << (pos & 7))))
      {
        uninit_read_idx = i;
        break;
      }
    }

    if (irop_config[q->op].has_dest)
    {
      IROperand dop = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dop);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(dvr);
        if (pos >= 0 && pos < UDR_MAX_VAR_POS)
          written[pos >> 3] |= (uint8_t)(1u << (pos & 7));
      }
    }
  }
#undef UDR_MAX_VAR_POS

  if (uninit_read_idx < 0)
    return 0;

  /* Build CFG + dominators and verify the uninit read dominates every RETURN. */
  IRCFG *cfg = tcc_ir_cfg_build(ir);
  if (!cfg || cfg->num_blocks == 0)
  {
    if (cfg)
      tcc_ir_cfg_free(cfg);
    return 0;
  }
  tcc_ir_cfg_compute_dominators(cfg);

  int read_block = cfg->instr_to_block[uninit_read_idx];
  int ok = 1;
  for (int i = 0; i < n && ok; i++)
  {
    IRQuadCompact *rq = &ir->compact_instructions[i];
    TccIrOp op = rq->op;
    int is_ret = (op == TCCIR_OP_RETURNVALUE || op == TCCIR_OP_RETURNVOID);
    int is_implicit_ret = 0;
    if (op == TCCIR_OP_JUMP || op == TCCIR_OP_JUMPIF)
    {
      IROperand d = tcc_ir_op_get_dest(ir, rq);
      int t = (int)irop_get_imm64_ex(ir, d);
      if (t >= n)
        is_implicit_ret = 1;
    }
    if (!is_ret && !is_implicit_ret)
      continue;
    int ret_block = cfg->instr_to_block[i];
    if (read_block == ret_block)
    {
      /* Same block: read must come before the RETURN in linear order. */
      if (uninit_read_idx >= i)
        ok = 0;
    }
    else if (!tcc_ir_cfg_dominates(cfg, read_block, ret_block))
    {
      ok = 0;
    }
  }
  tcc_ir_cfg_free(cfg);

  if (!ok)
    return 0;

  LOG_IR_GEN("UNINIT-DOM-RETURN: collapsing function body to infinite loop "
             "(uninit VAR read at i=%d dominates all RETURNs)", uninit_read_idx);

  for (int i = 0; i < n; i++)
  {
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[i].is_jump_target = 0;
  }

  ir->compact_instructions[0].op = TCCIR_OP_JUMP;
  ir->compact_instructions[0].is_jump_target = 1;
  IROperand self = irop_make_imm32(-1, 0, IROP_BTYPE_INT32);
  tcc_ir_set_dest(ir, 0, self);
  tcc_ir_set_src1(ir, 0, IROP_NONE);
  tcc_ir_set_src2(ir, 0, IROP_NONE);

  ir->ls.dirty_registers = 0;
  ir->ls.dirty_float_registers = 0;
  if (ir->ls.live_regs_by_instruction && ir->ls.live_regs_by_instruction_size > 0)
    memset(ir->ls.live_regs_by_instruction, 0,
           ir->ls.live_regs_by_instruction_size * sizeof(ir->ls.live_regs_by_instruction[0]));
  ir->leaffunc = 1;
  ir->noreturn = 1;
  if (tcc_state && tcc_state->cur_func_sym && tcc_state->cur_func_sym->type.ref)
    tcc_state->cur_func_sym->type.ref->f.func_noreturn = 1;

  return 1;
}

int tcc_ir_opt_uninit_dominates_return_ex(IROptCtx *ctx) { return tcc_ir_opt_uninit_dominates_return(ctx->ir); }

/* UB-Only Function Body Elide
 *
 * Generalises uninit_local_ub: if every STORE in the function has an address
 * that traces back through arithmetic / loads to a read of a never-initialized
 * local VAR, every observable effect of the function is UB.  Per C11 we may
 * choose any behaviour; choosing "return immediately" matches GCC -O2 on
 * gcc.c-torture/compile/pr24883.c (where the only side effect is a STORE
 * through an uninitialised `stl` pointer guarded by reads of other uninit
 * locals).  uninit_local_ub can't catch this case because the entry block
 * writes the loop counter before any uninit read; useless_function_body can't
 * either because the body contains essential STOREs and backward loop jumps.
 *
 * Unlike uninit_local_ub which collapses to `b .` (preserving non-termination
 * as the "chosen" UB behaviour for unconditional entry-block UB), this collapse
 * picks "fall through to epilogue" because the function is `void` and has no
 * other side effects worth preserving — matching GCC's choice.
 *
 * Loops with no remaining observable effect are permitted to be assumed
 * terminating (C11 6.8.5/6), so dropping backward jumps here is sound once
 * every STORE is UB-tainted.
 */
int tcc_ir_opt_ub_only_body_elide(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  /* O2-only: same gating philosophy as uninit_local_ub. */
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;

  /* First pass: scan for ops that make whole-function elision unsafe and
   * inventory the STOREs we'll need to prove are UB. */
  int has_store = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    switch (q->op)
    {
    /* Externally observable / unmodellable — can't elide. */
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_FUNCPARAMVAL:
    case TCCIR_OP_FUNCPARAMVOID:
    case TCCIR_OP_CALLSEQ_BEGIN:
    case TCCIR_OP_CALLARG_REG:
    case TCCIR_OP_CALLARG_STACK:
    case TCCIR_OP_CALLSEQ_END:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_RETURNVALUE: /* non-void return: can't drop the return value */
    case TCCIR_OP_TRAP:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_RETURN:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
    case TCCIR_OP_BLOCK_COPY:
    case TCCIR_OP_INIT_CHAIN_SLOT:
    case TCCIR_OP_PREFETCH:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_SWITCH_LOAD:
      return 0;
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
      has_store = 1;
      break;
    default:
      break;
    }

    /* Volatile sym access on any operand keeps the body alive. */
    for (int k = 0; k <= 2; k++)
    {
      IROperand op;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        if (!irop_config[q->op].has_src2)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }
      if (op.is_sym)
      {
        Sym *sym = irop_get_sym_ex(ir, op);
        if (sym && (sym->type.t & VT_VOLATILE))
          return 0;
      }
    }
  }

  /* No STOREs → useless_function_body already handles this; nothing to do. */
  if (!has_store)
    return 0;

#define UB_ELIDE_MAX_VAR_POS 1024
#define UB_ELIDE_MAX_TEMPS 8192
#define UB_ELIDE_MAX_STACK_OFFS 256
  uint8_t var_written[(UB_ELIDE_MAX_VAR_POS + 7) / 8] = {0};
  uint8_t var_addr_taken[(UB_ELIDE_MAX_VAR_POS + 7) / 8] = {0};
  uint8_t temp_tainted[(UB_ELIDE_MAX_TEMPS + 7) / 8] = {0};
  /* Bare stack slots (STACKOFF with vreg=-1, is_local) that are either
   * directly written (dest with is_lval) or have their address taken
   * (Addr[StackLoc[X]], is_lval=0).  Reads of any slot NOT in this set yield
   * uninitialised values — bumping any TEMP defined from such a read into the
   * tainted set so STOREs through it are recognised as UB. */
  int32_t stack_blocked_offs[UB_ELIDE_MAX_STACK_OFFS];
  int stack_blocked_count = 0;
  int stack_blocked_overflow = 0;

  /* Inventory VAR writes and address-takes (same logic as uninit_local_ub). */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 0; k <= 2; k++)
    {
      IROperand op;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        if (!irop_config[q->op].has_src2)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }
      /* Stack-slot blocking: a bare STACKOFF operand (vreg=-1, is_local) is
       * either a direct memory location or an address-of.  Either way, if
       * the slot is touched in any way other than a pure read, treat its
       * contents as potentially initialised. */
      if (!stack_blocked_overflow && irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_local && irop_get_vreg(op) == -1)
      {
        int block = 0;
        if (!op.is_lval)
          block = 1; /* Addr[StackLoc[X]] — pointer could be used to write */
        else if (k == 0)
          block = 1; /* dest with is_lval=1 — direct write to slot */
        if (block)
        {
          int32_t off = irop_get_stack_offset(op);
          int found = 0;
          for (int s = 0; s < stack_blocked_count; s++)
            if (stack_blocked_offs[s] == off)
            {
              found = 1;
              break;
            }
          if (!found)
          {
            if (stack_blocked_count >= UB_ELIDE_MAX_STACK_OFFS)
              stack_blocked_overflow = 1;
            else
              stack_blocked_offs[stack_blocked_count++] = off;
          }
        }
      }
      int32_t vr = irop_get_vreg(op);
      if (vr < 0)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
        continue;
      if (op.is_local && !op.is_lval)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos >= 0 && pos < UB_ELIDE_MAX_VAR_POS)
          var_addr_taken[pos >> 3] |= (uint8_t)(1u << (pos & 7));
      }
    }
    if (q->op == TCCIR_OP_LEA && irop_config[q->op].has_src1)
    {
      IROperand op = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(op);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos >= 0 && pos < UB_ELIDE_MAX_VAR_POS)
          var_addr_taken[pos >> 3] |= (uint8_t)(1u << (pos & 7));
      }
    }
    /* Any VAR appearing as dest is "written" at some point. */
    if (irop_config[q->op].has_dest)
    {
      IROperand dop = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dop);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(dvr);
        /* Position out of range: conservatively skip — we have no record of it
         * either way, so reads of out-of-range VARs won't be classified as
         * uninit either (see below).  Safe. */
        if (pos >= 0 && pos < UB_ELIDE_MAX_VAR_POS)
          var_written[pos >> 3] |= (uint8_t)(1u << (pos & 7));
      }
    }
  }

  /* Helper: classify a source operand as "tainted by reading uninit VAR or
   * uninit stack slot". */
#define SRC_IS_UNINIT_READ(sop) ({                                                                                     \
    int _t = 0;                                                                                                        \
    int32_t _vr = irop_get_vreg(sop);                                                                                  \
    if (_vr >= 0 && TCCIR_DECODE_VREG_TYPE(_vr) == TCCIR_VREG_TYPE_VAR && !((sop).is_local && !(sop).is_lval))         \
    {                                                                                                                  \
      int _p = TCCIR_DECODE_VREG_POSITION(_vr);                                                                        \
      if (_p >= 0 && _p < UB_ELIDE_MAX_VAR_POS &&                                                                      \
          !(var_written[_p >> 3] & (uint8_t)(1u << (_p & 7))) &&                                                       \
          !(var_addr_taken[_p >> 3] & (uint8_t)(1u << (_p & 7))))                                                      \
        _t = 1;                                                                                                        \
    }                                                                                                                  \
    /* Bare stack-slot read (StackLoc[X] as a value source, vreg=-1). */                                               \
    if (!_t && !stack_blocked_overflow && irop_get_tag(sop) == IROP_TAG_STACKOFF &&                                    \
        (sop).is_local && (sop).is_lval && irop_get_vreg(sop) == -1)                                                   \
    {                                                                                                                  \
      int32_t _off = irop_get_stack_offset(sop);                                                                       \
      int _blocked = 0;                                                                                                \
      for (int _s = 0; _s < stack_blocked_count; _s++)                                                                 \
        if (stack_blocked_offs[_s] == _off)                                                                            \
        {                                                                                                              \
          _blocked = 1;                                                                                                \
          break;                                                                                                       \
        }                                                                                                              \
      if (!_blocked)                                                                                                   \
        _t = 1;                                                                                                        \
    }                                                                                                                  \
    _t;                                                                                                                \
  })

#define TEMP_IS_TAINTED(sop) ({                                                                                        \
    int _t = 0;                                                                                                        \
    int32_t _vr = irop_get_vreg(sop);                                                                                  \
    if (_vr >= 0 && TCCIR_DECODE_VREG_TYPE(_vr) == TCCIR_VREG_TYPE_TEMP)                                               \
    {                                                                                                                  \
      int _p = TCCIR_DECODE_VREG_POSITION(_vr);                                                                        \
      if (_p >= 0 && _p < UB_ELIDE_MAX_TEMPS &&                                                                        \
          (temp_tainted[_p >> 3] & (uint8_t)(1u << (_p & 7))))                                                         \
        _t = 1;                                                                                                        \
    }                                                                                                                  \
    _t;                                                                                                                \
  })

  /* Forward fixpoint: propagate taint through TEMP defs.  TEMPs are usually
   * single-def in TCC's IR, so a couple of passes suffice; bound iterations
   * defensively. */
  for (int iter = 0; iter < 16; iter++)
  {
    int changed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (!irop_config[q->op].has_dest)
        continue;
      IROperand dop = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dop);
      if (dvr < 0)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      /* Skip STORE-through-TEMP forms: dest with is_lval means "address",
       * not "this TEMP gets a new value". */
      if (dop.is_lval)
        continue;
      int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
      if (dpos < 0 || dpos >= UB_ELIDE_MAX_TEMPS)
        continue;
      if (temp_tainted[dpos >> 3] & (uint8_t)(1u << (dpos & 7)))
        continue;

      int taint = 0;
      if (irop_config[q->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (SRC_IS_UNINIT_READ(s) || TEMP_IS_TAINTED(s))
          taint = 1;
      }
      if (!taint && irop_config[q->op].has_src2)
      {
        IROperand s = tcc_ir_op_get_src2(ir, q);
        if (SRC_IS_UNINIT_READ(s) || TEMP_IS_TAINTED(s))
          taint = 1;
      }

      if (taint)
      {
        temp_tainted[dpos >> 3] |= (uint8_t)(1u << (dpos & 7));
        changed = 1;
      }
    }
    if (!changed)
      break;
  }

  /* Verify every STORE has a tainted address. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_STORE_POSTINC)
      continue;

    /* Address operand: dest for all three STORE forms (the base pointer). */
    IROperand dop = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(dop);
    int tainted = 0;

    if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
    {
      if (TEMP_IS_TAINTED(dop))
        tainted = 1;
    }
    else if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR && dop.is_lval && !dop.is_local)
    {
      /* Store-through a non-local VAR slot (unusual): if the VAR is uninit,
       * the address is garbage. */
      int p = TCCIR_DECODE_VREG_POSITION(dvr);
      if (p >= 0 && p < UB_ELIDE_MAX_VAR_POS &&
          !(var_written[p >> 3] & (uint8_t)(1u << (p & 7))) &&
          !(var_addr_taken[p >> 3] & (uint8_t)(1u << (p & 7))))
        tainted = 1;
    }

    if (!tainted)
      return 0; /* a STORE has a real, well-defined address — can't elide */
  }

#undef SRC_IS_UNINIT_READ
#undef TEMP_IS_TAINTED
#undef UB_ELIDE_MAX_VAR_POS
#undef UB_ELIDE_MAX_TEMPS
#undef UB_ELIDE_MAX_STACK_OFFS

  LOG_IR_GEN("UB-ELIDE: collapsing function body to empty "
             "(every STORE goes through uninit-pointer address — whole-function UB)");

  /* NOP everything — leave codegen to emit a bare prologue + bx lr.  Mirrors
   * useless_function_body's bookkeeping. */
  for (int i = 0; i < n; i++)
  {
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[i].is_jump_target = 0;
  }

  ir->ls.dirty_registers = 0;
  ir->ls.dirty_float_registers = 0;
  if (ir->ls.live_regs_by_instruction && ir->ls.live_regs_by_instruction_size > 0)
    memset(ir->ls.live_regs_by_instruction, 0,
           ir->ls.live_regs_by_instruction_size * sizeof(ir->ls.live_regs_by_instruction[0]));
  ir->leaffunc = 1;

  return 1;
}

int tcc_ir_opt_ub_only_body_elide_ex(IROptCtx *ctx) { return tcc_ir_opt_ub_only_body_elide(ctx->ir); }

/* Local-only body elide.
 *
 * Sister of ub_only_body_elide.  Where that pass collapses a void function
 * whose every STORE goes through an uninit pointer (so the only effect is
 * UB), this one collapses a void function whose every effect is confined to
 * the function's own stack frame and the curated set of pure soft-float /
 * long-int helpers.  No caller can observe such a function's work —
 * everything dies on return — so we can legally emit just `bx lr`.
 *
 * Closes the gcc.c-torture compile/991213-1 gap (48 → 1): `void p(t, n) {
 * double s = ...t[n/2]...; for (...) s += 2*...t[i]...; }`.  TCC keeps `s`
 * alive across the loop via __aeabi_dadd, even though `s` itself never
 * escapes; useless_function_body bails on the calls, ub_only_body_elide
 * bails because the STOREs aren't UB.
 *
 * What we allow:
 *   - STOREs to local-pointer TEMPs (LEA of stack-local, propagated via
 *     ASSIGN/ADD/SUB)
 *   - calls to tcc_ir_is_pure_aeabi() helpers
 *   - calls to __aeabi_cdcmple / __aeabi_cfcmple (flag-cmp helpers: they
 *     only side-effect CPSR, which dies on return alongside the body)
 *   - calls to memmove/memcpy/memset family iff the destination arg
 *     (FUNCPARAMVAL/FUNCPARAMVOID param_idx 0) is also a local-pointer TEMP
 *   - calls to __tcc_va_arg / __tcc_va_start iff the va_list arg (param 0)
 *     is a local-pointer TEMP — the helper only mutates *ap_ptr, which on
 *     ARM is a local char* whose state dies with the frame.  Closes the
 *     gcc.c-torture compile/20001123-1 gap (15 → 1)
 *   - everything else useless_function_body allows
 *
 * What we bail on (same conservative gating as ub_only_body_elide plus the
 * call/store filters above): RETURNVALUE, inline asm, IJUMP, TRAP, setjmp/
 * longjmp, VLA, BLOCK_COPY, init-chain, switch tables, prefetch, volatile
 * sym access, any non-allowlisted FUNCCALL, any STORE whose dest isn't
 * provably local.
 */
int tcc_ir_opt_local_only_body_elide(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;


  /* Static-chain functions are off-limits in both directions:
   *   - has_static_chain=1: this is a nested function.  Its `StackLoc[N]`
   *     operands encode offsets in the PARENT frame (machine_op.c rewrites
   *     them to chain-relative addresses via R10), so what looks like a
   *     local STORE is actually a write to the parent's stack — externally
   *     observable.  See tests/gcctestsuite execute/20061220-1,
   *     execute/nest-align-1, and tests/ir_tests/nested_capture_*.
   *   - SET_CHAIN present: this is a parent function that exposes its frame
   *     to a nested callee.  The callee can read/write the parent's locals,
   *     so even the parent's `local-only` writes are observable.  The CALL
   *     to the nested function would already trip the non-pure-aeabi guard,
   *     but bail explicitly to keep the contract obvious. */
  if (ir->has_static_chain)
    return 0;

#define LOCAL_ONLY_MAX_MEMMOVE_CALLS 256
#define LOCAL_ONLY_MAX_TEMPS 8192

  /* Pass 0: scan for unconditional bails, record allowed memmove-like
   * calls along with their call_ids for later first-arg verification. */
  int memmove_call_ids[LOCAL_ONLY_MAX_MEMMOVE_CALLS];
  int n_memmove_calls = 0;
  int has_observable_op = 0;
  IROperand return_src = IROP_NONE;
  int return_count = 0;
  int return_void_count = 0;
  int first_return_idx = -1;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    switch (q->op)
    {
    /* Hard bails: same set as ub_only_body_elide; these can publish state
     * or do non-local control flow we cannot reason about. */
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_TRAP:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_RETURN:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
    case TCCIR_OP_BLOCK_COPY:
    case TCCIR_OP_INIT_CHAIN_SLOT:
    case TCCIR_OP_SET_CHAIN:
    case TCCIR_OP_PREFETCH:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_SWITCH_LOAD:
      return 0;
    case TCCIR_OP_RETURNVALUE:
    {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int tag = irop_get_tag(s);
      if (tag != IROP_TAG_IMM32 && tag != IROP_TAG_I64)
        return 0;
      if (return_count == 0)
      {
        return_src = s;
        first_return_idx = i;
      }
      else if (tag != irop_get_tag(return_src) || s.btype != return_src.btype ||
               irop_get_imm64_ex(ir, s) != irop_get_imm64_ex(ir, return_src))
      {
        return 0;
      }
      return_count++;
      break;
    }
    case TCCIR_OP_RETURNVOID:
      return_void_count++;
      break;
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    {
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (!callee)
        return 0;
      const char *name = get_tok_str(callee->v, NULL);
      if (!name)
        return 0;
      has_observable_op = 1;
      if (tcc_ir_is_pure_aeabi(name))
        break;
      /* Flag-cmp helpers (__aeabi_cdcmple / __aeabi_cfcmple): functionally
       * pure — they read both operands, set CPSR, return nothing.  The CPSR
       * flags are caller-invisible on function return, so a call whose only
       * "side effect" is flag-setting is as elidable as any pure aeabi
       * helper.  Without this, an empty-body `if (a < b) {}` over doubles
       * keeps the cdcmple call and its dadd-fed comparand alive (see
       * gcc.c-torture compile/pr45969-1.c). */
      if (ir_opt_is_flag_cmp_helper_name(name))
        break;
      /* memmove/memcpy/memset family, and the va_list helpers: each writes
       * through its first arg only.  If that destination points into our
       * own stack frame, the writes are unobservable; we'll verify later.
       * __tcc_va_arg also reads from the caller's va arg area, but that's
       * caller-supplied state we cannot affect — the only write is to
       * *ap_ptr (the local va_list). */
      int is_memlike = strcmp(name, "__aeabi_memmove4") == 0 || strcmp(name, "__aeabi_memmove8") == 0 ||
                       strcmp(name, "__aeabi_memmove") == 0 || strcmp(name, "__aeabi_memcpy4") == 0 ||
                       strcmp(name, "__aeabi_memcpy8") == 0 || strcmp(name, "__aeabi_memcpy") == 0 ||
                       strcmp(name, "__aeabi_memset") == 0 || strcmp(name, "__aeabi_memset4") == 0 ||
                       strcmp(name, "__aeabi_memset8") == 0 || strcmp(name, "__aeabi_memclr") == 0 ||
                       strcmp(name, "__aeabi_memclr4") == 0 || strcmp(name, "__aeabi_memclr8") == 0 ||
                       strcmp(name, "memmove") == 0 || strcmp(name, "memcpy") == 0 ||
                       strcmp(name, "memset") == 0 || strcmp(name, "__tcc_va_arg") == 0 ||
                       strcmp(name, "__tcc_va_start") == 0;
      if (!is_memlike)
        return 0;
      if (n_memmove_calls >= LOCAL_ONLY_MAX_MEMMOVE_CALLS)
        return 0;
      IROperand call_id_op = tcc_ir_op_get_src2(ir, q);
      int call_id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, call_id_op));
      memmove_call_ids[n_memmove_calls++] = call_id;
      break;
    }
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
      has_observable_op = 1;
      break;
    default:
      break;
    }

    /* Volatile sym access on any operand keeps the body alive. */
    for (int k = 0; k <= 2; k++)
    {
      IROperand op;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        if (!irop_config[q->op].has_src2)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }
      if (op.is_sym)
      {
        Sym *sym = irop_get_sym_ex(ir, op);
        if (sym && (sym->type.t & VT_VOLATILE))
          return 0;
      }
    }
  }

  /* useless_function_body already covers the no-call-no-store case and
   * runs after us — leave it alone to avoid double-counting. */
  if (!has_observable_op)
    return 0;
  if (return_count > 0 && return_void_count > 0)
    return 0;

  /* Pass 1: forward fixpoint marking TEMPs that hold a local-stack-frame
   * pointer.  Seed from LEA of any stack-local; propagate through ASSIGN,
   * ADD, SUB (pointer + integer offset stays local). */
  uint8_t local_ptr[(LOCAL_ONLY_MAX_TEMPS + 7) / 8] = {0};

#define LP_GET(p) ((local_ptr[(p) >> 3] & (uint8_t)(1u << ((p) & 7))) != 0)
#define LP_SET(p)                                                                                                      \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((p) >= 0 && (p) < LOCAL_ONLY_MAX_TEMPS)                                                                        \
      local_ptr[(p) >> 3] |= (uint8_t)(1u << ((p) & 7));                                                               \
  } while (0)

#define IS_LOCAL_PTR_OP(sop)                                                                                           \
  ({                                                                                                                   \
    int _r = 0;                                                                                                        \
    int32_t _vr = irop_get_vreg(sop);                                                                                  \
    int _tag = irop_get_tag(sop);                                                                                      \
    if (_tag == IROP_TAG_STACKOFF && (sop).is_local && !(sop).is_lval && !(sop).is_llocal && !(sop).is_param)          \
      _r = 1;                                                                                                          \
    if (_vr >= 0 && TCCIR_DECODE_VREG_TYPE(_vr) == TCCIR_VREG_TYPE_TEMP)                                               \
    {                                                                                                                  \
      int _p = TCCIR_DECODE_VREG_POSITION(_vr);                                                                        \
      if (_p >= 0 && _p < LOCAL_ONLY_MAX_TEMPS && LP_GET(_p))                                                          \
        _r = 1;                                                                                                        \
    }                                                                                                                  \
    _r;                                                                                                                \
  })

  for (int iter = 0; iter < 16; iter++)
  {
    int changed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (!irop_config[q->op].has_dest)
        continue;
      IROperand dop = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dop);
      if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      if (dop.is_lval) /* STORE-through-TEMP form is not a def of this TEMP */
        continue;
      int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
      if (dpos < 0 || dpos >= LOCAL_ONLY_MAX_TEMPS)
        continue;
      if (LP_GET(dpos))
        continue;

      int set = 0;
      switch (q->op)
      {
      case TCCIR_OP_LEA:
      {
        /* Address of a stack-local (anonymous offset or local VAR) — its
         * lifetime is bounded by the function, so the resulting pointer
         * is local-only.  Non-volatile parameters are automatic objects too:
         * taking &P0 materializes the callee's parameter slot, not caller
         * state. */
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (s.is_llocal)
          break;
        int tag = irop_get_tag(s);
        if (tag == IROP_TAG_STACKOFF && s.is_local)
          set = 1;
        else
        {
          int32_t svr = irop_get_vreg(s);
          if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR && s.is_local)
            set = 1;
          else if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_PARAM &&
                   !ir_opt_param_vreg_is_volatile(TCCIR_DECODE_VREG_POSITION(svr)))
            set = 1;
        }
        break;
      }
      case TCCIR_OP_ASSIGN:
      case TCCIR_OP_STORE:
      {
        /* ASSIGN, or — after var-to-tmp promotion — a STORE with non-lval
         * TEMP dest, which is semantically a TEMP definition (the `is_lval`
         * check above already filtered out STORE-through-pointer).  Propagate
         * local-pointer status from the source. */
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (IS_LOCAL_PTR_OP(s))
          set = 1;
        break;
      }
      case TCCIR_OP_ADD:
      case TCCIR_OP_SUB:
      {
        /* local-pointer ± non-pointer is still local-pointer.  We don't
         * track non-pointerness here, but for the purpose of "writes via
         * this address only hit our frame," it suffices that one operand
         * is a local-pointer. */
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        IROperand s2 = tcc_ir_op_get_src2(ir, q);
        if (IS_LOCAL_PTR_OP(s1) || IS_LOCAL_PTR_OP(s2))
          set = 1;
        break;
      }
      case TCCIR_OP_MLA:
      {
        /* MLA dest = src1 * src2 + accum.  If accum is a local-pointer,
         * the result is local-pointer + scaled-non-pointer-offset.  Closes
         * the pr41181 pattern: fusion merged `n*250` and `&best_paths + ...`
         * into one MLA, hiding the underlying pointer-plus-offset shape. */
        IROperand accum = tcc_ir_op_get_accum(ir, q);
        if (IS_LOCAL_PTR_OP(accum))
          set = 1;
        break;
      }
      default:
        break;
      }

      if (set)
      {
        LP_SET(dpos);
        changed = 1;
      }
    }
    if (!changed)
      break;
  }

  /* Pass 2a: every STORE/STORE_INDEXED/STORE_POSTINC must write through
   * a local-pointer address. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_STORE_POSTINC)
      continue;
    IROperand dop = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(dop);
    int ok = 0;
    if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
    {
      /* After var-to-tmp promotion, a STORE with non-lval TEMP dest is just
       * a TEMP definition (no memory write).  Pass 1 already propagated
       * local-pointer status into the TEMP if applicable; for accepting the
       * STORE itself, a TEMP def is unconditionally local-only. */
      if (q->op == TCCIR_OP_STORE && !dop.is_lval)
        ok = 1;
      else if (IS_LOCAL_PTR_OP(dop))
        ok = 1;
    }
    else if (q->op == TCCIR_OP_STORE)
    {
      /* Direct automatic-object stores and STACKOFF/local dests are local-only. */
      int tag = irop_get_tag(dop);
      if (ir_opt_direct_auto_vreg_store_is_local(dop))
        ok = 1;
      else if (tag == IROP_TAG_STACKOFF && dop.is_local && !dop.is_param)
        ok = 1;
    }
    else if (q->op == TCCIR_OP_STORE_INDEXED)
    {
      /* Direct indexed writes into a local stack object, e.g.
       * Addr[StackLoc[-N]] <-- val STORE_INDEXED idx, are still confined to
       * this frame.  Require the address-of form; an lvalue stack slot here
       * would mean "load a pointer from the stack, then store through it". */
      int tag = irop_get_tag(dop);
      if (tag == IROP_TAG_STACKOFF && dop.is_local && !dop.is_lval && !dop.is_llocal && !dop.is_param)
        ok = 1;
    }
    else
    {
      /* STORE_INDEXED/STORE_POSTINC write through the destination pointer.
       * A PARAM/VAR vreg in that slot is the pointer value, not the automatic
       * object itself, so only proven local-pointer TEMPs are accepted. */
    }
    if (!ok)
      return 0;
  }

  /* Pass 2b: each memmove-like call's first-arg PARAM must be a local
   * pointer (so the writes the callee does land in our frame). */
  for (int j = 0; j < n_memmove_calls; j++)
  {
    int target_call_id = memmove_call_ids[j];
    int verified = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID)
        continue;
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int64_t encoded = irop_get_imm64_ex(ir, src2);
      if (TCCIR_DECODE_CALL_ID(encoded) != target_call_id)
        continue;
      if (TCCIR_DECODE_PARAM_IDX(encoded) != 0)
        continue;
      IROperand val = tcc_ir_op_get_src1(ir, q);
      if (IS_LOCAL_PTR_OP(val))
        verified = 1;
      break;
    }
    if (!verified)
      return 0;
  }

#undef LP_GET
#undef LP_SET
#undef IS_LOCAL_PTR_OP
#undef LOCAL_ONLY_MAX_MEMMOVE_CALLS
#undef LOCAL_ONLY_MAX_TEMPS

  LOG_IR_GEN("LOCAL-ONLY-ELIDE: collapsing function body — every side effect "
             "is confined to the local stack frame (no caller-visible state)");

  for (int i = 0; i < n; i++)
  {
    if (return_count > 0 && i == first_return_idx)
    {
      ir->compact_instructions[i].op = TCCIR_OP_RETURNVALUE;
      tcc_ir_set_src1(ir, i, return_src);
    }
    else
    {
      ir->compact_instructions[i].op = TCCIR_OP_NOP;
    }
    ir->compact_instructions[i].is_jump_target = 0;
  }

  ir->ls.dirty_registers = 0;
  ir->ls.dirty_float_registers = 0;
  if (ir->ls.live_regs_by_instruction && ir->ls.live_regs_by_instruction_size > 0)
    memset(ir->ls.live_regs_by_instruction, 0,
           ir->ls.live_regs_by_instruction_size * sizeof(ir->ls.live_regs_by_instruction[0]));
  ir->leaffunc = 1;

  /* The body referenced P0/P1/... so linear-scan parked them in callee-
   * saved registers (r4+); the prologue's parameter-setup pass would still
   * emit `mov r4, r0; mov r5, r1` based on those allocations.  With every
   * IR op NOPed the params are dead, so clear the allocations.  This
   * mirrors ir->leaffunc=1 + dirty_registers=0 above: tell every consumer
   * that the body needs nothing. */
  for (int p = 0; p < ir->next_parameter; p++)
  {
    IRLiveInterval *iv = &ir->parameters_live_intervals[p];
    iv->allocation.r0 = PREG_NONE;
    iv->allocation.r1 = PREG_NONE;
    iv->allocation.offset = 0;
  }

  return 1;
}

int tcc_ir_opt_local_only_body_elide_ex(IROptCtx *ctx) { return tcc_ir_opt_local_only_body_elide(ctx->ir); }

/* Const-return UB elide.
 *
 * Sister of ub_only_body_elide / local_only_body_elide.  Those passes only
 * fire on void functions whose every STORE is UB / local-confined.  This one
 * collapses a *non-void* function whose entry block executes UB (reads from
 * an untouched local stack slot) before any observable side effect, and
 * whose every RETURNVALUE returns the same constant.
 *
 * Per C11 6.3.2.1 / 4.3, reading an uninitialised auto with never-taken
 * address is UB; once UB has been executed the program's entire behaviour
 * is undefined and the implementation may choose any continuation.  GCC -O2
 * picks "return the constant immediately, skipping the body."  Closes the
 * gcc.c-torture compile/20011109-1 gap (`die`: 140 -> 3): the body's `for
 * (x=0; x < n.e; ...)` reads uninit `n.e` in the loop guard, all paths
 * eventually flow to `return o` where `o=0` was never reassigned, so GCC
 * emits a 3-insn `return 0` and skips every call/store in between.
 *
 * Gating (conservative):
 *   - O2-only.
 *   - Function must have >=1 RETURNVALUE, all with the same constant src
 *     (IMM32 / I64).  RETURNVOID anywhere -> bail (mixed-return ambiguity).
 *   - No inline asm, IJUMP, TRAP, setjmp/longjmp, VLA primitives, SET_CHAIN
 *     / INIT_CHAIN_SLOT, BUILTIN_APPLY*, SWITCH_TABLE / SWITCH_LOAD.
 *   - No volatile sym access anywhere.
 *   - No nested-function context (has_static_chain).
 *   - No LEA of a STACKOFF and no `Addr[StackLoc[...]]` operand anywhere —
 *     if the address of any local escapes, we can't tell if that slot was
 *     written through a pointer, so we can't safely call any STACKOFF read
 *     "uninit".
 *
 * Firing condition (in the entry block, in program order, skipping NOPs):
 *   - We must encounter at least one STACKOFF read (is_lval, is_local,
 *     !is_param) whose offset matches no STORE/STORE_INDEXED/STORE_POSTINC
 *     dest in the function — i.e. an uninit local stack read.
 *   - That uninit read must happen BEFORE any observable side effect
 *     (STORE / FUNCCALL* / BLOCK_COPY / PREFETCH / CALLSEQ_*) in the entry
 *     block — otherwise the side effect was well-defined and we'd lose it.
 *   - Entry block ends at JUMP / JUMPIF / RETURN; if we exit without
 *     finding uninit, bail.
 *
 * Output: NOP every instruction, write a single RETURNVALUE-with-constant
 * at index 0.  Codegen emits a bare prologue + `movs r0, #c; bx lr`. */
int tcc_ir_opt_const_return_uninit_elide(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;
  if (ir->has_static_chain)
    return 0;

#define CRUE_MAX_STORE_OFFSETS 512
  int store_offsets[CRUE_MAX_STORE_OFFSETS];
  int n_store_offsets = 0;

  IROperand rv_src = IROP_NONE;
  int rv_count = 0;
  int64_t rv_val = 0;
  int rv_const_tag = 0;
  int rv_btype = 0;

  /* Pass 1: hard-bail scan + inventory of STORE dest offsets + RETURNVALUE
   * source validation + Addr[StackLoc]/LEA-of-STACKOFF detection. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    switch (q->op)
    {
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_TRAP:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
    case TCCIR_OP_SET_CHAIN:
    case TCCIR_OP_INIT_CHAIN_SLOT:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_RETURN:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_SWITCH_LOAD:
    case TCCIR_OP_RETURNVOID:
      return 0;
    case TCCIR_OP_RETURNVALUE:
    {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int tag = irop_get_tag(s);
      if (tag != IROP_TAG_IMM32 && tag != IROP_TAG_I64)
        return 0;
      int64_t v = irop_get_imm64_ex(ir, s);
      if (rv_count == 0)
      {
        rv_val = v;
        rv_btype = s.btype;
        rv_const_tag = tag;
        rv_src = s;
      }
      else if (v != rv_val || tag != rv_const_tag || s.btype != rv_btype)
        return 0;
      rv_count++;
      break;
    }
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    {
      IROperand dop = tcc_ir_op_get_dest(ir, q);
      if (irop_get_tag(dop) == IROP_TAG_STACKOFF && dop.is_local && !dop.is_param)
      {
        int off = (int)irop_get_stack_offset(dop);
        if (n_store_offsets >= CRUE_MAX_STORE_OFFSETS)
          return 0;
        store_offsets[n_store_offsets++] = off;
      }
      break;
    }
    default:
      break;
    }

    /* Scan all operands: bail on Addr[StackLoc] (stack address escapes —
     * any STACKOFF read could be initialized via that alias) and on volatile
     * sym access. */
    for (int k = 0; k <= 2; k++)
    {
      IROperand op;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        if (!irop_config[q->op].has_src2)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }
      if (irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_local && !op.is_lval && !op.is_param)
        return 0;
      if (op.is_sym)
      {
        Sym *sym = irop_get_sym_ex(ir, op);
        if (sym && (sym->type.t & VT_VOLATILE))
          return 0;
      }
    }

    /* LEA with a STACKOFF source materialises a stack address — same risk
     * as a direct Addr[StackLoc] operand. */
    if (q->op == TCCIR_OP_LEA && irop_config[q->op].has_src1)
    {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      if (irop_get_tag(s) == IROP_TAG_STACKOFF)
        return 0;
    }
  }

  if (rv_count == 0)
    return 0;

  /* Pass 2: walk the linear program prefix from entry looking for the
   * first uninit STACKOFF read vs. the first observable side effect.  We
   * do NOT stop at jump targets — on the first iteration through any loop
   * the back-edge isn't yet taken, so control reaches the loop header via
   * fall-through from entry, and a UB read in the loop header IS executed
   * on entry. */
  int found_uninit = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Check src1/src2 for uninit STACKOFF read. */
    for (int k = 1; k <= 2 && !found_uninit; k++)
    {
      if (k == 1 && !irop_config[q->op].has_src1)
        continue;
      if (k == 2 && !irop_config[q->op].has_src2)
        continue;
      IROperand sop = (k == 1) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      if (irop_get_tag(sop) != IROP_TAG_STACKOFF)
        continue;
      if (!sop.is_lval)
        continue;
      if (!sop.is_local)
        continue;
      if (sop.is_param)
        continue;
      /* Spilled vregs (assigned vreg in the STACKOFF operand) carry a
       * value that was defined by an earlier ASSIGN/etc.  The spill code
       * is injected during codegen, not visible as an IR STORE here, so
       * our store-offset table won't list it — yet the read is well
       * defined.  Only treat raw frontend STACKOFFs (vreg == -1) as
       * potentially uninit. */
      if (irop_has_vreg(sop) && irop_get_vreg(sop) >= 0)
        continue;
      int off = (int)irop_get_stack_offset(sop);
      int touched = 0;
      for (int j = 0; j < n_store_offsets; j++)
      {
        if (store_offsets[j] == off)
        {
          touched = 1;
          break;
        }
      }
      if (!touched)
      {
        found_uninit = 1;
        break;
      }
    }

    /* If we hit an observable op before any uninit read, the observable
     * effect would be lost — bail. */
    if (!found_uninit)
    {
      switch (q->op)
      {
      case TCCIR_OP_STORE:
      case TCCIR_OP_STORE_INDEXED:
      case TCCIR_OP_STORE_POSTINC:
      case TCCIR_OP_FUNCCALLVAL:
      case TCCIR_OP_FUNCCALLVOID:
      case TCCIR_OP_FUNCPARAMVAL:
      case TCCIR_OP_FUNCPARAMVOID:
      case TCCIR_OP_CALLSEQ_BEGIN:
      case TCCIR_OP_CALLARG_REG:
      case TCCIR_OP_CALLARG_STACK:
      case TCCIR_OP_CALLSEQ_END:
      case TCCIR_OP_BLOCK_COPY:
      case TCCIR_OP_PREFETCH:
        return 0;
      default:
        break;
      }
    }

    /* Entry-block terminators. */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_RETURNVALUE ||
        q->op == TCCIR_OP_RETURNVOID)
      break;
  }

#undef CRUE_MAX_STORE_OFFSETS

  if (!found_uninit)
    return 0;

  LOG_IR_GEN("CONST-RETURN-UNINIT-ELIDE: collapsing function to a single "
             "RETURNVALUE constant (entry-block UB read poisons all paths; "
             "every RETURNVALUE returns the same constant)");

  /* NOP everything, then place a single RETURNVALUE-const at index 0. */
  for (int i = 0; i < n; i++)
  {
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[i].is_jump_target = 0;
  }

  ir->compact_instructions[0].op = TCCIR_OP_RETURNVALUE;
  tcc_ir_set_dest(ir, 0, IROP_NONE);
  tcc_ir_set_src1(ir, 0, rv_src);
  tcc_ir_set_src2(ir, 0, IROP_NONE);

  ir->ls.dirty_registers = 0;
  ir->ls.dirty_float_registers = 0;
  if (ir->ls.live_regs_by_instruction && ir->ls.live_regs_by_instruction_size > 0)
    memset(ir->ls.live_regs_by_instruction, 0,
           ir->ls.live_regs_by_instruction_size * sizeof(ir->ls.live_regs_by_instruction[0]));
  ir->leaffunc = 1;

  /* Clear param allocations: the body no longer references any param, so
   * the prologue's `mov r4, r0` (etc) parameter-setup should be skipped.
   * Mirrors local_only_body_elide. */
  for (int p = 0; p < ir->next_parameter; p++)
  {
    IRLiveInterval *iv = &ir->parameters_live_intervals[p];
    iv->allocation.r0 = PREG_NONE;
    iv->allocation.r1 = PREG_NONE;
    iv->allocation.offset = 0;
  }

  return 1;
}

int tcc_ir_opt_const_return_uninit_elide_ex(IROptCtx *ctx) { return tcc_ir_opt_const_return_uninit_elide(ctx->ir); }

/* Null-Store Dominates Return — UB exploit for STORE through compile-time NULL.
 *
 * Detects functions where some STORE has its address operand provably equal to
 * a compile-time constant 0 (NULL pointer dereference) on at least one
 * execution path, and that STORE dominates every RETURNVOID.  Per C11 those
 * executions are UB and we may legally choose any behaviour; we pick
 * "collapse to bx lr", matching GCC -O2 on gcc.c-torture/compile/pr36817.c
 * where `unsigned *p=0; *p++=0;` reduces the entire body to a single return.
 *
 * Approach: linear forward scan from entry tracking which TEMPs / VARs hold
 * a compile-time-known zero (propagated through ASSIGN of #0 and ASSIGN of a
 * known-zero source).  Stop at any operation that breaks linear flow
 * (unconditional JUMP, RETURN, CALL, IJUMP, SWITCH_TABLE, asm).  When the
 * scan finds a STORE through a known-zero address, verify its block
 * dominates every RETURNVOID before collapsing.
 *
 * The linear-scan + dominator check is sound:
 *   - Stopping at unconditional JUMP ensures we never claim UB based on a
 *     hypothetical state at code skipped by the jump.
 *   - Killing known-zero on any non-zero write means the recorded "known
 *     zero" is the value seen on the LINEAR fall-through path from entry.
 *   - The dominator check ensures every actual execution reaches the STORE,
 *     so UB on the linear path implies UB on every execution.
 */
int tcc_ir_opt_null_store_dom_return(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  /* O2-only: same gating philosophy as the other UB-exploit passes. */
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;

  /* Bail on unanalyzable ops + non-void returns.  Inline asm / IJUMP can hide
   * writes through pointers we can't see; non-void returns would need a
   * synthesized return value we don't have.  An explicit RETURNVOID is not
   * required — TCC's IR often elides it and relies on the codegen epilogue. */
  for (int i = 0; i < n; i++)
  {
    TccIrOp op = ir->compact_instructions[i].op;
    switch (op)
    {
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_TRAP:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_RETURN:
    case TCCIR_OP_RETURNVALUE:
      return 0;
    default:
      break;
    }
    /* Volatile sym access on any operand is observable; can't elide. */
    IRQuadCompact *q = &ir->compact_instructions[i];
    for (int k = 0; k <= 2; k++)
    {
      IROperand op2;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op2 = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        op2 = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        if (!irop_config[q->op].has_src2)
          continue;
        op2 = tcc_ir_op_get_src2(ir, q);
      }
      if (op2.is_sym)
      {
        Sym *sym = irop_get_sym_ex(ir, op2);
        if (sym && (sym->type.t & VT_VOLATILE))
          return 0;
      }
    }
  }
#define NSDR_MAX_TEMP 8192
#define NSDR_MAX_VAR 1024
  uint8_t temp_zero[(NSDR_MAX_TEMP + 7) / 8] = {0};
  uint8_t var_zero[(NSDR_MAX_VAR + 7) / 8] = {0};
  uint8_t var_addr_taken[(NSDR_MAX_VAR + 7) / 8] = {0};

  /* Pre-scan: identify address-taken VARs (skip them — pointer writes could
   * have initialized them invisibly). */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 0; k <= 2; k++)
    {
      IROperand op;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        if (!irop_config[q->op].has_src2)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }
      int32_t vr = irop_get_vreg(op);
      if (vr < 0)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
        continue;
      if (op.is_local && !op.is_lval)
      {
        int p = TCCIR_DECODE_VREG_POSITION(vr);
        if (p >= 0 && p < NSDR_MAX_VAR)
          var_addr_taken[p >> 3] |= (uint8_t)(1u << (p & 7));
      }
    }
    if (q->op == TCCIR_OP_LEA && irop_config[q->op].has_src1)
    {
      IROperand op = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(op);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int p = TCCIR_DECODE_VREG_POSITION(vr);
        if (p >= 0 && p < NSDR_MAX_VAR)
          var_addr_taken[p >> 3] |= (uint8_t)(1u << (p & 7));
      }
    }
  }

#define VREG_IS_KNOWN_ZERO(_op)                                                                                            \
  ({                                                                                                                       \
    int _r = 0;                                                                                                            \
    int32_t _vr = irop_get_vreg(_op);                                                                                      \
    if (_vr >= 0 && !(_op).is_lval)                                                                                        \
    {                                                                                                                      \
      int _t = TCCIR_DECODE_VREG_TYPE(_vr);                                                                                \
      int _p = TCCIR_DECODE_VREG_POSITION(_vr);                                                                            \
      if (_t == TCCIR_VREG_TYPE_TEMP && _p >= 0 && _p < NSDR_MAX_TEMP)                                                     \
      {                                                                                                                    \
        if (temp_zero[_p >> 3] & (uint8_t)(1u << (_p & 7)))                                                                \
          _r = 1;                                                                                                          \
      }                                                                                                                    \
      else if (_t == TCCIR_VREG_TYPE_VAR && _p >= 0 && _p < NSDR_MAX_VAR &&                                                \
               !(var_addr_taken[_p >> 3] & (uint8_t)(1u << (_p & 7))))                                                     \
      {                                                                                                                    \
        if (var_zero[_p >> 3] & (uint8_t)(1u << (_p & 7)))                                                                 \
          _r = 1;                                                                                                          \
      }                                                                                                                    \
    }                                                                                                                      \
    _r;                                                                                                                    \
  })

  int ub_store_idx = -1;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Check STORE through known-NULL pointer (before applying any writes
     * from this instruction — STORE's "dest" is the address it reads). */
    if (q->op == TCCIR_OP_STORE)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (dest.is_lval && !dest.is_local)
      {
        /* Direct immediate NULL address operand. */
        if (irop_is_immediate(dest) && irop_get_imm64_ex(ir, dest) == 0)
        {
          ub_store_idx = i;
          break;
        }
        /* Vreg currently known to be zero. */
        int32_t vr = irop_get_vreg(dest);
        if (vr >= 0)
        {
          int t = TCCIR_DECODE_VREG_TYPE(vr);
          int p = TCCIR_DECODE_VREG_POSITION(vr);
          if (t == TCCIR_VREG_TYPE_TEMP && p >= 0 && p < NSDR_MAX_TEMP)
          {
            if (temp_zero[p >> 3] & (uint8_t)(1u << (p & 7)))
            {
              ub_store_idx = i;
              break;
            }
          }
          else if (t == TCCIR_VREG_TYPE_VAR && p >= 0 && p < NSDR_MAX_VAR &&
                   !(var_addr_taken[p >> 3] & (uint8_t)(1u << (p & 7))))
          {
            if (var_zero[p >> 3] & (uint8_t)(1u << (p & 7)))
            {
              ub_store_idx = i;
              break;
            }
          }
        }
      }
    }

    /* Apply the write effect of this instruction to known-zero tracking. */
    if (irop_config[q->op].has_dest)
    {
      IROperand dst = tcc_ir_op_get_dest(ir, q);
      /* is_lval (deref) or is_local (stack address) destinations don't
       * define a vreg in the value sense. */
      if (!dst.is_lval && !dst.is_local)
      {
        int32_t dvr = irop_get_vreg(dst);
        if (dvr >= 0)
        {
          int dtype = TCCIR_DECODE_VREG_TYPE(dvr);
          int dpos = TCCIR_DECODE_VREG_POSITION(dvr);

          int makes_zero = 0;
          if (q->op == TCCIR_OP_ASSIGN && irop_config[q->op].has_src1)
          {
            IROperand src1 = tcc_ir_op_get_src1(ir, q);
            if (irop_is_immediate(src1) && irop_get_imm64_ex(ir, src1) == 0)
              makes_zero = 1;
            else if (VREG_IS_KNOWN_ZERO(src1))
              makes_zero = 1;
          }

          if (dtype == TCCIR_VREG_TYPE_TEMP && dpos >= 0 && dpos < NSDR_MAX_TEMP)
          {
            if (makes_zero)
              temp_zero[dpos >> 3] |= (uint8_t)(1u << (dpos & 7));
            else
              temp_zero[dpos >> 3] &= (uint8_t)~(1u << (dpos & 7));
          }
          else if (dtype == TCCIR_VREG_TYPE_VAR && dpos >= 0 && dpos < NSDR_MAX_VAR)
          {
            if (makes_zero)
              var_zero[dpos >> 3] |= (uint8_t)(1u << (dpos & 7));
            else
              var_zero[dpos >> 3] &= (uint8_t)~(1u << (dpos & 7));
          }
        }
      }
    }

    /* Stop conditions: any op that ends linear forward flow.  An unconditional
     * JUMP makes subsequent linear-order instructions unreachable from this
     * point (they'd need to be entered via a jump target with possibly
     * different state, which we can't track here).  Other terminators have
     * similar semantics. */
    if (q->op == TCCIR_OP_JUMP)
      break;
    if (q->op == TCCIR_OP_RETURNVOID || q->op == TCCIR_OP_RETURNVALUE)
      break;
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
      break;
    if (q->op == TCCIR_OP_SWITCH_TABLE)
      break;
  }

#undef VREG_IS_KNOWN_ZERO
#undef NSDR_MAX_TEMP
#undef NSDR_MAX_VAR

  if (ub_store_idx < 0)
    return 0;

  /* Verify the UB STORE's block dominates every function exit.  An "exit" is
   * either an explicit RETURNVOID or a CFG-leaf block (no successors — fall
   * off the end of the function, which TCC's codegen handles by appending a
   * bx lr). */
  IRCFG *cfg = tcc_ir_cfg_build(ir);
  if (!cfg || cfg->num_blocks == 0)
  {
    if (cfg)
      tcc_ir_cfg_free(cfg);
    return 0;
  }
  tcc_ir_cfg_compute_dominators(cfg);

  int store_block = cfg->instr_to_block[ub_store_idx];
  int ok = 1;
  /* Check explicit RETURNVOIDs. */
  for (int i = 0; i < n && ok; i++)
  {
    TccIrOp op = ir->compact_instructions[i].op;
    if (op != TCCIR_OP_RETURNVOID)
      continue;
    int ret_block = cfg->instr_to_block[i];
    if (store_block == ret_block)
    {
      if (ub_store_idx >= i)
        ok = 0;
    }
    else if (!tcc_ir_cfg_dominates(cfg, store_block, ret_block))
    {
      ok = 0;
    }
  }
  /* Check CFG-leaf blocks (implicit fall-off exits). */
  for (int b = 0; b < cfg->num_blocks && ok; b++)
  {
    if (cfg->blocks[b].num_succs != 0)
      continue;
    if (b == store_block)
      continue; /* same block: STORE comes before the implicit exit by construction */
    if (!tcc_ir_cfg_dominates(cfg, store_block, b))
      ok = 0;
  }
  tcc_ir_cfg_free(cfg);

  if (!ok)
    return 0;

  LOG_IR_GEN("NULL-STORE-DOM-RETURN: collapsing function body to bx lr "
             "(STORE at i=%d through compile-time NULL dominates all RETURNVOIDs)", ub_store_idx);

  /* NOP everything — codegen will emit bare prologue + bx lr.  Mirrors
   * ub_only_body_elide's bookkeeping. */
  for (int i = 0; i < n; i++)
  {
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[i].is_jump_target = 0;
  }

  ir->ls.dirty_registers = 0;
  ir->ls.dirty_float_registers = 0;
  if (ir->ls.live_regs_by_instruction && ir->ls.live_regs_by_instruction_size > 0)
    memset(ir->ls.live_regs_by_instruction, 0,
           ir->ls.live_regs_by_instruction_size * sizeof(ir->ls.live_regs_by_instruction[0]));
  ir->leaffunc = 1;

  return 1;
}

int tcc_ir_opt_null_store_dom_return_ex(IROptCtx *ctx) { return tcc_ir_opt_null_store_dom_return(ctx->ir); }
