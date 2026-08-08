/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2026 Mateusz Stadnik
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

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "log.h"

static inline const char *th_reg_name(uint32_t r)
{
  static const char *names[] = {
      "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc",
  };
  static char buf[16];
  if (r < 16)
    return names[r];
  snprintf(buf, sizeof buf, "r%u", r);
  return buf;
}

static inline const char *th_shift_name(int type)
{
  switch (type)
  {
  case 0:
    return "none";
  case 1:
    return "rrx";
  case 2:
    return "lsl";
  case 3:
    return "lsr";
  case 4:
    return "asr";
  case 5:
    return "ror";
  default:
    return "?shift";
  }
}

#if TCC_LOG_THOP
#define THOP_TRACE(...) fprintf(stderr, __VA_ARGS__)
#else
#define THOP_TRACE(...)                                                                                                \
  do                                                                                                                   \
  {                                                                                                                    \
  } while (0)
#endif

#if TCC_LOG_THUMB
#define LOG(...) LOG_THUMB(__VA_ARGS__)
#define TRACE(...) LOG_THUMB(__VA_ARGS__)
#else
#define LOG(...)                                                                                                       \
  do                                                                                                                   \
  {                                                                                                                    \
  } while (0)
#define TRACE(...)                                                                                                     \
  do                                                                                                                   \
  {                                                                                                                    \
  } while (0)
#endif

#define ceil_div(x, d) ((x + (d - 1)) / d)

#define R0 0
#define R1 1
#define R2 2
#define R3 3
#define R4 4
#define R5 5
#define R6 6
#define R7 7
#define R8 8
#define R9 9
#define R10 10
#define R11 11
#define R12 12
#define R_IP R12
#define R_SP 13
#define R_LR 14
#define R_PC 15

#define R_FP R7

typedef enum
{
  FLAGS_BEHAVIOUR_NOT_IMPORTANT = 0,
  FLAGS_BEHAVIOUR_SET = 1,
  FLAGS_BEHAVIOUR_BLOCK = 2,
} thumb_flags_behaviour;

typedef enum
{
  ENFORCE_ENCODING_NONE = 0,
  ENFORCE_ENCODING_16BIT = 1,
  ENFORCE_ENCODING_32BIT = 2,
} thumb_enforce_encoding;

typedef struct thumb_opcode
{
  uint8_t size;
  uint32_t opcode;
} thumb_opcode;

typedef enum thumb_shift_type
{
  THUMB_SHIFT_NONE,
  THUMB_SHIFT_RRX,
  THUMB_SHIFT_LSL,
  THUMB_SHIFT_LSR,
  THUMB_SHIFT_ASR,
  THUMB_SHIFT_ROR,
} thumb_shift_type;

typedef enum thumb_shift_mode
{
  THUMB_SHIFT_IMMEDIATE,
  THUMB_SHIFT_REGISTER,
} thumb_shift_mode;

typedef struct thumb_shift
{
  thumb_shift_type type;
  uint32_t value;
  thumb_shift_mode mode;
} thumb_shift;

static const thumb_shift _thumb_shift_default_val = {THUMB_SHIFT_NONE, 0, THUMB_SHIFT_IMMEDIATE};
#define THUMB_SHIFT_DEFAULT _thumb_shift_default_val

typedef struct
{
  /* ───── implemented now (bits 0-15) ───── */
  uint64_t t16 : 1;       /* 16-bit Thumb-1 (all profiles)      */
  uint64_t t32 : 1;       /* 32-bit Thumb-2 wide encodings      */
  uint64_t it : 1;        /* IT blocks                          */
  uint64_t mod_imm : 1;   /* th_pack_const modified imm         */
  uint64_t movw_movt : 1; /* movw/movt 16-bit imm moves         */
  uint64_t dsp : 1;       /* sel, uadd8, usub8, pkhbt, qadd, …  */
  uint64_t sat : 1;       /* ssat/usat                          */
  uint64_t div : 1;       /* udiv/sdiv                          */
  uint64_t bfx : 1;       /* bfi, bfc, sbfx, ubfx               */
  uint64_t clz_rbit : 1;  /* clz, rbit                          */
  uint64_t ldaex : 1;     /* lda/stl acquire/release (v8)       */
  uint64_t vfp_sp : 1;    /* single-precision FP                */
  uint64_t vfp_dp : 1;    /* double-precision FP                */
  uint64_t tbb_tbh : 1;   /* tbb/tbh table branches             */
  uint64_t cbz : 1;       /* cbz/cbnz                           */
  uint64_t hwdiv_t16 : 1; /* reserved for narrow div forms      */

  /* ───── reserved / future enablers (bits 16-31) ───── */
  uint64_t sec : 1;         /* sg, bxns, blxns                    */
  uint64_t sec_tt : 1;      /* tt, ttt, tta, ttat                 */
  uint64_t lob : 1;         /* low-overhead-branch: wls, dls, le… */
  uint64_t pacbti : 1;      /* pac, aut, pacg, autg, bti          */
  uint64_t cde : 1;         /* custom datapath: cx{1,2,3}, vcx…   */
  uint64_t ras : 1;         /* reliability / esb                  */
  uint64_t fp16 : 1;        /* half-precision FP                  */
  uint64_t fp_armv8 : 1;    /* vrint*, vsel, vmaxnm, vminnm       */
  uint64_t fp_dp_d32 : 1;   /* 32 double registers (d16..d31)     */
  uint64_t mve_int : 1;     /* integer MVE                        */
  uint64_t mve_fp : 1;      /* FP MVE                             */
  uint64_t cache_maint : 1; /* dc, ic cache maintenance forms     */
  uint64_t debug : 1;       /* bkpt variants, hlt, dbg imm        */
  uint64_t coproc : 1;      /* mcr/mrc/mcrr/mrrc/cdp              */
  uint64_t lrcpc : 1;       /* load-acquire RCpc forms            */
  uint64_t unpriv_ls : 1;   /* ldrt/strt family                   */

  /* bits 32-47: architect's playground — reserved without commitment */
  uint64_t reserved_arch : 16;
  /* bits 48-63: vendor / compiler-specific feature flags  */
  uint64_t reserved_vendor : 16;
} thop_feat;

_Static_assert(sizeof(thop_feat) == sizeof(uint64_t), "thop_feat must pack into 64 bits");

typedef struct
{
  uint32_t t16 : 1;
  uint32_t t32 : 1;
  uint32_t it : 1;
  uint32_t mod_imm : 1;
  uint32_t movw_movt : 1;
  uint32_t dsp : 1;
  uint32_t sat : 1;
  uint32_t div : 1;
  uint32_t bfx : 1;
  uint32_t clz_rbit : 1;
  uint32_t ldaex : 1;
  uint32_t vfp_sp : 1;
  uint32_t vfp_dp : 1;
  uint32_t tbb_tbh : 1;
  uint32_t cbz : 1;
  uint32_t hwdiv_t16 : 1;
  uint32_t sec : 1;
  uint32_t sec_tt : 1;
  uint32_t lob : 1;
  uint32_t pacbti : 1;
  uint32_t cde : 1;
  uint32_t ras : 1;
  uint32_t fp16 : 1;
  uint32_t fp_armv8 : 1;
  uint32_t fp_dp_d32 : 1;
  uint32_t mve_int : 1;
  uint32_t mve_fp : 1;
  uint32_t cache_maint : 1;
  uint32_t debug : 1;
  uint32_t coproc : 1;
  uint32_t lrcpc : 1;
  uint32_t unpriv_ls : 1;
} thop_feat32;

_Static_assert(sizeof(thop_feat32) == sizeof(uint32_t), "thop_feat32 must pack into 32 bits");

static inline uint32_t thop_feat32_bits(thop_feat32 f)
{
  uint32_t b;
  memcpy(&b, &f, sizeof b);
  return b;
}

thop_feat thumb_resolve_features(const char *march, const char *mfpu, uint64_t extra_feat_bits);

/* Resolve only the FP-unit feature bits for a -mfpu / .fpu name (no core
   features). Used by the `.fpu` assembler directive. */
thop_feat thumb_resolve_fpu(const char *mfpu);

/* ───── Backend-owned target-dependent config ─────
 *
 * Forward-declared as `struct target_dependent_config` in tcc.h; generic
 * code sees only the pointer.  The full shape (feature mask, TrustZone
 * flag, -mcpu= name) is ARM-private and lives here. */

struct target_dependent_config
{
  const char *mcpu_name;
  thop_feat feat;
  bool is_secure_tz;
};

extern struct target_dependent_config arm_target_dependent;

typedef enum
{
  IMM_NONE,
  IMM_RAW,          /* plain N-bit value, optional scale */
  IMM_PACK_CONST,   /* ARMv7-M modified-immediate (th_pack_const) */
  IMM_PACK_3_8_1,   /* scattered 12-bit (movw/adr)               */
  IMM_PACK_10_11_0, /* branch encoding                            */
  IMM_SIGNED_PUW,   /* load/store with P/U/W bits                 */
} imm_kind;

typedef struct
{
  uint16_t kind : 3;       /* imm_kind (6 values)            */
  uint16_t width : 4;      /* max bits of the *user* value   */
  uint16_t scale_log2 : 2; /* 0=byte, 1=half, 2=word         */
  uint16_t is_signed : 1;
} imm_spec;

_Static_assert(sizeof(imm_spec) == 2, "imm_spec must pack into 16 bits");

typedef enum
{
  REG_ANY = 0,
  REG_LOW_ONLY = 1 << 0, /* R0..R7 */
  REG_NOT_SP = 1 << 1,
  REG_NOT_PC = 1 << 2,
  REG_NOT_LR = 1 << 3,
  REG_EQ_RN = 1 << 4,   /* rd must equal rn (e.g. T1 add_imm) */
  REG_EQ_RM = 1 << 5,   /* rd must equal rm (e.g. T1 add_sp_reg) */
  REG_SP_ONLY = 1 << 6, /* must be SP (r13) */
  REG_PC_ONLY = 1 << 7, /* must be PC (r15) */
  /* ── bitmask-field constraints (applied to rm when used as reglist) ── */
  REG_LOW_REGSET = 1 << 8,         /* only bits [7:0] may be set in rm */
  REG_RM_BIT_NOT_SP = 1 << 9,      /* bit 13 of rm must NOT be set */
  REG_RM_BITS_NOT_LR_PC = 1 << 10, /* bits 14,15 of rm must NOT be set */
} reg_mask;

/* Where an operand lands in the final 16/32-bit word */
typedef struct
{
  uint8_t shift; /* LSB position */
  uint8_t width; /* bit width; 0 = field unused */
} bitfield;

typedef enum thop_variant_size
{
  THOP_VARIANT_NONE = 0,
  THOP_VARIANT_T16 = 2,
  THOP_VARIANT_T32 = 4,
} thop_variant_size;

typedef struct
{
  thop_feat32 feat;

  imm_spec imm;
  bitfield rd_place, rn_place, rm_place, ra_place;
  bitfield imm_place;
  bitfield shift_type_bits; /* e.g. [5:4] in T3 */
  bitfield shift_imm2_bits; /* [7:6]  */
  bitfield shift_imm3_bits; /* [14:12] */
  bitfield imm2_place;
  bitfield split_imm2_place; /* places (a.imm >> 0) & 0x3 */
  bitfield split_imm3_place; /* places (a.imm >> 2) & 0x7 */
  bitfield puw_bits;
  bitfield rm_raw_place; /* place raw rm value at this position (not ARM-encoded) */
  bitfield dn_rd_split;  /* DN:Rd split (T1 high-register MOV) — Rd low bits at shift/width, D computed from rd>>3 */

    uint16_t rd_con, rn_con, rm_con, ra_con;

  uint16_t size : 3;          /* thop_variant_size (0, 2, 4) */
  uint16_t shift_allowed : 6; /* bitmask of THUMB_SHIFT_* */
  uint16_t puw_fixed : 3;     /* when puw_bits.width==0, match only this */
  uint16_t has_s_bit : 1;     /* s_bit always at position 20 when set */
  uint16_t implicit_s : 1;    /* T16 always sets flags */
  uint16_t forbid_s_in_it : 1;
  uint16_t has_rd_hi : 1; /* rd_hi_place always {7, 1} when set */
} thop_variant_shape;

_Static_assert(sizeof(thop_variant_shape) == 44, "thop_variant_shape");

typedef struct thop_args thop_args;
typedef thumb_opcode (*thop_custom_emit)(uint32_t base, const thop_args *a);

typedef struct
{
  const thop_variant_shape *shape;
  uint32_t base;
  thop_custom_emit custom;
} thop_variant;

typedef struct thop_table
{
  const char *name;
  const thop_variant *variants;
  size_t variant_count;
} thop_table;

#define TH_TABLE(id, mnemonic, ...)                                                                                    \
  static const thop_variant id##_VARIANTS[] = {__VA_ARGS__};                                                           \
  static const thop_table id = {                                                                                       \
      .name = mnemonic,                                                                                                \
      .variants = id##_VARIANTS,                                                                                       \
      .variant_count = sizeof(id##_VARIANTS) / sizeof(id##_VARIANTS[0]),                                               \
  }

/* ───── Emit engine ───── */

struct thop_args
{
  uint32_t rd, rn, rm, ra;
  uint32_t imm;
  uint32_t imm2;
  thumb_shift shift;
  thumb_flags_behaviour flags;
  thumb_enforce_encoding enc;
  bool in_it_block;
  uint8_t puw;
  uint8_t exclude_bit; /* clear this bit from rm before rm_raw_place placement */
};

/* ───── Utility declarations (defined in thumb.c) ───── */

uint32_t th_packimm_10_11_0(uint32_t imm);
uint32_t th_packimm_3_8_1(uint32_t imm);

uint32_t th_pack_const(uint32_t imm);
uint32_t th_encbranch_b_t3(uint32_t imm);

uint32_t th_encbranch(int pos, int addr);
uint32_t th_encbranch_8(int pos, int addr);
uint32_t th_encbranch_11(int pos, int addr);
uint32_t th_encbranch_20(int pos, int addr);

void th_sym_t();
void th_sym_d();

uint32_t th_shift_type_to_op(thumb_shift shift);
uint32_t th_shift_value_to_sr_type(thumb_shift shift);

thumb_opcode th_generic_op_reg_shift_with_status(uint32_t op, uint32_t rd, uint32_t rn, uint32_t rm,
                                                 thumb_flags_behaviour setflags, thumb_shift shift);

thumb_opcode thop_emit_error(const char *name, const thop_variant *table, size_t n, thop_args a);

/* Bulk helpers — type-pun through memcpy (defined behaviour, the
   compiler folds it away). Used for profile composition and the
   engine's subset test; single-capability checks use named fields. */
static inline uint64_t thop_feat_bits(thop_feat f)
{
  uint64_t b;
  memcpy(&b, &f, sizeof b);
  return b;
}

static inline thop_feat thop_feat_from_bits(uint64_t b)
{
  thop_feat f;
  memcpy(&f, &b, sizeof f);
  return f;
}

static inline thop_feat thop_feat_or(thop_feat a, thop_feat b)
{
  return thop_feat_from_bits(thop_feat_bits(a) | thop_feat_bits(b));
}

static inline thop_feat thop_feat32_widen(thop_feat32 f32)
{
  return thop_feat_from_bits((uint64_t)thop_feat32_bits(f32));
}

bool thop_feat32_subset(thop_feat32 sub, thop_feat sup);

static inline const char *thop_feat_bit_name(int bit)
{
  static const char *names[] = {
      [0] = "t16",        [1] = "t32",        [2] = "it",           [3] = "mod_imm",   [4] = "movw_movt",
      [5] = "dsp",        [6] = "sat",        [7] = "div",          [8] = "bfx",       [9] = "clz_rbit",
      [10] = "ldaex",     [11] = "vfp_sp",    [12] = "vfp_dp",      [13] = "tbb_tbh",  [14] = "cbz",
      [15] = "hwdiv_t16", [16] = "sec",       [17] = "sec_tt",      [18] = "lob",      [19] = "pacbti",
      [20] = "cde",       [21] = "ras",       [22] = "fp16",        [23] = "fp_armv8", [24] = "fp_dp_d32",
      [25] = "mve_int",   [26] = "mve_fp",    [27] = "cache_maint", [28] = "debug",    [29] = "coproc",
      [30] = "lrcpc",     [31] = "unpriv_ls",
  };
  if (bit >= 0 && bit < (int)(sizeof(names) / sizeof(names[0])) && names[bit])
    return names[bit];
  return "?";
}

static inline const char *thop_feat_hint(int bit)
{
  switch (bit)
  {
  case 11:
    return "enable with -mfpu=fpv4-sp-d16 or -mfpu=fpv5-sp-d16";
  case 12:
    return "enable with -mfpu=fpv5-d16";
  case 22:
    return "enable with -mfpu that supports fp16";
  case 23:
    return "requires ARMv8-M FP extensions";
  default:
    return NULL;
  }
}

static inline void thop_feat_describe_missing(thop_feat need, thop_feat have, char *buf, size_t bufsz)
{
  uint64_t missing = thop_feat_bits(need) & ~thop_feat_bits(have);
  size_t pos = 0;
  for (int i = 0; i < 64 && missing && pos < bufsz - 1; i++)
  {
    if (!(missing & (1ull << i)))
      continue;
    missing &= ~(1ull << i);
    const char *name = thop_feat_bit_name(i);
    const char *hint = thop_feat_hint(i);
    int n;
    if (hint)
      n = snprintf(buf + pos, bufsz - pos, "%s%s (%s)", pos ? ", " : "", name, hint);
    else
      n = snprintf(buf + pos, bufsz - pos, "%s%s", pos ? ", " : "", name);
    if (n > 0)
      pos += (size_t)n;
  }
  if (pos == 0 && bufsz > 0)
    buf[0] = '\0';
}

static inline __attribute__((always_inline)) bool thop_reg_ok(uint32_t reg, reg_mask con)
{
  if ((con & REG_LOW_ONLY) && reg > 7)
    return false;
  if ((con & REG_NOT_SP) && reg == 13)
    return false;
  if ((con & REG_NOT_PC) && reg == 15)
    return false;
  if ((con & REG_NOT_LR) && reg == 14)
    return false;
  if ((con & REG_SP_ONLY) && reg != 13)
    return false;
  if ((con & REG_PC_ONLY) && reg != 15)
    return false;
  return true;
}

static inline __attribute__((always_inline)) uint32_t thop_place(uint32_t val, bitfield bf)
{
  if (bf.width == 0)
    return 0;
  return (val & ((1u << bf.width) - 1)) << bf.shift;
}

static inline __attribute__((always_inline)) bool thop_try_imm(const thop_variant_shape *s, uint32_t imm,
                                                               uint32_t *out_bits)
{
  const imm_spec *spec = &s->imm;
  *out_bits = 0;

  if (spec->kind == IMM_NONE)
    return imm == 0;

  uint32_t scaled = imm;

  if (spec->is_signed)
  {
    int32_t simm = (int32_t)scaled;
    if (simm >= 0)
      return false;
    scaled = (uint32_t)(-simm);
  }

  if (spec->scale_log2 > 0)
  {
    uint32_t mask = (1u << spec->scale_log2) - 1;
    if (scaled & mask)
      return false;
    scaled >>= spec->scale_log2;
  }

  switch (spec->kind)
  {
  case IMM_RAW:
    if (scaled >= (1u << spec->width))
      return false;
    *out_bits = thop_place(scaled, s->imm_place);
    return true;

  case IMM_PACK_CONST:
  {
    uint32_t packed = th_pack_const(imm);
    if (!packed && imm != 0)
      return false;
    *out_bits = packed;
    return true;
  }

  case IMM_PACK_3_8_1:
    if (spec->width ? (scaled >= (1u << spec->width)) : (scaled > 0xFFFF))
      return false;
    *out_bits = th_packimm_3_8_1(scaled);
    return true;

  case IMM_PACK_10_11_0:
    *out_bits = th_packimm_10_11_0(imm);
    return true;

  default:
    return false;
  }
}

thumb_opcode thop_emit(const char *name, const thop_variant *table, size_t n, thop_args a);
