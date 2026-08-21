/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2001-2004 Fabrice Bellard
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

/* block.c -- Statement and compound-block parsing, scopes and VLA handling.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* ------------------------------------------------------------------------- */
/* VLA */

static void vla_restore(int loc)
{
  if (!loc)
    return;

  if (tcc_state->ir)
  {
    tcc_ir_gen_vla_sp_restore(tcc_state->ir, loc);
  }
  else
  {
    gen_vla_sp_restore(loc);
  }
}

static void vla_leave(struct scope *o)
{
  struct scope *c = cur_scope, *v = NULL;
  for (; c != o && c; c = c->prev)
    if (c->vla.num)
      v = c;
  if (v)
    vla_restore(v->vla.locorig);
}
/* ------------------------------------------------------------------------- */
/* local scopes */

static void new_scope(struct scope *o)
{
  /* copy and link previous scope */
  *o = *cur_scope;
  o->prev = cur_scope;
  cur_scope = o;
  /* Reset VLA bookkeeping for the new scope. The scope struct is copied from
   * the parent, so we must clear these fields or we'll restore SP using the
   * parent's slots. */
  cur_scope->vla.num = 0;
  cur_scope->vla.loc = 0;
  cur_scope->vla.locorig = 0;
  /* NOTE: We no longer unconditionally save SP for every scope. A pre-VLA SP
   * save slot is allocated lazily only if/when the first VLA is declared in
   * this scope. */
  /* record local declaration stack position */
  o->lstk = local_stack;
  o->llstk = local_label_stack;
  ++local_scope;
}

static void prev_scope(struct scope *o, int is_expr)
{
  vla_leave(o->prev);

  if (o->cl.s != o->prev->cl.s)
    block_cleanup(o->prev);

  /* pop locally defined labels */
  label_pop(&local_label_stack, o->llstk, is_expr);

  /* In the is_expr case (a statement expression is finished here),
     vtop might refer to symbols on the local_stack.  Either via the
     type or via vtop->sym.  We can't pop those nor any that in turn
     might be referred to.  To make it easier we don't roll back
     any symbols in that case; some upper level call to block() will
     do that.  We do have to remove such symbols from the lookup
     tables, though.  sym_pop will do that.  */

  /* pop locally defined symbols */
  pop_local_syms(o->lstk, is_expr);
  cur_scope = o->prev;
  --local_scope;
}

/* leave a scope via break/continue(/goto) */
static void leave_scope(struct scope *o)
{
  if (!o)
    return;
  try_call_scope_cleanup(o->cl.s);
  vla_leave(o);
}

/* short versiona for scopes with 'if/do/while/switch' which can
   declare only types (of struct/union/enum) */
static void new_scope_s(struct scope *o)
{
  o->lstk = local_stack;
  ++local_scope;
}

static void prev_scope_s(struct scope *o)
{
  sym_pop(&local_stack, o->lstk, 0);
  --local_scope;
}

/* ------------------------------------------------------------------------- */
/* call block from 'for do while' loops */

static void lblock(int *bsym, int *csym)
{
  struct scope *lo = loop_scope, *co = cur_scope;
  int *b = co->bsym, *c = co->csym;
  if (csym)
  {
    co->csym = csym;
    loop_scope = co;
  }
  co->bsym = bsym;
  block(0);
  co->bsym = b;
  if (csym)
  {
    co->csym = c;
    loop_scope = lo;
  }
}

static void block_1(int flags);

/* Wrapper that scopes the variadic struct-argument temp pool to one
 * statement.  Slots reserved while parsing this statement (and its
 * sub-expressions) are released on exit so sibling statements reuse them,
 * but a nested block() — e.g. a GNU statement-expression used as a call
 * argument — saves/restores the mask and so cannot recycle a slot the
 * enclosing call still has in flight. */
void block(int flags)
{
  uint64_t saved_arg_struct_busy = arg_struct_temp_busy;
  block_1(flags);
  arg_struct_temp_busy = saved_arg_struct_busy;
}

static void block_1(int flags)
{
  int a, b, c, d, e, t;
  struct scope o;
  Sym *s;

  if (flags & STMT_EXPR)
  {
    /* default return value is (void) */
    vpushi(0);
    vtop->type.t = VT_VOID;
  }

again:
  t = tok;
  /* If the token carries a value, next() might destroy it. Only with
     invalid code such as f(){"123"4;} */
  if (TOK_HAS_VALUE(t))
    goto expr;
  next();

  if (debug_modes)
    tcc_tcov_check_line(tcc_state, 0), tcc_tcov_block_begin(tcc_state);

  if (t == TOK_IF)
  {
    new_scope_s(&o);
    skip('(');
    gexpr();
    check_nonvoid_value();
    skip(')');
    a = tcc_ir_codegen_test_gen(tcc_state->ir, 1, -1);
    block(0);
    if (tok == TOK_ELSE)
    {
      int if_nocode, else_nocode;
      SValue dest;
      if_nocode = nocode_wanted; /* save reachability after if-body */
      svalue_init(&dest);
      dest.vr = -1;
      dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
      dest.c.i = -1;     /* Will be patched to end of else block */
      d = tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
      tcc_ir_backpatch_to_here(tcc_state->ir, a);
      CODE_ON(); /* Code after if-branch is reachable via else path */
      next();
      block(0);
      else_nocode = nocode_wanted; /* save reachability after else-body */
      tcc_ir_backpatch_to_here(tcc_state->ir, d);
      /* If both branches are unreachable (both returned/broke),
         code after the if-else is also unreachable */
      if ((if_nocode & else_nocode) & CODE_OFF_BIT)
        nocode_wanted |= CODE_OFF_BIT;
      else
        CODE_ON();
    }
    else
    {
      tcc_ir_backpatch_to_here(tcc_state->ir, a);
      CODE_ON(); /* Code after if is reachable when condition is false */
    }
    prev_scope_s(&o);
  }
  else if (t == TOK_WHILE)
  {
    new_scope_s(&o);
    d = gind();
    skip('(');
    gexpr();
    check_nonvoid_value();
    skip(')');
    // fprintf(stderr, "WHILE_COND: file=%s line=%d r=0x%x type=0x%x vr=%d VT_LVAL=%d VT_VALMASK=0x%x btype=0x%x\n",
    //         file->filename, file->line_num, vtop->r, vtop->type.t, vtop->vr, (vtop->r & VT_LVAL) ? 1 : 0,
    //         vtop->r & VT_VALMASK, vtop->type.t & VT_BTYPE);
    // a = gvtst(1, 0);
    a = tcc_ir_codegen_test_gen(tcc_state->ir, 1, -1);
    b = -1; /* Initialize continue chain with -1 sentinel */
    lblock(&a, &b);
    d = gjmp_addr(d);
    // gsym_addr(b, d);
    tcc_ir_backpatch_to_here(tcc_state->ir, a);
    tcc_ir_backpatch(tcc_state->ir, b, d);
    // gsym(a);
    prev_scope_s(&o);
  }
  else if (t == '{')
  {
    if (debug_modes)
      tcc_debug_stabn(tcc_state, N_LBRAC, ind - func_ind);
    new_scope(&o);

    /* handle local labels declarations */
    while (tok == TOK_LABEL)
    {
      do
      {
        next();
        if (tok < TOK_UIDENT)
          expect("label identifier");
        Sym *lbl = label_push(&local_label_stack, tok, LABEL_DECLARED);
        /* Allocate a 40-byte nonlocal-goto jmp_buf on the stack for each
         * __label__. The buffer stores 10 words for non-local goto:
         *   [0-28]: r4-r11 (callee-saved regs), [32]: SP, [36]: resume_addr.
         * This ensures longjmp from a nested function restores all register
         * state correctly, not just FP/SP. */
        if (tcc_state->ir)
        {
          loc = (loc - 40) & ~7; /* 40 bytes, 8-byte aligned */
          lbl->c = loc;          /* store buffer FP offset in label sym */
        }
        next();
      } while (tok == ',');
      skip(';');
    }

    while (tok != '}')
    {
      decl(VT_LOCAL);
      if (tok != '}')
      {
        if (flags & STMT_EXPR)
          vpop();
        block(flags | STMT_COMPOUND);
      }
    }

    prev_scope(&o, flags & STMT_EXPR);
    if (debug_modes)
      tcc_debug_stabn(tcc_state, N_RBRAC, ind - func_ind);
    /* Suppress next() only for the outermost '}' of an inline expansion body.
     * For nested '{...}' blocks inside the inline body, next() must fire so
     * that the enclosing while(tok != '}') loop can continue correctly.
     * in_inline_expansion stores the local_scope level of the inline entry;
     * the outermost '}' is exactly when local_scope equals that level. */
    if (local_scope && !(tcc_state->in_inline_expansion && local_scope == tcc_state->in_inline_expansion))
      next();
    else if (!local_scope)
    {
      /* For main(), always generate return 0 even if nocode_wanted is set
       * (which can happen due to control flow analysis after if/else etc.) */
      if (nocode_wanted && !strcmp(funcname, "main") && (func_vt.t & VT_BTYPE) == VT_INT)
        CODE_ON();
      if (!nocode_wanted)
        check_func_return();
    }
  }
  else if (t == TOK_RETURN)
  {
    b = (func_vt.t & VT_BTYPE) != VT_VOID;
    if (tok != ';')
    {
      gexpr();
      if (b)
      {
        gen_assign_cast(&func_vt);
      }
      else
      {
        if (vtop->type.t != VT_VOID)
          tcc_warning("void function returns a value");
        vtop--;
        print_vstack("block(1)");
      }
    }
    else if (b)
    {
      tcc_warning("'return' with no value");
      b = 0;
    }
    leave_scope(root_scope);
    if (b)
    {
      if (tcc_state->in_inline_expansion)
      {
        if ((func_vt.t & VT_BTYPE) == VT_STRUCT)
        {
          /* Struct return in inline expansion: instead of copying to the
           * return slot, redirect inline_return_loc to point at the source
           * if it's a simple stack local.  This eliminates one memmove —
           * the caller's assignment will copy directly from the inlined
           * function's local variable. */
          if ((vtop->r & (VT_LOCAL | VT_LVAL)) == (VT_LOCAL | VT_LVAL) && vtop->vr == -1 &&
              !tcc_state->inline_return_redirected)
          {
            /* Only the FIRST return may retarget the slot.  Retargeting again
             * would move the caller's read away from the slot the first
             * return left its value in — and that first return emits no copy,
             * so its path would read an uninitialized slot (a two-return
             * `return local;` / `return f();` body did exactly that).  Later
             * returns take the vstore() copy below, which writes into this
             * same retargeted slot. */
            LOG_INLINE_STRUCT("[inline-struct] redirect return: loc %d -> %d", (int)tcc_state->inline_return_loc,
                              (int)vtop->c.i);
            tcc_state->inline_return_loc = vtop->c.i;
            tcc_state->inline_return_redirected = 1;
            vtop--;
          }
          else
          {
            /* Fallback: copy via vstore() when source is not a simple local */
            SValue src_save = *vtop;
            vtop--;
            CValue ret_cv;
            ret_cv.i = tcc_state->inline_return_loc;
            vsetc(&func_vt, VT_LOCAL | VT_LVAL, &ret_cv);
            vtop->vr = -1;
            vpushv(&src_save);
            vstore();
            vtop--;
          }
        }
        else
        {
          /* Inside inline expansion: store return value to local slot
           * instead of emitting RETURNVALUE IR op.
           * Must materialize VT_CMP/VT_JMP (comparison flags) into a 0/1
           * register value before the STORE, just like gfunc_return does
           * via tcc_ir_codegen_cmp_jmp_set.  Without this, a return of a
           * comparison expression (e.g. "return a >= b;") would store the
           * raw operand register instead of the boolean result. */
          tcc_ir_codegen_cmp_jmp_set(tcc_state->ir);
          /* If vtop is an lval (e.g. "return *p;" or "return x;" where x is
           * a local), emit an explicit LOAD into a temp first.  Without this
           * the resulting STORE would carry a DEREF source operand, which
           * the 64-bit store backend cannot split via mach_make_hi_half
           * (it expects a register pair, not a pointer).  Mirrors the
           * LOAD step in gfunc_return for the non-inline return path. */
          if (vtop->r & VT_LVAL)
          {
            SValue load_dst;
            svalue_init(&load_dst);
            load_dst.type = vtop->type;
            load_dst.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
            load_dst.r = 0;
            load_dst.c.i = 0;
            tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, vtop, NULL, &load_dst);
            vtop->vr = load_dst.vr;
            vtop->r = 0;
          }
          SValue ret_dst;
          svalue_init(&ret_dst);
          ret_dst.type = func_vt;
          /* The inline return slot is allocated at a minimum of 4 bytes (see
           * the rsize clamp where inline_ret_loc is carved out), and the
           * consumer of the expanded call reads it as a word.  Storing a
           * narrow return type at its own width left the upper bytes of the
           * slot uninitialised, so the reader tested stack garbage:
           *   strb rX,[sp,#4] ... ldr rX,[sp,#4]; cmp rX,#0
           * made `if (u8_returning_fn(...))` a coin flip.  The value is
           * already zero/sign-extended in its register, so writing the whole
           * word is correct for a narrow read of the slot as well as for the
           * word read the consumer actually performs. */
          {
            int ret_bt = ret_dst.type.t & VT_BTYPE;
            if (ret_bt == VT_BOOL || ret_bt == VT_BYTE || ret_bt == VT_SHORT)
              ret_dst.type.t = (ret_dst.type.t & ~VT_BTYPE) | VT_INT;
          }
          ret_dst.r = VT_LOCAL | VT_LVAL;
          /* The vreg (when the call site bound one) makes this a write to a
           * compiler-generated local rather than to a raw frame slot, so the
           * allocator can keep the inlined return value in a register. */
          ret_dst.vr = tcc_state->inline_return_vr;
          ret_dst.c.i = tcc_state->inline_return_loc;
          tcc_ir_put(tcc_state->ir, TCCIR_OP_STORE, vtop, NULL, &ret_dst);
          vtop--;
        }
      }
      else
      {
        gfunc_return(&func_vt);
      }
    }
    skip(';');
    /* jump unless last stmt in top-level block */
    if (tok != '}' || local_scope != 1)
    {
      SValue dest;
      svalue_init(&dest);
      dest.vr = -1;
      dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
      dest.c.i = rsym;   /* Chain return jumps: point to previous rsym */
      rsym = tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
      // rsym = gjmp(rsym);
    }
    if (debug_modes)
      tcc_tcov_block_end(tcc_state, -1);
    CODE_OFF();
  }
  else if (t == TOK_BREAK)
  {
    /* compute jump */
    SValue dest;
    if (!cur_scope->bsym)
      tcc_error("cannot break");
    if (cur_switch && cur_scope->bsym == cur_switch->bsym)
      leave_scope(cur_switch->scope);
    else
      leave_scope(loop_scope);
    svalue_init(&dest);
    dest.vr = -1;
    dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
    dest.c.i = *cur_scope->bsym;
    *cur_scope->bsym = tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
    // *cur_scope->bsym = gjmp(*cur_scope->bsym);
    skip(';');
  }
  else if (t == TOK_CONTINUE)
  {
    /* compute jump */
    SValue dest;
    if (!cur_scope->csym)
      tcc_error("cannot continue");
    leave_scope(loop_scope);
    svalue_init(&dest);
    dest.vr = -1;
    dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
    dest.c.i = *cur_scope->csym;
    // *cur_scope->csym = gjmp(*cur_scope->csym);
    *cur_scope->csym = tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
    skip(';');
  }
  else if (t == TOK_FOR)
  {
    int saved_line_num;
    new_scope(&o);

    skip('(');
    if (tok != ';')
    {
      /* c99 for-loop init decl? */
      if (!decl(VT_JMP))
      {
        /* no, regular for-loop init expr */
        gexpr();
        gv_discarded_volatile();
        vpop();
      }
    }
    skip(';');
    a = b = -1; /* Initialize break/continue chains with -1 sentinel */
    c = d = gind();
    if (tok != ';')
    {
      gexpr();
      check_nonvoid_value();
      a = tcc_ir_codegen_test_gen(tcc_state->ir, 1, -1);
    }
    skip(';');
    if (tok != ')')
    {
      // e = gjmp(0);
      SValue dest;
      svalue_init(&dest);
      dest.vr = -1;
      dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
      dest.c.i = -1;
      e = tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
      // d = gind();
      c = tcc_state->ir->next_instruction_index;
      gexpr();
      gv_discarded_volatile();
      vpop();
      gjmp_addr(d);
      tcc_ir_backpatch_to_here(tcc_state->ir, e);
      // gsym(e);
    }
    skip(')');
    /* Save line number before loop body for backward jump */
    saved_line_num = file->line_num;
    lblock(&a, &b);
    /* Temporarily restore line number for backward jump instruction */
    {
      int cur_line = file->line_num;
      file->line_num = saved_line_num;
      d = gjmp_addr(c);
      file->line_num = cur_line;
    }
    tcc_ir_backpatch_to_here(tcc_state->ir, a);
    tcc_ir_backpatch(tcc_state->ir, b, c);
    /* If there was no exit condition and no break (a == -1 after lblock),
       the loop is infinite and code after it is unreachable. */
    if (a == -1)
      nocode_wanted |= CODE_OFF_BIT;
    // gsym_addr(b, d);
    // gsym(a);
    prev_scope(&o, 0);
  }
  else if (t == TOK_DO)
  {
    new_scope_s(&o);
    a = b = -1; /* Initialize break/continue chains with -1 sentinel */
    d = gind();
    lblock(&a, &b);
    /* continue jumps land at the condition check of the do/while */
    tcc_ir_backpatch_to_here(tcc_state->ir, b);
    skip(TOK_WHILE);
    skip('(');
    gexpr();
    check_nonvoid_value();
    skip(')');
    skip(';');
    // c = gvtst(0, 0);
    c = tcc_ir_codegen_test_gen(tcc_state->ir, 0, -1);

    // gsym_addr(c, d);
    tcc_ir_backpatch(tcc_state->ir, c, d);
    // gsym(a);
    tcc_ir_backpatch_to_here(tcc_state->ir, a);
    prev_scope_s(&o);
  }
  else if (t == TOK_SWITCH)
  {
    struct switch_t *sw;
    SValue dest;

    sw = tcc_mallocz(sizeof *sw);
    sw->bsym = &a;
    sw->scope = cur_scope;
    sw->prev = cur_switch;
    sw->nocode_wanted = nocode_wanted;
    cur_switch = sw;

    new_scope_s(&o);
    skip('(');
    gexpr();
    skip(')');
    if (!is_integer_btype(vtop->type.t & VT_BTYPE))
      tcc_error("switch value not an integer");
    sw->sv = *vtop--; /* save switch value */
    print_vstack("block(2)");
    a = -1; /* Initialize break chain with -1 sentinel */
    svalue_init(&dest);
    dest.vr = -1;
    dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
    dest.c.i = -1;     /* Initial jump target, will be patched */
    b = tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
    // b = gjmp(0); /* jump to first case */
    lblock(&a, NULL);
    /* If the switch has a default label, no explicit breaks were emitted
     * (a == -1), and the last case ends with dead code (return/goto/continue),
     * then ALL paths through the switch exit without reaching code after it.
     * Must be checked before the implicit break overwrites 'a'. */
    int switch_exits_all = sw->def_sym && (a == -1) && (nocode_wanted & CODE_OFF_BIT);
    dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
    dest.c.i = a;
    a = tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
    // a = gjmp(a); /* add implicit break */
    /* case lookup */
    // gsym(b);

    prev_scope_s(&o);
    if (sw->nocode_wanted)
      goto skip_switch;
    case_sort(sw);
    sw->bsym = NULL; /* marker for 32bit:gen_opl() */
    vpushv(&sw->sv);
    // gv(RC_INT);
    c = tcc_state->ir->next_instruction_index; /* save start of case comparisons */
    /* The switch value is copied into a temporary vreg used by the case
      comparison chain.  When it already IS a plain temp or register-parameter
      vreg the copy is pure overhead — it is never coalesced away, so it costs
      a `mov` and a second live range spanning the whole switch body (the
      dispatch block sits after the bodies).  Reuse the vreg directly there.

      Three exclusions:
        - anything narrower than a register.  The copy is what widens a
          `char`/`short` switch value once; reused, every node of the compare
          chain re-materialises the extension (pr48809::foo +82, pr91632::foo
          +16 instructions without this guard), and the widened temp is also
          what lets const-prop collapse a constant-argument call;
        - a VAR vreg, which keeps its own identity through the body — the copy
          is what pins the dispatch to the entry value;
        - sw->n == 0, where `c` would otherwise point at whatever follows the
          empty comparison chain instead of a real landing instruction. */
    int use_jump_table = switch_can_use_jump_table(sw);
    int sv_vreg_type = vtop->vr >= 0 ? TCCIR_DECODE_VREG_TYPE(vtop->vr) : -1;
    int sv_btype = vtop->type.t & VT_BTYPE;
    int sv_full_width = (sv_btype == VT_INT || sv_btype == VT_LLONG || sv_btype == VT_PTR);
    int reuse_switch_vreg = sw->n > 0 && sv_full_width && !(vtop->type.t & VT_BITFIELD) &&
                            !tcc_ir_operand_needs_dereference(vtop) &&
                            (sv_vreg_type == TCCIR_VREG_TYPE_TEMP ||
                             (sv_vreg_type == TCCIR_VREG_TYPE_PARAM && !(vtop->r & VT_LOCAL)));
    if (!reuse_switch_vreg)
    {
      svalue_init(&dest);
      dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      /* Preserve the original type so the IR can tag the vreg correctly
        (notably VT_LLONG needs 8-byte spill slots). */
      dest.type = vtop->type;
      tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, vtop, NULL, &dest);
      vtop->vr = dest.vr;
      vtop->r = 0;
    }
    /* Force bitfield extraction before switch comparison / jump table.
     * The ASSIGN above copies the raw containing word into the temp vreg;
     * the bitfield bits must be extracted (SHL+SAR) now so that ALL
     * subsequent uses — bounds check AND SWITCH_TABLE — operate on the
     * extracted integer, not the full word.  Without this, the bounds
     * check extracts on a duplicated copy while SWITCH_TABLE still sees
     * the unextracted full word, causing a wild jump. */
    if (vtop->type.t & VT_BITFIELD)
      gv(RC_INT);
    /* Build case jump chain; start with empty default chain (-1).
     * Use jump table for dense switches, otherwise fall back to binary search. */
    int switch_table_id = -1;
    if (use_jump_table)
    {
      switch_table_id = tcc_state->ir->num_switch_tables; /* ID of the table about to be created */
      d = gcase_jump_table(sw, -1);
    }
    else
    {
      d = gcase(sw->p, sw->n, -1);
    }
    vpop();

    tcc_ir_backpatch(tcc_state->ir, b, c);
    int def_target;
    if (sw->def_sym)
    {
      tcc_ir_backpatch(tcc_state->ir, d, sw->def_sym);
      def_target = sw->def_sym;
    }
    else
    {
      tcc_ir_backpatch_to_here(tcc_state->ir, d);
      def_target = tcc_state->ir->next_instruction_index;
    }
    /* Resolve switch table default entries: gcase_jump_table() initially sets
     * default entries to -1 (unresolved forward reference).  Now that the
     * default label is known, update those entries so the codegen backpatcher
     * can emit correct PC-relative offsets instead of falling back to the
     * epilogue address. */
    if (switch_table_id >= 0)
    {
      TCCIRSwitchTable *table = &tcc_state->ir->switch_tables[switch_table_id];
      table->default_target = def_target;
      for (int k = 0; k < table->num_entries; k++)
      {
        if (table->targets[k] < 0)
          table->targets[k] = def_target;
      }
    }
    // gsym(d);
  skip_switch:
    /* break label */
    // gsym(a);
    tcc_ir_backpatch_to_here(tcc_state->ir, a);
    /* If every path through the switch exits (has default, no breaks, last
     * case is dead code), code after the switch is unreachable. Restore
     * CODE_OFF so that check_func_return() is not triggered spuriously. */
    if (switch_exits_all)
      CODE_OFF();
    end_switch();
  }
  else if (t == TOK_CASE)
  {
    struct case_t *cr;
    if (!cur_switch)
      expect("switch");
    cr = tcc_malloc(sizeof(struct case_t));
    dynarray_add(&cur_switch->p, &cur_switch->n, cr);
    t = cur_switch->sv.type.t;
    cr->v1 = cr->v2 = value64(expr_const64(), t);
    if (tok == TOK_DOTS && gnu_ext)
    {
      next();
      cr->v2 = value64(expr_const64(), t);
      if (case_cmp(cr->v2, cr->v1) < 0)
        tcc_warning("empty case range");
    }
    /* case and default are unreachable from a switch under nocode_wanted */
    if (!cur_switch->nocode_wanted)
      cr->ind = gind();
    cr->line = file->line_num;
    skip(':');
    goto block_after_label;
  }
  else if (t == TOK_DEFAULT)
  {
    if (!cur_switch)
      expect("switch");
    if (cur_switch->def_sym)
      tcc_error("too many 'default'");
    cur_switch->def_sym = cur_switch->nocode_wanted ? -1 : gind();
    skip(':');
    goto block_after_label;
  }
  else if (t == TOK_GOTO)
  {
    vla_restore(cur_scope->vla.locorig);
    if (tok == '*' && gnu_ext)
    {
      /* computed goto */
      next();
      gexpr();
      if ((vtop->type.t & VT_BTYPE) != VT_PTR)
        expect("pointer");
      ggoto();
    }
    else if (tok >= TOK_UIDENT)
    {
      /* Check for non-local goto from nested function to parent __label__ */
      NestedFunc *cur_nf = tcc_state->current_nested_func;
      int is_nonlocal_goto = 0;
      if (cur_nf && tcc_state->ir)
      {
        for (int ngi = 0; ngi < cur_nf->nb_nlgotos; ngi++)
        {
          if (cur_nf->nlgoto_label_tokens[ngi] == tok)
          {
            /* This is a non-local goto - emit longjmp to parent's jmp_buf.
             * The jmp_buf is accessed as a captured variable via the static chain. */
            int buf_off = cur_nf->nlgoto_buf_offsets[ngi];

            /* Create SValue for buffer address via chain-relative access.
             * vreg=-1 + VT_LOCAL + is_lval=false → MACH_OP_CHAIN_REL with no deref,
             * giving us the address of the buffer in the parent's frame. */
            SValue buf_sv;
            svalue_init(&buf_sv);
            buf_sv.type.t = VT_INT;
            buf_sv.type.ref = NULL;
            buf_sv.r = VT_LOCAL;  /* FP-relative in parent, becomes chain-relative */
            buf_sv.c.i = buf_off; /* parent's FP offset of the jmp_buf */
            buf_sv.vr = -1;       /* no vreg = pure stack offset → triggers CHAIN_REL */

            /* Emit NL_LONGJMP: restores callee-saved regs, SP from 40-byte buffer and jumps to resume addr */
            tcc_ir_put(tcc_state->ir, TCCIR_OP_NL_LONGJMP, &buf_sv, NULL, NULL);
            /* longjmp doesn't return - mark code as dead */
            CODE_OFF();
            is_nonlocal_goto = 1;
            next();
            break;
          }
        }
      }

      if (!is_nonlocal_goto)
      {
        s = label_find(tok);
        /* put forward definition if needed */
        if (!s)
          s = label_push(&global_label_stack, tok, LABEL_FORWARD);
        else if (s->r == LABEL_DECLARED)
          s->r = LABEL_FORWARD;

        if (s->r & LABEL_FORWARD)
        {
          /* start new goto chain for cleanups, linked via label->next */
          if (cur_scope->cl.s && !nocode_wanted)
          {
            sym_push2(&pending_gotos, SYM_FIELD, 0, cur_scope->cl.n);
            pending_gotos->prev_tok = s;
            s = sym_push2(&s->next, SYM_FIELD, 0, 0);
            /* sym_push2 zeroes the Sym, but 0 is a valid IR instruction index:
             * an empty chain must be -1 (what label_push stores), otherwise
             * gjmp() links this goto's JMP to instruction 0 and the label's
             * backpatch walks into it.  That is harmless while instruction 0
             * is not a jump, but at -O1+ a const-folded leading `if (f())`
             * makes it one, and it gets silently retargeted at the label. */
            s->jnext = -1;
            pending_gotos->next = s;
          }
          s->jnext = gjmp(s->jnext);
        }
        else
        {
          try_call_cleanup_goto(s->cleanupstate);
          gjmp_addr(s->jind);
        }
        next();
      } /* !is_nonlocal_goto */
    }
    else
    {
      expect("label identifier");
    }
    skip(';');
  }
  else if (t == TOK_ASM1 || t == TOK_ASM2 || t == TOK_ASM3)
  {
    asm_instr();
  }
  else
  {
    if (tok == ':' && t >= TOK_UIDENT)
    {
      /* label case */
      next();
      s = label_find(t);
      if (s)
      {
        if (s->r == LABEL_DEFINED)
          tcc_error("duplicate label '%s'", get_tok_str(s->v, NULL));
        s->r = LABEL_DEFINED;
        if (s->next)
        {
          Sym *pcl; /* pending cleanup goto */
          for (pcl = s->next; pcl; pcl = pcl->prev)
            if (pcl->jnext >= 0) /* Only backpatch if there's an actual forward jump */
              tcc_ir_backpatch_to_here(tcc_state->ir, pcl->jnext);
          sym_pop(&s->next, NULL, 0);
        }
        else if (s->jnext >= 0) /* Only backpatch if there's an actual forward jump */
          tcc_ir_backpatch_to_here(tcc_state->ir, s->jnext);
      }
      else
      {
        s = label_push(&global_label_stack, t, LABEL_DEFINED);
      }
      s->jind = gind();
      s->cleanupstate = cur_scope->cl.s;

    block_after_label:
    {
      /* Accept attributes after labels (e.g. 'unused') */
      AttributeDef ad_tmp;
      parse_attribute(&ad_tmp);
    }
      if (debug_modes)
        tcc_tcov_reset_ind(tcc_state);
      vla_restore(cur_scope->vla.locorig);

      if (tok != '}')
      {
        if (0 == (flags & STMT_COMPOUND))
          goto again;
        /* C23: insert implicit null-statement whithin compound statement */
      }
      else
      {
        /* we accept this, but it is a mistake */
        tcc_warning_c(warn_all)("deprecated use of label at end of compound statement");
      }
    }
    else
    {
      /* expression case */
      if (t != ';')
      {
        unget_tok(t);
      expr:
        if (flags & STMT_EXPR)
        {
          vpop();
          gexpr();
        }
        else
        {
          gexpr();
          tcc_ir_codegen_drop_return(tcc_state->ir);
          gv_discarded_volatile();
          vpop();
        }
        skip(';');
      }
    }
  }

  if (debug_modes)
    tcc_tcov_check_line(tcc_state, 0), tcc_tcov_block_end(tcc_state, 0);
}
