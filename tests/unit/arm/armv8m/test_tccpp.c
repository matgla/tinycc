/*
 *  test_tccpp.c - white-box unit tests for isolated helpers in tccpp.c
 *  (build_tccpp/run_unit_tests_tccpp)
 *
 *  Focuses on the helpers that can be exercised without invoking the full
 *  compiler pipeline: CString, token interning, token-string buffers,
 *  pragma-pack replay, and preprocessor directive / lexer smoke tests.
 */

#include "tcc.h"
#include "ut.h"

#include <setjmp.h>
#include <string.h>

ST_FUNC void cstr_u8cat(CString *cstr, int ch);
ST_FUNC int cstr_vprintf(CString *cstr, const char *fmt, va_list ap);
ST_FUNC int *tok_str_realloc(TokenString *s, int new_size);
ST_FUNC void pp_error(CString *cs);
ST_FUNC void parse_define(void);
ST_FUNC void tccpp_putfile(const char *filename);
ST_FUNC void preprocess_start(TCCState *s1, int filetype);
ST_FUNC void preprocess_end(TCCState *s1);
ST_FUNC int tcc_preprocess(TCCState *s1);

static void ut_tccpp_setup(void)
{
  tccpp_new(tcc_state);
}

static void ut_tccpp_teardown(void)
{
  tccpp_delete(tcc_state);
}

UT_SUITE_SETUP(ut_tccpp_setup);
UT_SUITE_TEARDOWN(ut_tccpp_teardown);

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

/* ============================================================================
 * Helpers for tests that need a minimal input file
 * ============================================================================ */

static struct BufferedFile ut_input_bf;
static unsigned char ut_input_buf[512];

static int ut_open_input(const char *s)
{
  size_t n = strlen(s);
  UT_ASSERT(n + 3 <= sizeof(ut_input_buf));
  ut_input_buf[0] = ' '; /* dummy byte so buf_ptr-1 stays in range */
  memcpy(ut_input_buf + 1, s, n);
  ut_input_buf[1 + n] = CH_EOB;
  memset(&ut_input_bf, 0, sizeof(ut_input_bf));
  ut_input_bf.buf_ptr = ut_input_buf + 1;
  ut_input_bf.buf_end = ut_input_buf + 1 + n;
  ut_input_bf.fd = -1;
  ut_input_bf.line_num = 1;
  ut_input_bf.line_ref = 1;
  ut_input_bf.true_filename = ut_input_bf.filename;
  ut_input_bf.filename[0] = '\0';
  ut_input_bf.ifdef_stack_ptr = tcc_state->ifdef_stack;
  file = &ut_input_bf;
  tok_flags = TOK_FLAG_BOL;
  return 0;
}

static void ut_reset_pp_state(void)
{
  tcc_state->include_stack_ptr = tcc_state->include_stack;
  tcc_state->ifdef_stack_ptr = tcc_state->ifdef_stack;
}

static int call_cstr_vprintf(CString *cstr, const char *fmt, ...)
{
  va_list ap;
  int n;

  va_start(ap, fmt);
  n = cstr_vprintf(cstr, fmt, ap);
  va_end(ap);
  return n;
}

/* ============================================================================
 * Additional CString helper tests
 * ============================================================================ */

UT_TEST(test_cstr_u8cat_encodes_unicode)
{
  CString cstr;
  cstr_new(&cstr);
  cstr_u8cat(&cstr, 'A');
  cstr_u8cat(&cstr, 0xE9);
  cstr_u8cat(&cstr, 0x20AC);
  cstr_u8cat(&cstr, 0x4F60);
  UT_ASSERT_EQ(cstr.size, 9);
  const unsigned char *p = (const unsigned char *)cstr.data;
  UT_ASSERT_EQ(p[0], 'A');
  UT_ASSERT_EQ(p[1], 0xC3);
  UT_ASSERT_EQ(p[2], 0xA9);
  UT_ASSERT_EQ(p[3], 0xE2);
  UT_ASSERT_EQ(p[4], 0x82);
  UT_ASSERT_EQ(p[5], 0xAC);
  UT_ASSERT_EQ(p[6], 0xE4);
  UT_ASSERT_EQ(p[7], 0xBD);
  UT_ASSERT_EQ(p[8], 0xA0);
  cstr_free(&cstr);
  return 0;
}

UT_TEST(test_cstr_u8cat_rejects_surrogate)
{
  CString cstr;
  cstr_new(&cstr);
  tcc_state->error_set_jmp_enabled = 1;
  if (setjmp(tcc_state->error_jmp_buf) == 0)
  {
    cstr_u8cat(&cstr, 0xD800);
    tcc_state->error_set_jmp_enabled = 0;
    cstr_free(&cstr);
    return -1;
  }
  tcc_state->error_set_jmp_enabled = 0;
  cstr_free(&cstr);
  return 0;
}

UT_TEST(test_cstr_printf_reallocs_for_long_format)
{
  CString cstr;
  cstr_new(&cstr);
  char payload[200];
  memset(payload, 'x', sizeof(payload) - 1);
  payload[sizeof(payload) - 1] = '\0';
  int n = cstr_printf(&cstr, "prefix %s suffix", payload);
  UT_ASSERT_EQ(n, 14 + (int)sizeof(payload) - 1);
  UT_ASSERT_EQ(cstr.size, n);
  UT_ASSERT(cstr.size_allocated >= n + 1);
  UT_ASSERT(strncmp(cstr.data, "prefix ", 7) == 0);
  UT_ASSERT(strstr(cstr.data, payload) != NULL);
  UT_ASSERT_EQ(cstr.data[n], '\0');
  cstr_free(&cstr);
  return 0;
}

UT_TEST(test_cstr_cat_len_minus_one_on_empty_string)
{
  CString cstr;
  cstr_new(&cstr);
  cstr_cat(&cstr, "", -1);
  UT_ASSERT_EQ(cstr.size, 0);
  cstr_cat(&cstr, "x", -1);
  UT_ASSERT_EQ(cstr.size, 1);
  cstr_free(&cstr);
  return 0;
}

/* ============================================================================
 * Additional get_tok_str tests
 * ============================================================================ */

UT_TEST(test_get_tok_str_float_and_special_tokens)
{
  CValue cv;
  memset(&cv, 0, sizeof(cv));
  UT_ASSERT_STREQ(get_tok_str(TOK_CFLOAT, &cv), "<float>");
  UT_ASSERT_STREQ(get_tok_str(TOK_CDOUBLE, &cv), "<double>");
  UT_ASSERT_STREQ(get_tok_str(TOK_CLDOUBLE, &cv), "<long double>");
  UT_ASSERT_STREQ(get_tok_str(TOK_CFLOAT_I, &cv), "<imaginary float>");
  UT_ASSERT_STREQ(get_tok_str(TOK_CDOUBLE_I, &cv), "<imaginary double>");
  UT_ASSERT_STREQ(get_tok_str(TOK_CLDOUBLE_I, &cv), "<imaginary long double>");
  UT_ASSERT_STREQ(get_tok_str(TOK_CINT_I, &cv), "<imaginary int>");
  UT_ASSERT_STREQ(get_tok_str(TOK_LINENUM, &cv), "<linenumber>");
  UT_ASSERT_STREQ(get_tok_str(TOK_PACK_REPLAY, &cv), "<pack-replay>");
  return 0;
}

UT_TEST(test_get_tok_str_pp_tokens)
{
  CValue cv;
  static char num[] = "123";
  cv.str.data = num;
  cv.str.size = sizeof(num);
  UT_ASSERT_STREQ(get_tok_str(TOK_PPNUM, &cv), "123");
  UT_ASSERT_STREQ(get_tok_str(TOK_PPSTR, &cv), "123");
  return 0;
}

UT_TEST(test_get_tok_str_wide_char_and_string)
{
  CValue cv;
  cv.i = 'A';
  UT_ASSERT_STREQ(get_tok_str(TOK_LCHAR, &cv), "L'A'");
  static nwchar_t whello[] = L"hello";
  cv.str.data = (char *)whello;
  cv.str.size = sizeof(whello);
  UT_ASSERT_STREQ(get_tok_str(TOK_LSTR, &cv), "L\"hello\"");
  return 0;
}

UT_TEST(test_get_tok_str_anonymous_and_nameless)
{
  UT_ASSERT_STREQ(get_tok_str(SYM_FIRST_ANOM + 3, NULL), "L.3");
  UT_ASSERT_STREQ(get_tok_str(0, NULL), "<no name>");
  return 0;
}

UT_TEST(test_get_tok_str_invalid_control_char)
{
  UT_ASSERT_STREQ(get_tok_str(1, NULL), "<\\x01>");
  UT_ASSERT_STREQ(get_tok_str(127, NULL), "<\\x7f>");
  return 0;
}

/* ============================================================================
 * Additional TokenString / tok_get tests
 * ============================================================================ */

UT_TEST(test_tok_str_add2_string_round_trip)
{
  TokenString str;
  tok_str_new(&str);
  static char hello[] = "hello";
  CValue cv;
  cv.str.data = hello;
  cv.str.size = sizeof(hello);
  tok_str_add2(&str, TOK_STR, &cv);
  const int *p = tok_str_buf(&str);
  CValue cv_out;
  int t;
  tok_get(&t, &p, &cv_out);
  UT_ASSERT_EQ(t, TOK_STR);
  UT_ASSERT_EQ(cv_out.str.size, (int)sizeof(hello));
  UT_ASSERT_STREQ(cv_out.str.data, "hello");
  tok_str_free_str(tok_str_ensure_heap(&str));
  return 0;
}

UT_TEST(test_tok_get_unsigned_and_double)
{
  TokenString str;
  tok_str_new(&str);
  CValue cv;
  cv.i = 0xFFFFFFFFULL;
  tok_str_add2(&str, TOK_CUINT, &cv);
  cv.d = 2.5;
  tok_str_add2(&str, TOK_CDOUBLE, &cv);
  const int *p = tok_str_buf(&str);
  CValue cv_out;
  int t;
  tok_get(&t, &p, &cv_out);
  UT_ASSERT_EQ(t, TOK_CUINT);
  UT_ASSERT_EQ((unsigned)cv_out.i, 0xFFFFFFFFU);
  tok_get(&t, &p, &cv_out);
  UT_ASSERT_EQ(t, TOK_CDOUBLE);
  UT_ASSERT(cv_out.d == 2.5);
  tok_str_free_str(tok_str_ensure_heap(&str));
  return 0;
}

UT_TEST(test_tok_get_line_and_pack_replay)
{
  TokenString str;
  tok_str_new(&str);
  CValue cv;
  cv.i = 42;
  tok_str_add2(&str, TOK_LINENUM, &cv);
  cv.i = (TCC_PCH_REPLAY_PACK_SET << 16) | 4;
  tok_str_add2(&str, TOK_PACK_REPLAY, &cv);
  const int *p = tok_str_buf(&str);
  CValue cv_out;
  int t;
  tok_get(&t, &p, &cv_out);
  UT_ASSERT_EQ(t, TOK_LINENUM);
  UT_ASSERT_EQ(cv_out.i, 42);
  tok_get(&t, &p, &cv_out);
  UT_ASSERT_EQ(t, TOK_PACK_REPLAY);
  UT_ASSERT_EQ(cv_out.i, (TCC_PCH_REPLAY_PACK_SET << 16) | 4);
  tok_str_free_str(tok_str_ensure_heap(&str));
  return 0;
}

UT_TEST(test_tok_get_ppnum_ppstr_round_trip)
{
  TokenString str;
  tok_str_new(&str);
  static char ppn[] = "123abc";
  CValue cv;
  cv.str.data = ppn;
  cv.str.size = sizeof(ppn);
  tok_str_add2(&str, TOK_PPNUM, &cv);
  static char pps[] = "\"hi\"";
  cv.str.data = pps;
  cv.str.size = sizeof(pps);
  tok_str_add2(&str, TOK_PPSTR, &cv);
  const int *p = tok_str_buf(&str);
  CValue cv_out;
  int t;
  tok_get(&t, &p, &cv_out);
  UT_ASSERT_EQ(t, TOK_PPNUM);
  UT_ASSERT_EQ(cv_out.str.size, (int)sizeof(ppn));
  UT_ASSERT_STREQ(cv_out.str.data, ppn);
  tok_get(&t, &p, &cv_out);
  UT_ASSERT_EQ(t, TOK_PPSTR);
  UT_ASSERT_EQ(cv_out.str.size, (int)sizeof(pps));
  UT_ASSERT_STREQ(cv_out.str.data, pps);
  tok_str_free_str(tok_str_ensure_heap(&str));
  return 0;
}

/* ============================================================================
 * define_push / define_find / macro_is_equal tests
 * ============================================================================ */

UT_TEST(test_define_push_and_find_object_macro)
{
  TokenSym *ts = tok_alloc("objmac", 6);
  TokenString str;
  tok_str_new(&str);
  CValue cv;
  cv.i = 123;
  tok_str_add2(&str, TOK_CINT, &cv);
  tok_str_add(&str, 0);
  int *body = tok_str_ensure_heap(&str);
  Sym *boundary = define_stack;
  define_push(ts->tok, MACRO_OBJ, body, NULL);
  Sym *s = define_find(ts->tok);
  UT_ASSERT(s != NULL);
  UT_ASSERT_EQ(s->v, ts->tok);
  UT_ASSERT_EQ(s->type.t & MACRO_FUNC, 0);
  UT_ASSERT_EQ(s->d[0], TOK_CINT);
  UT_ASSERT_EQ(s->d[1], 123);
  free_defines(boundary);
  return 0;
}

UT_TEST(test_define_push_redefinition_checks_equality)
{
  TokenSym *ts = tok_alloc("redef", 5);
  TokenString str1, str2;
  tok_str_new(&str1);
  tok_str_new(&str2);
  CValue cv;
  cv.i = 1;
  tok_str_add2(&str1, TOK_CINT, &cv);
  tok_str_add(&str1, 0);
  cv.i = 2;
  tok_str_add2(&str2, TOK_CINT, &cv);
  tok_str_add(&str2, 0);
  int *d1 = tok_str_ensure_heap(&str1);
  int *d2 = tok_str_ensure_heap(&str2);
  Sym *boundary = define_stack;
  define_push(ts->tok, MACRO_OBJ, d1, NULL);
  define_push(ts->tok, MACRO_OBJ, d2, NULL);
  Sym *s = define_find(ts->tok);
  UT_ASSERT(s != NULL);
  UT_ASSERT_EQ(s->d[1], 2);
  free_defines(boundary);
  return 0;
}

UT_TEST(test_define_push_function_macro_with_args)
{
  TokenSym *ts = tok_alloc("addfn", 5);
  int xtok = tok_alloc("x", 1)->tok;
  int ytok = tok_alloc("y", 1)->tok;
  Sym *boundary = define_stack;
  sym_push2(&define_stack, xtok | SYM_FIELD, 0, 0);
  Sym *first = define_stack;
  sym_push2(&define_stack, ytok | SYM_FIELD, 0, 0);
  TokenString str;
  tok_str_new(&str);
  tok_str_add(&str, '(');
  tok_str_add(&str, xtok);
  tok_str_add(&str, '+');
  tok_str_add(&str, ytok);
  tok_str_add(&str, ')');
  tok_str_add(&str, 0);
  int *body = tok_str_ensure_heap(&str);
  define_push(ts->tok, MACRO_FUNC, body, first);
  Sym *s = define_find(ts->tok);
  UT_ASSERT(s != NULL);
  UT_ASSERT(s->type.t & MACRO_FUNC);
  UT_ASSERT(s->next == first);
  free_defines(boundary);
  return 0;
}

UT_TEST(test_define_push_equal_body_no_warning)
{
  TokenSym *ts = tok_alloc("samebody", 8);
  TokenString str1, str2;
  tok_str_new(&str1);
  tok_str_new(&str2);
  CValue cv;
  cv.i = 7;
  tok_str_add2(&str1, TOK_CINT, &cv);
  tok_str_add(&str1, 0);
  tok_str_add2(&str2, TOK_CINT, &cv);
  tok_str_add(&str2, 0);
  int *d1 = tok_str_ensure_heap(&str1);
  int *d2 = tok_str_ensure_heap(&str2);
  Sym *boundary = define_stack;
  define_push(ts->tok, MACRO_OBJ, d1, NULL);
  define_push(ts->tok, MACRO_OBJ, d2, NULL);
  Sym *s = define_find(ts->tok);
  UT_ASSERT(s != NULL);
  UT_ASSERT_EQ(s->d[1], 7);
  free_defines(boundary);
  return 0;
}

/* ============================================================================
 * Misc accessible frontend helpers
 * ============================================================================ */

UT_TEST(test_skip_to_eol_skips_logical_line)
{
  UT_ASSERT(ut_open_input("hello world") == 0);
  tok = '+';
  skip_to_eol(0);
  UT_ASSERT_EQ(tok, TOK_LINEFEED);
  UT_ASSERT(file->buf_ptr == file->buf_end);
  file = NULL;
  return 0;
}

UT_TEST(test_skip_to_eol_warns_on_extra_tokens)
{
  UT_ASSERT(ut_open_input("extra") == 0);
  tok = '+';
  skip_to_eol(1);
  UT_ASSERT_EQ(tok, TOK_LINEFEED);
  file = NULL;
  return 0;
}

UT_TEST(test_skip_to_eol_returns_on_linefeed)
{
  UT_ASSERT(ut_open_input("") == 0);
  tok = TOK_LINEFEED;
  skip_to_eol(0);
  UT_ASSERT_EQ(tok, TOK_LINEFEED);
  UT_ASSERT(file->buf_ptr == ut_input_buf + 1);
  file = NULL;
  return 0;
}

UT_TEST(test_expect_raises_error)
{
  tcc_state->error_set_jmp_enabled = 1;
  if (setjmp(tcc_state->error_jmp_buf) == 0)
  {
    expect("some token");
    tcc_state->error_set_jmp_enabled = 0;
    return -1;
  }
  tcc_state->error_set_jmp_enabled = 0;
  return 0;
}

UT_TEST(test_unget_tok_pushes_token_back)
{
  UT_ASSERT(ut_open_input("") == 0);
  tok = '+';
  unget_tok('*');
  UT_ASSERT_EQ(tok, '*');
  UT_ASSERT(macro_ptr != NULL);
  end_macro();
  UT_ASSERT(macro_ptr == NULL);
  file = NULL;
  return 0;
}

UT_TEST(test_unget_tok_allocates_second_buffer)
{
  UT_ASSERT(ut_open_input("") == 0);
  tok = '+';
  unget_tok('*');
  unget_tok('/');
  UT_ASSERT_EQ(tok, '/');
  UT_ASSERT(macro_ptr != NULL);
  end_macro();
  end_macro();
  UT_ASSERT(macro_ptr == NULL);
  file = NULL;
  return 0;
}

/* ============================================================================
 * Additional coverage for CString / TokenString internals
 * ============================================================================ */

UT_TEST(test_cstr_vprintf_formats_va_list)
{
  CString cstr;
  cstr_new(&cstr);
  int n = call_cstr_vprintf(&cstr, "v=%d s=%s", 7, "bar");
  UT_ASSERT_EQ(n, 9);
  UT_ASSERT_STREQ(cstr.data, "v=7 s=bar");
  cstr_free(&cstr);
  return 0;
}

UT_TEST(test_tok_str_realloc_inline_to_heap)
{
  TokenString str;
  tok_str_new(&str);
  int *heap = tok_str_realloc(&str, TOKSTR_SMALL_BUFSIZE + 1);
  UT_ASSERT(heap != NULL);
  UT_ASSERT(str.allocated_len > 0);
  UT_ASSERT(heap == str.data.str);
  UT_ASSERT(heap != str.data.small_buf);
  tok_str_free_str(heap);
  return 0;
}

UT_TEST(test_tok_str_realloc_heap_grows)
{
  TokenString str;
  tok_str_new(&str);
  int *buf1 = tok_str_realloc(&str, 8);
  int alloc1 = str.allocated_len;
  int *buf2 = tok_str_realloc(&str, 200);
  UT_ASSERT(buf1 != NULL);
  UT_ASSERT(str.allocated_len >= 200);
  UT_ASSERT(str.allocated_len > alloc1);
  UT_ASSERT(buf2 == str.data.str);
  tok_str_free_str(buf2);
  return 0;
}

UT_TEST(test_tok_get_long_long_round_trip)
{
  TokenString str;
  tok_str_new(&str);
  CValue cv;
  cv.i = 0x123456789ABCDEF0ULL;
  tok_str_add2(&str, TOK_CLLONG, &cv);
  cv.i = 0xFEDCBA9876543210ULL;
  tok_str_add2(&str, TOK_CULLONG, &cv);

  const int *p = tok_str_buf(&str);
  CValue out;
  int t;
  tok_get(&t, &p, &out);
  UT_ASSERT_EQ(t, TOK_CLLONG);
  UT_ASSERT_EQ(out.i, 0x123456789ABCDEF0ULL);
  tok_get(&t, &p, &out);
  UT_ASSERT_EQ(t, TOK_CULLONG);
  UT_ASSERT_EQ((unsigned long long)out.i, 0xFEDCBA9876543210ULL);
  tok_str_free_str(tok_str_ensure_heap(&str));
  return 0;
}

UT_TEST(test_tok_get_float_double_round_trip)
{
  TokenString str;
  tok_str_new(&str);
  CValue cv;
  cv.f = 1.5f;
  tok_str_add2(&str, TOK_CFLOAT, &cv);
  cv.d = 2.25;
  tok_str_add2(&str, TOK_CDOUBLE, &cv);

  const int *p = tok_str_buf(&str);
  CValue out;
  int t;
  tok_get(&t, &p, &out);
  UT_ASSERT_EQ(t, TOK_CFLOAT);
  UT_ASSERT(out.f == 1.5f);
  tok_get(&t, &p, &out);
  UT_ASSERT_EQ(t, TOK_CDOUBLE);
  UT_ASSERT(out.d == 2.25);
  tok_str_free_str(tok_str_ensure_heap(&str));
  return 0;
}

UT_TEST(test_get_tok_str_long_long_constants)
{
  CValue cv;
  cv.i = 123456789012345ULL;
  UT_ASSERT_STREQ(get_tok_str(TOK_CLLONG, &cv), "123456789012345");
  cv.i = 0xFFFFFFFFFFFFFFFFULL;
  UT_ASSERT_STREQ(get_tok_str(TOK_CULLONG, &cv), "18446744073709551615");
  cv.i = 0x80000000U;
  UT_ASSERT_STREQ(get_tok_str(TOK_CULONG, &cv), "2147483648");
  return 0;
}

UT_TEST(test_get_tok_str_more_two_char_tokens)
{
  UT_ASSERT_STREQ(get_tok_str(TOK_LT, NULL), "<");
  UT_ASSERT_STREQ(get_tok_str(TOK_GT, NULL), ">");
  UT_ASSERT_STREQ(get_tok_str(TOK_LE, NULL), "<=");
  UT_ASSERT_STREQ(get_tok_str(TOK_GE, NULL), ">=");
  UT_ASSERT_STREQ(get_tok_str(TOK_NE, NULL), "!=");
  UT_ASSERT_STREQ(get_tok_str(TOK_LAND, NULL), "&&");
  UT_ASSERT_STREQ(get_tok_str(TOK_LOR, NULL), "||");
  UT_ASSERT_STREQ(get_tok_str(TOK_INC, NULL), "++");
  UT_ASSERT_STREQ(get_tok_str(TOK_DEC, NULL), "--");
  UT_ASSERT_STREQ(get_tok_str(TOK_SHL, NULL), "<<");
  UT_ASSERT_STREQ(get_tok_str(TOK_SAR, NULL), ">>");
  UT_ASSERT_STREQ(get_tok_str(TOK_A_ADD, NULL), "+=");
  UT_ASSERT_STREQ(get_tok_str(TOK_A_SUB, NULL), "-=");
  UT_ASSERT_STREQ(get_tok_str(TOK_A_MUL, NULL), "*=");
  UT_ASSERT_STREQ(get_tok_str(TOK_A_DIV, NULL), "/=");
  UT_ASSERT_STREQ(get_tok_str(TOK_A_MOD, NULL), "%=");
  UT_ASSERT_STREQ(get_tok_str(TOK_A_AND, NULL), "&=");
  UT_ASSERT_STREQ(get_tok_str(TOK_A_XOR, NULL), "^=");
  UT_ASSERT_STREQ(get_tok_str(TOK_A_OR, NULL), "|=");
  UT_ASSERT_STREQ(get_tok_str(TOK_ARROW, NULL), "->");
  UT_ASSERT_STREQ(get_tok_str(TOK_TWODOTS, NULL), "..");
  UT_ASSERT_STREQ(get_tok_str(TOK_TWOSHARPS, NULL), "##");
  UT_ASSERT_STREQ(get_tok_str(TOK_A_SHL, NULL), "<<=");
  UT_ASSERT_STREQ(get_tok_str(TOK_A_SAR, NULL), ">>=");
  return 0;
}

UT_TEST(test_define_find_returns_null_for_undefined)
{
  TokenSym *ts = tok_alloc("undef_xxx", 9);
  UT_ASSERT(define_find(ts->tok) == NULL);
  return 0;
}

/* ============================================================================
 * next() / skip() / preprocess() smoke tests
 * ============================================================================ */

UT_TEST(test_next_lexes_identifier)
{
  UT_ASSERT(ut_open_input("foo") == 0);
  ut_reset_pp_state();
  next();
  int foo_tok = tok_alloc("foo", 3)->tok;
  UT_ASSERT_EQ(tok, foo_tok);
  file = NULL;
  return 0;
}

UT_TEST(test_next_lexes_number_with_tok_num)
{
  UT_ASSERT(ut_open_input("42") == 0);
  ut_reset_pp_state();
  parse_flags |= PARSE_FLAG_TOK_NUM;
  next();
  UT_ASSERT_EQ(tok, TOK_CINT);
  UT_ASSERT_EQ(tokc.i, 42);
  parse_flags = 0;
  file = NULL;
  return 0;
}

UT_TEST(test_next_lexes_string_with_tok_str)
{
  UT_ASSERT(ut_open_input("\"hello\"") == 0);
  ut_reset_pp_state();
  parse_flags |= PARSE_FLAG_TOK_STR;
  next();
  UT_ASSERT_EQ(tok, TOK_STR);
  UT_ASSERT_EQ(tokc.str.size, 6);
  UT_ASSERT_STREQ(tokc.str.data, "hello");
  parse_flags = 0;
  file = NULL;
  return 0;
}

UT_TEST(test_next_lexes_increment_operator)
{
  UT_ASSERT(ut_open_input("++") == 0);
  ut_reset_pp_state();
  next();
  UT_ASSERT_EQ(tok, TOK_INC);
  file = NULL;
  return 0;
}

UT_TEST(test_next_skips_c_comment)
{
  UT_ASSERT(ut_open_input("/* skipped */ x") == 0);
  ut_reset_pp_state();
  next();
  int x_tok = tok_alloc("x", 1)->tok;
  UT_ASSERT_EQ(tok, x_tok);
  file = NULL;
  return 0;
}

UT_TEST(test_skip_advances_when_token_matches)
{
  UT_ASSERT(ut_open_input("(") == 0);
  ut_reset_pp_state();
  next();
  UT_ASSERT_EQ(tok, '(');
  skip('(');
  UT_ASSERT_EQ(tok, TOK_EOF);
  file = NULL;
  return 0;
}

UT_TEST(test_skip_errors_when_token_mismatches)
{
  UT_ASSERT(ut_open_input("x") == 0);
  ut_reset_pp_state();
  next();
  tcc_state->error_set_jmp_enabled = 1;
  if (setjmp(tcc_state->error_jmp_buf) == 0)
  {
    skip('(');
    tcc_state->error_set_jmp_enabled = 0;
    file = NULL;
    return -1;
  }
  tcc_state->error_set_jmp_enabled = 0;
  file = NULL;
  return 0;
}

UT_TEST(test_preprocess_define_object_macro)
{
  Sym *boundary = define_stack;

  ut_reset_pp_state();
  UT_ASSERT(ut_open_input("#define X 123\n") == 0);
  preprocess(1);

  int xtok = tok_alloc("X", 1)->tok;
  Sym *s = define_find(xtok);
  UT_ASSERT(s != NULL);
  UT_ASSERT_EQ(s->v, xtok);

  const int *p = s->d;
  CValue cv;
  int t;
  tok_get(&t, &p, &cv);
  UT_ASSERT_EQ(t, TOK_PPNUM);
  UT_ASSERT_STREQ(cv.str.data, "123");

  free_defines(boundary);
  file = NULL;
  return 0;
}

UT_TEST(test_preprocess_undef_removes_macro)
{
  Sym *boundary = define_stack;
  int xtok = tok_alloc("Y", 1)->tok;
  define_push(xtok, MACRO_OBJ, NULL, NULL);
  UT_ASSERT(define_find(xtok) != NULL);

  ut_reset_pp_state();
  UT_ASSERT(ut_open_input("#undef Y\n") == 0);
  preprocess(1);

  UT_ASSERT(define_find(xtok) == NULL);
  free_defines(boundary);
  file = NULL;
  return 0;
}

/* ============================================================================
 * Additional preprocessor / lexer coverage
 * ============================================================================ */

UT_TEST(test_tok_str_free_str_null_is_safe)
{
  tok_str_free_str(NULL);
  return 0;
}

UT_TEST(test_begin_macro_static_buffer_end_macro_resets)
{
  static struct BufferedFile bf;
  memset(&bf, 0, sizeof(bf));
  bf.line_num = 1;
  file = &bf;

  TokenString str;
  tok_str_new(&str);
  tok_str_add(&str, TOK_IF);
  tok_str_add(&str, 0);

  int len_before = str.len;
  begin_macro(&str, 0);
  UT_ASSERT(macro_ptr == tok_str_buf(&str));
  end_macro();
  UT_ASSERT_EQ(str.len, 0);
  UT_ASSERT_EQ(str.need_spc, 0);

  file = NULL;
  (void)len_before;
  return 0;
}

UT_TEST(test_next_lexes_string_with_escapes)
{
  UT_ASSERT(ut_open_input("\"a\\nb\\\\c\"") == 0);
  ut_reset_pp_state();
  parse_flags |= PARSE_FLAG_TOK_STR;
  next();
  UT_ASSERT_EQ(tok, TOK_STR);
  UT_ASSERT_EQ(tokc.str.size, 6);
  UT_ASSERT_EQ(tokc.str.data[0], 'a');
  UT_ASSERT_EQ(tokc.str.data[1], '\n');
  UT_ASSERT_EQ(tokc.str.data[2], 'b');
  UT_ASSERT_EQ(tokc.str.data[3], '\\');
  UT_ASSERT_EQ(tokc.str.data[4], 'c');
  parse_flags = 0;
  file = NULL;
  return 0;
}

UT_TEST(test_next_lexes_hex_number)
{
  UT_ASSERT(ut_open_input("0xff") == 0);
  ut_reset_pp_state();
  parse_flags |= PARSE_FLAG_TOK_NUM;
  next();
  UT_ASSERT_EQ(tok, TOK_CINT);
  UT_ASSERT_EQ(tokc.i, 255);
  parse_flags = 0;
  file = NULL;
  return 0;
}

UT_TEST(test_next_with_macro_substitution)
{
  Sym *boundary = define_stack;
  int xtok = tok_alloc("X", 1)->tok;
  TokenString str;
  tok_str_new(&str);
  CValue cv;
  cv.i = 1;
  tok_str_add2(&str, TOK_CINT, &cv);
  tok_str_add(&str, 0);
  int *body = tok_str_ensure_heap(&str);
  define_push(xtok, MACRO_OBJ, body, NULL);

  UT_ASSERT(ut_open_input("X") == 0);
  ut_reset_pp_state();
  parse_flags |= PARSE_FLAG_PREPROCESS;
  next();
  UT_ASSERT_EQ(tok, TOK_CINT);
  UT_ASSERT_EQ(tokc.i, 1);
  parse_flags = 0;

  while (macro_ptr)
    end_macro();
  free_defines(boundary);
  file = NULL;
  return 0;
}

UT_TEST(test_tccpp_putfile_relative_path)
{
  static struct BufferedFile bf;
  memset(&bf, 0, sizeof(bf));
  strcpy(bf.filename, "/tmp/original.c");
  bf.true_filename = bf.filename;
  file = &bf;

  tccpp_putfile("other.c");
  UT_ASSERT_STREQ(bf.filename, "/tmp/other.c");
  UT_ASSERT(bf.true_filename != bf.filename);
  tcc_free(bf.true_filename);
  file = NULL;
  return 0;
}

UT_TEST(test_tccpp_putfile_absolute_path)
{
  static struct BufferedFile bf;
  memset(&bf, 0, sizeof(bf));
  strcpy(bf.filename, "/tmp/original.c");
  bf.true_filename = bf.filename;
  file = &bf;

  tccpp_putfile("/other/path.c");
  UT_ASSERT_STREQ(bf.filename, "/other/path.c");
  file = NULL;
  return 0;
}

UT_TEST(test_preprocess_start_end_lifecycle)
{
  BufferedFile *orig_bf;
  BufferedFile *cmd_bf;

  tccpp_delete(tcc_state);
  tcc_open_bf(tcc_state, "test.c", 0);
  orig_bf = file;
  tcc_state->include_stack_ptr = tcc_state->include_stack;
  tcc_state->ifdef_stack_ptr = tcc_state->ifdef_stack;

  preprocess_start(tcc_state, AFF_TYPE_C);
  UT_ASSERT(file != NULL);
  UT_ASSERT(file != orig_bf);
  UT_ASSERT_EQ(tcc_state->pack_stack_ptr, tcc_state->pack_stack);
  UT_ASSERT_EQ(parse_flags, 0);
  UT_ASSERT(tcc_state->include_stack_ptr == tcc_state->include_stack + 1);

  cmd_bf = file;
  file = NULL;
  tcc_free(cmd_bf);
  tcc_free(orig_bf);
  preprocess_end(tcc_state);
  tccpp_new(tcc_state);
  return 0;
}

UT_TEST(test_preprocess_start_asm_file)
{
  BufferedFile *orig_bf;

  tccpp_delete(tcc_state);
  tcc_open_bf(tcc_state, "test.S", 0);
  orig_bf = file;
  preprocess_start(tcc_state, AFF_TYPE_ASM);
  UT_ASSERT(file == orig_bf);
  UT_ASSERT(parse_flags & PARSE_FLAG_ASM_FILE);
  UT_ASSERT_EQ(set_idnum('.', 0), IS_ID);

  file = NULL;
  tcc_free(orig_bf);
  preprocess_end(tcc_state);
  tccpp_new(tcc_state);
  return 0;
}

UT_TEST(test_tcc_preprocess_simple)
{
  FILE *fp = tmpfile();
  Sym *boundary = define_stack;
  int xtok = tok_alloc("X", 1)->tok;
  TokenString str;
  BufferedFile *bf;
  CValue cv;
  char buf[256];
  size_t n;

  UT_ASSERT(fp != NULL);
  tcc_state->ppfp = fp;
  tcc_state->Pflag = LINE_MACRO_OUTPUT_FORMAT_NONE;

  tok_str_new(&str);
  cv.i = 1;
  tok_str_add2(&str, TOK_CINT, &cv);
  tok_str_add(&str, 0);
  define_push(xtok, MACRO_OBJ, tok_str_ensure_heap(&str), NULL);

  tcc_open_bf(tcc_state, "test.c", 2);
  memcpy(file->buffer, "X\n", 2);

  UT_ASSERT_EQ(tcc_preprocess(tcc_state), 0);

  rewind(fp);
  n = fread(buf, 1, sizeof(buf) - 1, fp);
  buf[n] = '\0';
  UT_ASSERT(strstr(buf, "1") != NULL);

  fclose(fp);
  tcc_state->ppfp = NULL;
  bf = file;
  file = NULL;
  tcc_free(bf);
  free_defines(boundary);
  return 0;
}

UT_TEST(test_parse_define_function_macro_direct)
{
  Sym *boundary = define_stack;
  int addtok = tok_alloc("add", 3)->tok;

  ut_reset_pp_state();
  UT_ASSERT(ut_open_input("(x,y) (x+y)\n") == 0);
  tok = addtok;
  parse_define();

  Sym *s = define_find(addtok);
  UT_ASSERT(s != NULL);
  UT_ASSERT(s->type.t & MACRO_FUNC);
  UT_ASSERT(s->next != NULL);

  free_defines(boundary);
  file = NULL;
  return 0;
}

UT_TEST(test_parse_define_variadic_direct)
{
  Sym *boundary = define_stack;
  int logtok = tok_alloc("logfn", 5)->tok;

  ut_reset_pp_state();
  UT_ASSERT(ut_open_input("(...) __VA_ARGS__\n") == 0);
  tok = logtok;
  parse_define();

  Sym *s = define_find(logtok);
  UT_ASSERT(s != NULL);
  UT_ASSERT(s->type.t & MACRO_FUNC);

  free_defines(boundary);
  file = NULL;
  return 0;
}

UT_TEST(test_pp_error_dumps_macro_context)
{
  TokenString str;
  CString cs;

  UT_ASSERT(ut_open_input("") == 0);

  tok_str_new(&str);
  tok_str_add(&str, TOK_IF);
  tok_str_add(&str, 0);
  begin_macro(&str, 0);

  cstr_new(&cs);
  pp_expr = TOK_IF;
  pp_error(&cs);

  UT_ASSERT(strstr(cs.data, "bad preprocessor expression") != NULL);
  UT_ASSERT(strstr(cs.data, "if") != NULL);

  cstr_free(&cs);
  file = NULL;
  return 0;
}

UT_TEST(test_define_undef_unmaterialized_builtin)
{
  Sym *s = tcc_mallocz(sizeof(Sym));
  /* Use a builtin token that is unlikely to have been materialized yet. */
  s->v = TOK___HAS_INCLUDE;
  define_undef(s);
  tcc_free(s);
  return 0;
}

UT_TEST(test_preprocess_ifdef_endif)
{
  Sym *boundary = define_stack;
  ut_reset_pp_state();
  UT_ASSERT(ut_open_input("#ifdef UNDEF\nx\n#endif\n") == 0);
  preprocess(1);
  UT_ASSERT_EQ(tok, TOK_LINEFEED);
  free_defines(boundary);
  file = NULL;
  return 0;
}

UT_TEST(test_preprocess_ifdef_defined)
{
  Sym *boundary = define_stack;
  int xtok = tok_alloc("X", 1)->tok;
  define_push(xtok, MACRO_OBJ, NULL, NULL);

  ut_reset_pp_state();
  UT_ASSERT(ut_open_input("#ifdef X\ny\n#endif\n") == 0);
  preprocess(1);
  UT_ASSERT_EQ(tok, TOK_LINEFEED);

  free_defines(boundary);
  file = NULL;
  return 0;
}

UT_TEST(test_preprocess_ifndef_bof)
{
  ut_reset_pp_state();
  UT_ASSERT(ut_open_input("#ifndef FOO\n#endif\n") == 0);
  preprocess(1);
  UT_ASSERT_EQ(tok, TOK_LINEFEED);
  UT_ASSERT_EQ(file->ifndef_macro, 0);
  file = NULL;
  return 0;
}

UT_TEST(test_preprocess_if_elif_else)
{
  ut_reset_pp_state();
  UT_ASSERT(ut_open_input("#if 1\na\n#elif 1\nb\n#else\nc\n#endif\n") == 0);
  preprocess(1);
  UT_ASSERT_EQ(tok, TOK_LINEFEED);
  file = NULL;
  return 0;
}

UT_TEST(test_preprocess_defined_operator)
{
  ut_reset_pp_state();
  UT_ASSERT(ut_open_input("#if defined(UNKNOWN)\n#endif\n") == 0);
  preprocess(1);
  UT_ASSERT_EQ(tok, TOK_LINEFEED);
  file = NULL;
  return 0;
}

UT_TEST(test_preprocess_line_directive)
{
  ut_reset_pp_state();
  UT_ASSERT(ut_open_input("#line 42 \"new.c\"\n") == 0);
  file->line_num = 10;
  file->line_ref = 10;
  strcpy(file->filename, "orig.c");
  file->true_filename = file->filename;
  preprocess(1);
  /* #line N means the *following* line is line N, so the directive itself
     is recorded as line N-1. */
  UT_ASSERT_EQ(file->line_num, 41);
  UT_ASSERT_STREQ(file->filename, "new.c");
  file = NULL;
  return 0;
}

UT_TEST(test_preprocess_warning)
{
  ut_reset_pp_state();
  UT_ASSERT(ut_open_input("#warning hello\n") == 0);
  preprocess(1);
  UT_ASSERT_EQ(tok, TOK_LINEFEED);
  file = NULL;
  return 0;
}

UT_TEST(test_preprocess_error)
{
  ut_reset_pp_state();
  UT_ASSERT(ut_open_input("#error boom\n") == 0);
  tcc_state->error_set_jmp_enabled = 1;
  if (setjmp(tcc_state->error_jmp_buf) == 0)
  {
    preprocess(1);
    tcc_state->error_set_jmp_enabled = 0;
    file = NULL;
    return -1;
  }
  tcc_state->error_set_jmp_enabled = 0;
  file = NULL;
  return 0;
}

UT_TEST(test_preprocess_include_errors)
{
  ut_reset_pp_state();
  UT_ASSERT(ut_open_input("#include \"missing.h\"\n") == 0);
  tcc_state->error_set_jmp_enabled = 1;
  if (setjmp(tcc_state->error_jmp_buf) == 0)
  {
    preprocess(1);
    tcc_state->error_set_jmp_enabled = 0;
    file = NULL;
    return -1;
  }
  tcc_state->error_set_jmp_enabled = 0;
  file = NULL;
  return 0;
}

UT_TEST(test_preprocess_pragma_pack)
{
  tcc_state->pack_stack_ptr = tcc_state->pack_stack;
  ut_reset_pp_state();
  UT_ASSERT(ut_open_input("#pragma pack(2)\n") == 0);
  preprocess(1);
  UT_ASSERT_EQ(*tcc_state->pack_stack_ptr, 2);
  file = NULL;
  return 0;
}

UT_TEST(test_preprocess_pragma_once)
{
  ut_reset_pp_state();
  UT_ASSERT(ut_open_input("#pragma once\n") == 0);
  strcpy(file->filename, "test.h");
  file->true_filename = file->filename;
  preprocess(1);
  UT_ASSERT_EQ(tok, TOK_LINEFEED);
  file = NULL;
  return 0;
}

UT_TEST(test_preprocess_unknown_pragma)
{
  ut_reset_pp_state();
  UT_ASSERT(ut_open_input("#pragma unknown_stuff\n") == 0);
  preprocess(1);
  UT_ASSERT_EQ(tok, TOK_LINEFEED);
  file = NULL;
  return 0;
}

UT_TEST(test_preprocess_pragma_push_pop_macro)
{
  Sym *boundary = define_stack;
  int xtok = tok_alloc("X", 1)->tok;
  define_push(xtok, MACRO_OBJ, NULL, NULL);

  ut_reset_pp_state();
  UT_ASSERT(ut_open_input("#pragma push_macro(\"X\")\n#pragma pop_macro(\"X\")\n") == 0);
  preprocess(1);
  UT_ASSERT_EQ(tok, TOK_LINEFEED);
  preprocess(1);
  UT_ASSERT_EQ(tok, TOK_LINEFEED);

  free_defines(boundary);
  file = NULL;
  return 0;
}

/* ============================================================================
 * C11 6.10.9 _Pragma operator (handled in next(), delegating to pragma_parse)
 * ============================================================================ */

/* The lexer must intern `_Pragma' to the dedicated TOK__Pragma id so next()
   can treat it as a preprocessor operator rather than a plain identifier. */
UT_TEST(test_pragma_operator_token_recognized)
{
  UT_ASSERT_EQ(tok_alloc("_Pragma", 7)->tok, TOK__Pragma);
  UT_ASSERT_STREQ(get_tok_str(TOK__Pragma, NULL), "_Pragma");
  return 0;
}

/* A literal `_Pragma("pack(N)")' must be consumed by next() and take effect
   exactly like `#pragma pack(N)', leaving the following token (`x') intact. */
UT_TEST(test_pragma_operator_applies_pack_literal)
{
  int saved_output = tcc_state->output_type;
  int xtok = tok_alloc("x", 1)->tok;

  tcc_state->output_type = TCC_OUTPUT_OBJ; /* real compile, not -E */
  tcc_state->pack_stack_ptr = tcc_state->pack_stack;
  *tcc_state->pack_stack_ptr = 0;
  ut_reset_pp_state();
  UT_ASSERT(ut_open_input("_Pragma(\"pack(2)\") x") == 0);
  parse_flags |= PARSE_FLAG_PREPROCESS;
  next();
  UT_ASSERT_EQ(*tcc_state->pack_stack_ptr, 2); /* pack applied */
  UT_ASSERT_EQ(tok, xtok);                     /* operator disappeared */

  parse_flags = 0;
  tcc_state->pack_stack_ptr = tcc_state->pack_stack;
  *tcc_state->pack_stack_ptr = 0;
  tcc_state->output_type = saved_output;
  file = NULL;
  return 0;
}

/* The whole point of the operator: `_Pragma' produced by macro expansion (the
   DO_PRAGMA idiom).  Recognition lives in next(), *after* substitution, so a
   macro whose body is `_Pragma ( "pack(2)" )' must still apply the pack. */
UT_TEST(test_pragma_operator_applies_pack_from_macro)
{
  Sym *boundary = define_stack;
  int saved_output = tcc_state->output_type;
  int mtok = tok_alloc("DOPACK", 6)->tok;
  int ytok = tok_alloc("y", 1)->tok;
  TokenString body;
  CValue cv;

  /* macro body:  _Pragma ( "pack(2)" ) */
  tok_str_new(&body);
  tok_str_add(&body, TOK__Pragma);
  tok_str_add(&body, '(');
  cv.str.data = "pack(2)";
  cv.str.size = 8; /* 7 chars + NUL */
  tok_str_add2(&body, TOK_STR, &cv);
  tok_str_add(&body, ')');
  tok_str_add(&body, 0);
  define_push(mtok, MACRO_OBJ, tok_str_ensure_heap(&body), NULL);

  tcc_state->output_type = TCC_OUTPUT_OBJ;
  tcc_state->pack_stack_ptr = tcc_state->pack_stack;
  *tcc_state->pack_stack_ptr = 0;
  ut_reset_pp_state();
  UT_ASSERT(ut_open_input("DOPACK y") == 0);
  parse_flags |= PARSE_FLAG_PREPROCESS;
  next();
  UT_ASSERT_EQ(*tcc_state->pack_stack_ptr, 2); /* pack applied */
  UT_ASSERT_EQ(tok, ytok);                     /* token after the macro */

  parse_flags = 0;
  tcc_state->pack_stack_ptr = tcc_state->pack_stack;
  *tcc_state->pack_stack_ptr = 0;
  tcc_state->output_type = saved_output;
  free_defines(boundary);
  file = NULL;
  return 0;
}

/* Under -E the operator is rewritten to a `#pragma ...' line, and the
   destringized `\"' turns back into `"'.  Exercises the tcc_preprocess()
   output path end-to-end. */
UT_TEST(test_pragma_operator_rewrites_under_dash_E)
{
  static const char src[] = "_Pragma(\"message \\\"x\\\"\")\n";
  FILE *fp = tmpfile();
  BufferedFile *bf;
  int saved_output = tcc_state->output_type;
  int len = (int)sizeof(src) - 1;
  char buf[256];
  size_t n;

  UT_ASSERT(fp != NULL);
  tcc_state->ppfp = fp;
  tcc_state->Pflag = LINE_MACRO_OUTPUT_FORMAT_NONE;
  tcc_state->output_type = TCC_OUTPUT_PREPROCESS;

  tcc_open_bf(tcc_state, "test.c", len);
  memcpy(file->buffer, src, len);

  UT_ASSERT_EQ(tcc_preprocess(tcc_state), 0);

  rewind(fp);
  n = fread(buf, 1, sizeof(buf) - 1, fp);
  buf[n] = '\0';
  UT_ASSERT(strstr(buf, "#pragma message \"x\"") != NULL);

  fclose(fp);
  tcc_state->ppfp = NULL;
  tcc_state->output_type = saved_output;
  bf = file;
  file = NULL;
  tcc_free(bf);
  return 0;
}

/* A non-string operand must raise a hard error rather than be mis-parsed. */
UT_TEST(test_pragma_operator_rejects_non_string_operand)
{
  int saved_output = tcc_state->output_type;
  int result;

  tcc_state->output_type = TCC_OUTPUT_OBJ;
  ut_reset_pp_state();
  UT_ASSERT(ut_open_input("_Pragma(123)") == 0);
  parse_flags |= PARSE_FLAG_PREPROCESS;
  tcc_state->error_set_jmp_enabled = 1;
  if (setjmp(tcc_state->error_jmp_buf) == 0)
  {
    next();
    result = -1; /* expected a longjmp out of next() */
  }
  else
  {
    result = 0; /* error correctly raised */
  }
  tcc_state->error_set_jmp_enabled = 0;
  parse_flags = 0;
  tcc_state->output_type = saved_output;
  file = NULL;
  return result;
}
