/*
 *  test_ir_type.c - suite for ir/type.c type classification helpers
 *
 *  All tested functions are pure bitfield predicates on `int t` (the
 *  VT_* type encoding from tcc.h) or on TccIrOp enum values. No
 *  TCCIRState or compiler state needed.
 */

#define USING_GLOBALS
#include "ir.h"

#include "ut.h"

/* ------------------------------------------------------------------ tests */

UT_TEST(test_type_is_float)
{
  UT_ASSERT(tcc_ir_type_is_float(VT_FLOAT));
  UT_ASSERT(tcc_ir_type_is_float(VT_DOUBLE));
  UT_ASSERT(tcc_ir_type_is_float(VT_LDOUBLE));
  UT_ASSERT(!tcc_ir_type_is_float(VT_INT));
  UT_ASSERT(!tcc_ir_type_is_float(VT_LLONG));
  UT_ASSERT(!tcc_ir_type_is_float(VT_PTR));
  return 0;
}

UT_TEST(test_type_is_double)
{
  UT_ASSERT(tcc_ir_type_is_double(VT_DOUBLE));
  UT_ASSERT(tcc_ir_type_is_double(VT_LDOUBLE));
  UT_ASSERT(!tcc_ir_type_is_double(VT_FLOAT));
  UT_ASSERT(!tcc_ir_type_is_double(VT_INT));
  return 0;
}

UT_TEST(test_type_is_64bit)
{
  UT_ASSERT(tcc_ir_type_is_64bit(VT_LLONG));
  UT_ASSERT(tcc_ir_type_is_64bit(VT_DOUBLE));
  UT_ASSERT(tcc_ir_type_is_64bit(VT_LDOUBLE));
  UT_ASSERT(!tcc_ir_type_is_64bit(VT_INT));
  UT_ASSERT(!tcc_ir_type_is_64bit(VT_FLOAT));
  UT_ASSERT(!tcc_ir_type_is_64bit(VT_PTR));

  /* float _Complex = 8 bytes => 64-bit */
  UT_ASSERT(tcc_ir_type_is_64bit(VT_FLOAT | VT_COMPLEX));
  /* double _Complex = 16 bytes => 64-bit */
  UT_ASSERT(tcc_ir_type_is_64bit(VT_DOUBLE | VT_COMPLEX));
  /* int _Complex is not 64-bit via this predicate */
  UT_ASSERT(!tcc_ir_type_is_64bit(VT_INT | VT_COMPLEX));
  return 0;
}

UT_TEST(test_type_is_ptr)
{
  UT_ASSERT(tcc_ir_type_is_ptr(VT_PTR));
  UT_ASSERT(!tcc_ir_type_is_ptr(VT_INT));
  UT_ASSERT(!tcc_ir_type_is_ptr(VT_FLOAT));
  return 0;
}

UT_TEST(test_type_is_struct)
{
  UT_ASSERT(tcc_ir_type_is_struct(VT_STRUCT));
  UT_ASSERT(!tcc_ir_type_is_struct(VT_INT));
  UT_ASSERT(!tcc_ir_type_is_struct(VT_PTR));
  return 0;
}

UT_TEST(test_type_is_void)
{
  UT_ASSERT(tcc_ir_type_is_void(VT_VOID));
  UT_ASSERT(!tcc_ir_type_is_void(VT_INT));
  return 0;
}

UT_TEST(test_type_unsigned_signed)
{
  UT_ASSERT(tcc_ir_type_is_unsigned(VT_INT | VT_UNSIGNED));
  UT_ASSERT(!tcc_ir_type_is_unsigned(VT_INT));

  /* Signed: not unsigned, not float */
  UT_ASSERT(tcc_ir_type_is_signed(VT_INT));
  UT_ASSERT(!tcc_ir_type_is_signed(VT_INT | VT_UNSIGNED));
  UT_ASSERT(!tcc_ir_type_is_signed(VT_FLOAT));
  return 0;
}

UT_TEST(test_type_is_bool)
{
  UT_ASSERT(tcc_ir_type_is_bool(VT_CMP));
  UT_ASSERT(tcc_ir_type_is_bool(VT_INT | VT_CMP));
  /* VT_VOID = 0 has no bits in common with VT_CMP (0x13), so is_bool → false */
  UT_ASSERT(!tcc_ir_type_is_bool(VT_VOID));
  return 0;
}

UT_TEST(test_type_is_int)
{
  UT_ASSERT(tcc_ir_type_is_int(VT_INT));
  UT_ASSERT(tcc_ir_type_is_int(VT_LLONG));
  UT_ASSERT(tcc_ir_type_is_int(VT_PTR));
  UT_ASSERT(!tcc_ir_type_is_int(VT_FLOAT));
  UT_ASSERT(!tcc_ir_type_is_int(VT_DOUBLE));
  return 0;
}

UT_TEST(test_type_op_needs_fpu)
{
  UT_ASSERT(tcc_ir_type_op_needs_fpu(TCCIR_OP_FADD));
  UT_ASSERT(tcc_ir_type_op_needs_fpu(TCCIR_OP_FSUB));
  UT_ASSERT(tcc_ir_type_op_needs_fpu(TCCIR_OP_FMUL));
  UT_ASSERT(tcc_ir_type_op_needs_fpu(TCCIR_OP_FDIV));
  UT_ASSERT(tcc_ir_type_op_needs_fpu(TCCIR_OP_FNEG));
  UT_ASSERT(tcc_ir_type_op_needs_fpu(TCCIR_OP_FCMP));
  UT_ASSERT(tcc_ir_type_op_needs_fpu(TCCIR_OP_CVT_ITOF));
  UT_ASSERT(tcc_ir_type_op_needs_fpu(TCCIR_OP_CVT_FTOI));
  UT_ASSERT(tcc_ir_type_op_needs_fpu(TCCIR_OP_CVT_FTOF));

  UT_ASSERT(!tcc_ir_type_op_needs_fpu(TCCIR_OP_ADD));
  UT_ASSERT(!tcc_ir_type_op_needs_fpu(TCCIR_OP_MUL));
  UT_ASSERT(!tcc_ir_type_op_needs_fpu(TCCIR_OP_LOAD));
  UT_ASSERT(!tcc_ir_type_op_needs_fpu(TCCIR_OP_JUMP));
  return 0;
}

UT_TEST(test_type_spilled_sv)
{
  SValue sv_spilled = {0};
  sv_spilled.pr0_reg = PREG_REG_NONE;
  sv_spilled.pr0_spilled = 0;
  UT_ASSERT(tcc_ir_type_spilled(&sv_spilled));

  SValue sv_flagspilled = {0};
  sv_flagspilled.pr0_reg = 0; /* valid reg */
  sv_flagspilled.pr0_spilled = 1;
  UT_ASSERT(tcc_ir_type_spilled(&sv_flagspilled));

  SValue sv_live = {0};
  sv_live.pr0_reg = 0;
  sv_live.pr0_spilled = 0;
  UT_ASSERT(!tcc_ir_type_spilled(&sv_live));
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(ir_type)
{
  UT_RUN(test_type_is_float);
  UT_RUN(test_type_is_double);
  UT_RUN(test_type_is_64bit);
  UT_RUN(test_type_is_ptr);
  UT_RUN(test_type_is_struct);
  UT_RUN(test_type_is_void);
  UT_RUN(test_type_unsigned_signed);
  UT_RUN(test_type_is_bool);
  UT_RUN(test_type_is_int);
  UT_RUN(test_type_op_needs_fpu);
  UT_RUN(test_type_spilled_sv);
}
