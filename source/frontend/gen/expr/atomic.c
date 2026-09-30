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
 * Read-modify-write stays a call: an LDREX/STREX loop never completes on
 * the RP2350's PSRAM, so how to make it atomic is the runtime's to decide.
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

void parse_atomic(int atok)
{
  int size, align, arg, t, save = 0;
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
                                          /* __atomic_nand_fetch */ "avm.v"};
  const char *template = templates[(atok - TOK___atomic_store)];

  atom = atom_ptr = NULL;
  size = 0; /* pacify compiler */
  next();
  skip('(');
  for (arg = 0;;)
  {
    expr_eq();
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
          (atok > TOK___atomic_compare_exchange &&
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

  /* __atomic_load_n is __atomic_load returning the value: same helper. */
  snprintf(buf, sizeof(buf), "%s_%d", get_tok_str(atok == TOK___atomic_load_n ? TOK___atomic_load : atok, 0), size);
  vpush_helper_func(tok_alloc_const(buf));
  {
    int call_argc = arg - save;
    int stack_count = call_argc + 1;
    const int call_id = tcc_state->ir ? tcc_state->ir->next_call_id++ : 0;
    SValue param_num;
    SValue call_id_sv;
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
    if ((ct.t & VT_BTYPE) == VT_VOID)
    {
      tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVOID, &vtop[-call_argc], &call_id_sv, NULL);
      vtop -= stack_count;
      vpushi(0);
      vtop->type = ct;
      vtop->r = VT_CONST;
      return;
    }
    else
    {
      SValue dest;
      svalue_init(&dest);
      dest.type = ct;
      dest.r = 0;
      dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVAL, &vtop[-call_argc], &call_id_sv, &dest);

      vtop -= stack_count;
      vpushi(0);
      vtop->type = ct;
      vtop->vr = dest.vr;
      PUT_R_RET(vtop, ct.t);
    }
  }
  t = ct.t & VT_BTYPE;
  if (t == VT_BYTE || t == VT_SHORT || t == VT_BOOL)
  {
#ifdef PROMOTE_RET
    vtop->r |= BFVAL(VT_MUSTCAST, 1);
#else
    vtop->type.t = VT_INT;
#endif
  }
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
