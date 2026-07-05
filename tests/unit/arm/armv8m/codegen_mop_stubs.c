/*
 *  codegen_mop_stubs.c - backend-mop stub layer for ir/codegen.c dispatch tests
 *
 *  tcc_ir_codegen_generate() (ir/codegen.c) dispatches every IR instruction to
 *  one of ~76 tcc_gen_machine_*_mop() functions normally implemented in
 *  arm-thumb-gen.c. That file is 13k+ lines and drags in ELF/section/frontend
 *  machinery this unit-test binary doesn't provide, so it isn't linked here.
 *  These stubs record every call (mop name, IR op, operand kinds/vregs)
 *  instead of emitting real machine code, so tests can assert dispatch-level
 *  facts without linking the real backend. A handful of functions whose
 *  return value feeds codegen.c's own control flow (branch sizing, dry-run
 *  scratch bookkeeping) read from a small settable "knobs" struct instead of
 *  a fixed value.
 *
 *  Fusion/peephole functions (try_strd/ldrd_*, subs_eq_select_01,
 *  mul_const_add_fused_mop, mlal_accum_mop) always return 0 (record the
 *  attempt, take the always-tested fallback path). The real fused encoding
 *  logic lives in arm-thumb-gen.c, not codegen.c, so this costs nothing in
 *  codegen.c coverage -- see docs/plan_codegen_unit_tests.md for the
 *  documented-gap rationale.
 */

#define USING_GLOBALS
#include "tcc.h"
#include "codegen_mop_stubs.h"

#define CGSTUB_MAX_CALLS 8192

static CgStubCall cgstub_log[CGSTUB_MAX_CALLS];
static int cgstub_log_count;
/* 0 while inside a dry-run pass, 1 otherwise (real-run, or the single pass
 * when can_skip_dry_run means dry_run_start/end are never called at all). */
static int cgstub_current_pass = 1;

typedef struct CgStubKnobs
{
  int branch_size_16;
  int switch_table_entry_size;
  int switch_load_entry_size;
  int lr_push_count;
  unsigned int scratch_regs_pushed;
  int insn_scratch_count;
  unsigned short insn_scratch_saves_mask;
} CgStubKnobs;

static CgStubKnobs cgstub_knobs;
static CgStubLastProlog cgstub_last_prolog;

static const MachineOperand CGSTUB_NO_OP; /* zero-init: kind == MACH_OP_NONE (0), vreg == 0 */

/* Fake frontend value-stack globals -- defined below (see "Non-mop link
 * dependencies"); forward-declared here so cgstub_reset() can reset vtop to
 * empty alongside every other stub knob. `_vstack` has no declaration in
 * tcc.h (only a local `extern SValue _vstack[];` inside the one ir/codegen.c
 * function that needs it), so it needs its own extern here too, unlike
 * `vtop` (already ST_DATA-declared by tcc.h). */
extern SValue _vstack[];

void cgstub_reset(void)
{
  cgstub_log_count = 0;
  cgstub_current_pass = 1;
  memset(&cgstub_knobs, 0, sizeof(cgstub_knobs));
  cgstub_knobs.switch_table_entry_size = 4;
  cgstub_knobs.switch_load_entry_size = 4;
  memset(&cgstub_last_prolog, 0, sizeof(cgstub_last_prolog));
  vtop = _vstack; /* empty fake value-stack */
  /* nocode_wanted (stubs.c) gates tcc_ir_put() itself (`if (nocode_wanted &
   * ~0x20000000) return -1;` -- silently drops the instruction): reset here,
   * not just at the end of the one test that sets it, so an assertion
   * failure partway through that test (an early `return -1` from
   * UT_ASSERT/UT_ASSERT_EQ, skipping any end-of-test manual cleanup) can't
   * leak a nonzero value into every later test in the binary. */
  nocode_wanted = 0;
}

int cgstub_total_calls(void)
{
  return cgstub_log_count;
}

int cgstub_call_count(const char *mop_name)
{
  int n = 0;
  for (int i = 0; i < cgstub_log_count; i++)
    if (!strcmp(cgstub_log[i].mop_name, mop_name))
      n++;
  return n;
}

int cgstub_call_count_pass(const char *mop_name, int pass)
{
  int n = 0;
  for (int i = 0; i < cgstub_log_count; i++)
    if (cgstub_log[i].pass == pass && !strcmp(cgstub_log[i].mop_name, mop_name))
      n++;
  return n;
}

const CgStubCall *cgstub_nth_call(const char *mop_name, int n)
{
  int k = 0;
  for (int i = 0; i < cgstub_log_count; i++)
  {
    if (!strcmp(cgstub_log[i].mop_name, mop_name))
    {
      if (k == n)
        return &cgstub_log[i];
      k++;
    }
  }
  return NULL;
}

const CgStubCall *cgstub_nth_call_any(int n)
{
  if (n < 0 || n >= cgstub_log_count)
    return NULL;
  return &cgstub_log[n];
}

void cgstub_set_branch_size_16(int enable)
{
  cgstub_knobs.branch_size_16 = enable;
}

void cgstub_set_switch_entry_sizes(int table_entry_size, int load_entry_size)
{
  cgstub_knobs.switch_table_entry_size = table_entry_size;
  cgstub_knobs.switch_load_entry_size = load_entry_size;
}

void cgstub_set_lr_push_count(int n)
{
  cgstub_knobs.lr_push_count = n;
}

void cgstub_set_scratch_regs_pushed(unsigned int mask)
{
  cgstub_knobs.scratch_regs_pushed = mask;
}

void cgstub_set_next_insn_scratch(int count, unsigned short saves_mask)
{
  cgstub_knobs.insn_scratch_count = count;
  cgstub_knobs.insn_scratch_saves_mask = saves_mask;
}

const CgStubLastProlog *cgstub_get_last_prolog(void)
{
  return &cgstub_last_prolog;
}

/* --------------------------------------------------------------------------
 * Call-log recording helpers
 * -------------------------------------------------------------------------- */

static void cgstub_push(const char *name, TccIrOp op,
                        MachineOperandKind dest_kind, int dest_vreg,
                        MachineOperandKind src1_kind, int src1_vreg,
                        MachineOperandKind src2_kind, int src2_vreg,
                        int aux0, int aux1)
{
  if (cgstub_log_count >= CGSTUB_MAX_CALLS)
    return;
  CgStubCall *c = &cgstub_log[cgstub_log_count++];
  c->mop_name = name;
  c->ir_op = op;
  c->pass = cgstub_current_pass;
  c->dest_kind = dest_kind;
  c->dest_vreg = dest_vreg;
  c->src1_kind = src1_kind;
  c->src1_vreg = src1_vreg;
  c->src2_kind = src2_kind;
  c->src2_vreg = src2_vreg;
  c->aux0 = aux0;
  c->aux1 = aux1;
}

static void cgstub_record_ex(const char *name, TccIrOp op, MachineOperand dest, MachineOperand src1,
                             MachineOperand src2, int aux0, int aux1)
{
  cgstub_push(name, op, dest.kind, dest.vreg, src1.kind, src1.vreg, src2.kind, src2.vreg, aux0, aux1);
}

static void cgstub_record(const char *name, TccIrOp op, MachineOperand dest, MachineOperand src1,
                          MachineOperand src2)
{
  cgstub_record_ex(name, op, dest, src1, src2, 0, 0);
}

/* ============================================================================
 * Data processing / arithmetic
 * ============================================================================ */

void tcc_gen_machine_data_processing_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest,
                                         TccIrOp op, uint32_t barrel_shift)
{
  (void)barrel_shift;
  cgstub_record("data_processing_mop", op, dest, src1, src2);
}

void tcc_gen_machine_data_processing_mop_flags(MachineOperand src1, MachineOperand src2, MachineOperand dest,
                                               TccIrOp op)
{
  cgstub_record("data_processing_mop_flags", op, dest, src1, src2);
}

void tcc_gen_machine_cmp_eq64_mop(MachineOperand src1, MachineOperand src2)
{
  cgstub_record("cmp_eq64_mop", (TccIrOp)-1, CGSTUB_NO_OP, src1, src2);
}

int tcc_gen_machine_subs_eq_select_01(MachineOperand src1, MachineOperand src2, MachineOperand dest)
{
  cgstub_record("subs_eq_select_01", (TccIrOp)-1, dest, src1, src2);
  return 0;
}

void tcc_gen_machine_ubfx_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest)
{
  cgstub_record("ubfx_mop", (TccIrOp)-1, dest, src1, src2);
}

void tcc_gen_machine_bfi_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, uint32_t params)
{
  (void)params;
  cgstub_record("bfi_mop", (TccIrOp)-1, dest, src1, src2);
}

void tcc_gen_machine_assign_mop(MachineOperand src, MachineOperand dest, TccIrOp op)
{
  cgstub_record("assign_mop", op, dest, src, CGSTUB_NO_OP);
}

void tcc_gen_machine_pack64_mop(MachineOperand src_lo, MachineOperand src_hi, MachineOperand dest)
{
  cgstub_record("pack64_mop", (TccIrOp)-1, dest, src_lo, src_hi);
}

void tcc_gen_machine_setif_mop(MachineOperand src, MachineOperand dest, TccIrOp op)
{
  cgstub_record("setif_mop", op, dest, src, CGSTUB_NO_OP);
}

void tcc_gen_machine_bool_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op)
{
  cgstub_record("bool_mop", op, dest, src1, src2);
}

void tcc_gen_machine_muldiv_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op)
{
  cgstub_record("muldiv_mop", op, dest, src1, src2);
}

int tcc_gen_machine_mul_const_add_fused_mop(MachineOperand mul_var, int64_t mul_const, MachineOperand mul_dest,
                                            MachineOperand add_base, MachineOperand add_dest)
{
  (void)mul_const;
  (void)add_dest;
  cgstub_record("mul_const_add_fused_mop", (TccIrOp)-1, mul_dest, mul_var, add_base);
  return 0;
}

void tcc_gen_machine_mla_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, MachineOperand accum)
{
  (void)accum;
  cgstub_record("mla_mop", (TccIrOp)-1, dest, src1, src2);
}

void tcc_gen_machine_umull_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest)
{
  cgstub_record("umull_mop", (TccIrOp)-1, dest, src1, src2);
}

void tcc_gen_machine_smull_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest)
{
  cgstub_record("smull_mop", (TccIrOp)-1, dest, src1, src2);
}

int tcc_gen_machine_mlal_accum_mop(MachineOperand src1, MachineOperand src2, MachineOperand accum,
                                   MachineOperand dest, int is_signed)
{
  (void)accum;
  (void)is_signed;
  cgstub_record("mlal_accum_mop", (TccIrOp)-1, dest, src1, src2);
  return 0;
}

void tcc_gen_machine_fp_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op,
                            int is_complex)
{
  cgstub_record_ex("fp_mop", op, dest, src1, src2, is_complex, 0);
}

void tcc_gen_machine_vla_mop(MachineOperand dest, MachineOperand src1, MachineOperand src2, TccIrOp op)
{
  cgstub_record("vla_mop", op, dest, src1, src2);
}

/* ============================================================================
 * Load / store / addressing
 * ============================================================================ */

void tcc_gen_machine_load_mop(MachineOperand src, MachineOperand dest, TccIrOp op)
{
  cgstub_record("load_mop", op, dest, src, CGSTUB_NO_OP);
}

void tcc_gen_machine_store_mop(MachineOperand dest, MachineOperand src, TccIrOp op)
{
  cgstub_record("store_mop", op, dest, src, CGSTUB_NO_OP);
}

int tcc_gen_machine_try_strd_spill(int reg1, int32_t off1, int reg2, int32_t off2)
{
  (void)off1;
  (void)off2;
  cgstub_push("try_strd_spill", (TccIrOp)-1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, reg1, reg2);
  return 0;
}

int tcc_gen_machine_try_ldrd_spill(int reg1, int32_t off1, int reg2, int32_t off2)
{
  (void)off1;
  (void)off2;
  cgstub_push("try_ldrd_spill", (TccIrOp)-1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, reg1, reg2);
  return 0;
}

int tcc_gen_machine_try_ldrd_base(int reg1, int reg2, int base_reg, int32_t off)
{
  (void)base_reg;
  (void)off;
  cgstub_push("try_ldrd_base", (TccIrOp)-1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, reg1, reg2);
  return 0;
}

int tcc_gen_machine_try_strd_base(int reg1, int reg2, int base_reg, int32_t off)
{
  (void)base_reg;
  (void)off;
  cgstub_push("try_strd_base", (TccIrOp)-1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, reg1, reg2);
  return 0;
}

int tcc_gen_machine_try_strd_imm_spill(int64_t val1, int64_t val2, int32_t off1, int32_t off2)
{
  (void)val1;
  (void)val2;
  (void)off1;
  (void)off2;
  cgstub_record("try_strd_imm_spill", (TccIrOp)-1, CGSTUB_NO_OP, CGSTUB_NO_OP, CGSTUB_NO_OP);
  return 0;
}

int tcc_gen_machine_try_strd_imm_base(int64_t val1, int64_t val2, int base_reg, int32_t off)
{
  (void)val1;
  (void)val2;
  (void)base_reg;
  (void)off;
  cgstub_record("try_strd_imm_base", (TccIrOp)-1, CGSTUB_NO_OP, CGSTUB_NO_OP, CGSTUB_NO_OP);
  return 0;
}

void tcc_gen_machine_load_indexed_mop(MachineOperand dest, MachineOperand base, MachineOperand index,
                                      MachineOperand scale, TccIrOp op)
{
  (void)scale;
  cgstub_record("load_indexed_mop", op, dest, base, index);
}

void tcc_gen_machine_store_indexed_mop(MachineOperand base, MachineOperand index, MachineOperand scale,
                                       MachineOperand value, TccIrOp op)
{
  (void)scale;
  /* aux0 carries value.u.imm.val when the stored value is an immediate --
   * needed to oracle-assert the merged 32-bit constant the byte-to-word
   * coalescing peephole produces (ir/codegen.c ~3650-3792), since
   * CgStubCall's kind/vreg fields alone can't distinguish "some immediate"
   * from "the specific merged word". */
  int aux0 = (value.kind == MACH_OP_IMM) ? (int)value.u.imm.val : 0;
  cgstub_record_ex("store_indexed_mop", op, base, index, value, aux0, 0);
}

void tcc_gen_machine_load_postinc_mop(MachineOperand dest, MachineOperand ptr, MachineOperand offset, TccIrOp op)
{
  cgstub_record("load_postinc_mop", op, dest, ptr, offset);
}

void tcc_gen_machine_store_postinc_mop(MachineOperand ptr, MachineOperand value, MachineOperand offset,
                                       TccIrOp op)
{
  cgstub_record("store_postinc_mop", op, ptr, value, offset);
}

void tcc_gen_machine_lea_mop(MachineOperand dest, MachineOperand src)
{
  cgstub_record("lea_mop", (TccIrOp)-1, dest, src, CGSTUB_NO_OP);
}

void tcc_gen_machine_block_copy_mop(TCCIRState *ir, IROperand dest, IROperand src, int size)
{
  (void)ir;
  cgstub_push("block_copy_mop", (TccIrOp)-1, MACH_OP_NONE, irop_get_vreg(dest), MACH_OP_NONE, irop_get_vreg(src),
             MACH_OP_NONE, -1, size, 0);
}

void tcc_gen_machine_spill_block_copy(int32_t src_spill_off, int32_t dst_spill_off, int nwords)
{
  (void)src_spill_off;
  (void)dst_spill_off;
  cgstub_push("spill_block_copy", (TccIrOp)-1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, nwords, 0);
}

/* ============================================================================
 * Control flow / jumps / switch
 * ============================================================================ */

void tcc_gen_machine_indirect_jump_mop(MachineOperand src, TccIrOp op)
{
  cgstub_record("indirect_jump_mop", op, CGSTUB_NO_OP, src, CGSTUB_NO_OP);
}

int tcc_gen_machine_jump_mop(TccIrOp op, int32_t target_ir, int ir_idx)
{
  cgstub_push("jump_mop", op, MACH_OP_NONE, -1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, target_ir, ir_idx);
  return cgstub_knobs.branch_size_16 ? 2 : 4;
}

int tcc_gen_machine_conditional_jump_mop(int32_t condition, TccIrOp op, int32_t target_ir, int ir_idx)
{
  (void)condition;
  cgstub_push("conditional_jump_mop", op, MACH_OP_NONE, -1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, target_ir, ir_idx);
  return cgstub_knobs.branch_size_16 ? 2 : 4;
}

int tcc_gen_machine_cbz_jump_mop(int rn, int nonzero, int32_t target_ir, int ir_idx)
{
  (void)rn;
  (void)nonzero;
  cgstub_push("cbz_jump_mop", (TccIrOp)-1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, target_ir, ir_idx);
  return cgstub_knobs.branch_size_16 ? 2 : 4;
}

int tcc_gen_machine_pending_pool_size(void)
{
  cgstub_record("pending_pool_size", (TccIrOp)-1, CGSTUB_NO_OP, CGSTUB_NO_OP, CGSTUB_NO_OP);
  return 0;
}

int tcc_gen_machine_switch_table_dry_run_size(int num_entries)
{
  cgstub_push("switch_table_dry_run_size", (TccIrOp)-1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, MACH_OP_NONE, -1,
             num_entries, 0);
  return num_entries * cgstub_knobs.switch_table_entry_size;
}

void tcc_gen_machine_switch_table_mop(MachineOperand src, struct TCCIRSwitchTable *table, struct TCCIRState *ir,
                                      int ir_idx)
{
  (void)table;
  (void)ir;
  cgstub_record_ex("switch_table_mop", (TccIrOp)-1, CGSTUB_NO_OP, src, CGSTUB_NO_OP, ir_idx, 0);
}

int tcc_gen_machine_switch_load_dry_run_size(int num_entries)
{
  cgstub_push("switch_load_dry_run_size", (TccIrOp)-1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, MACH_OP_NONE, -1,
             num_entries, 0);
  return num_entries * cgstub_knobs.switch_load_entry_size;
}

void tcc_gen_machine_switch_load_mop(MachineOperand src, MachineOperand dest, struct TCCIRSwitchValueTable *vtab,
                                     struct TCCIRState *ir, int ir_idx)
{
  (void)vtab;
  (void)ir;
  cgstub_record_ex("switch_load_mop", (TccIrOp)-1, dest, src, CGSTUB_NO_OP, ir_idx, 0);
}

void tcc_gen_machine_backpatch_jump(int address, int offset)
{
  cgstub_push("backpatch_jump", (TccIrOp)-1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, address, offset);
}

void tcc_gen_machine_end_instruction(void)
{
  cgstub_record("end_instruction", (TccIrOp)-1, CGSTUB_NO_OP, CGSTUB_NO_OP, CGSTUB_NO_OP);
}

void tcc_gen_machine_select_mop(MachineOperand then_val, MachineOperand else_val, MachineOperand dest,
                                int cond_code)
{
  cgstub_record_ex("select_mop", (TccIrOp)-1, dest, then_val, else_val, cond_code, 0);
}

/* ============================================================================
 * Calls / parameters / return
 * ============================================================================ */

void tcc_gen_machine_func_parameter_mop(MachineOperand src1, MachineOperand src2_enc, TccIrOp op)
{
  cgstub_record("func_parameter_mop", op, CGSTUB_NO_OP, src1, src2_enc);
}

void tcc_gen_machine_func_call_mop(MachineOperand func_mop, IROperand call_id, MachineOperand dest, int drop_value,
                                   TCCIRState *ir, int call_idx)
{
  (void)call_id;
  (void)ir;
  cgstub_record_ex("func_call_mop", (TccIrOp)-1, dest, func_mop, CGSTUB_NO_OP, drop_value, call_idx);
}

void tcc_gen_machine_return_value_mop(MachineOperand src, TccIrOp op)
{
  cgstub_record("return_value_mop", op, CGSTUB_NO_OP, src, CGSTUB_NO_OP);
}

/* AAPCS-shaped but minimal: first 4 words in R0-R3, the rest on the outgoing
 * stack area. Good enough for the pre-scan's stack-size estimate and for
 * Phase 4's call-family dispatch tests; not a full ABI classifier. */
int thumb_build_call_layout_from_ir(TCCIRState *ir, int call_idx, int call_id, int argc_hint,
                                    TCCAbiCallLayout *layout, IROperand **out_args, MachineOperand **out_mops)
{
  (void)ir;
  (void)call_idx;
  (void)call_id;
  (void)out_args;
  (void)out_mops;
  int next_reg = 0;
  int32_t next_stack = 0;
  int argc = argc_hint;
  for (int a = 0; a < argc && a < layout->capacity; a++)
  {
    TCCAbiArgLoc *l = &layout->locs[a];
    memset(l, 0, sizeof(*l));
    if (next_reg < 4)
    {
      l->kind = TCC_ABI_LOC_REG;
      l->reg_base = (uint8_t)next_reg;
      l->reg_count = 1;
      l->size = 4;
      next_reg++;
    }
    else
    {
      l->kind = TCC_ABI_LOC_STACK;
      l->stack_off = next_stack;
      l->size = 4;
      next_stack += 4;
    }
  }
  layout->stack_size = next_stack;
  return argc;
}

/* ============================================================================
 * Prologue / epilogue
 * ============================================================================ */

void tcc_gen_machine_prolog(int leaffunc, uint64_t used_registers, int stack_size, uint32_t extra_prologue_regs)
{
  cgstub_last_prolog.called = 1;
  cgstub_last_prolog.leaffunc = leaffunc;
  cgstub_last_prolog.used_registers = used_registers;
  cgstub_last_prolog.stack_size = stack_size;
  cgstub_last_prolog.extra_prologue_regs = extra_prologue_regs;
  cgstub_push("prolog", (TccIrOp)-1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, leaffunc, stack_size);
}

void tcc_gen_machine_epilog(int leaffunc)
{
  cgstub_push("epilog", (TccIrOp)-1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, leaffunc, 0);
}

void tcc_gen_machine_finish_noreturn(void)
{
  cgstub_record("finish_noreturn", (TccIrOp)-1, CGSTUB_NO_OP, CGSTUB_NO_OP, CGSTUB_NO_OP);
}

/* ============================================================================
 * Two-pass (dry-run/real-run) bookkeeping
 * ============================================================================ */

void tcc_gen_machine_dry_run_init(void)
{
  cgstub_record("dry_run_init", (TccIrOp)-1, CGSTUB_NO_OP, CGSTUB_NO_OP, CGSTUB_NO_OP);
}

void tcc_gen_machine_dry_run_start(void)
{
  cgstub_current_pass = 0;
  cgstub_record("dry_run_start", (TccIrOp)-1, CGSTUB_NO_OP, CGSTUB_NO_OP, CGSTUB_NO_OP);
}

void tcc_gen_machine_dry_run_end(void)
{
  cgstub_record("dry_run_end", (TccIrOp)-1, CGSTUB_NO_OP, CGSTUB_NO_OP, CGSTUB_NO_OP);
  cgstub_current_pass = 1;
}

int tcc_gen_machine_dry_run_get_lr_push_count(void)
{
  cgstub_record("dry_run_get_lr_push_count", (TccIrOp)-1, CGSTUB_NO_OP, CGSTUB_NO_OP, CGSTUB_NO_OP);
  return cgstub_knobs.lr_push_count;
}

uint32_t tcc_gen_machine_dry_run_get_scratch_regs_pushed(void)
{
  cgstub_record("dry_run_get_scratch_regs_pushed", (TccIrOp)-1, CGSTUB_NO_OP, CGSTUB_NO_OP, CGSTUB_NO_OP);
  return cgstub_knobs.scratch_regs_pushed;
}

void tcc_gen_machine_reset_scratch_state(void)
{
  cgstub_record("reset_scratch_state", (TccIrOp)-1, CGSTUB_NO_OP, CGSTUB_NO_OP, CGSTUB_NO_OP);
}

void tcc_gen_machine_insn_scratch_reset(void)
{
  cgstub_record("insn_scratch_reset", (TccIrOp)-1, CGSTUB_NO_OP, CGSTUB_NO_OP, CGSTUB_NO_OP);
}

int tcc_gen_machine_insn_scratch_count(void)
{
  int v = cgstub_knobs.insn_scratch_count;
  cgstub_record("insn_scratch_count", (TccIrOp)-1, CGSTUB_NO_OP, CGSTUB_NO_OP, CGSTUB_NO_OP);
  cgstub_knobs.insn_scratch_count = 0; /* one-shot */
  return v;
}

uint16_t tcc_gen_machine_insn_scratch_saves_mask(void)
{
  uint16_t v = cgstub_knobs.insn_scratch_saves_mask;
  cgstub_record("insn_scratch_saves_mask", (TccIrOp)-1, CGSTUB_NO_OP, CGSTUB_NO_OP, CGSTUB_NO_OP);
  cgstub_knobs.insn_scratch_saves_mask = 0; /* one-shot */
  return v;
}

void tcc_gen_machine_branch_opt_init(void)
{
  cgstub_record("branch_opt_init", (TccIrOp)-1, CGSTUB_NO_OP, CGSTUB_NO_OP, CGSTUB_NO_OP);
}

void tcc_gen_machine_branch_opt_analyze(uint32_t *ir_to_code_mapping, int mapping_size)
{
  (void)ir_to_code_mapping;
  cgstub_push("branch_opt_analyze", (TccIrOp)-1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, mapping_size,
             0);
}

void tcc_gen_machine_mov_equiv_reset(void)
{
  cgstub_record("mov_equiv_reset", (TccIrOp)-1, CGSTUB_NO_OP, CGSTUB_NO_OP, CGSTUB_NO_OP);
}

void tcc_gen_machine_reserve_pool_bytes(int upcoming_bytes)
{
  cgstub_push("reserve_pool_bytes", (TccIrOp)-1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, MACH_OP_NONE, -1,
             upcoming_bytes, 0);
}

void tcc_gen_machine_strldr_cache_reset(void)
{
  cgstub_record("strldr_cache_reset", (TccIrOp)-1, CGSTUB_NO_OP, CGSTUB_NO_OP, CGSTUB_NO_OP);
}

void tcc_gen_machine_imm_cache_reset(void)
{
  cgstub_record("imm_cache_reset", (TccIrOp)-1, CGSTUB_NO_OP, CGSTUB_NO_OP, CGSTUB_NO_OP);
}

void tcc_gen_machine_imm_cache_invalidate_live(uint32_t live_mask)
{
  cgstub_push("imm_cache_invalidate_live", (TccIrOp)-1, MACH_OP_NONE, -1, MACH_OP_NONE, -1, MACH_OP_NONE, -1,
             (int)live_mask, 0);
}

/* ============================================================================
 * Static chain (nested functions)
 * ============================================================================ */

void tcc_gen_machine_set_chain(void)
{
  cgstub_record("set_chain", (TccIrOp)-1, CGSTUB_NO_OP, CGSTUB_NO_OP, CGSTUB_NO_OP);
}

void tcc_gen_machine_restore_chain(void)
{
  cgstub_record("restore_chain", (TccIrOp)-1, CGSTUB_NO_OP, CGSTUB_NO_OP, CGSTUB_NO_OP);
}

void tcc_gen_machine_init_chain_slot(IROperand src1)
{
  cgstub_push("init_chain_slot", (TccIrOp)-1, MACH_OP_NONE, -1, MACH_OP_NONE, irop_get_vreg(src1), MACH_OP_NONE, -1,
             0, 0);
}

/* ============================================================================
 * Misc: trap / prefetch / setjmp-longjmp / __builtin_apply
 * ============================================================================ */

void tcc_gen_machine_trap_mop(void)
{
  cgstub_record("trap_mop", (TccIrOp)-1, CGSTUB_NO_OP, CGSTUB_NO_OP, CGSTUB_NO_OP);
}

void tcc_gen_machine_prefetch_mop(MachineOperand addr, int rw)
{
  cgstub_record_ex("prefetch_mop", (TccIrOp)-1, CGSTUB_NO_OP, addr, CGSTUB_NO_OP, rw, 0);
}

void tcc_gen_machine_setjmp_mop(MachineOperand buf, MachineOperand area, MachineOperand dest)
{
  cgstub_record("setjmp_mop", (TccIrOp)-1, dest, buf, area);
}

void tcc_gen_machine_longjmp_mop(MachineOperand buf)
{
  cgstub_record("longjmp_mop", (TccIrOp)-1, CGSTUB_NO_OP, buf, CGSTUB_NO_OP);
}

void tcc_gen_machine_nl_setjmp_mop(MachineOperand buf, MachineOperand dest)
{
  cgstub_record("nl_setjmp_mop", (TccIrOp)-1, dest, buf, CGSTUB_NO_OP);
}

void tcc_gen_machine_nl_longjmp_mop(MachineOperand buf)
{
  cgstub_record("nl_longjmp_mop", (TccIrOp)-1, CGSTUB_NO_OP, buf, CGSTUB_NO_OP);
}

void tcc_gen_machine_builtin_apply_args_mop(MachineOperand dest)
{
  cgstub_record("builtin_apply_args_mop", (TccIrOp)-1, dest, CGSTUB_NO_OP, CGSTUB_NO_OP);
}

void tcc_gen_machine_builtin_apply_mop(MachineOperand fn, MachineOperand args, MachineOperand dest)
{
  cgstub_record("builtin_apply_mop", (TccIrOp)-1, dest, fn, args);
}

/* ============================================================================
 * Non-mop link dependencies (found by trial-linking tcc_ir_codegen_generate)
 * ============================================================================ */

/* `loc` is declared ST_DATA int rsym, anon_sym, ind, loc; in tcc.h. `ind` is
 * already defined in stubs.c; `loc` is not, and codegen.c's frame-size
 * accounting references it. */
int loc;

/* Debug tracking variable; extern'd by ir/codegen.c itself (normally defined
 * in arm-thumb-gen.c). */
int g_debug_current_op = -1;

/* `vtop`/`_vstack`: the frontend's value stack, normally defined in
 * tccgen.c (not linked here). Needed only by tcc_ir_codegen_cmp_jmp_set()/
 * tcc_ir_codegen_test_gen() (ir/codegen.c), which read/write the current
 * top-of-stack SValue's fields directly -- no gv()/vpush()/vswap() frontend
 * machinery is exercised, so this minimal fake stack (one push depth is all
 * either function ever needs) is sufficient. See codegen_mop_stubs.h for the
 * cgstub_vtop_push()/cgstub_vtop_get() test-facing API. */
SValue _vstack[VSTACK_SIZE];
SValue *vtop = _vstack; /* empty: vtop == _vstack, matching codegen.c's
                          * `vtop < _vstack + 1` empty-stack guard */

SValue *cgstub_vtop_push(void)
{
  vtop++;
  svalue_init(vtop);
  return vtop;
}

SValue *cgstub_vtop_get(void)
{
  return (vtop < _vstack + 1) ? NULL : vtop;
}

void tcc_debug_line_num(TCCState *s1, int line_num)
{
  (void)s1;
  (void)line_num;
}

void tcc_debug_prolog_epilog(TCCState *s1, int value)
{
  (void)s1;
  (void)value;
}

/* TCCIR_OP_INLINE_ASM lowering is a documented gap (see
 * docs/plan_codegen_unit_tests.md) -- it needs real frontend value-stack
 * (vtop/gv) semantics this harness doesn't provide. No-op; still recorded so
 * an accidental hit is visible in the call log rather than silently ignored. */
void tcc_asm_emit_inline(ASMOperand *operands, int nb_operands, int nb_outputs, int nb_labels,
                         uint8_t *clobber_regs, const uint8_t *reserved_regs, const char *asm_str, int asm_len,
                         int must_subst)
{
  (void)operands;
  (void)nb_operands;
  (void)nb_outputs;
  (void)nb_labels;
  (void)clobber_regs;
  (void)reserved_regs;
  (void)asm_str;
  (void)asm_len;
  (void)must_subst;
  cgstub_record("asm_emit_inline", (TccIrOp)-1, CGSTUB_NO_OP, CGSTUB_NO_OP, CGSTUB_NO_OP);
}

/* Used by tcc_ir_codegen_backpatch_jumps() to patch switch-table entries
 * directly into the (fake) text section buffer -- real little-endian
 * semantics needed, not just a call recorder. */
void write32le(unsigned char *p, uint32_t x)
{
  p[0] = (unsigned char)(x & 0xff);
  p[1] = (unsigned char)((x >> 8) & 0xff);
  p[2] = (unsigned char)((x >> 16) & 0xff);
  p[3] = (unsigned char)((x >> 24) & 0xff);
}

/* Real impl lives in arm-thumb-gen.c; ir->spill_cache is a plain embedded
 * struct (not a pointer), so this is cheap to implement for real rather than
 * fake -- same semantics as the production function. */
void tcc_ir_spill_cache_clear(SpillCache *cache)
{
  for (int i = 0; i < SPILL_CACHE_SIZE; i++)
    cache->entries[i].valid = 0;
  cache->last_emit_kind = 0;
  cache->last_emit_ind = 0;
  cache->last_emit_reg = 0;
  cache->last_emit_offset = 0;
}

void tcc_ir_spill_cache_invalidate_reg(SpillCache *cache, int reg)
{
  for (int i = 0; i < SPILL_CACHE_SIZE; i++)
  {
    if (cache->entries[i].valid && cache->entries[i].reg == reg)
      cache->entries[i].valid = 0;
  }
}

void tcc_ir_spill_cache_invalidate_offset(SpillCache *cache, int offset)
{
  for (int i = 0; i < SPILL_CACHE_SIZE; i++)
  {
    if (cache->entries[i].valid && cache->entries[i].offset == offset)
      cache->entries[i].valid = 0;
  }
}

void tcc_ir_spill_cache_record(SpillCache *cache, int reg, int offset)
{
  tcc_ir_spill_cache_invalidate_reg(cache, reg);
  tcc_ir_spill_cache_invalidate_offset(cache, offset);

  for (int i = 0; i < SPILL_CACHE_SIZE; i++)
  {
    if (!cache->entries[i].valid)
    {
      cache->entries[i].valid = 1;
      cache->entries[i].reg = reg;
      cache->entries[i].offset = offset;
      return;
    }
  }
  cache->entries[0].valid = 1;
  cache->entries[0].reg = reg;
  cache->entries[0].offset = offset;
}

int tcc_ir_spill_cache_lookup(SpillCache *cache, int offset)
{
  for (int i = 0; i < SPILL_CACHE_SIZE; i++)
  {
    if (cache->entries[i].valid && cache->entries[i].offset == offset)
      return cache->entries[i].reg;
  }
  return -1;
}

/* From tccopt.c -- FP materialization cache setup, mirrors the already-
 * stubbed tcc_opt_fp_mat_cache_free() in stubs.c (teardown counterpart). */
void tcc_opt_fp_mat_cache_clear(TCCIRState *ir)
{
  (void)ir;
}

/* From tccgen.c -- ir/core.c:tcc_ir_local_add() calls sym_push() to build a
 * local-stack symbol.  This binary links ir/core.c but not tccgen.c, so a NULL
 * returning stub satisfies the linker for hand-built IR tests (no real
 * frontend symbol table is present). */
Sym *sym_push(int v, CType *type, int r, int c)
{
  (void)v; (void)type; (void)r; (void)c;
  return NULL;
}

/* ELF section/relocation emitters reached only from the real arm-thumb-asm.c
 * (g()/gen_le32()/gen_expr32(), pulled in by test_tccasm.c's tccasm.c include)
 * and tccdbg.c's DWARF writer (test_tccdbg.c).  tccelf.c is not linked into
 * this binary, so provide them here.  section_realloc is a verbatim copy of
 * tccelf.c's bump reallocator (so it works if code is actually emitted);
 * greloca is a no-op — no test in this binary inspects emitted relocations.
 *
 * NB: this is a main-binary-only stub file.  The backend binary already gets
 * these from codegen_backend_stubs.c and the switch-data path from
 * elfsec_stubs.c, so keeping them out of those shared files avoids a
 * multiple-definition clash. */
void section_realloc(Section *sec, unsigned long new_size)
{
  unsigned long size = sec->data_allocated;
  unsigned char *data;
  if (size == 0)
    size = 256;
  while (size < new_size)
    size *= 2;
  data = (unsigned char *)tcc_realloc(sec->data, size);
  memset(data + sec->data_allocated, 0, size - sec->data_allocated);
  sec->data = data;
  sec->data_allocated = size;
}

void greloca(Section *s, Sym *sym, unsigned long offset, int type, addr_t addend)
{
  (void)s;
  (void)sym;
  (void)offset;
  (void)type;
  (void)addend;
}
