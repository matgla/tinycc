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
/* asm_opcode/tcc_asm_set_fpu are the two calls tccasm.c makes OUT to the ARM
 * assembler backend (arm-thumb-asm.c), which is also linked into this binary.
 * Redirect them to local stubs so the real backend entry points stay
 * unreferenced (and get GC-sectioned away) — otherwise they drag the whole
 * Thumb instruction encoder in, whose own asm_expr dependency is unavailable
 * here (it is renamed to tccasm_ut_asm_expr above). */
#define asm_opcode tccasm_ut_asm_opcode
#define tcc_asm_set_fpu tccasm_ut_tcc_asm_set_fpu
#include "tccasm.c"
#undef asm_expr
#undef asm_global_instr
#undef asm_instr
#undef asm_int_expr
#undef find_constraint
#undef get_asm_sym
#undef tcc_asm_emit_inline
#undef tcc_assemble
#undef asm_opcode
#undef tcc_asm_set_fpu

#include <string.h>

#include "ut.h"

/* Test-harness hooks implemented in stubs.c. */
void utb_set_tok_str(int tok, const char *name);

/* ---------------------------------------------------------------- stubs */
/* The main UT binary does not link tccgen.c/tccpp.c, so define the few
 * production helpers the assembler expression / symbol helpers need. */

/* Token globals consumed by asm_expr_* / asm_int_expr. */
int tok;
CValue tokc;

/* Parser globals reached by the asm directive parser. */
int parse_flags;
const int *macro_ptr;

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
 * ut_set_next_token() or the multi-token helpers below; without that the
 * stream ends immediately. */
#define UTB_TOKEN_Q_SIZE 16

typedef struct
{
  int tok;
  CValue val;
  int has_val;
} UtbQueuedToken;

static UtbQueuedToken utb_token_q[UTB_TOKEN_Q_SIZE];
static int utb_token_q_len = 0;
static int utb_token_q_pos = 0;

static void utb_clear_token_q(void)
{
  utb_token_q_len = 0;
  utb_token_q_pos = 0;
}

static void utb_queue_token(int t)
{
  if (utb_token_q_len < UTB_TOKEN_Q_SIZE)
  {
    UtbQueuedToken *slot = &utb_token_q[utb_token_q_len++];
    slot->tok = t;
    slot->has_val = 0;
  }
}

static void utb_queue_ppnum(const char *s)
{
  if (utb_token_q_len < UTB_TOKEN_Q_SIZE)
  {
    UtbQueuedToken *slot = &utb_token_q[utb_token_q_len++];
    slot->tok = TOK_PPNUM;
    slot->val.str.data = (char *)s;
    slot->val.str.size = (int)strlen(s) + 1;
    slot->has_val = 1;
  }
}

static void utb_queue_str(const char *s)
{
  if (utb_token_q_len < UTB_TOKEN_Q_SIZE)
  {
    UtbQueuedToken *slot = &utb_token_q[utb_token_q_len++];
    slot->tok = TOK_STR;
    slot->val.str.data = (char *)s;
    slot->val.str.size = (int)strlen(s) + 1;
    slot->has_val = 1;
  }
}

static void utb_queue_ppstr(const char *s)
{
  if (utb_token_q_len < UTB_TOKEN_Q_SIZE)
  {
    UtbQueuedToken *slot = &utb_token_q[utb_token_q_len++];
    slot->tok = TOK_PPSTR;
    slot->val.str.data = (char *)s;
    slot->val.str.size = (int)strlen(s) + 1;
    slot->has_val = 1;
  }
}

static void ut_set_next_token(int t)
{
  utb_clear_token_q();
  utb_queue_token(t);
}

void next(void)
{
  if (utb_token_q_pos < utb_token_q_len)
  {
    const UtbQueuedToken *slot = &utb_token_q[utb_token_q_pos++];
    tok = slot->tok;
    if (slot->has_val)
      tokc = slot->val;
  }
  else
  {
    tok = TOK_EOF;
  }
}

/* tccpp.c: consume an expected token.  Tests never feed mismatched input. */
void skip(int c)
{
  (void)c;
  next();
}

/* Stubs for tccasm.c directive code paths that pull in symbols from modules
 * not linked into the main unit-test binary.
 *
 * pstrcpy/pstrcat are NOT defined here: the main UT binary already links
 * test_ld_script.c (pstrcpy) and test_tccdbg.c (pstrcat), so a local copy
 * would be a multiple-definition clash. */
void update_storage(Sym *sym) { (void)sym; }
void tccpp_putfile(const char *filename) { (void)filename; }
void arm_init(TCCState *s1) { (void)s1; }
void tcc_debug_start(TCCState *s1) { (void)s1; }
void tcc_debug_line(TCCState *s1) { (void)s1; }
void tcc_debug_end(TCCState *s1) { (void)s1; }

/* tccpp.c: drop the rest of a logical line.  The real version walks the
 * preprocessor's file buffer; here the token stream is the queue, so mirror
 * only the postcondition the directive parser relies on (tok == TOK_LINEFEED). */
void skip_to_eol(int warn)
{
  (void)warn;
  tok = TOK_LINEFEED;
}

/* Local stand-ins for tccasm.c's two calls out to the ARM assembler backend
 * (redirected via the #define block above).  The directive tests never emit an
 * instruction, and the .fpu test only needs parsing to survive. */
void tccasm_ut_asm_opcode(TCCState *s1, int opcode)
{
  (void)s1;
  (void)opcode;
}

void tccasm_ut_tcc_asm_set_fpu(const char *name) { (void)name; }

int tcc_gen_machine_dry_run_is_active(void) { return 0; }

TokenString *tok_str_alloc(void)
{
  TokenString *s = tcc_mallocz(sizeof(TokenString));
  return s;
}

static void ut_tok_str_grow(TokenString *s, int need)
{
  if (s->allocated_len == 0)
  {
    int cap = TOKSTR_SMALL_BUFSIZE;
    int *p;
    if (cap < need)
      cap = need;
    p = tcc_malloc((unsigned long)cap * sizeof(int));
    memcpy(p, s->data.small_buf, (size_t)s->len * sizeof(int));
    s->allocated_len = cap;
    s->data.str = p;
  }
  else if (s->len + need > s->allocated_len)
  {
    int cap = s->allocated_len * 2;
    while (cap < s->len + need)
      cap *= 2;
    s->data.str = tcc_realloc(s->data.str, (unsigned long)cap * sizeof(int));
    s->allocated_len = cap;
  }
}

void tok_str_add(TokenString *s, int t)
{
  ut_tok_str_grow(s, 1);
  tok_str_buf(s)[s->len++] = t;
}

void tok_str_add_tok(TokenString *s)
{
  ut_tok_str_grow(s, 2);
  tok_str_buf(s)[s->len++] = tok;
  if (tok >= TOK_CCHAR && tok <= TOK_LINENUM)
  {
    tok_str_add(s, tokc.i);
  }
  else if (tok == TOK_STR || tok == TOK_LSTR || tok == TOK_PPNUM || tok == TOK_PPSTR)
  {
    int size = tokc.str.size;
    int nb = 1 + (size + (int)sizeof(int) - 1) / (int)sizeof(int);
    tok_str_add(s, size);
    ut_tok_str_grow(s, nb - 1);
    memcpy(&tok_str_buf(s)[s->len], tokc.str.data, (size_t)size);
    s->len += nb - 1;
  }
}

void begin_macro(TokenString *str, int alloc) { (void)str; (void)alloc; }
void end_macro(void) {}

/* Helper for directive tests: initialise an in-memory section. */
static void ut_setup_section(Section *sec, unsigned char *buf, size_t size)
{
  memset(sec, 0, sizeof(*sec));
  sec->data = buf;
  sec->data_allocated = size;
  sec->sh_type = SHT_PROGBITS;
  sec->sh_addralign = 1;
}

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

UT_TEST(test_find_constraint_named_reference_not_found)
{
  ASMOperand operands[3] = {0};
  const char *tail = NULL;

  UT_ASSERT_EQ(tccasm_ut_find_constraint(operands, 3, "[missing]", &tail), -1);
  UT_ASSERT(tail != NULL);
  UT_ASSERT(strcmp(tail, "") == 0);
  return 0;
}

UT_TEST(test_find_constraint_invalid_start_returns_minus_one)
{
  ASMOperand operands[3] = {0};

  UT_ASSERT_EQ(tccasm_ut_find_constraint(operands, 3, "x", NULL), -1);
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

UT_TEST(test_asm_expr_prod_multiplies_and_divides)
{
  ExprValue e;

  tok = TOK_PPNUM;
  tokc.str.data = "6";
  tokc.str.size = 2;
  utb_clear_token_q();
  utb_queue_token('*');
  utb_queue_ppnum("7");
  asm_expr_prod(tcc_state, &e);
  UT_ASSERT_EQ(e.v, 42);
  UT_ASSERT(e.sym == NULL);

  tok = TOK_PPNUM;
  tokc.str.data = "20";
  tokc.str.size = 3;
  utb_clear_token_q();
  utb_queue_token('/');
  utb_queue_ppnum("4");
  asm_expr_prod(tcc_state, &e);
  UT_ASSERT_EQ(e.v, 5);
  UT_ASSERT(e.sym == NULL);
  return 0;
}

UT_TEST(test_asm_expr_prod_modulo_and_shifts)
{
  ExprValue e;

  tok = TOK_PPNUM;
  tokc.str.data = "17";
  tokc.str.size = 3;
  utb_clear_token_q();
  utb_queue_token('%');
  utb_queue_ppnum("5");
  asm_expr_prod(tcc_state, &e);
  UT_ASSERT_EQ(e.v, 2);
  UT_ASSERT(e.sym == NULL);

  tok = TOK_PPNUM;
  tokc.str.data = "3";
  tokc.str.size = 2;
  utb_clear_token_q();
  utb_queue_token(TOK_SHL);
  utb_queue_ppnum("4");
  asm_expr_prod(tcc_state, &e);
  UT_ASSERT_EQ(e.v, 48);
  UT_ASSERT(e.sym == NULL);

  tok = TOK_PPNUM;
  tokc.str.data = "64";
  tokc.str.size = 3;
  utb_clear_token_q();
  utb_queue_token(TOK_SAR);
  utb_queue_ppnum("3");
  asm_expr_prod(tcc_state, &e);
  UT_ASSERT_EQ(e.v, 8);
  UT_ASSERT(e.sym == NULL);
  return 0;
}

UT_TEST(test_asm_expr_logic_bitwise_ops)
{
  ExprValue e;

  tok = TOK_PPNUM;
  tokc.str.data = "0x0f";
  tokc.str.size = 5;
  utb_clear_token_q();
  utb_queue_token('&');
  utb_queue_ppnum("0xf0");
  asm_expr_logic(tcc_state, &e);
  UT_ASSERT_EQ(e.v, 0);

  tok = TOK_PPNUM;
  tokc.str.data = "0x0f";
  tokc.str.size = 5;
  utb_clear_token_q();
  utb_queue_token('|');
  utb_queue_ppnum("0xf0");
  asm_expr_logic(tcc_state, &e);
  UT_ASSERT_EQ(e.v, 0xff);

  tok = TOK_PPNUM;
  tokc.str.data = "0xff";
  tokc.str.size = 5;
  utb_clear_token_q();
  utb_queue_token('^');
  utb_queue_ppnum("0x0f");
  asm_expr_logic(tcc_state, &e);
  UT_ASSERT_EQ(e.v, 0xf0);
  return 0;
}

UT_TEST(test_asm_expr_sum_adds_and_subtracts_constants)
{
  ExprValue e;

  tok = TOK_PPNUM;
  tokc.str.data = "10";
  tokc.str.size = 3;
  utb_clear_token_q();
  utb_queue_token('+');
  utb_queue_ppnum("32");
  asm_expr_sum(tcc_state, &e);
  UT_ASSERT_EQ(e.v, 42);
  UT_ASSERT(e.sym == NULL);

  tok = TOK_PPNUM;
  tokc.str.data = "100";
  tokc.str.size = 4;
  utb_clear_token_q();
  utb_queue_token('-');
  utb_queue_ppnum("58");
  asm_expr_sum(tcc_state, &e);
  UT_ASSERT_EQ(e.v, 42);
  UT_ASSERT(e.sym == NULL);
  return 0;
}

UT_TEST(test_asm_expr_sum_adds_constant_to_symbol)
{
  int sym_tok = token_for_name("offset");
  ExprValue e;
  tcc_state->leading_underscore = 0;

  tok = sym_tok;
  utb_clear_token_q();
  utb_queue_token('+');
  utb_queue_ppnum("7");
  asm_expr_sum(tcc_state, &e);

  UT_ASSERT_EQ(e.v, 7);
  UT_ASSERT(e.sym != NULL);

  global_stack = e.sym->prev;
  tcc_free(e.sym);
  return 0;
}

UT_TEST(test_asm_expr_cmp_relational_operators)
{
  ExprValue e;

  tok = TOK_PPNUM;
  tokc.str.data = "2";
  tokc.str.size = 2;
  utb_clear_token_q();
  utb_queue_token(TOK_EQ);
  utb_queue_ppnum("2");
  asm_expr_cmp(tcc_state, &e);
  UT_ASSERT_EQ(e.v, -1);

  tok = TOK_PPNUM;
  tokc.str.data = "2";
  tokc.str.size = 2;
  utb_clear_token_q();
  utb_queue_token(TOK_NE);
  utb_queue_ppnum("3");
  asm_expr_cmp(tcc_state, &e);
  UT_ASSERT_EQ(e.v, -1);

  tok = TOK_PPNUM;
  tokc.str.data = "2";
  tokc.str.size = 2;
  utb_clear_token_q();
  utb_queue_token(TOK_LT);
  utb_queue_ppnum("3");
  asm_expr_cmp(tcc_state, &e);
  UT_ASSERT_EQ(e.v, -1);

  tok = TOK_PPNUM;
  tokc.str.data = "3";
  tokc.str.size = 2;
  utb_clear_token_q();
  utb_queue_token(TOK_GE);
  utb_queue_ppnum("2");
  asm_expr_cmp(tcc_state, &e);
  UT_ASSERT_EQ(e.v, -1);

  tok = TOK_PPNUM;
  tokc.str.data = "2";
  tokc.str.size = 2;
  utb_clear_token_q();
  utb_queue_token(TOK_LE);
  utb_queue_ppnum("2");
  asm_expr_cmp(tcc_state, &e);
  UT_ASSERT_EQ(e.v, -1);

  tok = TOK_PPNUM;
  tokc.str.data = "3";
  tokc.str.size = 2;
  utb_clear_token_q();
  utb_queue_token(TOK_GT);
  utb_queue_ppnum("2");
  asm_expr_cmp(tcc_state, &e);
  UT_ASSERT_EQ(e.v, -1);

  tok = TOK_PPNUM;
  tokc.str.data = "2";
  tokc.str.size = 2;
  utb_clear_token_q();
  utb_queue_token(TOK_EQ);
  utb_queue_ppnum("3");
  asm_expr_cmp(tcc_state, &e);
  UT_ASSERT_EQ(e.v, 0);
  return 0;
}

UT_TEST(test_asm_expr_top_level_evaluates_comparison)
{
  ExprValue e;

  tok = TOK_PPNUM;
  tokc.str.data = "5";
  tokc.str.size = 2;
  utb_clear_token_q();
  utb_queue_token('+');
  utb_queue_ppnum("3");
  utb_queue_token(TOK_EQ);
  utb_queue_ppnum("8");
  tccasm_ut_asm_expr(tcc_state, &e);

  UT_ASSERT_EQ(e.v, -1);
  UT_ASSERT(e.sym == NULL);
  return 0;
}

UT_TEST(test_asm_int_expr_evaluates_arithmetic_expr)
{
  tok = TOK_PPNUM;
  tokc.str.data = "4";
  tokc.str.size = 2;
  utb_clear_token_q();
  utb_queue_token('*');
  utb_queue_ppnum("5");

  UT_ASSERT_EQ(tccasm_ut_asm_int_expr(tcc_state), 20);
  return 0;
}

UT_TEST(test_asm_expr_unary_parses_parenthesized_expr)
{
  ExprValue e;

  tok = '(';
  utb_clear_token_q();
  utb_queue_ppnum("42");
  utb_queue_token(')');
  asm_expr_unary(tcc_state, &e);

  UT_ASSERT_EQ(e.v, 42);
  UT_ASSERT(e.sym == NULL);
  return 0;
}

UT_TEST(test_asm_expr_unary_parses_local_label_forward_ref)
{
  ExprValue e;
  tcc_state->leading_underscore = 0;

  tok = TOK_PPNUM;
  tokc.str.data = "1f";
  tokc.str.size = 3;
  utb_clear_token_q();
  asm_expr_unary(tcc_state, &e);

  UT_ASSERT_EQ(e.v, 0);
  UT_ASSERT(e.sym != NULL);

  global_stack = e.sym->prev;
  tcc_free(e.sym);
  return 0;
}

/* ------------------------------------------------------------------ directives */

UT_TEST(test_asm_parse_directive_align_and_fill_value)
{
  Section sec;
  unsigned char buf[64];

  ut_setup_section(&sec, buf, sizeof(buf));
  cur_text_section = &sec;
  ind = 1;

  tok = TOK_ASMDIR_align;
  utb_clear_token_q();
  utb_queue_ppnum("4");
  utb_queue_token(',');
  utb_queue_ppnum("0xAB");
  asm_parse_directive(tcc_state, 1);

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(sec.data_offset, 4);
  UT_ASSERT(sec.sh_addralign >= 4);
  UT_ASSERT_EQ(buf[1], 0xAB);
  UT_ASSERT_EQ(buf[2], 0xAB);
  UT_ASSERT_EQ(buf[3], 0xAB);
  return 0;
}

UT_TEST(test_asm_parse_directive_p2align_and_skip)
{
  Section sec;
  unsigned char buf[64];

  ut_setup_section(&sec, buf, sizeof(buf));
  cur_text_section = &sec;
  ind = 0;

  tok = TOK_ASMDIR_skip;
  utb_clear_token_q();
  utb_queue_ppnum("3");
  asm_parse_directive(tcc_state, 1);
  UT_ASSERT_EQ(ind, 3);
  UT_ASSERT_EQ(sec.data_offset, 3);

  tok = TOK_ASMDIR_p2align;
  utb_clear_token_q();
  utb_queue_ppnum("2");
  asm_parse_directive(tcc_state, 1);
  UT_ASSERT_EQ(ind, 4);
  return 0;
}

UT_TEST(test_asm_parse_directive_byte_word_long)
{
  Section sec;
  unsigned char buf[64];

  ut_setup_section(&sec, buf, sizeof(buf));
  cur_text_section = &sec;
  ind = 0;

  tok = TOK_ASMDIR_byte;
  utb_clear_token_q();
  utb_queue_ppnum("1");
  utb_queue_token(',');
  utb_queue_ppnum("2");
  asm_parse_directive(tcc_state, 1);
  UT_ASSERT_EQ(buf[0], 1);
  UT_ASSERT_EQ(buf[1], 2);
  UT_ASSERT_EQ(ind, 2);

  tok = TOK_ASMDIR_word;
  utb_clear_token_q();
  utb_queue_ppnum("0x1234");
  asm_parse_directive(tcc_state, 1);
  UT_ASSERT_EQ(buf[2], 0x34);
  UT_ASSERT_EQ(buf[3], 0x12);
  UT_ASSERT_EQ(ind, 4);

  tok = TOK_ASMDIR_long;
  utb_clear_token_q();
  utb_queue_ppnum("0xdeadbeef");
  asm_parse_directive(tcc_state, 1);
  UT_ASSERT_EQ(buf[4], 0xef);
  UT_ASSERT_EQ(buf[5], 0xbe);
  UT_ASSERT_EQ(buf[6], 0xad);
  UT_ASSERT_EQ(buf[7], 0xde);
  UT_ASSERT_EQ(ind, 8);
  return 0;
}

UT_TEST(test_asm_parse_directive_quad)
{
  Section sec;
  unsigned char buf[64];

  ut_setup_section(&sec, buf, sizeof(buf));
  cur_text_section = &sec;
  ind = 0;

  tok = TOK_ASMDIR_quad;
  utb_clear_token_q();
  utb_queue_ppnum("0x1122334455667788");
  asm_parse_directive(tcc_state, 1);

  UT_ASSERT_EQ(ind, 8);
  UT_ASSERT_EQ(buf[0], 0x88);
  UT_ASSERT_EQ(buf[1], 0x77);
  UT_ASSERT_EQ(buf[2], 0x66);
  UT_ASSERT_EQ(buf[3], 0x55);
  UT_ASSERT_EQ(buf[4], 0x44);
  UT_ASSERT_EQ(buf[5], 0x33);
  UT_ASSERT_EQ(buf[6], 0x22);
  UT_ASSERT_EQ(buf[7], 0x11);
  return 0;
}

UT_TEST(test_asm_parse_directive_fill)
{
  Section sec;
  unsigned char buf[64];

  ut_setup_section(&sec, buf, sizeof(buf));
  cur_text_section = &sec;
  ind = 0;

  tok = TOK_ASMDIR_fill;
  utb_clear_token_q();
  utb_queue_ppnum("2");
  utb_queue_token(',');
  utb_queue_ppnum("2");
  utb_queue_token(',');
  utb_queue_ppnum("0xAABB");
  asm_parse_directive(tcc_state, 1);

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(buf[0], 0xBB);
  UT_ASSERT_EQ(buf[1], 0xAA);
  UT_ASSERT_EQ(buf[2], 0xBB);
  UT_ASSERT_EQ(buf[3], 0xAA);
  return 0;
}

UT_TEST(test_asm_parse_directive_org)
{
  Section sec;
  unsigned char buf[64];

  ut_setup_section(&sec, buf, sizeof(buf));
  cur_text_section = &sec;
  ind = 0;

  tok = TOK_ASMDIR_org;
  utb_clear_token_q();
  utb_queue_ppnum("8");
  asm_parse_directive(tcc_state, 1);

  UT_ASSERT_EQ(ind, 8);
  UT_ASSERT_EQ(sec.data_offset, 8);
  return 0;
}

UT_TEST(test_asm_parse_directive_string_and_ascii)
{
  Section sec;
  unsigned char buf[64];

  ut_setup_section(&sec, buf, sizeof(buf));
  cur_text_section = &sec;
  ind = 0;

  tok = TOK_ASMDIR_string;
  utb_clear_token_q();
  utb_queue_str("abc");
  asm_parse_directive(tcc_state, 1);
  UT_ASSERT_EQ(buf[0], 'a');
  UT_ASSERT_EQ(buf[1], 'b');
  UT_ASSERT_EQ(buf[2], 'c');
  UT_ASSERT_EQ(buf[3], '\0');
  UT_ASSERT_EQ(ind, 4);

  tok = TOK_ASMDIR_ascii;
  utb_clear_token_q();
  utb_queue_str("xy");
  asm_parse_directive(tcc_state, 1);
  UT_ASSERT_EQ(buf[4], 'x');
  UT_ASSERT_EQ(buf[5], 'y');
  UT_ASSERT_EQ(ind, 6);
  return 0;
}

UT_TEST(test_asm_parse_directive_text_and_data)
{
  Section *text_sec;
  Section *data_sec;

  tok = TOK_ASMDIR_text;
  utb_clear_token_q();
  utb_queue_token(TOK_LINEFEED);
  asm_parse_directive(tcc_state, 1);
  text_sec = cur_text_section;
  UT_ASSERT(text_sec != NULL);
  UT_ASSERT_EQ(ind, 0);
  UT_ASSERT_EQ(text_sec->data_offset, 0);

  tok = TOK_ASMDIR_data;
  utb_clear_token_q();
  utb_queue_token(TOK_LINEFEED);
  asm_parse_directive(tcc_state, 1);
  data_sec = cur_text_section;
  UT_ASSERT(data_sec != NULL);
  UT_ASSERT(data_sec != text_sec);
  UT_ASSERT_EQ(ind, 0);
  UT_ASSERT_EQ(data_sec->data_offset, 0);

  tcc_free(text_sec);
  tcc_free(data_sec);
  return 0;
}

UT_TEST(test_asm_parse_directive_previous)
{
  Section sec_a;
  Section sec_b;
  unsigned char buf_a[64];
  unsigned char buf_b[64];

  ut_setup_section(&sec_a, buf_a, sizeof(buf_a));
  ut_setup_section(&sec_b, buf_b, sizeof(buf_b));
  sec_a.data_offset = 12; /* pretend 12 bytes were emitted in sec_a earlier */
  cur_text_section = &sec_b;
  last_text_section = &sec_a;
  ind = 4;

  tok = TOK_ASMDIR_previous;
  utb_clear_token_q();
  asm_parse_directive(tcc_state, 1);

  /* .previous swaps cur/last and use_section1() saves the leaving section's
     ind while restoring the entered section's saved offset. */
  UT_ASSERT(cur_text_section == &sec_a);
  UT_ASSERT_EQ(ind, 12);              /* restored from sec_a.data_offset */
  UT_ASSERT_EQ(sec_b.data_offset, 4); /* ind saved into the section we left */
  UT_ASSERT(last_text_section == &sec_b);
  return 0;
}

UT_TEST(test_asm_parse_directive_thumb_func)
{
  int name = token_for_name("funcsym");

  tcc_state->thumb_func = -1;
  tok = TOK_ASMDIR_thumb_func;
  utb_clear_token_q();
  utb_queue_token(TOK_LINEFEED);
  asm_parse_directive(tcc_state, 1);
  UT_ASSERT_EQ(tcc_state->thumb_func, 1);

  tcc_state->thumb_func = -1;
  tok = TOK_ASMDIR_thumb_func;
  utb_clear_token_q();
  utb_queue_token(name);
  asm_parse_directive(tcc_state, 1);
  UT_ASSERT_EQ(tcc_state->thumb_func, name);
  return 0;
}

UT_TEST(test_asm_parse_directive_fpu)
{
  int name = token_for_name("fpv5-sp-d16");

  tok = TOK_ASMDIR_fpu;
  utb_clear_token_q();
  utb_queue_token(name);
  utb_queue_token(TOK_LINEFEED);
  asm_parse_directive(tcc_state, 1);

  /* Survives parsing and calls the stub FPU handler. */
  return 0;
}

UT_TEST(test_asm_parse_directive_globl_and_weak)
{
  int name = token_for_name("globsym");
  Sym *gsym;
  Sym *wsym;

  tcc_state->leading_underscore = 0;
  tok = TOK_ASMDIR_globl;
  utb_clear_token_q();
  utb_queue_token(name);
  utb_queue_token(TOK_LINEFEED);
  asm_parse_directive(tcc_state, 1);

  gsym = global_stack;
  UT_ASSERT(gsym != NULL);
  UT_ASSERT((gsym->type.t & VT_STATIC) == 0);

  /* sym_find() is stubbed to always miss (no real symbol table is linked into
     this binary), so .weak creates a fresh asm symbol rather than reusing
     globsym's; assert on the symbol it actually produced. */
  tok = TOK_ASMDIR_weak;
  utb_clear_token_q();
  utb_queue_token(name);
  utb_queue_token(TOK_LINEFEED);
  asm_parse_directive(tcc_state, 1);

  wsym = global_stack;
  UT_ASSERT(wsym != gsym);
  UT_ASSERT(wsym->a.weak == 1);
  UT_ASSERT((wsym->type.t & VT_STATIC) == 0);

  global_stack = wsym->prev;
  tcc_free(wsym);
  global_stack = gsym->prev;
  tcc_free(gsym);
  return 0;
}

UT_TEST(test_asm_parse_directive_type_function)
{
  int name = token_for_name("typesym");
  Sym *sym;

  tcc_state->leading_underscore = 0;
  tok = TOK_ASMDIR_type;
  utb_clear_token_q();
  utb_queue_token(name);
  utb_queue_token(',');
  utb_queue_str("function");
  asm_parse_directive(tcc_state, 1);

  sym = global_stack;
  UT_ASSERT(sym != NULL);
  UT_ASSERT((sym->type.t & VT_ASM_FUNC) != 0);

  global_stack = sym->prev;
  tcc_free(sym);
  return 0;
}

UT_TEST(test_asm_parse_directive_macro_and_endm)
{
  int name = token_for_name("mymacro");

  tok = TOK_ASMDIR_macro;
  utb_clear_token_q();
  utb_queue_token(name);
  utb_queue_token(TOK_LINEFEED);
  utb_queue_token(TOK_ASMDIR_endm);
  utb_queue_token(TOK_LINEFEED);
  asm_parse_directive(tcc_state, 1);

  UT_ASSERT(asm_macro_find(name) != NULL);
  asm_macros_free();
  return 0;
}

UT_TEST(test_asm_parse_directive_rept_zero)
{
  tok = TOK_ASMDIR_rept;
  utb_clear_token_q();
  utb_queue_ppnum("0");
  utb_queue_token(TOK_LINEFEED);
  utb_queue_token(TOK_ASMDIR_endr);
  utb_queue_token(TOK_LINEFEED);
  asm_parse_directive(tcc_state, 1);

  return 0;
}

UT_TEST(test_asm_parse_directive_file_and_ident)
{
  char filename[] = "\"foo.c\"";

  tok = TOK_ASMDIR_file;
  utb_clear_token_q();
  utb_queue_ppnum("1");
  utb_queue_token(TOK_PPSTR);
  tokc.str.data = filename;
  tokc.str.size = (int)sizeof(filename);
  asm_parse_directive(tcc_state, 1);

  tok = TOK_ASMDIR_ident;
  utb_clear_token_q();
  utb_queue_str("version");
  asm_parse_directive(tcc_state, 1);

  return 0;
}

UT_TEST(test_asm_parse_directive_set_feature)
{
  tok = TOK_ASMDIR_set;
  utb_clear_token_q();
  utb_queue_token(token_for_name("mips16"));
  utb_queue_token(TOK_LINEFEED);
  asm_parse_directive(tcc_state, 1);
  return 0;
}

UT_TEST(test_asm_parse_directive_asciz)
{
  Section sec;
  unsigned char buf[64];

  ut_setup_section(&sec, buf, sizeof(buf));
  cur_text_section = &sec;
  ind = 0;

  tok = TOK_ASMDIR_asciz;
  utb_clear_token_q();
  utb_queue_str("ab");
  asm_parse_directive(tcc_state, 1);

  UT_ASSERT_EQ(ind, 3);
  UT_ASSERT_EQ(buf[0], 'a');
  UT_ASSERT_EQ(buf[1], 'b');
  UT_ASSERT_EQ(buf[2], '\0');
  return 0;
}

UT_TEST(test_asm_parse_directive_global_and_hidden)
{
  int name = token_for_name("ghsym");
  Sym *gsym;
  Sym *hsym;

  tcc_state->leading_underscore = 0;

  tok = TOK_ASMDIR_global;
  utb_clear_token_q();
  utb_queue_token(name);
  utb_queue_token(TOK_LINEFEED);
  asm_parse_directive(tcc_state, 1);

  gsym = global_stack;
  UT_ASSERT(gsym != NULL);
  UT_ASSERT((gsym->type.t & VT_STATIC) == 0);

  tok = TOK_ASMDIR_hidden;
  utb_clear_token_q();
  utb_queue_token(name);
  utb_queue_token(TOK_LINEFEED);
  asm_parse_directive(tcc_state, 1);

  hsym = global_stack;
  UT_ASSERT(hsym != gsym);
  UT_ASSERT_EQ(hsym->a.visibility, STV_HIDDEN);

  global_stack = hsym->prev;
  tcc_free(hsym);
  global_stack = gsym->prev;
  tcc_free(gsym);
  return 0;
}

UT_TEST(test_asm_parse_directive_type_object_and_unknown)
{
  int name = token_for_name("objsym");
  Sym *sym;

  tcc_state->leading_underscore = 0;

  tok = TOK_ASMDIR_type;
  utb_clear_token_q();
  utb_queue_token(name);
  utb_queue_token(',');
  utb_queue_str("object");
  asm_parse_directive(tcc_state, 1);

  sym = global_stack;
  UT_ASSERT(sym != NULL);
  UT_ASSERT((sym->type.t & VT_ASM) != 0);

  global_stack = sym->prev;
  tcc_free(sym);

  name = token_for_name("weirdsym");
  tok = TOK_ASMDIR_type;
  utb_clear_token_q();
  utb_queue_token(name);
  utb_queue_token(',');
  utb_queue_str("weird");
  asm_parse_directive(tcc_state, 1);

  sym = global_stack;
  UT_ASSERT(sym != NULL);
  global_stack = sym->prev;
  tcc_free(sym);
  return 0;
}

UT_TEST(test_asm_parse_directive_section_with_options)
{
  Section sec_old;
  unsigned char buf[64];
  Section *new_sec;

  ut_setup_section(&sec_old, buf, sizeof(buf));
  cur_text_section = &sec_old;
  ind = 0;

  tok = TOK_ASMDIR_section;
  utb_clear_token_q();
  utb_queue_token(token_for_name("mysec"));
  utb_queue_token(',');
  utb_queue_str("wx");
  utb_queue_token(TOK_LINEFEED);
  asm_parse_directive(tcc_state, 1);

  new_sec = cur_text_section;
  UT_ASSERT(new_sec != &sec_old);
  tcc_free(new_sec);
  return 0;
}

UT_TEST(test_asm_parse_directive_push_and_popsection)
{
  Section sec_old;
  unsigned char buf[64];
  Section *pushed;

  ut_setup_section(&sec_old, buf, sizeof(buf));
  cur_text_section = &sec_old;
  ind = 7;

  tok = TOK_ASMDIR_pushsection;
  utb_clear_token_q();
  utb_queue_token(token_for_name("pushed"));
  utb_queue_token(TOK_LINEFEED);
  asm_parse_directive(tcc_state, 1);

  pushed = cur_text_section;
  UT_ASSERT(pushed != &sec_old);
  UT_ASSERT_EQ(sec_old.data_offset, 7);
  UT_ASSERT(pushed->prev == &sec_old);

  tok = TOK_ASMDIR_popsection;
  utb_clear_token_q();
  utb_queue_token(TOK_LINEFEED);
  asm_parse_directive(tcc_state, 1);

  UT_ASSERT(cur_text_section == &sec_old);
  UT_ASSERT_EQ(ind, 7);
  UT_ASSERT(pushed->prev == NULL);

  tcc_free(pushed);
  return 0;
}

UT_TEST(test_asm_parse_directive_ident_with_identifier)
{
  tok = TOK_ASMDIR_ident;
  utb_clear_token_q();
  utb_queue_token(token_for_name("version_id"));
  utb_queue_token(TOK_LINEFEED);
  asm_parse_directive(tcc_state, 1);
  return 0;
}

UT_TEST(test_asm_parse_directive_file_with_ppstr_and_identifier)
{
  char filename[] = "\"foo.c\"";

  tok = TOK_ASMDIR_file;
  utb_clear_token_q();
  utb_queue_ppnum("1");
  utb_queue_ppstr(filename);
  utb_queue_token(TOK_LINEFEED);
  asm_parse_directive(tcc_state, 1);

  tok = TOK_ASMDIR_file;
  utb_clear_token_q();
  utb_queue_ppnum("2");
  utb_queue_token(token_for_name("bar"));
  utb_queue_token(TOK_LINEFEED);
  asm_parse_directive(tcc_state, 1);
  return 0;
}

UT_TEST(test_asm_parse_directive_syntax_thumb_symver)
{
  tok = TOK_ASMDIR_syntax;
  utb_clear_token_q();
  utb_queue_token(token_for_name("unified"));
  utb_queue_token(TOK_LINEFEED);
  asm_parse_directive(tcc_state, 1);

  tok = TOK_ASMDIR_thumb;
  utb_clear_token_q();
  utb_queue_token(TOK_LINEFEED);
  asm_parse_directive(tcc_state, 1);

  tok = TOK_ASMDIR_symver;
  utb_clear_token_q();
  utb_queue_token(token_for_name("name"));
  utb_queue_token(',');
  utb_queue_token('@');
  utb_queue_token(token_for_name("ver"));
  utb_queue_token(TOK_LINEFEED);
  asm_parse_directive(tcc_state, 1);
  return 0;
}

UT_TEST(test_asm_parse_directive_rept_nonzero)
{
  Section sec;
  unsigned char buf[64];

  ut_setup_section(&sec, buf, sizeof(buf));
  cur_text_section = &sec;
  ind = 0;

  tok = TOK_ASMDIR_rept;
  utb_clear_token_q();
  utb_queue_ppnum("2");
  utb_queue_token(TOK_LINEFEED);
  utb_queue_token(TOK_ASMDIR_thumb);
  utb_queue_token(TOK_LINEFEED);
  utb_queue_token(TOK_ASMDIR_endr);
  utb_queue_token(TOK_LINEFEED);
  asm_parse_directive(tcc_state, 1);

  return 0;
}

UT_TEST(test_asm_parse_directive_macro_with_args)
{
  int name = token_for_name("add_two");
  int arg1 = token_for_name("a");
  int arg2 = token_for_name("b");

  tok = TOK_ASMDIR_macro;
  utb_clear_token_q();
  utb_queue_token(name);
  utb_queue_token(arg1);
  utb_queue_token(',');
  utb_queue_token(arg2);
  utb_queue_token(TOK_LINEFEED);
  utb_queue_token(TOK_ASMDIR_thumb);
  utb_queue_token(TOK_LINEFEED);
  utb_queue_token(TOK_ASMDIR_endm);
  utb_queue_token(TOK_LINEFEED);
  asm_parse_directive(tcc_state, 1);

  UT_ASSERT(asm_macro_find(name) != NULL);
  UT_ASSERT_EQ(asm_macro_find(name)->nb_args, 2);
  asm_macros_free();
  return 0;
}

UT_TEST(test_tcc_assemble_internal_comment_and_instruction)
{
  Section sec;
  unsigned char buf[64];
  int opcode = token_for_name("nop");

  ut_setup_section(&sec, buf, sizeof(buf));
  cur_text_section = &sec;
  ind = 0;

  utb_clear_token_q();
  utb_queue_token('#');
  utb_queue_token(token_for_name("comment"));
  utb_queue_token(TOK_LINEFEED);
  utb_queue_token(opcode);
  utb_queue_token(TOK_LINEFEED);
  utb_queue_token(TOK_EOF);

  tcc_assemble_internal(tcc_state, 0, 1);
  return 0;
}

UT_TEST(test_tcc_assemble_internal_directive)
{
  Section sec;
  unsigned char buf[64];

  ut_setup_section(&sec, buf, sizeof(buf));
  cur_text_section = &sec;
  ind = 0;

  utb_clear_token_q();
  utb_queue_token(TOK_ASMDIR_syntax);
  utb_queue_token(token_for_name("unified"));
  utb_queue_token(TOK_LINEFEED);
  utb_queue_token(TOK_EOF);

  tcc_assemble_internal(tcc_state, 0, 1);
  return 0;
}

UT_TEST(test_tcc_assemble_internal_macro_invocation)
{
  Section sec;
  unsigned char buf[64];
  int mname = token_for_name("mymacro");
  int arg = token_for_name("x");

  ut_setup_section(&sec, buf, sizeof(buf));
  cur_text_section = &sec;
  ind = 0;

  utb_clear_token_q();
  utb_queue_token(TOK_ASMDIR_macro);
  utb_queue_token(mname);
  utb_queue_token(arg);
  utb_queue_token(TOK_LINEFEED);
  utb_queue_token(TOK_ASMDIR_endm);
  utb_queue_token(TOK_LINEFEED);
  utb_queue_token(mname);
  utb_queue_ppnum("42");
  utb_queue_token(TOK_LINEFEED);
  utb_queue_token(TOK_EOF);

  tcc_assemble_internal(tcc_state, 0, 1);
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
  UT_RUN(test_find_constraint_named_reference_not_found);
  UT_RUN(test_find_constraint_invalid_start_returns_minus_one);
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
  UT_RUN(test_asm_expr_prod_multiplies_and_divides);
  UT_RUN(test_asm_expr_prod_modulo_and_shifts);
  UT_RUN(test_asm_expr_logic_bitwise_ops);
  UT_RUN(test_asm_expr_sum_adds_and_subtracts_constants);
  UT_RUN(test_asm_expr_sum_adds_constant_to_symbol);
  UT_RUN(test_asm_expr_cmp_relational_operators);
  UT_RUN(test_asm_expr_top_level_evaluates_comparison);
  UT_RUN(test_asm_int_expr_evaluates_arithmetic_expr);
  UT_RUN(test_asm_expr_unary_parses_parenthesized_expr);
  UT_RUN(test_asm_expr_unary_parses_local_label_forward_ref);
  UT_RUN(test_asm_parse_directive_align_and_fill_value);
  UT_RUN(test_asm_parse_directive_p2align_and_skip);
  UT_RUN(test_asm_parse_directive_byte_word_long);
  UT_RUN(test_asm_parse_directive_quad);
  UT_RUN(test_asm_parse_directive_fill);
  UT_RUN(test_asm_parse_directive_org);
  UT_RUN(test_asm_parse_directive_string_and_ascii);
  UT_RUN(test_asm_parse_directive_text_and_data);
  UT_RUN(test_asm_parse_directive_previous);
  UT_RUN(test_asm_parse_directive_thumb_func);
  UT_RUN(test_asm_parse_directive_fpu);
  UT_RUN(test_asm_parse_directive_globl_and_weak);
  UT_RUN(test_asm_parse_directive_type_function);
  UT_RUN(test_asm_parse_directive_macro_and_endm);
  UT_RUN(test_asm_parse_directive_rept_zero);
  UT_RUN(test_asm_parse_directive_file_and_ident);
  UT_RUN(test_asm_parse_directive_set_feature);
  UT_RUN(test_asm_parse_directive_asciz);
  UT_RUN(test_asm_parse_directive_global_and_hidden);
  UT_RUN(test_asm_parse_directive_type_object_and_unknown);
  UT_RUN(test_asm_parse_directive_section_with_options);
  UT_RUN(test_asm_parse_directive_push_and_popsection);
  UT_RUN(test_asm_parse_directive_ident_with_identifier);
  UT_RUN(test_asm_parse_directive_file_with_ppstr_and_identifier);
  UT_RUN(test_asm_parse_directive_syntax_thumb_symver);
  UT_RUN(test_asm_parse_directive_rept_nonzero);
  UT_RUN(test_asm_parse_directive_macro_with_args);
  UT_RUN(test_tcc_assemble_internal_comment_and_instruction);
  UT_RUN(test_tcc_assemble_internal_directive);
  UT_RUN(test_tcc_assemble_internal_macro_invocation);
}
