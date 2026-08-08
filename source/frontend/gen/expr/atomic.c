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

  sprintf(buf, "%s_%d", get_tok_str(atok, 0), size);
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
  gen_cast(&ct);
  if (save)
  {
    vpush(&ct);
    *vtop = store;
    vswap();
    vstore();
  }
}
