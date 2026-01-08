/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#pragma once

#include <stdint.h>

#include "tccls.h"

#define PREG_SPILLED 0x80
#define PREG_NONE 0xFF /* pr0/pr1 not allocated (replaces -1 for uint8_t) */

typedef enum TccIrOp
{
  TCCIR_OP_ADD,
  TCCIR_OP_ADC_USE,
  TCCIR_OP_ADC_GEN,
  TCCIR_OP_SUB,
  TCCIR_OP_SUBC_USE,
  TCCIR_OP_SUBC_GEN,
  TCCIR_OP_MUL,
  TCCIR_OP_UMULL,
  TCCIR_OP_DIV,
  TCCIR_OP_UMOD,
  TCCIR_OP_IMOD,
  TCCIR_OP_AND,
  TCCIR_OP_OR,
  TCCIR_OP_XOR,
  TCCIR_OP_SHL,
  TCCIR_OP_SAR,
  TCCIR_OP_SHR,
  TCCIR_OP_PDIV,
  TCCIR_OP_UDIV,
  TCCIR_OP_CMP,
  TCCIR_OP_RETURNVOID,
  TCCIR_OP_RETURNVALUE,
  TCCIR_OP_JUMP,
  TCCIR_OP_JUMPIF,
  /* Indirect jump (computed goto): target in src1 */
  TCCIR_OP_IJUMP,
  TCCIR_OP_SETIF,
  TCCIR_OP_TEST_ZERO,
  TCCIR_OP_FUNCPARAMVOID,
  TCCIR_OP_FUNCPARAMVAL,
  TCCIR_OP_FUNCCALLVOID,
  TCCIR_OP_FUNCCALLVAL,
  TCCIR_OP_LOAD,
  TCCIR_OP_STORE,
  TCCIR_OP_ASSIGN,
  TCCIR_OP_LEA, /* Load Effective Address: dest = &src1 (compute address without loading) */
  /* Floating point operations */
  TCCIR_OP_FADD, /* float/double addition */
  TCCIR_OP_FSUB, /* float/double subtraction */
  TCCIR_OP_FMUL, /* float/double multiplication */
  TCCIR_OP_FDIV, /* float/double division */
  TCCIR_OP_FNEG, /* float/double negation */
  TCCIR_OP_FCMP, /* float/double comparison */
  /* Floating point conversion operations */
  TCCIR_OP_CVT_FTOF, /* float to double or double to float */
  TCCIR_OP_CVT_ITOF, /* int to float/double */
  TCCIR_OP_CVT_FTOI, /* float/double to int */
  /* Logical boolean operations - produce 0/1 result */
  TCCIR_OP_BOOL_OR,  /* (src1 != 0) || (src2 != 0) -> 0/1 */
  TCCIR_OP_BOOL_AND, /* (src1 != 0) && (src2 != 0) -> 0/1 */

  /* Variable-length array (VLA) / dynamic stack allocation */
  TCCIR_OP_VLA_ALLOC,      /* adjust SP by runtime size, with alignment */
  TCCIR_OP_VLA_SP_SAVE,    /* save current SP to a fixed stack slot */
  TCCIR_OP_VLA_SP_RESTORE, /* restore SP from a fixed stack slot */

  /* No-operation placeholder for dead instructions */
  TCCIR_OP_NOP,
} TccIrOp;

typedef struct CType CType;
typedef struct SValue SValue;

typedef struct TACQuadruple TACQuadruple;
typedef struct Sym Sym;

/* Sentinel value indicating an interval hasn't started yet.
 * Using 0xFFFFFFFF since instruction indices are non-negative. */
#define INTERVAL_NOT_STARTED 0xFFFFFFFF

typedef struct IRVregReplacement
{
  uint16_t r0; // first physical register
  uint16_t r1; // second physical register (for long long)
  int offset;  // stack offset if spilled
} IRVregReplacement;

typedef struct IRLiveInterval
{
  uint8_t start_within_if : 1; // whether the interval starts within an if block
  uint8_t addrtaken : 1;       // whether the variable's address is taken
  uint8_t is_float : 1;        // whether this is a float/double variable
  uint8_t is_double : 1;       // whether this is a double (vs float)
  uint8_t is_llong : 1;        // whether this is a long long (64-bit int)
  uint8_t use_vfp : 1;         // whether to use VFP registers (hard float)
  uint8_t is_lvalue : 1;
  uint32_t start; // start instruction index
  uint32_t end;   // end instruction index
  IRVregReplacement allocation;
  int8_t incoming_reg0;    // for params: which register arg arrives in (-1 if stack)
  int8_t incoming_reg1;    // for doubles: second register (-1 if not double or stack)
  int16_t original_offset; // for params: original offset from function entry point
  int stack_slot_index;    // index into stack layout (-1 if not stack-backed)
} IRLiveInterval;

/* IRCallSite: explicit binding of call arguments to a FUNCCALL instruction.
 * Phase 1: arg list references the original FUNCPARAMVAL instructions by index.
 * This avoids backend IR scanning and makes argument ownership stable.
 */
typedef struct IRCallSite
{
  int call_instr_index; /* index into ir->instructions (current, post-opts) */
  int call_orig_index;  /* stable orig_index for debugging/mapping */
  int argc;
  int *arg_instr_index_by_num; /* length argc; each is an index into ir->instructions */
} IRCallSite;

/* SpillCache: Track which registers hold which stack slot values.
 * Used to avoid redundant loads when value is already in a register after storeback.
 * Invalidated by: function calls, branches, stores to different offsets with same register.
 */
#define SPILL_CACHE_SIZE 8
typedef struct SpillCacheEntry
{
  int8_t valid;   // Whether this entry is valid
  int8_t reg;     // Register containing the value
  int32_t offset; // Stack offset (FP-relative)
} SpillCacheEntry;

typedef struct SpillCache
{
  SpillCacheEntry entries[SPILL_CACHE_SIZE];
} SpillCache;

typedef enum TCCStackSlotKind
{
  TCC_STACK_SLOT_SPILL = 1,
  TCC_STACK_SLOT_PARAM_SPILL,
  TCC_STACK_SLOT_LOCAL,
  TCC_STACK_SLOT_VLA_SAVE,
} TCCStackSlotKind;

typedef struct TCCStackSlot
{
  TCCStackSlotKind kind;
  int vreg;      // primary owner vreg (or -1 for shared/fixed slots)
  int offset;    // frame-pointer relative offset (bytes)
  int size;      // slot size in bytes
  int alignment; // required alignment in bytes (power of two)
  uint8_t live_across_calls;
  uint8_t addressable; // non-zero if slot must remain addressable (addr taken)
} TCCStackSlot;

typedef struct TCCStackLayout
{
  TCCStackSlot *slots;
  int slot_count;
  int slot_capacity;
} TCCStackLayout;

typedef struct TCCMachineScratchRegs
{
  unsigned char reg_count;
  unsigned char saved_mask;
  int regs[2];
} TCCMachineScratchRegs;

#define TCC_MACHINE_SCRATCH_NEEDS_PAIR (1u << 0)
#define TCC_MACHINE_SCRATCH_PREFERS_FLOAT (1u << 1)
#define TCC_MACHINE_SCRATCH_ALLOW_REUSE (1u << 2)

typedef struct TCCMaterializedValue
{
  uint8_t used_scratch;
  uint8_t is_64bit;
  uint8_t original_pr0;
  uint8_t original_pr1;
  unsigned short original_r;
  uint64_t original_c_i;
  TCCMachineScratchRegs scratch;
} TCCMaterializedValue;

typedef struct TCCMaterializedAddr
{
  uint8_t used_scratch;
  uint8_t original_pr0;
  uint8_t original_pr1;
  unsigned short original_r;
  uint64_t original_c_i;
  TCCMachineScratchRegs scratch;
} TCCMaterializedAddr;

typedef struct TCCMaterializedDest
{
  uint8_t needs_storeback;
  uint8_t is_64bit;
  uint8_t original_pr0;
  uint8_t original_pr1;
  unsigned short original_r;
  int frame_offset;
  TCCMachineScratchRegs scratch;
} TCCMaterializedDest;

typedef struct TCCIRState
{
  // number of function parameters
  int8_t parameters_count;

  uint8_t leaffunc : 1;
  uint8_t processing_if : 1;
  uint8_t check_for_backwards_jumps : 1;
  uint8_t basic_block_start : 1;
  uint8_t prevent_coalescing;
  int32_t loc;

  TACQuadruple *instructions;
  IRLiveInterval **active_set;

  IRLiveInterval *variables_live_intervals;
  int variables_live_intervals_size;
  int next_local_variable;

  IRLiveInterval *temporary_variables_live_intervals;
  int temporary_variables_live_intervals_size;
  int next_temporary_variable;

  IRLiveInterval *parameters_live_intervals;
  int parameters_live_intervals_size;
  int next_parameter;

  int next_live_interval_index;
  int instructions_size;
  int next_instruction_index;

  /* Current instruction index during code generation - used for scratch register allocation */
  int codegen_instruction_idx;

  /* Callsite table: built from FUNCPARAM* / FUNCCALL* stream so backends
   * and liveness do not need to scan the IR instruction stream.
   */
  IRCallSite *callsites;
  int callsite_count;
  int callsite_capacity;
  int *callsite_index_by_call_instr; /* maps call instruction index -> callsite index */
  int callsite_index_by_call_instr_size;

  uint32_t *ignored_vregs;
  int ignored_vregs_size;

  SpillCache spill_cache; // Cache for tracking register-stack mappings during codegen
  TCCStackLayout stack_layout;

  /* Mapping from IR instruction index to generated machine code offset (section-relative).
   * Size is (next_instruction_index + 1) to include the epilogue mapping.
   * This is populated during tcc_ir_generate_code() and is used after codegen
   * for features like GCC's labels-as-values (&&label). */
  uint32_t *ir_to_code_mapping;
  int ir_to_code_mapping_size;

  /* Mapping from ORIGINAL IR instruction index (pre-DCE/compaction) to generated
   * machine code offset. Label positions (s->jind) are recorded before DCE, so
   * this mapping is the correct one to use for &&label materialization.
   */
  uint32_t *orig_ir_to_code_mapping;
  int orig_ir_to_code_mapping_size;

  LSLiveIntervalState ls;
} TCCIRState;

TCCIRState *tcc_ir_allocate_block();
void tcc_ir_release_block(TCCIRState *ir);

/* If the value is an lvalue (memory reference), emit an IR load so the
 * SValue becomes a plain value suitable for arithmetic/indirect calls. */
void tcc_ir_load_if_lvalue(TCCIRState *ir, SValue *sv);

void tcc_ir_add_function_parameters(TCCIRState *ir, CType *func_type);

int tcc_ir_gvtst(TCCIRState *ir, int inv, int t);

void tcc_ir_gen_opi(TCCIRState *ir, int op);
void tcc_ir_gen_opf(TCCIRState *ir, int op);
int tcc_ir_put(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest);

int tcc_ir_get_vreg_temp(TCCIRState *ir);
int tcc_ir_get_vreg_var(TCCIRState *ir);
int tcc_ir_get_vreg_param(TCCIRState *ir);

void tcc_ir_set_float_type(TCCIRState *ir, int vreg, int is_float, int is_double);
void tcc_ir_set_llong_type(TCCIRState *ir, int vreg);
void tcc_ir_set_original_offset(TCCIRState *ir, int vreg, int offset);
int tcc_ir_get_reg_type(TCCIRState *ir, int vreg);

void tcc_ir_liveness_analysis(TCCIRState *ir);
void tcc_ir_register_allocation_params(TCCIRState *ir);
/* For parameters that arrive on the caller stack (beyond r0-r3 per AAPCS),
 * do not allocate separate local spill slots. They already have a stable
 * incoming stack home for the duration of the call. */
void tcc_ir_avoid_spilling_stack_passed_params(TCCIRState *ir);
void tcc_ir_generate_code(TCCIRState *ir);
void tcc_ir_build_stack_layout(TCCIRState *ir);
const TCCStackSlot *tcc_ir_stack_slot_by_vreg(const TCCIRState *ir, int vreg);
const TCCStackSlot *tcc_ir_stack_slot_by_offset(const TCCIRState *ir, int frame_offset);
void tcc_ir_materialize_value(TCCIRState *ir, SValue *sv, TCCMaterializedValue *result);
void tcc_ir_materialize_addr(TCCIRState *ir, SValue *sv, TCCMaterializedAddr *result);
void tcc_ir_materialize_dest(TCCIRState *ir, SValue *dest, TCCMaterializedDest *result);

/* Build and query the callsite table used for parameter binding.
 * `tcc_ir_build_callsites()` is idempotent and can be called multiple times;
 * it rebuilds the table to match the current IR stream.
 */
void tcc_ir_build_callsites(TCCIRState *ir);
const IRCallSite *tcc_ir_callsite_for_call(const TCCIRState *ir, int call_instr_index);

int tcc_ir_add_local_variable(TCCIRState *ir, Sym *sym, int stack_offset);
void tcc_ir_assign_physical_register(TCCIRState *ir, int vreg, int offset, int r0, int r1);
const char *tcc_ir_get_op_name(TccIrOp op);
void tcc_ir_show(TCCIRState *ir);
void tcc_ir_drop_return_value(TCCIRState *ir);
void tcc_ir_set_addrtaken(TCCIRState *ir, int vreg);

void tcc_ir_patch_live_intervals_registers(TCCIRState *ir);
void tcc_ir_backpatch(TCCIRState *ir, int t, int target_address);
void tcc_ir_backpatch_to_here(TCCIRState *ir, int t);
void tcc_ir_backpatch_first(TCCIRState *ir, int t, int target_address);
int tcc_ir_gjmp_append(TCCIRState *ir, int n, int t);
int tcc_ir_generate_test(TCCIRState *ir, int inv, int t);
int tcc_ir_dead_code_elimination(TCCIRState *ir);
int tcc_ir_dead_store_elimination(TCCIRState *ir);
int tcc_ir_constant_propagation(TCCIRState *ir);
int tcc_ir_tmp_constant_propagation(TCCIRState *ir);
int tcc_ir_copy_propagation(TCCIRState *ir);
int tcc_ir_arithmetic_cse(TCCIRState *ir);
int tcc_ir_bool_cse(TCCIRState *ir);
int tcc_ir_bool_idempotent(TCCIRState *ir);
int tcc_ir_bool_simplification(TCCIRState *ir);
int tcc_ir_return_value_optimization(TCCIRState *ir);
int tcc_ir_store_load_forwarding(TCCIRState *ir);
int tcc_ir_redundant_store_elimination(TCCIRState *ir);
void tcc_ir_print_vreg(int vreg);
void tcc_ir_generate_cmp_jmp_set(TCCIRState *ir);
void tcc_ir_start_basic_block(TCCIRState *ir);

/* Machine-independent spill helpers (defined in tccir.c) */
int tcc_ir_is_spilled(SValue *sv);
int tcc_ir_is_64bit(int t);

/* Machine-dependent spill handling (defined in machine-specific code, e.g., arm-thumb-gen.c) */

/* Spill cache management for avoiding redundant loads */
void tcc_ir_spill_cache_clear(SpillCache *cache);
void tcc_ir_spill_cache_record(SpillCache *cache, int reg, int offset);
int tcc_ir_spill_cache_lookup(SpillCache *cache, int offset);
void tcc_ir_spill_cache_invalidate_reg(SpillCache *cache, int reg);
void tcc_ir_spill_cache_invalidate_offset(SpillCache *cache, int offset);

/* Check if FPU supports double precision (defined in arm-thumb-gen.c) */
int arm_fpu_supports_double(int fpu_type);

typedef enum TCCIR_VREG_TYPE
{
  TCCIR_VREG_TYPE_VAR = 1,
  TCCIR_VREG_TYPE_TEMP = 2,
  TCCIR_VREG_TYPE_PARAM = 3,
} TCCIR_VREG_TYPE;

#define TCCIR_DECODE_VREG_POSITION(vr) (vr & 0xFFFFFFF)
#define TCCIR_DECODE_VREG_TYPE(vr) (vr >> 28)
#define TCCIR_ENCODE_VREG(type, position) (((type) << 28) | (position))