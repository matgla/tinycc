/*
 *  TCC IR - Floating Point Generation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"

/* FP operations */
void tcc_ir_gen_fadd(TCCIRState *ir)
{
  tcc_ir_gen_f(ir, '+');
}
void tcc_ir_gen_fsub(TCCIRState *ir)
{
  tcc_ir_gen_f(ir, '-');
}
void tcc_ir_gen_fmul(TCCIRState *ir)
{
  tcc_ir_gen_f(ir, '*');
}

void tcc_ir_gen_f(TCCIRState *ir, int op)
{
  TccIrOp ir_op;
  SValue dest;
  int is_double;

  /* Determine the IR operation based on token */
  switch (op)
  {
  case '+':
    ir_op = TCCIR_OP_FADD;
    break;
  case '-':
    ir_op = TCCIR_OP_FSUB;
    break;
  case '*':
    ir_op = TCCIR_OP_FMUL;
    break;
  case '/':
    ir_op = TCCIR_OP_FDIV;
    break;
  case 'n': /* negation */
    ir_op = TCCIR_OP_FNEG;
    break;
  case 'c': /* compare */
    ir_op = TCCIR_OP_FCMP;
    tcc_ir_put(ir, ir_op, &vtop[-1], &vtop[0], NULL);
    --vtop;
    vtop->r = VT_CMP;
    vtop->cmp_op = TOK_LT; /* default, will be fixed up later */
    vtop->jfalse = -1;     /* -1 = no chain */
    vtop->jtrue = -1;      /* -1 = no chain */
    vtop->vr = -1;         /* clear stale vreg so gv() materializes the CMP result */
    return;
  case 't': /* float-to-float conversion */
    ir_op = TCCIR_OP_CVT_FTOF;
    break;
  case 'i': /* int-to-float conversion */
    ir_op = TCCIR_OP_CVT_ITOF;
    break;
  case 'f': /* float-to-int conversion */
    ir_op = TCCIR_OP_CVT_FTOI;
    break;
  default:
    /* Comparison operations */
    if (op >= TOK_ULT && op <= TOK_GT)
    {
      ir_op = TCCIR_OP_FCMP;

      int cmp_op = op;

      /* An inline DCP compare uses a different flag convention entirely.
       * RCMP writes the relation straight into NZCV in the AEABI's own
       * encoding -- C set means "ordered and >=", Z set means equal, V set
       * means unordered -- which is read with *unsigned* conditions.  That is
       * not the signed three-way encoding __aeabi_cdcmple produces (flags as
       * if `cmp r,#0` on -1/0/1/2; see lib/fp/arm/rp2350/dcp_aeabi.S), so the
       * fix-up below has to be a different one:
       *
       *   a >  b  ->  RCMP(a, b), HI      (no swap: HI *is* ">")
       *   a >= b  ->  RCMP(a, b), HS
       *   a <  b  ->  RCMP(b, a), HI      (RCMP has no "<"; mirror instead)
       *   a <= b  ->  RCMP(b, a), HS
       *   a == b  ->  RCMP(a, b), EQ      (Z alone)
       *   a != b  ->  RCMP(a, b), NE
       *
       * Unordered falls out correctly everywhere: RCMP leaves C clear on
       * unordered, so all four relational tests are false and only NE is true.
       */
      const FloatingPointConfig *dcp_fpu = architecture_config.fpu;
      const int dcp_cmp = dcp_fpu && dcp_fpu->has_dcmp && dcp_fpu->double_impl == FP_DOUBLE_IMPL_DCP &&
                          (vtop[0].type.t & VT_BTYPE) == VT_DOUBLE && (vtop[-1].type.t & VT_BTYPE) == VT_DOUBLE;
      if (dcp_cmp)
      {
        switch (op)
        {
        case TOK_LT:
        case TOK_ULT:
          vswap();
          cmp_op = TOK_UGT;
          break;
        case TOK_LE:
        case TOK_ULE:
          vswap();
          cmp_op = TOK_UGE;
          break;
        case TOK_GT:
        case TOK_UGT:
          cmp_op = TOK_UGT;
          break;
        case TOK_GE:
        case TOK_UGE:
          cmp_op = TOK_UGE;
          break;
        default: /* EQ / NE read Z directly and need no fix-up */
          break;
        }
      }
      /* Soft-float path: __aeabi_cdcmple(a,b) / __aeabi_cfcmple(a,b) only set
       * correct CPSR flags for LE/LT/EQ/NE.  For GT/GE the NaN "unordered"
       * mapping makes the condition evaluate TRUE instead of FALSE, so swap
       * the operands and test the mirrored condition:
       *   a >  b  ->  cdcmple(b, a), test LT
       *   a >= b  ->  cdcmple(b, a), test LE
       */
      else if (op == TOK_GT || op == TOK_UGT)
      {
        vswap();
        cmp_op = (op == TOK_GT) ? TOK_LT : TOK_ULT;
      }
      else if (op == TOK_GE || op == TOK_UGE)
      {
        vswap();
        cmp_op = (op == TOK_GE) ? TOK_LE : TOK_ULE;
      }

      tcc_ir_put(ir, ir_op, &vtop[-1], &vtop[0], NULL);
      --vtop;
      vtop->r = VT_CMP;
      vtop->cmp_op = cmp_op;
      vtop->jfalse = -1; /* -1 = no chain */
      vtop->jtrue = -1;  /* -1 = no chain */
      vtop->vr = -1;     /* clear stale vreg so gv() materializes the CMP result */
      return;
    }
    tcc_error("tcc_ir_gen_f: unknown floating point operation: 0x%x", op);
    return;
  }

  /* Handle negation (unary) */
  if (ir_op == TCCIR_OP_FNEG)
  {
    svalue_init(&dest);
    dest.vr = tcc_ir_get_vreg_temp(ir);
    dest.r = 0;
    dest.type = vtop->type;
    /* Mark temp as float/double */
    is_double = (vtop->type.t & VT_BTYPE) == VT_DOUBLE || (vtop->type.t & VT_BTYPE) == VT_LDOUBLE;
    tcc_ir_set_float_type(ir, dest.vr, 1, is_double);
    tcc_ir_put(ir, ir_op, &vtop[0], NULL, &dest);
    vtop->vr = dest.vr;
    vtop->r = 0;
    return;
  }

  /* Check if this is a complex addition/subtraction operation */
  int is_complex_op = ((vtop[-1].type.t & VT_COMPLEX) || (vtop[0].type.t & VT_COMPLEX));

  if (is_complex_op &&
      (ir_op == TCCIR_OP_FADD || ir_op == TCCIR_OP_FSUB || ir_op == TCCIR_OP_FMUL || ir_op == TCCIR_OP_FDIV))
  {
    /* Phase 3: Complex addition/subtraction
     * For complex: (a+bi) + (c+di) = (a+c) + (b+d)i
     * We generate two FP operations and use a single vr to track the result.
     * The code generator (arm-thumb-gen.c) will recognize complex operands
     * and emit two soft-float library calls.
     */
    int base_type = vtop[-1].type.t & VT_BTYPE;

    /* Create destination SValue with complex type */
    svalue_init(&dest);
    dest.vr = tcc_ir_get_vreg_temp(ir);
    dest.r = 0;
    dest.type.t = (base_type | VT_COMPLEX);

    /* Mark as float type (not double) for register allocation */
    is_double = (base_type == VT_DOUBLE || base_type == VT_LDOUBLE);
    tcc_ir_set_float_type(ir, dest.vr, 1, is_double);
    /* Phase 3: Mark as complex type so register allocator allocates pairs */
    tcc_ir_vreg_type_set_complex(ir, dest.vr);

    /* Generate a single complex operation - the code generator will
     * recognize the complex type and emit two soft-float calls */
    tcc_ir_put(ir, ir_op, &vtop[-1], &vtop[0], &dest);

    vtop[-1].vr = dest.vr;
    vtop[-1].r = 0;
    vtop[-1].type.t = dest.type.t;
    --vtop;
    return;
  }

  /* Binary FP operations and conversions */
  svalue_init(&dest);
  dest.vr = tcc_ir_get_vreg_temp(ir);
  dest.r = 0;
  if (ir_op == TCCIR_OP_CVT_ITOF || ir_op == TCCIR_OP_CVT_FTOI || ir_op == TCCIR_OP_CVT_FTOF)
  {
    /* For conversions, dest type depends on the operation */
    if (ir_op == TCCIR_OP_CVT_ITOF)
    {
      /* int to float: result is float type of destination */
      dest.type = vtop->type;
      is_double = (vtop->type.t & VT_BTYPE) == VT_DOUBLE || (vtop->type.t & VT_BTYPE) == VT_LDOUBLE;
      tcc_ir_set_float_type(ir, dest.vr, 1, is_double);
    }
    else if (ir_op == TCCIR_OP_CVT_FTOI)
    {
      /* float to int: result is int type */
      dest.type.t = VT_INT;
    }
    else /* TCCIR_OP_CVT_FTOF */
    {
      /* float-to-float: result is destination type */
      dest.type = vtop->type;
      is_double = (vtop->type.t & VT_BTYPE) == VT_DOUBLE || (vtop->type.t & VT_BTYPE) == VT_LDOUBLE;
      tcc_ir_set_float_type(ir, dest.vr, 1, is_double);
    }
  }
  else
  {
    dest.type = vtop[-1].type;
    /* Mark temp as float/double */
    is_double = (vtop[-1].type.t & VT_BTYPE) == VT_DOUBLE || (vtop[-1].type.t & VT_BTYPE) == VT_LDOUBLE;
    tcc_ir_set_float_type(ir, dest.vr, 1, is_double);
  }
  tcc_ir_put(ir, ir_op, &vtop[-1], &vtop[0], &dest);
  if (ir_op == TCCIR_OP_CVT_ITOF || ir_op == TCCIR_OP_CVT_FTOI || ir_op == TCCIR_OP_CVT_FTOF)
  {
    vtop->vr = dest.vr;
    vtop->r = 0;
    vtop->type = dest.type;
  }
  else
  {
    vtop[-1].vr = dest.vr;
    vtop[-1].r = 0;
    --vtop;
  }
}

void tcc_ir_gen_fdiv(TCCIRState *ir)
{
  tcc_ir_gen_f(ir, '/');
}
void tcc_ir_gen_fneg(TCCIRState *ir)
{
  tcc_ir_gen_f(ir, 'n');
}
void tcc_ir_gen_fcmp(TCCIRState *ir)
{
  tcc_ir_gen_f(ir, 'c');
}

/* Conversions */
void tcc_ir_gen_cvt_ftof(TCCIRState *ir)
{
  tcc_ir_gen_f(ir, 't');
}
void tcc_ir_gen_cvt_itof(TCCIRState *ir)
{
  tcc_ir_gen_f(ir, 'i');
}
void tcc_ir_gen_cvt_ftoi(TCCIRState *ir)
{
  tcc_ir_gen_f(ir, 'f');
}
