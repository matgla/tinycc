# Generic Thumb-2 Opcode Generator — Design Proposal

Target: [arm-thumb-opcodes.c](arm-thumb-opcodes.c) (3978 lines) and its header [arm-thumb-opcodes.h](arm-thumb-opcodes.h).

## Goal

Replace the ~100 hand-written `th_*` opcode builders with a small set of **declarative encoding descriptors** plus one engine (`thop_emit`) that picks the narrowest variant whose constraints all pass. Per-mnemonic "generators" collapse to short `static const` tables.

Constraints to model declaratively:
- `imm_packed const` — reuse `th_pack_const`, `th_packimm_3_8_1`, `th_packimm_10_11_0`.
- Register role constraints — allowed / disallowed registers (low-only, not SP, not PC, rd==rn, …).
- Immediate verification — bit width, signedness, scale/alignment, pack-success.
- Encoding enforcement — `ENFORCE_ENCODING_16BIT` / `_32BIT` / `_NONE`.
- Shifts — which `THUMB_SHIFT_*` types are legal, where the bits land.
- Flags — position of the S bit, whether `FLAGS_BEHAVIOUR_SET` is legal inside an IT block.

## 1. Primitive building blocks

```c
/* Immediate kinds — how a raw imm becomes field bits */
typedef enum {
    IMM_NONE,
    IMM_RAW,           /* plain N-bit value, optional scale */
    IMM_PACK_CONST,    /* ARMv7-M modified-immediate (th_pack_const) */
    IMM_PACK_3_8_1,    /* scattered 12-bit (movw/adr)               */
    IMM_PACK_10_11_0,  /* branch encoding                            */
    IMM_SIGNED_PUW,    /* load/store with P/U/W bits                 */
} imm_kind;

typedef struct {
    imm_kind kind;
    uint8_t  width;       /* max bits of the *user* value  */
    uint8_t  scale_log2;  /* 0=byte, 1=half, 2=word        */
    bool     is_signed;
} imm_spec;

/* Register role constraints — OR-able */
typedef enum {
    REG_ANY      = 0,
    REG_LOW_ONLY = 1 << 0,  /* R0..R7 */
    REG_NOT_SP   = 1 << 1,
    REG_NOT_PC   = 1 << 2,
    REG_NOT_LR   = 1 << 3,
    REG_EQ_RN    = 1 << 4,  /* rd must equal rn (e.g. T1 add_imm) */
} reg_mask;

/* Where an operand lands in the final 16/32-bit word */
typedef struct {
    uint8_t shift;   /* LSB position */
    uint8_t width;   /* bit width; 0 = field unused */
} bitfield;
```

## 2. The variant descriptor

```c
typedef struct {
    uint8_t  size;                 /* 16 or 32 */
    uint32_t base;                 /* fixed bits of the encoding */
    thumb_enforce_encoding req;    /* 16BIT / 32BIT / ANY */

    /* Operand placement — any can be {0,0} meaning unused */
    bitfield rd, rn, rm, ra;

    /* Per-operand role constraints */
    reg_mask rd_con, rn_con, rm_con, ra_con;

    /* Immediate */
    imm_spec imm;
    /* For IMM_PACK_* the packer already places bits; imm_place={0,0}
       means "OR the whole packed word". */
    bitfield imm_place;

    /* Shift (register-register forms) */
    uint8_t  shift_allowed;        /* bitmask of THUMB_SHIFT_* */
    bitfield shift_type_bits;      /* e.g. [5:4] in T3 */
    bitfield shift_imm2_bits;      /* [7:6]  */
    bitfield shift_imm3_bits;      /* [14:12] */

    /* Flags */
    bitfield s_bit;                /* width=0 if no S bit */
    bool     forbid_s_in_it;

    /* Load/store PUW */
    bitfield puw_bits;
    uint8_t  puw_fixed;            /* when puw_bits.width==0, match only this */
} thop_variant;
```

## 3. Mnemonic generator = short table + wrapper

```c
/* Ordered narrow → wide; thop_emit returns the first success. */
static const thop_variant TH_ADD_IMM[] = {
    /* T1: adds rd, rn, #imm3 — rd/rn low, imm<=7 */
    { .size=16, .base=0x1c00, .req=ENFORCE_ENCODING_NONE,
      .rd={0,3}, .rn={3,3},
      .rd_con=REG_LOW_ONLY, .rn_con=REG_LOW_ONLY,
      .imm={IMM_RAW,3,0,0}, .imm_place={6,3},
      .forbid_s_in_it=true },

    /* T2: adds rd, #imm8 (rd==rn), rd low */
    { .size=16, .base=0x3000, .req=ENFORCE_ENCODING_NONE,
      .rd={8,3}, .rd_con=REG_LOW_ONLY|REG_EQ_RN,
      .imm={IMM_RAW,8,0,0}, .imm_place={0,8},
      .forbid_s_in_it=true },

    /* T3: add{s}.w rd, rn, #<const>  — modified imm */
    { .size=32, .base=0xf1000000, .req=ENFORCE_ENCODING_NONE,
      .rd={8,4}, .rn={16,4},
      .rd_con=REG_NOT_PC, .rn_con=REG_NOT_PC,
      .imm={IMM_PACK_CONST,32,0,0},
      .s_bit={20,1} },

    /* T4: addw rd, rn, #imm12 — no S flag */
    { .size=32, .base=0xf2000000, .req=ENFORCE_ENCODING_NONE,
      .rd={8,4}, .rn={16,4},
      .rd_con=REG_NOT_PC,
      .imm={IMM_PACK_3_8_1,12,0,0} },
};

thumb_opcode th_add_imm(uint32_t rd, uint32_t rn, uint32_t imm,
                        thumb_flags_behaviour flags,
                        thumb_enforce_encoding enc)
{
    return thop_emit(TH_ADD_IMM, ARRAY_LEN(TH_ADD_IMM),
                     (thop_args){ .rd=rd, .rn=rn, .imm=imm,
                                  .flags=flags, .enc=enc });
}
```

`thop_emit` walks the table, for each variant checks (in order):

1. `enc` vs `variant.req`.
2. Register role constraints (`REG_LOW_ONLY`, `REG_NOT_PC`, `REG_EQ_RN`, …).
3. Shift legality (`shift.type` in `shift_allowed`).
4. Flag legality (`FLAGS_BEHAVIOUR_SET` but `s_bit.width==0` → skip; `forbid_s_in_it` + in-IT → skip).
5. Immediate fit — bit width, sign, scale mask; for `IMM_PACK_CONST` call `th_pack_const`, skip on 0.

First variant that passes produces the bits; the rest are skipped. Emits `THOP_TRACE` per variant for debugging.

## 4. Merge candidates (direct wins)

From the current file, these groups collapse to **one descriptor table each**, differing only in `base` and a couple of per-variant constraints. A macro like `DEFINE_ALU_IMM(add, 0xf1000000, 0xf2000000, ...)` can expand them:

| Group | Members | Differentiator |
|---|---|---|
| **ALU imm** | `add_imm`, `sub_imm`, `adc_imm`, `sbc_imm`, `rsb_imm`, `orr_imm`, `and_imm`, `bic_imm`, `eor_imm`, `orn_imm` | T3 `base`; T4 only for add/sub |
| **ALU reg-reg (shifted)** | `add_reg`, `sub_reg`, `adc_reg`, `sbc_reg`, `rsb_reg`, `orr_reg`, `and_reg`, `bic_reg`, `eor_reg`, `orn_reg` | T1 base for low-reg form, T3 base |
| **Compare/test** | `cmp_imm/reg`, `cmn_imm/reg`, `tst_imm/reg`, `teq` | Force `rd=0xf`, `s_bit=SET`, no writeback |
| **Shift imm** | `lsl_imm`, `lsr_imm`, `asr_imm`, `ror_imm` | Shift-type field only |
| **Shift reg** | `lsl_reg`, `lsr_reg`, `asr_reg`, `ror_reg` | Shift-type field |
| **Load imm (byte/half/word, signed/unsigned)** | `ldr_imm`, `ldrb_imm`, `ldrh_imm`, `ldrsb_imm`, `ldrsh_imm` | Size/sign bits in `base`, `scale_log2`, T1 legality |
| **Store imm** | `str_imm`, `strb_imm`, `strh_imm` | Same as above |
| **Load/store reg (shifted)** | `ldr_reg`, `ldrb_reg`, `ldrh_reg`, `ldrsb_reg`, `ldrsh_reg`, `str_reg`, `strb_reg`, `strh_reg` | Shift restricted to LSL; base bits |
| **Unprivileged forms** | `ldrt`, `ldrbt`, `ldrht`, `ldrsbt`, `ldrsht`, `strt`, `strbt`, `strht` | Same skeleton, `puw_fixed` |
| **Exclusive/acquire** | `lda`, `ldab`, `ldah`, `stl`, `stlb`, `stlh` | Identical shape, base bits only |
| **Extend** | `sxtb`, `sxth`, `uxtb`, `uxth` | Rotation imm + base |
| **Rev family** | `rev`, `rev16`, `revsh`, `rbit` | Two regs, base only |
| **Bitfield** | `bfi`, `bfc`, `sbfx`, `ubfx`, `ssat`, `usat` | lsb/width placement is identical |

Rough impact: ~40 of the ~100 builders become a descriptor plus a 1-line wrapper. Likely >1500 lines of structural code removed.

## 5. Things to keep separate (do **not** force-merge)

- **Branch family** (`b_t1..t4`, `bl`, `cbz`, `tbb`) — reach/split logic is branch-specific. Its own small engine around `IMM_PACK_10_11_0`.
- **VFP** (`vadd`, `vsub`, `vldr`, …) — different register file with `D:Vd` split across two fields. A separate VFP descriptor with a `vreg` bitfield that knows about the split; don't overload `bitfield` with it.
- **`ldm`/`stm`/`push`/`pop`** — register-set bitmask semantics don't fit `rd/rn/rm`. Keep bespoke.
- **`it`, `mrs`, `msr`, `cps`, `svc`, `bkpt`** — truly unique, not worth descriptor overhead.

## 6. Why this shape specifically

- `thop_emit` is the **single** place that knows 16-vs-32 fallback, so `ENFORCE_ENCODING_*` logic stops being re-implemented per mnemonic.
- `th_pack_const` / `th_packimm_*` plug in via `imm_kind`, so their "returns 0 on failure → fallback" protocol is centralized.
- Register constraints are flag sets, composable: "not SP, not PC" is `REG_NOT_SP|REG_NOT_PC`, not a per-function `if` chain.
- Descriptors are `static const`, so the compiler can inline/const-fold; there is no runtime dispatch cost beyond walking ~2–4 entries.

## 7. Rollout plan

1. Land `thop_variant` + `thop_emit` next to the existing `th_generic_op_imm_with_status` in [arm-thumb-opcodes.c](arm-thumb-opcodes.c).
2. Port **ALU imm** group first — it already uses `th_generic_op_imm_with_status`, so risk is low and the asm tests under [tests/thumb/armv8m/](tests/thumb/armv8m/) will catch regressions byte-for-byte.
3. Port **ALU reg** next (same shape).
4. Do **load/store** in one batch — biggest single source of near-duplicated code.
5. Leave branches, VFP, `ldm`/`stm` for later (or forever).

## 8. Existing helpers that plug in unchanged

From the current file, these can be called directly from `thop_emit` based on `imm_kind`:

- `th_pack_const` ([arm-thumb-opcodes.c:137](arm-thumb-opcodes.c#L137)) — for `IMM_PACK_CONST`.
- `th_packimm_3_8_1` ([arm-thumb-opcodes.c:119](arm-thumb-opcodes.c#L119)) — for `IMM_PACK_3_8_1`.
- `th_packimm_10_11_0` ([arm-thumb-opcodes.c:109](arm-thumb-opcodes.c#L109)) — for branch family.
- `th_shift_value_to_sr_type` — for the shift-type bitfield.

The helpers `th_generic_op_imm_with_status` and `th_generic_op_reg_shift_with_status` are effectively mini-engines for a single 32-bit variant; the new engine subsumes them.

## 9. Per-architecture / feature gating

The existing code uses scattered `#ifndef TCC_TARGET_ARM_ARCHV6M` guards. We can model this declaratively with a **feature mask** per variant — orthogonal to profile names, because most availability differences are really *extension* differences (DSP, IT block, modified-immediate, `movw`, FPU) rather than strict v6-M/v7-M/v8-M boundaries.

### 9.1 Feature bits

`thop_feat` is a 64-bit capability word so we don't run out as ARM keeps adding extensions. Bits 32–63 are currently **reserved / not-yet-emitted**, but defined up front so descriptor tables and `-march=`/`-mfpu=`/`-mcmse`/`-mextension=` flags can be annotated now and the emitter filled in later without a format break.

It is declared as a **plain struct of 1-bit fields**: single-capability queries use named fields (`f.div`), and bulk operations (profile-OR, subset test) go through small memcpy-based helpers. No `THOP_F_*` mask macros, no union view — pure bitfield access:

```c
typedef struct {
    /* ───── implemented now (bits 0-15) ───── */
    uint64_t t16         : 1;  /* 16-bit Thumb-1 (all profiles)      */
    uint64_t t32         : 1;  /* 32-bit Thumb-2 wide encodings      */
    uint64_t it          : 1;  /* IT blocks                          */
    uint64_t mod_imm     : 1;  /* th_pack_const modified imm         */
    uint64_t movw_movt   : 1;  /* movw/movt 16-bit imm moves         */
    uint64_t dsp         : 1;  /* sel, uadd8, usub8, pkhbt, qadd, …  */
    uint64_t sat         : 1;  /* ssat/usat                          */
    uint64_t div         : 1;  /* udiv/sdiv                          */
    uint64_t bfx         : 1;  /* bfi, bfc, sbfx, ubfx               */
    uint64_t clz_rbit    : 1;  /* clz, rbit                          */
    uint64_t ldaex       : 1;  /* lda/stl acquire/release (v8)       */
    uint64_t vfp_sp      : 1;  /* single-precision FP                */
    uint64_t vfp_dp      : 1;  /* double-precision FP                */
    uint64_t tbb_tbh     : 1;  /* tbb/tbh table branches             */
    uint64_t cbz         : 1;  /* cbz/cbnz                           */
    uint64_t hwdiv_t16   : 1;  /* reserved for narrow div forms      */

    /* ───── reserved / future enablers (bits 16-31) ───── */
    /* Descriptor tables may already set these; thop_emit filters
       them out until a profile enables them. As emitters land, drop
       matching fields into the profile initializers below. */

    /* ARMv8-M Security Extension (TrustZone-M / CMSE)                */
    uint64_t sec         : 1;  /* sg, bxns, blxns                    */
    uint64_t sec_tt      : 1;  /* tt, ttt, tta, ttat                 */

    /* ARMv8.1-M Mainline core additions                              */
    uint64_t lob         : 1;  /* low-overhead-branch: wls, dls, le… */
    uint64_t pacbti      : 1;  /* pac, aut, pacg, autg, bti          */
    uint64_t cde         : 1;  /* custom datapath: cx{1,2,3}, vcx…   */
    uint64_t ras         : 1;  /* reliability / esb                  */

    /* Floating-point extras                                          */
    uint64_t fp16        : 1;  /* half-precision FP (vcvtb/vcvtt.f16)*/
    uint64_t fp_armv8    : 1;  /* vrint*, vsel, vmaxnm, vminnm       */
    uint64_t fp_dp_d32   : 1;  /* 32 double registers (d16..d31)     */

    /* MVE / Helium (vector extension, v8.1-M Mainline)               */
    uint64_t mve_int     : 1;  /* integer MVE                        */
    uint64_t mve_fp      : 1;  /* FP MVE (requires mve_int + FP)     */

    /* Misc future                                                    */
    uint64_t cache_maint : 1;  /* dc, ic cache maintenance forms     */
    uint64_t debug       : 1;  /* bkpt variants, hlt, dbg imm        */
    uint64_t coproc      : 1;  /* mcr/mrc/mcrr/mrrc/cdp              */
    uint64_t lrcpc       : 1;  /* load-acquire RCpc forms            */
    uint64_t unpriv_ls   : 1;  /* ldrt/strt family (already partial) */

    /* bits 32-47: architect's playground — reserved without commitment */
    uint64_t reserved_arch   : 16;
    /* bits 48-63: vendor / Zig-side compiler-specific feature flags  */
    uint64_t reserved_vendor : 16;
} thop_feat;

_Static_assert(sizeof(thop_feat) == sizeof(uint64_t),
               "thop_feat must pack into 64 bits");

/* Bulk helpers — type-pun through memcpy (defined behaviour, the
   compiler folds it away). Used for profile composition and the
   engine's subset test; single-capability checks use `f.<name>` direct. */
static inline uint64_t thop_feat_bits(thop_feat f) {
    uint64_t b; memcpy(&b, &f, sizeof b); return b;
}
static inline thop_feat thop_feat_from_bits(uint64_t b) {
    thop_feat f; memcpy(&f, &b, sizeof f); return f;
}
static inline thop_feat thop_feat_or(thop_feat a, thop_feat b) {
    return thop_feat_from_bits(thop_feat_bits(a) | thop_feat_bits(b));
}
static inline bool thop_feat_subset(thop_feat sub, thop_feat sup) {
    uint64_t s = thop_feat_bits(sub);
    return (s & thop_feat_bits(sup)) == s;
}

### 9.2 Profile = union of features

Each profile's **mandatory** feature set is declared as a `static const thop_feat` with designated bitfield initializers — no mask macros, no preprocessor OR chains. Optional extensions (security, MVE, PACBTI, CDE, …) are separate constants added on top with `-mcmse`, `-mextension=`, or implied by `-mcpu=` using `thop_feat_or`.

```c
/* ──── mandatory cores ──── */
static const thop_feat THOP_PROFILE_ARMV6M_CORE = {
    .t16 = 1
};

static const thop_feat THOP_PROFILE_ARMV7M_CORE = {
    .t16 = 1, .t32 = 1, .it = 1, .mod_imm = 1,
    .movw_movt = 1, .bfx = 1, .clz_rbit = 1,
    .tbb_tbh = 1, .cbz = 1, .sat = 1, .div = 1
};

static const thop_feat THOP_PROFILE_ARMV7EM_CORE = {   /* Cortex-M4/M7 */
    .t16 = 1, .t32 = 1, .it = 1, .mod_imm = 1,
    .movw_movt = 1, .bfx = 1, .clz_rbit = 1,
    .tbb_tbh = 1, .cbz = 1, .sat = 1, .div = 1,
    .dsp = 1
};

static const thop_feat THOP_PROFILE_ARMV8M_BASE_CORE = {
    .t16 = 1, .movw_movt = 1, .cbz = 1, .ldaex = 1
};

static const thop_feat THOP_PROFILE_ARMV8M_MAIN_CORE = {
    .t16 = 1, .t32 = 1, .it = 1, .mod_imm = 1,
    .movw_movt = 1, .bfx = 1, .clz_rbit = 1,
    .tbb_tbh = 1, .cbz = 1, .sat = 1, .div = 1,
    .dsp = 1, .ldaex = 1, .fp_armv8 = 1
};

static const thop_feat THOP_PROFILE_ARMV81M_MAIN_CORE = { /* Cortex-M55/M85 */
    .t16 = 1, .t32 = 1, .it = 1, .mod_imm = 1,
    .movw_movt = 1, .bfx = 1, .clz_rbit = 1,
    .tbb_tbh = 1, .cbz = 1, .sat = 1, .div = 1,
    .dsp = 1, .ldaex = 1, .fp_armv8 = 1, .lob = 1
};

/* ──── optional extension bundles ──── */
static const thop_feat THOP_EXT_CMSE     = { .sec = 1, .sec_tt = 1 };
static const thop_feat THOP_EXT_PACBTI   = { .pacbti = 1 };
static const thop_feat THOP_EXT_CDE      = { .cde = 1 };
static const thop_feat THOP_EXT_MVE_INT  = { .mve_int = 1 };
static const thop_feat THOP_EXT_MVE_FP   = {
    .mve_int = 1, .mve_fp = 1, .fp16 = 1
};

/* ──── FPU bundles (map from -mfpu=) ──── */
static const thop_feat THOP_FPU_NONE          = { 0 };
static const thop_feat THOP_FPU_VFPV4_SP_D16  = { .vfp_sp = 1 };
static const thop_feat THOP_FPU_FPV5_SP_D16   = {
    .vfp_sp = 1, .fp_armv8 = 1
};
static const thop_feat THOP_FPU_FPV5_D16      = {
    .vfp_sp = 1, .vfp_dp = 1, .fp_armv8 = 1
};
static const thop_feat THOP_FPU_FPV5_D32      = {
    .vfp_sp = 1, .vfp_dp = 1, .fp_armv8 = 1, .fp_dp_d32 = 1
};
static const thop_feat THOP_FPU_FP_ARMV8_FULL = {
    .vfp_sp = 1, .vfp_dp = 1, .fp_armv8 = 1, .fp_dp_d32 = 1,
    .fp16 = 1
};
```

### 9.3 Runtime target — `-march=` / `-mfpu=`

Target profile is chosen **at compile invocation**, not at TCC build time. A single `armv8m-tcc` binary accepts `-march=…` and `-mfpu=…` and lowers accordingly:

```
-march=armv6-m
-march=armv7-m
-march=armv7e-m
-march=armv8-m.base
-march=armv8-m.main
-march=armv8.1-m.main
-mfpu=none | vfpv4-sp-d16 | fpv5-sp-d16 | fpv5-d16 | fpv5-d32 | fp-armv8-full
-mcmse                                    (enable security extension)
-mextension=mve | mve.fp | pacbti | cde   (repeatable, or comma-separated)
-mcpu=<name>                              (implies -march + extensions)
```

`-mcpu=` is a convenience alias. Planned mappings (emitters can come later, flag parsing lands up-front so future rollouts don't need new table format):

| `-mcpu=` | Implies |
|---|---|
| `cortex-m0` / `m0plus` / `m1` | `-march=armv6-m` |
| `cortex-m3` | `-march=armv7-m` |
| `cortex-m4` | `-march=armv7e-m` (+`-mfpu=vfpv4-sp-d16` for `m4f`) |
| `cortex-m7` | `-march=armv7e-m -mfpu=fpv5-d16` |
| `cortex-m23` | `-march=armv8-m.base` |
| `cortex-m33` / `m35p` | `-march=armv8-m.main -mfpu=fpv5-sp-d16` |
| `cortex-m55` | `-march=armv8.1-m.main -mfpu=fp-armv8-full -mextension=mve.fp` |
| `cortex-m85` | `-march=armv8.1-m.main -mfpu=fp-armv8-full -mextension=mve.fp,pacbti` |

The parsed combination becomes a `thop_feat` word on `TCCState`:

```c
struct TCCState {
    /* … existing fields … */
    thop_feat thop_target_feat;   /* union of profile + FPU feature bits */
};
```

Populated once during option parsing:

```c
static thop_feat thop_feats_from_march(const char *s) {
    if (!strcmp(s, "armv6-m"))       return THOP_PROFILE_ARMV6M_CORE;
    if (!strcmp(s, "armv7-m"))       return THOP_PROFILE_ARMV7M_CORE;
    if (!strcmp(s, "armv7e-m"))      return THOP_PROFILE_ARMV7EM_CORE;
    if (!strcmp(s, "armv8-m.base"))  return THOP_PROFILE_ARMV8M_BASE_CORE;
    if (!strcmp(s, "armv8-m.main"))  return THOP_PROFILE_ARMV8M_MAIN_CORE;
    if (!strcmp(s, "armv8.1-m.main"))return THOP_PROFILE_ARMV81M_MAIN_CORE;
    tcc_error("unknown -march=%s", s);
}

static thop_feat thop_feats_from_mfpu(const char *s) {
    if (!s || !strcmp(s, "none"))    return THOP_FPU_NONE;
    if (!strcmp(s, "vfpv4-sp-d16"))  return THOP_FPU_VFPV4_SP_D16;
    if (!strcmp(s, "fpv5-sp-d16"))   return THOP_FPU_FPV5_SP_D16;
    if (!strcmp(s, "fpv5-d16"))      return THOP_FPU_FPV5_D16;
    if (!strcmp(s, "fpv5-d32"))      return THOP_FPU_FPV5_D32;
    if (!strcmp(s, "fp-armv8-full")) return THOP_FPU_FP_ARMV8_FULL;
    tcc_error("unknown -mfpu=%s", s);
}

static thop_feat thop_feats_from_extension(const char *s) {
    if (!strcmp(s, "mve"))     return THOP_EXT_MVE_INT;
    if (!strcmp(s, "mve.fp"))  return THOP_EXT_MVE_FP;
    if (!strcmp(s, "pacbti"))  return THOP_EXT_PACBTI;
    if (!strcmp(s, "cde"))     return THOP_EXT_CDE;
    tcc_error("unknown -mextension=%s", s);
}

/* after all flags parsed */
thop_feat f = thop_feat_or(thop_feats_from_march(march),
                           thop_feats_from_mfpu(mfpu));
for (int i = 0; i < n_ext; i++)
    f = thop_feat_or(f, thop_feats_from_extension(ext_names[i]));
if (mcmse_flag) f = thop_feat_or(f, THOP_EXT_CMSE);
s->thop_target_feat = f;
```

**Sanity check at parse time** — reject ill-formed combinations so errors come from `-march`, not from code generation:

```c
if (s->thop_target_feat.mve_fp && !s->thop_target_feat.vfp_sp)
    tcc_error("-mextension=mve.fp requires an FP unit (-mfpu=…)");

if ((s->thop_target_feat.sec || s->thop_target_feat.sec_tt) &&
    !(s->thop_target_feat.t32 || s->thop_target_feat.movw_movt))
    tcc_error("-mcmse requires a mainline or v8-M baseline profile");
```

A build-time `TCC_TARGET_ARM_DEFAULT_ARCH` macro only decides the **default** when `-march` is absent; it no longer gates encodings.

### 9.4 Descriptor extension

```c
typedef struct {
    uint8_t   size;
    uint32_t  base;
    thumb_enforce_encoding req;
    thop_feat feat;                /* required features — subset of target */
    /* … rest as before … */
} thop_variant;
```

### 9.5 Engine gating

`thop_emit` takes `TCCState *s` (already in scope at every code-gen call site) and filters per-invocation:

```c
thumb_opcode thop_emit(TCCState *s,
                       const thop_variant *table, size_t n,
                       thop_args a)
{
    for (const thop_variant *v = table; v < table + n; ++v) {
        if (!thop_feat_subset(v->feat, s->thop_target_feat)) continue; /* arch */
        if (v->req != ENFORCE_ENCODING_NONE && v->req != a.enc)   continue;
        /* register / imm / shift / flags checks … */
        if (all_pass) return emit_bits(v, a);
    }
    tcc_error("no legal encoding for this instruction on -march=%s",
              thop_arch_name(s->thop_target_feat));
}
```

The gate is one AND + compare per variant (~2–4 variants per table) — negligible, and the whole table walk is data-cache friendly.

### 9.6 Example: `add_imm` on any target

```c
static const thop_variant TH_ADD_IMM[] = {
    { .size=16, .base=0x1c00,     .feat={.t16=1},              /* T1 */ },
    { .size=16, .base=0x3000,     .feat={.t16=1},              /* T2 */ },
    { .size=32, .base=0xf1000000, .feat={.t32=1, .mod_imm=1},  /* T3 */ },
    { .size=32, .base=0xf2000000, .feat={.t32=1},              /* T4 */ },
};
```

Same table, any `-march=`. On `-march=armv6-m`, T3/T4 fail the feature gate and the engine falls through to T1/T2 only. On `-march=armv7-m`, all four are candidates and narrowest-wins picks T1 when legal. `-march=armv6-m` + `ENFORCE_ENCODING_32BIT` → legitimate error ("no legal encoding on armv6-m").

### 9.7 Cross-compatibility assertions

Since the same table serves every architecture, add a small startup self-test (guarded by `CONFIG_TCC_DEBUG`) that walks every descriptor table and asserts:

- At least one variant with `.feat ⊆ THOP_PROFILE_ARMV6M_CORE` exists for every mnemonic that v6-M *should* support (guards against accidental v7-M-only tables).
- No variant's `.feat` contains bits unknown to any known profile (catches typos).
- Every variant whose `.feat` includes a "reserved / not-yet-emitted" bit (§9.1 bits 16+) is flagged with a `TODO` comment; the self-test warns if such an entry is ever selected at runtime, so partially-implemented features can't silently emit garbage.

### 9.8 What this replaces

Every current `#ifndef TCC_TARGET_ARM_ARCHV6M` / `#ifdef TCC_TARGET_HAS_FPU` block inside an opcode builder becomes a `.feat=` bit on the corresponding variant. Arch gating moves out of function bodies *and* out of the build system, into the descriptor tables — one place, data-driven, observable at runtime via `-march=`.

Audit target during rollout: grep for `TCC_TARGET_ARM_ARCHV6M` and `TCC_TARGET_HAS_` inside [arm-thumb-opcodes.c](arm-thumb-opcodes.c) — every hit becomes a feature bit on the corresponding descriptor.

### 9.9 Rollout impact on §7

Section 7's plan gains one prerequisite step:

0. **Wire `-march=` / `-mfpu=` parsing** in [libtcc.c](libtcc.c) / options table, populate the target config global (§10), and thread it into `thop_emit`. Default value = current build-time profile, so existing tests pass unchanged.

Then steps 1–5 proceed as before; each ported mnemonic gains its `.feat=` annotations as part of the port, and the `#ifndef TCC_TARGET_ARM_ARCHV6M` blocks in the old function body are deleted in the same commit.

## 10. Target config — outside `TCCState`

`TCCState` must stay target-agnostic (it's shared with the frontend, IR, preprocessor, linker). Arch-dependent configuration lives in a **separate backend-owned global**, queried via a narrow API from generic code. The goal: delete every `#ifdef TCC_TARGET_ARM_*` / `#ifdef CONFIG_TCC_HAS_*` scattered across `tccgen.c`, the IR, and the linker, replaced by a call like `tcc_target_has(TCC_CAP_HW_DIVIDE)`.

### 10.1 The global

New file [arm-thumb-target.c](arm-thumb-target.c) (header in [arm-thumb-target.h](arm-thumb-target.h)), owned by the ARM backend. Not in any generic header.

```c
/* arm-thumb-target.h — backend-private, included only by ARM backend files */

typedef struct {
    /* identity */
    const char *march_name;      /* "armv7e-m", … */
    const char *mfpu_name;       /* "fpv5-sp-d16" / "none" */
    const char *mcpu_name;       /* "cortex-m33" / NULL */

    /* capability bits */
    thop_feat   feat;            /* §9.1 mask, the authoritative source */

    /* numeric derivations (computed from feat at init, not recomputed) */
    uint8_t     ptr_size;        /* 4 */
    uint8_t     int_reg_count;   /* 13 */
    uint8_t     fp_reg_count;    /* 0 / 32 / 64 depending on FPU */
    uint8_t     default_align;   /* 4 */
    uint8_t     stack_align;     /* 8 (AAPCS) */
    bool        big_endian;
    bool        has_it_blocks;
    bool        has_fpu;
    bool        is_secure_tz;    /* CMSE code model */
} thop_target_config;

extern const thop_target_config *thop_target;   /* set once, read many */
```

The backing storage lives in `arm-thumb-target.c`:

```c
static thop_target_config g_thop_target_storage;
const thop_target_config  *thop_target = &g_thop_target_storage;

void thop_target_init(thop_feat feat,
                      const char *march, const char *mfpu, const char *mcpu)
{
    g_thop_target_storage = (thop_target_config){
        .march_name     = march,
        .mfpu_name      = mfpu,
        .mcpu_name      = mcpu,
        .feat           = feat,
        .ptr_size       = 4,
        .int_reg_count  = 13,
        .fp_reg_count   = feat.fp_dp_d32 ? 64
                        : feat.vfp_dp    ? 32
                        : feat.vfp_sp    ? 32 : 0,
        .default_align  = 4,
        .stack_align    = 8,
        .big_endian     = false,
        .has_it_blocks  = feat.it,
        .has_fpu        = feat.vfp_sp,
        .is_secure_tz   = feat.sec,
    };
}
```

Exposed as `const *` so generic code reads but never writes.

### 10.2 Generic-facing API — `tcc_target.h`

A single small header generic modules include. It knows nothing about ARM; it is the only contract across the backend boundary:

```c
/* tcc_target.h — included by tccgen.c, ir/*, tccls.c, tccelf.c … */

/* Opaque feature id — an integer, assigned by the active backend's
   thop_feat bits, but generic code treats it as opaque. */
typedef uint64_t tcc_target_feat;

/* Well-known, cross-arch queries every backend must answer. */
int    tcc_target_ptr_size(void);         /* bytes                  */
int    tcc_target_int_reg_count(void);
int    tcc_target_fp_reg_count(void);
int    tcc_target_stack_align(void);
int    tcc_target_default_align(void);
bool   tcc_target_big_endian(void);
const char *tcc_target_arch_name(void);

/* Capability query — generic code knows a symbolic name, not the bit. */
bool   tcc_target_has(tcc_target_cap cap);

/* A small enumeration of capabilities generic code actually asks about.
   Not every backend must implement every capability; unknown → false. */
typedef enum {
    TCC_CAP_HW_DIVIDE,
    TCC_CAP_HW_FP_SP,
    TCC_CAP_HW_FP_DP,
    TCC_CAP_HW_FP_HP,
    TCC_CAP_DSP_SIMD,
    TCC_CAP_SATURATING_ARITH,
    TCC_CAP_BITFIELD_INSTRS,
    TCC_CAP_COND_EXEC,        /* IT blocks / conditional moves */
    TCC_CAP_MOVE_IMM_WIDE,    /* movw/movt */
    TCC_CAP_VECTOR,           /* MVE / NEON-like */
    TCC_CAP_SECURITY,         /* TrustZone-M / CMSE */
    TCC_CAP_POINTER_AUTH,     /* PACBTI */
    TCC_CAP_LOW_OVERHEAD_LOOP,
    /* … extended as generic code grows new branches … */
} tcc_target_cap;
```

### 10.3 ARM implementation

`arm-thumb-target.c` provides the bodies by translating capabilities to `thop_feat` bits:

```c
bool tcc_target_has(tcc_target_cap cap) {
    const thop_feat f = thop_target->feat;
    switch (cap) {
    case TCC_CAP_HW_DIVIDE:         return f.div;
    case TCC_CAP_HW_FP_SP:          return f.vfp_sp;
    case TCC_CAP_HW_FP_DP:          return f.vfp_dp;
    case TCC_CAP_HW_FP_HP:          return f.fp16;
    case TCC_CAP_DSP_SIMD:          return f.dsp;
    case TCC_CAP_SATURATING_ARITH:  return f.sat;
    case TCC_CAP_BITFIELD_INSTRS:   return f.bfx;
    case TCC_CAP_COND_EXEC:         return f.it;
    case TCC_CAP_MOVE_IMM_WIDE:     return f.movw_movt;
    case TCC_CAP_VECTOR:            return f.mve_int;
    case TCC_CAP_SECURITY:          return f.sec;
    case TCC_CAP_POINTER_AUTH:      return f.pacbti;
    case TCC_CAP_LOW_OVERHEAD_LOOP: return f.lob;
    }
    return false;
}

int tcc_target_ptr_size(void)        { return thop_target->ptr_size; }
int tcc_target_int_reg_count(void)   { return thop_target->int_reg_count; }
int tcc_target_fp_reg_count(void)    { return thop_target->fp_reg_count; }
int tcc_target_stack_align(void)     { return thop_target->stack_align; }
int tcc_target_default_align(void)   { return thop_target->default_align; }
bool tcc_target_big_endian(void)     { return thop_target->big_endian; }
const char *tcc_target_arch_name(void) { return thop_target->march_name; }
```

Backend-internal code (in `arm-thumb-*.c`) keeps using the raw `thop_target->feat` mask and `thop_emit`'s per-variant gating — that's the fast path. The cross-module API is there only for the **generic** modules.

### 10.4 Rewriting generic code

**Before** (hypothetical sample from `tccgen.c` / `ir/opt.c`):

```c
#if defined(TCC_TARGET_ARM_ARCHV7M) || defined(TCC_TARGET_ARM_ARCHV8M_MAIN)
    emit_hw_div(vtop);
#else
    emit_libcall_udiv(vtop);
#endif
```

**After**:

```c
if (tcc_target_has(TCC_CAP_HW_DIVIDE))
    emit_hw_div(vtop);
else
    emit_libcall_udiv(vtop);
```

The generic file no longer mentions ARM. Adding a new target later just means a new `*-target.c` implementing `tcc_target_has`.

### 10.5 Where the global gets written

One setter per invocation, called from the option-parsing code (which is already under the backend include path):

```c
/* in the -march / -mfpu / -mcpu handler */
thop_feat feat = thop_feat_or(thop_feats_from_march(march),
                              thop_feats_from_mfpu(mfpu));
for (int i = 0; i < n_ext; i++)
    feat = thop_feat_or(feat, thop_feats_from_extension(ext_names[i]));
if (mcmse) feat = thop_feat_or(feat, THOP_EXT_CMSE);

thop_target_init(feat, march, mfpu, mcpu);
```

The global is set **once** per `tcc_new()` invocation, before any codegen runs. If `libtcc` is used as a JIT with multiple `TCCState` instances, each must call `thop_target_init` before code generation, and mixing targets within one process is not supported — which matches how the preprocessor target symbols (`__ARM_ARCH_*`) already behave. Documenting this is enough; adding per-state target would push architectural state back into `TCCState` and re-create the problem we're solving.

### 10.6 What this deletes from the codebase

Grep-driven cleanup targets (expected survivors only in backend files, not in generic ones):

```
TCC_TARGET_ARM_ARCHV6M       # becomes runtime !tcc_target_has(...)
TCC_TARGET_ARM_ARCHV7M
TCC_TARGET_ARM_ARCHV7EM
TCC_TARGET_ARM_ARCHV8M_*
CONFIG_TCC_HAS_FPU
TCC_TARGET_HAS_VFP*
```

After rollout: those macros remain only inside the `arm-thumb-*.c` unit and `arm-thumb-target.c` (for bootstrap defaults), not in `tccgen.c`, `tccpp.c`, `tccelf.c`, `tccld.c`, or anything under `ir/`.

### 10.7 Lifecycle & testing

- **Init order**: `tcc_new` → parse CLI → `thop_target_init` → codegen. Any codegen call before init is a programmer bug; assert `thop_target->march_name != NULL` in `thop_emit`.
- **Reentrancy**: single-process single-target, per §10.5.
- **Self-test** (debug build): on first `thop_emit` call, assert `thop_target->feat != 0` and that `feat`'s bits are a subset of the union of all defined profile + extension masks — catches missed bits.
- **Tests**: all existing `tests/thumb/armv8m/` tests pass unchanged; add a handful that toggle `-march` and assert the correct lowering path (e.g. hw-div vs libcall for `-march=armv6-m` vs `-march=armv7-m`).

## 11. Merging with the existing `ArchitectureConfig`

The repo already has the right shape in [tcc.h:2415-2426](tcc.h#L2415-L2426):

- `ArchitectureConfig` — arch-owned struct (`pointer_size`, `stack_align`, `reg_size`, `parameter_registers`, `has_fpu`, `static_chain_reg`, `*fpu`).
- `architecture_config` — single extern global, instantiated in [arch/armv8m.c](arch/armv8m.c).
- `FloatingPointConfig` — pluggable via `.fpu` pointer, instantiated in [arch/fpu/arm/fpv5-sp-d16.c](arch/fpu/arm/fpv5-sp-d16.c).

§10's "target config global" **is** `architecture_config`. No second global, no `thop_target` parallel structure. The work is (a) extending the struct with `thop_feat`, (b) moving `arch/armv8m.c` into a per-profile tree, (c) adding the cap-query wrapper.

### 11.1 Extended `ArchitectureConfig`

```c
/* tcc.h — additions to the existing struct */
typedef struct ArchitectureConfig
{
    /* existing fields — unchanged */
    int8_t pointer_size;
    int8_t stack_align;
    int8_t reg_size;
    int8_t parameter_registers;
    int8_t has_fpu : 1;
    int8_t static_chain_reg;
    const FloatingPointConfig *fpu;

    /* NEW — identity + backend capability struct */
    const char *march_name;      /* "armv8-m.main", … */
    const char *mcpu_name;       /* "cortex-m33" / NULL */
    thop_feat   feat;            /* bitfield struct from §9.1 */
    uint8_t     int_reg_count;   /* 13 for ARM-M */
    uint8_t     default_align;
    bool        big_endian;
    bool        is_secure_tz;    /* TrustZone-M / CMSE model */
} ArchitectureConfig;
```

`thop_feat` and the profile constants from §9.1–9.2 live in [arch/arm/arm.h](arch/arm/arm.h) (see §11.3). `ArchitectureConfig` moves out of `tcc.h` into `arch/arch.h`, which pulls in the active backend header — generic code reads feat only via `tcc_target_has()`, never by field name, so it stays arch-agnostic.

### 11.2 Generic cap-query (§10.2) reads the same global

```c
/* tcc_target.h — tiny generic wrapper, no ARM knowledge */
bool tcc_target_has(tcc_target_cap cap);
int  tcc_target_ptr_size(void);      /* architecture_config.pointer_size */
int  tcc_target_int_reg_count(void); /* architecture_config.int_reg_count */
/* … */
```

The ARM implementation (in [arch/arm/arm.c](arch/arm/arm.c)) does the cap→bit translation against `architecture_config.feat`, exactly as in §10.3. The `thop_target` name in §10 goes away — every reference becomes `architecture_config`.

### 11.3 Proposed directory tree — `arch/arm/…`

Collapse the flat `arch/armv8m.c` + `arch/fpu/arm/` into a properly nested tree so every profile is its own translation unit and new profiles slot in without touching existing ones.

```
arch/
├── arch.h                     # ArchitectureConfig lives here (moved from tcc.h)
├── arch.c                     # tcc_target_has() dispatcher (trivial on single-arch build)
├── arm_aapcs.c                # unchanged, stays at this level (shared across ARM)
└── arm/
    ├── arm.h                  # thop_feat union + bitfields, THOP_PROFILE_*_CORE,
    │                          # THOP_EXT_*, THOP_FPU_* constants (§9.1–9.2)
    ├── arm.c                  # cap→feat translation + thop_target_init()
    ├── profiles/
    │   ├── armv6m.c           # .feat = THOP_PROFILE_ARMV6M_CORE
    │   ├── armv7m.c
    │   ├── armv7em.c
    │   ├── armv8m_base.c
    │   ├── armv8m_main.c      # ← replaces current arch/armv8m.c
    │   └── armv81m_main.c
    ├── cpus/                  # -mcpu= shortcuts (optional TU per CPU)
    │   ├── cortex_m0.c
    │   ├── cortex_m3.c
    │   ├── cortex_m4.c        # + m4f variant
    │   ├── cortex_m7.c
    │   ├── cortex_m23.c
    │   ├── cortex_m33.c       # + m35p
    │   ├── cortex_m55.c
    │   └── cortex_m85.c
    └── fpu/                   # ← moved from arch/fpu/arm/
        ├── none.c
        ├── vfpv4_sp_d16.c
        ├── fpv5_sp_d16.c      # ← moved from arch/fpu/arm/fpv5-sp-d16.c
        ├── fpv5_d16.c
        ├── fpv5_d32.c
        └── fp_armv8_full.c
```

Rationale:
- `arch/arm/profiles/` holds the **static template** for each `-march=` value — one `ArchitectureConfig` initializer.
- `arch/arm/cpus/` is the convenience layer — each CPU TU just `memcpy`s the profile template and sets `mcpu_name` / `.fpu`. Optional; `-mcpu=` could alternatively resolve to `-march=…` + `-mfpu=…` in the option parser and skip cpus/ entirely. Pick whichever feels less fragile.
- `arch/arm/fpu/` becomes arch-specific FPU templates — already how `arch/fpu/arm/` is organized, just promoted under the ARM umbrella for locality.
- Nothing in `arch/arm/` is included by generic code. The generic contract stays `arch.h` + `tcc_target.h`.

### 11.4 Each profile file becomes a tiny template

```c
/* arch/arm/profiles/armv7em.c */
#include "tcc.h"
#include "arch/arm/arm.h"

const ArchitectureConfig arm_profile_armv7em = {
    /* existing fields */
    .pointer_size        = 4,
    .stack_align         = 8,
    .reg_size            = 4,
    .parameter_registers = 4,
    .has_fpu             = 0,              /* filled by -mfpu= */
    .static_chain_reg    = 10,
    .fpu                 = NULL,           /* filled by -mfpu= */
    /* new fields */
    .march_name          = "armv7e-m",
    .mcpu_name           = NULL,
    .feat                = THOP_PROFILE_ARMV7EM_CORE,
    .int_reg_count       = 13,
    .default_align       = 4,
    .big_endian          = false,
    .is_secure_tz        = false,
};
```

`arch/arm/arm.c` provides the applier — picks the profile template by `-march=`, overlays FPU / extension bits, and publishes:

```c
ArchitectureConfig architecture_config;   /* the single extern */

void arm_target_init(const char *march, const char *mfpu,
                     const char *mcpu,   thop_feat extra_feat)
{
    const ArchitectureConfig *tpl = arm_resolve_profile(march, mcpu);
    architecture_config = *tpl;                                /* copy */
    architecture_config.feat = thop_feat_or(architecture_config.feat,
                                            extra_feat);
    if (mfpu) {
        const FloatingPointConfig *fpu = arm_resolve_fpu(mfpu);
        architecture_config.fpu     = fpu;
        architecture_config.has_fpu = fpu != NULL;
        architecture_config.feat    = thop_feat_or(architecture_config.feat,
                                                   arm_fpu_feat(mfpu));
    }
    if (mcpu) architecture_config.mcpu_name = mcpu;
}
```

This fuses §9 (`-march`/`-mfpu`/`-mcmse`/`-mextension` parsing), §10 (single init point, never written by generic code), and the existing `architecture_config` contract into one flow.

### 11.5 FPU side: reuse existing `FloatingPointConfig`

Add `thop_feat fpu_feat` to `FloatingPointConfig` so each FPU template declares which capability bits it provides (initialized from the matching `THOP_FPU_*` constant); `arm_target_init` folds it in via `thop_feat_or`. The `has_*` booleans in `FloatingPointConfig` stay — those are codegen dispatch flags and work fine alongside the opcode-level feat struct.

### 11.6 Makefile impact

Build the union of: `arch/arm/arm.c`, exactly one `arch/arm/profiles/*.c` (selected by configure or all linked and selected at runtime — all-in is simpler and only ~1 KB), exactly one or all `arch/arm/fpu/*.c`, and the relevant `arch/arm/cpus/*.c`. Since profiles are `const` data with distinct names, linking all of them is harmless and lets a single `armv8m-tcc` binary switch via `-march=` at runtime — which was the whole point of §9.3.

### 11.7 What gets deleted on rollout

- [arch/armv8m.c](arch/armv8m.c) — replaced by [arch/arm/profiles/armv8m_main.c](arch/arm/profiles/armv8m_main.c) + shared init in [arch/arm/arm.c](arch/arm/arm.c).
- `arch/fpu/arm/` — moves wholesale to `arch/arm/fpu/`.
- `#ifdef TCC_TARGET_ARM_ARCH*` in generic code — all become `tcc_target_has(TCC_CAP_*)`.
- Any `architecture_config.has_fpu` check inside generic code that's *really* asking "is SP FP available?" — replaced by `tcc_target_has(TCC_CAP_HW_FP_SP)`. Keep `has_fpu` only where it's genuinely a binary toggle (prologue/epilogue VFP save/restore decisions).

