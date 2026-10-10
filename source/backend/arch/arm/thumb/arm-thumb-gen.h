/*
 *  ARMvX-m code generator for TCC
 *  Uses thumb instruction set
 *
 *  Based on:
 *  ARM Thumb 2 instruction functions for TCC
 *  Copyright (c) 2020 Erlend J. Sveen
 *  from:
 * https://git.erlendjs.no/erlendjs/tinycc/-/blob/arm-thumb/arm-thumb-gen.c
 *        https://git.erlendjs.no/erlendjs/tinycc/-/blob/arm-thumb/arm-thumb-instructions.c
 *
 *  And
 *
 *  ARMv4 code generator for TCC
 *
 *  Copyright (c) 2003 Daniel Glöckner
 *  Copyright (c) 2012 Thomas Preud'homme
 *
 *  Based on i386-gen.c by Fabrice Bellard
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

/* Declarations shared by the Thumb-2 code generator's source files
 * (arm-thumb-gen.c and arm-thumb-{emit,alu,mem,fp,frame,call}.c), which
 * used to be one 18k-line TU.  Each function or variable declared at the
 * bottom is defined in the file its comment names. */

#ifndef ARM_THUMB_GEN_H
#define ARM_THUMB_GEN_H

#if defined(TCC_ARM_EABI) && !defined(TCC_ARM_VFP)
#error "Currently TinyCC only supports float computation with VFP instructions"
#endif

#ifndef CONFIG_TCC_CPUVER
#define CONFIG_TCC_CPUVER 5
#endif

#include "source/backend/arch/arm/arm.h"
#include "source/backend/arch/arm/ssa_opt_arm.h"
#include "arm-thumb-defs.h"
#include "source/opt/include/opt.h"
#include "tcc-chained-hash.h"
#include "tcc.h"
#include "tccir.h"
#include "tccls.h"
#include "tcctype.h"
#include "arm-thumb-callsite.h"
#include "memory/small_sequence.h"

/* Inline-first literal-pool position scratch (heap only past the inline cap). */
TCC_SMALL_SEQUENCE_DEFINE(ThumbLitPosSeq, int, 128)

/* Helper macro: split a 64-bit value into (lo, hi) uint32_t pair for load_full_const.
 * Avoids int64_t in function signatures — TCC ARM codegen miscounts int64_t register pairs. */
#define LFC_SPLIT(v) (uint32_t)((uint64_t)(v)), (uint32_t)((uint64_t)(v) >> 32)

enum Armv8mRegisters
{
  ARM_R0 = 0,
  ARM_R1 = 1,
  ARM_R2 = 2,
  ARM_R3 = 3,
  ARM_R4 = 4,
  ARM_R5 = 5,
  ARM_R6 = 6,
  ARM_R7 = 7,
  ARM_R8 = 8,
  ARM_R9 = 9,
  ARM_R10 = 10,
  ARM_R11 = 11,
  ARM_R12 = 12,
  ARM_SP = 13,
  ARM_LR = 14,
  ARM_PC = 15
};

#define USING_GLOBALS
#include "tcc.h"

#include <stdio.h>
#include <stdlib.h>

#include "source/backend/arch/fpu/arm/fpv5-sp-d16.h"
#include "source/backend/arch/fpu/arm/rp2350-dcp.h"
#include "source/backend/arch/fpu/arm/fpv5-d16.h"
#include "source/backend/arch/arm/thumb/thumb.h"
#include "source/backend/arch/arm/thumb/thop_adr.h"
#include "source/backend/arch/arm/thumb/thop_alu_imm.h"
#include "source/backend/arch/arm/thumb/thop_alu_reg.h"
#include "source/backend/arch/arm/thumb/thop_block.h"
#include "source/backend/arch/arm/thumb/thop_branch.h"
#include "source/backend/arch/arm/thumb/thop_cmp.h"
#include "source/backend/arch/arm/thumb/thop_extend.h"
#include "source/backend/arch/arm/thumb/thop_coproc.h"
#include "source/backend/arch/arm/thumb/thop_vfp.h"
#include "source/backend/arch/arm/thumb/thop_ldr_literal.h"
#include "source/backend/arch/arm/thumb/thop_ldrd.h"
#include "source/backend/arch/arm/thumb/thop_mem_imm.h"
#include "source/backend/arch/arm/thumb/thop_mem_reg.h"
#include "source/backend/arch/arm/thumb/thop_mov.h"
#include "source/backend/arch/arm/thumb/thop_mul.h"
#include "source/backend/arch/arm/thumb/thop_mvn.h"
#include "source/backend/arch/arm/thumb/thop_pld.h"
#include "source/backend/arch/arm/thumb/thop_shift_imm.h"
#include "source/backend/arch/arm/thumb/thop_rev.h"
#include "source/backend/arch/arm/thumb/thop_shift_reg.h"
#include "source/backend/arch/arm/thumb/thop_system.h"

#include <inttypes.h>
ST_FUNC void tcc_gen_machine_strldr_cache_set_enabled(int enabled);
ST_FUNC void tcc_gen_machine_imm_cache_reset(void);
/* Structure to track scratch register allocation with potential save/restore */
typedef struct ScratchRegAlloc
{
  int reg : 29;            /* The allocated scratch register (range 0-15 for ARM) */
  uint32_t saved : 2;      /* 0=not saved, 1=PUSH to stack, 2=STR to scratch area */
  uint32_t would_save : 1; /* Whether a push was needed (set in both dry-run and real emit) */
} ScratchRegAlloc;

typedef struct ScratchRegAllocs
{
  int regs[8];         /* The allocated scratch registers */
  int count;           /* Number of registers allocated */
  uint32_t saved_mask; /* Bitmask of registers that were saved (pushed) */
} ScratchRegAllocs;

/* ============================================================
 * MachineCodegenContext — per-instruction scratch-register tracker
 * ============================================================
 * Used by the MachineOperand-based (_mop) code-generation path.
 * Callers allocate scratches via mach_alloc_scratch(), then call
 * mach_release_all() at the end of the instruction to pop them in LIFO order.
 */

/* Forward declarations needed by the mach_* helpers (defined later in this file). */
typedef thumb_opcode (*thumb_imm_handler_t)(uint32_t rd, uint32_t rn, uint32_t imm,
                                            thumb_flags_behaviour flags_behaviour,
                                            thumb_enforce_encoding enforce_encoding);

#define MACH_CTX_MAX_SCRATCH 12

typedef struct MachineCodegenContext
{
  ScratchRegAlloc scratches[MACH_CTX_MAX_SCRATCH];
  int n_scratch;
} MachineCodegenContext;

/* --- Hard-float single-precision VFP support ---
 * Float operands reach codegen as MACH_OP_VFP_REG (u.reg.r0 = s-register 0-31),
 * a distinct kind from GPR MACH_OP_REG.  s14/s15 are held out of the allocator
 * (arm_init) as fixed VFP scratch so the FP emitters can shuttle GPR/imm/spill
 * operands through the FPU without clobbering a live float.  is_vfp_reg tests a
 * raw allocation register number (LS_VFP_REG_BASE + n) as written by the RA. */
#define VFP_SCRATCH0 14
#define VFP_SCRATCH1 15

/* ============================================================
 * Dry-Run Code Generation State
 * ============================================================
 * Two-pass code generation system for optimal register allocation.
 * Pass 1 (Dry Run): Analyze register needs without emitting code
 * Pass 2 (Real Emit): Generate code with optimal prologue based on Pass 1
 */

typedef struct CodeGenDryRunState
{
  int active;                   /* 1 = dry run, 0 = real emit */
  uint32_t scratch_regs_pushed; /* Bitmap of regs pushed as scratch */
  int scratch_push_count;       /* Total scratch push operations */
  int lr_push_count;            /* Times LR specifically was pushed */
  int instruction_count;        /* IR instructions processed */
  int max_nested_saves;         /* Max nested-call save slots used at any call site */
  int rodata_anchor_sites;      /* Shared-.rodata addresses materialised in the body */
} CodeGenDryRunState;
/* True only for the discovery dry run: "model what would happen" rather than
 * "emit what the real pass emits". */
#define DRY_RUN_MODELLING (dry_run_state.active && !dry_run_rehearsal)

/* Literal pool dedup uses the same bucket+chain scheme as TinyCC's ELF hashes.
 * We only hash entries created through th_literal_pool_find_or_allocate(), so
 * plain th_literal_pool_allocate() users stay distinct. */
#define LITERAL_POOL_HASH_BUCKET_COUNT 512
/* First size of the real and dry-run literal pool arrays; both grow by
 * doubling in th_literal_pool_allocate and live for the whole TU. */
#ifdef CONFIG_TCC_LOW_MEM
#define LITERAL_POOL_FIRST_SIZE 16
#else
#define LITERAL_POOL_FIRST_SIZE 64
#endif
#define LITERAL_POOL_LOOKUP_CACHE_SIZE 16

typedef struct LiteralPoolLookupCacheEntry
{
  Sym *sym;
  int64_t imm;
  int pool_index;
  uint32_t hash;
  int valid;
} LiteralPoolLookupCacheEntry;

typedef struct LiteralPoolLookupCache
{
  LiteralPoolLookupCacheEntry entries[LITERAL_POOL_LOOKUP_CACHE_SIZE];
} LiteralPoolLookupCache;

/* Structure to save/restore thumb_gen_state for dry-run isolation */
typedef struct ThumbGenStateSnapshot
{
  int code_size;
  int literal_pool_count;
  int literal_pool_size;
  int pool_window_first;
  int pool_bytes;
  ThumbLiteralPoolEntry *literal_pool;
  Sym *cached_global_sym;
  int cached_global_reg;
  int function_argument_count;
  int call_sites_by_id_size;
  ThumbGenCallSite *call_sites_by_id;
} ThumbGenStateSnapshot;

/* ============================================================
 * Branch Instruction Optimization State
 * ============================================================
 * Tracks branch instructions during dry-run to select optimal
 * 16-bit vs 32-bit encodings based on actual jump distances.
 */

typedef enum
{
  BRANCH_ENC_UNKNOWN = 0,
  BRANCH_ENC_16BIT = 16,
  BRANCH_ENC_32BIT = 32
} BranchEncoding;

typedef struct BranchInfo
{
  int ir_index;            /* IR instruction index of the branch */
  int source_addr;         /* Code address where branch is emitted */
  int target_ir;           /* Target IR instruction index */
  int target_addr;         /* Target code address (computed after dry-run) */
  int offset;              /* Computed offset = target - source - 4 */
  int is_conditional;      /* 1 = conditional (JUMPIF), 0 = unconditional (JUMP) */
  BranchEncoding encoding; /* Selected encoding after analysis */
} BranchInfo;

typedef struct BranchOptState
{
  BranchInfo *branches;     /* Array of branch info */
  int branch_count;         /* Number of branches */
  int branch_capacity;      /* Allocated capacity */
  int optimization_enabled; /* Flag to enable/disable */
  int code_size_reduction;  /* Total bytes saved */
} BranchOptState;

/* Forward declarations for helpers used by spill preloading. */
int load_short_from_base(int ir, int base, int fc, int sign);
int load_ushort_from_base(int ir, int base, int fc, int sign);
int load_byte_from_base(int ir, int base, int fc, int sign);
int load_ubyte_from_base(int ir, int base, int fc, int sign);

/* Immediate-value cache: tracks the last pure-integer constant loaded into
 * each register by tcc_machine_load_constant (no symbol involved).  Persists
 * across IR instruction boundaries so consecutive STORE instructions that
 * materialise the same constant can skip the redundant MOV.  Reset at jump
 * targets and function calls. */
/* Per-register materialisation cache.  `sym == NULL` means the register holds
 * the plain constant `value`; `sym != NULL` means it holds the address of that
 * symbol plus addend `value` (so a later reference to the same global address
 * can skip the redundant literal-pool load).  Invalidated per-register on every
 * clobbering emit and at IR boundaries, just like the constant cache. */
typedef struct ImmCacheEntry { int64_t value; Sym *sym; uint8_t valid; } ImmCacheEntry;

/* Bit layout of tcc_ir_zero_half64_at's byte, as it arrives in bits 18-23 of
 * `barrel_shift`.  Mirrors source/opt/flat/fusion/zero_half64.c. */
#define ZH64_S1_LO 0x01u
#define ZH64_S1_HI 0x02u
#define ZH64_S2_LO 0x04u
#define ZH64_S2_HI 0x08u
#define ZH64_D_LO 0x10u
#define ZH64_D_HI 0x20u
/* Bit 6 does not come from zero_half64 -- it is source/opt/flat/fusion/
 * cmp_hi_only.c's verdict, sharing the same side table and the same journey
 * through `barrel_shift`: this 64-bit CMP is against a constant whose low half
 * is zero and every reader of its flags is a strict order test, so the low
 * words cannot change the answer and only the high words need comparing. */
#define ZH64_CMP_HI 0x40u

/* ---------------------------------------------------------------------------
 * STR -> LDR redundant-reload peephole
 *
 * Tracks recent immediate-offset STR Rt, [Rn, #imm] emissions and skips the
 * subsequent LDR Rt, [Rn, #imm] at the call site when Rt is still known to
 * hold the stored value.  The cache is reset at every IR instruction
 * boundary (via tcc_gen_machine_mov_coalesce_reset, same hook as plan C)
 * so that cross-IR equivalences cannot be exploited — any IR op may be a
 * branch target, and the runtime register/memory state on an entry-by-jump
 * path is not what the emission-order state predicts.
 *
 * Only puw == 6 (P=1, U=1, W=0 — no writeback) STR/LDR forms are tracked.
 * The classifier below recognises the T1 16-bit, T2 16-bit SP-relative, and
 * T3 32-bit encodings (those that cover the common stack-spill path).
 * --------------------------------------------------------------------------- */

typedef struct StrLdrCacheEntry
{
  uint8_t valid;
  uint8_t rt;
  uint8_t rn;
  uint8_t size;  /* encoding size, 2 or 4 */
  uint8_t width; /* bytes the access moves; only 4 is ever recorded */
  int imm;
  uint32_t puw;
} StrLdrCacheEntry;

#define STRLDR_CACHE_CAPACITY 8

/* The last word access to the frame, if it was the last instruction emitted:
 * what frame_word_pair_rewind pairs the next one with. */
typedef struct FrameWordAccess
{
  int kind; /* 0 none, 1 STR, 2 LDR */
  int start, end;
  int reg, base, off; /* off: signed byte offset from base */
} FrameWordAccess;

/* Architectural ceiling for a pool window: the patch in
 * th_literal_pool_generate() encodes (aligned_position - 4), and both the T1
 * LDR-literal and the LDRD-literal forms carry an 8-bit word-scaled offset,
 * i.e. at most 1020 bytes from the load to its literal. */
#define THUMB_POOL_RANGE_LIMIT 1020
/* Slack under the ceiling for what the span measurement cannot see coming: a
 * flush suppressed by op_in_it_block is deferred to the end of the IT block
 * (up to 4 conditioned insns, each able to add a pool entry), and multi-op
 * sequences reserve only an estimate of their size. */
#define THUMB_POOL_FLUSH_SLACK 64

typedef thumb_opcode (*thumb_reg_handler_t)(uint32_t rd, uint32_t rn, uint32_t rm,
                                            thumb_flags_behaviour flags_behaviour, thumb_shift shift_type,
                                            thumb_enforce_encoding enforce_encoding);
typedef struct ThumbDataProcessingHandler
{
  thumb_imm_handler_t imm_handler;
  thumb_reg_handler_t reg_handler;
} ThumbDataProcessingHandler;

/* ============================================================
 * thumb_and_imm_form / thumb_emit_and_imm_special
 * ============================================================
 * One-instruction lowerings of `AND Rd, Rn, #mask` that never materialize the
 * mask.  Thumb-2's AND-immediate takes only a modified immediate (8 bits
 * rotated, or a byte replicated across the word), so an ordinary bitfield mask
 * -- 0x7FF, 0x000FFFFF, 0x007FFFFF -- costs a movw/movt pair or a literal-pool
 * load on top of the AND.  In the soft-float library that pair is the single
 * most common shape there is: every classifier opens with
 * `bits & DOUBLE_MANT_MASK`.
 *
 * The classification is a pure function of the mask so that a caller can
 * decide BEFORE resolving operands into registers -- resolving can itself emit
 * (a spilled source becomes a load), so a caller that resolved first and then
 * fell through to the general path would emit that load twice.
 *
 * None of these forms writes the flags, so the caller must already have
 * established that no flag result is wanted.  `flags` is passed through for
 * BIC, which does have a flag-setting encoding.
 */
typedef enum ThumbAndImmForm
{
  AND_IMM_NONE = 0, /* nothing better than the general path */
  AND_IMM_UXTB,     /* mask 0xFF */
  AND_IMM_UXTH,     /* mask 0xFFFF */
  AND_IMM_UBFX,     /* low-contiguous run of ones */
  AND_IMM_BIC,      /* complement encodes as a modified immediate */
} ThumbAndImmForm;

typedef thumb_opcode (*thumb_regonly3_handler_t)(uint32_t rd, uint32_t rn, uint32_t rm);

typedef thumb_opcode (*thumb_longmul_handler_t)(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm);

typedef enum ThumbArgMoveKind
{
  THUMB_ARG_MOVE_REG,
  THUMB_ARG_MOVE_IMM,
  THUMB_ARG_MOVE_IMM64,      /* load 64-bit immediate into register pair */
  THUMB_ARG_MOVE_LOCAL_ADDR, /* compute address of local: fp + offset */
  THUMB_ARG_MOVE_STRUCT,     /* load struct words into consecutive registers */
  THUMB_ARG_MOVE_MOP,        /* generic: load MachineOperand into dst_reg (+ dst_reg_hi for 64-bit) */
} ThumbArgMoveKind;

typedef struct ThumbArgMove
{
  ThumbArgMoveKind kind;
  int dst_reg;
  int dst_reg_hi;        /* valid when kind==THUMB_ARG_MOVE_IMM64 */
  int src_reg;           /* valid when kind==THUMB_ARG_MOVE_REG */
  uint32_t imm;          /* valid when kind==THUMB_ARG_MOVE_IMM */
  uint64_t imm64;        /* valid when kind==THUMB_ARG_MOVE_IMM64 */
  Sym *sym;              /* valid when kind==THUMB_ARG_MOVE_IMM */
  int local_offset;      /* valid when kind==THUMB_ARG_MOVE_LOCAL_ADDR */
  int local_is_param;    /* valid when kind==THUMB_ARG_MOVE_LOCAL_ADDR - if true, add offset_to_args */
  int struct_word_count; /* valid when kind==THUMB_ARG_MOVE_STRUCT */
  int struct_src_align;  /* struct natural alignment (bytes); gates source LDRD */
  MachineOperand mop;    /* valid when kind==THUMB_ARG_MOVE_MOP */
} ThumbArgMove;

/* Context for function call generation - reduces parameter passing */
typedef struct CallGenContext
{
  ThumbGenCallSite *call_site;
  TCCAbiCallLayout *layout;
  IROperand *args;
  MachineOperand *mops;
  int argc;
  int stack_size;
  uint32_t arg_move_dst_mask; /* Registers that will be explicitly written by register arg moves.
                               * These are safe to clobber as scratch during stack arg placement
                               * because the subsequent register moves will overwrite them. */
  uint32_t call_target_regs;  /* Registers naming the indirect call target (read at the call). */
} CallGenContext;

/* Copy a (possibly split) struct argument's stack portion into the outgoing
 * argument area.  `src_align` is the struct's natural alignment in bytes.
 *
 * Adjacent word pairs are copied with LDRD/STRD instead of two LDR/STR.  The
 * destination is the outgoing arg area — SP-relative with a word-multiple
 * offset and SP 8-byte aligned at the call boundary — so STRD is always
 * alignment-safe.  LDRD additionally requires the *source* address to be
 * 4-byte aligned, which holds exactly when the struct's natural alignment is
 * >= 4 (the stack portion starts at base + words_in_regs*4, a word multiple). */
/* Stack-passed struct arguments at least this many words are copied through
 * r0-r3/IP/LR saved around the copy -- LDM/STM, or a memcpy call for an
 * unaligned source -- rather than with scratch registers found free one pair
 * at a time (~4 bytes a word).  Measured with the memcpy form on zig.c .text:
 * 32 words 5,822,056; 8 words 5,754,812; 6 words 5,751,108; 4 words
 * 5,772,344 -- 8 keeps 6- and 7-word structs on the faster inline copy. */
#define STACK_ARG_MEMCPY_MIN_WORDS 8
#define STACK_ARG_UNROLL_MAX_WORDS 16

/* One collected immediate stack store, for the grouped/windowed emission path. */
typedef struct StackImmArg
{
  int off;
  uint32_t val;
} StackImmArg;

/* Defined in arm-thumb-gen.c. */
extern struct Sym *_lfc_sym;
extern ThumbGeneratorState thumb_gen_state;
extern enum float_abi float_abi;
extern unsigned char text_and_data_separation;
extern unsigned char allow_r9_write;
extern unsigned char pic;
extern unsigned char sb_relative_got;
extern int offset_to_args;
extern uint32_t pushed_registers;
extern int allocated_stack_size;
extern int epilogue_stack_dealloc;
extern int vararg_push_size;
extern uint32_t scratch_global_exclude;
extern int rodata_anchor_reg;
extern signed char scratch_push_type[128];
extern int scratch_push_count;
extern int tail_call_pending;
extern int g_debug_current_op;
extern CodeGenDryRunState dry_run_state;
extern int dry_run_rehearsal;
extern int helper_call_sp_bias;
extern int call_args_sp_bias;
extern int asm_save_sp_bias;
extern ThumbLiteralPoolEntry *dry_run_literal_pool;
extern int dry_run_literal_pool_count;
extern int pool_entries_total;
extern int align_pad_max;
extern int dry_run_literal_pool_size;
extern TCCChainedHash literal_pool_hash;
extern LiteralPoolLookupCache literal_pool_last_lookup;
extern int mov_equiv_it_pending;
extern ImmCacheEntry imm_cache[16];
extern int pool_flush_it_pending;
extern FrameWordAccess frame_word_last;
extern int slot_self_copy_off;
thumb_flags_behaviour flags_safe(void);
Sym *validate_sym_for_reloc(Sym *sym);
int fp_adjust_local_offset(int frame_offset, int is_param);
int param_frame_offset(int param_off);
uint32_t scratch_exclude_baseline(void);
int resolve_chain_base(TCCIRState *ir, int ci, uint32_t exclude_regs, ScratchRegAlloc *out_scratch,
                              int *used_scratch);
thumb_opcode thumb_call_imm_handler(thumb_imm_handler_t fn, uint32_t rd, uint32_t rn, uint32_t imm,
                                           thumb_flags_behaviour flags, thumb_enforce_encoding encoding);
int is_vfp_reg(int r);
int vfp_num(int r);
int mach_alloc_scratch(MachineCodegenContext *ctx, uint32_t excl);
int mach_alloc_scratch_for_sym(MachineCodegenContext *ctx, uint32_t excl, Sym *sym, int64_t imm);
void mach_release_all(MachineCodegenContext *ctx);
int mach_var_owns_spill_slot(int vreg);
void mach_load_slot(int dest_reg, const MachineOperand *op);
int mach_slot_load_width(const MachineOperand *op);
int mach_slot_store_width(const MachineOperand *op);
int mach_ensure_in_reg(MachineCodegenContext *ctx, const MachineOperand *op, uint32_t excl);
uint32_t mach_reg_excl(const MachineOperand *op);
int mach_ensure_imm_or_reg(MachineCodegenContext *ctx, const MachineOperand *op, uint32_t excl,
                                  thumb_imm_handler_t imm_handler, int dest_reg, int src1_reg,
                                  thumb_flags_behaviour flags, bool *imm_emitted);
int mach_get_dest_reg(MachineCodegenContext *ctx, const MachineOperand *op, uint32_t excl);
void mach_writeback_dest(const MachineOperand *op, int reg);
void tcc_gen_mach_load_to_reg(int dest_reg, const MachineOperand *op);
void map_sym_d(void);
void map_sym_t(void);
int scratch_push_sp_bias(void);
uint32_t literal_pool_hash_func(Sym *sym, int64_t imm);
int literal_pool_lookup_cache_find(LiteralPoolLookupCache *cache, uint32_t full_hash, Sym *sym,
                                                 int64_t imm);
void literal_pool_lookup_cache_insert(LiteralPoolLookupCache *cache, uint32_t full_hash, Sym *sym,
                                                    int64_t imm, int pool_index);
int literal_pool_hash_find(TCCChainedHash *hash, ThumbLiteralPoolEntry *pool, uint32_t full_hash,
                                         Sym *sym, int64_t imm);
void literal_pool_hash_insert(TCCChainedHash *hash, uint32_t full_hash, int pool_index);
int branch_fits_t1(int offset);
int branch_fits_t2(int offset);
ScratchRegAlloc get_scratch_reg_with_save(uint32_t exclude_regs);
void restore_scratch_reg(ScratchRegAlloc *alloc);
void restore_all_pushed_scratch_regs(void);
int ot_check(thumb_opcode op);
void imm_cache_reset_all(void);
void imm_cache_invalidate_reg(int reg);
int try_scratch_reg_for_sym_addr(Sym *raw_sym, int64_t imm, uint32_t exclude_regs, ScratchRegAlloc *out);
void mov_equiv_reset_all(void);
int mov_equiv_it_block_length(thumb_opcode op);
void mov_equiv_invalidate_reg(int reg);
void mov_equiv_record_mov(int rd, int rm);
int ot_check_mov_reg(uint32_t rd, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                            thumb_enforce_encoding enc, bool in_it);
int decode_mov_reg_plain(thumb_opcode op, int *rd_out, int *rm_out);
ST_FUNC void tcc_gen_machine_strldr_cache_reset(void);
void codegen_internal_merge_point(void);
void strldr_cache_invalidate_reg(int reg);
void strldr_cache_invalidate_mem(int rn, int imm, int width);
void strldr_cache_record_access(int rt, int rn, int imm, uint32_t puw, int size, int width, int is_store);
int strldr_cache_try_match_ldr(int rt, int rn, int imm, uint32_t puw, int size, int width);
int strldr_cache_str_is_redundant(int rt, int rn, int imm, uint32_t puw, int size);
int strldr_cache_ldrd_is_redundant(int rt, int rt2, int rn, int imm, uint32_t puw);
int decode_str_ldr_imm(thumb_opcode op, int *is_str_out, int *rt_out, int *rn_out, int *imm_out,
                              uint32_t *puw_out, int *width_out);
int decode_strd_ldrd_imm(thumb_opcode op, int *is_str_out, int *rn_out, int *imm_out);
int ot_check_ldr_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc);
int ot_check_str_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc);
uint32_t mapcc(int cc);
void o(unsigned int i);
void th_literal_pool_generate(void);
int th_pool_span_after(int upcoming_bytes);
void th_literal_pool_note_entry(const ThumbLiteralPoolEntry *entry);
void th_literal_pool_reserve_upcoming_bytes(int upcoming_bytes);
int th_literal_pool_would_flush_for(int upcoming_bytes);
int is_valid_opcode(thumb_opcode op);
int thumb_op_is_plain_branch(thumb_opcode op);
int thumb_op_is_pure_flag_setter(thumb_opcode op);
int thumb_decode_dest_reg(thumb_opcode op);

/* Defined in arm-thumb-emit.c. */
int ot(thumb_opcode op);
thumb_opcode th_generic_mov_imm(uint32_t r, int imm);
ScratchRegAlloc th_offset_to_reg_ex(int off, int sign, uint32_t exclude_regs);
int th_patch_call(int t, int a);
void gadd_sp_ex(int val, int scratch_reg);
void gadd_sp(int val);
int load_word_from_base(int ir, int base, int fc, int sign);
int store_word_to_base(int ir, int base, int fc, int sign);
int sym_is_4_byte_aligned_for_64bit(Sym *sym, int32_t addend);
int try_strd_pair(int lo_reg, int hi_reg, int base, int abs_off, int sign);
int try_ldrd_pair(int lo_reg, int hi_reg, int base, int abs_off, int sign);
ST_FUNC int tcc_gen_machine_try_ldrd_spill(int reg1, int32_t off1, int reg2, int32_t off2);
ST_FUNC int tcc_gen_machine_try_ldrd_base(int reg1, int reg2, int base_reg, int32_t off);
ST_FUNC int tcc_gen_machine_try_strd_base(int reg1, int reg2, int base_reg, int32_t off);
ST_FUNC void tcc_machine_load_spill_slot(int dest_reg, int frame_offset);
ST_FUNC void tcc_machine_store_spill_slot(int src_reg, int frame_offset);
ST_FUNC void tcc_machine_store_param_slot(int src_reg, int frame_offset);
void th_store32_imm_or_reg_ex(int src_reg, uint32_t base_reg, int abs_off, int sign, uint32_t extra_exclude);
void th_store16_imm_or_reg(int src_reg, uint32_t base_reg, int abs_off, int sign);
void th_store8_imm_or_reg(int src_reg, uint32_t base_reg, int abs_off, int sign);
void load_full_const(int r, int r1, uint32_t imm_lo, uint32_t imm_hi);
ST_FUNC void tcc_machine_addr_of_stack_slot(int dest_reg, int frame_offset, int is_param);
ST_FUNC void tcc_machine_load_constant(int dest_reg, int dest_reg_high, int64_t value, int is_64bit, Sym *sym);
void load_from_base(int r, int r1, int irop_btype, int is_unsigned, int fc, int sign, uint32_t base);
thumb_opcode thumb_call_reg_handler(thumb_reg_handler_t fn, uint32_t rd, uint32_t rn, uint32_t rm,
                                           thumb_flags_behaviour flags, thumb_shift shift,
                                           thumb_enforce_encoding encoding);
void thumb_require_materialized_reg(const char *ctx, const char *operand, int reg);
uint32_t thumb_exclude_mask_for_regs(int count, const int *regs);
bool thumb_is_hw_reg(int reg);

/* Defined in arm-thumb-alu.c. */
bool mach_op_64_names_memory(const MachineOperand *op);
MachineOperand mach_resolve_deref_64(MachineCodegenContext *mctx, const MachineOperand *op, uint32_t *excl);
MachineOperand mach_make_lo_half(const MachineOperand *op);
MachineOperand mach_make_hi_half(const MachineOperand *op);
void mach_ensure_pair_in_regs(MachineCodegenContext *ctx, const MachineOperand *op64,
                                     uint32_t *excl, int *out_lo, int *out_hi);
ST_FUNC void tcc_gen_machine_assign_mop(MachineOperand src, MachineOperand dest, TccIrOp op);

/* Defined in arm-thumb-fp.c. */
MachineOperand mach_make_complex_real(const MachineOperand *op);
MachineOperand mach_make_complex_imag(const MachineOperand *op);

/* Defined in arm-thumb-frame.c. */
ST_FUNC void tcc_gen_machine_store_to_stack_ex(int reg, int offset, uint32_t extra_exclude);
void gcall_or_jump_mop(int is_jmp, MachineOperand target);
void load_immediate(int reg, uint32_t imm, Sym *sym, int update_flags);
int load_constant_from_holding_reg(int reg, int64_t key);

/* Defined in arm-thumb-call.c. */
int thumb_callee_needs_local_call_marker(Sym *sym);
Sym *thumb_local_libc_helper(Sym *sym);
int thumb_callee_in_this_module(const MachineOperand *func_mop);
int thumb_callee_noreturn(const MachineOperand *func_mop);

#endif /* ARM_THUMB_GEN_H */
