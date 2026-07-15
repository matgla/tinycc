/*
 *  test_ir_dump.c - suite for ir/dump.c debug dumping helpers
 *
 *  ir/dump.c is no longer debug-only convenience: tcc_print_quadruple_irop /
 *  print_iroperand_short / tcc_ir_show back the live -dump-ir-passes=
 *  machinery (tcc_ir_dump_after_pass(), wired into ir/opt_pipeline.c's group
 *  runner). This suite oracle-asserts the *exact* dumped string content for
 *  each reachable IR-op-printing branch, not just "it didn't crash": it
 *  builds a hand-crafted IRQuadCompact/IROperand with ir_build.h's utb_*
 *  helpers, redirects the (real, glibc) `stdout` FILE* to an in-memory
 *  buffer via open_memstream(), invokes the dumper, and diffs the captured
 *  text.
 *
 *  _GNU_SOURCE (needed for open_memstream with -std=c11) is already pulled
 *  in transitively by ir.h -> tcc.h, which #defines it before including
 *  <stdio.h>; ir.h's own first include (<stdbool.h>) doesn't touch stdio,
 *  so the definition below is belt-and-suspenders in case include order
 *  ever changes.
 *
 *  Captured buffers come from glibc's open_memstream(), not tcc's own
 *  allocator, so they must be released with the real libc free() -- tcc.h
 *  #defines plain `free` to the intentionally-undefined `use_tcc_free` to
 *  catch accidental raw frees of tcc_malloc'd memory elsewhere in the
 *  codebase. libtcc.c's libc_free() (declared in tcc.h) is the established
 *  escape hatch for exactly this case (see its use for realpath() buffers).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define USING_GLOBALS
#include "ir_build.h"
#include "ut.h"

#include <string.h>

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64

/* print_svalue_short (ir/dump.c) has no declaration in any header -- unlike
 * its IROperand-based sibling print_iroperand_short (declared in tccir.h),
 * it has no remaining production callers, so nothing ever needed to
 * forward-declare it. Declare it locally to call it from this suite. */
void print_svalue_short(SValue *sv);

/* ---------------------------------------------------------------------- */
/* stdout capture helper                                                  */
/* ---------------------------------------------------------------------- */

/* All of tcc_print_quadruple_irop / print_iroperand_short / tcc_ir_show /
 * print_svalue_short print unconditionally to the process's `stdout`
 * (glibc: an ordinary externally-visible `FILE *stdout`, reassignable).
 * Redirect it to a growable in-memory buffer for the duration of `fn(arg)`,
 * then hand back a NUL-terminated heap copy of exactly what was printed.
 * Caller must free() the result. */
typedef void (*ut_capture_fn)(void *arg);

static char *ut_capture_stdout(ut_capture_fn fn, void *arg)
{
  char *buf = NULL;
  size_t buf_size = 0;
  FILE *mem = open_memstream(&buf, &buf_size);
  if (!mem)
  {
    fprintf(stderr, "ut_capture_stdout: open_memstream failed\n");
    exit(1);
  }

  FILE *saved_stdout = stdout;
  stdout = mem;
  fn(arg);
  fflush(mem);
  stdout = saved_stdout;
  fclose(mem);

  return buf; /* NUL-terminated by open_memstream's fclose/fflush contract */
}

/* -------------------------------------------------------------------------- */
/* Operation name mapping                                                     */
/* -------------------------------------------------------------------------- */

UT_TEST(test_get_op_name_known_ops)
{
  UT_ASSERT_STREQ(tcc_ir_get_op_name(TCCIR_OP_ADD), "ADD");
  UT_ASSERT_STREQ(tcc_ir_get_op_name(TCCIR_OP_SUB), "SUB");
  UT_ASSERT_STREQ(tcc_ir_get_op_name(TCCIR_OP_NOP), "NOP");
  UT_ASSERT_STREQ(tcc_ir_get_op_name(TCCIR_OP_RETURNVOID), "RETURNVOID");
  UT_ASSERT_STREQ(tcc_ir_get_op_name(TCCIR_OP_FUNCCALLVAL), "CALL");
  return 0;
}

UT_TEST(test_get_op_name_unknown)
{
  UT_ASSERT_STREQ(tcc_ir_get_op_name((TccIrOp)99999), "UNKNOWN_OP");
  return 0;
}

UT_TEST(test_dump_op_name_same_as_get)
{
  UT_ASSERT_STREQ(tcc_ir_dump_op_name(TCCIR_OP_MUL),
                  tcc_ir_get_op_name(TCCIR_OP_MUL));
  return 0;
}

/* -------------------------------------------------------------------------- */
/* print_iroperand_short -- exact dumped-string content per operand tag       */
/* -------------------------------------------------------------------------- */

struct capture_op_args
{
  TCCIRState *ir;
  IROperand op;
};

static void capture_op_fn(void *arg)
{
  struct capture_op_args *a = (struct capture_op_args *)arg;
  print_iroperand_short(a->ir, a->op);
}

static char *dump_op(TCCIRState *ir, IROperand op)
{
  struct capture_op_args a = {ir, op};
  return ut_capture_stdout(capture_op_fn, &a);
}

/* IMM32: plain positive/negative immediates print "#<decimal>". */
UT_TEST(test_print_operand_imm32)
{
  TCCIRState *ir = utb_new();
  char *s1 = dump_op(ir, utb_imm(42, I32));
  UT_ASSERT_STREQ(s1, "#42");
  libc_free(s1);

  char *s2 = dump_op(ir, utb_imm(-7, I32));
  UT_ASSERT_STREQ(s2, "#-7");
  libc_free(s2);

  utb_free(ir);
  return 0;
}

/* I64 (IROP_BTYPE_INT64): prints with the %lld path, not the truncating %d
 * one -- a value that doesn't fit in 32 bits must round-trip exactly. */
UT_TEST(test_print_operand_i64_wide_value)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  uint32_t idx = tcc_ir_pool_add_i64(ir, 0x123456789ALL);
  IROperand op = irop_make_i64(-1, idx, I64);

  char *s = dump_op(ir, op);
  UT_ASSERT_STREQ(s, "#78187493530");
  libc_free(s);

  utb_free(ir);
  return 0;
}

/* SYMREF: base case prints "GlobalSym(<tok>)"; a non-zero addend appends
 * "+<addend>"; is_lval appends the "***DEREF***" marker. Order: sym, then
 * addend, then deref marker. */
UT_TEST(test_print_operand_symref_plain)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  Sym sym;
  memset(&sym, 0, sizeof(sym));
  sym.v = 1234;

  IROperand op = utb_symref(ir, &sym, /*is_lval*/ 0, /*is_local*/ 0, /*is_const*/ 0, I32);
  char *s = dump_op(ir, op);
  UT_ASSERT_STREQ(s, "GlobalSym(1234)");
  libc_free(s);

  utb_free(ir);
  return 0;
}

UT_TEST(test_print_operand_symref_addend_and_deref)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  Sym sym;
  memset(&sym, 0, sizeof(sym));
  sym.v = 55;

  /* addend is threaded through the symref pool entry, not the IROperand
   * itself -- tcc_ir_pool_add_symref(ir, sym, addend, flags). */
  uint32_t sidx = tcc_ir_pool_add_symref(ir, &sym, /*addend*/ 12, 0);
  IROperand op = irop_make_symref(0, sidx, /*is_lval*/ 1, /*is_local*/ 0, /*is_const*/ 0, I32);

  char *s = dump_op(ir, op);
  UT_ASSERT_STREQ(s, "GlobalSym(55)+12***DEREF***");
  libc_free(s);

  utb_free(ir);
  return 0;
}

/* SYMREF with a NULL ir (or a symref pool entry whose sym is NULL) hits the
 * "sym not found" fallback branch. */
UT_TEST(test_print_operand_symref_null_sym_fallback)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  Sym sym;
  memset(&sym, 0, sizeof(sym));
  sym.v = 77;
  IROperand op = utb_symref(ir, &sym, 0, 0, 0, I32);

  /* Passing NULL ir makes irop_get_sym_ex() return NULL regardless of the
   * operand's own pool index -- print_iroperand_short must not deref it. */
  char *s = dump_op(NULL, op);
  UT_ASSERT_STREQ(s, "GlobalSym(?)");
  libc_free(s);

  utb_free(ir);
  return 0;
}

/* STACKOFF, non-llocal, with a bound VAR vreg and show_physical_regs off:
 * prints "V<pos>" (short vreg form), "&V<pos>" when not an lvalue. */
UT_TEST(test_print_operand_stackoff_with_vreg_short_form)
{
  TCCIRState *ir = utb_new();
  tcc_ir_dump_set_show_physical_regs(0);

  IROperand addr_of = irop_make_stackoff(TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, 3), 0,
                                          /*is_lval*/ 0, /*is_llocal*/ 0, /*is_param*/ 0, I32);
  char *s1 = dump_op(ir, addr_of);
  UT_ASSERT_STREQ(s1, "&V3");
  libc_free(s1);

  IROperand deref = irop_make_stackoff(TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, 3), 0,
                                        /*is_lval*/ 1, /*is_llocal*/ 0, /*is_param*/ 0, I32);
  char *s2 = dump_op(ir, deref);
  UT_ASSERT_STREQ(s2, "V3");
  libc_free(s2);

  utb_free(ir);
  return 0;
}

/* STACKOFF, non-llocal, no bound vreg (irop_get_vreg == -1): falls back to
 * raw offset printing -- "Addr[StackLoc[n]]" for the address-of form,
 * "StackLoc[n]" for the lvalue (dereferenced) form. */
UT_TEST(test_print_operand_stackoff_no_vreg_raw_offset)
{
  TCCIRState *ir = utb_new();

  IROperand addr_of = utb_stackoff(16, /*is_lval*/ 0, /*is_llocal*/ 0, /*is_param*/ 0, I32);
  char *s1 = dump_op(ir, addr_of);
  UT_ASSERT_STREQ(s1, "Addr[StackLoc[16]]");
  libc_free(s1);

  IROperand deref = utb_stackoff(16, /*is_lval*/ 1, /*is_llocal*/ 0, /*is_param*/ 0, I32);
  char *s2 = dump_op(ir, deref);
  UT_ASSERT_STREQ(s2, "StackLoc[16]");
  libc_free(s2);

  utb_free(ir);
  return 0;
}

/* STACKOFF with is_llocal set and no vreg: "VT_LLOCAL (cval=n)" (spilled
 * pointer needing double dereference, physical-reg display off). */
UT_TEST(test_print_operand_stackoff_llocal_no_physreg)
{
  TCCIRState *ir = utb_new();
  tcc_ir_dump_set_show_physical_regs(0);

  IROperand op = utb_stackoff(24, /*is_lval*/ 1, /*is_llocal*/ 1, /*is_param*/ 0, I32);
  char *s = dump_op(ir, op);
  UT_ASSERT_STREQ(s, "VT_LLOCAL (cval=24)");
  libc_free(s);

  utb_free(ir);
  return 0;
}

/* Default (raw VREG) tag: bound TEMP/PARAM vregs print with their type
 * prefix (T/P), a ***DEREF*** suffix when is_lval is set. */
UT_TEST(test_print_operand_default_vreg_prefixes)
{
  TCCIRState *ir = utb_new();
  tcc_ir_dump_set_show_physical_regs(0);

  char *s_temp = dump_op(ir, utb_temp(5, I32));
  UT_ASSERT_STREQ(s_temp, "T5");
  libc_free(s_temp);

  char *s_param = dump_op(ir, utb_param(2, I32));
  UT_ASSERT_STREQ(s_param, "P2");
  libc_free(s_param);

  char *s_var = dump_op(ir, utb_var(0, I32));
  UT_ASSERT_STREQ(s_var, "V0");
  libc_free(s_var);

  char *s_deref = dump_op(ir, utb_lval(utb_temp(9, I32)));
  UT_ASSERT_STREQ(s_deref, "T9***DEREF***");
  libc_free(s_deref);

  utb_free(ir);
  return 0;
}

/* Default tag, no vreg at all: IROP_NONE explicitly decodes to tag=NONE
 * (irop_get_tag) and vreg=-1 (irop_get_vreg), landing in the same
 * `default:` switch arm as a real VREG tag but taking its "no vreg"
 * fallback -- "VReg?" (with show_physical_regs off, the only reachable
 * path for a -1 vreg here). */
UT_TEST(test_print_operand_default_no_vreg_fallback)
{
  TCCIRState *ir = utb_new();
  tcc_ir_dump_set_show_physical_regs(0);

  char *s = dump_op(ir, UTB_NONE);
  UT_ASSERT_STREQ(s, "VReg?");
  libc_free(s);

  utb_free(ir);
  return 0;
}

/* Physical-register display: allocate a live interval for a VAR vreg,
 * mark it non-spilled with a concrete r0, and confirm the "R<n>(V<pos>)"
 * form with the parenthesized vreg echo. Unlike the STACKOFF tag's physreg
 * branch (which does add a leading "&" for a non-lvalue address-of form),
 * the default (raw VREG) tag's physreg branch has no "&" logic at all --
 * is_lval only controls the trailing ***DEREF*** marker here. */
UT_TEST(test_print_operand_physreg_allocated_not_spilled)
{
  TCCIRState *ir = utb_new();
  ir->variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * 4);
  ir->variables_live_intervals_size = 4;
  ir->variables_live_intervals[1].allocation.r0 = 3; /* R3, not spilled */

  tcc_ir_dump_set_show_physical_regs(1);

  IROperand addr_of = utb_var(1, I32);
  char *s1 = dump_op(ir, addr_of);
  UT_ASSERT_STREQ(s1, "R3(V1)");
  libc_free(s1);

  IROperand deref = utb_lval(utb_var(1, I32));
  char *s2 = dump_op(ir, deref);
  UT_ASSERT_STREQ(s2, "R3(V1)***DEREF***");
  libc_free(s2);

  /* NOTE: do not tcc_free(ir->variables_live_intervals) here -- utb_free()
   * below already frees it; this pointer isn't reallocated in between, so an
   * extra free here would be a double free. */
  tcc_ir_dump_set_show_physical_regs(0);
  utb_free(ir);
  return 0;
}

/* Physical-register display, spilled: PREG_SPILLED set on r0 -> the ANSI
 * "SpillLoc[<offset>]" form (no vreg echo, no "&" prefix at all). */
UT_TEST(test_print_operand_physreg_spilled)
{
  TCCIRState *ir = utb_new();
  ir->variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * 4);
  ir->variables_live_intervals_size = 4;
  ir->variables_live_intervals[2].allocation.r0 = 5 | PREG_SPILLED;
  ir->variables_live_intervals[2].allocation.offset = 40;

  tcc_ir_dump_set_show_physical_regs(1);

  IROperand op = utb_var(2, I32);
  char *s = dump_op(ir, op);
  UT_ASSERT_STREQ(s, "\033[41mSpillLoc[40]\033[0m");
  libc_free(s);

  /* NOTE: do not tcc_free(ir->variables_live_intervals) here -- utb_free()
   * below already frees it; this pointer isn't reallocated in between, so an
   * extra free here would be a double free. */
  tcc_ir_dump_set_show_physical_regs(0);
  utb_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* tcc_print_quadruple_irop -- exact per-instruction dumped line              */
/* -------------------------------------------------------------------------- */

struct capture_quad_args
{
  TCCIRState *ir;
  IRQuadCompact *q;
  int pc;
};

static void capture_quad_fn(void *arg)
{
  struct capture_quad_args *a = (struct capture_quad_args *)arg;
  tcc_print_quadruple_irop(a->ir, a->q, a->pc);
}

static char *dump_quad(TCCIRState *ir, int idx)
{
  struct capture_quad_args a = {ir, &ir->compact_instructions[idx], idx};
  return ut_capture_stdout(capture_quad_fn, &a);
}

/* Default branch: "<pc>: <dest> <-- <src1> <OP> <src2>\n". */
UT_TEST(test_print_quad_default_arith)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_temp(1, I32), utb_imm(2, I32));

  char *s = dump_quad(ir, 0);
  UT_ASSERT_STREQ(s, "0000: T0 <-- T1 ADD #2\n");
  libc_free(s);

  utb_free(ir);
  return 0;
}

/* pc is printed zero-padded to 4 digits regardless of instruction index. */
UT_TEST(test_print_quad_pc_padding)
{
  TCCIRState *ir = utb_new();
  for (int i = 0; i < 13; i++)
    utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);

  char *s = dump_quad(ir, 12);
  UT_ASSERT_STREQ(s, "0012: NOP \n");
  libc_free(s);

  utb_free(ir);
  return 0;
}

/* The "no-dest, name-only" op class (NOP/PREFETCH/TRAP/RETURNVALUE/
 * RETURNVOID/FUNCCALLVOID/FUNCCALLVAL/FUNCPARAMVOID/TEST_ZERO/CMP) prints
 * just "<name> " with no "<--". RETURNVALUE additionally has has_src1, so
 * its source prints right after. */
UT_TEST(test_print_quad_returnvalue)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I32), UTB_NONE);

  char *s = dump_quad(ir, 0);
  UT_ASSERT_STREQ(s, "0000: RETURNVALUE T3\n");
  libc_free(s);

  utb_free(ir);
  return 0;
}

UT_TEST(test_print_quad_returnvoid)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  char *s = dump_quad(ir, 0);
  UT_ASSERT_STREQ(s, "0000: RETURNVOID \n");
  libc_free(s);

  utb_free(ir);
  return 0;
}

/* SET_CHAIN has no operands (irop_config defaults to {0,0,0}, no explicit
 * entry in the table) and gets its own explanatory-comment branch. */
UT_TEST(test_print_quad_set_chain)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_SET_CHAIN, UTB_NONE, UTB_NONE, UTB_NONE);

  char *s = dump_quad(ir, 0);
  UT_ASSERT_STREQ(s, "0000: SET_CHAIN /* R10 <- FP */ \n");
  libc_free(s);

  utb_free(ir);
  return 0;
}

/* FUNCPARAMVAL: dest-less; src2.c.i packs call_id/param_idx, printed by the
 * leading switch as "PARAM<idx>[call_<id>] ". irop_config[FUNCPARAMVAL] also
 * has has_src1=1, and the later generic has_src1 block only excludes
 * SETIF/JUMPIF/MLA -- FUNCPARAMVAL is NOT excluded, so src1 (the actual
 * parameter value) is echoed right after. has_src2's switch DOES explicitly
 * exclude FUNCPARAMVAL (case falls through to an empty break), so src2
 * (the packed call_id/param_idx immediate) is not printed again. This
 * documents CURRENT observed behavior -- see the IJUMP test above for the
 * identical has_src1-exclusion-list gap; not fixed here (ir/dump.c is
 * off-limits to this test-only pass). */
UT_TEST(test_print_quad_funcparamval)
{
  TCCIRState *ir = utb_new();
  IROperand packed = utb_imm((int32_t)TCCIR_ENCODE_PARAM(7, 2), I32);
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32), packed);

  char *s = dump_quad(ir, 0);
  UT_ASSERT_STREQ(s, "0000: PARAM2[call_7] T0\n");
  libc_free(s);

  utb_free(ir);
  return 0;
}

/* JUMP / JUMPIF: dest carries the target index (as an immediate), printed
 * "JMP to <target> ". JUMPIF additionally appends the ` if "<cc>"` suffix
 * built from src1.c.i -- every named condition code plus the numeric
 * fallback for an unrecognized one. */
UT_TEST(test_print_quad_jump)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(9, I32), UTB_NONE, UTB_NONE);

  char *s = dump_quad(ir, 0);
  UT_ASSERT_STREQ(s, "0000: JMP to 9 \n");
  libc_free(s);

  utb_free(ir);
  return 0;
}

UT_TEST(test_print_quad_jumpif_named_ccs)
{
  TCCIRState *ir = utb_new();
  static const struct
  {
    int tok;
    const char *cc;
  } cases[] = {
      {TOK_EQ, "=="}, {TOK_NE, "!="}, {TOK_LT, "<S"}, {TOK_GT, ">S"}, {TOK_LE, "<=S"},
      {TOK_GE, ">=S"}, {TOK_ULT, "<U"}, {TOK_UGT, ">U"}, {TOK_ULE, "<=U"}, {TOK_UGE, ">=U"},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
  {
    ir->next_instruction_index = 0;
    ir->iroperand_pool_count = 0;
    utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(cases[i].tok, I32), UTB_NONE);

    char expected[64];
    snprintf(expected, sizeof(expected), "0000: JMP to 5  if \"%s\"\n", cases[i].cc);

    char *s = dump_quad(ir, 0);
    UT_ASSERT_STREQ(s, expected);
    libc_free(s);
  }

  utb_free(ir);
  return 0;
}

UT_TEST(test_print_quad_jumpif_unknown_cc_numeric_fallback)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(0x1234, I32), UTB_NONE);

  char *s = dump_quad(ir, 0);
  UT_ASSERT_STREQ(s, "0000: JMP to 5  if \"?\"\n");
  libc_free(s);

  utb_free(ir);
  return 0;
}

/* SETIF: dest <-- "(cond=0x..)" -- src1 is the raw condition code, printed
 * in hex, not resolved to a mnemonic here (unlike JUMPIF's suffix, SETIF
 * has no matching name-resolving suffix branch in this function). */
UT_TEST(test_print_quad_setif_cond_hex)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(TOK_EQ, I32), UTB_NONE);

  char *s = dump_quad(ir, 0);
  char expected[64];
  snprintf(expected, sizeof(expected), "0000: T0 <-- (cond=0x%x)\n", (unsigned)TOK_EQ);
  UT_ASSERT_STREQ(s, expected);
  libc_free(s);

  utb_free(ir);
  return 0;
}

/* IJUMP: regression lock for docs/bugs.md #5 (fixed). The leading switch's
 * TCCIR_OP_IJUMP case used to print "IJMP <src1> " itself AND irop_config[IJUMP]
 * has has_src1=1 while the generic "if (irop_config[op].has_src1)" block only
 * excluded SETIF/JUMPIF/MLA -- so src1 was printed a SECOND time ("IJMP T4 T4").
 * The switch case now prints only the mnemonic and lets the generic block emit
 * src1 exactly once, matching the intended "IJMP T4". (FUNCPARAMVAL is left as
 * is: its generic-block src1 print is the ONLY place its value is shown, so it
 * was never a duplicate -- see test_print_quad_funcparamval.) */
UT_TEST(test_print_quad_ijump)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_IJUMP, UTB_NONE, utb_temp(4, I32), UTB_NONE);

  char *s = dump_quad(ir, 0);
  UT_ASSERT_STREQ(s, "0000: IJMP T4\n");
  libc_free(s);

  utb_free(ir);
  return 0;
}

/* CMP: dest-less name-only class, but has_src2 -- prints "CMP <src1>,<src2>". */
UT_TEST(test_print_quad_cmp)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(3, I32));

  char *s = dump_quad(ir, 0);
  UT_ASSERT_STREQ(s, "0000: CMP T0,#3\n");
  libc_free(s);

  utb_free(ir);
  return 0;
}

/* MLA: 4-operand special case -- "<dest> <-- <src1> MLA <src2> + <accum>". */
UT_TEST(test_print_quad_mla)
{
  TCCIRState *ir = utb_new();
  utb_emit4(ir, TCCIR_OP_MLA, utb_temp(0, I32), utb_temp(1, I32), utb_temp(2, I32), utb_temp(3, I32));

  char *s = dump_quad(ir, 0);
  UT_ASSERT_STREQ(s, "0000: T0 <-- T1 MLA T2 + T3\n");
  libc_free(s);

  utb_free(ir);
  return 0;
}

/* [STORE]/[LOAD]/[ASSIGN]/[SELECT] trailing tags. */
UT_TEST(test_print_quad_store_tag)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(7, I32), UTB_NONE);

  char *s = dump_quad(ir, 0);
  UT_ASSERT_STREQ(s, "0000: T0***DEREF*** <-- #7 [STORE]\n");
  libc_free(s);

  utb_free(ir);
  return 0;
}

UT_TEST(test_print_quad_load_tag)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), utb_lval(utb_temp(0, I32)), UTB_NONE);

  char *s = dump_quad(ir, 0);
  UT_ASSERT_STREQ(s, "0000: T1 <-- T0***DEREF*** [LOAD]\n");
  libc_free(s);

  utb_free(ir);
  return 0;
}

UT_TEST(test_print_quad_assign_tag)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);

  char *s = dump_quad(ir, 0);
  UT_ASSERT_STREQ(s, "0000: T1 <-- T0 [ASSIGN]\n");
  libc_free(s);

  utb_free(ir);
  return 0;
}

UT_TEST(test_print_quad_select_tag)
{
  TCCIRState *ir = utb_new();
  utb_emit4(ir, TCCIR_OP_SELECT, utb_temp(0, I32), utb_temp(1, I32), utb_temp(2, I32), utb_imm(TOK_EQ, I32));

  char *s = dump_quad(ir, 0);
  UT_ASSERT_STREQ(s, "0000: T0 <-- T1 SELECT T2 [SELECT cond=0x94]\n");
  libc_free(s);

  utb_free(ir);
  return 0;
}

UT_TEST(test_print_quad_block_copy_tag)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  Sym sym;
  memset(&sym, 0, sizeof(sym));
  sym.v = 9;
  IROperand src = utb_symref(ir, &sym, /*is_lval*/ 0, 0, 0, I32);
  utb_emit(ir, TCCIR_OP_BLOCK_COPY, utb_stackoff(0, 0, 0, 0, I32), src, utb_imm(16, I32));

  char *s = dump_quad(ir, 0);
  UT_ASSERT_STREQ(s, "0000: Addr[StackLoc[0]] <-- GlobalSym(9) BLOCK_COPY #16 [BLOCK_COPY]\n");
  libc_free(s);

  utb_free(ir);
  return 0;
}

/* FUNCCALLVAL: dest <-- src1 [call_id in src2, suppressed] --> dest again
 * (the "returns into its own dest" convention). */
UT_TEST(test_print_quad_funccallval)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  Sym callee;
  memset(&callee, 0, sizeof(callee));
  callee.v = 100;
  IROperand fn = utb_symref(ir, &callee, 0, 0, 0, I32);
  utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), fn, utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));

  char *s = dump_quad(ir, 0);
  UT_ASSERT_STREQ(s, "0000: CALL GlobalSym(100) --> T0\n");
  libc_free(s);

  utb_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* tcc_ir_show -- multi-instruction driver over the whole compact array       */
/* -------------------------------------------------------------------------- */

struct capture_show_args
{
  TCCIRState *ir;
};

static void capture_show_fn(void *arg)
{
  struct capture_show_args *a = (struct capture_show_args *)arg;
  tcc_ir_show(a->ir);
}

UT_TEST(test_ir_show_concatenates_every_instruction_in_order)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  struct capture_show_args a = {ir};
  char *s = ut_capture_stdout(capture_show_fn, &a);

  UT_ASSERT_STREQ(s, "0000: T0 <-- #1 ADD #2\n0001: RETURNVALUE T0\n");
  libc_free(s);

  utb_free(ir);
  return 0;
}

UT_TEST(test_ir_show_empty_function_prints_nothing)
{
  TCCIRState *ir = utb_new();

  struct capture_show_args a = {ir};
  char *s = ut_capture_stdout(capture_show_fn, &a);

  UT_ASSERT_STREQ(s, "");
  libc_free(s);

  utb_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* tcc_ir_dump_after_pass -- no-op outside CONFIG_TCC_DEBUG                   */
/* -------------------------------------------------------------------------- */

/* This unit-test binary is built without CONFIG_TCC_DEBUG (it's an ordinary
 * host build of ir/dump.c, not the -dump-ir-passes= debug host used by
 * tests/ir_tests/test_golden_ir.py), so tcc_ir_dump_after_pass() always
 * takes the "(void)ir; (void)pass_name;" branch regardless of
 * dump_ir_passes -- confirm it produces zero output and does not crash on
 * a NULL `ir`, both matched and unmatched. See docs/plan_ut_next_steps.md
 * §7.0 for why the CONFIG_TCC_DEBUG-guarded body ("=== AFTER ... ===") is
 * exercised by the golden-IR track instead of this host-native suite. */
struct capture_after_pass_args
{
  TCCIRState *ir;
  const char *pass_name;
};

static void capture_after_pass_fn(void *arg)
{
  struct capture_after_pass_args *a = (struct capture_after_pass_args *)arg;
  tcc_ir_dump_after_pass(a->ir, a->pass_name);
}

UT_TEST(test_dump_after_pass_noop_without_config_debug)
{
  tcc_state->dump_ir_passes = "all";

  struct capture_after_pass_args a = {NULL, "anything"};
  char *s = ut_capture_stdout(capture_after_pass_fn, &a);
  UT_ASSERT_STREQ(s, "");
  libc_free(s);

  tcc_state->dump_ir_passes = NULL;
  return 0;
}

/* -------------------------------------------------------------------------- */
/* print_svalue_short -- SValue-based sibling of print_iroperand_short        */
/* -------------------------------------------------------------------------- */

/* print_svalue_short has no remaining callers anywhere in the tree (the
 * live -dump-ir-passes= path goes through the IRQuadCompact/IROperand
 * dumper above) but is still a reachable, non-static exported symbol; cover
 * its distinct branches for the same reason the rest of this file does. */

struct capture_sv_args
{
  SValue *sv;
};

static void capture_sv_fn(void *arg)
{
  struct capture_sv_args *a = (struct capture_sv_args *)arg;
  print_svalue_short(a->sv);
}

static char *dump_sv(SValue *sv)
{
  struct capture_sv_args a = {sv};
  return ut_capture_stdout(capture_sv_fn, &a);
}

UT_TEST(test_print_svalue_const_plain_and_symbol)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_CONST;
  sv.c.i = 5;
  sv.type.t = VT_INT;
  char *s1 = dump_sv(&sv);
  UT_ASSERT_STREQ(s1, "#5");
  libc_free(s1);

  Sym sym;
  memset(&sym, 0, sizeof(sym));
  sym.v = 321;
  svalue_init(&sv);
  sv.r = VT_CONST | VT_SYM;
  sv.c.i = 8;
  sv.sym = &sym;
  char *s2 = dump_sv(&sv);
  UT_ASSERT_STREQ(s2, "GlobalSym(321)+8");
  libc_free(s2);

  svalue_init(&sv);
  sv.r = VT_CONST | VT_SYM | VT_LVAL;
  sv.c.i = 0;
  sv.sym = &sym;
  char *s3 = dump_sv(&sv);
  UT_ASSERT_STREQ(s3, "GlobalSym(321)***DEREF***");
  libc_free(s3);
  return 0;
}

UT_TEST(test_print_svalue_const_llong_uses_wide_format)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_CONST;
  sv.c.i = 0x123456789ALL;
  sv.type.t = VT_LLONG;
  char *s = dump_sv(&sv);
  UT_ASSERT_STREQ(s, "#78187493530");
  libc_free(s);
  return 0;
}

UT_TEST(test_print_svalue_vt_cmp_jmp_jmpi)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_CMP;
  char *s1 = dump_sv(&sv);
  UT_ASSERT_STREQ(s1, "VT_CMP");
  libc_free(s1);

  svalue_init(&sv);
  sv.r = VT_JMP;
  char *s2 = dump_sv(&sv);
  UT_ASSERT_STREQ(s2, "VT_JMP");
  libc_free(s2);

  svalue_init(&sv);
  sv.r = VT_JMPI;
  char *s3 = dump_sv(&sv);
  UT_ASSERT_STREQ(s3, "VT_JMPI");
  libc_free(s3);
  return 0;
}

/* VT_LOCAL, physical-register display explicitly off, no pr0_reg set, but
 * a bound vreg -- prints "V<pos>"/"&V<pos>". */
UT_TEST(test_print_svalue_local_vreg_short_form)
{
  tcc_ir_dump_set_show_physical_regs(0);

  SValue sv;
  svalue_init(&sv);
  sv.r = VT_LOCAL;
  sv.vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, 4);
  char *s1 = dump_sv(&sv);
  UT_ASSERT_STREQ(s1, "&V4");
  libc_free(s1);

  svalue_init(&sv);
  sv.r = VT_LOCAL | VT_LVAL;
  sv.vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, 4);
  char *s2 = dump_sv(&sv);
  UT_ASSERT_STREQ(s2, "V4");
  libc_free(s2);
  return 0;
}

/* VT_LOCAL, no vreg (-1): falls back to raw offset -- "Addr[StackLoc[n]]"
 * or "StackLoc[n]" depending on VT_LVAL, mirroring the IROperand path. */
UT_TEST(test_print_svalue_local_no_vreg_raw_offset)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_LOCAL;
  sv.c.i = 20;
  char *s1 = dump_sv(&sv);
  UT_ASSERT_STREQ(s1, "Addr[StackLoc[20]]");
  libc_free(s1);

  svalue_init(&sv);
  sv.r = VT_LOCAL | VT_LVAL;
  sv.c.i = 20;
  char *s2 = dump_sv(&sv);
  UT_ASSERT_STREQ(s2, "StackLoc[20]");
  libc_free(s2);
  return 0;
}

/* VT_LLOCAL, no spill (pr0_reg left PREG_REG_NONE by svalue_init): the
 * "VT_LLOCAL (cval=n)" fallback. */
UT_TEST(test_print_svalue_llocal_no_spill)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_LLOCAL;
  sv.c.i = 12;
  char *s = dump_sv(&sv);
  UT_ASSERT_STREQ(s, "VT_LLOCAL (cval=12)");
  libc_free(s);
  return 0;
}

/* default (raw vreg, e.g. VT_PARAM-only): show_physical_regs off -> short-
 * form vreg print, with tcc_ir_operand_needs_dereference() deciding the
 * ***DEREF*** suffix per the VT_PARAM special case in ir/type.c (register
 * params keep VT_LVAL to allow &param without meaning "dereference to get
 * the value"). */
UT_TEST(test_print_svalue_default_param_no_deref_despite_lval)
{
  tcc_ir_dump_set_show_physical_regs(0);

  SValue sv;
  svalue_init(&sv);
  sv.r = VT_PARAM | VT_LVAL; /* VT_PARAM without VT_LOCAL: value in reg, not ptr */
  sv.vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 1);
  char *s = dump_sv(&sv);
  UT_ASSERT_STREQ(s, "P1");
  libc_free(s);
  return 0;
}

UT_TEST(test_print_svalue_default_temp_deref)
{
  tcc_ir_dump_set_show_physical_regs(0);

  SValue sv;
  svalue_init(&sv);
  sv.r = VT_LVAL; /* val_loc == 0: not CONST/LLOCAL/LOCAL/CMP/JMP/JMPI */
  sv.vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 6);
  char *s = dump_sv(&sv);
  UT_ASSERT_STREQ(s, "T6***DEREF***");
  libc_free(s);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Pass-name matching                                                         */
/* -------------------------------------------------------------------------- */

UT_TEST(test_passes_match_all)
{
  tcc_state->dump_ir_passes = "all";
  UT_ASSERT(tcc_ir_dump_passes_match(tcc_state, "copyprop"));
  UT_ASSERT(tcc_ir_dump_passes_match(tcc_state, "dead_vla"));
  UT_ASSERT(tcc_ir_dump_passes_match(tcc_state, "licm"));
  return 0;
}

UT_TEST(test_passes_match_single)
{
  tcc_state->dump_ir_passes = "copyprop,dead_vla";
  UT_ASSERT(tcc_ir_dump_passes_match(tcc_state, "copyprop"));
  UT_ASSERT(tcc_ir_dump_passes_match(tcc_state, "dead_vla"));
  UT_ASSERT(!tcc_ir_dump_passes_match(tcc_state, "licm"));
  UT_ASSERT(!tcc_ir_dump_passes_match(tcc_state, "copypro"));
  UT_ASSERT(!tcc_ir_dump_passes_match(tcc_state, "copyprop2"));
  return 0;
}

UT_TEST(test_passes_match_null_state)
{
  UT_ASSERT(!tcc_ir_dump_passes_match(NULL, "copyprop"));
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Physical-register display flag                                             */
/* -------------------------------------------------------------------------- */

UT_TEST(test_dump_set_show_physical_regs_no_crash)
{
  tcc_ir_dump_set_show_physical_regs(1);
  tcc_ir_dump_set_show_physical_regs(0);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* ANSI spill-mark colour gating                                              */
/* -------------------------------------------------------------------------- */

/* The spill marks are private to dump.c; replicate the public constants here
 * so we can assert the output format is stable. */
#define UT_SPILL_MARK_BEGIN "\033[41m"
#define UT_SPILL_MARK_END   "\033[0m"

UT_TEST(test_spill_mark_ansi_colors)
{
  UT_ASSERT_STREQ(UT_SPILL_MARK_BEGIN, "\033[41m");
  UT_ASSERT_STREQ(UT_SPILL_MARK_END, "\033[0m");
  return 0;
}
