/*
 *  ARMvX-m opcodes for TCC
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

#ifndef TARGET_DEFS_ONLY

#define USING_GLOBALS
#include "tcc.h"

#include "arm-thumb-opcodes.h"

thumb_opcode th_nop() {
  return (thumb_opcode){
      .size = 2,
      .opcode = 0xbf00,
  };
}

uint32_t th_packimm_10_11_0(uint32_t imm) {
  const uint32_t imm11 = (imm >> 1) & 0x7ff;
  const uint32_t imm10 = (imm >> 12) & 0x3ff;
  const uint32_t s = (imm >> 24) & 1;
  const uint32_t j1 = ~((imm >> 23) ^ s) & 1;
  const uint32_t j2 = ~((imm >> 22) ^ s) & 1;
  return (s << 26) | (imm10 << 16) | (j1 << 13) | (j2 << 11) | imm11;
}

uint32_t th_pack_const(uint32_t imm) {
  printf("th_pack_const imm: 0x%x\n", imm);
  // 00000000 00000000 00000000 abcdefgh
  if ((imm & 0xffffff00) == 0) {
    return imm;
  }
  // 00000000 abcdefgh 00000000 abcdefgh
  else if (!(imm & 0xff00ff00) && (imm >> 16) == (imm & 0xff)) {
    return (1 << 12) | (imm & 0xff);
  }
  // abcdefgh 00000000 abcdefgh 00000000
  else if (!(imm & 0x00ff00ff) && ((imm >> 16) & 0xff00) == (imm & 0xff00)) {
    return (2 << 12) | ((imm >> 8) & 0xff);
  }
  // abcdefgh abcdefgh abcdefgh abcdefgh
  else if ((imm & 0xffff) == ((imm >> 16) & 0xffff) &&
           ((imm >> 8) & 0xff) == (imm & 0xff)) {
    return (3 << 12) | (imm & 0xff);
  } else {
    for (uint32_t i = 8, j = 0; i <= 0x1F; i++, j++) {
      uint32_t mask = 0xFF000000 >> j;
      uint32_t one = 0x80000000 >> j;

      if ((imm & one) == one && (imm & ~mask) == 0) {
        uint32_t _i = i >> 4;
        uint32_t imm3 = (i >> 1) & 7;
        uint32_t a = i & 1;
        uint32_t bcdefgh = (imm >> (24 - j)) & 0x7f;

        return (_i << 26) | (imm3 << 12) | (a << 7) | bcdefgh;
      }
    }
  }
  return 0;
}

uint32_t th_encbranch_b_t3(uint32_t imm) {
  const uint32_t s = (imm >> 19) & 1;
  const uint32_t imm6 = (imm >> 11) & 0x3f;
  const uint32_t imm11 = imm & 0x7ff;
  const uint32_t j2 = (imm >> 18) & 1;
  const uint32_t j1 = (imm >> 17) & 1;
  const uint32_t a = (s << 10) | imm6;
  const uint32_t b = (j1 << 13) | (j2 << 11) | imm11;
  return (a << 16) | b;
}

uint32_t th_encbranch(int pos, int addr) {
  TRACE("th_encbranch pos: 0x%x, addr: 0x%x", pos, addr);
  return addr - pos - 4;
}

uint32_t th_encbranch_8(int pos, int addr) {
  addr = (addr - pos - 4) >> 1;
  if (addr >= 127 || addr < -128) {
    tcc_error("compiler_error: th_encbranch_8 too far address: %i\n", addr);
    return 0;
  }
  return addr & 0xff;
}

uint32_t th_encbranch_11(int pos, int addr) {
  addr = (addr - pos - 4) >> 1;
  if (addr >= 1023 || addr < -1024) {
    tcc_error("compiler_error: th_encbranch_11 too far address: %i\n", addr);
    return 0;
  }
  return addr & 0x7ff;
}

uint32_t th_encbranch_20(int pos, int addr) {
  addr = (addr - pos - 4) >> 1;
  TRACE("th_encbranch_20 pos %x addr %x\n", pos, addr);
  return addr;
}

uint32_t th_encbranch_24(int pos, int addr) {
  addr = (addr - pos - 4) >> 1;
  TRACE("th_encbranch_24 pos %x addr %x\n", pos, addr);
  return addr;
}

thumb_opcode th_bx_reg(uint16_t rm) {
  return (thumb_opcode){
      .size = 2,
      .opcode = (0x4700 | ((rm & 0xf) << 3)),
  };
}

thumb_opcode th_bl_t1(uint32_t imm) {
  const uint32_t packed = th_packimm_10_11_0(imm) | 0xF000D000;
  printf("th_bl_t1 imm: 0x%x packed: 0x%x\n", imm, packed);
  return (thumb_opcode){
      .size = 4,
      .opcode = packed,
  };
}

thumb_opcode th_blx_reg(uint16_t rm) {
  return (thumb_opcode){
      .size = 2,
      .opcode = (0x4780 | (rm << 3)),
  };
}

thumb_opcode th_b_t1(uint32_t cond, uint32_t imm8) {
  return (thumb_opcode){
      .size = 2,
      .opcode = 0xd000 | ((cond & 0xf) << 8) | (imm8 & 0xff),
  };
}

thumb_opcode th_b_t2(int32_t imm11) {
  const int32_t i = imm11 >> 1;
  if (i < 1023 && i > -1024 && !(imm11 & 1)) {
    return (thumb_opcode){
        .size = 2,
        .opcode = (0xe000 | (i & 0x7ff)),
    };
  }
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_b_t3(uint32_t op, uint32_t imm) {
  const uint32_t enc = th_encbranch_b_t3(imm);
  return (thumb_opcode){
      .size = 4,
      .opcode = (0xf0008000 | (op << 22) | enc),
  };
}

thumb_opcode th_b_t4(int32_t imm) {
  uint32_t packed = 0;
  if (imm > 16777215 || imm < -16777215)
    tcc_error("compiler_error: th_b_t4 too far address: 0x%x\n", imm);

  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf0009000 | th_packimm_10_11_0(imm),
  };
}

// all t32 arch
thumb_opcode th_mov_reg(uint16_t rd, uint16_t rm) {
  const uint16_t D = (rd >> 3) & 1;
  return (thumb_opcode){
      .size = 2,
      .opcode = (0x4600 | (D << 7) | (rm << 3) | (rd & 0x7)),
  };
}

thumb_opcode th_mov_imm(uint16_t rd, uint32_t imm, flags_behaviour setflags,
                        enforce_encoding encoding) {
  if (rd <= 7 && imm >= 0 && imm <= 255 && setflags != FLAGS_BEHAVIOUR_BLOCK &&
      encoding != ENFORCE_ENCODING_32BIT) {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x2000 | (rd << 8) | imm,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M

  if (rd != R_SP && rd != R_PC && encoding != ENFORCE_ENCODING_16BIT) {
    const uint32_t enc = th_pack_const(imm);
    printf("th_mov_imm: enc: 0x%x\n", enc);
    const uint32_t s = (setflags == FLAGS_BEHAVIOUR_SET) ? 1 : 0;
    if (enc)
      return (thumb_opcode){
          .size = 4,
          .opcode = 0xf04f0000 | enc | ((rd & 0xf) << 8) | (s << 20),
      };
  }

  if (imm >= 0 && imm <= 0xffff && rd != R_SP && rd != R_PC &&
      setflags != FLAGS_BEHAVIOUR_SET && encoding != ENFORCE_ENCODING_16BIT) {
    const uint16_t i = (imm >> 11) & 1;
    const uint32_t imm4 = (imm >> 12) & 0xf;
    const uint32_t imm3 = (imm >> 8) & 0x7;
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf2400000 | (i << 26) | (imm4 << 16) | (imm3 << 12) |
                  (rd << 8) | (imm & 0xff),
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_generic_op_imm_with_status(uint16_t op, uint16_t rd,
                                           uint16_t rn, uint32_t imm,
                                           flags_behaviour setflags) {
#ifndef TCC_TARGET_ARM_ARCHV6M
  const uint32_t packed = th_pack_const(imm);
  if (packed || imm == 0) {
    const uint32_t A = packed >> 16;
    const uint32_t B = packed & 0xffff;
    return (thumb_opcode){
        .size = 4,
        .opcode =
            ((op | ((setflags == FLAGS_BEHAVIOUR_SET) << 4) | rn | A) << 16) |
            (rd << 8 | B),
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_generic_op_imm(uint16_t op, uint16_t rd, uint16_t rn,
                               uint32_t imm) {
  return th_generic_op_imm_with_status(op, rd, rn, imm,
                                       FLAGS_BEHAVIOUR_NOT_IMPORTANT);
}

thumb_opcode th_add_reg(uint16_t rd, uint16_t rn, uint16_t rm) {
  if ((rd == R_PC) && (rm == R_PC)) {
    tcc_error("compiler_error: 'th_add_reg', PC can't be used as rdn and rm\n");
  }
  if (rd == rn) {
    // T2
    const uint16_t DN = (rd >> 3) & 1;
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x4400 | (DN << 7) | ((rm & 0xf) << 3) | (rd & 0x7),
    };
  } else if (rm < 8 && rd < 8 && rn < 8) {
    // T1
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x1800 | (rm << 6) | (rn << 3) | (rd),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else {
    return (thumb_opcode){
        .size = 4,
        .opcode = (0xeb00 | rn) << 16 | (rd << 8) | (1 << 20) | rm,

    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_add_imm(uint16_t rd, uint16_t rn, uint32_t imm) {
  if (rd == rn && rd < 8 && imm <= 255) {
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x3000 | (rd << 8) | imm),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rn != R_SP && rn != R_PC && rd != R_PC && rd != R_SP &&
           imm <= 4095) {
    const uint16_t i = (imm >> 11) & 1;
    const uint32_t imm3 = (imm >> 8) & 7;
    uint32_t op = (0xf200 | (i << 10) | rn) << 16;
    op |= ((imm3 << 12) | (rd << 8) | (imm & 0xff));
    return (thumb_opcode){
        .size = 4,
        .opcode = op,
    };
  } else if (rd != R_SP && rn != R_PC) {
    return th_generic_op_imm(0xf100, rd, rn, imm);
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_bic_imm(uint16_t rd, uint16_t rn, uint32_t imm) {
#ifndef TCC_TARGET_ARM_ARCHV6M
  if (rd != R_SP && rd != R_PC && rn != R_SP && rd != R_PC) {
    const uint32_t packed = th_pack_const(imm);
    if (packed || imm == 0) {
      return (thumb_opcode){
          .size = 4,
          .opcode = 0xf0200000 | packed | (rn << 16) | (rd << 8),
      };
    }
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_and_imm(uint16_t rd, uint16_t rn, uint32_t imm) {
  thumb_opcode op = th_generic_op_imm(0xf000, rd, rn, imm);
  return op.size != 0 ? op : th_bic_imm(rd, rn, ~imm);
}

thumb_opcode th_and_reg(uint16_t rd, uint16_t rn, uint16_t rm) {
  if (rd == rn && rm < 8 && rn < 8) {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x4000 | (rm << 3) | rd,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rn != R_SP && rn != R_PC && rm != R_SP && rm != R_PC) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xea000000 | (rn << 16) | (rd << 8) | rm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_xor_reg(uint16_t rd, uint16_t rn, uint16_t rm) {
  if (rd == rn && rm < 8 && rn < 8) {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x4040 | (rm << 3) | rd,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xea800000 | (rn << 16) | (rd << 8) | rm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_xor_imm(uint16_t rd, uint16_t rn, uint32_t imm) {
  return th_generic_op_imm(0xf080, rd, rn, imm);
}

thumb_opcode th_rsb_reg(uint16_t rd, uint16_t rn, uint16_t rm) {
#ifndef TCC_TARGET_ARM_ARCHV6M
  if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC && rm != R_SP &&
      rn != R_SP) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xebc00000 | (rn << 16) | (rd << 8) | rm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_sub_reg(uint16_t rd, uint16_t rn, uint16_t rm) {
  if (rd < 8 && rm < 8 && rn < 8) {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x1a00 | (rm << 6) | (rn << 3) | rd,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xeba00000 | (rn << 16) | (rd << 8) | rm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_adc_reg(uint16_t rd, uint16_t rn, uint16_t rm) {
  if (rd == rn && rm < 8 && rn < 8) {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x4140 | (rm << 3) | rd,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC && rm != R_SP &&
           rm != R_PC) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xeb400000 | (rn << 16) | (rd << 8) | rm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_adc_imm(uint16_t rd, uint16_t rn, uint32_t imm) {
  if (rn != R_SP && rn != R_PC && rd != R_SP && rn != R_PC) {
    return th_generic_op_imm(0xf140, rd, rn, imm);
  }
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_sbc_imm(uint16_t rd, uint16_t rn, uint32_t imm) {
  if (rn != R_SP && rn != R_PC && rd != R_SP && rn != R_PC) {
    return th_generic_op_imm(0xf160, rd, rn, imm);
  }
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_orr_imm(uint16_t rd, uint16_t rn, uint32_t imm) {
  if (rn != R_SP && rd != R_SP && rn != R_PC) {
    return th_generic_op_imm(0xf040, rd, rn, imm);
  }
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_sbc_reg(uint16_t rd, uint16_t rn, uint16_t rm) {
  if (rd == rn && rm < 8 && rn < 8) {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x4180 | (rm << 3) | rd,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC && rm != R_SP &&
           rm != R_PC) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xeb700000 | (rn << 16) | (rd << 8) | rm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_cmp_reg(uint16_t rn, uint16_t rm) {
  if (rm < 8 && rn < 8) {
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x4280 | (rm << 3) | rn),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (!(rm < 8 && rn < 8) && rm != R_PC && rn != R_PC) {
    const uint16_t N = (rn >> 3) & 0x1;
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x4500 | (N << 7) | (rm << 3) | (rn & 0x7)),
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_orr_reg(uint16_t rd, uint16_t rn, uint16_t rm) {
  if (rd == rn && rm < 8 && rn < 8) {
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x4300 | (rm << 3) | rd),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rd != R_PC && rn != R_PC && rm != R_SP && rm != R_PC) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xea400000 | (rn << 16) | (rd << 8) | rm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_sub_imm(uint16_t rd, uint16_t rn, uint32_t imm) {

  if (rd < 8 && rn < 8 && imm <= 7) {
    // T1
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x1e00 | (imm << 6) | (rn << 3) | rd),
    };
  } else if (rd == rn && imm <= 255 && rd < 8) {
    // T2
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x3800 | (rd << 8) | imm),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rd != R_PC && imm <= 0xfff) {
    // T4
    const uint16_t i = imm >> 11;
    const uint32_t imm3 = (imm >> 8) & 0x7;
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf2a00000 | (i << 26) | (rn << 16) | (imm3 << 12) |
                  (rd << 8) | (imm & 0xff),
    };
  } else if (rd != 13 && rd != 15) {
    const uint32_t enc = th_pack_const(imm);
    if (enc || imm == 0) {
      return (thumb_opcode){
          .size = 4,
          .opcode = 0xf1a00000 | (rn << 16) | (rd << 8) | enc,
      };
    }
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_push(uint16_t regs) {
  // T1 encoding R0-R7 + LR only, all armv-m
  // (T2 in armv8-m - inconsistent naming in reference manual)
  if (!(regs & 0xbf00)) {
    const uint16_t lr = (regs >> 14) & 1;
    return (thumb_opcode){
        .size = 2,
        .opcode = (0xb400 | (lr << 8) | (regs & 0xff)),
    };
  }
// T2 encoding R0-R12 + LR only, > armv7-m
// (T1 in armv8-m - inconsistent naming in reference manual)
#if defined(TCC_TARGET_ARM_ARCHV8M) || defined(TCC_TARGET_ARM_ARCHV7M)
  if (!(regs & 0xa000)) {
    return (thumb_opcode){
        .size = 4,
        .opcode = (0xe92d << 16 | regs),
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

int th_ldr_literal_estimate(uint16_t rt, uint32_t imm) {
  if (rt < 8 && !(imm & 3) && imm <= 0x3ff)
    return 2;
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (imm <= 0xfff)
    return 4;
#endif
  return 0;
}

thumb_opcode th_ldrsh_imm(uint32_t rt, uint32_t rn, uint32_t imm,
                          uint32_t puw) {
#ifndef TCC_TARGET_ARM_ARCHV6M
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rt != R_SP && imm <= 4095 && puw == 6) {
    uint32_t ins = (0xf9b0 | ((rn & 0xf))) << 16;
    ins |= (((rt & 0xf) << 12) | imm);
    return (thumb_opcode){
        .size = 4,
        .opcode = ins,
    };
  } else if (rt != R_SP && imm <= 255) {
    uint32_t ins = (0xf930 | (rn & 0xf)) << 16;
    ins |= (0x0800 | ((rt & 0xf) << 12) | (puw << 8) | imm);

    return (thumb_opcode){
        .size = 4,
        .opcode = ins,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_ldrsh_reg(uint32_t rt, uint32_t rn, uint32_t rm) {
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rm < 8 && rt < 8 && rn < 8) {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x5e00 | (rm << 6) | (rn << 3) | rt,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rt != R_SP && rm != R_SP && rn != R_SP) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf9300000 | (rn << 16) | (rt << 12) | rm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_ldrh_imm(uint16_t rt, uint16_t rn, uint32_t imm, uint32_t puw) {
  // T1 encoding, on armv6-m this one is the only one available
  if (puw == 6 && rn < 8 && rt < 8 && imm <= 62 && !(imm & 1)) {
    // imm[0] is enforced to be 0, and sould be divided by 2, thus offset is 5
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x8800 | (imm << 5) | (rn << 3) | rt),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (puw == 6 && rt != R_SP && imm <= 4095) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf8b00000 | (rn << 16) | (rt << 12) | imm,
    };
  } else if (rt != R_SP && imm <= 255) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf8300800 | (rn << 16) | (rt << 12) | (puw << 8) | imm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_ldrh_reg(uint32_t rt, uint32_t rn, uint32_t rm) {
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rm < 8 && rt < 8 && rn < 8) {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x5a00 | (rm << 6) | (rn << 3) | rt,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rt != R_SP && rm != R_SP && rm != R_PC) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf8300000 | (rn << 16) | (rt << 12) | rm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_ldrsb_imm(uint32_t rt, uint32_t rn, uint32_t imm,
                          uint32_t puw) {
#ifndef TCC_TARGET_ARM_ARCHV6M
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rt != R_SP && imm <= 4095 && puw == 6) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf9900000 | (rn << 16) | (rt << 12) | imm,
    };
  } else if (rt != R_SP && imm <= 255) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf9100800 | (rn << 16) | (rt << 12) | (puw << 8) | imm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_ldrsb_reg(uint32_t rt, uint32_t rn, uint32_t rm) {
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rm < 8 && rt < 8 && rn < 8) {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x5600 | (rm << 6) | (rn << 3) | rt,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rt != R_SP && rn != R_SP && rm != R_SP) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf9100000 | (rn << 16) | (rt << 12) | rm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_ldrb_imm(uint16_t rt, uint16_t rn, uint32_t imm, uint32_t puw) {
  // T1 encoding, on armv6-m this one is the only one available
  if (puw == 6 && rn < 8 && rt < 8 && imm <= 62 && !(imm & 1)) {
    // imm[0] is enforced to be 0, and sould be divided by 2, thus offset is 5
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x7800 | (imm << 5) | (rn << 3) | rt,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (puw == 6 && rt != R_SP && imm <= 4095) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf8900000 | (rn << 16) | (rt << 12) | imm,
    };
  } else if (rt != R_SP && imm <= 255) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf8100800 | (rn << 16) | (rt << 12) | (puw << 8) | imm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_ldrb_reg(uint32_t rt, uint32_t rn, uint32_t rm) {
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rm < 8 && rt < 8 && rn < 8) {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x5c00 | (rm << 6) | (rn << 3) | rt,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rt != R_SP && rm != R_SP && rm != R_PC) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf8100000 | (rn << 16) | (rt << 12) | rm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_ldr_imm(uint32_t rt, uint32_t rn, uint32_t imm, uint32_t puw) {
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (puw == 6 && rn < 8 && rt < 8 && imm <= 124 && !(imm & 3)) {
    // imm[0] is enforced to be 0, and sould be divided by 4, thus offset is 4
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x6800 | (imm << 4) | (rn << 3) | rt,
    };
  } else if (puw == 6 && rn == R_SP && rt < 8 && imm <= 1020) {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x9800 | (rt << 8) | (imm >> 2),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (puw == 6 && imm <= 4095) {
    uint32_t ins = (0xf8d0 | (rn & 0xf)) << 16;
    ins |= (rt << 12) | imm;
    return (thumb_opcode){
        .size = 4,
        .opcode = ins,
    };
  } else if (imm <= 255) {
    uint32_t ins = (0xf850 | (rn & 0xf)) << 16;
    ins |= (0x0800 | ((rt & 0xf) << 12) | ((puw & 0x7) << 8) | imm);
    return (thumb_opcode){
        .size = 4,
        .opcode = ins,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_ldr_reg(uint32_t rt, uint32_t rn, uint32_t rm) {
  if (rm < 8 && rt < 8 && rn < 8) {
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x5800 | (rm << 6) | (rn << 3) | rt),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rt != R_SP && rm != R_SP && rm != R_PC) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf8500000 | (rn << 16) | (rt << 12) | rm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_ldr_literal(uint16_t rt, uint32_t imm, uint32_t add) {
  if (rt < 8 && imm <= 1020) {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x4800 | (rt << 8) | imm >> 2,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rt != R_PC && imm <= 0xffff) {
    uint32_t ins = (0xf85f | ((add & 1) << 7)) << 16;
    ins |= (rt & 0xf) << 12 | imm;
    return (thumb_opcode){
        .size = 4,
        .opcode = ins,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_pop(uint16_t regs) {
  // T1 encoding R0-R7 + PC only, all armv-m
  // (T2 in armv8-m - inconsistent naming in reference manual)
  if (!(regs & 0x7f00)) {
    const uint16_t pc = (regs >> 15) & 1;
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xbc00 | (pc << 8) | (regs & 0xff),
    };
  }
// T2 encoding R0-R12 + PC + LR, > armv7-m
// (T1 in armv8-m - inconsistent naming in reference manual)
#if defined(TCC_TARGET_ARM_ARCHV8M) || defined(TCC_TARGET_ARM_ARCHV7M)
  if (!(regs & 0x2000)) {
    return (thumb_opcode){
        .size = 4,
        .opcode = (0xe8bd << 16) | regs,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

// STR
thumb_opcode th_strh_imm(uint16_t rt, uint16_t rn, uint32_t imm, uint16_t puw) {
  // T1 encoding, on armv6-m this one is the only one available
  if (puw == 6 && rn < 8 && rt < 8 && imm <= 62 && !(imm & 1)) {
    // imm[0] is enforced to be 0, and sould be divided by 2, thus offset is 5
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x8000 | (imm << 5) | (rn << 3) | rt),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (puw == 6 && rt != R_SP && imm <= 4095) {
    return (thumb_opcode){
        .size = 4,
        .opcode = (0xf8a00000 | (rn << 16) | (rt << 12) | imm),
    };
  } else if (rt != R_SP && imm <= 255) {
    return (thumb_opcode){
        .size = 4,
        .opcode =
            0xf8200800 | (rn << 16) | (rt << 12) | ((puw & 0x7) << 8) | imm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_strh_reg(uint32_t rt, uint32_t rn, uint32_t rm) {
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rm < 8 && rt < 8 && rn < 8) {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x5200 | (rm << 6) | (rn << 3) | rt,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rt != R_SP && rm != R_SP && rm != R_PC) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf8200000 | (rn << 16) | (rt << 12) | rm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_strb_imm(uint16_t rt, uint16_t rn, uint32_t imm, uint16_t puw) {
  // T1 encoding, on armv6-m this one is the only one available
  if (puw == 6 && rn < 8 && rt < 8 && imm <= 62 && !(imm & 1)) {
    // imm[0] is enforced to be 0, and sould be divided by 2, thus offset is 5
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x7000 | (imm << 5) | (rn << 3) | rt,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (puw == 6 && rt != R_SP && imm <= 4095) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf8800000 | (rn << 16) | (rt << 12) | imm,
    };
  } else if (rt != R_SP && imm <= 255) {
    return (thumb_opcode){
        .size = 4,
        .opcode =
            0xf8000800 | (rn << 16) | (rt << 12) | ((puw & 0x7) << 8) | imm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_strb_reg(uint32_t rt, uint32_t rn, uint32_t rm) {
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rm < 8 && rt < 8 && rn < 8) {
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x5400 | (rm << 6) | (rn << 3) | rt),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rt != R_SP && rm != R_SP && rm != R_PC) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf8000000 | (rn << 16) | (rt << 12) | rm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_str_imm(uint32_t rt, uint32_t rn, uint32_t imm, uint32_t puw) {
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (puw == 6 && rn < 8 && rt < 8 && imm <= 124 && !(imm & 3)) {
    // imm[0] is enforced to be 0, and sould be divided by 4, thus offset is 4
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x6000 | (imm << 4) | (rn << 3) | rt),
    };
  } else if (puw == 6 && rn == R_SP && rt < 8 && imm <= 1020) {
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x9000 | (rt << 8) | (imm >> 2)),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (puw == 6 && imm <= 4095) {
    return (thumb_opcode){
        .size = 4,
        .opcode = (0xf8c00000 | (rn << 16) | (rt << 12) | imm),
    };
  } else if (imm <= 255) {
    return (thumb_opcode){
        .size = 4,
        .opcode =
            (0xf8400800 | (rn << 16) | (rt << 12) | ((puw & 0x7) << 8) | imm),
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_str_reg(uint32_t rt, uint32_t rn, uint32_t rm) {
  if (rm < 8 && rt < 8 && rn < 8) {
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x5000 | (rm << 6) | (rn << 3) | rt),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rt != R_SP && rm != R_SP && rm != R_PC) {
    return (thumb_opcode){
        .size = 4,
        .opcode = (0xf8400000 | (rn << 16) | (rt << 12) | rm),
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_mul(uint16_t rd, uint16_t rn, uint16_t rm) {
  if (rd == rm && rd < 8 && rn < 8) {
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x4340 | (rn << 3) | rm),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else {
    return (thumb_opcode){
        .size = 4,
        .opcode = (0xfb00f000 | (rn << 16) | (rd << 8) | rm),
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_umull(uint32_t rdlo, uint32_t rdhi, uint16_t rn, uint16_t rm) {
#ifndef TCC_TARGET_ARM_ARCHV6M
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xfba00000 | (rn << 16) | (rdlo << 12) | (rdhi << 8) | rm,
  };
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_udiv(uint16_t rd, uint16_t rn, uint16_t rm) {
#ifndef TCC_TARGET_ARM_ARCHV6M
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xfbb0f0f0 | (rn << 16) | (rd << 8) | rm,
  };
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_sdiv(uint16_t rd, uint16_t rn, uint16_t rm) {
#ifndef TCC_TARGET_ARM_ARCHV6M
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xfb90f0f0 | (rn << 16) | (rd << 8) | rm,
  };
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_add_sp_imm(uint16_t rd, uint32_t imm) {
  // T1 on all armv-m
  if (rd < 8 && imm <= 1020 && !(imm & 0x3)) {
    return (thumb_opcode){
        .size = 2,
        .opcode = (0xa800 | (rd << 8) | (imm >> 2)),
    };
  }
  // T2 on all armv-m
  else if (rd == R_SP && imm <= 508 && !(imm & 0x3)) {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xb000 | (imm >> 2),
    };
  }
#if !defined(TCC_TARGET_ARM_ARCHV6M)
  // T3
  else if (rd != R_PC && imm <= 4095) {
    const uint16_t i = (imm >> 11) & 1;
    const uint32_t imm3 = (imm >> 8) & 7;
    return (thumb_opcode){
        .size = 4,
        .opcode =
            0xf20d0000 | (i << 26) | (imm3 << 12) | (rd << 8) | (imm & 0xff),
    };
  } else if (rd != R_PC) {
    const uint32_t enc = th_pack_const(imm);
    if (enc || imm == 0) {
      return (thumb_opcode){
          .size = 4,
          .opcode = 0xf10d0000 | enc | (rd << 8),
      };
    }
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_add_sp_reg(uint16_t rdm) {
  return (thumb_opcode){
      .size = 2,
      .opcode = 0x4485 | (rdm << 3),
  };
}

thumb_opcode th_rsb_imm(uint16_t rd, uint16_t rn, uint32_t imm,
                        flags_behaviour setflags) {
  if (rd < 8 && rn < 8 && imm == 0 && setflags == FLAGS_BEHAVIOUR_SET) {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x4240 | (rn << 3) | rd,
    };
  } else if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC) {
    return th_generic_op_imm_with_status(0xf1c0, rd, rn, imm, setflags);
  }
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_shift_armv7m(uint16_t rd, uint16_t rm, uint32_t imm,
                             uint32_t type) {
  const uint32_t imm3 = (imm >> 2) & 7;
  const uint32_t imm2 = imm & 0x3;
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xea4f0000 | (imm3 << 12) | (rd << 8) | (imm2 << 6) |
                (type << 4) | rm,
  };
}

thumb_opcode th_lsl_reg(uint16_t rd, uint16_t rn, uint16_t rm) {
  if (rd == rn && rm < 8 && rn < 8) {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x4080 | (rm << 3) | rd,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC && rm != R_SP &&
           rm != R_PC) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xfa00f000 | (rn << 16) | (rd << 8) | rm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_lsl_imm(uint16_t rd, uint16_t rm, uint32_t imm) {
  if (rm < 8 && rd < 8) {
    return (thumb_opcode){
        .size = 2,
        .opcode = ((imm << 6) | (rm << 3) | rd),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (imm >= 1 && imm <= 31) {
    return th_shift_armv7m(rd, rm, imm, 0);
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_lsr_reg(uint16_t rd, uint16_t rn, uint16_t rm) {
  if (rd == rn && rm < 8 && rn < 8) {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x40c0 | (rm << 3) | rd,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC && rm != R_SP &&
           rm != R_PC) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xfa20f000 | (rn << 16) | (rd << 8) | rm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_lsr_imm(uint16_t rd, uint16_t rm, uint32_t imm) {
  if (rm < 8 && rd < 8) {
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x0800 | (imm << 6) | (rm << 3) | rd),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (imm >= 1 && imm <= 31) {
    return th_shift_armv7m(rd, rm, imm, 1);
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_asr_reg(uint16_t rd, uint16_t rn, uint16_t rm) {
  if (rd == rn && rm < 8 && rn < 8) {
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x4100 | (rm << 3) | rd),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC && rm != R_SP &&
           rm != R_PC) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xfa40f000 | (rn << 16) | (rd << 8) | rm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_asr_imm(uint16_t rd, uint16_t rm, uint32_t imm) {
  if (rm < 8 && rd < 8) {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x1000 | (imm << 6) | (rm << 3) | rd,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (imm >= 1 && imm <= 31) {
    return th_shift_armv7m(rd, rm, imm, 2);
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_cmp_imm(uint16_t rn, uint32_t imm, enforce_encoding encoding) {
  if (rn < 8 && imm <= 255 && encoding != ENFORCE_ENCODING_32BIT) {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x2800 | (rn << 8) | imm,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else {
    const uint32_t packed = th_pack_const(imm);
    if (packed || imm == 0) {
      return (thumb_opcode){
          .size = 4,
          .opcode = 0xf1b00f00 | (rn << 16) | packed,
      };
    }
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

// VFP instructions

thumb_opcode th_vpush(uint32_t regs) {
  // single precision floating point registers for now
  // TODO: add support for hardfloat config
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xed2d0a00 | regs,
  };
}

thumb_opcode th_vpop(uint32_t regs) {
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xecbd0a00 | regs,
  };
}

thumb_opcode th_vmov_register(uint16_t vd, uint16_t vm) {
  if (vd <= 0x1f && vm <= 0x1f) {
    const uint16_t d = vd & 1;
    const uint16_t m = vm & 1;
    vd >>= 1;
    vm >>= 1;
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xeeb00a40 | (d << 22) | (vd << 12) | (m << 5) | vm,
    };
  }
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_vldr(uint32_t rn, uint32_t vd, uint32_t add,
                     uint32_t is_doubleword, uint32_t imm) {
  const uint32_t D = (vd >> 4) & 1;
  if (imm > 1020 || (imm & 0x3)) {
    tcc_error("compiler_error: 'th_vldr' imm is outside of range: 0x%x, max "
              "value: 0xff\n",
              imm);
  }
  if (is_doubleword) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xed100b00 | (D << 22) | ((add & 1) << 23) | (rn << 16) |
                  (vd << 12) | (imm > 2),
    };
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xed100a00 | ((add & 1) << 23) | (D << 22) | (rn << 16) |
                (vd << 12) | (imm > 2),
  };
}

thumb_opcode th_vstr(uint32_t rn, uint32_t vd, uint32_t add,
                     uint32_t is_doubleword, uint32_t imm) {
  const uint32_t D = (vd >> 4) & 1;
  if (imm > 1020 || (imm & 0x3)) {
    tcc_error("compiler_error: 'th_vstr' imm is outside of range: 0x%x, max "
              "value: 0xff\n",
              imm);
  }
  if (is_doubleword) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xed000b00 | (D << 22) | ((add & 1) << 23) | (rn << 16) |
                  (vd << 12) | (imm > 2),

    };
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xed000a00 | (D << 22) | ((add & 1) << 23) | (rn << 16) |
                (vd << 12) | (imm > 2),
  };
}

// move between core general purpose register and single precision floating
// point register
thumb_opcode th_vmov_gp_sp(uint16_t rt, uint16_t sn, uint16_t to_arm_register) {
  const uint16_t N = (sn >> 4) & 1;
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xee000a10 | (to_arm_register << 20) | (sn << 16) | (rt << 12) |
                (N << 7),
  };
}

// move between two general purpose registers and one doubleword register
thumb_opcode th_vmov_2gp_dp(uint16_t rt, uint16_t rt2, uint16_t dm,
                            uint16_t to_arm_register) {
  const uint16_t M = (dm >> 4) & 1;
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xec400b10 | (to_arm_register << 20) | (rt2 << 16) |
                (rt << 12) | (M << 5) | dm,
  };
}

thumb_opcode gen_th_sub_sp_imm(uint16_t rd, uint32_t imm) {
  // T1 encoding
  if (rd == R_SP && imm <= 508 && !(imm & 0x3)) {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xb080 | (imm >> 2),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  // T3 encoding
  else if (imm <= 4095 && rd != R_PC) {
    const uint32_t i = (imm >> 11) & 1;
    const uint32_t imm3 = (imm >> 8) & 0x7;
    return (thumb_opcode){
        .size = 4,
        .opcode =
            0xf2ad0000 | (i << 26) | (imm3 << 12) | (rd << 8) | (imm & 0xff),
    };
  } else if (rd != R_PC) {
    const uint32_t enc = th_pack_const(imm);
    if (enc || imm == 0) {
      return (thumb_opcode){
          .size = 4,
          .opcode = 0xf1ad0000 | (rd << 8) | enc,
      };
    }
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

uint32_t th_sub_sp_imm_estimate(uint16_t rd, uint32_t imm) {
  // T1 encoding
  if (rd == R_SP && imm <= 508 && !(imm & 0x3)) {
    return 2;
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  // T3 encoding
  else if (imm <= 4095 && rd != R_PC) {
    return 4;
  } else if (rd != R_PC) {
    return 4;
  }
#endif
  return 0;
}

thumb_opcode th_sub_sp_imm(uint16_t rd, uint32_t imm) {
  return gen_th_sub_sp_imm(rd, imm);
}

thumb_opcode th_vmrs(uint16_t rt) {
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xeef10a10 | (rt << 12),
  };
}

thumb_opcode th_vcvt_float_to_double(uint32_t vd, uint32_t vm) {
  return (thumb_opcode){
      .size = 4,
      .opcode = (0xeeb70ac0 | (vd << 12) | vm),
  };
}

thumb_opcode th_vcvt_double_to_float(uint32_t vd, uint32_t vm) {
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xeeb70bc0 | (vd << 12) | vm,
  };
}

thumb_opcode th_vcvt_fp_int(uint32_t vd, uint32_t vm, uint32_t opc, uint32_t sz,
                            uint32_t op) {
  return (thumb_opcode){
      .size = 4,
      .opcode =
          0xeeb80a40 | (opc << 16) | (vd << 12) | (sz << 8) | (op << 7) | vm,
  };
}

thumb_opcode th_it(uint16_t cond, uint16_t mask) {
  printf("Generating IT instruction: cond=%u, mask=0x%x\n", cond, mask);
  return (thumb_opcode){
      .size = 2,
      .opcode = 0xbf00 | (cond << 4) | (mask & 0xf),
  };
}

thumb_opcode th_svc(uint32_t imm) {
  if (imm <= 0xff) {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xdf00 | imm,
    };
  }
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

#endif // TARGET_DEFS_ONLY
