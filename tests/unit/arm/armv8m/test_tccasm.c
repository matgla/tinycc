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

/* ------------------------------------------------------------------ stubs */
/* The main UT binary does not link tccgen.c/tccpp.c, so define the few
 * production helpers the assembler expression / symbol helpers need. */

/* Token globals consumed by asm_expr_* / asm_int_expr. */
int tok;
CValue tokc;

/* tccgen.c: push a new global identifier.  Tests treat the result as opaque
 * and only inspect the fields touched by tccasm.c. */
Sym *global_identifier_push(int v, int t, int c)
{
  Sym *sym = tcc_mallocz(sizeof(Sym));
  sym->v = v;
  sym->type.t = t;
  sym->c = c;
  sym->prev = global_stack;
  global_stack = sym;
  return sym;
}

/* tccpp.c: advance the lexer.  Tests preset the next token with
 * ut_set_next_token(); without that the stream ends immediately. */
static int ut_next_token = TOK_EOF;

static void ut_set_next_token(int t)
{
  ut_next_token = t;
}

void next(void)
{
  tok = ut_next_token;
  ut_next_token = TOK_EOF;
}

/* tccpp.c: consume an expected token.  Tests never feed mismatched input. */
void skip(int c)
{
  (void)c;
  next();
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

UT_TEST(test_asm_label_find_returns_null_for_missing_name)
{
  int name = token_for_name("missing_label");
  tcc_state->leading_underscore = 0;

  UT_ASSERT(asm_label_find(name) == NULL);
  return 0;
}

UT_TEST(test_asm_label_push_creates_asm_symbol)
{
  int name = token_for_name("asm_sym");
  Sym *sym;
  tcc_state->leading_underscore = 0;

  sym = asm_label_push(name);

  UT_ASSERT(sym != NULL);
  UT_ASSERT((sym->type.t & VT_ASM) != 0);
  UT_ASSERT((sym->type.t & VT_EXTERN) != 0);
  UT_ASSERT((sym->type.t & VT_STATIC) != 0);
  UT_ASSERT_EQ(sym->v, name);

  global_stack = sym->prev;
  tcc_free(sym);
  return 0;
}

UT_TEST(test_asm_label_push_records_original_label_for_dotted_cname)
{
  int name = token_for_name("plain");
  Sym *sym;
  tcc_state->leading_underscore = 1;

  sym = asm_label_push(name);

  UT_ASSERT(sym != NULL);
  UT_ASSERT(strcmp(get_tok_str(sym->v, NULL), ".plain") == 0);
  UT_ASSERT_EQ(sym->asm_label, name);

  global_stack = sym->prev;
  tcc_free(sym);
  return 0;
}

UT_TEST(test_get_asm_sym_creates_new_symbol)
{
  int name = token_for_name("newsym");
  Sym *sym;
  tcc_state->leading_underscore = 0;

  sym = tccasm_ut_get_asm_sym(name, NULL);

  UT_ASSERT(sym != NULL);
  UT_ASSERT((sym->type.t & VT_ASM) != 0);

  global_stack = sym->prev;
  tcc_free(sym);
  return 0;
}

UT_TEST(test_get_asm_sym_copies_csym_c_field)
{
  int name = token_for_name("copied");
  Sym csym = {0};
  Sym *sym;
  tcc_state->leading_underscore = 0;
  csym.c = 0x1234;

  sym = tccasm_ut_get_asm_sym(name, &csym);

  UT_ASSERT(sym != NULL);
  UT_ASSERT_EQ(sym->c, 0x1234);

  global_stack = sym->prev;
  tcc_free(sym);
  return 0;
}

UT_TEST(test_asm_int_expr_parses_ppnum_constant)
{
  int v;
  tok = TOK_PPNUM;
  tokc.str.data = "42";
  tokc.str.size = 3;

  v = tccasm_ut_asm_int_expr(tcc_state);

  UT_ASSERT_EQ(v, 42);
  return 0;
}

UT_TEST(test_asm_expr_unary_parses_char_constant)
{
  ExprValue e;
  tok = TOK_CCHAR;
  tokc.i = 'Z';

  asm_expr_unary(tcc_state, &e);

  UT_ASSERT_EQ(e.v, 'Z');
  UT_ASSERT(e.sym == NULL);
  UT_ASSERT_EQ(e.pcrel, 0);
  return 0;
}

UT_TEST(test_asm_expr_unary_parses_identifier_as_symbol_reference)
{
  int name = token_for_name("symref");
  ExprValue e;
  tcc_state->leading_underscore = 0;
  tok = name;

  asm_expr_unary(tcc_state, &e);

  UT_ASSERT(e.sym != NULL);
  UT_ASSERT_EQ(e.v, 0);
  UT_ASSERT_EQ(e.pcrel, 0);

  global_stack = e.sym->prev;
  tcc_free(e.sym);
  return 0;
}

UT_TEST(test_asm_expr_unary_parses_ppnum_constant)
{
  ExprValue e;
  tok = TOK_PPNUM;
  tokc.str.data = "42";
  tokc.str.size = 3;

  asm_expr_unary(tcc_state, &e);

  UT_ASSERT_EQ(e.v, 42);
  UT_ASSERT(e.sym == NULL);
  UT_ASSERT_EQ(e.pcrel, 0);
  return 0;
}

UT_TEST(test_asm_expr_unary_parses_hex_and_octal_constants)
{
  ExprValue e;

  tok = TOK_PPNUM;
  tokc.str.data = "0x1f";
  tokc.str.size = 5;
  asm_expr_unary(tcc_state, &e);
  UT_ASSERT_EQ(e.v, 31);

  tok = TOK_PPNUM;
  tokc.str.data = "010";
  tokc.str.size = 4;
  asm_expr_unary(tcc_state, &e);
  UT_ASSERT_EQ(e.v, 8);
  return 0;
}

UT_TEST(test_asm_expr_unary_negates_constant)
{
  ExprValue e;
  tok = '-';
  ut_set_next_token(TOK_PPNUM);
  tokc.str.data = "7";
  tokc.str.size = 2;

  asm_expr_unary(tcc_state, &e);

  UT_ASSERT_EQ(e.v, -7);
  UT_ASSERT(e.sym == NULL);
  return 0;
}

UT_TEST(test_asm_expr_unary_bitwise_not_constant)
{
  ExprValue e;
  tok = '~';
  ut_set_next_token(TOK_PPNUM);
  tokc.str.data = "0";
  tokc.str.size = 2;

  asm_expr_unary(tcc_state, &e);

  UT_ASSERT_EQ((int64_t)e.v, ~0);
  UT_ASSERT(e.sym == NULL);
  return 0;
}

UT_TEST(test_asm_expr_unary_no_op_plus_constant)
{
  ExprValue e;
  tok = '+';
  ut_set_next_token(TOK_PPNUM);
  tokc.str.data = "9";
  tokc.str.size = 2;

  asm_expr_unary(tcc_state, &e);

  UT_ASSERT_EQ(e.v, 9);
  UT_ASSERT(e.sym == NULL);
  return 0;
}

UT_TEST(test_asm_expr_unary_no_op_equals_constant)
{
  ExprValue e;
  tok = '=';
  ut_set_next_token(TOK_PPNUM);
  tokc.str.data = "3";
  tokc.str.size = 2;

  asm_expr_unary(tcc_state, &e);

  UT_ASSERT_EQ(e.v, 3);
  UT_ASSERT(e.sym == NULL);
  return 0;
}

UT_TEST(test_asm_expr_unary_parses_wide_char_constant)
{
  ExprValue e;
  tok = TOK_LCHAR;
  tokc.i = 'W';

  asm_expr_unary(tcc_state, &e);

  UT_ASSERT_EQ(e.v, 'W');
  UT_ASSERT(e.sym == NULL);
  return 0;
}

UT_TEST(test_find_constraint_malformed_bracket_returns_minus_one)
{
  ASMOperand operands[3] = {0};
  const char *tail = NULL;

  /* Without a closing ']' find_constraint returns -1 and leaves *tail
   * pointing at the text after the opening bracket. */
  UT_ASSERT_EQ(tccasm_ut_find_constraint(operands, 3, "[dst", &tail), -1);
  UT_ASSERT(tail != NULL);
  UT_ASSERT(strcmp(tail, "dst") == 0);
  return 0;
}

UT_TEST(test_find_constraint_accepts_null_tail_pointer)
{
  ASMOperand operands[3] = {0};

  UT_ASSERT_EQ(tccasm_ut_find_constraint(operands, 3, "1:x", NULL), 1);
  return 0;
}

UT_TEST(test_asm_macros_free_releases_body_and_clears_list)
{
  AsmMacro *m = tcc_mallocz(sizeof(AsmMacro));
  TokenString *body = tcc_mallocz(sizeof(TokenString));

  m->name = token_for_name("with_body");
  m->body = body;
  asm_macros = m;

  asm_macros_free();

  UT_ASSERT(asm_macros == NULL);
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
  UT_RUN(test_asm_label_find_returns_null_for_missing_name);
  UT_RUN(test_asm_label_push_creates_asm_symbol);
  UT_RUN(test_asm_label_push_records_original_label_for_dotted_cname);
  UT_RUN(test_get_asm_sym_creates_new_symbol);
  UT_RUN(test_get_asm_sym_copies_csym_c_field);
  UT_RUN(test_asm_int_expr_parses_ppnum_constant);
  UT_RUN(test_asm_expr_unary_parses_char_constant);
  UT_RUN(test_asm_expr_unary_parses_identifier_as_symbol_reference);
  UT_RUN(test_asm_expr_unary_parses_ppnum_constant);
  UT_RUN(test_asm_expr_unary_parses_hex_and_octal_constants);
  UT_RUN(test_asm_expr_unary_negates_constant);
  UT_RUN(test_asm_expr_unary_bitwise_not_constant);
  UT_RUN(test_asm_expr_unary_no_op_plus_constant);
  UT_RUN(test_asm_expr_unary_no_op_equals_constant);
  UT_RUN(test_asm_expr_unary_parses_wide_char_constant);
  UT_RUN(test_find_constraint_malformed_bracket_returns_minus_one);
  UT_RUN(test_find_constraint_accepts_null_tail_pointer);
  UT_RUN(test_asm_macros_free_releases_body_and_clears_list);
}
