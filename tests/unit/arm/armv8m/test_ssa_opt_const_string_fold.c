/*
 *  test_ssa_opt_const_string_fold.c - suite for source/opt/ssa/const_string_fold.c
 *  and its handler TUs (str_strlen.c, str_strcmp.c).
 *
 *  Two layers:
 *    - Handler-level: build a flat FUNCCALL on a bare TCCIRState, construct a
 *      StrFoldCtx by hand (builtin_id set directly, so no callee-name mapping is
 *      needed) and drive tcc_strfold_<x>.can_fold / .fold.  Covers the fold /
 *      no-fold matrix of str_strlen.c and str_strcmp.c without ELF section data
 *      (stack-strlen byte-store path + zero-length compare paths).
 *    - Driver-level: a real ssa_ctx (CFG + SSA + vinfo) run through
 *      tcc_ir_ssa_opt_const_string_fold, exercising registry dispatch (by
 *      builtin_id), the unknown-builtin skip, and the changes>0 rebuild path.
 *
 *  HARNESS NOTES:
 *    - Links the real source/opt/ssa handler TUs via UT11.
 *    - Reuses ir_build.h / ssa_build.h fixtures (same tricks as
 *      test_opt_constfold.c for the flat tcc_ir_opt_const_string_calls).
 */

#include "ssa_build.h"
#include "str_handlers.h"
#include "const_string_fold.h"

#include "ut.h"

#define I32 IROP_BTYPE_INT32
#define I8 IROP_BTYPE_INT8

/* Callee SYMREF whose token resolves via resolve_str_builtin_by_tok (a
 * TOK_builtin_* token, or any other for the unknown-builtin case).  The driver
 * needs this to pick a handler; the handlers themselves trust ctx->builtin_id.
 * (UT11's get_tok_str stub returns "", so name resolution is unavailable — the
 * token path is the one that works here.) */
static IROperand csf_callee(TCCIRState *ir, Sym *sym, int tok)
{
  sym->v = tok;
  uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, 0, 0);
  return irop_make_symref(0, sidx, 0, 0, 0, I32);
}

/* Emit `strlen(&stackbuf)` where stackbuf already holds `bytes` (incl. NUL).
 * Returns the FUNCCALLVAL index. */
static int emit_stack_strlen_call(TCCIRState *ir, const char *bytes, int n, int call_id)
{
  for (int k = 0; k < n; k++)
    utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(k, 1, 0, 0, I8),
             utb_imm((unsigned char)bytes[k], I8), UTB_NONE);
  IROperand buf = irop_make_stackoff(-1, 0, 0, 0, 0, I32);
  buf.is_local = 1;
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, buf,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  return utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), UTB_NONE,
                  utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 1), I32));
}

static StrFoldCtx csf_ctx(TCCIRState *ir, int call_idx, int builtin_id, int is_valued)
{
  StrFoldCtx c;
  c.ssa = NULL;
  c.ir = ir;
  c.call_idx = call_idx;
  c.builtin_id = builtin_id;
  c.is_valued = is_valued;
  return c;
}

/* ----------------------------------------------------- strlen handler */

/* POSITIVE: stack "hi\0" -> strlen folds to ASSIGN #2. */
UT_TEST(test_csf_strlen_stack_positive)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  int i_call = emit_stack_strlen_call(ir, "hi", 3, 1); /* 'h','i','\0' */
  StrFoldCtx c = csf_ctx(ir, i_call, STRBI_STRLEN, 1);

  UT_ASSERT_EQ(tcc_strfold_strlen.can_fold(&c), 1);
  UT_ASSERT_EQ(tcc_strfold_strlen.fold(&c), 1);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i_call)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, i_call)), 2);

  utb_free(ir);
  return 0;
}

/* GUARD: no NUL terminator -> stack scan fails -> can_fold=0, call untouched. */
UT_TEST(test_csf_strlen_stack_no_nul_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  int i_call = emit_stack_strlen_call(ir, "hi", 2, 1); /* no NUL */
  StrFoldCtx c = csf_ctx(ir, i_call, STRBI_STRLEN, 1);

  UT_ASSERT_EQ(tcc_strfold_strlen.can_fold(&c), 0);
  UT_ASSERT_EQ(tcc_strfold_strlen.fold(&c), 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* GUARD: a FUNCCALLVOID strlen is not a value -> is_valued=0 -> no fold. */
UT_TEST(test_csf_strlen_void_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  int i_call = emit_stack_strlen_call(ir, "hi", 3, 1);
  /* Reinterpret the same call as a void call for the is_valued=0 branch. */
  StrFoldCtx c = csf_ctx(ir, i_call, STRBI_STRLEN, 0);

  UT_ASSERT_EQ(tcc_strfold_strlen.can_fold(&c), 0);
  UT_ASSERT_EQ(tcc_strfold_strlen.fold(&c), 0);

  utb_free(ir);
  return 0;
}

/* ----------------------------------------------------- strcmp family */

static int emit_cmp_call(TCCIRState *ir, int nparams, int has_len, int len, int call_id)
{
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(1, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  if (has_len)
    utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(len, I32),
             utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 2), I32));
  return utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(2, I32), UTB_NONE,
                  utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, nparams), I32));
}

/* POSITIVE: memcmp(a, b, 0) -> ASSIGN #0 (n==0 needs no string data). */
UT_TEST(test_csf_memcmp_zero_len_positive)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  int i_call = emit_cmp_call(ir, 3, 1, 0, 1);
  StrFoldCtx c = csf_ctx(ir, i_call, STRBI_MEMCMP, 1);

  UT_ASSERT_EQ(tcc_strfold_memcmp.can_fold(&c), 1);
  UT_ASSERT_EQ(tcc_strfold_memcmp.fold(&c), 1);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, i_call)), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: strncmp(a, b, 0) -> ASSIGN #0. */
UT_TEST(test_csf_strncmp_zero_len_positive)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  int i_call = emit_cmp_call(ir, 3, 1, 0, 1);
  StrFoldCtx c = csf_ctx(ir, i_call, STRBI_STRNCMP, 1);

  UT_ASSERT_EQ(tcc_strfold_strncmp.can_fold(&c), 1);
  UT_ASSERT_EQ(tcc_strfold_strncmp.fold(&c), 1);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, i_call)), 0);

  utb_free(ir);
  return 0;
}

/* GUARD: strcmp with non-constant (temp) string args -> eval fails -> no fold. */
UT_TEST(test_csf_strcmp_nonconst_args_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  int i_call = emit_cmp_call(ir, 2, 0, 0, 1);
  StrFoldCtx c = csf_ctx(ir, i_call, STRBI_STRCMP, 1);

  UT_ASSERT_EQ(tcc_strfold_strcmp.can_fold(&c), 0);
  UT_ASSERT_EQ(tcc_strfold_strcmp.fold(&c), 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* GUARD: strncmp missing its length param (only 2 params) -> no fold. */
UT_TEST(test_csf_strncmp_missing_len_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  int i_call = emit_cmp_call(ir, 2, 0, 0, 1);
  StrFoldCtx c = csf_ctx(ir, i_call, STRBI_STRNCMP, 1);

  UT_ASSERT_EQ(tcc_strfold_strncmp.can_fold(&c), 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* ----------------------------------------------------- strcpy guards
 *
 * The positive strcpy->BLOCK_COPY fold needs a real const-string symbol (ELF
 * section data), which the hand-built harness has no cheap way to synthesize
 * (same limit as the strcmp real-string paths) -- it is covered by the runtime
 * IR test 352_ssa_const_string_fold.c.  Here we pin the can_fold guards. */

/* GUARD: dst param is not a stack address (a plain temp) -> no fold. */
UT_TEST(test_csf_strcpy_nonstack_dst_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(1, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 1), I32));
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(2, I32), UTB_NONE,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 2), I32));
  StrFoldCtx c = csf_ctx(ir, i_call, STRBI_STRCPY, 1);

  UT_ASSERT_EQ(tcc_strfold_strcpy.can_fold(&c), 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* GUARD: stack dst but non-constant src -> no fold. */
UT_TEST(test_csf_strcpy_nonconst_src_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  IROperand dst = irop_make_stackoff(-1, 0, 0, 0, 0, I32);
  dst.is_local = 1;
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, dst,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(1, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 1), I32));
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(2, I32), UTB_NONE,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 2), I32));
  StrFoldCtx c = csf_ctx(ir, i_call, STRBI_STRCPY, 1);

  UT_ASSERT_EQ(tcc_strfold_strcpy.can_fold(&c), 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* ----------------------------------------------------- driver / registry */

static void csf_build_ssa(ssa_ctx *c)
{
  ssa_ctx_build_cfg(c);
  if (c->cfg)
    tcc_ir_cfg_compute_dom_frontiers(c->cfg);
  ssa_ctx_build_ssa_plain(c);
  ssa_ctx_rebuild(c);
}

/* Driver dispatch: a call whose name is not a string builtin is skipped
 * (resolve_str_builtin_id -> STRBI_UNKNOWN), no rebuild, call intact. */
UT_TEST(test_csf_driver_unknown_builtin_skip)
{
  ssa_ctx c = ssa_ctx_new(1, 10);
  static Sym callee_sym;
  /* TOK_IDENT is not a TOK_builtin_* token -> resolve_str_builtin_by_tok
   * returns STRBI_UNKNOWN -> driver skips it. */
  IROperand callee = csf_callee(c.ir, &callee_sym, TOK_IDENT);

  utb_emit(c.ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  int i_call = utb_emit(c.ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));

  csf_build_ssa(&c);
  UT_ASSERT_EQ(tcc_ir_ssa_opt_const_string_fold(c.ctx), 0);
  UT_ASSERT_EQ(utb_op(c.ir, i_call), TCCIR_OP_FUNCCALLVAL);

  ssa_ctx_free(&c);
  return 0;
}

/* Driver end-to-end: memcmp(a, b, 0) dispatches by builtin_id to the memcmp
 * handler, folds to ASSIGN #0, and the changes>0 rebuild path runs. */
UT_TEST(test_csf_driver_memcmp_zero_fold)
{
  ssa_ctx c = ssa_ctx_new(1, 10);
  static Sym callee_sym;
  IROperand callee = csf_callee(c.ir, &callee_sym, TOK_builtin_memcmp);

  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(0, I32));
  utb_emit(c.ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  utb_emit(c.ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(1, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 1), I32));
  utb_emit(c.ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 2), I32));
  int i_call = utb_emit(c.ir, TCCIR_OP_FUNCCALLVAL, utb_temp(2, I32), callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 3), I32));

  csf_build_ssa(&c);
  int changes = tcc_ir_ssa_opt_const_string_fold(c.ctx);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(c.ir, i_call), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src1(c.ir, i_call)), 0);

  ssa_ctx_free(&c);
  return 0;
}

/* Driver: no calls at all -> zero changes, no crash. */
UT_TEST(test_csf_driver_no_calls)
{
  ssa_ctx c = ssa_ctx_new(1, 10);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(7, I32));
  csf_build_ssa(&c);
  UT_ASSERT_EQ(tcc_ir_ssa_opt_const_string_fold(c.ctx), 0);
  ssa_ctx_free(&c);
  return 0;
}

/* ------------------------------------------ string-search handler guards
 *
 * The positive folds (strspn/strcspn counts, strchr/strstr/strpbrk pointers)
 * need real const-string ELF section data, which the hand-built harness has no
 * cheap way to synthesize -- they are covered by the runtime IR test
 * 357_ssa_string_search_fold.c.  Here we pin the can_fold guards: non-constant
 * operands and void (discarded-result) calls must never fold. */

/* Emit a 2-arg FUNCCALL* with both args non-constant temps; return call idx. */
static int emit_two_temp_call(TCCIRState *ir, int is_valued, int call_id)
{
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(1, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  if (is_valued)
    return utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(2, I32), UTB_NONE,
                    utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 2), I32));
  return utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, UTB_NONE,
                  utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 2), I32));
}

/* GUARD: non-constant args never fold, for every new handler. */
UT_TEST(test_csf_search_nonconst_no_fold)
{
  const StrFoldHandler *hs[] = {
      &tcc_strfold_strspn, &tcc_strfold_strcspn, &tcc_strfold_strchr,
      &tcc_strfold_index,  &tcc_strfold_strrchr, &tcc_strfold_rindex,
      &tcc_strfold_strstr, &tcc_strfold_strpbrk,
  };
  for (unsigned k = 0; k < sizeof(hs) / sizeof(hs[0]); k++)
  {
    TCCIRState *ir = utb_new();
    utb_pools_init(ir);
    int i_call = emit_two_temp_call(ir, 1, 1);
    StrFoldCtx c = csf_ctx(ir, i_call, hs[k]->builtin_id, 1);
    UT_ASSERT_EQ(hs[k]->can_fold(&c), 0);
    UT_ASSERT_EQ(hs[k]->fold(&c), 0);
    UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);
    utb_free(ir);
  }
  return 0;
}

/* GUARD: a void (discarded-result) call never folds. */
UT_TEST(test_csf_search_void_no_fold)
{
  const StrFoldHandler *hs[] = {&tcc_strfold_strspn, &tcc_strfold_strchr,
                                &tcc_strfold_strstr, &tcc_strfold_strpbrk};
  for (unsigned k = 0; k < sizeof(hs) / sizeof(hs[0]); k++)
  {
    TCCIRState *ir = utb_new();
    utb_pools_init(ir);
    int i_call = emit_two_temp_call(ir, 0, 1);
    StrFoldCtx c = csf_ctx(ir, i_call, hs[k]->builtin_id, 0);
    UT_ASSERT_EQ(hs[k]->can_fold(&c), 0);
    UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVOID);
    utb_free(ir);
  }
  return 0;
}

/* GUARD: strchr with a non-constant char (second arg a temp) does not fold. */
UT_TEST(test_csf_strchr_nonconst_char_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  int i_call = emit_two_temp_call(ir, 1, 1);
  StrFoldCtx c = csf_ctx(ir, i_call, STRBI_STRCHR, 1);
  UT_ASSERT_EQ(tcc_strfold_strchr.can_fold(&c), 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);
  utb_free(ir);
  return 0;
}

/* Emit a 3-arg FUNCCALL* with non-constant temps for s/c and `n` immediate. */
static int emit_memchr_call(TCCIRState *ir, int is_valued, int n, int call_id)
{
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(1, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(n, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 2), I32));
  if (is_valued)
    return utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(2, I32), UTB_NONE,
                    utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 3), I32));
  return utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, UTB_NONE,
                  utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 3), I32));
}

/* GUARD: memchr with a non-constant haystack/needle does not fold. */
UT_TEST(test_csf_memchr_nonconst_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  int i_call = emit_memchr_call(ir, 1, 4, 1);
  StrFoldCtx c = csf_ctx(ir, i_call, STRBI_MEMCHR, 1);
  UT_ASSERT_EQ(tcc_strfold_memchr.can_fold(&c), 0);
  UT_ASSERT_EQ(tcc_strfold_memchr.fold(&c), 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);
  utb_free(ir);
  return 0;
}

/* GUARD: a void (discarded-result) memchr never folds. */
UT_TEST(test_csf_memchr_void_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  int i_call = emit_memchr_call(ir, 0, 4, 1);
  StrFoldCtx c = csf_ctx(ir, i_call, STRBI_MEMCHR, 0);
  UT_ASSERT_EQ(tcc_strfold_memchr.can_fold(&c), 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVOID);
  utb_free(ir);
  return 0;
}

UT_COVERS("const_string_fold");
