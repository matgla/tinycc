/*
 *  test_tccasm.c - suite for tccasm.c
 *
 *  Covers parser-independent assembler helpers without invoking production
 *  lexer, section, or code-emission machinery.
 */

#define USING_GLOBALS
#define asm_expr tccasm_ut_asm_expr
#define asm_global_instr tccasm_ut_asm_global_instr
#define asm_instr tccasm_ut_asm_instr
#define asm_int_expr tccasm_ut_asm_int_expr
#define find_constraint tccasm_ut_find_constraint
#define get_asm_sym tccasm_ut_get_asm_sym
#define tcc_asm_emit_inline tccasm_ut_tcc_asm_emit_inline
#define tcc_assemble tccasm_ut_tcc_assemble
#include "tccasm.c"
#undef asm_expr
#undef asm_global_instr
#undef asm_instr
#undef asm_int_expr
#undef find_constraint
#undef get_asm_sym
#undef tcc_asm_emit_inline
#undef tcc_assemble

#include <string.h>

#include "ut.h"

/* Test-harness hooks implemented in stubs.c. */
void utb_set_tok_str(int tok, const char *name);

static int token_for_name(const char *name)
{
  return tok_alloc_const(name);
}

/* ------------------------------------------------------------------ tests */

UT_TEST(test_local_label_name_uses_gas_local_prefix)
{
  int tok1 = asm_get_local_label_name(tcc_state, 7);
  int tok2 = asm_get_local_label_name(tcc_state, 7);

  UT_ASSERT_EQ(tok1, tok2);
  UT_ASSERT(strcmp(get_tok_str(tok1, NULL), "L..7") == 0);
  return 0;
}

UT_TEST(test_asm2cname_is_identity_without_leading_underscore)
{
  int addeddot = -1;
  int name = token_for_name("plain");
  tcc_state->leading_underscore = 0;

  UT_ASSERT_EQ(asm2cname(name, &addeddot), name);
  UT_ASSERT_EQ(addeddot, 0);
  return 0;
}

UT_TEST(test_asm2cname_strips_leading_underscore_for_c_symbol)
{
  int addeddot = -1;
  int name = token_for_name("_entry");
  tcc_state->leading_underscore = 1;

  int cname = asm2cname(name, &addeddot);

  UT_ASSERT_EQ(addeddot, 0);
  UT_ASSERT(strcmp(get_tok_str(cname, NULL), "entry") == 0);
  return 0;
}

UT_TEST(test_asm2cname_prefixes_dot_for_plain_asm_symbol)
{
  int addeddot = -1;
  int name = token_for_name("entry");
  tcc_state->leading_underscore = 1;

  int cname = asm2cname(name, &addeddot);

  UT_ASSERT_EQ(addeddot, 1);
  UT_ASSERT(strcmp(get_tok_str(cname, NULL), ".entry") == 0);
  return 0;
}

UT_TEST(test_asm2cname_preserves_existing_dotted_asm_name)
{
  int addeddot = -1;
  int name = token_for_name("L.local");
  tcc_state->leading_underscore = 1;

  UT_ASSERT_EQ(asm2cname(name, &addeddot), name);
  UT_ASSERT_EQ(addeddot, 0);
  return 0;
}

UT_TEST(test_find_constraint_numeric_reference_updates_tail)
{
  ASMOperand operands[3] = {0};
  const char *tail = NULL;

  UT_ASSERT_EQ(tccasm_ut_find_constraint(operands, 3, "2:q", &tail), 2);
  UT_ASSERT(tail != NULL);
  UT_ASSERT(strcmp(tail, ":q") == 0);
  return 0;
}

UT_TEST(test_find_constraint_rejects_out_of_range_numeric_reference)
{
  ASMOperand operands[2] = {0};
  const char *tail = NULL;

  UT_ASSERT_EQ(tccasm_ut_find_constraint(operands, 2, "2", &tail), -1);
  UT_ASSERT(tail != NULL);
  UT_ASSERT(strcmp(tail, "") == 0);
  return 0;
}

UT_TEST(test_find_constraint_named_reference_matches_operand_id)
{
  ASMOperand operands[3] = {0};
  const char *tail = NULL;
  int named = token_for_name("dst");
  operands[1].id = named;

  UT_ASSERT_EQ(tccasm_ut_find_constraint(operands, 3, "[dst]suffix", &tail), 1);
  UT_ASSERT(tail != NULL);
  UT_ASSERT(strcmp(tail, "suffix") == 0);
  return 0;
}

UT_TEST(test_asm_macro_find_returns_matching_macro)
{
  AsmMacro first = {0};
  AsmMacro second = {0};
  int first_name = token_for_name("first_macro");
  int second_name = token_for_name("second_macro");

  first.name = first_name;
  first.next = &second;
  second.name = second_name;
  asm_macros = &first;

  UT_ASSERT(asm_macro_find(first_name) == &first);
  UT_ASSERT(asm_macro_find(second_name) == &second);
  UT_ASSERT(asm_macro_find(token_for_name("missing_macro")) == NULL);

  asm_macros = NULL;
  return 0;
}

UT_TEST(test_asm_get_prefix_name_formats_token)
{
  int tok;

  tok = asm_get_prefix_name(tcc_state, "PRE", 42);
  UT_ASSERT(strcmp(get_tok_str(tok, NULL), "PRE42") == 0);

  tok = asm_get_prefix_name(tcc_state, "L.", 0);
  UT_ASSERT(strcmp(get_tok_str(tok, NULL), "L.0") == 0);

  tok = asm_get_local_label_name(tcc_state, 123);
  UT_ASSERT(strcmp(get_tok_str(tok, NULL), "L..123") == 0);
  return 0;
}

UT_TEST(test_asm_macros_free_clears_list)
{
  AsmMacro *first = tcc_mallocz(sizeof(AsmMacro));
  AsmMacro *second = tcc_mallocz(sizeof(AsmMacro));

  first->name = token_for_name("m1");
  first->next = second;
  second->name = token_for_name("m2");
  asm_macros = first;

  asm_macros_free();

  UT_ASSERT(asm_macros == NULL);
  return 0;
}

UT_TEST(test_use_section1_saves_and_restores_data_offset)
{
  Section sec_a = {0};
  Section sec_b = {0};

  cur_text_section = &sec_a;
  ind = 10;
  sec_a.data_offset = 0;
  sec_b.data_offset = 20;

  use_section1(tcc_state, &sec_b);
  UT_ASSERT_EQ(sec_a.data_offset, 10);
  UT_ASSERT(cur_text_section == &sec_b);
  UT_ASSERT_EQ(ind, 20);

  use_section1(tcc_state, &sec_a);
  UT_ASSERT_EQ(sec_b.data_offset, 20);
  UT_ASSERT(cur_text_section == &sec_a);
  UT_ASSERT_EQ(ind, 10);
  return 0;
}

UT_TEST(test_use_section_switches_to_find_section_result)
{
  Section sec_old = {0};

  cur_text_section = &sec_old;
  ind = 5;
  sec_old.data_offset = 100;

  use_section(tcc_state, ".data");

  UT_ASSERT_EQ(sec_old.data_offset, 5);
  UT_ASSERT(cur_text_section != &sec_old);
  UT_ASSERT_EQ(cur_text_section->data_offset, 0);
  UT_ASSERT_EQ(ind, 0);
  return 0;
}

UT_TEST(test_push_section_and_pop_section_roundtrip)
{
  Section sec_old = {0};
  Section *pushed;

  cur_text_section = &sec_old;
  ind = 7;
  sec_old.data_offset = 50;

  push_section(tcc_state, ".data");
  pushed = cur_text_section;

  UT_ASSERT_EQ(sec_old.data_offset, 7);
  UT_ASSERT(pushed->prev == &sec_old);
  UT_ASSERT(pushed != &sec_old);
  UT_ASSERT_EQ(ind, 0);

  pop_section(tcc_state);

  UT_ASSERT(cur_text_section == &sec_old);
  UT_ASSERT_EQ(ind, 7);
  UT_ASSERT(pushed->prev == NULL);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(tccasm)
{
  UT_RUN(test_local_label_name_uses_gas_local_prefix);
  UT_RUN(test_asm2cname_is_identity_without_leading_underscore);
  UT_RUN(test_asm2cname_strips_leading_underscore_for_c_symbol);
  UT_RUN(test_asm2cname_prefixes_dot_for_plain_asm_symbol);
  UT_RUN(test_asm2cname_preserves_existing_dotted_asm_name);
  UT_RUN(test_find_constraint_numeric_reference_updates_tail);
  UT_RUN(test_find_constraint_rejects_out_of_range_numeric_reference);
  UT_RUN(test_find_constraint_named_reference_matches_operand_id);
  UT_RUN(test_asm_macro_find_returns_matching_macro);
  UT_RUN(test_asm_get_prefix_name_formats_token);
  UT_RUN(test_asm_macros_free_clears_list);
  UT_RUN(test_use_section1_saves_and_restores_data_offset);
  UT_RUN(test_use_section_switches_to_find_section_result);
  UT_RUN(test_push_section_and_pop_section_roundtrip);
}
