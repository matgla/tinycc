/*
 *  codegen_mop_stubs.h - query API for the ir/codegen.c backend-mop stub layer
 *
 *  ir/codegen.c's tcc_ir_codegen_generate() dispatches every IR instruction to
 *  one of ~76 tcc_gen_machine_*_mop() backend functions normally implemented
 *  in arm-thumb-gen.c (not linked into this unit-test binary). This header
 *  exposes the record/query API for the stub implementations of those
 *  functions in codegen_mop_stubs.c, so tests can assert dispatch-level facts
 *  (which mop got called, how many times, with what operand kinds, in what
 *  order) without linking the real backend.
 */

#ifndef CODEGEN_MOP_STUBS_H
#define CODEGEN_MOP_STUBS_H

typedef struct CgStubCall
{
  const char *mop_name;    /* stub function name, e.g. "data_processing_mop" */
  TccIrOp ir_op;            /* cq->op if known at the call site, else -1 */
  MachineOperandKind dest_kind, src1_kind, src2_kind; /* MACH_OP_NONE if n/a */
  int dest_vreg, src1_vreg, src2_vreg;                /* MachineOperand.vreg; -1 if n/a */
  int aux0, aux1;           /* spare ints for mops whose interesting args aren't MachineOperands
                              * (e.g. jump_mop's target_ir/ir_idx, try_strd_spill's regs) */
  int pass;                 /* 0 = dry-run, 1 = real-run */
} CgStubCall;

/* Reset the call log and all knobs to their documented defaults. Call at the
 * start of every UT_TEST that calls tcc_ir_codegen_generate(). */
void cgstub_reset(void);

int cgstub_total_calls(void);
int cgstub_call_count(const char *mop_name);
int cgstub_call_count_pass(const char *mop_name, int pass);
/* n-th call (0-based) to this specific mop; NULL if n is out of range. */
const CgStubCall *cgstub_nth_call(const char *mop_name, int n);
/* n-th call (0-based) across all mops, for call-order assertions; NULL if out of range. */
const CgStubCall *cgstub_nth_call_any(int n);

/* --- knobs: settable return values for the stubs whose result feeds
 * codegen.c's own control flow (code-offset tracking, frame layout). --- */

/* jump_mop / conditional_jump_mop / cbz_jump_mop return 2 bytes (16-bit Thumb)
 * when enabled, else 4 bytes (32-bit). Default: disabled (0), i.e. 4 bytes. */
void cgstub_set_branch_size_16(int enable);

/* switch_table_dry_run_size / switch_load_dry_run_size return num_entries *
 * this many bytes. Defaults: 4 / 4. */
void cgstub_set_switch_entry_sizes(int table_entry_size, int load_entry_size);

/* dry_run_get_lr_push_count return value. Default: 0. */
void cgstub_set_lr_push_count(int n);

/* dry_run_get_scratch_regs_pushed return value. Default: 0. */
void cgstub_set_scratch_regs_pushed(unsigned int mask);

/* One-shot: consumed by the very next insn_scratch_count()/
 * insn_scratch_saves_mask() pair (i.e. the next dispatched instruction),
 * then auto-resets to 0/0. codegen.c calls insn_scratch_reset() before every
 * dispatched instruction, which is what triggers the reset. */
void cgstub_set_next_insn_scratch(int count, unsigned short saves_mask);

/* --- full snapshot of the most recent tcc_gen_machine_prolog() call ---
 * Phase 7 (prolog/epilog + two-pass bookkeeping) needs every argument, not
 * just the two that fit in CgStubCall's generic aux0/aux1. */
typedef struct CgStubLastProlog
{
  int called;
  int leaffunc;
  unsigned long long used_registers;
  int stack_size;
  unsigned int extra_prologue_regs;
} CgStubLastProlog;

const CgStubLastProlog *cgstub_get_last_prolog(void);

/* --- fake frontend value-stack (vtop/_vstack), for
 * tcc_ir_codegen_cmp_jmp_set()/tcc_ir_codegen_test_gen() tests ---
 *
 * ir/codegen.c references the real `vtop`/`_vstack` globals normally defined
 * in tccgen.c (not linked here -- see docs/plan_codegen_unit_tests.md's
 * TCCIR_OP_INLINE_ASM gap for why). Both functions above only ever read/
 * write the CURRENT top-of-stack SValue's fields directly (no gv()/vpush()/
 * vswap() frontend machinery), so a minimal fake stack is enough: these
 * helpers manage `vtop`/`_vstack` (defined in codegen_mop_stubs.c) without
 * needing any other frontend state.
 *
 * cgstub_reset() resets the fake stack to empty (vtop == _vstack, matching
 * ir/codegen.c's `vtop < _vstack + 1` empty-stack guard). */

/* Push one entry and return a pointer to it (== the new vtop) for the test
 * to populate fields on directly (r, jtrue, jfalse, cmp_op, c.i, ...). The
 * returned SValue is zero-initialized (via svalue_init) before return. */
SValue *cgstub_vtop_push(void);

/* Current vtop (NULL-safe: returns NULL, not a dangling/underflowed pointer,
 * when the fake stack is empty). */
SValue *cgstub_vtop_get(void);

#endif /* CODEGEN_MOP_STUBS_H */
