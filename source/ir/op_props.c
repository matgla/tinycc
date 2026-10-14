/*
 *  TCC IR - One table of opcode properties
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"

#define K IROP_A_KNOWN

/* One row per opcode; a missing row is 0 and fails the selftest
 * (tests/unit test_op_props.c).  A row lists what the opcode does by itself:
 * a LOAD that reads through an lvalue source is IR_HZ_MEM_READ already, but an
 * ADD with an lvalue source is a memory read only through IR_HZ_SRC_LVAL. */
const uint32_t ir_op_props[TCCIR_OP_COUNT] = {
    /* integer ALU */
    [TCCIR_OP_ADD] = K | IROP_A_COMMUTATIVE,
    [TCCIR_OP_ADC_USE] = K | IR_HZ_FLAGS_READ,
    [TCCIR_OP_ADC_GEN] = K | IR_HZ_FLAGS_SET,
    [TCCIR_OP_SUB] = K,
    [TCCIR_OP_SUBC_USE] = K | IR_HZ_FLAGS_READ,
    [TCCIR_OP_SUBC_GEN] = K | IR_HZ_FLAGS_SET,
    [TCCIR_OP_MUL] = K | IROP_A_COMMUTATIVE,
    [TCCIR_OP_MLA] = K | IROP_A_SLOT3,
    [TCCIR_OP_UMULL] = K,
    [TCCIR_OP_SMULL] = K,
    [TCCIR_OP_UMAAL] = K | IROP_A_SLOT3,
    [TCCIR_OP_DIV] = K,
    [TCCIR_OP_UMOD] = K,
    [TCCIR_OP_IMOD] = K,
    [TCCIR_OP_PDIV] = K,
    [TCCIR_OP_UDIV] = K,
    [TCCIR_OP_AND] = K | IROP_A_COMMUTATIVE,
    [TCCIR_OP_OR] = K | IROP_A_COMMUTATIVE,
    [TCCIR_OP_XOR] = K | IROP_A_COMMUTATIVE,
    [TCCIR_OP_SHL] = K,
    [TCCIR_OP_SAR] = K,
    [TCCIR_OP_SHR] = K,
    [TCCIR_OP_ROR] = K,
    [TCCIR_OP_UBFX] = K,
    [TCCIR_OP_SBFX] = K,
    [TCCIR_OP_BFI] = K,
    [TCCIR_OP_ZEXT] = K,
    [TCCIR_OP_PACK64] = K,
    [TCCIR_OP_BOOL_OR] = K | IROP_A_COMMUTATIVE,
    [TCCIR_OP_BOOL_AND] = K | IROP_A_COMMUTATIVE,
    [TCCIR_OP_CLZ] = K,
    [TCCIR_OP_RBIT] = K,
    [TCCIR_OP_REV] = K,
    [TCCIR_OP_REV16] = K,
    [TCCIR_OP_ASSIGN] = K,
    [TCCIR_OP_LEA] = K,

    /* flags */
    [TCCIR_OP_CMP] = K | IR_HZ_FLAGS_SET,
    [TCCIR_OP_TEST_ZERO] = K | IR_HZ_FLAGS_SET,
    [TCCIR_OP_SETIF] = K | IR_HZ_FLAGS_READ,
    [TCCIR_OP_SELECT] = K | IR_HZ_FLAGS_READ | IROP_A_SLOT3,

    /* floating point */
    [TCCIR_OP_FADD] = K | IROP_A_FP,
    [TCCIR_OP_FSUB] = K | IROP_A_FP,
    [TCCIR_OP_FMUL] = K | IROP_A_FP,
    [TCCIR_OP_FDIV] = K | IROP_A_FP,
    [TCCIR_OP_FNEG] = K | IROP_A_FP,
    [TCCIR_OP_FCMP] = K | IROP_A_FP | IR_HZ_FLAGS_SET,
    [TCCIR_OP_CVT_FTOF] = K | IROP_A_FP,
    [TCCIR_OP_CVT_ITOF] = K | IROP_A_FP,
    [TCCIR_OP_CVT_FTOI] = K | IROP_A_FP,

    /* control flow */
    [TCCIR_OP_JUMP] = K | IR_HZ_BRANCH | IROP_A_NO_FALLTHROUGH,
    [TCCIR_OP_JUMPIF] = K | IR_HZ_BRANCH | IR_HZ_FLAGS_READ,
    [TCCIR_OP_IJUMP] = K | IR_HZ_BRANCH | IROP_A_NO_FALLTHROUGH,
    [TCCIR_OP_SWITCH_TABLE] = K | IR_HZ_BRANCH | IROP_A_NO_FALLTHROUGH,
    [TCCIR_OP_RETURNVOID] = K | IR_HZ_RETURN | IROP_A_NO_FALLTHROUGH,
    [TCCIR_OP_RETURNVALUE] = K | IR_HZ_RETURN | IROP_A_NO_FALLTHROUGH,
    [TCCIR_OP_TRAP] = K | IR_HZ_TRAP | IROP_A_NO_FALLTHROUGH,

    /* calls */
    [TCCIR_OP_FUNCPARAMVOID] = K | IR_HZ_CALL_PARAM,
    [TCCIR_OP_FUNCPARAMVAL] = K | IR_HZ_CALL_PARAM,
    [TCCIR_OP_FUNCCALLVOID] = K | IR_HZ_CALL,
    [TCCIR_OP_FUNCCALLVAL] = K | IR_HZ_CALL,
    [TCCIR_OP_CALLSEQ_BEGIN] = K | IR_HZ_CALL_SEQ,
    [TCCIR_OP_CALLARG_REG] = K | IR_HZ_CALL_SEQ,
    [TCCIR_OP_CALLARG_STACK] = K | IR_HZ_CALL_SEQ | IR_HZ_MEM_WRITE,
    [TCCIR_OP_CALLSEQ_END] = K | IR_HZ_CALL_SEQ,
    [TCCIR_OP_SET_CHAIN] = K | IR_HZ_CHAIN,
    [TCCIR_OP_INIT_CHAIN_SLOT] = K | IR_HZ_CHAIN | IR_HZ_MEM_WRITE,

    /* memory */
    [TCCIR_OP_LOAD] = K | IR_HZ_MEM_READ,
    [TCCIR_OP_STORE] = K | IR_HZ_MEM_WRITE,
    [TCCIR_OP_LOAD_INDEXED] = K | IR_HZ_MEM_READ | IROP_A_SLOT3,
    [TCCIR_OP_STORE_INDEXED] = K | IR_HZ_MEM_WRITE | IROP_A_SLOT3,
    [TCCIR_OP_LOAD_POSTINC] = K | IR_HZ_MEM_READ | IR_HZ_UPDATES_SRC,
    [TCCIR_OP_STORE_POSTINC] = K | IR_HZ_MEM_WRITE | IR_HZ_UPDATES_SRC,
    [TCCIR_OP_BLOCK_COPY] = K | IR_HZ_MEM_READ | IR_HZ_MEM_WRITE,
    [TCCIR_OP_SWITCH_LOAD] = K | IR_HZ_MEM_READ, /* its inline value table */
    [TCCIR_OP_RETURN_ADDRESS] = K | IR_HZ_MEM_READ, /* the saved-LR slot */
    [TCCIR_OP_PREFETCH] = K | IR_HZ_HINT,

    /* dynamic stack */
    [TCCIR_OP_VLA_ALLOC] = K | IR_HZ_VLA,
    [TCCIR_OP_VLA_SP_SAVE] = K | IR_HZ_VLA,
    [TCCIR_OP_VLA_SP_RESTORE] = K | IR_HZ_VLA,

    /* inline asm */
    [TCCIR_OP_ASM_INPUT] = K | IR_HZ_ASM,
    [TCCIR_OP_INLINE_ASM] = K | IR_HZ_ASM | IROP_A_MAY_BRANCH,
    [TCCIR_OP_ASM_OUTPUT] = K | IR_HZ_ASM,

    /* non-local control */
    [TCCIR_OP_SETJMP] = K | IR_HZ_NONLOCAL | IROP_A_RETURNS_TWICE,
    [TCCIR_OP_NL_SETJMP] = K | IR_HZ_NONLOCAL | IROP_A_RETURNS_TWICE,
    [TCCIR_OP_LONGJMP] = K | IR_HZ_NONLOCAL | IROP_A_NO_FALLTHROUGH,
    [TCCIR_OP_NL_LONGJMP] = K | IR_HZ_NONLOCAL | IROP_A_NO_FALLTHROUGH,
    [TCCIR_OP_BUILTIN_APPLY_ARGS] = K | IR_HZ_NONLOCAL,
    [TCCIR_OP_BUILTIN_APPLY] = K | IR_HZ_NONLOCAL,
    [TCCIR_OP_BUILTIN_RETURN] = K | IR_HZ_NONLOCAL | IROP_A_NO_FALLTHROUGH,

    [TCCIR_OP_NOP] = K,
};

const char *ir_hazard_name(uint32_t bit)
{
  switch (bit)
  {
  case IR_HZ_BRANCH:
    return "branch";
  case IR_HZ_RETURN:
    return "return";
  case IR_HZ_CALL:
    return "call";
  case IR_HZ_CALL_PARAM:
    return "call-param";
  case IR_HZ_CALL_SEQ:
    return "call-seq";
  case IR_HZ_MEM_READ:
    return "mem-read";
  case IR_HZ_MEM_WRITE:
    return "mem-write";
  case IR_HZ_UPDATES_SRC:
    return "updates-src";
  case IR_HZ_ASM:
    return "asm";
  case IR_HZ_VLA:
    return "vla";
  case IR_HZ_NONLOCAL:
    return "nonlocal";
  case IR_HZ_CHAIN:
    return "chain";
  case IR_HZ_TRAP:
    return "trap";
  case IR_HZ_HINT:
    return "hint";
  case IR_HZ_FLAGS_SET:
    return "flags-set";
  case IR_HZ_FLAGS_READ:
    return "flags-read";
  case IR_HZ_JOIN:
    return "join";
  case IR_HZ_JOIN_END:
    return "join-end";
  case IR_HZ_VOLATILE:
    return "volatile";
  case IR_HZ_DEST_LVAL:
    return "dest-lval";
  case IR_HZ_DEST_STACKOFF:
    return "dest-stackoff";
  case IR_HZ_SRC_LVAL:
    return "src-lval";
  default:
    return "?";
  }
}
