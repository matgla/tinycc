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

/* atomic.c -- C11 atomic builtin parsing.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

#define __ATOMIC_SEQ_CST_VALUE 5 /* the memory orders as <stdatomic.h> numbers them */

/* A memory barrier: a call to __tcc_dmb, which the ARM backend emits as one
 * DMB instead (tcc_gen_machine_func_call_mop).  Every pass already treats a
 * call as reading and writing any memory, so nothing moves across it. */
static void gen_atomic_barrier(void)
{
  const int call_id = tcc_state->ir->next_call_id++;
  SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, 0);
  vpush_helper_func(tok_alloc_const("__tcc_dmb"));
  tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVOID, vtop, &call_id_sv, NULL);
  vpop();
}

/* __atomic_load/__atomic_store of a 1-, 2- or 4-byte object, inline: a plain
 * LDR/STR of those sizes is single-copy atomic on ARMv7-M/ARMv8-M, and the
 * ordering is the standard C11 mapping for the architecture --
 *   load:  relaxed: ldr;  acquire/consume: ldr; dmb;  seq_cst: ldr; dmb
 *   store: relaxed: str;  release: dmb; str;  seq_cst: dmb; str; dmb
 * with an order that is not a constant, or not valid for the access, taken
 * as seq_cst.  A store, and a relaxed load, is a volatile access, so it is
 * neither dropped, merged nor moved out of a loop.  An ordered load needs no
 * such mark: the barrier call after it already reads and writes all memory,
 * so no pass merges it with another load or hoists it, and a volatile access
 * would make every pass treat each unmarked access of the whole function (of
 * any caller it is inlined into) as possibly volatile -- Zig's InternPool
 * accessors do an acquire load on every lookup.
 * Read-modify-write stays a call unless -minline-atomics: an LDREX/STREX
 * loop never completes on the RP2350's PSRAM, so how to make it atomic is the
 * runtime's to decide (inline_atomic_rmw).
 * vtop holds the arguments the template left: [ptr, order] for a load,
 * [ptr, value, order] for a store.  Returns 0 to fall back to the call. */
static int inline_atomic_access(int atok, int size, CType *atom)
{
#if defined TCC_TARGET_ARM
  if (!tcc_state->ir || NOEVAL_WANTED || (size != 1 && size != 2 && size != 4) ||
      (atok != TOK___atomic_load && atok != TOK___atomic_load_n && atok != TOK___atomic_store))
    return 0;
  int order = __ATOMIC_SEQ_CST_VALUE;
  if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
    order = (int)vtop->c.i;
  vpop();
  CType vol = *atom;
  vol.t |= VT_VOLATILE;
  /* An acquire load and a release store are one LDA / STL on ARMv8-M: a
   * call the backend emits as that instruction where its operands are
   * (thumb_inline_atomic_call), which writes no register but the loaded
   * value, and to every pass reads and writes memory as the barrier did.
   * Seq_cst keeps the DMB mapping it shares with code built before. */
  if ((atok == TOK___atomic_load || atok == TOK___atomic_load_n) && (order == 1 || order == 2))
  {
    char name[32];
    snprintf(name, sizeof(name), "__tcc_ax_lda%d_1", size);
    gen_internal_call(name, 1, atom);
    return 1;
  }
  if (atok == TOK___atomic_store && order == 3)
  {
    char name[32];
    CType void_type = {.t = VT_VOID};
    snprintf(name, sizeof(name), "__tcc_ax_stl%d_2", size);
    gen_internal_call(name, 2, &void_type);
    vpop();
    return 1;
  }
  if (atok == TOK___atomic_load || atok == TOK___atomic_load_n)
  {
    if (order != 0 && order != 1 && order != 2)
      order = __ATOMIC_SEQ_CST_VALUE;
    if (order != 0 && !(atom->t & VT_VOLATILE))
      vol.t &= ~VT_VOLATILE;
    mk_pointer(&vol);
    vtop->type = vol;
    indir();
    gv(RC_INT);
    if (order != 0)
      gen_atomic_barrier();
    return 1;
  }
  if (order != 0 && order != 3)
    order = __ATOMIC_SEQ_CST_VALUE;
  if (order != 0)
    gen_atomic_barrier();
  vswap();
  mk_pointer(&vol);
  vtop->type = vol;
  indir();
  vswap();
  vstore();
  vpop();
  if (order == __ATOMIC_SEQ_CST_VALUE)
    gen_atomic_barrier();
  return 1;
#else
  (void)atok;
  (void)size;
  (void)atom;
  return 0;
#endif
}

/* The memory order on top of the stack, popped: a constant, or seq_cst. */
static int atomic_pop_order(void)
{
  int order = __ATOMIC_SEQ_CST_VALUE;
  if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
    order = (int)vtop->c.i;
  vpop();
  return order < 0 || order > __ATOMIC_SEQ_CST_VALUE ? __ATOMIC_SEQ_CST_VALUE : order;
}

/* The ordering an inline read-modify-write needs, as the flags of its
 * helper's name (tcc_gen_machine_func_call_mop reads them back):
 *   1 acquire: LDAEX (consume, acquire, acq_rel, seq_cst)
 *   2 release: STLEX (release, acq_rel, seq_cst)
 *   4 seq_cst: and a DMB after -- STLEX does not order a later plain LDR,
 *     and a seq_cst load here is `ldr; dmb` (inline_atomic_access). */
static int atomic_order_flags(int order)
{
  static const unsigned char flags[] = {0, 1, 1, 2, 3, 7};
  return flags[order];
}

/* Pops the value on top of the stack into a register and returns it. */
static SValue atomic_pop_reg(void)
{
  gv(RC_INT);
  SValue v = *vtop;
  vpop();
  return v;
}

/* -minline-atomics: __atomic_compare_exchange, __atomic_exchange and the
 * fetch-and-op family on a 1-, 2- or 4-byte integer or pointer, inline.  The
 * frontend still emits a call -- to __tcc_ax_<op><size>_<flags>, with the
 * pointer in R0 and the operands in R1 (and R2) -- so every pass treats it
 * as one that reads and writes any memory; the ARM backend then emits an
 * LDREX/STREX loop in place of the BL that returns the old value in R0
 * (thumb_inline_atomic_call).  Off by default: an exclusive store to the
 * RP2350's PSRAM never succeeds, so only code that keeps its atomics in SRAM
 * -- the YasOS kernel -- may opt in; everything else calls the runtime.
 * vtop holds the arguments the template left: [ptr, expected-ptr, desired,
 * weak, success, failure] for a compare-exchange, [ptr, value, order] for the
 * rest (__atomic_exchange's result pointer is already off the stack).
 * Returns 0 to fall back to the call. */
static int inline_atomic_rmw_ok(int atok, int size, CType *atom)
{
#if defined TCC_TARGET_ARM
  const int bt = atom->t & VT_BTYPE;
  return tcc_state->inline_atomics && tcc_state->ir && !NOEVAL_WANTED && (size == 1 || size == 2 || size == 4) &&
         atok >= TOK___atomic_exchange && atok <= TOK___atomic_nand_fetch &&
         (bt == VT_BYTE || bt == VT_SHORT || bt == VT_INT || bt == VT_BOOL || bt == VT_PTR);
#else
  (void)atok;
  (void)size;
  (void)atom;
  return 0;
#endif
}

/* A pointer argument the inline expansions dereference and nothing else:
 * the expected pointer of a compare-exchange (read, and written back on
 * failure), and the value pointer of an exchange, compare-exchange or store
 * (read once).  True when that expansion is inline, so parse_atomic may keep
 * its `&local` unmarked; a store's value is inline whatever
 * -minline-atomics says (inline_atomic_access), the rest only under it. */
static int atomic_ptr_arg_inline(char spec, int atok, int size, CType *atom)
{
#if defined TCC_TARGET_ARM
  if (spec == 'p')
    return atok == TOK___atomic_compare_exchange && inline_atomic_rmw_ok(atok, size, atom);
  if (spec == 'l')
    return inline_atomic_rmw_ok(atok, size, atom) ||
           (atok == TOK___atomic_store && tcc_state->ir && !NOEVAL_WANTED &&
            (size == 1 || size == 2 || size == 4));
#else
  (void)spec;
  (void)atok;
  (void)size;
  (void)atom;
#endif
  return 0;
}

static int inline_atomic_rmw(int atok, int size, CType *atom, CType *ct)
{
#if defined TCC_TARGET_ARM
  static const char *const ops[] = {"xchg", "cas", "add", "sub", "or", "xor", "and", "nand",
                                    "add", "sub", "or", "xor", "and", "nand"};
  if (!inline_atomic_rmw_ok(atok, size, atom))
    return 0;
  char name[40];
  if (atok == TOK___atomic_compare_exchange)
  {
    int flags = atomic_order_flags(atomic_pop_order());
    flags |= atomic_order_flags(atomic_pop_order());
    vpop(); /* weak: a strong compare-exchange is a valid weak one */
    SValue desired = atomic_pop_reg();
    /* `&local` stays an address (parse_atomic kept the local unmarked):
     * *expected is then the variable itself. */
    SValue expected_ptr;
    if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_LOCAL)
    {
      expected_ptr = *vtop;
      vpop();
    }
    else
      expected_ptr = atomic_pop_reg();
    SValue ptr = atomic_pop_reg();
    /* *expected is a plain object; tcc's _Atomic is VT_VOLATILE, which the
     * cast to the atomic's pointer type gave it. */
    vpushv(&expected_ptr);
    indir();
    vtop->type.t &= ~VT_VOLATILE;
    SValue expected = atomic_pop_reg();
    vpushv(&ptr);
    vpushv(&expected);
    vpushv(&desired);
    snprintf(name, sizeof(name), "__tcc_ax_cas%d_%d", size, flags);
    gen_internal_call(name, 3, atom);
    gen_cast(atom); /* the helper's zero extension, to compare with expected */
    SValue old = atomic_pop_reg();
    /* Success is old == expected; a failure writes old back to *expected. */
    vpushv(&old);
    vpushv(&expected);
    gen_op(TOK_EQ);
    SValue ok = atomic_pop_reg();
    vpushv(&old);
    vpushv(&expected);
    gen_op(TOK_NE);
    int skip = tcc_ir_codegen_test_gen(tcc_state->ir, 1, -1);
    vpushv(&expected_ptr);
    indir();
    vtop->type.t &= ~VT_VOLATILE;
    vpushv(&old);
    vstore();
    vpop();
    tcc_ir_backpatch_to_here(tcc_state->ir, skip);
    vpushv(&ok);
    return 1;
  }
  const int flags = atomic_order_flags(atomic_pop_order());
  const int op_fetch = atok >= TOK___atomic_add_fetch;
  SValue value = atomic_pop_reg();
  SValue ptr = atomic_pop_reg();
  vpushv(&ptr);
  vpushv(&value);
  snprintf(name, sizeof(name), "__tcc_ax_%s%d_%d", ops[atok - TOK___atomic_exchange], size, flags);
  gen_internal_call(name, 2, atom);
  if (op_fetch)
  {
    /* The new value, from the old one. */
    static const int arith[] = {'+', '-', '|', '^', '&', '&'};
    const int k = atok - TOK___atomic_add_fetch;
    gen_cast(ct);
    vpushv(&value);
    gen_op(arith[k]);
    if (atok == TOK___atomic_nand_fetch)
    {
      vpushi(-1);
      gen_op('^');
    }
  }
  return 1;
#else
  (void)atok;
  (void)size;
  (void)atom;
  (void)ct;
  return 0;
#endif
}

/* A call of NAME taking the CALL_ARGC values on top of the stack as its
 * arguments.  Leaves the result, of type CT, on top: a constant 0 for a void
 * CT, else the returned register, narrow types marked for the extension the
 * helpers do not promise.  Also the call an inline asm of system instructions
 * becomes (tccasm.c). */
ST_FUNC void gen_internal_call(const char *name, int call_argc, CType *ct)
{
  const int stack_count = call_argc + 1;
  const int call_id = tcc_state->ir ? tcc_state->ir->next_call_id++ : 0;
  SValue param_num;
  SValue call_id_sv;
  int t;

  vpush_helper_func(tok_alloc_const(name));
  vrott(stack_count);

  svalue_init(&param_num);
  param_num.vr = -1;
  param_num.r = VT_CONST;
  for (t = 0; t < call_argc; ++t)
  {
    param_num.c.i = TCCIR_ENCODE_PARAM(call_id, t);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[-call_argc + 1 + t], &param_num, NULL);
  }

  call_id_sv = tcc_ir_svalue_call_id_argc(call_id, call_argc);
  /* A call without arguments still needs its call site (as gfunc_call). */
  if (!call_argc)
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVOID, NULL, &call_id_sv, NULL);
  if ((ct->t & VT_BTYPE) == VT_VOID)
  {
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVOID, &vtop[-call_argc], &call_id_sv, NULL);
    vtop -= stack_count;
    vpushi(0);
    vtop->type = *ct;
    vtop->r = VT_CONST;
    return;
  }

  SValue dest;
  svalue_init(&dest);
  dest.type = *ct;
  dest.r = 0;
  dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
  tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVAL, &vtop[-call_argc], &call_id_sv, &dest);

  vtop -= stack_count;
  vpushi(0);
  vtop->type = *ct;
  vtop->vr = dest.vr;
  PUT_R_RET(vtop, ct->t);
  t = ct->t & VT_BTYPE;
  if (t == VT_BYTE || t == VT_SHORT || t == VT_BOOL)
  {
#ifdef PROMOTE_RET
    vtop->r |= BFVAL(VT_MUSTCAST, 1);
#else
    vtop->type.t = VT_INT;
#endif
  }
}

void parse_atomic(int atok)
{
  int size, align, arg, save = 0;
  CType *atom, *atom_ptr, ct = {0};
  SValue store;
  char buf[40];
  static const char *const templates[] = {/*
                                           * Each entry consists of callback and function template.
                                           * The template represents argument types and return type.
                                           *
                                           * ? void (return-only)
                                           * b bool
                                           * a atomic
                                           * A read-only atomic
                                           * p pointer to memory
                                           * v value
                                           * l load pointer
                                           * s save pointer
                                           * m memory model
                                           */

                                          /* keep in order of appearance in tcctok.h: */
                                          /* __atomic_store */ "alm.?",
                                          /* __atomic_load */ "Asm.v",
                                          /* __atomic_load_n */ "Am.v",
                                          /* __atomic_exchange */ "alsm.v",
                                          /* __atomic_compare_exchange */ "aplbmm.b",
                                          /* __atomic_fetch_add */ "avm.v",
                                          /* __atomic_fetch_sub */ "avm.v",
                                          /* __atomic_fetch_or */ "avm.v",
                                          /* __atomic_fetch_xor */ "avm.v",
                                          /* __atomic_fetch_and */ "avm.v",
                                          /* __atomic_fetch_nand */ "avm.v",
                                          /* __atomic_and_fetch */ "avm.v",
                                          /* __atomic_sub_fetch */ "avm.v",
                                          /* __atomic_or_fetch */ "avm.v",
                                          /* __atomic_xor_fetch */ "avm.v",
                                          /* __atomic_and_fetch */ "avm.v",
                                          /* __atomic_nand_fetch */ "avm.v",
                                          /* __atomic_store_n */ "avm.?",
                                          /* __atomic_exchange_n */ "avm.v",
                                          /* __atomic_compare_exchange_n */ "apvbmm.b"};
  const char *template = templates[(atok - TOK___atomic_store)];
  /* The generic form of a value form (see below). */
  const int atok_n = atok == TOK___atomic_compare_exchange_n ? TOK___atomic_compare_exchange : atok;

  atom = atom_ptr = NULL;
  size = 0; /* pacify compiler */
  next();
  skip('(');
  for (arg = 0;;)
  {
    /* The pointer arguments an inline expansion dereferences and nothing
     * else (atomic_ptr_arg_inline): usually `&local`.  Taking the address
     * would keep the local in memory for good -- a Zig optional on the spin
     * lock's path stayed a stack round trip, its test a 0/1 value, and a
     * compare-exchange's desired did the same -- so the locals unary &
     * names here stay unmarked if the argument comes out as just their
     * address, with no code for it; anything else marks them. */
    const int defer = atomic_ptr_arg_inline(template[arg], atok_n, size, atom);
    const int insns_before = defer ? tcc_state->ir->next_instruction_index : 0;
    /* An atomic inside an atomic's argument: keep the outer argument's list. */
    const unsigned char outer_defer = tcc_state->defer_addrtaken, outer_nb = tcc_state->nb_deferred_addrtaken;
    Sym *outer_syms[4];
    memcpy(outer_syms, tcc_state->deferred_addrtaken, sizeof(outer_syms));
    tcc_state->defer_addrtaken = defer;
    tcc_state->nb_deferred_addrtaken = 0;
    expr_eq();
    if (defer)
    {
      /* Pure: one & of one local, whose address is the one instruction the
       * argument made -- its LEA, still in vtop.  Then the LEA goes and the
       * argument becomes the local's address again, never materialized. */
      TCCIRState *ir = tcc_state->ir;
      IRQuadCompact *lea = ir->next_instruction_index == insns_before + 1 ? &ir->compact_instructions[insns_before] : NULL;
      const int pure = tcc_state->nb_deferred_addrtaken == 1 && lea && lea->op == TCCIR_OP_LEA &&
                       !(vtop->r & VT_LVAL) && (vtop->r & VT_VALMASK) != VT_CONST && vtop->vr >= 0 &&
                       irop_get_vreg(tcc_ir_op_get_dest(ir, lea)) == vtop->vr &&
                       tcc_state->deferred_addr_lval.sym == tcc_state->deferred_addrtaken[0];
      if (pure)
      {
        CType ptr_type = vtop->type;
        lea->op = TCCIR_OP_NOP;
        *vtop = tcc_state->deferred_addr_lval;
        vtop->r &= ~VT_LVAL;
        vtop->type = ptr_type;
      }
      else
        for (int k = 0; k < tcc_state->nb_deferred_addrtaken; k++)
        {
          tcc_state->deferred_addrtaken[k]->a.addrtaken = 1;
          tcc_ir_set_addrtaken(tcc_state->ir, tcc_state->deferred_addrtaken[k]->vreg);
        }
    }
    tcc_state->defer_addrtaken = outer_defer;
    tcc_state->nb_deferred_addrtaken = outer_nb;
    memcpy(tcc_state->deferred_addrtaken, outer_syms, sizeof(outer_syms));
    switch (template[arg])
    {
    case 'a':
    case 'A':
      atom_ptr = &vtop->type;
      if ((atom_ptr->t & VT_BTYPE) != VT_PTR)
        expect("pointer");
      atom = pointed_type(atom_ptr);
      size = type_size(atom, &align);
      if (size > 8 || (size & (size - 1)) ||
          (atok >= TOK___atomic_fetch_add && atok <= TOK___atomic_nand_fetch &&
           (0 == btype_size(atom->t & VT_BTYPE) || (atom->t & VT_BTYPE) == VT_PTR)))
        expect("integral or integer-sized pointer target type");
      /* GCC does not care either: */
      /* if (!(atom->t & VT_ATOMIC))
          tcc_warning("pointer target declaration is missing '_Atomic'"); */
      break;

    case 'p':
      if ((vtop->type.t & VT_BTYPE) != VT_PTR || type_size(pointed_type(&vtop->type), &align) != size)
        tcc_error("pointer target type mismatch in argument %d", arg + 1);
      gen_assign_cast(atom_ptr);
      break;
    case 'v':
      gen_assign_cast(atom);
      break;
    case 'l':
      indir();
      gen_assign_cast(atom);
      break;
    case 's':
      save = 1;
      indir();
      store = *vtop;
      vpop();
      break;
    case 'm':
      gen_assign_cast(&int_type);
      break;
    case 'b':
      ct.t = VT_BOOL;
      gen_assign_cast(&ct);
      break;
    }
    if ('.' == template[++arg])
      break;
    skip(',');
  }
  skip(')');

  ct.t = VT_VOID;
  switch (template[arg + 1])
  {
  case 'b':
    ct.t = VT_BOOL;
    break;
  case 'v':
    ct = *atom;
    break;
  }

  /* A value form leaves the stack as its generic form does once the operand
   * is loaded: the same operation from here on, and the same helper. */
  if (atok == TOK___atomic_store_n)
    atok = TOK___atomic_store;
  else if (atok == TOK___atomic_exchange_n)
    atok = TOK___atomic_exchange;
  else if (atok == TOK___atomic_compare_exchange_n)
    atok = TOK___atomic_compare_exchange;

  if (inline_atomic_access(atok, size, atom))
  {
    if ((ct.t & VT_BTYPE) == VT_VOID)
    {
      vpushi(0);
      vtop->type = ct;
      return;
    }
    goto result;
  }
  if (inline_atomic_rmw(atok, size, atom, &ct))
    goto result;

  /* __atomic_load_n is __atomic_load returning the value: same helper. */
  snprintf(buf, sizeof(buf), "%s_%d", get_tok_str(atok == TOK___atomic_load_n ? TOK___atomic_load : atok, 0), size);
  gen_internal_call(buf, arg - save, &ct);
  if ((ct.t & VT_BTYPE) == VT_VOID)
    return;
result:
  gen_cast(&ct);
  if (save)
  {
    vpush(&ct);
    *vtop = store;
    vswap();
    vstore();
  }
}
