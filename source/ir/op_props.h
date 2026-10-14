/*
 *  TCC IR - One table of opcode properties and the hazard vocabulary
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include <stdint.h>

/* Every fact the optimizer needs about an opcode lives in ir_op_props[]
 * (op_props.c), one row per op, instead of in a private switch in each pass.
 *
 * The low bits are HAZARDS: a reason an instruction may stop a transform that
 * moves, forwards, merges or deletes code across it.  Passes ask
 * "is anything in this range a hazard?" with IR_HZ_ALL minus the classes they
 * have proven harmless (opt_range.h), so a hazard class added here is checked
 * by every such caller without touching them -- deny by default.  Some hazards
 * come from the instruction rather than its opcode (a join, a volatile access,
 * a destination that names memory); ir_op_props[] never sets those.
 *
 * The high bits are ATTRIBUTES: descriptive facts that are never a hazard on
 * their own (has a fourth operand, commutative, ...).
 *
 * Effects a CALL may have (read or write memory, longjmp, ...) are all under
 * IR_HZ_CALL: a pass that has proven a callee harmless drops IR_HZ_CALL, and
 * nothing else then blocks it.  The same holds for the other op classes: the
 * memory a SETJMP writes is IR_HZ_NONLOCAL, not IR_HZ_MEM_WRITE. */

/* ---- hazards from the opcode ---- */
#define IR_HZ_BRANCH 0x00000001u      /* JUMP JUMPIF IJUMP SWITCH_TABLE: control may go elsewhere */
#define IR_HZ_RETURN 0x00000002u      /* RETURNVOID RETURNVALUE */
#define IR_HZ_CALL 0x00000004u        /* FUNCCALLVAL FUNCCALLVOID: an unknown callee runs */
#define IR_HZ_CALL_PARAM 0x00000008u  /* FUNCPARAMVAL FUNCPARAMVOID: bound to a later call */
#define IR_HZ_CALL_SEQ 0x00000010u    /* CALLSEQ_BEGIN/END CALLARG_REG/STACK: lowered call setup */
#define IR_HZ_MEM_READ 0x00000020u    /* the opcode reads memory (LOAD*, BLOCK_COPY, ...) */
#define IR_HZ_MEM_WRITE 0x00000040u   /* the opcode writes memory (STORE*, BLOCK_COPY, ...) */
#define IR_HZ_UPDATES_SRC 0x00000080u /* LOAD/STORE_POSTINC: also advances its pointer operand */
#define IR_HZ_ASM 0x00000100u         /* INLINE_ASM and its ASM_INPUT/ASM_OUTPUT markers */
#define IR_HZ_VLA 0x00000200u         /* VLA_ALLOC VLA_SP_SAVE VLA_SP_RESTORE: dynamic SP */
#define IR_HZ_NONLOCAL 0x00000400u    /* setjmp/longjmp, __builtin_apply* / __builtin_return */
#define IR_HZ_CHAIN 0x00000800u       /* SET_CHAIN INIT_CHAIN_SLOT: nested-function static chain */
#define IR_HZ_TRAP 0x00001000u        /* TRAP (also __builtin_unreachable) */
#define IR_HZ_HINT 0x00002000u        /* PREFETCH: no value, but not dead either */
#define IR_HZ_FLAGS_SET 0x00004000u   /* writes the condition flags */
#define IR_HZ_FLAGS_READ 0x00008000u  /* reads the condition flags */
/* ---- hazards from the instruction (never in ir_op_props[]) ---- */
#define IR_HZ_JOIN 0x00010000u        /* a jump target: control may enter here, NOP or not */
#define IR_HZ_JOIN_END 0x00020000u    /* range queries only: the range's end is a jump target */
#define IR_HZ_VOLATILE 0x00040000u    /* touches volatile memory (tcc_ir_instr_access_is_volatile) */
#define IR_HZ_DEST_LVAL 0x00080000u   /* the destination is an lvalue: the op stores through it */
#define IR_HZ_DEST_STACKOFF 0x00100000u /* the destination is a STACKOFF operand: a frame write */
#define IR_HZ_SRC_LVAL 0x00200000u    /* a source (op4 included) is an lvalue or llocal: a memory read */

/* Every hazard, the default for "nothing in between may matter".  Callers
 * subtract what they have proven harmless; never build a mask by OR-ing up. */
#define IR_HZ_ALL 0x003FFFFFu
#define IR_HZ_FROM_OP 0x0000FFFFu      /* the hazards ir_op_props[] can carry */
#define IR_HZ_FROM_INSTR 0x003F0000u   /* the hazards only an instruction can carry */

/* ---- attributes ---- */
#define IROP_A_NO_FALLTHROUGH 0x01000000u /* never continues at the next instruction */
#define IROP_A_RETURNS_TWICE 0x02000000u  /* SETJMP NL_SETJMP: the next instruction is also a landing */
#define IROP_A_SLOT3 0x04000000u          /* has a fourth operand at pool[operand_base+3] */
#define IROP_A_FP 0x08000000u             /* floating point: an FPU instruction or a soft-float call */
#define IROP_A_COMMUTATIVE 0x10000000u    /* src1 and src2 may be swapped (single-dest ops only) */
#define IROP_A_MAY_BRANCH 0x20000000u     /* INLINE_ASM: an asm goto may jump to one of its labels */
#define IROP_A_KNOWN 0x80000000u          /* the row was written: the selftest rejects a zero row */

/* Ops after which the next instruction is not the only continuation. */
#define IROP_ENDS_BLOCK (IR_HZ_BRANCH | IROP_A_NO_FALLTHROUGH | IROP_A_RETURNS_TWICE | IROP_A_MAY_BRANCH)

extern const uint32_t ir_op_props[];

/* 1 when `op` (any TccIrOp below TCCIR_OP_COUNT) has any of the bits in `mask`. */
static inline int ir_op_has(int op, uint32_t mask)
{
  return (ir_op_props[op] & mask) != 0;
}

/* A set of opcodes, for the few places that must name individual ops (a
 * legacy helper's exact op list, see ir_range_safe_except). */
typedef struct IROpSet
{
  uint64_t w[2];
} IROpSet;

#define IROPSET_BIT_(op, word) ((((unsigned)(op)) >> 6) == (word) ? (uint64_t)1 << ((unsigned)(op) & 63) : 0)
#define IROPSET_W_(word, op) | IROPSET_BIT_(op, word)
/* IROPSET(TCCIR_OP_A, TCCIR_OP_B, ...) -- up to 16 ops, a constant expression. */
#define IROPSET(...) ((IROpSet){{0 IROPSET_EACH_(0, __VA_ARGS__), 0 IROPSET_EACH_(1, __VA_ARGS__)}})
#define IROPSET_NONE ((IROpSet){{0, 0}})

static inline int ir_opset_has(IROpSet s, int op)
{
  return (int)((s.w[((unsigned)op) >> 6] >> (((unsigned)op) & 63)) & 1);
}

/* IROPSET_EACH_(word, a, b, c) -> IROPSET_W_(word, a) IROPSET_W_(word, b) ... */
#define IROPSET_NARG_(...) IROPSET_NARG_I_(__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1)
#define IROPSET_NARG_I_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, N, ...) N
#define IROPSET_CAT_(a, b) IROPSET_CAT_I_(a, b)
#define IROPSET_CAT_I_(a, b) a##b
#define IROPSET_EACH_(w, ...) IROPSET_CAT_(IROPSET_E_, IROPSET_NARG_(__VA_ARGS__))(w, __VA_ARGS__)
#define IROPSET_E_1(w, a) IROPSET_W_(w, a)
#define IROPSET_E_2(w, a, ...) IROPSET_W_(w, a) IROPSET_E_1(w, __VA_ARGS__)
#define IROPSET_E_3(w, a, ...) IROPSET_W_(w, a) IROPSET_E_2(w, __VA_ARGS__)
#define IROPSET_E_4(w, a, ...) IROPSET_W_(w, a) IROPSET_E_3(w, __VA_ARGS__)
#define IROPSET_E_5(w, a, ...) IROPSET_W_(w, a) IROPSET_E_4(w, __VA_ARGS__)
#define IROPSET_E_6(w, a, ...) IROPSET_W_(w, a) IROPSET_E_5(w, __VA_ARGS__)
#define IROPSET_E_7(w, a, ...) IROPSET_W_(w, a) IROPSET_E_6(w, __VA_ARGS__)
#define IROPSET_E_8(w, a, ...) IROPSET_W_(w, a) IROPSET_E_7(w, __VA_ARGS__)
#define IROPSET_E_9(w, a, ...) IROPSET_W_(w, a) IROPSET_E_8(w, __VA_ARGS__)
#define IROPSET_E_10(w, a, ...) IROPSET_W_(w, a) IROPSET_E_9(w, __VA_ARGS__)
#define IROPSET_E_11(w, a, ...) IROPSET_W_(w, a) IROPSET_E_10(w, __VA_ARGS__)
#define IROPSET_E_12(w, a, ...) IROPSET_W_(w, a) IROPSET_E_11(w, __VA_ARGS__)
#define IROPSET_E_13(w, a, ...) IROPSET_W_(w, a) IROPSET_E_12(w, __VA_ARGS__)
#define IROPSET_E_14(w, a, ...) IROPSET_W_(w, a) IROPSET_E_13(w, __VA_ARGS__)
#define IROPSET_E_15(w, a, ...) IROPSET_W_(w, a) IROPSET_E_14(w, __VA_ARGS__)
#define IROPSET_E_16(w, a, ...) IROPSET_W_(w, a) IROPSET_E_15(w, __VA_ARGS__)

/* What a helper that predates this table never checked, kept so it answers
 * exactly as before.  Each use is a known gap -- `grep -rn IR_LEGACY_GAP` lists
 * them -- to be closed or justified in a commit of its own:
 *
 *   ir_op_has(op, IR_HZ_FLAGS_READ) && !ir_opset_has(IR_LEGACY_GAP_OPS(TCCIR_OP_ADC_USE), op)
 *   ir_range_safe(ir, lo, hi, IR_HZ_ALL & ~IR_LEGACY_GAP_HZ(IR_HZ_VOLATILE)) */
#define IR_LEGACY_GAP_OPS(...) IROPSET(__VA_ARGS__)
#define IR_LEGACY_GAP_HZ(bits) (bits)

/* Name of one IR_HZ_* bit, for logs and the selftest; "?" for anything else. */
const char *ir_hazard_name(uint32_t bit);
