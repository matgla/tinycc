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

#define USING_GLOBALS
#include "thumb.h"
#include "tcc.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Thumb feature profiles, extensions, and FPU bundles
 * ═══════════════════════════════════════════════════════════════════ */

/* ───── Profile definitions ───── */

static const thop_feat THOP_PROFILE_ARMV6M_CORE = {.t16 = 1};

static const thop_feat THOP_PROFILE_ARMV7M_CORE = {.t16 = 1,
                                                   .t32 = 1,
                                                   .it = 1,
                                                   .mod_imm = 1,
                                                   .movw_movt = 1,
                                                   .bfx = 1,
                                                   .clz_rbit = 1,
                                                   .tbb_tbh = 1,
                                                   .cbz = 1,
                                                   .sat = 1,
                                                   .div = 1};

static const thop_feat THOP_PROFILE_ARMV7EM_CORE = {.t16 = 1,
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
                                                    .dsp = 1};

static const thop_feat THOP_PROFILE_ARMV8M_BASE_CORE = {.t16 = 1, .movw_movt = 1, .cbz = 1, .ldaex = 1};

static const thop_feat THOP_PROFILE_ARMV8M_MAIN_CORE = {.t16 = 1,
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
                                                        .fp_armv8 = 1};

static const thop_feat THOP_PROFILE_ARMV81M_MAIN_CORE = {.t16 = 1,
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
                                                         .fp_armv8 = 1,
                                                         .lob = 1};

/* ───── Optional extension bundles ───── */

// static const thop_feat THOP_EXT_CMSE = {.sec = 1, .sec_tt = 1};
// static const thop_feat THOP_EXT_PACBTI = {.pacbti = 1};
// static const thop_feat THOP_EXT_CDE = {.cde = 1};
// static const thop_feat THOP_EXT_MVE_INT = {.mve_int = 1};
// static const thop_feat THOP_EXT_MVE_FP = {.mve_int = 1, .mve_fp = 1, .fp16 = 1};

/* ───── FPU bundles ───── */

static const thop_feat THOP_FPU_NONE = {0};
static const thop_feat THOP_FPU_VFPV4_SP_D16 = {.vfp_sp = 1};
static const thop_feat THOP_FPU_FPV5_SP_D16 = {.vfp_sp = 1, .fp_armv8 = 1};
static const thop_feat THOP_FPU_FPV5_D16 = {.vfp_sp = 1, .vfp_dp = 1, .fp_armv8 = 1};
static const thop_feat THOP_FPU_FPV5_D32 = {.vfp_sp = 1, .vfp_dp = 1, .fp_armv8 = 1, .fp_dp_d32 = 1};
static const thop_feat THOP_FPU_FP_ARMV8_FULL = {.vfp_sp = 1, .vfp_dp = 1, .fp_armv8 = 1, .fp_dp_d32 = 1, .fp16 = 1};

/* ───── Resolve helpers ───── */

static thop_feat thop_feats_from_march(const char *s)
{
  if (!s)
    return THOP_PROFILE_ARMV8M_MAIN_CORE;

  const char *plus = strchr(s, '+');
  size_t base_len = plus ? (size_t)(plus - s) : strlen(s);

  static const struct {
    const char *name;
    const thop_feat *feat;
  } archs[] = {
      {"armv6-m", &THOP_PROFILE_ARMV6M_CORE},
      {"armv7-m", &THOP_PROFILE_ARMV7M_CORE},
      {"armv7e-m", &THOP_PROFILE_ARMV7EM_CORE},
      {"armv8-m.base", &THOP_PROFILE_ARMV8M_BASE_CORE},
      {"armv8-m.main", &THOP_PROFILE_ARMV8M_MAIN_CORE},
      {"armv8.1-m.main", &THOP_PROFILE_ARMV81M_MAIN_CORE},
  };

  thop_feat feat = {0};
  bool found = false;
  for (size_t i = 0; i < sizeof(archs) / sizeof(archs[0]); i++) {
    if (strlen(archs[i].name) == base_len && !strncmp(s, archs[i].name, base_len)) {
      feat = *archs[i].feat;
      found = true;
      break;
    }
  }
  if (!found) {
    tcc_error("unknown -march=%s", s);
    return feat;
  }

  while (plus && *plus == '+') {
    const char *ext = plus + 1;
    const char *next = strchr(ext, '+');
    size_t ext_len = next ? (size_t)(next - ext) : strlen(ext);

    if (ext_len == 3 && !strncmp(ext, "dsp", 3))
      feat.dsp = 1;
    else if (ext_len == 3 && !strncmp(ext, "fpu", 3))
      feat.vfp_sp = 1;
    else if (ext_len == 2 && !strncmp(ext, "fp", 2))
      feat.vfp_sp = 1;
    else if (ext_len == 5 && !strncmp(ext, "fp.dp", 5)) {
      feat.vfp_sp = 1;
      feat.vfp_dp = 1;
    } else if (ext_len == 3 && !strncmp(ext, "mve", 3))
      feat.mve_int = 1;
    else if (ext_len == 6 && !strncmp(ext, "mve.fp", 6)) {
      feat.mve_int = 1;
      feat.mve_fp = 1;
    } else if (ext_len == 6 && !strncmp(ext, "pacbti", 6))
      feat.pacbti = 1;
    else if (ext_len == 3 && !strncmp(ext, "sec", 3))
      feat.sec = 1;
    else if (ext_len == 3 && !strncmp(ext, "lob", 3))
      feat.lob = 1;
    else
      tcc_warning("ignoring unknown -march extension '+%.*s'", (int)ext_len, ext);

    plus = next;
  }

  return feat;
}

static thop_feat thop_feats_from_mfpu(const char *s)
{
  if (!s || !strcmp(s, "none"))
    return THOP_FPU_NONE;
  if (!strcmp(s, "vfpv4-sp-d16") || !strcmp(s, "fpv4-sp-d16"))
    return THOP_FPU_VFPV4_SP_D16;
  if (!strcmp(s, "fpv5-sp-d16"))
    return THOP_FPU_FPV5_SP_D16;
  if (!strcmp(s, "fpv5-d16"))
    return THOP_FPU_FPV5_D16;
  if (!strcmp(s, "fpv5-d32"))
    return THOP_FPU_FPV5_D32;
  if (!strcmp(s, "fp-armv8-full"))
    return THOP_FPU_FP_ARMV8_FULL;
  tcc_error("unknown -mfpu=%s", s);
  return THOP_FPU_NONE;
}

thop_feat thumb_resolve_features(const char *march, const char *mfpu, uint64_t extra_feat_bits)
{
  thop_feat feat = thop_feats_from_march(march);
  feat = thop_feat_or(feat, thop_feat_from_bits(extra_feat_bits));
  if (mfpu)
    feat = thop_feat_or(feat, thop_feats_from_mfpu(mfpu));

  if (feat.mve_fp && !feat.vfp_sp)
    tcc_error("-mextension=mve.fp requires an FP unit (-mfpu=…)");
  if ((feat.sec || feat.sec_tt) && !(feat.t32 || feat.movw_movt))
    tcc_error("-mcmse requires a mainline or v8-M baseline profile");

  return feat;
}

/* Resolve only the FP-unit feature bits for a given -mfpu / .fpu name.
   Unlike thumb_resolve_features(), this does not fold in any core/profile
   features, so callers can OR the result into an already-resolved target
   feature set.  Used by the assembler's `.fpu` directive.  Errors on an
   unknown name. */
thop_feat thumb_resolve_fpu(const char *mfpu)
{
  return thop_feats_from_mfpu(mfpu);
}

/* ═══════════════════════════════════════════════════════════════════
 *  thop_emit — generic Thumb instruction encoding engine
 *
 *  Walks variants narrow→wide, returns the first whose constraints
 *  all pass.  Returns {.size=0} if no variant matches.
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode thop_emit_error(const char *name, const thop_variant *table, size_t n, thop_args a)
{
  const thop_feat target_feat = arm_target_dependent.feat;

  bool has_feat_mismatch = false;
  for (size_t i = 0; i < n; i++) {
    if (!thop_feat32_subset(table[i].shape->feat, target_feat)) {
      has_feat_mismatch = true;
      break;
    }
  }

  if (has_feat_mismatch) {
    fprintf(stderr, "thop_emit: '%s': no variant matched (%zu candidates)\n", name, n);
    for (size_t i = 0; i < n; i++) {
      const thop_variant_shape *s = table[i].shape;
      if (!thop_feat32_subset(s->feat, target_feat)) {
        char missing[256];
        thop_feat_describe_missing(thop_feat32_widen(s->feat), target_feat, missing, sizeof missing);
        fprintf(stderr, "  T%d: missing features: %s\n", (int)i + 1, missing);
      }
    }
  }

  THOP_TRACE("thop_emit: no variant matched (%zu candidates)\n", n);
  THOP_TRACE("  args: rd=%s rn=%s rm=%s ra=%s imm=0x%x imm2=0x%x\n",
             th_reg_name(a.rd), th_reg_name(a.rn),
             th_reg_name(a.rm), th_reg_name(a.ra),
             (unsigned)a.imm, (unsigned)a.imm2);
  THOP_TRACE("  flags=%d enc=%d shift=%s #%u puw=%u in_it=%d\n",
             a.flags, a.enc, th_shift_name(a.shift.type),
             (unsigned)a.shift.value, (unsigned)a.puw, a.in_it_block);

  for (size_t i = 0; i < n; i++)
  {
    const thop_variant *v = &table[i];
    const thop_variant_shape *s = v->shape;

    THOP_TRACE("  T%d (base=0x%x, %s): REJECT ",
               (int)i + 1, v->base, s->size == THOP_VARIANT_T16 ? "T16" : "T32");

    if (!thop_feat32_subset(s->feat, target_feat)) {
      THOP_TRACE("target features mismatch\n");
      continue;
    }

    if (a.enc == ENFORCE_ENCODING_16BIT && s->size != THOP_VARIANT_T16)
      THOP_TRACE("encoding forced T16 but variant is T32\n");
    else if (a.enc == ENFORCE_ENCODING_32BIT && s->size != THOP_VARIANT_T32)
      THOP_TRACE("encoding forced T32 but variant is T16\n");
    else if ((s->rd_place.width || s->rd_con) && !thop_reg_ok(a.rd, s->rd_con))
      THOP_TRACE("%s (%s) constraint failed\n", "rd", th_reg_name(a.rd));
    else if ((s->rn_place.width || s->rn_con) && !thop_reg_ok(a.rn, s->rn_con))
      THOP_TRACE("%s (%s) constraint failed\n", "rn", th_reg_name(a.rn));
    else if ((s->rm_place.width || s->rm_con) && !thop_reg_ok(a.rm, s->rm_con))
      THOP_TRACE("%s (%s) constraint failed\n", "rm", th_reg_name(a.rm));
    else if ((s->ra_place.width || s->ra_con) && !thop_reg_ok(a.ra, s->ra_con))
      THOP_TRACE("%s (%s) constraint failed\n", "ra", th_reg_name(a.ra));
    else if ((s->rd_con & REG_EQ_RN) && a.rd != a.rn)
      THOP_TRACE("%s (%s) must equal %s (%s)\n", "rd", th_reg_name(a.rd), "rn", th_reg_name(a.rn));
    else if ((s->rd_con & REG_EQ_RM) && a.rd != a.rm)
      THOP_TRACE("%s (%s) must equal %s (%s)\n", "rd", th_reg_name(a.rd), "rm", th_reg_name(a.rm));
    else if (a.flags == FLAGS_BEHAVIOUR_SET && !s->has_s_bit && !s->implicit_s)
      THOP_TRACE("needs S flag but variant has no s_bit\n");
    else if (s->forbid_s_in_it && a.in_it_block && a.flags == FLAGS_BEHAVIOUR_SET)
      THOP_TRACE("S flag forbidden inside IT block\n");
    else if (s->implicit_s && a.in_it_block)
      THOP_TRACE("implicit_s variant forbidden inside IT block\n");
    else if (a.shift.type != THUMB_SHIFT_NONE && (!s->shift_type_bits.width && !s->shift_imm2_bits.width && !s->shift_imm3_bits.width && s->shift_allowed == 0))
      THOP_TRACE("shift requested but variant has no shift fields\n");
    else if (a.shift.type != THUMB_SHIFT_NONE && s->shift_allowed != 0 && !(s->shift_allowed & (1u << a.shift.type)))
      THOP_TRACE("shift type not in allowed mask\n");
    else if (s->puw_bits.width == 0 && s->puw_fixed != 0 && a.puw != s->puw_fixed)
      THOP_TRACE("puw mismatch\n");
    else if (s->imm.kind != IMM_NONE) {
      uint32_t tmp;
      if (!thop_try_imm(s, a.imm, &tmp))
        THOP_TRACE("immediate doesn't fit encoding\n");
      else
        THOP_TRACE("unknown immediate mismatch\n");
    }
    else if (v->custom)
      THOP_TRACE("custom emitter returned 0\n");
    else
      THOP_TRACE("unknown\n");
  }

  return (thumb_opcode){.size = 0, .opcode = 0};
}

/* ═══════════════════════════════════════════════════════════════════
 *  Utility functions (moved from arm-thumb-opcodes.c)
 * ═══════════════════════════════════════════════════════════════════ */

uint32_t th_packimm_10_11_0(uint32_t imm)
{
  const uint32_t imm11 = (imm >> 1) & 0x7ff;
  const uint32_t imm10 = (imm >> 12) & 0x3ff;
  const uint32_t s = (imm >> 24) & 1;
  const uint32_t j1 = ~((imm >> 23) ^ s) & 1;
  const uint32_t j2 = ~((imm >> 22) ^ s) & 1;
  return (s << 26) | (imm10 << 16) | (j1 << 13) | (j2 << 11) | imm11;
}

uint32_t th_packimm_3_8_1(uint32_t imm)
{
  const uint32_t imm8 = imm & 0xff;
  const uint32_t imm3 = (imm >> 8) & 0x7;
  const uint32_t i = (imm >> 11) & 1;
  const uint32_t imm4 = (imm >> 12) & 0xf;
  return (i << 26) | (imm4 << 16) | (imm3 << 12) | imm8;
}

typedef struct ThPackConstCacheEntry
{
  uint32_t imm;
  uint32_t packed;
  uint8_t valid;
} ThPackConstCacheEntry;

#define TH_PACK_CONST_CACHE_SIZE 64 /* YASOS: 256 -> 64 saves ~2.3 KiB .bss; pure
                                       perf cache (miss => recompute const pack). */
static ThPackConstCacheEntry th_pack_const_cache[TH_PACK_CONST_CACHE_SIZE];

uint32_t th_pack_const(uint32_t imm)
{
  const uint32_t idx = (imm ^ (imm >> 9) ^ (imm >> 17) ^ (imm >> 25)) & (TH_PACK_CONST_CACHE_SIZE - 1);
  ThPackConstCacheEntry *cache = &th_pack_const_cache[idx];
  uint32_t packed;

  if (cache->valid && cache->imm == imm)
    return cache->packed;

  // 00000000 00000000 00000000 abcdefgh
  if ((imm & 0xffffff00) == 0)
  {
    packed = imm;
  }
  // 00000000 abcdefgh 00000000 abcdefgh
  else if (!(imm & 0xff00ff00) && (imm >> 16) == (imm & 0xff))
  {
    packed = (1 << 12) | (imm & 0xff);
  }
  // abcdefgh 00000000 abcdefgh 00000000
  else if (!(imm & 0x00ff00ff) && ((imm >> 16) & 0xff00) == (imm & 0xff00))
  {
    packed = (2 << 12) | ((imm >> 8) & 0xff);
  }
  // abcdefgh abcdefgh abcdefgh abcdefgh
  else if ((imm & 0xffff) == ((imm >> 16) & 0xffff) && ((imm >> 8) & 0xff) == (imm & 0xff))
  {
    packed = (3 << 12) | (imm & 0xff);
  }
  else
  {
    packed = 0;
    for (uint32_t i = 8, j = 0; i <= 0x1F; i++, j++)
    {
      uint32_t mask = 0xFF000000 >> j;
      uint32_t one = 0x80000000 >> j;

      if ((imm & one) == one && (imm & ~mask) == 0)
      {
        uint32_t _i = i >> 4;
        uint32_t imm3 = (i >> 1) & 7;
        uint32_t a = i & 1;
        uint32_t bcdefgh = (imm >> (24 - j)) & 0x7f;

        packed = (_i << 26) | (imm3 << 12) | (a << 7) | bcdefgh;
        break;
      }
    }
  }
  cache->imm = imm;
  cache->packed = packed;
  cache->valid = 1;
  return packed;
}

uint32_t th_encbranch_b_t3(uint32_t imm)
{
  const uint32_t s = (imm >> 19) & 1;
  const uint32_t imm6 = (imm >> 11) & 0x3f;
  const uint32_t imm11 = imm & 0x7ff;
  const uint32_t j2 = (imm >> 18) & 1;
  const uint32_t j1 = (imm >> 17) & 1;
  const uint32_t a = (s << 10) | imm6;
  const uint32_t b = (j1 << 13) | (j2 << 11) | imm11;
  return (a << 16) | b;
}

uint32_t th_encbranch(int pos, int addr)
{
  TRACE("th_encbranch pos: 0x%x, addr: 0x%x", pos, addr);
  return addr - pos - 4;
}

uint32_t th_encbranch_8(int pos, int addr)
{
  addr = (addr - pos - 4) >> 1;
  if (addr > 127 || addr < -128)
  {
    tcc_error("compiler_error: th_encbranch_8 too far address: %i\n", addr);
    return 0;
  }
  return addr & 0xff;
}

uint32_t th_encbranch_11(int pos, int addr)
{
  addr = (addr - pos - 4) >> 1;
  if (addr >= 1023 || addr < -1024)
  {
    tcc_error("compiler_error: th_encbranch_11 too far address: %i\n", addr);
    return 0;
  }
  return addr & 0x7ff;
}

uint32_t th_encbranch_20(int pos, int addr)
{
  addr = (addr - pos - 4) >> 1;
  TRACE("th_encbranch_20 pos %x addr %x\n", pos, addr);
  return addr;
}

uint32_t th_shift_type_to_op(thumb_shift shift)
{
  switch (shift.type)
  {
  case THUMB_SHIFT_ASR:
    return 4;
  case THUMB_SHIFT_LSL:
    return 2;
  case THUMB_SHIFT_LSR:
    return 3;
  case THUMB_SHIFT_ROR:
    return 7;
  default:
    tcc_error("compiler_error: 'th_shift_type_to_op', unknown shift type %d\n", shift.type);
    return 0;
  }
}

uint32_t th_shift_value_to_sr_type(thumb_shift shift)
{
  switch (shift.type)
  {
  case THUMB_SHIFT_NONE:
  case THUMB_SHIFT_LSL:
    return 0;
  case THUMB_SHIFT_LSR:
    return 1;
  case THUMB_SHIFT_ASR:
    return 2;
  case THUMB_SHIFT_ROR:
  case THUMB_SHIFT_RRX:
    return 3;
  };
  return 0;
}

thumb_opcode th_generic_op_reg_shift_with_status(uint32_t op, uint32_t rd, uint32_t rn, uint32_t rm,
                                                 thumb_flags_behaviour flags, thumb_shift shift)
{
  int s = 0;
  const int sr = th_shift_value_to_sr_type(shift);
  const int imm2 = shift.value & 0x3;
  const int imm3 = (shift.value >> 2) & 0x7;
  if (flags == FLAGS_BEHAVIOUR_SET)
    s = 1;

  /* Guard against invalid register values (e.g., -1 or PREG_SPILLED) */
  if (rd > 15 || rn > 15 || rm > 15)
  {
    tcc_error("compiler_error: 'th_generic_op_reg_shift_with_status' invalid register: rd=%d, rn=%d, rm=%d (op=0x%x)\n",
              rd, rn, rm, op);
  }

  return (thumb_opcode){
      .size = 4,
      .opcode = (op << 16) | (rn << 16) | (rd << 8) | rm | (sr << 4) | (imm2 << 6) | (imm3 << 12) | (s << 20),
  };
}

// Thumb ELF management
// Start of T32 instructions
void th_sym_t()
{
  const int info = ELFW(ST_INFO)(STB_LOCAL, STT_NOTYPE);
  set_elf_sym(symtab_section, ind, 0, info, 0, 1, "$t");
}

// Start of data
void th_sym_d()
{
  const int info = ELFW(ST_INFO)(STB_LOCAL, STT_NOTYPE);
  set_elf_sym(symtab_section, ind, 0, info, 0, 1, "$d");
}
