/* thop_shapes_def.h -- every Thumb encoding shape, indexed by
 * enum thop_shape_id (thop_shapes.h).  Included once, by thumb.c. */

const thop_variant_shape thop_shapes[THOP_SHAPE_COUNT] = {

    /* ---- thop_adr.c ---- */

    /* T1: ADR <Rd>, #<imm8*4>  —  rd low reg, imm scaled by 4, positive */
    [THOP_SHAPE_ADR_T1] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {8, 3},
        .rd_con = REG_LOW_ONLY,
        .imm = {.kind = IMM_RAW, .width = 8, .scale_log2 = 2},
        .imm_place = {0, 8},
        .feat = {.t16 = 1},
    },

    /* T3: ADR <Rd>, #<imm12>  —  positive, plain 12-bit */
    [THOP_SHAPE_ADR_T3] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rd_con = REG_NOT_PC,
        .imm = {.kind = IMM_PACK_3_8_1, .width = 12},
        .feat = {.t32 = 1},
    },

    /* T4: ADR <Rd>, #-<imm12>  —  negative offset */
    [THOP_SHAPE_ADR_T4] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rd_con = REG_NOT_PC,
        .imm = {.kind = IMM_PACK_3_8_1, .width = 12, .is_signed = true},
        .feat = {.t32 = 1},
    },

    /* ---- thop_alu_imm.c ---- */

    /* 16-bit: OP <Rdn>, #<imm8>  —  rd==rn, low regs only */
    [THOP_SHAPE_T16_IMM8] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {8, 3},
        .rn_place = {8, 3},
        .rd_con = REG_LOW_ONLY | REG_EQ_RN,
        .rn_con = REG_LOW_ONLY,
        .imm = {.kind = IMM_RAW, .width = 8},
        .imm_place = {0, 8},
        .implicit_s = true,
        .feat = {.t16 = 1},
    },

    /* 16-bit: OP <Rd>, <Rn>, #<imm3>  —  both low regs */
    [THOP_SHAPE_T16_IMM3] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {0, 3},
        .rn_place = {3, 3},
        .rd_con = REG_LOW_ONLY,
        .rn_con = REG_LOW_ONLY,
        .imm = {.kind = IMM_RAW, .width = 3},
        .imm_place = {6, 3},
        .implicit_s = true,
        .feat = {.t16 = 1},
    },

    /* 32-bit: OP{S}.W <Rd>, <Rn>, #<const>  —  modified immediate */
    [THOP_SHAPE_T32_MOD_IMM] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rn_place = {16, 4},
        .rd_con = REG_NOT_PC,
        .rn_con = REG_NOT_PC,
        .imm = {.kind = IMM_PACK_CONST, .width = 12},
        .has_s_bit = 1,
        .feat = {.t32 = 1, .mod_imm = 1},
    },

    /* 32-bit: OPW <Rd>, <Rn>, #<imm12>  —  plain 12-bit */
    [THOP_SHAPE_T32_IMM12] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rn_place = {16, 4},
        .rd_con = REG_NOT_PC,
        .imm = {.kind = IMM_PACK_3_8_1, .width = 12},
        .feat = {.t32 = 1},
    },

    /* 16-bit: ADD SP, SP, #<imm7*4>  —  rd/rn implicit SP, imm scaled by 4 */
    [THOP_SHAPE_T16_ADD_SP_IMM] = {
        .size = THOP_VARIANT_T16,
        .rd_con = REG_SP_ONLY,
        .rn_con = REG_SP_ONLY,
        .imm = {.kind = IMM_RAW, .width = 7, .scale_log2 = 2},
        .imm_place = {0, 7},
        .feat = {.t16 = 1},
    },

    /* 16-bit: ADD <Rd>, SP, #<imm8*4>  —  rd low reg, rn implicit SP, imm scaled by 4 */
    [THOP_SHAPE_T16_ADD_SP_IMM8] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {8, 3},
        .rd_con = REG_LOW_ONLY,
        .rn_con = REG_SP_ONLY,
        .imm = {.kind = IMM_RAW, .width = 8, .scale_log2 = 2},
        .imm_place = {0, 8},
        .feat = {.t16 = 1},
    },

    /* 16-bit: SUB SP, SP, #<imm7*4>  —  rd/rn implicit SP, imm scaled by 4 */
    [THOP_SHAPE_T16_SUB_SP_IMM] = {
        .size = THOP_VARIANT_T16,
        .rd_con = REG_SP_ONLY,
        .rn_con = REG_SP_ONLY,
        .imm = {.kind = IMM_RAW, .width = 7, .scale_log2 = 2},
        .imm_place = {0, 7},
        .feat = {.t16 = 1},
    },

    /* ---- thop_alu_reg.c ---- */

    /* 16-bit: OP <Rd>, <Rn>, <Rm>  —  all low regs, no shift */
    [THOP_SHAPE_T16_REG3] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {0, 3},
        .rn_place = {3, 3},
        .rm_place = {6, 3},
        .rd_con = REG_LOW_ONLY,
        .rn_con = REG_LOW_ONLY,
        .rm_con = REG_LOW_ONLY,
        .implicit_s = true,
        .feat = {.t16 = 1},
    },

    /* 32-bit: OP{S}.W <Rd>, <Rn>, <Rm>{,shift}  —  with S bit and shift */
    [THOP_SHAPE_T32_REG_SHIFT] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rn_place = {16, 4},
        .rm_place = {0, 4},
        .rd_con = REG_NOT_PC,
        .rn_con = REG_NOT_PC,
        .rm_con = REG_NOT_SP | REG_NOT_PC,
        .has_s_bit = 1,
        .shift_type_bits = {4, 2},
        .shift_imm2_bits = {6, 2},
        .shift_imm3_bits = {12, 3},
        .feat = {.t32 = 1},
    },

    /* ADD <Rdm>, SP, <Rdm>  —  rd==rm, rn==SP, DN:Rd split */
    [THOP_SHAPE_T16_ADD_SP_REG] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {0, 3},
        .has_rd_hi = 1,
        .rn_place = {3, 4},
        .rd_con = REG_EQ_RM,
        .rn_con = REG_SP_ONLY,
        .feat = {.t16 = 1},
    },

    /* ADD <Rdn>, <Rm>  —  rd==rn, any reg, no shift, no S, DN:Rd split */
    [THOP_SHAPE_T16_ADD_T2] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {0, 3},
        .has_rd_hi = 1,
        .rn_place = {0, 3},
        .rm_place = {3, 4},
        .rd_con = REG_EQ_RN,
        .feat = {.t16 = 1},
    },

    /* 16-bit: OP <Rdn>, <Rm>  —  rd==rn, all low, no shift (ADC, SBC, AND, ORR, EOR, BIC, etc.) */
    [THOP_SHAPE_T16_REG_RDN_RM] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {0, 3},
        .rm_place = {3, 3},
        .rd_con = REG_LOW_ONLY | REG_EQ_RN,
        .rn_con = REG_LOW_ONLY,
        .rm_con = REG_LOW_ONLY,
        .implicit_s = true,
        .feat = {.t16 = 1},
    },

    /* ---- thop_bitfield.c ---- */
    [THOP_SHAPE_T32_BFX] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rn_place = {16, 4},
        .rd_con = REG_NOT_PC,
        .rn_con = REG_ANY,
        .imm = {.kind = IMM_RAW, .width = 5},
        .split_imm2_place = {6, 2},
        .split_imm3_place = {12, 3},
        .imm2_place = {0, 5},
        .feat = {.t32 = 1, .bfx = 1},
    },

    /* SSAT with LSL (or no shift) — base has sh=0 */
    [THOP_SHAPE_T32_SSAT_LSL] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rn_place = {16, 4},
        .rd_con = REG_NOT_PC,
        .shift_imm3_bits = {12, 3},
        .shift_imm2_bits = {6, 2},
        .shift_allowed = (1u << THUMB_SHIFT_NONE) | (1u << THUMB_SHIFT_LSL),
        .imm2_place = {0, 5},
        .feat = {.t32 = 1, .sat = 1},
    },

    /* SSAT with ASR — base has sh=1 */
    [THOP_SHAPE_T32_SSAT_ASR] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rn_place = {16, 4},
        .rd_con = REG_NOT_PC,
        .shift_imm3_bits = {12, 3},
        .shift_imm2_bits = {6, 2},
        .shift_allowed = (1u << THUMB_SHIFT_ASR),
        .imm2_place = {0, 5},
        .feat = {.t32 = 1, .sat = 1},
    },

    /* USAT with LSL (or no shift) — base has sh=0 */
    [THOP_SHAPE_T32_USAT_LSL] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rn_place = {16, 4},
        .rd_con = REG_NOT_PC,
        .shift_imm3_bits = {12, 3},
        .shift_imm2_bits = {6, 2},
        .shift_allowed = (1u << THUMB_SHIFT_NONE) | (1u << THUMB_SHIFT_LSL),
        .imm2_place = {0, 5},
        .feat = {.t32 = 1, .sat = 1},
    },

    /* USAT with ASR — base has sh=1 */
    [THOP_SHAPE_T32_USAT_ASR] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rn_place = {16, 4},
        .rd_con = REG_NOT_PC,
        .shift_imm3_bits = {12, 3},
        .shift_imm2_bits = {6, 2},
        .shift_allowed = (1u << THUMB_SHIFT_ASR),
        .imm2_place = {0, 5},
        .feat = {.t32 = 1, .sat = 1},
    },

    /* ---- thop_block.c ---- */

    /* T1 narrow: push {reglist}, [lr]  —  raw reglist in bits [7:0], lr flag at bit 8 */
    [THOP_SHAPE_PUSH_T1] = {
        .size = THOP_VARIANT_T16,
        .rm_raw_place = {0, 8},           /* raw register list in bits [7:0] */
        .imm = {.kind = IMM_RAW, .width = 1},
        .imm_place = {8, 1},              /* LR flag at bit 8 */
        .rm_con = REG_LOW_REGSET | REG_RM_BITS_NOT_LR_PC,
        .feat = {.t16 = 1},
    },

    /* T2 wide: push {reglist}  —  register list in bits [15:3], SP/PC not allowed */
    [THOP_SHAPE_PUSH_T2] = {
        .size = THOP_VARIANT_T32,
        .rm_place = {0, 13},              /* register list in bits [12:0] (r0-r12) */
        .imm = {.kind = IMM_RAW, .width = 1},
        .imm_place = {14, 1},             /* LR/M flag at bit 14 */
        .rm_con = REG_RM_BITS_NOT_LR_PC,
        .feat = {.t32 = 1},
    },

    /* T1 narrow: pop {reglist}, [pc]  —  raw reglist in bits [7:0], pc flag at bit 8 */
    [THOP_SHAPE_POP_T1] = {
        .size = THOP_VARIANT_T16,
        .rm_raw_place = {0, 8},           /* raw register list in bits [7:0] */
        .imm = {.kind = IMM_RAW, .width = 1},
        .imm_place = {8, 1},              /* PC flag at bit 8 */
        .rm_con = REG_LOW_REGSET | REG_RM_BITS_NOT_LR_PC,
        .feat = {.t16 = 1},
    },

    /* T2 wide: pop {reglist}  —  register list in bits [15:3], SP not allowed */
    [THOP_SHAPE_POP_T2] = {
        .size = THOP_VARIANT_T32,
        .rm_place = {0, 15},              /* register list in bits [14:0] (r0-r12 + LR) */
        .imm = {.kind = IMM_RAW, .width = 1},
        .imm_place = {15, 1},             /* PC/P flag at bit 15 */
        .rm_con = REG_RM_BIT_NOT_SP,
        .feat = {.t32 = 1},
    },

    /* T1 narrow: ldm {rn}, {reglist}!  —  rn in bits [8:5], raw reglist in bits [7:0] */
    [THOP_SHAPE_LDM_T1] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {8, 3},               /* rn in bits [10:8] */
        .rd_con = REG_LOW_ONLY,
        .rm_raw_place = {0, 8},           /* raw register list in bits [7:0] */
        .rm_con = REG_LOW_REGSET,
        .feat = {.t16 = 1},
    },

    /* T3 wide: ldmia {rn}!, {reglist}  —  rn at [19:16], reglist at [12:0], writeback at [21] */
    [THOP_SHAPE_LDM_T3] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {16, 4},              /* rn in bits [19:16] */
        .rm_place = {0, 16},              /* register list in bits [15:0] (r0-r12, LR, PC) */
        .rm_con = REG_RM_BIT_NOT_SP,
        .imm = {.kind = IMM_RAW, .width = 1},
        .imm_place = {21, 1},             /* writeback bit at position 21 */
        .feat = {.t32 = 1},
    },

    /* T1 narrow: stm {rn}!, {reglist}  —  rn in bits [8:5], raw reglist in bits [7:0] */
    [THOP_SHAPE_STM_T1] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {8, 3},               /* rn in bits [10:8] */
        .rd_con = REG_LOW_ONLY,
        .rm_raw_place = {0, 8},           /* raw register list in bits [7:0] */
        .rm_con = REG_LOW_REGSET,
        .feat = {.t16 = 1},
    },

    /* T3 wide: stmia {rn}!, {reglist}  —  rn at [19:16], reglist at [12:0], writeback at [21] */
    [THOP_SHAPE_STM_T3] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {16, 4},              /* rn in bits [19:16] */
        .rm_place = {0, 15},              /* register list in bits [14:0] (r0-r12, LR) */
        .rm_con = REG_RM_BIT_NOT_SP,
        .imm = {.kind = IMM_RAW, .width = 1},
        .imm_place = {21, 1},             /* writeback bit at position 21 */
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_LDMDB] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {16, 4},
        .rm_place = {0, 16},              /* register list in bits [15:0] (r0-r12, LR, PC) */
        .rm_con = REG_RM_BIT_NOT_SP,
        .imm = {.kind = IMM_RAW, .width = 1},
        .imm_place = {21, 1},
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_STMDB] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {16, 4},
        .rm_place = {0, 15},              /* register list in bits [14:0] (r0-r12, LR) */
        .rm_con = REG_RM_BIT_NOT_SP,
        .imm = {.kind = IMM_RAW, .width = 1},
        .imm_place = {21, 1},
        .feat = {.t32 = 1},
    },

    /* ---- thop_branch.c ---- */
    [THOP_SHAPE_BX] = {
        .size = THOP_VARIANT_T16,
        .rm_place = {3, 4},
        .feat = {.t16 = 1},
    },
    [THOP_SHAPE_BL_T1] = {
        .size = THOP_VARIANT_T32,
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_BLX_REG] = {
        .size = THOP_VARIANT_T16,
        .rm_place = {3, 4},
        .feat = {.t16 = 1},
    },
    [THOP_SHAPE_B_COND_T16] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {8, 4},
        .imm = {.kind = IMM_RAW, .width = 8},
        .imm_place = {0, 8},
        .feat = {.t16 = 1},
    },
    [THOP_SHAPE_B_T3] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {22, 4},
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_B_T4] = {
        .size = THOP_VARIANT_T32,
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_B_T2] = {
        .size = THOP_VARIANT_T16,
        .feat = {.t16 = 1},
    },
    [THOP_SHAPE_CBZ] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {0, 3},
        .rd_con = REG_LOW_ONLY,
        .feat = {.t16 = 1, .cbz = 1},
    },

    /* ---- thop_cmp.c ---- */

    /* T1: CMP <Rn>, #<imm8>  —  16-bit, rn low, imm8 raw */
    [THOP_SHAPE_T16_CMP_IMM] = {
        .size = THOP_VARIANT_T16,
        .rn_place = {8, 3},
        .rn_con = REG_LOW_ONLY,
        .imm = {.kind = IMM_RAW, .width = 8},
        .imm_place = {0, 8},
        .implicit_s = true,
        .feat = {.t16 = 1},
    },

    /* T2/T1 (32-bit): CMP/CMN/TST/TEQ.W <Rn>, #<const>  —  modified imm, rd=0xF hardcoded */
    [THOP_SHAPE_T32_CMP_IMM] = {
        .size = THOP_VARIANT_T32,
        .rn_place = {16, 4},
        .rn_con = REG_NOT_PC,
        .imm = {.kind = IMM_PACK_CONST, .width = 12},
        .implicit_s = true,
        .feat = {.t32 = 1, .mod_imm = 1},
    },

    /* T1: CMP/CMN/TST <Rn>, <Rm>  —  16-bit, both low, no shift */
    [THOP_SHAPE_T16_CMP_REG] = {
        .size = THOP_VARIANT_T16,
        .rn_place = {0, 3},
        .rm_place = {3, 3},
        .rn_con = REG_LOW_ONLY,
        .rm_con = REG_LOW_ONLY,
        .implicit_s = true,
        .feat = {.t16 = 1},
    },

    /* T2: CMP <Rn>, <Rm>  —  rn any (not PC), rm any, no shift */
    [THOP_SHAPE_T16_CMP_REG_T2] = {
        .size = THOP_VARIANT_T16,
        .rm_place = {3, 3},
        .rn_con = REG_NOT_PC,
        .rm_con = REG_ANY,
        .implicit_s = true,
        .feat = {.t16 = 1},
    },

    /* T3/T2 (32-bit): CMP/CMN/TST/TEQ.W <Rn>, <Rm>{,shift}  —  rd=0xF hardcoded */
    [THOP_SHAPE_T32_CMP_REG] = {
        .size = THOP_VARIANT_T32,
        .rn_place = {16, 4},
        .rm_place = {0, 4},
        .rn_con = REG_NOT_PC,
        .rm_con = REG_NOT_SP | REG_NOT_PC,
        .shift_type_bits = {4, 2},
        .shift_imm2_bits = {6, 2},
        .shift_imm3_bits = {12, 3},
        .implicit_s = true,
        .feat = {.t32 = 1},
    },

    /* ---- thop_coproc.c ---- */
    [THOP_SHAPE_COPROC] = {
        .size = THOP_VARIANT_T32,
        .feat = {.t32 = 1, .coproc = 1},
    },

    /* ---- thop_dsp.c ---- */
    [THOP_SHAPE_DSP_REG3] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rn_place = {16, 4},
        .rm_place = {0, 4},
        .feat = {.t32 = 1, .dsp = 1},
    },
    [THOP_SHAPE_PKH] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rn_place = {16, 4},
        .rm_place = {0, 4},
        .shift_allowed = (1u << THUMB_SHIFT_LSL) | (1u << THUMB_SHIFT_ASR),
        .feat = {.t32 = 1, .dsp = 1},
    },

    /* ---- thop_extend.c ---- */

    /* T1: <OP> <Rd>, <Rm>  —  16-bit, rd/rm low, no rotation */
    [THOP_SHAPE_T16_EXTEND] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {0, 3},
        .rm_place = {3, 3},
        .rd_con = REG_LOW_ONLY,
        .rm_con = REG_LOW_ONLY,
        .imm = {.kind = IMM_RAW, .width = 0}, /* rotate must be 0 for T1 */
        .shift_allowed = (1u << THUMB_SHIFT_ROR),
        .feat = {.t16 = 1},
    },

    /* T2: <OP> <Rd>, <Rm>{, ROR #<rotate>}  —  32-bit, rotate in bits [5:4] */
    [THOP_SHAPE_T32_EXTEND] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rm_place = {0, 4},
        .ra_place = {16, 4}, /* rm duplicated at bits [19:16] */
        .rd_con = REG_NOT_PC,
        .imm = {.kind = IMM_RAW, .width = 2, .scale_log2 = 3},
        .imm_place = {4, 2},
        .shift_allowed = (1u << THUMB_SHIFT_ROR),
        .feat = {.t32 = 1},
    },

    /* ---- thop_ldaex.c ---- */
    [THOP_SHAPE_LDAEX] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {12, 4},
        .rn_place = {16, 4},
        .feat = {.t32 = 1, .ldaex = 1},
    },
    [THOP_SHAPE_STLEX] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {0, 4},
        .rn_place = {16, 4},
        .rm_place = {12, 4},
        .feat = {.t32 = 1, .ldaex = 1},
    },

    /* ---- thop_ldr_literal.c ---- */

    /* T1: LDR <Rt>, [PC, #<imm8*4>]  —  rt low reg, imm scaled by 4 */
    [THOP_SHAPE_LDR_LIT_T1] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {8, 3},
        .rd_con = REG_LOW_ONLY,
        .imm = {.kind = IMM_RAW, .width = 8, .scale_log2 = 2},
        .imm_place = {0, 8},
        .feat = {.t16 = 1},
    },
    [THOP_SHAPE_LDR_LIT_T32] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {12, 4},
        .rd_con = REG_NOT_PC,
        .feat = {.t32 = 1},
    },

    /* ---- thop_ldrd.c ---- */
    [THOP_SHAPE_LDRD] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {12, 4},
        .rn_place = {16, 4},
        .rm_place = {8, 4},
        .imm = {.kind = IMM_RAW, .width = 8},
        .feat = {.t32 = 1},
    },

    /* ---- thop_ldrex.c ---- */
    [THOP_SHAPE_LDREX] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {12, 4},
        .rn_place = {16, 4},
        .imm = {.kind = IMM_RAW, .width = 8},
        .imm_place = {0, 8},
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_STREX] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rn_place = {12, 4},
        .rm_place = {16, 4},
        .imm = {.kind = IMM_RAW, .width = 8},
        .imm_place = {0, 8},
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_LDREXB] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {12, 4},
        .rn_place = {16, 4},
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_STREXB] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {0, 4},
        .rn_place = {12, 4},
        .rm_place = {16, 4},
        .feat = {.t32 = 1},
    },

    /* ---- thop_mem_exclusive.c ---- */
    [THOP_SHAPE_T32_EXCLUSIVE] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {12, 4},
        .rn_place = {16, 4},
        .feat = {.t32 = 1},
    },

    /* ---- thop_mem_imm.c ---- */
    [THOP_SHAPE_T16_MEM_IMM4] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {0, 3}, .rn_place = {3, 3},
        .rd_con = REG_LOW_ONLY, .rn_con = REG_LOW_ONLY,
        .imm = {.kind = IMM_RAW, .width = 5, .scale_log2 = 2},
        .imm_place = {6, 5},
        .puw_fixed = 6,
        .feat = {.t16 = 1},
    },
    [THOP_SHAPE_T16_MEM_IMM0] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {0, 3}, .rn_place = {3, 3},
        .rd_con = REG_LOW_ONLY, .rn_con = REG_LOW_ONLY,
        .imm = {.kind = IMM_RAW, .width = 5, .scale_log2 = 0},
        .imm_place = {6, 5},
        .puw_fixed = 6,
        .feat = {.t16 = 1},
    },
    [THOP_SHAPE_T16_MEM_IMM1] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {0, 3}, .rn_place = {3, 3},
        .rd_con = REG_LOW_ONLY, .rn_con = REG_LOW_ONLY,
        .imm = {.kind = IMM_RAW, .width = 5, .scale_log2 = 1},
        .imm_place = {6, 5},
        .puw_fixed = 6,
        .feat = {.t16 = 1},
    },
    [THOP_SHAPE_T16_MEM_SP_IMM4] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {8, 3},
        .rd_con = REG_LOW_ONLY,
        .rn_con = REG_SP_ONLY, /* SP is implicit in encoding */
        .imm = {.kind = IMM_RAW, .width = 8, .scale_log2 = 2},
        .imm_place = {0, 8},
        .puw_fixed = 6,
        .feat = {.t16 = 1},
    },
    [THOP_SHAPE_T32_MEM_POS_ANY_NOTPC] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {12, 4}, .rn_place = {16, 4},
        .rd_con = REG_ANY, .rn_con = REG_NOT_PC,
        .imm = {.kind = IMM_RAW, .width = 12, .scale_log2 = 0},
        .imm_place = {0, 12},
        .puw_fixed = 6,
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_T32_MEM_POS_NOSP_NOTPC] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {12, 4}, .rn_place = {16, 4},
        .rd_con = REG_NOT_SP, .rn_con = REG_NOT_PC,
        .imm = {.kind = IMM_RAW, .width = 12, .scale_log2 = 0},
        .imm_place = {0, 12},
        .puw_fixed = 6,
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_T32_MEM_PC_POS] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {12, 4}, .rn_place = {16, 4},
        .rd_con = REG_ANY, .rn_con = REG_PC_ONLY,
        .imm = {.kind = IMM_RAW, .width = 12, .scale_log2 = 0},
        .imm_place = {0, 12},
        .puw_fixed = 6,
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_T32_MEM_PC_NEG] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {12, 4}, .rn_place = {16, 4},
        .rd_con = REG_ANY, .rn_con = REG_PC_ONLY,
        .imm = {.kind = IMM_RAW, .width = 12, .scale_log2 = 0},
        .imm_place = {0, 12},
        .puw_fixed = 4,
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_T32_MEM_IDX_ANY] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {12, 4}, .rn_place = {16, 4},
        .rd_con = REG_ANY, .rn_con = REG_ANY,
        .imm = {.kind = IMM_RAW, .width = 8, .scale_log2 = 0},
        .imm_place = {0, 8},
        .puw_bits = {8, 3},
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_T32_MEM_IDX_NOSP] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {12, 4}, .rn_place = {16, 4},
        .rd_con = REG_NOT_SP, .rn_con = REG_ANY,
        .imm = {.kind = IMM_RAW, .width = 8, .scale_log2 = 0},
        .imm_place = {0, 8},
        .puw_bits = {8, 3},
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_T32_MEM_IDX_ANY_NOTPC] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {12, 4}, .rn_place = {16, 4},
        .rd_con = REG_ANY, .rn_con = REG_NOT_PC,
        .imm = {.kind = IMM_RAW, .width = 8, .scale_log2 = 0},
        .imm_place = {0, 8},
        .puw_bits = {8, 3},
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_T32_MEM_IDX_NOSP_NOTPC] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {12, 4}, .rn_place = {16, 4},
        .rd_con = REG_NOT_SP, .rn_con = REG_NOT_PC,
        .imm = {.kind = IMM_RAW, .width = 8, .scale_log2 = 0},
        .imm_place = {0, 8},
        .puw_bits = {8, 3},
        .feat = {.t32 = 1},
    },

    /* ---- thop_mem_reg.c ---- */

    /* T1: <OP> <Rt>, [<Rn>, <Rm>]  —  16-bit, all low, no shift */
    [THOP_SHAPE_T16_MEM_REG] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {0, 3},
        .rn_place = {3, 3},
        .rm_place = {6, 3},
        .rd_con = REG_LOW_ONLY,
        .rn_con = REG_LOW_ONLY,
        .rm_con = REG_LOW_ONLY,
        .feat = {.t16 = 1},
    },

    /* T2/T3/T4 (32-bit): <OP> <Rt>, [<Rn>, <Rm>{, LSL #<imm>}]  —  shift amount in imm2 bits [5:4] */
    [THOP_SHAPE_T32_MEM_REG] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {12, 4},
        .rn_place = {16, 4},
        .rm_place = {0, 4},
        .rd_con = REG_NOT_SP,
        .rn_con = REG_NOT_PC,
        .rm_con = REG_NOT_SP | REG_NOT_PC,
        .shift_imm2_bits = {4, 2},
        .shift_allowed = (1u << THUMB_SHIFT_LSL),
        .feat = {.t32 = 1},
    },

    /* ---- thop_mem_unpriv.c ---- */
    [THOP_SHAPE_T32_MEM_UNPRIV] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {12, 4},
        .rn_place = {16, 4},
        .imm = {.kind = IMM_RAW, .width = 8},
        .imm_place = {0, 8},
        .feat = {.t32 = 1},
    },

    /* ---- thop_mov.c ---- */

    /* T1 high-register MOV: MOV <Rd>, <Rm>  —  no shift, no S */
    [THOP_SHAPE_MOV_REG_T1_HIGH] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {0, 3},
        .dn_rd_split = {0, 3},
        .rm_place = {3, 4},
        .rd_con = REG_NOT_PC,
        .rm_con = REG_ANY,
        .feat = {.t16 = 1},
    },
    [THOP_SHAPE_MOV_REG_T1_SHIFT] = {
        .size = THOP_VARIANT_T16,
        .shift_allowed = (1u << THUMB_SHIFT_LSL) | (1u << THUMB_SHIFT_LSR) | (1u << THUMB_SHIFT_ASR),
        .has_s_bit = 1,
        .feat = {.t16 = 1},
    },

    /* T3 wide MOV: MOV{S}.W <Rd>, <Rm>{,shift} */
    [THOP_SHAPE_MOV_REG_T3] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rn_place = {16, 4},
        .rm_place = {0, 4},
        .has_s_bit = 1,
        .shift_type_bits = {4, 2},
        .shift_imm2_bits = {6, 2},
        .shift_imm3_bits = {12, 3},
        .shift_allowed = (1u << THUMB_SHIFT_LSL) | (1u << THUMB_SHIFT_LSR) | (1u << THUMB_SHIFT_ASR) |
                         (1u << THUMB_SHIFT_ROR) | (1u << THUMB_SHIFT_RRX),
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_MOV_IMM_T1] = {
        .size = THOP_VARIANT_T16,
        .implicit_s = true,
        .feat = {.t16 = 1},
    },

    /* T3: MOV <Rd>, #<const>  —  modified immediate */
    [THOP_SHAPE_MOV_IMM_T3] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rd_con = REG_NOT_SP | REG_NOT_PC,
        .imm = {.kind = IMM_PACK_CONST},
        .has_s_bit = 1,
        .feat = {.t32 = 1, .mod_imm = 1},
    },

    /* T4: MOVW <Rd>, #<imm16> */
    [THOP_SHAPE_MOV_IMM_T4] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rd_con = REG_NOT_SP | REG_NOT_PC,
        .imm = {.kind = IMM_PACK_3_8_1},
        .feat = {.t32 = 1, .movw_movt = 1},
    },
    [THOP_SHAPE_MOVT] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rd_con = REG_NOT_SP | REG_NOT_PC,
        .imm = {.kind = IMM_PACK_3_8_1},
        .feat = {.t32 = 1, .movw_movt = 1},
    },
    [THOP_SHAPE_MOV_REG_SHIFT_T1] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {0, 3},
        .rm_place = {0, 3},
        .ra_place = {3, 3},
        .rd_con = REG_LOW_ONLY | REG_EQ_RM,
        .rm_con = REG_LOW_ONLY,
        .ra_con = REG_LOW_ONLY,
        .implicit_s = 1,
        .shift_allowed = (1u << THUMB_SHIFT_LSL) | (1u << THUMB_SHIFT_LSR) | (1u << THUMB_SHIFT_ASR) |
                         (1u << THUMB_SHIFT_ROR),
        .feat = {.t16 = 1},
    },
    [THOP_SHAPE_MOV_REG_SHIFT_T3] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rm_place = {16, 4},
        .ra_place = {0, 4},
        .has_s_bit = 1,
        .shift_type_bits = {21, 2},
        .shift_allowed = (1u << THUMB_SHIFT_LSL) | (1u << THUMB_SHIFT_LSR) | (1u << THUMB_SHIFT_ASR) |
                         (1u << THUMB_SHIFT_ROR),
        .feat = {.t32 = 1},
    },

    /* ---- thop_mrs.c ---- */
    [THOP_SHAPE_MRS] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rm_place = {0, 8},
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_MSR] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {16, 4},
        .rm_place = {0, 8},
        .imm = {.kind = IMM_RAW, .width = 2},
        .imm2_place = {10, 2},
        .feat = {.t32 = 1},
    },

    /* ---- thop_mul.c ---- */
    [THOP_SHAPE_MUL_T16] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {0, 3},
        .rm_place = {3, 3},
        .implicit_s = 1,
        .feat = {.t16 = 1},
    },
    [THOP_SHAPE_MUL_T32] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rn_place = {16, 4},
        .rm_place = {0, 4},
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_MLA] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rn_place = {16, 4},
        .rm_place = {0, 4},
        .ra_place = {12, 4},
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_LONG_MUL] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rn_place = {0, 4},
        .rm_place = {16, 4},
        .ra_place = {12, 4},
        .feat = {.t32 = 1},
    },

    /* UMAAL: {RdHi:RdLo} = Rn * Rm + RdLo + RdHi.  Part of the DSP extension
     * (ARMv7E-M, ARMv8-M Mainline with DSP), unlike the four above. */
    [THOP_SHAPE_LONG_MUL_DSP] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rn_place = {0, 4},
        .rm_place = {16, 4},
        .ra_place = {12, 4},
        .feat = {.t32 = 1, .dsp = 1},
    },
    [THOP_SHAPE_DIV] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rn_place = {16, 4},
        .rm_place = {0, 4},
        .feat = {.t32 = 1, .div = 1},
    },

    /* ---- thop_mvn.c ---- */

    /* T1: MVN <Rd>, <Rm>  —  rd==rn, low regs, implicit S */
    [THOP_SHAPE_MVN_REG_T1] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {0, 3},
        .rm_place = {3, 3},
        .rd_con = REG_LOW_ONLY | REG_EQ_RN,
        .rn_con = REG_LOW_ONLY,
        .rm_con = REG_LOW_ONLY,
        .implicit_s = true,
        .feat = {.t16 = 1},
    },

    /* T3: MVN{S}.W <Rd>, <Rm>{,shift} */
    [THOP_SHAPE_MVN_REG_T3] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rn_place = {16, 4},
        .rm_place = {0, 4},
        .rd_con = REG_NOT_PC,
        .rn_con = REG_NOT_PC,
        .rm_con = REG_NOT_SP | REG_NOT_PC,
        .has_s_bit = 1,
        .shift_type_bits = {4, 2},
        .shift_imm2_bits = {6, 2},
        .shift_imm3_bits = {12, 3},
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_MVN_IMM_T3] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .has_s_bit = 1,
        .feat = {.t32 = 1},
    },

    /* ---- thop_rev.c ---- */

    /* T1: <OP> <Rd>, <Rm>  —  16-bit, rd/rm low */
    [THOP_SHAPE_T16_REV] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {0, 3},
        .rm_place = {3, 3},
        .rd_con = REG_LOW_ONLY,
        .rm_con = REG_LOW_ONLY,
        .feat = {.t16 = 1},
    },

    /* T2: <OP> <Rd>, <Rm>  —  32-bit, rm duplicated at bits [19:16] */
    [THOP_SHAPE_T32_REV] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rm_place = {0, 4},
        .ra_place = {16, 4},
        .rd_con = REG_NOT_PC,
        .feat = {.t32 = 1},
    },

    /* T2 only (no 16-bit variant): rbit */
    [THOP_SHAPE_T32_RBIT] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rm_place = {0, 4},
        .ra_place = {16, 4},
        .rd_con = REG_NOT_PC,
        .feat = {.t32 = 1, .clz_rbit = 1},
    },

    /* ---- thop_shift_imm.c ---- */

    /* T1: <OP> <Rd>, <Rm>, #<imm5>  —  16-bit, rd/rm low, imm5 raw.
     *  LSL/LSR/ASR share this shape; shift type encoded in bits [12:11].
     */
    [THOP_SHAPE_T16_SHIFT_IMM] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {0, 3},
        .rm_place = {3, 3},
        .rd_con = REG_LOW_ONLY,
        .rm_con = REG_LOW_ONLY,
        .imm = {.kind = IMM_RAW, .width = 5},
        .imm_place = {6, 5},
        .shift_type_bits = {11, 2},
        .implicit_s = true,
        .feat = {.t16 = 1},
    },

    /* T3: MOV{S}.W <Rd>, <Rm>, <shift>  —  32-bit, shift immediate */
    [THOP_SHAPE_T32_SHIFT_IMM] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rm_place = {0, 4},
        .rd_con = REG_NOT_PC,
        .rm_con = REG_NOT_PC,
        .has_s_bit = 1,
        .shift_imm3_bits = {12, 3},
        .shift_imm2_bits = {6, 2},
        .shift_type_bits = {4, 2},
        .feat = {.t32 = 1},
    },

    /* ---- thop_shift_reg.c ---- */

    /* T1: <OP> <Rdn>, <Rm>  —  16-bit, all low, rd==rn, no shift field */
    [THOP_SHAPE_T16_SHIFT_REG] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {0, 3},
        .rm_place = {3, 3},
        .rd_con = REG_LOW_ONLY | REG_EQ_RN,
        .rn_con = REG_LOW_ONLY,
        .rm_con = REG_LOW_ONLY,
        .implicit_s = true,
        .feat = {.t16 = 1},
    },

    /* T2/T3 (32-bit): <OP>{S}.W <Rd>, <Rn>, <Rm>  —  no shift field, rd/rn/rm any (not PC/SP) */
    [THOP_SHAPE_T32_SHIFT_REG] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rn_place = {16, 4},
        .rm_place = {0, 4},
        .rd_con = REG_NOT_PC,
        .rn_con = REG_NOT_PC,
        .rm_con = REG_NOT_SP | REG_NOT_PC,
        .has_s_bit = 1,
        .feat = {.t32 = 1},
    },

    /* ---- thop_system.c ---- */
    [THOP_SHAPE_HINT_T16] = {
        .size = THOP_VARIANT_T16,
        .imm = {.kind = IMM_RAW, .width = 4},
        .imm_place = {4, 4},
        .feat = {.t16 = 1},
    },
    [THOP_SHAPE_HINT_T32] = {
        .size = THOP_VARIANT_T32,
        .imm = {.kind = IMM_RAW, .width = 4},
        .imm_place = {0, 4},
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_IMM8_T16] = {
        .size = THOP_VARIANT_T16,
        .imm = {.kind = IMM_RAW, .width = 8},
        .imm_place = {0, 8},
        .feat = {.t16 = 1},
    },
    [THOP_SHAPE_UDF_T16] = {
        .size = THOP_VARIANT_T16,
        .imm = {.kind = IMM_RAW, .width = 8},
        .imm_place = {0, 8},
        .feat = {.t16 = 1},
    },
    [THOP_SHAPE_UDF_T32] = {
        .size = THOP_VARIANT_T32,
        .imm = {.kind = IMM_RAW, .width = 12},
        .imm_place = {0, 12},
        .imm2_place = {16, 4},
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_CPS] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {4, 1},
        .imm = {.kind = IMM_RAW, .width = 1},
        .imm_place = {0, 1},
        .imm2_place = {1, 1},
        .feat = {.t16 = 1},
    },
    [THOP_SHAPE_BARRIER] = {
        .size = THOP_VARIANT_T32,
        .imm = {.kind = IMM_RAW, .width = 4},
        .imm_place = {0, 4},
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_NOARG_T32] = {
        .size = THOP_VARIANT_T32,
        .feat = {.t32 = 1},
    },
    [THOP_SHAPE_IT] = {
        .size = THOP_VARIANT_T16,
        .rd_place = {4, 4},
        .imm = {.kind = IMM_RAW, .width = 4},
        .imm_place = {0, 4},
        .feat = {.t16 = 1},
    },
    [THOP_SHAPE_CLZ] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rm_place = {16, 4},
        .feat = {.t32 = 1, .clz_rbit = 1},
    },

    /* ---- thop_tbb.c ---- */
    [THOP_SHAPE_TBB] = {
        .size = THOP_VARIANT_T32,
        .rn_place = {16, 4},
        .rm_place = {0, 4},
        .feat = {.t32 = 1, .tbb_tbh = 1},
    },
    [THOP_SHAPE_TT] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {8, 4},
        .rn_place = {16, 4},
        .feat = {.t32 = 1},
    },

    /* ---- thop_vfp.c ---- */
    [THOP_SHAPE_VFP_SP] = {
        .size = THOP_VARIANT_T32,
        .feat = {.t32 = 1, .vfp_sp = 1},
    },
    [THOP_SHAPE_VFP_DP] = {
        .size = THOP_VARIANT_T32,
        .feat = {.t32 = 1, .vfp_dp = 1},
    },

    /* 64-bit *data movement* (vldr/vstr/vmov of a d-register).  Only double
     * ARITHMETIC needs vfp_dp: a single-precision-only unit such as FPv5-SP-D16
     * still has 16 single registers addressable as d0-d7, and moving 64 bits
     * through them is legal (verified against arm-none-eabi-as, and it is how GCC
     * passes doubles under -mfloat-abi=hard -mfpu=fpv5-sp-d16).  Gating these on
     * vfp_dp would make the hard-float double ABI unencodable on that FPU. */
    [THOP_SHAPE_VFP_DP_MOVE] = {
        .size = THOP_VARIANT_T32,
        .feat = {.t32 = 1, .vfp_sp = 1},
    },
    [THOP_SHAPE_VMOVGPSP] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {12, 4},
        .imm2_place = {20, 1},
        .feat = {.t32 = 1, .vfp_sp = 1},
    },
    [THOP_SHAPE_VMOV2GPDP] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {12, 4},
        .rn_place = {16, 4},
        .imm2_place = {20, 1},
        .feat = {.t32 = 1, .vfp_sp = 1}, /* data movement — see SHAPE_VFP_DP_MOVE */
    },
    [THOP_SHAPE_VMRS] = {
        .size = THOP_VARIANT_T32,
        .rd_place = {12, 4},
        .feat = {.t32 = 1, .vfp_sp = 1},
    },
    [THOP_SHAPE_VCVT_FD] = {
        .size = THOP_VARIANT_T32,
        .feat = {.t32 = 1, .vfp_dp = 1},
    },
    [THOP_SHAPE_VCVT_DF] = {
        .size = THOP_VARIANT_T32,
        .feat = {.t32 = 1, .vfp_dp = 1},
    },
    [THOP_SHAPE_VCVT_FP_INT_SP] = {
        .size = THOP_VARIANT_T32,
        .imm = {.kind = IMM_RAW, .width = 4},
        .imm_place = {16, 4},
        .imm2_place = {7, 1},
        .puw_bits = {8, 1},
        .feat = {.t32 = 1, .vfp_sp = 1},
    },
    [THOP_SHAPE_VCVT_FP_INT_DP] = {
        .size = THOP_VARIANT_T32,
        .imm = {.kind = IMM_RAW, .width = 4},
        .imm_place = {16, 4},
        .imm2_place = {7, 1},
        .puw_bits = {8, 1},
        .feat = {.t32 = 1, .vfp_dp = 1},
    },
};
