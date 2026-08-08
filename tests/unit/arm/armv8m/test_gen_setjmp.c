/*
 *  test_gen_setjmp.c - suite for the setjmp/longjmp/builtin_apply mop family
 *  in arm-thumb-gen.c.
 *
 *  Mirrors test_gen_dispatch_smoke.c: calls tcc_gen_machine_*_mop() DIRECTLY
 *  (bypassing ir/codegen.c's dispatch loop) with hand-built MachineOperand
 *  arguments, and asserts on the real Thumb-2 bytes emitted into a real
 *  Section via the real o()/section_add machinery.  No IR, no dispatch loop,
 *  no frontend.
 *
 *  Operand shapes are taken from how tccgen.c actually constructs the
 *  SValues for these ops (verified by reading TOK_builtin_setjmp,
 *  TOK_builtin_longjmp, the non-local-goto NL_SETJMP/NL_LONGJMP call sites,
 *  and TOK_builtin_apply_args/TOK_builtin_apply around tccgen.c:22232-22279
 *  and tccgen.c:17686-17750):
 *
 *    - setjmp's "buf" (the user's void** argument) and longjmp's "buf" are
 *      ordinary pointer-valued expressions -> MACH_OP_REG holding the
 *      pointer value.
 *    - setjmp's "area" (hidden r4-r11 save area) and nl_setjmp/nl_longjmp's
 *      "buf" (the compiler-allocated jmp_buf local) are built as
 *      `r = VT_LOCAL, vr = -1, no VT_LVAL` i.e. address-of-local ->
 *      MACH_OP_FRAME_ADDR (see ir/machine_op.c machine_op_from_ir()).
 *    - dest operands (setjmp/nl_setjmp return value, builtin_apply_args
 *      pointer result, builtin_apply call result) are plain int/ptr temps
 *      -> MACH_OP_REG once register-allocated.
 *    - builtin_apply's fn/args are ordinary pointer-valued expressions ->
 *      MACH_OP_REG.
 *
 *  Every byte-level expected value below was captured empirically: a
 *  temporary fprintf(stderr, ...) dump of every emitted halfword was added
 *  to each test, the standalone trial binary was run, and the printed bytes
 *  were hand-decoded against the Thumb-2 encoding tables (T1 16-bit
 *  LDR/STR-imm require Rn AND Rt both in r0-r7 -- e.g. through R_IP/R12 or
 *  into R8-R11 they widen to the 32-bit T3 form 0xf8dc/0xf8cc; hi-reg MOV
 *  is the 0x46xx T1 form; BX/BLX Rm is 0x4700|(Rm<<3) / 0x4780|(Rm<<3)) and
 *  cross-checked against the individual thop_* encoder tests (e.g.
 *  test_thop_mem_imm.c for LDR/STR-imm T1 vs T3 forms, test_thop_branch.c
 *  for BX/BLX-reg) before being encoded as the oracle assertions below --
 *  not hand-derived from ISA tables alone. The fprintf dump was removed
 *  afterward; only the confirmed values remain.
 *
 *  `allocated_stack_size` (arm-thumb-gen.c) feeds fp_adjust_local_offset(),
 *  which the FRAME_ADDR-operand tests below depend on being 0 (it is only
 *  ever mutated by the real prologue codegen, which this suite never
 *  calls) -- reset explicitly in setup_gen() so results don't depend on
 *  what some other suite in the shared binary did first.
 */

#define USING_GLOBALS
#include "ir.h"
#include "source/backend/arch/arm/arm.h"
#include "source/backend/arch/arm/thumb/thumb.h"
#include "source/ir/machine_op.h"
#include "codegen_backend_stubs.h"
#include "elfsec_stubs.h"

#include "ut.h"

/* Not exposed via any header (ST_DATA global in arm-thumb-gen.c); declared
 * here the same way tccdbg.c does, purely to reset it for determinism. */
extern int allocated_stack_size;

/* ------------------------------------------------------------------ helpers */

static void setup_gen(void)
{
  elfsec_reset();
  cgb_reset();
  arm_target_init("armv8-m.main", NULL, "cortex-m33", 0);
  cur_text_section = elfsec_new_section(".text");
  ind = 0;
  tcc_state->registers_for_allocator = 13;
  tcc_state->float_abi = ARM_HARD_FLOAT;
  tcc_state->need_frame_pointer = 0; /* not reset between tests; be explicit */
  allocated_stack_size = 0;          /* likewise: only prologue codegen sets it */
}

static MachineOperand mop_reg(int r, int btype)
{
  MachineOperand m;
  memset(&m, 0, sizeof(m));
  m.kind = MACH_OP_REG;
  m.btype = btype;
  m.u.reg.r0 = r;
  m.u.reg.r1 = -1;
  return m;
}

static MachineOperand mop_frame_addr(int32_t offset, int btype)
{
  MachineOperand m;
  memset(&m, 0, sizeof(m));
  m.kind = MACH_OP_FRAME_ADDR;
  m.btype = btype;
  m.u.frame.offset = offset;
  return m;
}

static MachineOperand mop_none(void)
{
  MachineOperand m;
  memset(&m, 0, sizeof(m));
  m.kind = MACH_OP_NONE;
  return m;
}

static uint16_t read_le16(const unsigned char *p)
{
  return (uint16_t)(p[0] | (p[1] << 8));
}

/* ------------------------------------------------------------------ longjmp */

/* __builtin_longjmp(buf): buf already in R1 (MACH_OP_REG, no deref).
 * Real body (arm-thumb-gen.c tcc_gen_machine_longjmp_mop):
 *   MOV   IP, R1               (copy buf ptr to IP so it survives restores)
 *   LDR   R0, [IP, #4]         resume addr
 *   LDR   R1, [IP, #8]         saved SP
 *   LDR   R2, [IP, #12]        &save_area
 *   LDR   R4-R7,  [R2, #0..12] restore callee-saved (T1 16-bit: R2 and Rt low)
 *   LDR   R8-R11, [R2, #16..28] restore callee-saved (T3 32-bit: Rt is hi reg)
 *   MOV   SP, R1
 *   BX    R0
 */
UT_TEST(test_longjmp_reg_buf_emits_expected_sequence)
{
  setup_gen();

  tcc_gen_machine_longjmp_mop(mop_reg(R1, IROP_BTYPE_INT32));

  UT_ASSERT_EQ(ind, 42);

  const unsigned char *p = cur_text_section->data;

  UT_ASSERT_EQ(read_le16(p + 0), 0x468c); /* MOV IP, R1 */
  UT_ASSERT_EQ(read_le16(p + 2), 0xf8dc); /* LDR R0, [IP, #4]  (T3, hw1) */
  UT_ASSERT_EQ(read_le16(p + 4), 0x0004); /*                   (T3, hw2) */
  UT_ASSERT_EQ(read_le16(p + 6), 0xf8dc); /* LDR R1, [IP, #8]  (T3, hw1) */
  UT_ASSERT_EQ(read_le16(p + 8), 0x1008); /*                   (T3, hw2) */
  UT_ASSERT_EQ(read_le16(p + 10), 0xf8dc); /* LDR R2, [IP, #12] (T3, hw1) */
  UT_ASSERT_EQ(read_le16(p + 12), 0x200c); /*                   (T3, hw2) */
  UT_ASSERT_EQ(read_le16(p + 14), 0x6814); /* LDR R4, [R2, #0]  (T1) */
  UT_ASSERT_EQ(read_le16(p + 16), 0x6855); /* LDR R5, [R2, #4]  (T1) */
  UT_ASSERT_EQ(read_le16(p + 18), 0x6896); /* LDR R6, [R2, #8]  (T1) */
  UT_ASSERT_EQ(read_le16(p + 20), 0x68d7); /* LDR R7, [R2, #12] (T1) */
  UT_ASSERT_EQ(read_le16(p + 22), 0xf8d2); /* LDR R8, [R2, #16] (T3, hw1) */
  UT_ASSERT_EQ(read_le16(p + 24), 0x8010); /*                   (T3, hw2) */
  UT_ASSERT_EQ(read_le16(p + 26), 0xf8d2); /* LDR R9, [R2, #20] (T3, hw1) */
  UT_ASSERT_EQ(read_le16(p + 28), 0x9014); /*                   (T3, hw2) */
  UT_ASSERT_EQ(read_le16(p + 30), 0xf8d2); /* LDR R10, [R2, #24] (T3, hw1) */
  UT_ASSERT_EQ(read_le16(p + 32), 0xa018); /*                    (T3, hw2) */
  UT_ASSERT_EQ(read_le16(p + 34), 0xf8d2); /* LDR R11, [R2, #28] (T3, hw1) */
  UT_ASSERT_EQ(read_le16(p + 36), 0xb01c); /*                    (T3, hw2) */
  UT_ASSERT_EQ(read_le16(p + 38), 0x468d); /* MOV SP, R1 */
  UT_ASSERT_EQ(read_le16(p + 40), 0x4700); /* BX R0 */

  return 0;
}

/* ------------------------------------------------------------------ nl_longjmp */

/* nl_longjmp with a plain-register buf (not CHAIN_REL): structurally
 * identical shape to longjmp but with different fixed offsets (buf[0..7] =
 * r4-r11, buf[8]=SP, buf[9]=resume) and buf already materialized in R2.
 * All LDRs are now through IP (a hi reg base), so ALL of them -- even the
 * r4-r7 restores -- widen to the 32-bit T3 form (unlike longjmp's R2-based
 * restore loop, which stays 16-bit for r4-r7). */
UT_TEST(test_nl_longjmp_reg_buf_emits_expected_sequence)
{
  setup_gen();

  tcc_gen_machine_nl_longjmp_mop(mop_reg(R2, IROP_BTYPE_INT32));

  UT_ASSERT_EQ(ind, 46);

  const unsigned char *p = cur_text_section->data;

  UT_ASSERT_EQ(read_le16(p + 0), 0x4694); /* MOV IP, R2 */
  UT_ASSERT_EQ(read_le16(p + 2), 0xf8dc); /* LDR R0, [IP, #36] (resume) */
  UT_ASSERT_EQ(read_le16(p + 4), 0x0024);
  UT_ASSERT_EQ(read_le16(p + 6), 0xf8dc); /* LDR R1, [IP, #32] (saved SP) */
  UT_ASSERT_EQ(read_le16(p + 8), 0x1020);
  UT_ASSERT_EQ(read_le16(p + 10), 0xf8dc); /* LDR R4, [IP, #0] */
  UT_ASSERT_EQ(read_le16(p + 12), 0x4000);
  UT_ASSERT_EQ(read_le16(p + 14), 0xf8dc); /* LDR R5, [IP, #4] */
  UT_ASSERT_EQ(read_le16(p + 16), 0x5004);
  UT_ASSERT_EQ(read_le16(p + 18), 0xf8dc); /* LDR R6, [IP, #8] */
  UT_ASSERT_EQ(read_le16(p + 20), 0x6008);
  UT_ASSERT_EQ(read_le16(p + 22), 0xf8dc); /* LDR R7, [IP, #12] */
  UT_ASSERT_EQ(read_le16(p + 24), 0x700c);
  UT_ASSERT_EQ(read_le16(p + 26), 0xf8dc); /* LDR R8, [IP, #16] */
  UT_ASSERT_EQ(read_le16(p + 28), 0x8010);
  UT_ASSERT_EQ(read_le16(p + 30), 0xf8dc); /* LDR R9, [IP, #20] */
  UT_ASSERT_EQ(read_le16(p + 32), 0x9014);
  UT_ASSERT_EQ(read_le16(p + 34), 0xf8dc); /* LDR R10, [IP, #24] */
  UT_ASSERT_EQ(read_le16(p + 36), 0xa018);
  UT_ASSERT_EQ(read_le16(p + 38), 0xf8dc); /* LDR R11, [IP, #28] */
  UT_ASSERT_EQ(read_le16(p + 40), 0xb01c);
  UT_ASSERT_EQ(read_le16(p + 42), 0x468d); /* MOV SP, R1 */
  UT_ASSERT_EQ(read_le16(p + 44), 0x4700); /* BX R0 */

  return 0;
}

/* nl_longjmp with a MACH_OP_FRAME_ADDR buf (direct, same-frame jmp_buf --
 * the non-CHAIN_REL branch of the "else" arm). mach_ensure_in_reg's
 * MACH_OP_FRAME_ADDR case allocates its own scratch (independent of the
 * ctx passed to the mop), which get_scratch_reg_with_save chooses to save
 * with a PUSH/POP pair around the whole sequence -- so unlike the two
 * tests above, the very last emitted halfword is that POP, one slot after
 * the BX R0. We pin the exact structural landmarks (buf address materialized
 * into R0, tail is "...; BX R0; POP {r0}") rather than the full byte trace,
 * since the SUB immediate's exact value depends on fp_adjust_local_offset's
 * scratch-push bias, which is a documented but incidental side effect of
 * which scratch register the allocator happens to pick here. */
UT_TEST(test_nl_longjmp_frame_addr_buf_ends_in_push_pop_wrapped_bx_r0)
{
  setup_gen();

  tcc_gen_machine_nl_longjmp_mop(mop_frame_addr(-40, IROP_BTYPE_INT32));

  UT_ASSERT_EQ(ind, 54);

  const unsigned char *p = cur_text_section->data;

  UT_ASSERT_EQ(read_le16(p + 0), 0xb401); /* PUSH {r0} (save scratch for FRAME_ADDR) */
  /* [2,4]: SUB.W R0, SP, #imm -- address-of-stack-slot into the scratch (R0) */
  UT_ASSERT_EQ(read_le16(p + 6), 0x4684);  /* MOV IP, R0 (buf ptr survives restores) */
  UT_ASSERT_EQ(read_le16(p + 8), 0xf8dc);  /* LDR R0, [IP, #36] (resume) */
  UT_ASSERT_EQ(read_le16(p + 10), 0x0024);
  UT_ASSERT_EQ(read_le16(p + ind - 6), 0x468d); /* MOV SP, R1 */
  UT_ASSERT_EQ(read_le16(p + ind - 4), 0x4700); /* BX R0 */
  UT_ASSERT_EQ(read_le16(p + ind - 2), 0xbc01); /* POP {r0} (restore FRAME_ADDR scratch) */

  return 0;
}

/* ------------------------------------------------------------------ setjmp */

/* __builtin_setjmp(buf) with buf already in R1 (MACH_OP_REG) and the hidden
 * r4-r11 save area at FP-32 (MACH_OP_FRAME_ADDR), dest in R3.
 * The "area" is handled by a *direct* call to tcc_machine_addr_of_stack_slot
 * straight into R_IP (not through mach_ensure_in_reg/mach_alloc_scratch), so
 * -- unlike the FRAME_ADDR longjmp case above -- there is no wrapping
 * PUSH/POP and the SUB immediate is the unadjusted offset magnitude (32).
 * Full sequence (all captured empirically):
 *   SUB.W IP, SP, #32            area address -> IP
 *   STR   R4-R7,  [IP, #0..12]   (T1 16-bit)
 *   STR   R8-R11, [IP, #16..28]  (T3 32-bit: Rt is a hi reg)
 *   STR   IP, [R1, #12]          &area -> buf[3]
 *   STR   R7, [R1, #0]           FP -> buf[0]
 *   MOV   IP, SP
 *   STR   IP, [R1, #8]           SP -> buf[2]
 *   ADR   IP, resume (T3 32-bit)
 *   ORR.W IP, IP, #1             Thumb bit
 *   STR   IP, [R1, #4]           resume -> buf[1]
 *   MOV.W R3, #0                 dest = 0
 *   B.W   +4
 *   MOV.W R3, #1                 dest = 1 (resume_label)
 */
UT_TEST(test_setjmp_reg_buf_frame_area_reg_dest_shape)
{
  setup_gen();

  tcc_gen_machine_setjmp_mop(mop_reg(R1, IROP_BTYPE_INT32), mop_frame_addr(-32, IROP_BTYPE_INT32),
                             mop_reg(R3, IROP_BTYPE_INT32));

  UT_ASSERT_EQ(ind, 72);

  const unsigned char *p = cur_text_section->data;

  UT_ASSERT_EQ(read_le16(p + 0), 0xf1ad); /* SUB.W IP, SP, #32 (hw1) */
  UT_ASSERT_EQ(read_le16(p + 2), 0x0c20); /*                   (hw2) */
  UT_ASSERT_EQ(read_le16(p + 4), 0xf8cc); /* STR R4, [IP, #0]  (hw1) */
  UT_ASSERT_EQ(read_le16(p + 6), 0x4000); /*                   (hw2) */
  UT_ASSERT_EQ(read_le16(p + 8), 0xf8cc); /* STR R5, [IP, #4] */
  UT_ASSERT_EQ(read_le16(p + 10), 0x5004);
  UT_ASSERT_EQ(read_le16(p + 12), 0xf8cc); /* STR R6, [IP, #8] */
  UT_ASSERT_EQ(read_le16(p + 14), 0x6008);
  UT_ASSERT_EQ(read_le16(p + 16), 0xf8cc); /* STR R7, [IP, #12] */
  UT_ASSERT_EQ(read_le16(p + 18), 0x700c);
  UT_ASSERT_EQ(read_le16(p + 20), 0xf8cc); /* STR R8, [IP, #16] */
  UT_ASSERT_EQ(read_le16(p + 22), 0x8010);
  UT_ASSERT_EQ(read_le16(p + 24), 0xf8cc); /* STR R9, [IP, #20] */
  UT_ASSERT_EQ(read_le16(p + 26), 0x9014);
  UT_ASSERT_EQ(read_le16(p + 28), 0xf8cc); /* STR R10, [IP, #24] */
  UT_ASSERT_EQ(read_le16(p + 30), 0xa018);
  UT_ASSERT_EQ(read_le16(p + 32), 0xf8cc); /* STR R11, [IP, #28] */
  UT_ASSERT_EQ(read_le16(p + 34), 0xb01c);
  UT_ASSERT_EQ(read_le16(p + 36), 0xf8c1); /* STR IP, [R1, #12] (&area -> buf[3]) */
  UT_ASSERT_EQ(read_le16(p + 38), 0xc00c);
  UT_ASSERT_EQ(read_le16(p + 40), 0x600f); /* STR R7, [R1, #0]  (FP -> buf[0]) */
  UT_ASSERT_EQ(read_le16(p + 42), 0x46ec); /* MOV IP, SP */
  UT_ASSERT_EQ(read_le16(p + 44), 0xf8c1); /* STR IP, [R1, #8]  (SP -> buf[2]) */
  UT_ASSERT_EQ(read_le16(p + 46), 0xc008);
  UT_ASSERT_EQ(read_le16(p + 48), 0xf20f); /* ADR IP, resume (hw1) */
  UT_ASSERT_EQ(read_le16(p + 50), 0x0c10); /*                (hw2) */
  UT_ASSERT_EQ(read_le16(p + 52), 0xf04c); /* ORR.W IP, IP, #1 (hw1) */
  UT_ASSERT_EQ(read_le16(p + 54), 0x0c01); /*                  (hw2) */
  UT_ASSERT_EQ(read_le16(p + 56), 0xf8c1); /* STR IP, [R1, #4] (resume -> buf[1]) */
  UT_ASSERT_EQ(read_le16(p + 58), 0xc004);
  UT_ASSERT_EQ(read_le16(p + 60), 0xf04f); /* MOV.W R3, #0 (hw1) */
  UT_ASSERT_EQ(read_le16(p + 62), 0x0300); /*              (hw2) */
  UT_ASSERT_EQ(read_le16(p + 64), 0xf000); /* B.W +4 (hw1) */
  UT_ASSERT_EQ(read_le16(p + 66), 0xb802); /*         (hw2) */
  UT_ASSERT_EQ(read_le16(p + 68), 0xf04f); /* MOV.W R3, #1 (hw1) */
  UT_ASSERT_EQ(read_le16(p + 70), 0x0301); /*              (hw2) */

  return 0;
}

/* buf == MACH_OP_NONE: setjmp must synthesize a scratch and MOV it to 0
 * rather than crash (dead-path / uninitialized-value tolerance, mirrors
 * mach_ensure_in_reg's MACH_OP_NONE handling documented at its call site). */
UT_TEST(test_setjmp_none_buf_does_not_crash)
{
  setup_gen();

  tcc_gen_machine_setjmp_mop(mop_none(), mop_frame_addr(-32, IROP_BTYPE_INT32), mop_reg(R0, IROP_BTYPE_INT32));

  UT_ASSERT(ind > 0);
  /* buf_reg is allocated via mach_alloc_scratch (picks R0 here, which needs
   * a save/restore wrap since it's "in use"), then synthesized with
   * th_mov_imm(buf_reg, 0, ...): PUSH {r0} ; MOVS R0, #0 (T1 imm8 form). */
  const unsigned char *p = cur_text_section->data;
  UT_ASSERT_EQ(read_le16(p + 0), 0xb401); /* PUSH {r0} */
  UT_ASSERT_EQ((read_le16(p + 2) & 0xf800), 0x2000);
  UT_ASSERT_EQ(read_le16(p + 2) & 0x00ff, 0); /* immediate 0 */

  return 0;
}

/* setjmp with area.kind != MACH_OP_FRAME_ADDR must be treated as a hard
 * compiler error (tcc_error), not silently mis-encoded -- documented
 * directly in the function's own tcc_error() call. We don't invoke this
 * path (tcc_error is stubbed to abort the process in this harness), but we
 * verify the FRAME_ADDR precondition holds for the intended call shape by
 * asserting the other tests above (which explicitly pass FRAME_ADDR) all
 * pass; no separate direct test needed here since triggering the error path
 * would terminate the test binary. */

/* ------------------------------------------------------------------ nl_setjmp */

/* nl_setjmp(buf, dest): buf as MACH_OP_FRAME_ADDR (compiler-allocated
 * 40-byte jmp_buf local), dest in R0. Same tail shape as setjmp (dest=0,
 * B.W skip, dest=1). nl_setjmp's buf goes through
 * mach_ensure_in_reg -> mach_alloc_scratch, which here picks R0; that
 * scratch is saved/restored with a PUSH/POP wrap (same pattern as the
 * nl_longjmp FRAME_ADDR case), so the address computation (SUB.W R0, SP,
 * #36) is bracketed by PUSH {r0} ... POP {r0}.  Because buf_reg (R0) is a
 * LOW register (unlike setjmp's R_IP), the r4-r7 STRs stay 16-bit T1 here
 * (contrast with setjmp's IP-based STRs, which are all 32-bit T3). */
UT_TEST(test_nl_setjmp_frame_buf_reg_dest_shape)
{
  setup_gen();

  tcc_gen_machine_nl_setjmp_mop(mop_frame_addr(-40, IROP_BTYPE_INT32), mop_reg(R0, IROP_BTYPE_INT32));

  UT_ASSERT_EQ(ind, 62);

  const unsigned char *p = cur_text_section->data;

  UT_ASSERT_EQ(read_le16(p + 0), 0xb401); /* PUSH {r0} (save scratch for FRAME_ADDR) */
  UT_ASSERT_EQ(read_le16(p + 2), 0xf1ad); /* SUB.W R0, SP, #36 (hw1) */
  UT_ASSERT_EQ(read_le16(p + 4), 0x0024); /*                   (hw2) */
  UT_ASSERT_EQ(read_le16(p + 6), 0x6004); /* STR R4, [R0, #0]  (T1) */
  UT_ASSERT_EQ(read_le16(p + 8), 0x6045); /* STR R5, [R0, #4]  (T1) */
  UT_ASSERT_EQ(read_le16(p + 10), 0x6086); /* STR R6, [R0, #8]  (T1) */
  UT_ASSERT_EQ(read_le16(p + 12), 0x60c7); /* STR R7, [R0, #12] (T1) */
  UT_ASSERT_EQ(read_le16(p + 14), 0xf8c0); /* STR R8, [R0, #16] (T3) */
  UT_ASSERT_EQ(read_le16(p + 16), 0x8010);
  UT_ASSERT_EQ(read_le16(p + 18), 0xf8c0); /* STR R9, [R0, #20] */
  UT_ASSERT_EQ(read_le16(p + 20), 0x9014);
  UT_ASSERT_EQ(read_le16(p + 22), 0xf8c0); /* STR R10, [R0, #24] */
  UT_ASSERT_EQ(read_le16(p + 24), 0xa018);
  UT_ASSERT_EQ(read_le16(p + 26), 0xf8c0); /* STR R11, [R0, #28] */
  UT_ASSERT_EQ(read_le16(p + 28), 0xb01c);
  UT_ASSERT_EQ(read_le16(p + 30), 0x46ec); /* MOV IP, SP */
  UT_ASSERT_EQ(read_le16(p + 32), 0xf8c0); /* STR IP, [R0, #32] (SP -> buf[8]) */
  UT_ASSERT_EQ(read_le16(p + 34), 0xc020);
  UT_ASSERT_EQ(read_le16(p + 36), 0xf20f); /* ADR IP, resume */
  UT_ASSERT_EQ(read_le16(p + 38), 0x0c10);
  UT_ASSERT_EQ(read_le16(p + 40), 0xf04c); /* ORR.W IP, IP, #1 */
  UT_ASSERT_EQ(read_le16(p + 42), 0x0c01);
  UT_ASSERT_EQ(read_le16(p + 44), 0xf8c0); /* STR IP, [R0, #36] (resume -> buf[9]) */
  UT_ASSERT_EQ(read_le16(p + 46), 0xc024);
  UT_ASSERT_EQ(read_le16(p + 48), 0xf04f); /* MOV.W R0, #0 */
  UT_ASSERT_EQ(read_le16(p + 50), 0x0000);
  UT_ASSERT_EQ(read_le16(p + 52), 0xf000); /* B.W +4 */
  UT_ASSERT_EQ(read_le16(p + 54), 0xb802);
  UT_ASSERT_EQ(read_le16(p + 56), 0xf04f); /* MOV.W R0, #1 */
  UT_ASSERT_EQ(read_le16(p + 58), 0x0001);
  UT_ASSERT_EQ(read_le16(p + 60), 0xbc01); /* POP {r0} (restore FRAME_ADDR scratch) */

  return 0;
}

/* buf == MACH_OP_NONE for nl_setjmp: same tolerant fallback as setjmp.
 *
 * mach_alloc_scratch() can't consult real liveness/allocator state with
 * tcc_state->ir == NULL (this harness's setup), so it conservatively PUSHes
 * a register to free it up before handing it back as the scratch buf_reg --
 * empirically confirmed (temporary stderr byte dump): PUSH {R0} (0xb401),
 * THEN MOVS R0, #0 (0x2000), not MOVS as the very first instruction. */
UT_TEST(test_nl_setjmp_none_buf_does_not_crash)
{
  setup_gen();

  tcc_gen_machine_nl_setjmp_mop(mop_none(), mop_reg(R1, IROP_BTYPE_INT32));

  UT_ASSERT(ind > 4);
  const unsigned char *p = cur_text_section->data;
  UT_ASSERT_EQ(read_le16(p + 0), 0xb401); /* PUSH {R0} -- frees R0 for use as the scratch buf_reg */
  UT_ASSERT_EQ((read_le16(p + 2) & 0xf800), 0x2000); /* MOVS <buf_reg>, #0 */
  UT_ASSERT_EQ(read_le16(p + 2) & 0x00ff, 0);

  return 0;
}

/* ------------------------------------------------------------------ builtin_apply_args */

/* builtin_apply_args(dest): dest = FP + tcc_state->apply_args_offset (SP
 * here since need_frame_pointer is 0), via a direct
 * tcc_machine_addr_of_stack_slot(dest_reg, offset, 0) call -- a single
 * SUB.W dest, SP, #|offset| (offset < 0, unadjusted since
 * allocated_stack_size == 0 and no scratch push precedes it here). */
UT_TEST(test_builtin_apply_args_emits_code_for_dest)
{
  setup_gen();
  tcc_state->apply_args_offset = -20;

  tcc_gen_machine_builtin_apply_args_mop(mop_reg(R0, IROP_BTYPE_INT32));

  UT_ASSERT_EQ(ind, 4);
  const unsigned char *p = cur_text_section->data;
  UT_ASSERT_EQ(read_le16(p + 0), 0xf1ad); /* SUB.W R0, SP, #20 (hw1) */
  UT_ASSERT_EQ(read_le16(p + 2), 0x0014); /*                   (hw2) */

  return 0;
}

/* apply_args_offset == 0: tcc_machine_addr_of_stack_slot's frame_offset==0
 * fast path emits a plain MOV dest, <base_reg> (here SP, since
 * need_frame_pointer is 0) instead of an ADD/SUB #0. */
UT_TEST(test_builtin_apply_args_offset_zero_emits_mov_from_sp)
{
  setup_gen();
  tcc_state->apply_args_offset = 0;

  tcc_gen_machine_builtin_apply_args_mop(mop_reg(R2, IROP_BTYPE_INT32));

  UT_ASSERT_EQ(ind, 2);
  const unsigned char *p = cur_text_section->data;
  UT_ASSERT_EQ(read_le16(p + 0), 0x466a); /* MOV R2, SP */

  return 0;
}

/* ------------------------------------------------------------------ builtin_apply */

/* builtin_apply(fn, args, dest): args and fn both simple REG operands not
 * in the clobbered set (R0-R3, IP), so no relocation-to-safe-scratch step
 * is needed; fn != R_IP so an explicit MOV IP,fn is required.
 * Sequence (fn=R5, args=R6, dest=R2):
 *   MOV   IP, R5                 (fn -> IP)
 *   LDR   R0, [R6, #4]
 *   LDR   R1, [R6, #8]
 *   LDR   R2, [R6, #12]
 *   LDR   R3, [R6, #16]
 *   BLX   IP
 *   MOV   R2, R0                 (dest != R0, so copy result out)
 */
UT_TEST(test_builtin_apply_fn_args_regs_not_clobbered_emits_expected_sequence)
{
  setup_gen();

  tcc_gen_machine_builtin_apply_mop(mop_reg(R5, IROP_BTYPE_INT32), mop_reg(R6, IROP_BTYPE_INT32),
                                    mop_reg(R2, IROP_BTYPE_INT32));

  UT_ASSERT_EQ(ind, 14); /* 7 16-bit instructions */

  const unsigned char *p = cur_text_section->data;
  UT_ASSERT_EQ(read_le16(p + 0), 0x46ac); /* MOV IP, R5 */
  UT_ASSERT_EQ(read_le16(p + 2), 0x6870); /* LDR R0, [R6, #4]  */
  UT_ASSERT_EQ(read_le16(p + 4), 0x68b1); /* LDR R1, [R6, #8]  */
  UT_ASSERT_EQ(read_le16(p + 6), 0x68f2); /* LDR R2, [R6, #12] */
  UT_ASSERT_EQ(read_le16(p + 8), 0x6933); /* LDR R3, [R6, #16] */
  UT_ASSERT_EQ(read_le16(p + 10), 0x47e0); /* BLX IP  (0x4780 | (IP<<3)) */
  UT_ASSERT_EQ(read_le16(p + 12), 0x4602); /* MOV R2, R0 */

  return 0;
}

/* fn already in R_IP: the explicit "MOV IP, fn" must be skipped. */
UT_TEST(test_builtin_apply_fn_already_in_ip_skips_extra_mov)
{
  setup_gen();

  tcc_gen_machine_builtin_apply_mop(mop_reg(R_IP, IROP_BTYPE_INT32), mop_reg(R6, IROP_BTYPE_INT32),
                                    mop_reg(R0, IROP_BTYPE_INT32));

  /* No MOV IP,fn (already there), no MOV dest,R0 (dest==R0):
   * 4x LDR + BLX = 5 16-bit instructions = 10 bytes. */
  UT_ASSERT_EQ(ind, 10);

  const unsigned char *p = cur_text_section->data;
  UT_ASSERT_EQ(read_le16(p + 0), 0x6870); /* LDR R0, [R6, #4]  */
  UT_ASSERT_EQ(read_le16(p + 2), 0x68b1); /* LDR R1, [R6, #8]  */
  UT_ASSERT_EQ(read_le16(p + 4), 0x68f2); /* LDR R2, [R6, #12] */
  UT_ASSERT_EQ(read_le16(p + 6), 0x6933); /* LDR R3, [R6, #16] */
  UT_ASSERT_EQ(read_le16(p + 8), 0x47e0); /* BLX IP */

  return 0;
}

/* dest == R0: the explicit "MOV dest,R0" writeback must be skipped. */
UT_TEST(test_builtin_apply_dest_already_r0_skips_extra_mov)
{
  setup_gen();

  tcc_gen_machine_builtin_apply_mop(mop_reg(R5, IROP_BTYPE_INT32), mop_reg(R6, IROP_BTYPE_INT32),
                                    mop_reg(R0, IROP_BTYPE_INT32));

  /* MOV IP,fn + 4x LDR + BLX = 6 16-bit instructions = 12 bytes, no tail MOV. */
  UT_ASSERT_EQ(ind, 12);

  const unsigned char *p = cur_text_section->data;
  UT_ASSERT_EQ(read_le16(p + 0), 0x46ac); /* MOV IP, R5 */
  UT_ASSERT_EQ(read_le16(p + 10), 0x47e0); /* BLX IP (last instruction) */

  return 0;
}

/* args operand pre-allocated in a clobbered register (R1, part of R0-R3/IP)
 * must be relocated to a safe scratch (chosen outside R0-R3/IP -- observed
 * to be R4, PUSH/POP-wrapped since it is callee-saved) before the restore
 * loads run, otherwise the first load (r0 <- [args+4]) would destroy the
 * base pointer. Full observed sequence:
 *   PUSH {r4}
 *   MOV  R4, R1        relocate args_reg out of the clobbered set
 *   MOV  IP, R5        fn -> IP
 *   LDR  R0, [R4, #4]
 *   LDR  R1, [R4, #8]
 *   LDR  R2, [R4, #12]
 *   LDR  R3, [R4, #16]
 *   BLX  IP
 *   MOV  R2, R0        dest != R0
 *   POP  {r4}
 */
UT_TEST(test_builtin_apply_args_in_clobbered_reg_relocates_to_safe_scratch)
{
  setup_gen();

  tcc_gen_machine_builtin_apply_mop(mop_reg(R5, IROP_BTYPE_INT32), mop_reg(R1, IROP_BTYPE_INT32),
                                    mop_reg(R2, IROP_BTYPE_INT32));

  UT_ASSERT_EQ(ind, 20);

  const unsigned char *p = cur_text_section->data;
  UT_ASSERT_EQ(read_le16(p + 0), 0xb410); /* PUSH {r4} */
  UT_ASSERT_EQ(read_le16(p + 2), 0x460c); /* MOV R4, R1 (relocate args base) */
  UT_ASSERT_EQ(read_le16(p + 4), 0x46ac); /* MOV IP, R5 */
  UT_ASSERT_EQ(read_le16(p + 6), 0x6860); /* LDR R0, [R4, #4]  */
  UT_ASSERT_EQ(read_le16(p + 8), 0x68a1); /* LDR R1, [R4, #8]  */
  UT_ASSERT_EQ(read_le16(p + 10), 0x68e2); /* LDR R2, [R4, #12] */
  UT_ASSERT_EQ(read_le16(p + 12), 0x6923); /* LDR R3, [R4, #16] */
  UT_ASSERT_EQ(read_le16(p + 14), 0x47e0); /* BLX IP */
  UT_ASSERT_EQ(read_le16(p + 16), 0x4602); /* MOV R2, R0 */
  UT_ASSERT_EQ(read_le16(p + 18), 0xbc10); /* POP {r4} */

  return 0;
}
