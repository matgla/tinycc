/*
 *  test_tccgen.c - suite for tccgen.c
 *
 *  Covers exported frontend type helpers against the real tccgen.c object.
 */

#include "tcc.h"
#include "ut.h"

/* Not declared in tcc.h, but exported from tccgen.c. */
const char *get_value_type(int r);

static CType simple_type(int t)
{
  CType type;
  type.t = t;
  type.ref = NULL;
  return type;
}

static Sym sym_for_type(CType type, int c, int r)
{
  Sym sym;
  memset(&sym, 0, sizeof(sym));
  sym.type = type;
  sym.c = c;
  sym.r = (unsigned short)r;
  return sym;
}

static int assert_type_size(CType type, int expected_size,
                            int expected_align)
{
  int align = -1;
  int size = type_size(&type, &align);
  UT_ASSERT_EQ(size, expected_size);
  UT_ASSERT_EQ(align, expected_align);
  return 0;
}

UT_TEST(test_type_size_scalar_armv8m_abi)
{
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_VOID), 1, 1), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_BOOL), 1, 1), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_BYTE), 1, 1), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_SHORT), 2, 2), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_INT), 4, 4), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_LONG | VT_INT), 4, 4), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_LLONG), 8, 8), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_FLOAT), 4, 4), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_DOUBLE), 8, 8), 0);
  return 0;
}

UT_TEST(test_type_size_complex_types)
{
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_FLOAT | VT_COMPLEX), 8, 4), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_DOUBLE | VT_COMPLEX), 16, 8), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_INT | VT_COMPLEX), 8, 4), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_SHORT | VT_COMPLEX), 4, 2), 0);
  return 0;
}

UT_TEST(test_type_size_pointer_and_array)
{
  CType elem = simple_type(VT_SHORT);
  Sym elem_ref = sym_for_type(elem, 7, 0);
  CType array_type = simple_type(VT_PTR | VT_ARRAY);
  CType ptr_type = simple_type(VT_PTR);

  array_type.ref = &elem_ref;
  UT_ASSERT_EQ(assert_type_size(array_type, 14, 2), 0);

  ptr_type.ref = &elem_ref;
  UT_ASSERT_EQ(assert_type_size(ptr_type, PTR_SIZE, PTR_SIZE), 0);
  return 0;
}

UT_TEST(test_type_size_struct_and_incomplete_enum)
{
  CType struct_type = simple_type(VT_STRUCT);
  Sym struct_ref = sym_for_type(simple_type(VT_INT), 12, 4);
  CType enum_type = simple_type(VT_ENUM | VT_INT);
  Sym enum_ref = sym_for_type(simple_type(VT_INT), -1, 0);

  struct_type.ref = &struct_ref;
  UT_ASSERT_EQ(assert_type_size(struct_type, 12, 4), 0);

  enum_type.ref = &enum_ref;
  UT_ASSERT_EQ(assert_type_size(enum_type, -1, 0), 0);
  return 0;
}

UT_TEST(test_exact_log2p1_alignment_encoding)
{
  UT_ASSERT_EQ(exact_log2p1(0), 0);
  UT_ASSERT_EQ(exact_log2p1(1), 1);
  UT_ASSERT_EQ(exact_log2p1(2), 2);
  UT_ASSERT_EQ(exact_log2p1(4), 3);
  UT_ASSERT_EQ(exact_log2p1(8), 4);
  UT_ASSERT_EQ(exact_log2p1(16), 5);
  UT_ASSERT_EQ(exact_log2p1(256), 9);
  return 0;
}

UT_TEST(test_exact_log2p1_non_powers_and_large)
{
  UT_ASSERT_EQ(exact_log2p1(3), 2);
  UT_ASSERT_EQ(exact_log2p1(5), 3);
  UT_ASSERT_EQ(exact_log2p1(6), 3);
  UT_ASSERT_EQ(exact_log2p1(7), 3);
  UT_ASSERT_EQ(exact_log2p1(0x100), 9);
  UT_ASSERT_EQ(exact_log2p1(0x10000), 17);
  UT_ASSERT_EQ(exact_log2p1(0x40000000), 31);
  return 0;
}

UT_TEST(test_type_size_ldouble_qlong_qfloat)
{
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_LDOUBLE), 8, 8), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_QLONG), 16, 8), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_QFLOAT), 16, 8), 0);
  return 0;
}

UT_TEST(test_type_size_complete_enum_and_func)
{
  CType enum_type = simple_type(VT_ENUM | VT_INT);
  Sym enum_ref = sym_for_type(simple_type(VT_INT), 0, 0);

  enum_type.ref = &enum_ref;
  UT_ASSERT_EQ(assert_type_size(enum_type, 4, 4), 0);

  UT_ASSERT_EQ(assert_type_size(simple_type(VT_FUNC), 1, 1), 0);
  return 0;
}

UT_TEST(test_is_float_recognizes_fp_btypes)
{
  UT_ASSERT(is_float(VT_FLOAT));
  UT_ASSERT(is_float(VT_DOUBLE));
  UT_ASSERT(is_float(VT_LDOUBLE));
  UT_ASSERT(is_float(VT_QFLOAT));
  UT_ASSERT(!is_float(VT_INT));
  UT_ASSERT(!is_float(VT_BYTE));
  UT_ASSERT(!is_float(VT_SHORT));
  UT_ASSERT(!is_float(VT_LLONG));
  UT_ASSERT(!is_float(VT_PTR));
  UT_ASSERT(!is_float(VT_STRUCT));
  UT_ASSERT(!is_float(VT_BOOL));
  return 0;
}

UT_TEST(test_get_value_type_returns_null)
{
  UT_ASSERT(get_value_type(0) == NULL);
  UT_ASSERT(get_value_type(VT_LOCAL) == NULL);
  UT_ASSERT(get_value_type(VT_CONST) == NULL);
  return 0;
}

UT_TEST(test_ieee_finite)
{
  UT_ASSERT(ieee_finite(0.0));
  UT_ASSERT(ieee_finite(-0.0));
  UT_ASSERT(ieee_finite(1.0));
  UT_ASSERT(ieee_finite(-1.0));
  UT_ASSERT(ieee_finite(1.5));
  UT_ASSERT(!ieee_finite(1.0 / 0.0));
  UT_ASSERT(!ieee_finite(-1.0 / 0.0));
  UT_ASSERT(!ieee_finite(0.0 / 0.0));
  return 0;
}

UT_SUITE(tccgen)
{
  UT_RUN(test_type_size_scalar_armv8m_abi);
  UT_RUN(test_type_size_complex_types);
  UT_RUN(test_type_size_pointer_and_array);
  UT_RUN(test_type_size_struct_and_incomplete_enum);
  UT_RUN(test_type_size_ldouble_qlong_qfloat);
  UT_RUN(test_type_size_complete_enum_and_func);
  UT_RUN(test_exact_log2p1_alignment_encoding);
  UT_RUN(test_exact_log2p1_non_powers_and_large);
  UT_RUN(test_is_float_recognizes_fp_btypes);
  UT_RUN(test_get_value_type_returns_null);
  UT_RUN(test_ieee_finite);
}
