/*
 *  test_tccpp.c - white-box unit tests for isolated helpers in tccpp.c
 *  (build_tccpp/run_unit_tests_tccpp)
 *
 *  Focuses on the helpers that can be exercised without invoking the full
 *  lexer/preprocessor: CString, token interning, token-string buffers, and
 *  the pragma-pack replay handler.
 */

#include "tcc.h"
#include "ut.h"

#include <setjmp.h>
#include <string.h>

static void ut_tccpp_setup(void)
{
  tccpp_new(tcc_state);
}

static void ut_tccpp_teardown(void)
{
  tccpp_delete(tcc_state);
}

/* ============================================================================
 * CString helpers
 * ============================================================================ */

UT_TEST(test_cstr_new_initializes_empty)
{
  CString cstr;
  cstr_new(&cstr);
  UT_ASSERT_EQ(cstr.size, 0);
  UT_ASSERT_EQ(cstr.size_allocated, 0);
  UT_ASSERT(cstr.data == NULL);
  cstr_free(&cstr);
  return 0;
}

UT_TEST(test_cstr_ccat_appends_bytes_and_grows)
{
  CString cstr;
  cstr_new(&cstr);
  cstr_ccat(&cstr, 'a');
  cstr_ccat(&cstr, 'b');
  cstr_ccat(&cstr, 'c');
  UT_ASSERT_EQ(cstr.size, 3);
  UT_ASSERT(cstr.size_allocated >= 3);
  UT_ASSERT_EQ(cstr.data[0], 'a');
  UT_ASSERT_EQ(cstr.data[1], 'b');
  UT_ASSERT_EQ(cstr.data[2], 'c');
  cstr_free(&cstr);
  return 0;
}

UT_TEST(test_cstr_cat_appends_with_various_len_modes)
{
  CString cstr;
  cstr_new(&cstr);

  /* Explicit positive len: copies exactly len bytes, no terminator. */
  cstr_cat(&cstr, "ab", 2);
  cstr_cat(&cstr, "cd", 2);
  UT_ASSERT_EQ(cstr.size, 4);
  UT_ASSERT_EQ(cstr.data[0], 'a');
  UT_ASSERT_EQ(cstr.data[1], 'b');
  UT_ASSERT_EQ(cstr.data[2], 'c');
  UT_ASSERT_EQ(cstr.data[3], 'd');

  /* len == 0 means strlen(str) + 1: include the terminating NUL. */
  cstr_reset(&cstr);
  cstr_cat(&cstr, "ef", 0);
  UT_ASSERT_EQ(cstr.size, 3);
  UT_ASSERT_EQ(cstr.data[0], 'e');
  UT_ASSERT_EQ(cstr.data[1], 'f');
  UT_ASSERT_EQ(cstr.data[2], '\0');

  /* len == -1 means strlen(str): copy bytes without a terminator. */
  cstr_reset(&cstr);
  cstr_cat(&cstr, "gh", -1);
  UT_ASSERT_EQ(cstr.size, 2);
  UT_ASSERT_EQ(cstr.data[0], 'g');
  UT_ASSERT_EQ(cstr.data[1], 'h');

  cstr_free(&cstr);
  return 0;
}

UT_TEST(test_cstr_reset_clears_size_keeps_buffer)
{
  CString cstr;
  cstr_new(&cstr);
  cstr_cat(&cstr, "hello", 5);
  int allocated_before = cstr.size_allocated;
  cstr_reset(&cstr);
  UT_ASSERT_EQ(cstr.size, 0);
  UT_ASSERT_EQ(cstr.size_allocated, allocated_before);
  UT_ASSERT(cstr.data != NULL);
  cstr_free(&cstr);
  return 0;
}

UT_TEST(test_cstr_printf_formats_into_buffer)
{
  CString cstr;
  cstr_new(&cstr);
  int n = cstr_printf(&cstr, "%d %s %x", 42, "foo", 0xff);
  UT_ASSERT(n > 0);
  UT_ASSERT_STREQ(cstr.data, "42 foo ff");
  cstr_free(&cstr);
  return 0;
}

UT_TEST(test_cstr_free_on_zeroed_cstring_is_safe)
{
  CString cstr;
  memset(&cstr, 0, sizeof(cstr));
  /* data is NULL; production code calls tcc_free(NULL), which is a no-op. */
  cstr_free(&cstr);
  return 0;
}

UT_TEST(test_cstr_wccat_appends_wide_chars)
{
  CString cstr;
  cstr_new(&cstr);
  cstr_wccat(&cstr, 'A');
  cstr_wccat(&cstr, 'B');
  UT_ASSERT_EQ(cstr.size, 2 * (int)sizeof(nwchar_t));
  const nwchar_t *w = (const nwchar_t *)cstr.data;
  UT_ASSERT_EQ(w[0], 'A');
  UT_ASSERT_EQ(w[1], 'B');
  cstr_free(&cstr);
  return 0;
}

/* ============================================================================
 * Token interning
 * ============================================================================ */

UT_TEST(test_tok_alloc_returns_same_token_for_same_string)
{
  TokenSym *ts1 = tok_alloc("myidentifier", 12);
  TokenSym *ts2 = tok_alloc("myidentifier", 12);
  UT_ASSERT(ts1 != NULL);
  UT_ASSERT(ts1 == ts2);
  UT_ASSERT_EQ(ts1->tok, ts2->tok);
  UT_ASSERT_EQ(ts1->len, 12);
  UT_ASSERT_STREQ(ts1->str, "myidentifier");
  return 0;
}

UT_TEST(test_tok_alloc_returns_distinct_tokens_for_distinct_strings)
{
  TokenSym *ts1 = tok_alloc("alpha", 5);
  TokenSym *ts2 = tok_alloc("beta", 4);
  UT_ASSERT(ts1 != ts2);
  UT_ASSERT(ts1->tok != ts2->tok);
  return 0;
}

UT_TEST(test_tok_alloc_materializes_builtin_keyword_at_fixed_id)
{
  TokenSym *ts_if = tok_alloc("if", 2);
  UT_ASSERT(ts_if != NULL);
  UT_ASSERT_EQ(ts_if->tok, TOK_IF);
  UT_ASSERT_EQ(ts_if->len, 2);
  UT_ASSERT_STREQ(ts_if->str, "if");
  return 0;
}

UT_TEST(test_tok_alloc_const_matches_tok_alloc_with_strlen)
{
  int t1 = tok_alloc_const("gamma");
  int t2 = tok_alloc("gamma", 5)->tok;
  UT_ASSERT_EQ(t1, t2);
  return 0;
}

UT_TEST(test_tok_ensure_returns_builtin_symbol)
{
  TokenSym *ts = tok_ensure(TOK_WHILE);
  UT_ASSERT(ts != NULL);
  UT_ASSERT_EQ(ts->tok, TOK_WHILE);
  UT_ASSERT_STREQ(ts->str, "while");
  return 0;
}

UT_TEST(test_tok_ensure_returns_user_symbol_after_tok_alloc)
{
  TokenSym *ts_alloc = tok_alloc("delta", 5);
  TokenSym *ts_ensure = tok_ensure(ts_alloc->tok);
  UT_ASSERT(ts_ensure == ts_alloc);
  return 0;
}

/* ============================================================================
 * get_tok_str rendering
 * ============================================================================ */

UT_TEST(test_get_tok_str_keywords_and_punctuators)
{
  UT_ASSERT_STREQ(get_tok_str(TOK_IF, NULL), "if");
  UT_ASSERT_STREQ(get_tok_str(TOK_RETURN, NULL), "return");
  UT_ASSERT_STREQ(get_tok_str('+', NULL), "+");
  UT_ASSERT_STREQ(get_tok_str(TOK_EQ, NULL), "==");
  UT_ASSERT_STREQ(get_tok_str(TOK_DOTS, NULL), "...");
  UT_ASSERT_STREQ(get_tok_str(TOK_EOF, NULL), "<eof>");
  return 0;
}

UT_TEST(test_get_tok_str_user_identifier)
{
  TokenSym *ts = tok_alloc("epsilon", 7);
  UT_ASSERT_STREQ(get_tok_str(ts->tok, NULL), "epsilon");
  return 0;
}

UT_TEST(test_get_tok_str_integer_constant)
{
  CValue cv;
  cv.i = 12345;
  UT_ASSERT_STREQ(get_tok_str(TOK_CINT, &cv), "12345");
  return 0;
}

UT_TEST(test_get_tok_str_character_constant)
{
  CValue cv;
  cv.i = 'A';
  UT_ASSERT_STREQ(get_tok_str(TOK_CCHAR, &cv), "'A'");
  cv.i = '\n';
  UT_ASSERT_STREQ(get_tok_str(TOK_CCHAR, &cv), "'\\n'");
  cv.i = 1;
  UT_ASSERT_STREQ(get_tok_str(TOK_CCHAR, &cv), "'\\001'");
  cv.i = '\\';
  UT_ASSERT_STREQ(get_tok_str(TOK_CCHAR, &cv), "'\\\\'");
  return 0;
}

UT_TEST(test_get_tok_str_string_literal)
{
  CValue cv;
  static char hello[] = "hello";
  cv.str.data = hello;
  cv.str.size = sizeof(hello);
  UT_ASSERT_STREQ(get_tok_str(TOK_STR, &cv), "\"hello\"");

  static char esc[] = "\001";
  cv.str.data = esc;
  cv.str.size = sizeof(esc);
  UT_ASSERT_STREQ(get_tok_str(TOK_STR, &cv), "\"\\001\"");
  return 0;
}

/* ============================================================================
 * TokenString helpers
 * ============================================================================ */

UT_TEST(test_tok_str_alloc_initializes_empty)
{
  TokenString *str = tok_str_alloc();
  UT_ASSERT(str != NULL);
  UT_ASSERT_EQ(str->len, 0);
  UT_ASSERT_EQ(str->allocated_len, 0);
  tok_str_free(str);
  return 0;
}

UT_TEST(test_tok_str_add_stays_inline_then_grows)
{
  TokenString str;
  tok_str_new(&str);
  for (int i = 0; i < TOKSTR_SMALL_BUFSIZE + 4; i++)
    tok_str_add(&str, TOK_IDENT + i);
  UT_ASSERT_EQ(str.len, TOKSTR_SMALL_BUFSIZE + 4);
  UT_ASSERT(str.allocated_len > TOKSTR_SMALL_BUFSIZE);
  for (int i = 0; i < TOKSTR_SMALL_BUFSIZE + 4; i++)
    UT_ASSERT_EQ(tok_str_buf(&str)[i], TOK_IDENT + i);
  tok_str_free_str(tok_str_ensure_heap(&str));
  return 0;
}

UT_TEST(test_tok_str_add2_integer_round_trip)
{
  TokenString str;
  tok_str_new(&str);
  CValue cv;
  cv.i = 0xdeadbeef;
  tok_str_add2(&str, TOK_CINT, &cv);
  UT_ASSERT_EQ(str.len, 2);
  UT_ASSERT_EQ(tok_str_buf(&str)[0], TOK_CINT);
  UT_ASSERT_EQ((uint32_t)tok_str_buf(&str)[1], 0xdeadbeefU);
  tok_str_free_str(tok_str_ensure_heap(&str));
  return 0;
}

UT_TEST(test_tok_str_ensure_heap_empty_returns_null)
{
  TokenString str;
  tok_str_new(&str);
  int *heap = tok_str_ensure_heap(&str);
  UT_ASSERT(heap == NULL);
  tok_str_free_str(heap); /* must be a no-op */
  return 0;
}

UT_TEST(test_tok_str_ensure_heap_converts_inline_to_heap)
{
  TokenString str;
  tok_str_new(&str);
  tok_str_add(&str, TOK_IF);
  UT_ASSERT_EQ(str.allocated_len, 0);
  int *heap = tok_str_ensure_heap(&str);
  UT_ASSERT(heap != NULL);
  UT_ASSERT(str.allocated_len > 0);
  UT_ASSERT_EQ(heap[0], TOK_IF);
  tok_str_free_str(heap);
  return 0;
}

UT_TEST(test_tok_str_free_releases_heap_and_struct)
{
  TokenString *str = tok_str_alloc();
  CValue cv;
  cv.i = 1;
  tok_str_add2(str, TOK_CINT, &cv);
  tok_str_free(str);
  return 0;
}

/* ============================================================================
 * tok_get round-trip on a hand-built token stream
 * ============================================================================ */

UT_TEST(test_tok_get_round_trip_int_string_eof)
{
  TokenString str;
  tok_str_new(&str);

  CValue cv;
  cv.i = 42;
  tok_str_add2(&str, TOK_CINT, &cv);

  static char hello[] = "hello";
  cv.str.data = hello;
  cv.str.size = sizeof(hello);
  tok_str_add2(&str, TOK_STR, &cv);

  tok_str_add(&str, TOK_EOF);

  const int *p = tok_str_buf(&str);
  CValue cv_out;
  int t;

  tok_get(&t, &p, &cv_out);
  UT_ASSERT_EQ(t, TOK_CINT);
  UT_ASSERT_EQ(cv_out.i, 42);

  tok_get(&t, &p, &cv_out);
  UT_ASSERT_EQ(t, TOK_STR);
  UT_ASSERT_EQ(cv_out.str.size, (int)sizeof(hello));
  UT_ASSERT_STREQ(cv_out.str.data, "hello");

  tok_get(&t, &p, &cv_out);
  UT_ASSERT_EQ(t, TOK_EOF);

  tok_str_free_str(tok_str_ensure_heap(&str));
  return 0;
}

/* ============================================================================
 * Misc public helpers
 * ============================================================================ */

UT_TEST(test_set_idnum_changes_character_class)
{
  int prev_dot = set_idnum('.', IS_ID);
  UT_ASSERT_EQ(prev_dot, 0);
  int prev_id = set_idnum('.', 0);
  UT_ASSERT_EQ(prev_id, IS_ID);

  int prev_digit = set_idnum('7', 0);
  UT_ASSERT_EQ(prev_digit, IS_NUM);
  UT_ASSERT_EQ(set_idnum('7', IS_NUM), 0);
  return 0;
}

UT_TEST(test_tok_str_add_tok_line_number_tracking)
{
  static struct BufferedFile bf;
  memset(&bf, 0, sizeof(bf));
  bf.line_num = 10;
  file = &bf;

  TokenString str;
  tok_str_new(&str);
  tok = '+';
  tok_str_add_tok(&str);
  UT_ASSERT_EQ(str.len, 3);
  const int *p = tok_str_buf(&str);
  UT_ASSERT_EQ(p[0], TOK_LINENUM);
  UT_ASSERT_EQ(p[1], 10);
  UT_ASSERT_EQ(p[2], '+');

  tok_str_add_tok(&str);
  UT_ASSERT_EQ(str.len, 4);
  UT_ASSERT_EQ(p[3], '+');

  bf.line_num = 12;
  tok_str_add_tok(&str);
  UT_ASSERT_EQ(str.len, 7);
  UT_ASSERT_EQ(p[4], TOK_LINENUM);
  UT_ASSERT_EQ(p[5], 12);
  UT_ASSERT_EQ(p[6], '+');

  file = NULL;
  tok_str_free_str(tok_str_ensure_heap(&str));
  return 0;
}

UT_TEST(test_begin_macro_end_macro_restores_macro_ptr)
{
  static struct BufferedFile bf;
  memset(&bf, 0, sizeof(bf));
  bf.line_num = 1;
  file = &bf;

  const int *saved_macro_ptr = macro_ptr;

  TokenString *str = tok_str_alloc();
  tok_str_add(str, TOK_IF);

  begin_macro(str, 1);
  UT_ASSERT(macro_ptr == tok_str_buf(str));
  UT_ASSERT_EQ(str->alloc, 1);

  end_macro();
  UT_ASSERT(macro_ptr == saved_macro_ptr);

  file = NULL;
  return 0;
}

UT_TEST(test_end_macro_to_unwinds_to_target)
{
  static struct BufferedFile bf;
  memset(&bf, 0, sizeof(bf));
  bf.line_num = 1;
  file = &bf;

  const int *saved_macro_ptr = macro_ptr;

  TokenString *first = tok_str_alloc();
  TokenString *second = tok_str_alloc();

  begin_macro(first, 1);
  begin_macro(second, 1);
  UT_ASSERT(macro_ptr == tok_str_buf(second));

  end_macro_to(first);
  UT_ASSERT(macro_ptr == saved_macro_ptr);

  file = NULL;
  return 0;
}

extern Sym *define_stack;

UT_TEST(test_define_undef_clears_sym_define)
{
  TokenSym *ts = tok_alloc("undefme", 7);
  Sym *s = tcc_mallocz(sizeof(Sym));
  s->v = ts->tok;
  ts->sym_define = s;
  define_undef(s);
  UT_ASSERT(ts->sym_define == NULL);
  tcc_free(s);
  return 0;
}

UT_TEST(test_free_defines_pops_to_boundary)
{
  TokenSym *ts = tok_alloc("freeme", 6);
  Sym *boundary = define_stack;
  Sym *s = tcc_mallocz(sizeof(Sym));
  s->v = ts->tok;
  s->prev = define_stack;
  define_stack = s;
  ts->sym_define = s;

  free_defines(boundary);
  UT_ASSERT(define_stack == boundary);
  UT_ASSERT(ts->sym_define == NULL);
  return 0;
}

/* ============================================================================
 * #pragma pack replay state changes
 * ============================================================================ */

UT_TEST(test_pp_apply_pack_replay_set_push_pop)
{
  tcc_state->pack_stack_ptr = tcc_state->pack_stack;

  pp_apply_pack_replay(tcc_state,
                       (TCC_PCH_REPLAY_PACK_SET << 16) | 2);
  UT_ASSERT_EQ(*tcc_state->pack_stack_ptr, 2);

  pp_apply_pack_replay(tcc_state,
                       (TCC_PCH_REPLAY_PACK_PUSH << 16) | 4);
  UT_ASSERT(tcc_state->pack_stack_ptr == tcc_state->pack_stack + 1);
  UT_ASSERT_EQ(*tcc_state->pack_stack_ptr, 4);

  pp_apply_pack_replay(tcc_state,
                       (TCC_PCH_REPLAY_PACK_POP << 16));
  UT_ASSERT(tcc_state->pack_stack_ptr == tcc_state->pack_stack);
  UT_ASSERT_EQ(*tcc_state->pack_stack_ptr, 2);
  return 0;
}

UT_TEST(test_pp_apply_pack_replay_pop_empty_stack_errors)
{
  tcc_state->pack_stack_ptr = tcc_state->pack_stack;
  tcc_state->error_set_jmp_enabled = 1;
  if (setjmp(tcc_state->error_jmp_buf) == 0)
  {
    pp_apply_pack_replay(tcc_state, (TCC_PCH_REPLAY_PACK_POP << 16));
    /* If we get here, no error was raised. */
    tcc_state->error_set_jmp_enabled = 0;
    return -1;
  }
  /* Longjmp returned: error was raised as expected. */
  tcc_state->error_set_jmp_enabled = 0;
  return 0;
}

UT_TEST(test_pp_apply_pack_replay_push_full_stack_errors)
{
  tcc_state->pack_stack_ptr = tcc_state->pack_stack + PACK_STACK_SIZE - 1;
  tcc_state->error_set_jmp_enabled = 1;
  if (setjmp(tcc_state->error_jmp_buf) == 0)
  {
    pp_apply_pack_replay(tcc_state,
                         (TCC_PCH_REPLAY_PACK_PUSH << 16) | 1);
    tcc_state->error_set_jmp_enabled = 0;
    return -1;
  }
  tcc_state->error_set_jmp_enabled = 0;
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(tccpp)
{
  ut_tccpp_setup();

  /* CString */
  UT_RUN(test_cstr_new_initializes_empty);
  UT_RUN(test_cstr_ccat_appends_bytes_and_grows);
  UT_RUN(test_cstr_cat_appends_with_various_len_modes);
  UT_RUN(test_cstr_reset_clears_size_keeps_buffer);
  UT_RUN(test_cstr_printf_formats_into_buffer);
  UT_RUN(test_cstr_free_on_zeroed_cstring_is_safe);
  UT_RUN(test_cstr_wccat_appends_wide_chars);

  /* Token interning */
  UT_RUN(test_tok_alloc_returns_same_token_for_same_string);
  UT_RUN(test_tok_alloc_returns_distinct_tokens_for_distinct_strings);
  UT_RUN(test_tok_alloc_materializes_builtin_keyword_at_fixed_id);
  UT_RUN(test_tok_alloc_const_matches_tok_alloc_with_strlen);
  UT_RUN(test_tok_ensure_returns_builtin_symbol);
  UT_RUN(test_tok_ensure_returns_user_symbol_after_tok_alloc);

  /* get_tok_str */
  UT_RUN(test_get_tok_str_keywords_and_punctuators);
  UT_RUN(test_get_tok_str_user_identifier);
  UT_RUN(test_get_tok_str_integer_constant);
  UT_RUN(test_get_tok_str_character_constant);
  UT_RUN(test_get_tok_str_string_literal);

  /* TokenString */
  UT_RUN(test_tok_str_alloc_initializes_empty);
  UT_RUN(test_tok_str_add_stays_inline_then_grows);
  UT_RUN(test_tok_str_add2_integer_round_trip);
  UT_RUN(test_tok_str_ensure_heap_empty_returns_null);
  UT_RUN(test_tok_str_ensure_heap_converts_inline_to_heap);
  UT_RUN(test_tok_str_free_releases_heap_and_struct);

  /* tok_get round-trip */
  UT_RUN(test_tok_get_round_trip_int_string_eof);

  /* Misc public helpers */
  UT_RUN(test_set_idnum_changes_character_class);
  UT_RUN(test_tok_str_add_tok_line_number_tracking);
  UT_RUN(test_begin_macro_end_macro_restores_macro_ptr);
  UT_RUN(test_end_macro_to_unwinds_to_target);
  UT_RUN(test_define_undef_clears_sym_define);
  UT_RUN(test_free_defines_pops_to_boundary);

  /* #pragma pack replay */
  UT_RUN(test_pp_apply_pack_replay_set_push_pop);
  UT_RUN(test_pp_apply_pack_replay_pop_empty_stack_errors);
  UT_RUN(test_pp_apply_pack_replay_push_full_stack_errors);

  ut_tccpp_teardown();
}
