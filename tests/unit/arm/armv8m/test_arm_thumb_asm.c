/*
 *  test_arm_thumb_asm.c - suite for arm-thumb-asm.c (inline-asm parser/assembler)
 *
 *  arm-thumb-asm.c implements the GNU inline-asm (`__asm__`) block parser
 *  and per-mnemonic Thumb-2 encoder dispatch for the ARMv8-M backend. Most
 *  of the file (asm_opcode() and everything it reaches through
 *  process_operands()/parse_operand()) is driven by the real tokenizer
 *  (tok/next()/skip()/expect() from tccpp.c, asm_expr() from tccasm.c,
 *  section machinery from tccelf.c) — none of which are linked into this
 *  unit-test binary (see UT_COVERAGE_ONLY_SRCS in the Makefile). Building a
 *  standalone lexer/section stub sufficient to drive asm_opcode() end to end
 *  is out of scope here; this suite instead covers the *parser-context-free*
 *  slice of the file that is reachable directly at unit-test level:
 *
 *    - asm_parse_regvar(): token -> physical register number
 *    - thumb_parse_special_register() / thumb_parse_special_register_mask():
 *      MRS/MSR special-register name string -> SYSm / mask encoding
 *    - thumb_parse_token_suffix(): "addeq.w"-style token string ->
 *      (condition code, base-mnemonic token). This is the one `static`
 *      helper (get_base_instruction_name()) we get branch coverage on
 *      indirectly, since thumb_parse_token_suffix() calls it directly and
 *      is itself `ST_FUNC`/linkable; get_base_instruction_name() and its
 *      sibling parse_asm_suffix() cannot be called *directly* from this TU
 *      (both `static`), and parse_asm_suffix() has no caller at all in
 *      arm-thumb-asm.c (dead code, superseded by thumb_parse_token_suffix()).
 *    - thumb_generate_opcode_for_data_processing() /
 *      thumb_process_generic_data_op(): the per-mnemonic ALU dispatch table.
 *      These two take a pre-parsed `Operand ops[3]` array and an opcode
 *      token — no lexer needed. `Operand` has no header (it is a private
 *      type defined inside arm-thumb-asm.c), so this file mirrors its exact
 *      layout (enum + struct) to stay ABI-compatible; a mismatch here would
 *      fail loudly (wrong register/immediate decoded) rather than silently.
 *      Oracle: for each dispatch case we independently call the same
 *      th_<mnemonic>_* encoder this dispatcher is documented (by the
 *      switch's own case body) to route to, and assert the two
 *      thumb_opcode results are bit-for-bit identical. That way the test
 *      pins the *routing* (register/immediate operand mapping, flags
 *      behaviour, SP special-casing, ...) without duplicating the
 *      hex-opcode oracles that test_thop_alu_imm.c/test_thop_alu_reg.c/etc.
 *      already own.
 *
 *  NOT covered here (confirmed infeasible at unit-test granularity without
 *  substantial new stub machinery -- see docs/plan_codegen_unit_tests.md's
 *  "confirmed genuinely hard" bar):
 *    - asm_opcode() itself and every static thumb_*_opcode(s1, token) mnemonic
 *      handler (thumb_adr_opcode, thumb_single_memory_transfer_opcode, ...):
 *      all call process_operands()/parse_operand(), which need the real
 *      tok/next()/skip() token stream.
 *    - subst_asm_operand(): needs tok_alloc() (tccpp.c, not linked) on the
 *      anonymous-symbol path.
 *    - asm_clobber(): needs tok_alloc() (tccpp.c, not linked).
 *    - asm_compute_constraints() / asm_gen_code(): the "reference to another
 *      operand" constraint path (numeric or `[name]` constraints) calls
 *      find_constraint() (tccasm.c, not linked, and itself needs tok_alloc()
 *      on the `[name]` sub-path); asm_gen_code()'s VT_LLOCAL/memory-operand
 *      path calls svalue_to_iroperand()/machine_op_from_ir() (need a real
 *      TCCIRState). Both are excluded rather than deep-stubbed.
 *    - g()/gen_le16()/gen_le32()/gen_expr32(): trivially no-ops under
 *      nocode_wanted=1 at the *source* level, but the compiled function body
 *      still references tcc_gen_machine_dry_run_is_active()/section_realloc()
 *      (arm-thumb-gen.c / tccelf.c, not linked) on the taken-at-link-time
 *      side of the branch, so merely calling g() from this TU drags in
 *      symbols this harness doesn't provide.
 *    - thumb_parse_condition_str()/thumb_build_it_mask()/thumb_conditional_opcode()
 *      and the width/condition-suffix helpers (parse_asm_suffix(),
 *      get_base_instruction_name()) are all `static` -- not linkable from
 *      another TU, and thumb_conditional_opcode (the only public-ish path to
 *      thumb_parse_condition_str) itself needs next()/tok.
 *
 *  One small stub was added to fix a link error surfaced by exercising
 *  thumb_generate_opcode_for_data_processing()'s clz/bfc operand-validation
 *  paths: `expect()` in stubs.c (mirrors the existing _tcc_error stub).
 */

#define USING_GLOBALS
#include "arch/arm/thumb/thop_alu_imm.h"
#include "arch/arm/thumb/thop_alu_reg.h"
#include "arch/arm/thumb/thop_bitfield.h"
#include "arch/arm/thumb/thop_cmp.h"
#include "arch/arm/thumb/thop_dsp.h"
#include "arch/arm/thumb/thop_extend.h"
#include "arch/arm/thumb/thop_mov.h"
#include "arch/arm/thumb/thop_mul.h"
#include "arch/arm/thumb/thop_mvn.h"
#include "arch/arm/thumb/thop_rev.h"
#include "arch/arm/thumb/thop_system.h"
#include "arch/arm/thumb/thumb.h"
#include "tcc.h"

#include "ut.h"

/* ------------------------------------------------------------------ setup */

/* Test-harness hook implemented in stubs.c: populates get_tok_str()'s
 * settable token->name table. */
void utb_set_tok_str(int tok, const char *name);

static void setup_armv8m_main(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m33",
      .feat = (thop_feat){
          .t16 = 1,
          .t32 = 1,
          .it = 1,
          .mod_imm = 1,
          .movw_movt = 1,
          .bfx = 1,
          .clz_rbit = 1,
          .tbb_tbh = 1,
          .cbz = 1,
          .sat = 1,
          .div = 1,
          .dsp = 1,
          .ldaex = 1,
      },
      .is_secure_tz = false,
  };

  /* thumb_generate_opcode_for_data_processing() reads a WIDE/NARROW width
     qualifier from module-static `current_asm_suffix` state (set by
     thumb_parse_token_suffix() -- see THUMB_HAS_WIDE_QUALIFIER_FROM_STATE()).
     That state is process-global and outlives any single UT_TEST, so the
     token-suffix tests above (e.g. "subne.w") would otherwise leak a
     WIDTH_WIDE qualifier into every dispatch test that runs afterward in the
     same binary. thumb_parse_token_suffix() itself unconditionally resets
     the width to WIDTH_NONE before parsing, so calling it here with a
     bare/no-suffix token forces every dispatch test back to a known
     "no .w/.n qualifier" baseline regardless of suite run order. */
  int reset_base_token;
  utb_set_tok_str(399, "nop");
  thumb_parse_token_suffix(399, &reset_base_token);
}

/* Private Operand type mirror -- see file-header comment. Layout copied
 * verbatim from arm-thumb-asm.c (enum OPT_* + #define OP_* + struct Operand). */
enum
{
  UT_OPT_REG32,
  UT_OPT_REGSET32,
  UT_OPT_IM8,
  UT_OPT_IM8N,
  UT_OPT_IM32,
  UT_OPT_VREG32,
  UT_OPT_VREG64,
};
#define UT_OP_REG32 (1 << UT_OPT_REG32)
#define UT_OP_IM32 (1 << UT_OPT_IM32)

typedef struct Operand
{
  uint32_t type;
  union
  {
    uint8_t reg;
    uint32_t regset;
    ExprValue e;
  };
} Operand;

/* thumb_generate_opcode_for_data_processing() has no header declaration
 * (Operand is private to arm-thumb-asm.c), so forward-declare it here. */
thumb_opcode thumb_generate_opcode_for_data_processing(int token, thumb_shift shift, Operand *ops);

/* thumb_process_generic_data_op() is the helper shared by many data-processing
 * switch cases in arm-thumb-asm.c; expose it directly to test the width/encoding
 * and flags branches without going through the full dispatcher. */
typedef thumb_opcode (*thumb_generate_generic_imm_opcode)(uint32_t rd, uint32_t rn, uint32_t imm,
                                                          thumb_flags_behaviour flags, thumb_enforce_encoding encoding);
typedef thumb_opcode (*thumb_generate_generic_reg_opcode)(uint32_t rd, uint32_t rn, uint32_t rm,
                                                          thumb_flags_behaviour flags, thumb_shift shift,
                                                          thumb_enforce_encoding encoding);
typedef struct th_generic_op_data
{
  thumb_generate_generic_imm_opcode generate_imm_opcode;
  thumb_generate_generic_reg_opcode generate_reg_opcode;
  int regular_variant_token;
  int flags_variant_token;
} th_generic_op_data;

thumb_opcode thumb_process_generic_data_op(th_generic_op_data data, int token, thumb_shift shift, Operand *ops);

/* Other arm-thumb-asm.c helpers that are public (ST_FUNC) but have no
 * externally visible header; exercise them directly below. */
ST_FUNC void tcc_asm_set_fpu(const char *name);
ST_FUNC void asm_clobber(uint8_t *clobber_regs, const char *str);
ST_FUNC void asm_compute_constraints(ASMOperand *operands, int nb_operands, int nb_outputs,
                                     const uint8_t *clobber_regs, const uint8_t *reserved_regs,
                                     int *pout_reg);

/* arm-thumb-asm.c's asm_compute_constraints() references find_constraint() from
 * tccasm.c, which is not linked into the main unit-test binary. Provide a weak
 * stub so the reference resolves when tccasm.c is absent, while a real
 * tccasm.c link (coverage build) overrides it with the strong definition. The
 * tests below avoid the numeric/[name] reference path that would call this. */
__attribute__((weak)) int find_constraint(ASMOperand *operands, int nb_operands, const char *name,
                                          const char **pp)
{
  (void)operands;
  (void)nb_operands;
  (void)name;
  (void)pp;
  return -1;
}

/* Build a 3-way (rd, rn, imm) operand array the way thumb_data_processing_opcode()
 * would for a 3-operand form (e.g. "add r0, r1, #42"). */
static void ops_reg_reg_imm(Operand ops[3], uint8_t rd, uint8_t rn, uint32_t imm)
{
  memset(ops, 0, 3 * sizeof(ops[0]));
  ops[0].type = UT_OP_REG32;
  ops[0].reg = rd;
  ops[1].type = UT_OP_REG32;
  ops[1].reg = rn;
  ops[2].type = UT_OP_IM32;
  ops[2].e.v = imm;
}

/* Build a 3-way (rd, rn, rm) register-only operand array. */
static void ops_reg_reg_reg(Operand ops[3], uint8_t rd, uint8_t rn, uint8_t rm)
{
  memset(ops, 0, 3 * sizeof(ops[0]));
  ops[0].type = UT_OP_REG32;
  ops[0].reg = rd;
  ops[1].type = UT_OP_REG32;
  ops[1].reg = rn;
  ops[2].type = UT_OP_REG32;
  ops[2].reg = rm;
}

/* Build the 2-operand-instruction shape thumb_data_processing_opcode() feeds
 * the dispatcher for mnemonics like "clz rd, rm" / "rev rd, rm": ops[0]==rd
 * (as parsed), then the nb_ops==2 shuffle sets ops[1]=old ops[0] (rd) and
 * ops[2]=old ops[1] (rm). See thumb_data_processing_opcode()'s memcpy pair. */
static void ops_2operand_shuffled(Operand ops[3], uint8_t rd, uint8_t rm)
{
  memset(ops, 0, 3 * sizeof(ops[0]));
  ops[0].type = UT_OP_REG32;
  ops[0].reg = rd;
  ops[1].type = UT_OP_REG32;
  ops[1].reg = rd;
  ops[2].type = UT_OP_REG32;
  ops[2].reg = rm;
}

static bool opcode_eq(thumb_opcode a, thumb_opcode b)
{
  return a.size == b.size && a.opcode == b.opcode;
}

/* Build a minimal ASMOperand with a stack-backed SValue for
 * asm_compute_constraints() tests. */
static void make_asm_operand(ASMOperand *op, SValue *sv, const char *constraint, int r_location)
{
  memset(op, 0, sizeof(*op));
  memset(sv, 0, sizeof(*sv));
  op->vt = sv;
  op->reg = -1;
  strncpy(op->constraint, constraint, sizeof(op->constraint) - 1);
  sv->r = r_location;
}

/* Variant for the specific-register path: VT_LOCAL + a Sym whose r field
 * encodes the requested physical register. */
static void make_asm_operand_with_local_sym(ASMOperand *op, SValue *sv, Sym *sym, const char *constraint,
                                            int forced_reg)
{
  memset(op, 0, sizeof(*op));
  memset(sv, 0, sizeof(*sv));
  memset(sym, 0, sizeof(*sym));
  op->vt = sv;
  op->reg = -1;
  strncpy(op->constraint, constraint, sizeof(op->constraint) - 1);
  sv->r = VT_LOCAL;
  sv->sym = sym;
  sym->r = (unsigned short)forced_reg;
}

/* ============================================================ */
/*  asm_parse_regvar()                                           */
/* ============================================================ */

UT_TEST(test_parse_regvar_low_regs)
{
  UT_ASSERT_EQ(asm_parse_regvar(TOK_ASM_r0), 0);
  UT_ASSERT_EQ(asm_parse_regvar(TOK_ASM_r7), 7);
  return 0;
}

UT_TEST(test_parse_regvar_r11_r15_direct)
{
  /* Non-aliased high registers fall through to the `default: return t -
     TOK_ASM_r0` arm rather than the fp/ip/sp/lr/pc named cases. */
  UT_ASSERT_EQ(asm_parse_regvar(TOK_ASM_r11), 11);
  UT_ASSERT_EQ(asm_parse_regvar(TOK_ASM_r12), 12);
  UT_ASSERT_EQ(asm_parse_regvar(TOK_ASM_r13), 13);
  UT_ASSERT_EQ(asm_parse_regvar(TOK_ASM_r14), 14);
  UT_ASSERT_EQ(asm_parse_regvar(TOK_ASM_r15), 15);
  return 0;
}

UT_TEST(test_parse_regvar_named_aliases)
{
  UT_ASSERT_EQ(asm_parse_regvar(TOK_ASM_fp), 11);
  UT_ASSERT_EQ(asm_parse_regvar(TOK_ASM_ip), 12);
  UT_ASSERT_EQ(asm_parse_regvar(TOK_ASM_sp), 13);
  UT_ASSERT_EQ(asm_parse_regvar(TOK_ASM_lr), 14);
  UT_ASSERT_EQ(asm_parse_regvar(TOK_ASM_pc), 15);
  return 0;
}

UT_TEST(test_parse_regvar_vfp_single)
{
  UT_ASSERT_EQ(asm_parse_regvar(TOK_ASM_s0), 0);
  UT_ASSERT_EQ(asm_parse_regvar(TOK_ASM_s31), 31);
  return 0;
}

UT_TEST(test_parse_regvar_vfp_double)
{
  UT_ASSERT_EQ(asm_parse_regvar(TOK_ASM_d0), 0);
  UT_ASSERT_EQ(asm_parse_regvar(TOK_ASM_d15), 15);
  return 0;
}

UT_TEST(test_parse_regvar_non_register_token_is_invalid)
{
  /* TOK_ASM_push is a mnemonic token, well outside every register token
     range asm_parse_regvar() recognizes. */
  UT_ASSERT_EQ(asm_parse_regvar(TOK_ASM_push), -1);
  return 0;
}

/* ============================================================ */
/*  thumb_parse_special_register()                                */
/* ============================================================ */

uint32_t thumb_parse_special_register(int token);
uint32_t thumb_parse_special_register_mask(int token);

static int set_special_reg_tok(const char *name)
{
  /* Use a fixed high token id (comfortably below stubs.c's UTB_MAX_TOK==512
     and clear of any real builtin token range used elsewhere in this TU). */
  static int next_tok = 400;
  int tok = next_tok++;
  utb_set_tok_str(tok, name);
  return tok;
}

UT_TEST(test_special_register_apsr)
{
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("apsr")), 0x00);
  return 0;
}

UT_TEST(test_special_register_iapsr_checked_before_apsr)
{
  /* "iapsr" contains "apsr" as a substring; the iapsr branch must be tried
     first or every iapsr/eapsr/xpsr/ipsr/iepsr/epsr name would incorrectly
     match the generic "apsr" branch (0x00) via strstr(). */
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("iapsr")), 0x01);
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("eapsr")), 0x02);
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("xpsr")), 0x03);
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("ipsr")), 0x05);
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("iepsr")), 0x07);
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("epsr")), 0x06);
  return 0;
}

UT_TEST(test_special_register_msp_family_ns_before_plain)
{
  /* Same precedence hazard as apsr: "msplim_ns" contains "msplim" contains
     "msp", so the _ns and *lim variants must be tried before the bare name. */
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("msp")), 0x08);
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("psp")), 0x09);
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("msplim")), 0x0a);
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("psplim")), 0x0b);
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("msp_ns")), 0x88);
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("psp_ns")), 0x89);
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("msplim_ns")), 0x8a);
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("psplim_ns")), 0x8b);
  return 0;
}

UT_TEST(test_special_register_privilege_family)
{
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("primask")), 0x10);
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("basepri")), 0x11);
  /* Regression lock for bugs.md #11 (fixed): the `basepri_max` branch is now
     checked *before* the `basepri` branch in thumb_parse_special_register()
     (arm-thumb-asm.c), matching the longest-match-first ordering used by every
     other substring-hazard family in that function. Before the fix, "basepri"
     shadowed "basepri_max" (both contain "basepri"), so `MSR/MRS basepri_max`
     silently encoded BASEPRI's SYSm (0x11) instead of BASEPRI_MAX's (0x12) --
     a wrong-register miscompile in inline asm. This now asserts the correct
     0x12. */
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("basepri_max")), 0x12);
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("faultmask")), 0x13);
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("control")), 0x14);
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("primask_ns")), 0x90);
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("basepri_ns")), 0x91);
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("faultmask_ns")), 0x93);
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("control_ns")), 0x94);
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("sp_ns")), 0x98);
  return 0;
}

UT_TEST(test_special_register_uppercase_is_lowered)
{
  /* thumb_parse_special_register lower-cases into a local buffer before
     matching, so "APSR"/"MSP" (as a real assembler might see after a
     case-preserving lexer) must resolve identically to lowercase. */
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("APSR")), 0x00);
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("MSP")), 0x08);
  return 0;
}

UT_TEST(test_special_register_unknown_name)
{
  UT_ASSERT_EQ(thumb_parse_special_register(set_special_reg_tok("not_a_special_reg")), 0xff);
  return 0;
}

/* ============================================================ */
/*  thumb_parse_special_register_mask()                           */
/* ============================================================ */

UT_TEST(test_special_register_mask_variants)
{
  UT_ASSERT_EQ(thumb_parse_special_register_mask(set_special_reg_tok("apsr_nzcvqg")), 0x3);
  UT_ASSERT_EQ(thumb_parse_special_register_mask(set_special_reg_tok("apsr_nzcvq")), 0x2);
  UT_ASSERT_EQ(thumb_parse_special_register_mask(set_special_reg_tok("apsr_g")), 0x1);
  /* No recognized suffix -> default mask 0x2 (not 0, per the fall-through
     `return 0x2` at the end of the function). */
  UT_ASSERT_EQ(thumb_parse_special_register_mask(set_special_reg_tok("apsr")), 0x2);
  return 0;
}

/* ============================================================ */
/*  thumb_parse_token_suffix()                                    */
/* ============================================================ */

UT_TEST(test_token_suffix_no_suffix_defaults_to_al)
{
  int base_token = -1;
  int tok = set_special_reg_tok("add");
  int cond = thumb_parse_token_suffix(tok, &base_token);
  /* COND_AL == 14 (0xe), see cond_names[]'s {NULL, 14} terminator entry and
     thumb_parse_token_suffix's `condition = COND_AL` default. */
  UT_ASSERT_EQ(cond, 14);
  return 0;
}

UT_TEST(test_token_suffix_condition_eq)
{
  int base_token = -1;
  int tok = set_special_reg_tok("addeq");
  int cond = thumb_parse_token_suffix(tok, &base_token);
  UT_ASSERT_EQ(cond, 0); /* eq -> 0 */
  return 0;
}

UT_TEST(test_token_suffix_condition_ne_with_width)
{
  int base_token = -1;
  int tok = set_special_reg_tok("subne.w");
  int cond = thumb_parse_token_suffix(tok, &base_token);
  UT_ASSERT_EQ(cond, 1); /* ne -> 1 */
  return 0;
}

UT_TEST(test_token_suffix_condition_gt)
{
  int base_token = -1;
  int tok = set_special_reg_tok("movgt");
  int cond = thumb_parse_token_suffix(tok, &base_token);
  UT_ASSERT_EQ(cond, 0xc); /* gt -> 0xc */
  return 0;
}

UT_TEST(test_token_suffix_bx_two_char_base_with_condition)
{
  /* Exercises get_base_instruction_name()'s valid_2char_bases[] allowance
     for "bx"/"bl" (candidate_len==2 case), via "bxeq". */
  int base_token = -1;
  int tok = set_special_reg_tok("bxeq");
  int cond = thumb_parse_token_suffix(tok, &base_token);
  UT_ASSERT_EQ(cond, 0);
  return 0;
}

UT_TEST(test_token_suffix_width_only_no_condition)
{
  int base_token = -1;
  int tok = set_special_reg_tok("add.w");
  int cond = thumb_parse_token_suffix(tok, &base_token);
  /* No condition-code suffix present -> defaults to COND_AL. */
  UT_ASSERT_EQ(cond, 14);
  return 0;
}

/* ============================================================ */
/*  thumb_generate_opcode_for_data_processing() /                 */
/*  thumb_process_generic_data_op()                                */
/* ============================================================ */

UT_TEST(test_dispatch_adds_imm_sets_flags)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_imm(ops, 0, 1, 42);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_adds, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_add_imm(0, 1, 42, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_add_imm_unconditional_forces_32bit)
{
  /* thumb_conditional_scope==0 (no IT block active in this harness) and
     token==TOK_ASM_add (not the 's' variant) forces ENFORCE_ENCODING_32BIT
     for the immediate form -- this is the "outside an IT block, plain ADD
     must not silently narrow and drop flags-don't-care" branch. */
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_imm(ops, 2, 3, 100);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_add, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_add_imm(2, 3, 100, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_32BIT);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_add_reg)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 1, 2);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_add, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_add_reg(0, 1, 2, FLAGS_BEHAVIOUR_BLOCK, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_add_imm_sp_base_uses_sp_form)
{
  /* ops[1].reg == R_SP routes through th_add_imm(rd, R_SP, imm, ...)
     regardless of the general unconditional-32bit-encoding rule above. */
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_imm(ops, 0, R_SP, 32);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_add, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_add_imm(0, R_SP, 32, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_addw_imm_uses_addw_encoder)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_imm(ops, 4, 5, 200);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_addw, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_addw(4, 5, 200);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_addw_sp_base)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_imm(ops, 0, R_SP, 16);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_addw, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_addw(0, R_SP, 16);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_subs_imm_sets_flags)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_imm(ops, 0, 1, 5);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_subs, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_sub_imm(0, 1, 5, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_sub_reg_sp_base)
{
  /* sub rd, sp, rm -- ops[1].reg==R_SP routes to th_sub_reg(rd, R_SP, rm, ...).
     Unlike the immediate form, the register form never forces 32-bit
     encoding based on token/conditional-scope (only THUMB_HAS_WIDE_QUALIFIER
     -- unset here -- affects `encoding` before the switch). */
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, R_SP, 3);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_sub, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_sub_reg(0, R_SP, 3, FLAGS_BEHAVIOUR_BLOCK, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_subw_imm)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_imm(ops, 1, 2, 300);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_subw, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_subw(1, 2, 300);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_mov_imm_block_flags)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_imm(ops, 0, 0, 7);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_mov, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_mov_imm(0, 7, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_movs_imm_sets_flags)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_imm(ops, 1, 1, 9);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_movs, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_mov_imm(1, 9, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_movw_imm_forces_32bit)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_imm(ops, 2, 2, 0x1234);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_movw, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_mov_imm(2, 0x1234, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_32BIT);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_mov_reg)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 0, 3);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_mov, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want =
      th_mov_reg(0, 3, FLAGS_BEHAVIOUR_BLOCK, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, /*in_it=*/false);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_cmp_imm_always_sets_flags)
{
  /* cmp rn, #imm: only ops[1] (rn) and ops[2] (imm) are read; ops[0] (an
     unused "rd" slot in this dispatcher's shared 3-operand shape) is
     irrelevant, so ops_reg_reg_imm's rd argument is a don't-care here. */
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_imm(ops, /*rd=don't-care*/ 0, /*rn=*/4, /*imm=*/5);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_cmp, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_cmp_imm(4, 5, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_cmp_reg)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 6, 7);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_cmp, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_cmp_reg(0, 6, 7, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_cmn_imm)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_imm(ops, 0, 2, 10);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_cmn, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_cmn_imm(2, 10, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_cmn_reg)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 2, 3);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_cmn, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_cmn_reg(2, 3, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_teq_imm)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_imm(ops, 0, 1, 0xff);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_teq, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_teq_imm(1, 0xff, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_tst_imm)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_imm(ops, 0, 1, 0x0f);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_tst, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_tst_imm(1, 0x0f, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_tst_reg)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 1, 2);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_tst, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_tst_reg(1, 2, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_clz)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_2operand_shuffled(ops, 0, 5); /* clz r0, r5 */
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_clz, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_clz(0, 5);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_rbit)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_2operand_shuffled(ops, 1, 2);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_rbit, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_rbit(1, 2);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_rev_family)
{
  setup_armv8m_main();
  Operand ops[3];

  ops_2operand_shuffled(ops, 0, 1);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_rev, THUMB_SHIFT_DEFAULT, ops),
                       th_rev(0, 1, ENFORCE_ENCODING_NONE)));

  ops_2operand_shuffled(ops, 2, 3);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_rev16, THUMB_SHIFT_DEFAULT, ops),
                       th_rev16(2, 3, ENFORCE_ENCODING_NONE)));

  ops_2operand_shuffled(ops, 4, 5);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_revsh, THUMB_SHIFT_DEFAULT, ops),
                       th_revsh(4, 5, ENFORCE_ENCODING_NONE)));
  return 0;
}

UT_TEST(test_dispatch_sxtb_sxth_uxtb_uxth)
{
  setup_armv8m_main();
  Operand ops[3];

  ops_2operand_shuffled(ops, 0, 1);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_sxtb, THUMB_SHIFT_DEFAULT, ops),
                       th_sxtb(0, 1, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)));

  ops_2operand_shuffled(ops, 2, 3);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_sxth, THUMB_SHIFT_DEFAULT, ops),
                       th_sxth(2, 3, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)));

  ops_2operand_shuffled(ops, 4, 5);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_uxtb, THUMB_SHIFT_DEFAULT, ops),
                       th_uxtb(4, 5, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)));

  ops_2operand_shuffled(ops, 6, 7);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_uxth, THUMB_SHIFT_DEFAULT, ops),
                       th_uxth(6, 7, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)));
  return 0;
}

UT_TEST(test_dispatch_bfc)
{
  setup_armv8m_main();
  Operand ops[3];
  memset(ops, 0, sizeof(ops));
  ops[0].type = UT_OP_REG32;
  ops[0].reg = 3;
  ops[1].type = UT_OP_IM32;
  ops[1].e.v = 4; /* lsb */
  ops[2].type = UT_OP_IM32;
  ops[2].e.v = 8; /* width */
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_bfc, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_bfc(3, 4, 8);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_mul_no_swap_when_rd_ne_rn)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 1, 2); /* mul r0, r1, r2 : rd != rn, no swap */
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_mul, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_mul(0, 1, 2, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_mul_swaps_operands_when_rd_eq_rn)
{
  /* mul r0, r0, r1 : ops[0].reg == ops[1].reg triggers the rm/rn swap so
     th_mul is called as th_mul(rd=0, rn=ops[2]=1, rm=ops[0]=0, ...). */
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 0, 1);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_mul, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_mul(0, 1, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_muls_sets_flags)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 1, 2);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_muls, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_mul(0, 1, 2, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_sdiv_udiv)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 1, 2);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_sdiv, THUMB_SHIFT_DEFAULT, ops),
                       th_sdiv(0, 1, 2)));
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_udiv, THUMB_SHIFT_DEFAULT, ops),
                       th_udiv(0, 1, 2)));
  return 0;
}

UT_TEST(test_dispatch_uadd8_usub8_sel)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 1, 2);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_uadd8, THUMB_SHIFT_DEFAULT, ops),
                       th_uadd8(0, 1, 2)));
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_usub8, THUMB_SHIFT_DEFAULT, ops),
                       th_usub8(0, 1, 2)));
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_sel, THUMB_SHIFT_DEFAULT, ops),
                       th_sel(0, 1, 2)));
  return 0;
}

UT_TEST(test_dispatch_unknown_token_returns_zero_opcode)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 1, 2);
  /* TOK_ASM_push is a real token but not one of the data-processing switch
     cases -> falls through to the function's final `return (thumb_opcode){0, 0}`. */
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_push, THUMB_SHIFT_DEFAULT, ops);
  UT_ASSERT_EQ(got.size, 0);
  UT_ASSERT_EQ(got.opcode, 0);
  return 0;
}

/* ---- thumb_process_generic_data_op()-routed mnemonics (and/orr/eor/bic/
   mvn/rsb/adc/sbc/orn all share this helper; exercise a representative
   sample of the imm and reg paths, plus the 's'-variant flags-set case). */

UT_TEST(test_dispatch_and_imm_and_reg)
{
  setup_armv8m_main();
  Operand ops[3];

  ops_reg_reg_imm(ops, 0, 1, 0x55);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_and, THUMB_SHIFT_DEFAULT, ops),
                       th_and_imm(0, 1, 0x55, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE)));

  ops_reg_reg_reg(ops, 0, 1, 2);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_and, THUMB_SHIFT_DEFAULT, ops),
                       th_and_reg(0, 1, 2, FLAGS_BEHAVIOUR_BLOCK, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)));
  return 0;
}

UT_TEST(test_dispatch_ands_sets_flags)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_imm(ops, 0, 1, 0x0f);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_ands, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_and_imm(0, 1, 0x0f, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_orr_imm_and_reg)
{
  setup_armv8m_main();
  Operand ops[3];

  ops_reg_reg_imm(ops, 0, 1, 0xa0);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_orr, THUMB_SHIFT_DEFAULT, ops),
                       th_orr_imm(0, 1, 0xa0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE)));

  ops_reg_reg_reg(ops, 0, 1, 2);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_orr, THUMB_SHIFT_DEFAULT, ops),
                       th_orr_reg(0, 1, 2, FLAGS_BEHAVIOUR_BLOCK, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)));
  return 0;
}

UT_TEST(test_dispatch_orn_imm_and_reg)
{
  setup_armv8m_main();
  Operand ops[3];

  ops_reg_reg_imm(ops, 0, 1, 0x11);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_orn, THUMB_SHIFT_DEFAULT, ops),
                       th_orn_imm(0, 1, 0x11, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE)));

  ops_reg_reg_reg(ops, 0, 1, 2);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_orn, THUMB_SHIFT_DEFAULT, ops),
                       th_orn_reg(0, 1, 2, FLAGS_BEHAVIOUR_BLOCK, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)));
  return 0;
}

UT_TEST(test_dispatch_eor_imm_and_reg)
{
  setup_armv8m_main();
  Operand ops[3];

  ops_reg_reg_imm(ops, 0, 1, 0x22);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_eor, THUMB_SHIFT_DEFAULT, ops),
                       th_eor_imm(0, 1, 0x22, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE)));

  ops_reg_reg_reg(ops, 0, 1, 2);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_eor, THUMB_SHIFT_DEFAULT, ops),
                       th_eor_reg(0, 1, 2, FLAGS_BEHAVIOUR_BLOCK, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)));
  return 0;
}

UT_TEST(test_dispatch_bic_imm_and_reg)
{
  setup_armv8m_main();
  Operand ops[3];

  ops_reg_reg_imm(ops, 0, 1, 0x33);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_bic, THUMB_SHIFT_DEFAULT, ops),
                       th_bic_imm(0, 1, 0x33, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE)));

  ops_reg_reg_reg(ops, 0, 1, 2);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_bic, THUMB_SHIFT_DEFAULT, ops),
                       th_bic_reg(0, 1, 2, FLAGS_BEHAVIOUR_BLOCK, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)));
  return 0;
}

UT_TEST(test_dispatch_rsb_imm_and_reg)
{
  setup_armv8m_main();
  Operand ops[3];

  ops_reg_reg_imm(ops, 0, 1, 0);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_rsb, THUMB_SHIFT_DEFAULT, ops),
                       th_rsb_imm(0, 1, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE)));

  ops_reg_reg_reg(ops, 0, 1, 2);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_rsb, THUMB_SHIFT_DEFAULT, ops),
                       th_rsb_reg(0, 1, 2, FLAGS_BEHAVIOUR_BLOCK, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)));
  return 0;
}

UT_TEST(test_dispatch_mvn_imm_and_reg)
{
  /* th_mvn_imm/th_mvn_reg both take a (perhaps vestigial) middle register
     argument in addition to rd -- thumb_process_generic_data_op passes
     ops[1].reg there uniformly for every mnemonic it routes, same as every
     other generic op (and/orr/eor/...). */
  setup_armv8m_main();
  Operand ops[3];

  ops_reg_reg_imm(ops, 0, 1, 0xcc);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_mvn, THUMB_SHIFT_DEFAULT, ops),
                       th_mvn_imm(0, 1, 0xcc, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE)));

  ops_reg_reg_reg(ops, 0, 1, 2);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_mvn, THUMB_SHIFT_DEFAULT, ops),
                       th_mvn_reg(0, 1, 2, FLAGS_BEHAVIOUR_BLOCK, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)));
  return 0;
}

UT_TEST(test_dispatch_adc_imm_and_reg)
{
  setup_armv8m_main();
  Operand ops[3];

  ops_reg_reg_imm(ops, 0, 1, 1);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_adc, THUMB_SHIFT_DEFAULT, ops),
                       th_adc_imm(0, 1, 1, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE)));

  ops_reg_reg_reg(ops, 0, 1, 2);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_adc, THUMB_SHIFT_DEFAULT, ops),
                       th_adc_reg(0, 1, 2, FLAGS_BEHAVIOUR_BLOCK, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)));
  return 0;
}

UT_TEST(test_dispatch_sbc_imm_and_reg)
{
  setup_armv8m_main();
  Operand ops[3];

  ops_reg_reg_imm(ops, 0, 1, 2);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_sbc, THUMB_SHIFT_DEFAULT, ops),
                       th_sbc_imm(0, 1, 2, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE)));

  ops_reg_reg_reg(ops, 0, 1, 2);
  UT_ASSERT(opcode_eq(thumb_generate_opcode_for_data_processing(TOK_ASM_sbc, THUMB_SHIFT_DEFAULT, ops),
                       th_sbc_reg(0, 1, 2, FLAGS_BEHAVIOUR_BLOCK, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)));
  return 0;
}

UT_TEST(test_dispatch_generic_op_returns_zero_when_operand_neither_imm_nor_reg)
{
  /* thumb_process_generic_data_op falls through to `return (thumb_opcode){0, 0}`
     when ops[2] is neither an immediate-typed nor register-typed operand
     (e.g. a register-set operand, as used by push/pop-style mnemonics). */
  setup_armv8m_main();
  Operand ops[3];
  memset(ops, 0, sizeof(ops));
  ops[0].type = UT_OP_REG32;
  ops[1].type = UT_OP_REG32;
  ops[2].type = 0; /* neither UT_OP_REG32 nor UT_OP_IM32 */
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_and, THUMB_SHIFT_DEFAULT, ops);
  UT_ASSERT_EQ(got.size, 0);
  UT_ASSERT_EQ(got.opcode, 0);
  return 0;
}

/* ============================================================ */
/*  tcc_asm_set_fpu()                                            */
/* ============================================================ */

UT_TEST(test_fpu_enable_vfpv4_sp_d16)
{
  setup_armv8m_main();
  thop_feat before = arm_target_dependent.feat;
  tcc_asm_set_fpu("vfpv4-sp-d16");
  UT_ASSERT_EQ(arm_target_dependent.feat.vfp_sp, 1);
  /* OR semantics must preserve the already-enabled core features. */
  UT_ASSERT_EQ(arm_target_dependent.feat.t32, before.t32);
  return 0;
}

UT_TEST(test_fpu_enable_fpv5_d16)
{
  setup_armv8m_main();
  tcc_asm_set_fpu("fpv5-d16");
  UT_ASSERT_EQ(arm_target_dependent.feat.vfp_sp, 1);
  UT_ASSERT_EQ(arm_target_dependent.feat.vfp_dp, 1);
  UT_ASSERT_EQ(arm_target_dependent.feat.fp_armv8, 1);
  return 0;
}

/* ============================================================ */
/*  asm_clobber()                                                */
/* ============================================================ */

UT_TEST(test_clobber_register_sets_bit)
{
  uint8_t regs[NB_ASM_REGS] = {0};
  /* asm_clobber() resolves the register name through tok_alloc(); seed the
     token table so "r3" maps back to the register token asm_parse_regvar()
     recognizes. */
  utb_set_tok_str(TOK_ASM_r3, "r3");
  asm_clobber(regs, "r3");
  UT_ASSERT_EQ(regs[3], 1);
  return 0;
}

UT_TEST(test_clobber_alias_lr)
{
  uint8_t regs[NB_ASM_REGS] = {0};
  /* Make sure the string "lr" resolves to the lr alias token. */
  utb_set_tok_str(TOK_ASM_lr, "lr");
  asm_clobber(regs, "lr");
  UT_ASSERT_EQ(regs[14], 1);
  return 0;
}

UT_TEST(test_clobber_memory_cc_flags_are_noops)
{
  uint8_t regs[NB_ASM_REGS] = {0};
  asm_clobber(regs, "memory");
  asm_clobber(regs, "cc");
  asm_clobber(regs, "flags");
  for (int i = 0; i < NB_ASM_REGS; i++)
    UT_ASSERT_EQ(regs[i], 0);
  return 0;
}

/* ============================================================ */
/*  asm_compute_constraints()                                    */
/* ============================================================ */

UT_TEST(test_constraints_single_output_register)
{
  ASMOperand op;
  SValue sv;
  uint8_t clobber[NB_ASM_REGS] = {0};
  uint8_t reserved[NB_ASM_REGS] = {0};
  int out_reg = -1;
  make_asm_operand(&op, &sv, "r", /*r_location=*/0);
  asm_compute_constraints(&op, 1, 1, clobber, reserved, &out_reg);
  UT_ASSERT_EQ(op.reg, 0);
  return 0;
}

UT_TEST(test_constraints_single_input_register)
{
  ASMOperand op;
  SValue sv;
  uint8_t clobber[NB_ASM_REGS] = {0};
  uint8_t reserved[NB_ASM_REGS] = {0};
  int out_reg = -1;
  make_asm_operand(&op, &sv, "r", /*r_location=*/0);
  asm_compute_constraints(&op, 1, 0, clobber, reserved, &out_reg);
  UT_ASSERT_EQ(op.reg, 0);
  return 0;
}

UT_TEST(test_constraints_output_then_input_pair)
{
  /* Without an earlyclobber modifier, output and input "r" constraints may
     share a register (input only checks REG_IN_MASK). Use '&' to force the
     output to be allocated in a different register from inputs, and verify
     the solver skips the occupied register for the input. */
  ASMOperand ops[2];
  SValue svs[2];
  uint8_t clobber[NB_ASM_REGS] = {0};
  uint8_t reserved[NB_ASM_REGS] = {0};
  int out_reg = -1;
  make_asm_operand(&ops[0], &svs[0], "&r", 0);
  make_asm_operand(&ops[1], &svs[1], "r", 0);
  asm_compute_constraints(ops, 2, 1, clobber, reserved, &out_reg);
  UT_ASSERT_EQ(ops[0].reg, 0);
  UT_ASSERT_EQ(ops[1].reg, 1);
  return 0;
}

UT_TEST(test_constraints_read_write_modifier)
{
  ASMOperand op;
  SValue sv;
  uint8_t clobber[NB_ASM_REGS] = {0};
  uint8_t reserved[NB_ASM_REGS] = {0};
  int out_reg = -1;
  make_asm_operand(&op, &sv, "+r", 0);
  asm_compute_constraints(&op, 1, 1, clobber, reserved, &out_reg);
  UT_ASSERT_EQ(op.reg, 0);
  UT_ASSERT_EQ(op.is_rw, 1);
  return 0;
}

UT_TEST(test_constraints_memory_operand_llocal)
{
  ASMOperand op;
  SValue sv;
  uint8_t clobber[NB_ASM_REGS] = {0};
  uint8_t reserved[NB_ASM_REGS] = {0};
  int out_reg = -1;
  make_asm_operand(&op, &sv, "m", VT_LLOCAL);
  asm_compute_constraints(&op, 1, 0, clobber, reserved, &out_reg);
  UT_ASSERT_EQ(op.is_memory, 1);
  UT_ASSERT_EQ(op.reg, 0);
  return 0;
}

UT_TEST(test_constraints_immediate_operand_no_register)
{
  ASMOperand op;
  SValue sv;
  uint8_t clobber[NB_ASM_REGS] = {0};
  uint8_t reserved[NB_ASM_REGS] = {0};
  int out_reg = -1;
  make_asm_operand(&op, &sv, "i", VT_CONST);
  asm_compute_constraints(&op, 1, 0, clobber, reserved, &out_reg);
  UT_ASSERT_EQ(op.reg, -1);
  return 0;
}

UT_TEST(test_constraints_specific_register_via_local_sym)
{
  ASMOperand op;
  SValue sv;
  Sym sym;
  uint8_t clobber[NB_ASM_REGS] = {0};
  uint8_t reserved[NB_ASM_REGS] = {0};
  int out_reg = -1;
  make_asm_operand_with_local_sym(&op, &sv, &sym, "r", 5);
  asm_compute_constraints(&op, 1, 1, clobber, reserved, &out_reg);
  UT_ASSERT_EQ(op.reg, 5);
  return 0;
}

UT_TEST(test_constraints_reserved_regs_skipped)
{
  ASMOperand op;
  SValue sv;
  uint8_t clobber[NB_ASM_REGS] = {0};
  uint8_t reserved[NB_ASM_REGS] = {0};
  int out_reg = -1;
  reserved[0] = 1;
  make_asm_operand(&op, &sv, "r", 0);
  asm_compute_constraints(&op, 1, 1, clobber, reserved, &out_reg);
  UT_ASSERT_EQ(op.reg, 1);
  return 0;
}

/* ============================================================ */
/*  thumb_parse_token_suffix() -- additional branches            */
/* ============================================================ */

UT_TEST(test_token_suffix_narrow_qualifier)
{
  /* Use tok_alloc() to obtain the canonical token for the base mnemonic:
     earlier tests may have already registered "add" under a different id,
     and thumb_parse_token_suffix() resolves the stripped base via the same
     tok_alloc() lookup. */
  int add_tok = tok_alloc("add", 3)->tok;
  int add_n_tok = set_special_reg_tok("add.n");
  int base_token = -1;
  int cond = thumb_parse_token_suffix(add_n_tok, &base_token);
  UT_ASSERT_EQ(cond, 14);
  UT_ASSERT_EQ(base_token, add_tok);
  return 0;
}

UT_TEST(test_token_suffix_condition_aliases_cs_cc)
{
  int b_tok = set_special_reg_tok("b");
  int bcs_tok = set_special_reg_tok("bcs");
  int base_token = -1;
  int cond = thumb_parse_token_suffix(bcs_tok, &base_token);
  UT_ASSERT_EQ(cond, 2); /* cs -> 2 */
  UT_ASSERT_EQ(base_token, b_tok);

  int bcc_tok = set_special_reg_tok("bcc");
  cond = thumb_parse_token_suffix(bcc_tok, &base_token);
  UT_ASSERT_EQ(cond, 3); /* cc -> 3 */
  return 0;
}

UT_TEST(test_token_suffix_one_char_base_with_condition)
{
  /* Resolve the canonical token for "b" via tok_alloc(), matching how
     thumb_parse_token_suffix() looks up the stripped base mnemonic. */
  int b_tok = tok_alloc("b", 1)->tok;
  int beq_tok = set_special_reg_tok("beq");
  int base_token = -1;
  int cond = thumb_parse_token_suffix(beq_tok, &base_token);
  UT_ASSERT_EQ(cond, 0); /* eq -> 0 */
  UT_ASSERT_EQ(base_token, b_tok);
  return 0;
}

UT_TEST(test_token_suffix_unknown_suffix_returns_al)
{
  int addxyz_tok = set_special_reg_tok("addxyz");
  int base_token = -1;
  int cond = thumb_parse_token_suffix(addxyz_tok, &base_token);
  UT_ASSERT_EQ(cond, 14);
  UT_ASSERT_EQ(base_token, addxyz_tok);
  return 0;
}

/* ============================================================ */
/*  thumb_generate_opcode_for_data_processing() -- remaining     */
/*  flag-variant and qualifier branches                          */
/* ============================================================ */

UT_TEST(test_dispatch_sub_imm_unconditional_forces_32bit)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_imm(ops, 2, 3, 100);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_sub, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_sub_imm(2, 3, 100, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_32BIT);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_cmn_reg_wide_qualifier_forces_32bit)
{
  setup_armv8m_main();
  int base;
  int cmn_w_tok = set_special_reg_tok("cmn.w");
  thumb_parse_token_suffix(cmn_w_tok, &base); /* sets current_asm_suffix.width = WIDTH_WIDE */

  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 1, 2);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_cmn, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_cmn_reg(1, 2, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_32BIT);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_adcs_sets_flags)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 1, 2);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_adcs, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_adc_reg(0, 1, 2, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_orrs_sets_flags)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 1, 2);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_orrs, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_orr_reg(0, 1, 2, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_orns_sets_flags)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 1, 2);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_orns, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_orn_reg(0, 1, 2, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_eors_sets_flags)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 1, 2);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_eors, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_eor_reg(0, 1, 2, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_bics_sets_flags)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 1, 2);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_bics, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_bic_reg(0, 1, 2, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_mvns_sets_flags)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 1, 2);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_mvns, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_mvn_reg(0, 1, 2, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_rsbs_sets_flags)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 1, 2);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_rsbs, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_rsb_reg(0, 1, 2, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_sbcs_sets_flags)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 1, 2);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_sbcs, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_sbc_reg(0, 1, 2, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

/* ============================================================ */
/*  thumb_process_generic_data_op() direct                        */
/* ============================================================ */

static th_generic_op_data and_op_data(void)
{
  return (th_generic_op_data){
      .generate_imm_opcode = th_and_imm,
      .generate_reg_opcode = th_and_reg,
      .regular_variant_token = TOK_ASM_and,
      .flags_variant_token = TOK_ASM_ands,
  };
}

UT_TEST(test_process_generic_data_op_reg_forces_32bit_outside_it)
{
  setup_armv8m_main();
  Operand ops[3];
  /* rd==rn with low registers is encodable as T16, so the unconditional
     32-bit force for the regular variant is observable. */
  ops_reg_reg_reg(ops, 0, 0, 1);
  thumb_opcode got = thumb_process_generic_data_op(and_op_data(), TOK_ASM_and, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_and_reg(0, 0, 1, FLAGS_BEHAVIOUR_BLOCK, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_32BIT);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_process_generic_data_op_ands_sets_flags_no_force)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 0, 1);
  thumb_opcode got = thumb_process_generic_data_op(and_op_data(), TOK_ASM_ands, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_and_reg(0, 0, 1, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_process_generic_data_op_wide_qualifier_forces_32bit)
{
  setup_armv8m_main();
  int base;
  int and_w_tok = set_special_reg_tok("and.w");
  thumb_parse_token_suffix(and_w_tok, &base);

  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 0, 1);
  thumb_opcode got = thumb_process_generic_data_op(and_op_data(), TOK_ASM_and, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_and_reg(0, 0, 1, FLAGS_BEHAVIOUR_BLOCK, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_32BIT);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_process_generic_data_op_imm_path)
{
  setup_armv8m_main();
  Operand ops[3];
  ops_reg_reg_imm(ops, 0, 1, 0x55);
  thumb_opcode got = thumb_process_generic_data_op(and_op_data(), TOK_ASM_and, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_and_imm(0, 1, 0x55, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

/* ============================================================ */
/*  asm_compute_constraints() -- additional branches               */
/* ============================================================ */

UT_TEST(test_constraints_priority_specific_register_before_general)
{
  /* The specific-register path has priority 1, lower than general 'r' (3), so
     the solver allocates it first even when it appears later in operand order. */
  ASMOperand ops[2];
  SValue svs[2];
  Sym sym;
  uint8_t clobber[NB_ASM_REGS] = {0};
  uint8_t reserved[NB_ASM_REGS] = {0};
  int out_reg = -1;
  make_asm_operand(&ops[0], &svs[0], "r", VT_LOCAL);
  make_asm_operand_with_local_sym(&ops[1], &svs[1], &sym, "r", 0);
  asm_compute_constraints(ops, 2, 0, clobber, reserved, &out_reg);
  UT_ASSERT_EQ(ops[0].reg, 1);
  UT_ASSERT_EQ(ops[1].reg, 0);
  return 0;
}

UT_TEST(test_constraints_clobber_prevents_allocation)
{
  ASMOperand op;
  SValue sv;
  uint8_t clobber[NB_ASM_REGS] = {0};
  uint8_t reserved[NB_ASM_REGS] = {0};
  int out_reg = -1;
  clobber[0] = 1;
  make_asm_operand(&op, &sv, "r", VT_LOCAL);
  asm_compute_constraints(&op, 1, 0, clobber, reserved, &out_reg);
  UT_ASSERT_EQ(op.reg, 1);
  return 0;
}

UT_TEST(test_constraints_alternative_immediate_chosen)
{
  ASMOperand op;
  SValue sv;
  uint8_t clobber[NB_ASM_REGS] = {0};
  uint8_t reserved[NB_ASM_REGS] = {0};
  int out_reg = -1;
  make_asm_operand(&op, &sv, "i,r", VT_CONST);
  asm_compute_constraints(&op, 1, 0, clobber, reserved, &out_reg);
  UT_ASSERT_EQ(op.reg, -1);
  return 0;
}

UT_TEST(test_constraints_alternative_register_fallback)
{
  ASMOperand op;
  SValue sv;
  uint8_t clobber[NB_ASM_REGS] = {0};
  uint8_t reserved[NB_ASM_REGS] = {0};
  int out_reg = -1;
  make_asm_operand(&op, &sv, "i,r", VT_LOCAL);
  asm_compute_constraints(&op, 1, 0, clobber, reserved, &out_reg);
  UT_ASSERT_EQ(op.reg, 0);
  return 0;
}

UT_TEST(test_constraints_I_immediate_no_register)
{
  ASMOperand op;
  SValue sv;
  uint8_t clobber[NB_ASM_REGS] = {0};
  uint8_t reserved[NB_ASM_REGS] = {0};
  int out_reg = -1;
  make_asm_operand(&op, &sv, "I", VT_CONST);
  asm_compute_constraints(&op, 1, 0, clobber, reserved, &out_reg);
  UT_ASSERT_EQ(op.reg, -1);
  return 0;
}

UT_TEST(test_constraints_M_immediate_no_register)
{
  ASMOperand op;
  SValue sv;
  uint8_t clobber[NB_ASM_REGS] = {0};
  uint8_t reserved[NB_ASM_REGS] = {0};
  int out_reg = -1;
  make_asm_operand(&op, &sv, "M", VT_CONST);
  asm_compute_constraints(&op, 1, 0, clobber, reserved, &out_reg);
  UT_ASSERT_EQ(op.reg, -1);
  return 0;
}

UT_TEST(test_constraints_memory_output_llocal)
{
  ASMOperand op;
  SValue sv;
  uint8_t clobber[NB_ASM_REGS] = {0};
  uint8_t reserved[NB_ASM_REGS] = {0};
  int out_reg = -1;
  make_asm_operand(&op, &sv, "m", VT_LLOCAL);
  asm_compute_constraints(&op, 1, 1, clobber, reserved, &out_reg);
  UT_ASSERT_EQ(op.is_memory, 1);
  UT_ASSERT_EQ(op.reg, 0);
  return 0;
}

UT_TEST(test_constraints_out_reg_for_llocal_output)
{
  ASMOperand op;
  SValue sv;
  uint8_t clobber[NB_ASM_REGS] = {0};
  uint8_t reserved[NB_ASM_REGS] = {0};
  int out_reg = -1;
  make_asm_operand(&op, &sv, "r", VT_LLOCAL);
  asm_compute_constraints(&op, 1, 1, clobber, reserved, &out_reg);
  UT_ASSERT_EQ(op.reg, 0);
  UT_ASSERT_EQ(out_reg, 1);
  return 0;
}

/* ============================================================ */
/*  thumb_parse_token_suffix() -- additional branches              */
/* ============================================================ */

UT_TEST(test_token_suffix_uppercase_width)
{
  int add_tok = tok_alloc("add", 3)->tok;
  int add_W_tok = set_special_reg_tok("add.W");
  int base_token = -1;
  int cond = thumb_parse_token_suffix(add_W_tok, &base_token);
  UT_ASSERT_EQ(cond, 14);
  UT_ASSERT_EQ(base_token, add_tok);
  return 0;
}

UT_TEST(test_token_suffix_three_char_base_with_condition)
{
  int blx_tok = tok_alloc("blx", 3)->tok;
  int blxeq_tok = set_special_reg_tok("blxeq");
  int base_token = -1;
  int cond = thumb_parse_token_suffix(blxeq_tok, &base_token);
  UT_ASSERT_EQ(cond, 0); /* eq -> 0 */
  UT_ASSERT_EQ(base_token, blx_tok);
  return 0;
}

UT_TEST(test_token_suffix_width_only_on_two_char_base)
{
  int bx_tok = tok_alloc("bx", 2)->tok;
  int bx_w_tok = set_special_reg_tok("bx.w");
  int base_token = -1;
  int cond = thumb_parse_token_suffix(bx_w_tok, &base_token);
  UT_ASSERT_EQ(cond, 14);
  UT_ASSERT_EQ(base_token, bx_tok);
  return 0;
}

UT_TEST(test_token_suffix_remaining_condition_codes)
{
  int base_token = -1;
  UT_ASSERT_EQ(thumb_parse_token_suffix(set_special_reg_tok("movmi"), &base_token), 4);
  UT_ASSERT_EQ(thumb_parse_token_suffix(set_special_reg_tok("movpl"), &base_token), 5);
  UT_ASSERT_EQ(thumb_parse_token_suffix(set_special_reg_tok("movvs"), &base_token), 6);
  UT_ASSERT_EQ(thumb_parse_token_suffix(set_special_reg_tok("movvc"), &base_token), 7);
  UT_ASSERT_EQ(thumb_parse_token_suffix(set_special_reg_tok("movhi"), &base_token), 8);
  UT_ASSERT_EQ(thumb_parse_token_suffix(set_special_reg_tok("movls"), &base_token), 9);
  UT_ASSERT_EQ(thumb_parse_token_suffix(set_special_reg_tok("movge"), &base_token), 10);
  UT_ASSERT_EQ(thumb_parse_token_suffix(set_special_reg_tok("movlt"), &base_token), 11);
  UT_ASSERT_EQ(thumb_parse_token_suffix(set_special_reg_tok("movle"), &base_token), 13);
  return 0;
}

/* ============================================================ */
/*  tcc_asm_set_fpu() -- remaining names                         */
/* ============================================================ */

UT_TEST(test_fpu_enable_fpv5_sp_d16)
{
  setup_armv8m_main();
  tcc_asm_set_fpu("fpv5-sp-d16");
  UT_ASSERT_EQ(arm_target_dependent.feat.vfp_sp, 1);
  UT_ASSERT_EQ(arm_target_dependent.feat.vfp_dp, 0);
  return 0;
}

UT_TEST(test_fpu_enable_fpv5_d32)
{
  setup_armv8m_main();
  tcc_asm_set_fpu("fpv5-d32");
  UT_ASSERT_EQ(arm_target_dependent.feat.vfp_sp, 1);
  UT_ASSERT_EQ(arm_target_dependent.feat.vfp_dp, 1);
  return 0;
}

UT_TEST(test_fpu_enable_fp_armv8_full)
{
  setup_armv8m_main();
  tcc_asm_set_fpu("fp-armv8-full");
  UT_ASSERT_EQ(arm_target_dependent.feat.vfp_sp, 1);
  UT_ASSERT_EQ(arm_target_dependent.feat.vfp_dp, 1);
  UT_ASSERT_EQ(arm_target_dependent.feat.fp_armv8, 1);
  return 0;
}

UT_TEST(test_fpu_none_is_noop)
{
  setup_armv8m_main();
  int t32_before = arm_target_dependent.feat.t32;
  tcc_asm_set_fpu("none");
  UT_ASSERT_EQ(arm_target_dependent.feat.vfp_sp, 0);
  UT_ASSERT_EQ(arm_target_dependent.feat.t32, t32_before);
  return 0;
}

/* ============================================================ */
/*  asm_compute_constraints() -- additional reachable branches     */
/* ============================================================ */

UT_TEST(test_constraints_output_equals_modifier)
{
  ASMOperand op;
  SValue sv;
  uint8_t clobber[NB_ASM_REGS] = {0};
  uint8_t reserved[NB_ASM_REGS] = {0};
  int out_reg = -1;
  make_asm_operand(&op, &sv, "=r", 0);
  asm_compute_constraints(&op, 1, 1, clobber, reserved, &out_reg);
  UT_ASSERT_EQ(op.reg, 0);
  return 0;
}

UT_TEST(test_constraints_m_input_local_no_memory_flag)
{
  /* "m" on an input operand that is VT_LOCAL (not VT_LLOCAL) does not
     allocate a register or set is_memory. */
  ASMOperand op;
  SValue sv;
  uint8_t clobber[NB_ASM_REGS] = {0};
  uint8_t reserved[NB_ASM_REGS] = {0};
  int out_reg = -1;
  make_asm_operand(&op, &sv, "m", VT_LOCAL);
  asm_compute_constraints(&op, 1, 0, clobber, reserved, &out_reg);
  UT_ASSERT_EQ(op.reg, -1);
  UT_ASSERT_EQ(op.is_memory, 0);
  return 0;
}

UT_TEST(test_constraints_g_input_no_register)
{
  ASMOperand op;
  SValue sv;
  uint8_t clobber[NB_ASM_REGS] = {0};
  uint8_t reserved[NB_ASM_REGS] = {0};
  int out_reg = -1;
  make_asm_operand(&op, &sv, "g", VT_LOCAL);
  asm_compute_constraints(&op, 1, 0, clobber, reserved, &out_reg);
  UT_ASSERT_EQ(op.reg, -1);
  return 0;
}

UT_TEST(test_constraints_X_input_no_register)
{
  ASMOperand op;
  SValue sv;
  uint8_t clobber[NB_ASM_REGS] = {0};
  uint8_t reserved[NB_ASM_REGS] = {0};
  int out_reg = -1;
  make_asm_operand(&op, &sv, "X", VT_LOCAL);
  asm_compute_constraints(&op, 1, 0, clobber, reserved, &out_reg);
  UT_ASSERT_EQ(op.reg, -1);
  return 0;
}

UT_TEST(test_constraints_Q_input_no_register)
{
  ASMOperand op;
  SValue sv;
  uint8_t clobber[NB_ASM_REGS] = {0};
  uint8_t reserved[NB_ASM_REGS] = {0};
  int out_reg = -1;
  make_asm_operand(&op, &sv, "Q", VT_LOCAL);
  asm_compute_constraints(&op, 1, 0, clobber, reserved, &out_reg);
  UT_ASSERT_EQ(op.reg, -1);
  return 0;
}

/* ============================================================ */
/*  thumb_generate_opcode_for_data_processing() -- wide qualifier  */
/* ============================================================ */

UT_TEST(test_dispatch_mov_wide_qualifier_forces_32bit)
{
  setup_armv8m_main();
  int base;
  int mov_w_tok = set_special_reg_tok("mov.w");
  thumb_parse_token_suffix(mov_w_tok, &base); /* sets WIDTH_WIDE */
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 0, 3);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_mov, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want =
      th_mov_reg(0, 3, FLAGS_BEHAVIOUR_BLOCK, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_32BIT, /*in_it=*/false);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

UT_TEST(test_dispatch_and_wide_qualifier_forces_32bit)
{
  setup_armv8m_main();
  int base;
  int and_w_tok = set_special_reg_tok("and.w");
  thumb_parse_token_suffix(and_w_tok, &base); /* sets WIDTH_WIDE */
  Operand ops[3];
  ops_reg_reg_reg(ops, 0, 1, 2);
  thumb_opcode got = thumb_generate_opcode_for_data_processing(TOK_ASM_and, THUMB_SHIFT_DEFAULT, ops);
  thumb_opcode want = th_and_reg(0, 1, 2, FLAGS_BEHAVIOUR_BLOCK, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_32BIT);
  UT_ASSERT(opcode_eq(got, want));
  return 0;
}

/* ============================================================ */
/*  subst_asm_operand()                                            */
/* ============================================================ */

static void seed_subst_reg_names(void)
{
  utb_set_tok_str(TOK_ASM_r0, "r0");
  utb_set_tok_str(TOK_ASM_r1, "r1");
  utb_set_tok_str(TOK_ASM_r2, "r2");
  utb_set_tok_str(TOK_ASM_r3, "r3");
  utb_set_tok_str(TOK_ASM_r4, "r4");
  utb_set_tok_str(TOK_ASM_r5, "r5");
  utb_set_tok_str(TOK_ASM_r6, "r6");
  utb_set_tok_str(TOK_ASM_r7, "r7");
  utb_set_tok_str(TOK_ASM_r8, "r8");
  utb_set_tok_str(TOK_ASM_r9, "r9");
  utb_set_tok_str(TOK_ASM_r10, "r10");
  utb_set_tok_str(TOK_ASM_r11, "r11");
  utb_set_tok_str(TOK_ASM_r12, "r12");
  utb_set_tok_str(TOK_ASM_r13, "r13");
  utb_set_tok_str(TOK_ASM_r14, "r14");
  utb_set_tok_str(TOK_ASM_r15, "r15");
}

static const char *subst_operand_to_string(SValue *sv, int modifier)
{
  static CString cs;
  cstr_free(&cs);
  cstr_new(&cs);
  subst_asm_operand(&cs, sv, modifier);
  cstr_ccat(&cs, '\0');
  return cs.data;
}

static void make_sv_const(SValue *sv, int32_t value)
{
  memset(sv, 0, sizeof(*sv));
  sv->r = VT_CONST;
  sv->c.i = value;
}

static void make_sv_const_sym(SValue *sv, Sym *sym, int32_t offset)
{
  memset(sv, 0, sizeof(*sv));
  sv->r = VT_CONST | VT_SYM;
  sv->sym = sym;
  sv->c.i = offset;
}

static void make_sv_local(SValue *sv, int32_t offset)
{
  memset(sv, 0, sizeof(*sv));
  sv->r = VT_LOCAL;
  sv->c.i = offset;
}

static void make_sv_lval_reg(SValue *sv, int reg)
{
  memset(sv, 0, sizeof(*sv));
  sv->r = VT_LVAL | reg;
}

static void make_sv_reg(SValue *sv, int reg, int type)
{
  memset(sv, 0, sizeof(*sv));
  sv->r = reg;
  sv->type.t = type;
}

UT_TEST(test_subst_const_default)
{
  seed_subst_reg_names();
  SValue sv;
  make_sv_const(&sv, 42);
  UT_ASSERT_STREQ(subst_operand_to_string(&sv, 0), "#42");
  return 0;
}

UT_TEST(test_subst_const_c_modifier)
{
  seed_subst_reg_names();
  SValue sv;
  make_sv_const(&sv, 42);
  UT_ASSERT_STREQ(subst_operand_to_string(&sv, 'c'), "42");
  return 0;
}

UT_TEST(test_subst_const_P_modifier)
{
  seed_subst_reg_names();
  SValue sv;
  make_sv_const(&sv, 42);
  UT_ASSERT_STREQ(subst_operand_to_string(&sv, 'P'), "42");
  return 0;
}

UT_TEST(test_subst_const_n_modifier_known_bug)
{
  /* Known bug: modifier 'n' should print a negated immediate (e.g. #-42),
     but the implementation both omits the leading '#' (because the guard
     `modifier != 'n'` suppresses it) and fails to use the negated `val` it
     computed, printing sv->c.i unchanged. Current output is therefore the
     bare positive number. Flip the assertion to "#-42" once fixed. */
  seed_subst_reg_names();
  SValue sv;
  make_sv_const(&sv, 42);
  UT_ASSERT_STREQ(subst_operand_to_string(&sv, 'n'), "42");
  return 0;
}

UT_TEST(test_subst_const_lval_no_hash)
{
  seed_subst_reg_names();
  SValue sv;
  memset(&sv, 0, sizeof(sv));
  sv.r = VT_CONST | VT_LVAL;
  sv.c.i = 42;
  UT_ASSERT_STREQ(subst_operand_to_string(&sv, 0), "42");
  return 0;
}

UT_TEST(test_subst_const_sym_zero_offset)
{
  seed_subst_reg_names();
  SValue sv;
  Sym sym;
  memset(&sym, 0, sizeof(sym));
  sym.v = set_special_reg_tok("myvar");
  make_sv_const_sym(&sv, &sym, 0);
  UT_ASSERT_STREQ(subst_operand_to_string(&sv, 0), "#myvar");
  return 0;
}

UT_TEST(test_subst_const_sym_nonzero_offset)
{
  seed_subst_reg_names();
  SValue sv;
  Sym sym;
  memset(&sym, 0, sizeof(sym));
  sym.v = set_special_reg_tok("myvar");
  make_sv_const_sym(&sv, &sym, 8);
  UT_ASSERT_STREQ(subst_operand_to_string(&sv, 0), "#myvar+8");
  return 0;
}

UT_TEST(test_subst_const_sym_leading_underscore)
{
  seed_subst_reg_names();
  tcc_state->leading_underscore = 1;
  SValue sv;
  Sym sym;
  memset(&sym, 0, sizeof(sym));
  sym.v = set_special_reg_tok("myvar");
  make_sv_const_sym(&sv, &sym, 0);
  UT_ASSERT_STREQ(subst_operand_to_string(&sv, 0), "#_myvar");
  tcc_state->leading_underscore = 0;
  return 0;
}

UT_TEST(test_subst_local)
{
  seed_subst_reg_names();
  SValue sv;
  make_sv_local(&sv, -16);
  UT_ASSERT_STREQ(subst_operand_to_string(&sv, 0), "[fp,#-16]");
  return 0;
}

UT_TEST(test_subst_lval_reg)
{
  seed_subst_reg_names();
  SValue sv;
  make_sv_lval_reg(&sv, 3);
  UT_ASSERT_STREQ(subst_operand_to_string(&sv, 0), "[r3]");
  return 0;
}

UT_TEST(test_subst_reg_default)
{
  seed_subst_reg_names();
  SValue sv;
  make_sv_reg(&sv, 5, VT_INT);
  UT_ASSERT_STREQ(subst_operand_to_string(&sv, 0), "r5");
  return 0;
}

UT_TEST(test_subst_reg_byte_type)
{
  seed_subst_reg_names();
  SValue sv;
  make_sv_reg(&sv, 2, VT_BYTE);
  UT_ASSERT_STREQ(subst_operand_to_string(&sv, 0), "r2");
  return 0;
}

UT_TEST(test_subst_reg_short_type)
{
  seed_subst_reg_names();
  SValue sv;
  make_sv_reg(&sv, 2, VT_SHORT);
  UT_ASSERT_STREQ(subst_operand_to_string(&sv, 0), "r2");
  return 0;
}

UT_TEST(test_subst_reg_b_modifier)
{
  seed_subst_reg_names();
  SValue sv;
  make_sv_reg(&sv, 2, VT_INT);
  UT_ASSERT_STREQ(subst_operand_to_string(&sv, 'b'), "r2");
  return 0;
}

UT_TEST(test_subst_reg_w_modifier)
{
  seed_subst_reg_names();
  SValue sv;
  make_sv_reg(&sv, 2, VT_INT);
  UT_ASSERT_STREQ(subst_operand_to_string(&sv, 'w'), "r2");
  return 0;
}

UT_TEST(test_subst_reg_k_modifier)
{
  seed_subst_reg_names();
  SValue sv;
  make_sv_reg(&sv, 2, VT_INT);
  UT_ASSERT_STREQ(subst_operand_to_string(&sv, 'k'), "r2");
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(arm_thumb_asm)
{
  /* asm_parse_regvar */
  UT_RUN(test_parse_regvar_low_regs);
  UT_RUN(test_parse_regvar_r11_r15_direct);
  UT_RUN(test_parse_regvar_named_aliases);
  UT_RUN(test_parse_regvar_vfp_single);
  UT_RUN(test_parse_regvar_vfp_double);
  UT_RUN(test_parse_regvar_non_register_token_is_invalid);

  /* thumb_parse_special_register[_mask] */
  UT_RUN(test_special_register_apsr);
  UT_RUN(test_special_register_iapsr_checked_before_apsr);
  UT_RUN(test_special_register_msp_family_ns_before_plain);
  UT_RUN(test_special_register_privilege_family);
  UT_RUN(test_special_register_uppercase_is_lowered);
  UT_RUN(test_special_register_unknown_name);
  UT_RUN(test_special_register_mask_variants);

  /* thumb_parse_token_suffix */
  UT_RUN(test_token_suffix_no_suffix_defaults_to_al);
  UT_RUN(test_token_suffix_condition_eq);
  UT_RUN(test_token_suffix_condition_ne_with_width);
  UT_RUN(test_token_suffix_condition_gt);
  UT_RUN(test_token_suffix_bx_two_char_base_with_condition);
  UT_RUN(test_token_suffix_width_only_no_condition);
  UT_RUN(test_token_suffix_narrow_qualifier);
  UT_RUN(test_token_suffix_condition_aliases_cs_cc);
  UT_RUN(test_token_suffix_one_char_base_with_condition);
  UT_RUN(test_token_suffix_unknown_suffix_returns_al);
  UT_RUN(test_token_suffix_uppercase_width);
  UT_RUN(test_token_suffix_three_char_base_with_condition);
  UT_RUN(test_token_suffix_width_only_on_two_char_base);
  UT_RUN(test_token_suffix_remaining_condition_codes);

  /* tcc_asm_set_fpu */
  UT_RUN(test_fpu_enable_vfpv4_sp_d16);
  UT_RUN(test_fpu_enable_fpv5_d16);
  UT_RUN(test_fpu_enable_fpv5_sp_d16);
  UT_RUN(test_fpu_enable_fpv5_d32);
  UT_RUN(test_fpu_enable_fp_armv8_full);
  UT_RUN(test_fpu_none_is_noop);

  /* asm_clobber */
  UT_RUN(test_clobber_register_sets_bit);
  UT_RUN(test_clobber_alias_lr);
  UT_RUN(test_clobber_memory_cc_flags_are_noops);

  /* asm_compute_constraints */
  UT_RUN(test_constraints_single_output_register);
  UT_RUN(test_constraints_single_input_register);
  UT_RUN(test_constraints_output_then_input_pair);
  UT_RUN(test_constraints_read_write_modifier);
  UT_RUN(test_constraints_memory_operand_llocal);
  UT_RUN(test_constraints_immediate_operand_no_register);
  UT_RUN(test_constraints_specific_register_via_local_sym);
  UT_RUN(test_constraints_reserved_regs_skipped);
  UT_RUN(test_constraints_priority_specific_register_before_general);
  UT_RUN(test_constraints_clobber_prevents_allocation);
  UT_RUN(test_constraints_alternative_immediate_chosen);
  UT_RUN(test_constraints_alternative_register_fallback);
  UT_RUN(test_constraints_I_immediate_no_register);
  UT_RUN(test_constraints_M_immediate_no_register);
  UT_RUN(test_constraints_memory_output_llocal);
  UT_RUN(test_constraints_out_reg_for_llocal_output);
  UT_RUN(test_constraints_output_equals_modifier);
  UT_RUN(test_constraints_m_input_local_no_memory_flag);
  UT_RUN(test_constraints_g_input_no_register);
  UT_RUN(test_constraints_X_input_no_register);
  UT_RUN(test_constraints_Q_input_no_register);

  /* thumb_generate_opcode_for_data_processing (direct switch cases) */
  UT_RUN(test_dispatch_adds_imm_sets_flags);
  UT_RUN(test_dispatch_add_imm_unconditional_forces_32bit);
  UT_RUN(test_dispatch_add_reg);
  UT_RUN(test_dispatch_add_imm_sp_base_uses_sp_form);
  UT_RUN(test_dispatch_addw_imm_uses_addw_encoder);
  UT_RUN(test_dispatch_addw_sp_base);
  UT_RUN(test_dispatch_subs_imm_sets_flags);
  UT_RUN(test_dispatch_sub_reg_sp_base);
  UT_RUN(test_dispatch_subw_imm);
  UT_RUN(test_dispatch_sub_imm_unconditional_forces_32bit);
  UT_RUN(test_dispatch_mov_imm_block_flags);
  UT_RUN(test_dispatch_movs_imm_sets_flags);
  UT_RUN(test_dispatch_movw_imm_forces_32bit);
  UT_RUN(test_dispatch_mov_reg);
  UT_RUN(test_dispatch_mov_wide_qualifier_forces_32bit);
  UT_RUN(test_dispatch_and_wide_qualifier_forces_32bit);
  UT_RUN(test_dispatch_cmp_imm_always_sets_flags);
  UT_RUN(test_dispatch_cmp_reg);
  UT_RUN(test_dispatch_cmn_imm);
  UT_RUN(test_dispatch_cmn_reg);
  UT_RUN(test_dispatch_cmn_reg_wide_qualifier_forces_32bit);
  UT_RUN(test_dispatch_teq_imm);
  UT_RUN(test_dispatch_tst_imm);
  UT_RUN(test_dispatch_tst_reg);
  UT_RUN(test_dispatch_clz);
  UT_RUN(test_dispatch_rbit);
  UT_RUN(test_dispatch_rev_family);
  UT_RUN(test_dispatch_sxtb_sxth_uxtb_uxth);
  UT_RUN(test_dispatch_bfc);
  UT_RUN(test_dispatch_mul_no_swap_when_rd_ne_rn);
  UT_RUN(test_dispatch_mul_swaps_operands_when_rd_eq_rn);
  UT_RUN(test_dispatch_muls_sets_flags);
  UT_RUN(test_dispatch_sdiv_udiv);
  UT_RUN(test_dispatch_uadd8_usub8_sel);
  UT_RUN(test_dispatch_unknown_token_returns_zero_opcode);

  /* thumb_process_generic_data_op-routed mnemonics */
  UT_RUN(test_dispatch_and_imm_and_reg);
  UT_RUN(test_dispatch_ands_sets_flags);
  UT_RUN(test_dispatch_orr_imm_and_reg);
  UT_RUN(test_dispatch_orn_imm_and_reg);
  UT_RUN(test_dispatch_eor_imm_and_reg);
  UT_RUN(test_dispatch_bic_imm_and_reg);
  UT_RUN(test_dispatch_rsb_imm_and_reg);
  UT_RUN(test_dispatch_mvn_imm_and_reg);
  UT_RUN(test_dispatch_adc_imm_and_reg);
  UT_RUN(test_dispatch_sbc_imm_and_reg);
  UT_RUN(test_dispatch_adcs_sets_flags);
  UT_RUN(test_dispatch_orrs_sets_flags);
  UT_RUN(test_dispatch_orns_sets_flags);
  UT_RUN(test_dispatch_eors_sets_flags);
  UT_RUN(test_dispatch_bics_sets_flags);
  UT_RUN(test_dispatch_mvns_sets_flags);
  UT_RUN(test_dispatch_rsbs_sets_flags);
  UT_RUN(test_dispatch_sbcs_sets_flags);
  UT_RUN(test_dispatch_generic_op_returns_zero_when_operand_neither_imm_nor_reg);

  /* thumb_process_generic_data_op() direct */
  UT_RUN(test_process_generic_data_op_reg_forces_32bit_outside_it);
  UT_RUN(test_process_generic_data_op_ands_sets_flags_no_force);
  UT_RUN(test_process_generic_data_op_wide_qualifier_forces_32bit);
  UT_RUN(test_process_generic_data_op_imm_path);

  /* subst_asm_operand */
  UT_RUN(test_subst_const_default);
  UT_RUN(test_subst_const_c_modifier);
  UT_RUN(test_subst_const_P_modifier);
  UT_RUN(test_subst_const_n_modifier_known_bug);
  UT_RUN(test_subst_const_lval_no_hash);
  UT_RUN(test_subst_const_sym_zero_offset);
  UT_RUN(test_subst_const_sym_nonzero_offset);
  UT_RUN(test_subst_const_sym_leading_underscore);
  UT_RUN(test_subst_local);
  UT_RUN(test_subst_lval_reg);
  UT_RUN(test_subst_reg_default);
  UT_RUN(test_subst_reg_byte_type);
  UT_RUN(test_subst_reg_short_type);
  UT_RUN(test_subst_reg_b_modifier);
  UT_RUN(test_subst_reg_w_modifier);
  UT_RUN(test_subst_reg_k_modifier);
}
