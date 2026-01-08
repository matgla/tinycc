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

static const char *th_reg_name(unsigned r)
{
  static const char *names[] = {"r0", "r1", "r2",  "r3",  "r4",  "r5", "r6", "r7",
                                "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"};
  if (r < (sizeof(names) / sizeof(names[0])))
    return names[r];
  return "r?";
}

static const char *th_cond_name(unsigned cond)
{
  static const char *conds[] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                                "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};
  return cond < 16 ? conds[cond] : "??";
}

static const char *th_shift_name(thumb_shift_type t)
{
  switch (t)
  {
  case THUMB_SHIFT_NONE:
    return "";
  case THUMB_SHIFT_LSL:
    return "lsl";
  case THUMB_SHIFT_LSR:
    return "lsr";
  case THUMB_SHIFT_ASR:
    return "asr";
  case THUMB_SHIFT_ROR:
    return "ror";
  case THUMB_SHIFT_RRX:
    return "rrx";
  default:
    return "?";
  }
}

static void th_trace_regset(uint16_t regs)
{
  int first = 1;
  THOP_TRACE("{");
  for (unsigned r = 0; r < 16; ++r)
  {
    if (regs & (1u << r))
    {
      THOP_TRACE("%s%s", first ? "" : ",", th_reg_name(r));
      first = 0;
    }
  }
  THOP_TRACE("}");
}

static void th_trace_shift_suffix(thumb_shift shift)
{
  if (shift.type == THUMB_SHIFT_NONE)
    return;
  if (shift.type == THUMB_SHIFT_RRX)
  {
    THOP_TRACE(", rrx");
    return;
  }
  if (shift.mode == THUMB_SHIFT_REGISTER)
    THOP_TRACE(", %s %s", th_shift_name(shift.type), th_reg_name(shift.value));
  else
    THOP_TRACE(", %s #%u", th_shift_name(shift.type), (unsigned)shift.value);
}

thumb_opcode th_nop(thumb_enforce_encoding encoding)
{
  if (encoding == ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf3af8000,
    };
  }
  return (thumb_opcode){
      .size = 2,
      .opcode = 0xbf00,
  };
}

thumb_opcode th_sev(thumb_enforce_encoding encoding)
{
  if (encoding == ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf3af8004,
    };
  }
  return (thumb_opcode){
      .size = 2,
      .opcode = 0xbf40,
  };
}

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
  const uint32_t i = (imm >> 9) & 1;
  return (i << 26) | (imm3 << 12) | imm8;
}

uint32_t th_pack_const(uint32_t imm)
{
  // 00000000 00000000 00000000 abcdefgh
  if ((imm & 0xffffff00) == 0)
  {
    return imm;
  }
  // 00000000 abcdefgh 00000000 abcdefgh
  else if (!(imm & 0xff00ff00) && (imm >> 16) == (imm & 0xff))
  {
    return (1 << 12) | (imm & 0xff);
  }
  // abcdefgh 00000000 abcdefgh 00000000
  else if (!(imm & 0x00ff00ff) && ((imm >> 16) & 0xff00) == (imm & 0xff00))
  {
    return (2 << 12) | ((imm >> 8) & 0xff);
  }
  // abcdefgh abcdefgh abcdefgh abcdefgh
  else if ((imm & 0xffff) == ((imm >> 16) & 0xffff) && ((imm >> 8) & 0xff) == (imm & 0xff))
  {
    return (3 << 12) | (imm & 0xff);
  }
  else
  {
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

        return (_i << 26) | (imm3 << 12) | (a << 7) | bcdefgh;
      }
    }
  }
  return 0;
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
  if (addr >= 127 || addr < -128)
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

uint32_t th_encbranch_24(int pos, int addr)
{
  addr = (addr - pos - 4) >> 1;
  TRACE("th_encbranch_24 pos %x addr %x\n", pos, addr);
  return addr;
}

thumb_opcode th_bx_reg(uint16_t rm)
{
  THOP_TRACE("bx %s\n", th_reg_name(rm));
  return (thumb_opcode){
      .size = 2,
      .opcode = (0x4700 | ((rm & 0xf) << 3)),
  };
}

thumb_opcode th_bl_t1(uint32_t imm)
{
  THOP_TRACE("bl <imm 0x%x>\n", (unsigned)imm);
  const uint32_t packed = th_packimm_10_11_0(imm) | 0xF000D000;
  return (thumb_opcode){
      .size = 4,
      .opcode = packed,
  };
}

thumb_opcode th_blx_reg(uint16_t rm)
{
  THOP_TRACE("blx %s\n", th_reg_name(rm));
  return (thumb_opcode){
      .size = 2,
      .opcode = (0x4780 | (rm << 3)),
  };
}

thumb_opcode th_b_t1(uint32_t cond, uint32_t imm8)
{
  THOP_TRACE("b%s <imm8 0x%x>\n", th_cond_name(cond & 0xf), (unsigned)imm8);
  return (thumb_opcode){
      .size = 2,
      .opcode = 0xd000 | ((cond & 0xf) << 8) | (imm8 & 0xff),
  };
}

thumb_opcode th_b_t2(int32_t imm11)
{
  THOP_TRACE("b <imm11 %d>\n", (int)imm11);
  const int32_t i = imm11 >> 1;
  if (i < 1023 && i > -1024 && !(imm11 & 1))
  {
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

thumb_opcode th_b_t3(uint32_t op, uint32_t imm)
{
  THOP_TRACE("b%s.w <imm 0x%x>\n", th_cond_name(op & 0xf), (unsigned)imm);
  const uint32_t enc = th_encbranch_b_t3(imm);
  return (thumb_opcode){
      .size = 4,
      .opcode = (0xf0008000 | (op << 22) | enc),
  };
}

thumb_opcode th_b_t4(int32_t imm)
{
  THOP_TRACE("b.w <imm %d>\n", (int)imm);
  if (imm > 16777215 || imm < -16777215)
    tcc_error("compiler_error: th_b_t4 too far address: 0x%x\n", imm);

  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf0009000 | th_packimm_10_11_0(imm),
  };
}

thumb_opcode th_cbz(uint16_t rn, uint32_t imm, uint32_t nonzero)
{
  THOP_TRACE("%s %s, <imm 0x%x>\n", nonzero ? "cbnz" : "cbz", th_reg_name(rn), (unsigned)imm);
  const uint32_t imm5 = imm & 0x1f;
  const uint32_t i = (imm >> 5) & 0x1;

  return (thumb_opcode){
      .size = 2,
      .opcode = 0xb100 | nonzero << 11 | i << 9 | imm5 << 3 | rn,
  };
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

// all t32 arch
thumb_opcode th_mov_reg(uint32_t rd, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding, bool in_it)
{
  if (shift.mode == THUMB_SHIFT_REGISTER && shift.type != THUMB_SHIFT_NONE)
  {
    return th_mov_reg_shift(rd, rm, shift.value, flags, shift, encoding);
  }

  if (flags != FLAGS_BEHAVIOUR_SET && encoding != ENFORCE_ENCODING_32BIT && shift.type == THUMB_SHIFT_NONE)
  {
    const uint16_t D = (rd >> 3) & 1;
    THOP_TRACE("mov %s, %s\n", th_reg_name(rd), th_reg_name(rm));
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x4600 | (D << 7) | (rm << 3) | (rd & 0x7)),
    };
  }
  if (encoding != ENFORCE_ENCODING_32BIT && rd < 8 && rm < 8 && shift.type != THUMB_SHIFT_RRX &&
      shift.type != THUMB_SHIFT_ROR &&
      ((flags == FLAGS_BEHAVIOUR_SET && !in_it) || (flags != FLAGS_BEHAVIOUR_SET && in_it)))
  {
    THOP_TRACE("%s %s, %s, #%u\n", th_shift_name(shift.type), th_reg_name(rd), th_reg_name(rm), (unsigned)shift.value);
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x0000 | (th_shift_value_to_sr_type(shift) << 11) | shift.value << 6 | (rm << 3) | rd),
    };
  }
  if (encoding != ENFORCE_ENCODING_16BIT)
  {
    THOP_TRACE("mov%s %s, %s", flags == FLAGS_BEHAVIOUR_SET ? "s" : "", th_reg_name(rd), th_reg_name(rm));
    th_trace_shift_suffix(shift);
    THOP_TRACE("\n");
    return th_generic_op_reg_shift_with_status(0xea4f, rd, 0xf, rm, flags, shift);
  }
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_mov_imm(uint16_t rd, uint32_t imm, thumb_flags_behaviour setflags, thumb_enforce_encoding encoding)
{
  if (rd <= 7 && imm >= 0 && imm <= 255 && setflags != FLAGS_BEHAVIOUR_BLOCK && encoding != ENFORCE_ENCODING_32BIT)
  {
    THOP_TRACE("movs %s, #%u\n", th_reg_name(rd), (unsigned)imm);
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x2000 | (rd << 8) | imm,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M

  if (rd != R_SP && rd != R_PC && encoding != ENFORCE_ENCODING_16BIT)
  {
    const uint32_t enc = th_pack_const(imm);
    const uint32_t s = (setflags == FLAGS_BEHAVIOUR_SET) ? 1 : 0;
    if (enc)
    {
      THOP_TRACE("mov%s %s, #%u\n", s ? "s" : "", th_reg_name(rd), (unsigned)imm);
      return (thumb_opcode){
          .size = 4,
          .opcode = 0xf04f0000 | enc | ((rd & 0xf) << 8) | (s << 20),
      };
    }
  }

  if (imm >= 0 && imm <= 0xffff && rd != R_SP && rd != R_PC && setflags != FLAGS_BEHAVIOUR_SET &&
      encoding != ENFORCE_ENCODING_16BIT)
  {
    const uint16_t i = (imm >> 11) & 1;
    const uint32_t imm4 = (imm >> 12) & 0xf;
    const uint32_t imm3 = (imm >> 8) & 0x7;
    THOP_TRACE("movw %s, #%u\n", th_reg_name(rd), (unsigned)imm);
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf2400000 | (i << 26) | (imm4 << 16) | (imm3 << 12) | (rd << 8) | (imm & 0xff),
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_movt(uint32_t rd, uint32_t imm16)
{
  const uint32_t imm8 = imm16 & 0xff;
  const uint32_t imm3 = (imm16 >> 8) & 0x7;
  const uint32_t i = (imm16 >> 11) & 0x1;
  const uint32_t imm4 = (imm16 >> 12) & 0xf;

  if (rd == R_SP || rd == R_PC || imm16 > 0xffff)
  {
    tcc_error("compiler_error: 'th_movt', SP or PC can't be used as rd\n");
    return (thumb_opcode){0, 0};
  }

  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf2c00000 | i << 26 | imm4 << 16 | imm3 << 12 | rd << 8 | imm8,
  };
}

thumb_opcode th_generic_op_imm_with_status(uint16_t op, uint16_t rd, uint16_t rn, uint32_t imm,
                                           thumb_flags_behaviour setflags)
{
#ifndef TCC_TARGET_ARM_ARCHV6M
  const uint32_t packed = th_pack_const(imm);
  if (packed || imm == 0)
  {
    const uint32_t A = packed >> 16;
    const uint32_t B = packed & 0xffff;
    return (thumb_opcode){
        .size = 4,
        .opcode = ((op | ((setflags == FLAGS_BEHAVIOUR_SET) << 4) | rn | A) << 16) | (rd << 8 | B),
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_generic_op_imm(uint16_t op, uint16_t rd, uint16_t rn, uint32_t imm)
{
  return th_generic_op_imm_with_status(op, rd, rn, imm, FLAGS_BEHAVIOUR_NOT_IMPORTANT);
}

thumb_opcode th_add_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding)
{
  if ((rd == R_PC) && (rm == R_PC))
  {
    tcc_error("compiler_error: 'th_add_reg', PC can't be used as rdn and rm\n");
  }
  if (rm < 8 && rd < 8 && rn < 8 && encoding != ENFORCE_ENCODING_32BIT && shift.type == THUMB_SHIFT_NONE)
  {
    // T1
    THOP_TRACE("add%s %s, %s, %s\n", flags == FLAGS_BEHAVIOUR_SET ? "s" : "", th_reg_name(rd), th_reg_name(rn),
               th_reg_name(rm));
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x1800 | (rm << 6) | (rn << 3) | (rd),
    };
  }

  if (rd == rn && flags != FLAGS_BEHAVIOUR_SET && encoding != ENFORCE_ENCODING_32BIT && shift.type == THUMB_SHIFT_NONE)
  {
    // T2
    const uint16_t DN = (rd >> 3) & 1;
    THOP_TRACE("add %s, %s\n", th_reg_name(rd), th_reg_name(rm));
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x4400 | (DN << 7) | ((rm & 0xf) << 3) | (rd & 0x7),
    };
  }
  THOP_TRACE("add%s %s, %s, %s", flags == FLAGS_BEHAVIOUR_SET ? "s" : "", th_reg_name(rd), th_reg_name(rn),
             th_reg_name(rm));
  th_trace_shift_suffix(shift);
  THOP_TRACE("\n");
  return th_generic_op_reg_shift_with_status(0xeb00, rd, rn, rm, flags, shift);
}

thumb_opcode th_add_imm_t4(uint32_t rd, uint32_t rn, uint32_t imm)
{
  if (imm <= 4095)
  {
    const uint16_t i = (imm >> 11) & 1;
    const uint32_t imm3 = (imm >> 8) & 7;
    uint32_t op = (0xf200 | (i << 10) | rn) << 16;
    op |= ((imm3 << 12) | (rd << 8) | (imm & 0xff));
    return (thumb_opcode){
        .size = 4,
        .opcode = op,
    };
  }
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_add_imm(uint32_t rd, uint32_t rn, uint32_t imm, thumb_flags_behaviour flags,
                        thumb_enforce_encoding encoding)
{
  thumb_opcode op = {0, 0};
  if (rd == rn && rd < 8 && imm <= 255 && encoding != ENFORCE_ENCODING_32BIT)
  {
    THOP_TRACE("add%s %s, #%u\n", flags == FLAGS_BEHAVIOUR_SET ? "s" : "", th_reg_name(rd), (unsigned)imm);
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x3000 | (rd << 8) | imm),
    };
  }

  if (imm <= 7 && rd < 8 && rn < 8 && encoding != ENFORCE_ENCODING_32BIT)
  {
    THOP_TRACE("add%s %s, %s, #%u\n", flags == FLAGS_BEHAVIOUR_SET ? "s" : "", th_reg_name(rd), th_reg_name(rn),
               (unsigned)imm);
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x1c00 | (imm << 6) | (rn << 3) | rd),
    };
  }

  op = th_generic_op_imm_with_status(0xf100, rd, rn, imm, flags);
  if (op.size != 0)
  {
    THOP_TRACE("add%s %s, %s, #%u\n", flags == FLAGS_BEHAVIOUR_SET ? "s" : "", th_reg_name(rd), th_reg_name(rn),
               (unsigned)imm);
    return op;
  }
  if (imm <= 4095 && encoding != ENFORCE_ENCODING_16BIT && flags != FLAGS_BEHAVIOUR_SET)
  {
    THOP_TRACE("add %s, %s, #%u\n", th_reg_name(rd), th_reg_name(rn), (unsigned)imm);
    return th_add_imm_t4(rd, rn, imm);
  }
  return op;
}

thumb_opcode th_adr_imm(uint32_t rd, int imm, thumb_enforce_encoding encoding)
{
  if (imm <= 1020 && imm >= 0 && encoding != ENFORCE_ENCODING_32BIT && imm % 4 == 0)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xA000 | (rd << 8) | (imm >> 2),
    };
  }

  if (imm >= 0 && imm <= 4095)
  {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf20f0000 | (rd << 8) | th_packimm_3_8_1(imm),
    };
  }

  if (imm < 0 && imm >= -4096)
  {
    imm = -imm;
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf2af0000 | (rd << 8) | th_packimm_3_8_1(imm),
    };
  }

  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}
thumb_opcode th_bic_imm(uint32_t rd, uint32_t rn, uint32_t imm, thumb_flags_behaviour flags,
                        thumb_enforce_encoding encoding)
{
#ifndef TCC_TARGET_ARM_ARCHV6M
  if (rd != R_SP && rd != R_PC && rn != R_SP && rd != R_PC)
  {
    const uint32_t packed = th_pack_const(imm);
    const uint32_t s = (flags == FLAGS_BEHAVIOUR_SET);
    if (packed || imm == 0)
    {
      return (thumb_opcode){
          .size = 4,
          .opcode = 0xf0200000 | packed | (rn << 16) | (rd << 8) | (s << 20),
      };
    }
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_bic_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding)
{
  if (rm < 8 && rd < 8 && rn < 8 && shift.type == THUMB_SHIFT_NONE && encoding != ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x4380 | (rm << 3) | rd,
    };
  }
  return th_generic_op_reg_shift_with_status(0xea20, rd, rn, rm, flags, shift);
}

thumb_opcode th_and_imm(uint32_t rd, uint32_t rn, uint32_t imm, thumb_flags_behaviour setflags,
                        thumb_enforce_encoding encoding)
{
  thumb_opcode op = th_generic_op_imm_with_status(0xf000, rd, rn, imm, setflags);
  return op.size != 0 ? op : th_bic_imm(rd, rn, ~imm, setflags, encoding);
}

thumb_opcode th_and_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding)
{
  if (rd == rn && rm < 8 && rn < 8 && shift.type == THUMB_SHIFT_NONE && encoding != ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x4000 | (rm << 3) | rd,
    };
  }
  return th_generic_op_reg_shift_with_status(0xea00, rd, rn, rm, flags, shift);
}

thumb_opcode th_xor_reg(uint16_t rd, uint16_t rn, uint16_t rm)
{
  if (rd == rn && rm < 8 && rn < 8)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x4040 | (rm << 3) | rd,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC)
  {
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

thumb_opcode th_xor_imm(uint16_t rd, uint16_t rn, uint32_t imm)
{
  return th_generic_op_imm(0xf080, rd, rn, imm);
}

thumb_opcode th_rsb_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding)
{
  return th_generic_op_reg_shift_with_status(0xebc0, rd, rn, rm, flags, shift);
}

thumb_opcode th_sub_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding)
{
  if (rd < 8 && rm < 8 && rn < 8 && shift.type == THUMB_SHIFT_NONE && encoding != ENFORCE_ENCODING_32BIT)
  {
    THOP_TRACE("sub%s %s, %s, %s\n", flags == FLAGS_BEHAVIOUR_SET ? "s" : "", th_reg_name(rd), th_reg_name(rn),
               th_reg_name(rm));
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x1a00 | (rm << 6) | (rn << 3) | rd,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC)
  {
    const uint32_t imm3 = (shift.value >> 2) & 0x7;
    const uint32_t imm2 = shift.value & 0x3;
    const uint32_t s = (flags == FLAGS_BEHAVIOUR_SET) ? 1 : 0;
    THOP_TRACE("sub%s %s, %s, %s", s ? "s" : "", th_reg_name(rd), th_reg_name(rn), th_reg_name(rm));
    th_trace_shift_suffix(shift);
    THOP_TRACE("\n");
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xeba00000 | (s << 20) | (rn << 16) | (rd << 8) | rm | imm3 << 12 | imm2 << 6 |
                  th_shift_value_to_sr_type(shift) << 4,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_sub_sp_reg(uint32_t rd, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                           thumb_enforce_encoding encoding)
{
  return th_generic_op_reg_shift_with_status(0xeba0, rd, R_SP, rm, flags, shift);
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

thumb_opcode th_adc_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding)
{
  if (rd == rn && rm < 8 && rn < 8 && shift.type == THUMB_SHIFT_NONE && encoding != ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x4140 | (rm << 3) | rd,
    };
  }

  return th_generic_op_reg_shift_with_status(0xeb40, rd, rn, rm, flags, shift);
}

thumb_opcode th_adc_imm(uint32_t rd, uint32_t rn, uint32_t imm, thumb_flags_behaviour setflags,
                        thumb_enforce_encoding encoding)
{
  return th_generic_op_imm_with_status(0xf140, rd, rn, imm, setflags);
}

thumb_opcode th_sbc_imm(uint32_t rd, uint32_t rn, uint32_t imm, thumb_flags_behaviour flags,
                        thumb_enforce_encoding encoding)
{
  return th_generic_op_imm_with_status(0xf160, rd, rn, imm, flags);
}

thumb_opcode th_sbc_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding)
{
  if (rd == rn && rm < 8 && rn < 8 && shift.type == THUMB_SHIFT_NONE && encoding != ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x4180 | (rm << 3) | rd,
    };
  }
  return th_generic_op_reg_shift_with_status(0xeb60, rd, rn, rm, flags, shift);
}

thumb_opcode th_orr_imm(uint32_t rd, uint32_t rn, uint32_t imm, thumb_flags_behaviour setflags,
                        thumb_enforce_encoding encoding)
{
  (void)encoding; /* currently unused */
  if (rn != R_SP && rd != R_SP && rn != R_PC)
  {
    return th_generic_op_imm_with_status(0xf040, rd, rn, imm, setflags);
  }
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_cmp_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding)
{
  (void)rd;    /* CMP doesn't use rd - result goes to flags */
  (void)flags; /* CMP always sets flags */
  if (rm < 8 && rn < 8 && shift.type == THUMB_SHIFT_NONE && encoding != ENFORCE_ENCODING_32BIT)
  {
    THOP_TRACE("cmp %s, %s\n", th_reg_name(rn), th_reg_name(rm));
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x4280 | (rm << 3) | rn),
    };
  }
  else if (!(rm < 8 && rn < 8) && rm != R_PC && rn != R_PC && encoding != ENFORCE_ENCODING_32BIT &&
           shift.type == THUMB_SHIFT_NONE)
  {
    const uint16_t N = (rn >> 3) & 0x1;
    THOP_TRACE("cmp %s, %s\n", th_reg_name(rn), th_reg_name(rm));
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x4500 | (N << 7) | (rm << 3) | (rn & 0x7)),
    };
  }
  THOP_TRACE("cmp %s, %s", th_reg_name(rn), th_reg_name(rm));
  th_trace_shift_suffix(shift);
  THOP_TRACE("\n");
  return th_generic_op_reg_shift_with_status(0xebb0, 0xf, rn, rm, FLAGS_BEHAVIOUR_SET, shift);
}

thumb_opcode th_orr_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding)
{
  if (rd == rn && rm < 8 && rn < 8 && shift.type == THUMB_SHIFT_NONE && encoding != ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x4300 | (rm << 3) | rd),
    };
  }
  return th_generic_op_reg_shift_with_status(0xea40, rd, rn, rm, flags, shift);
}

thumb_opcode th_sub_imm_t4(uint32_t rd, uint32_t rn, uint32_t imm)
{
  if (rd != R_SP && rd != R_PC && imm <= 0xfff)
  {
    // T4
    const uint16_t i = imm >> 11;
    const uint32_t imm3 = (imm >> 8) & 0x7;
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf2a00000 | (i << 26) | (rn << 16) | (imm3 << 12) | (rd << 8) | (imm & 0xff),
    };
  }

  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_sub_imm(uint32_t rd, uint32_t rn, uint32_t imm, thumb_flags_behaviour flags,
                        thumb_enforce_encoding encoding)
{
  if (rd == rn && imm <= 255 && rd < 8 && encoding != ENFORCE_ENCODING_32BIT)
  {
    // T2
    THOP_TRACE("sub%s %s, #%u\n", flags == FLAGS_BEHAVIOUR_SET ? "s" : "", th_reg_name(rd), (unsigned)imm);
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x3800 | (rd << 8) | imm),
    };
  }

  if (rd < 8 && rn < 8 && imm <= 7 && encoding != ENFORCE_ENCODING_32BIT)
  {
    // T1
    THOP_TRACE("sub%s %s, %s, #%u\n", flags == FLAGS_BEHAVIOUR_SET ? "s" : "", th_reg_name(rd), th_reg_name(rn),
               (unsigned)imm);
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x1e00 | (imm << 6) | (rn << 3) | rd),
    };
  }

  if (rd != 13 && rd != 15)
  {
    const uint32_t enc = th_pack_const(imm);
    const uint32_t s = (flags == FLAGS_BEHAVIOUR_SET) ? 1 : 0;
    if (enc || imm == 0)
    {
      THOP_TRACE("sub%s %s, %s, #%u\n", s ? "s" : "", th_reg_name(rd), th_reg_name(rn), (unsigned)imm);
      return (thumb_opcode){
          .size = 4,
          .opcode = 0xf1a00000 | s << 20 | (rn << 16) | (rd << 8) | enc,
      };
    }
  }
  THOP_TRACE("sub %s, %s, #%u\n", th_reg_name(rd), th_reg_name(rn), (unsigned)imm);
  return th_sub_imm_t4(rd, rn, imm);
}

thumb_opcode th_push(uint16_t regs)
{
  // T1 encoding R0-R7 + LR only, all armv-m
  // (T2 in armv8-m - inconsistent naming in reference manual)
  if (!(regs & 0xbf00))
  {
    const uint16_t lr = (regs >> 14) & 1;
    THOP_TRACE("push ");
    th_trace_regset(regs);
    THOP_TRACE("\n");
    return (thumb_opcode){
        .size = 2,
        .opcode = (0xb400 | (lr << 8) | (regs & 0xff)),
    };
  }
// T2 encoding R0-R12 + LR only, > armv7-m
// (T1 in armv8-m - inconsistent naming in reference manual)
#if defined(TCC_TARGET_ARM_ARCHV8M) || defined(TCC_TARGET_ARM_ARCHV7M)
  if (!(regs & 0xa000))
  {
    THOP_TRACE("push ");
    th_trace_regset(regs);
    THOP_TRACE("\n");
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

int th_ldr_literal_estimate(uint16_t rt, uint32_t imm)
{
  if (rt < 8 && !(imm & 3) && imm <= 0x3ff)
    return 2;
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (imm <= 0xfff)
    return 4;
#endif
  return 0;
}

thumb_opcode th_ldrsh_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding encoding)
{
#ifndef TCC_TARGET_ARM_ARCHV6M
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rt != R_SP && imm <= 4095 && puw == 6 && rn != R_PC)
  {
    uint32_t ins = (0xf9b0 | ((rn & 0xf))) << 16;
    ins |= (((rt & 0xf) << 12) | imm);
    THOP_TRACE("ldrsh %s, [%s, #%d]\n", th_reg_name(rt), th_reg_name(rn), imm);
    return (thumb_opcode){
        .size = 4,
        .opcode = ins,
    };
  }
  else if (imm <= 4095 && rn == R_PC)
  {
    const uint32_t u = (puw & 0x2) >> 1;
    THOP_TRACE("ldrsh %s, [%s, #%c%d]\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf93f0000 | (rn << 16) | (rt << 12) | (u << 23) | imm,
    };
  }
  else if (rt != R_SP && imm <= 255)
  {
    uint32_t ins = (0xf930 | (rn & 0xf)) << 16;
    ins |= (0x0800 | ((rt & 0xf) << 12) | (puw << 8) | imm);
    {
      const uint32_t p = (puw >> 2) & 1;
      const uint32_t u = (puw >> 1) & 1;
      const uint32_t w = (puw >> 0) & 1;
      if (p && !w)
      {
        THOP_TRACE("ldrsh %s, [%s, #%c%d]\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else if (p && w)
      {
        THOP_TRACE("ldrsh %s, [%s, #%c%d]!\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else if (!p && w)
      {
        THOP_TRACE("ldrsh %s, [%s], #%c%d\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else
      {
        THOP_TRACE("ldrsh %s, [%s, #%c%d] (puw=%u)\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm,
                   (unsigned)puw);
      }
    }
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

thumb_opcode th_ldrsh_reg(uint32_t rt, uint32_t rn, uint32_t rm, thumb_shift shift, thumb_enforce_encoding encoding)
{
  if (shift.type != THUMB_SHIFT_NONE && shift.type != THUMB_SHIFT_LSL)
  {
    tcc_error("compiler_error: 'th_ldrsh_reg', only LSL shift supported\n");
  }
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rm < 8 && rt < 8 && rn < 8 && shift.type == THUMB_SHIFT_NONE && encoding != ENFORCE_ENCODING_32BIT)
  {
    THOP_TRACE("ldrsh %s, [%s, %s]\n", th_reg_name(rt), th_reg_name(rn), th_reg_name(rm));
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x5e00 | (rm << 6) | (rn << 3) | rt,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rt != R_SP && rm != R_SP && rm != R_SP)
  {
    THOP_TRACE("ldrsh %s, [%s, %s", th_reg_name(rt), th_reg_name(rn), th_reg_name(rm));
    th_trace_shift_suffix(shift);
    THOP_TRACE("]\n");
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf9300000 | (rn << 16) | (rt << 12) | rm | shift.value << 4,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_ldrh_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding encoding)
{
  // T1 encoding, on armv6-m this one is the only one available
  if (puw == 6 && rn < 8 && rt < 8 && imm <= 62 && encoding != ENFORCE_ENCODING_32BIT && !(imm & 1))
  {
    THOP_TRACE("ldrh %s, [%s, #%d]\n", th_reg_name(rt), th_reg_name(rn), imm);
    imm = imm >> 1;
    // imm[0] is enforced to be 0, and sould be divided by 2, thus offset is 5
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x8800 | (imm << 6) | (rn << 3) | rt),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (puw == 6 && rt != R_SP && imm >= 0 && imm <= 4095 && rn != R_PC)
  {
    THOP_TRACE("ldrh %s, [%s, #%d]\n", th_reg_name(rt), th_reg_name(rn), imm);
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf8b00000 | (rn << 16) | (rt << 12) | imm,
    };
  }
  else if (imm >= 0 && imm <= 4095 && rn == R_PC)
  {
    const uint32_t u = (puw & 0x2) >> 1;
    THOP_TRACE("ldrh %s, [%s, #%c%d]\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf83f0000 | (u << 23) | (rn << 16) | (rt << 12) | imm,
    };
  }
  else if (rt != R_SP && imm <= 255)
  {
    {
      const uint32_t p = (puw >> 2) & 1;
      const uint32_t u = (puw >> 1) & 1;
      const uint32_t w = (puw >> 0) & 1;
      if (p && !w)
      {
        THOP_TRACE("ldrh %s, [%s, #%c%d]\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else if (p && w)
      {
        THOP_TRACE("ldrh %s, [%s, #%c%d]!\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else if (!p && w)
      {
        THOP_TRACE("ldrh %s, [%s], #%c%d\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else
      {
        THOP_TRACE("ldrh %s, [%s, #%c%d] (puw=%u)\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm,
                   (unsigned)puw);
      }
    }
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

thumb_opcode th_ldrh_reg(uint32_t rt, uint32_t rn, uint32_t rm, thumb_shift shift, thumb_enforce_encoding encoding)
{

  if (shift.type != THUMB_SHIFT_NONE && shift.type != THUMB_SHIFT_LSL)
  {
    tcc_error("compiler_error: 'th_ldr_reg', only LSL shift supported\n");
  }
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rm < 8 && rt < 8 && rn < 8 && shift.type == THUMB_SHIFT_NONE && encoding != ENFORCE_ENCODING_32BIT)
  {
    THOP_TRACE("ldrh %s, [%s, %s]\n", th_reg_name(rt), th_reg_name(rn), th_reg_name(rm));
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x5a00 | (rm << 6) | (rn << 3) | rt,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rt != R_SP && rm != R_SP && rm != R_PC)
  {
    THOP_TRACE("ldrh %s, [%s, %s", th_reg_name(rt), th_reg_name(rn), th_reg_name(rm));
    th_trace_shift_suffix(shift);
    THOP_TRACE("]\n");
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf8300000 | (rn << 16) | (rt << 12) | rm | shift.value << 4,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_ldrsb_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding encoding)
{
#ifndef TCC_TARGET_ARM_ARCHV6M
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rt != R_SP && imm <= 4095 && puw == 6 && rn != R_PC)
  {
    THOP_TRACE("ldrsb %s, [%s, #%d]\n", th_reg_name(rt), th_reg_name(rn), imm);
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf9900000 | (rn << 16) | (rt << 12) | imm,
    };
  }
  else if (imm <= 4095 && rn == R_PC)
  {
    const uint32_t u = (puw & 0x2) >> 1;
    THOP_TRACE("ldrsb %s, [%s, #%c%d]\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf91f0000 | (rn << 16) | (rt << 12) | (u << 23) | imm,
    };
  }
  else if (rt != R_SP && imm <= 255)
  {
    {
      const uint32_t p = (puw >> 2) & 1;
      const uint32_t u = (puw >> 1) & 1;
      const uint32_t w = (puw >> 0) & 1;
      if (p && !w)
      {
        THOP_TRACE("ldrsb %s, [%s, #%c%d]\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else if (p && w)
      {
        THOP_TRACE("ldrsb %s, [%s, #%c%d]!\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else if (!p && w)
      {
        THOP_TRACE("ldrsb %s, [%s], #%c%d\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else
      {
        THOP_TRACE("ldrsb %s, [%s, #%c%d] (puw=%u)\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm,
                   (unsigned)puw);
      }
    }
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

thumb_opcode th_ldrsb_reg(uint32_t rt, uint32_t rn, uint32_t rm, thumb_shift shift, thumb_enforce_encoding encoding)
{
  if (shift.type != THUMB_SHIFT_NONE && shift.type != THUMB_SHIFT_LSL)
  {
    tcc_error("compiler_error: 'th_ldr_reg', only LSL shift supported\n");
  }

  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rm < 8 && rt < 8 && rn < 8 && encoding != ENFORCE_ENCODING_32BIT && shift.type == THUMB_SHIFT_NONE)
  {
    THOP_TRACE("ldrsb %s, [%s, %s]\n", th_reg_name(rt), th_reg_name(rn), th_reg_name(rm));
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x5600 | (rm << 6) | (rn << 3) | rt,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rt != R_SP && rm != R_SP && rm != R_SP)
  {
    THOP_TRACE("ldrsb %s, [%s, %s", th_reg_name(rt), th_reg_name(rn), th_reg_name(rm));
    th_trace_shift_suffix(shift);
    THOP_TRACE("]\n");
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf9100000 | (rn << 16) | (rt << 12) | rm | shift.value << 4,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_ldrb_imm(uint16_t rt, uint16_t rn, int imm, uint32_t puw, thumb_enforce_encoding encoding)
{
  // T1 encoding, on armv6-m this one is the only one available
  if (puw == 6 && rn < 8 && rt < 8 && imm <= 31 && encoding != ENFORCE_ENCODING_32BIT)
  {
    // imm[0] is enforced to be 0, and sould be divided by 2, thus offset is 5
    THOP_TRACE("ldrb %s, [%s, #%d]\n", th_reg_name(rt), th_reg_name(rn), imm);
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x7800 | (imm << 6) | (rn << 3) | rt,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (puw == 6 && rt != R_SP && imm >= 0 && imm <= 4095 && rn != R_PC)
  {
    THOP_TRACE("ldrb %s, [%s, #%d]\n", th_reg_name(rt), th_reg_name(rn), imm);
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf8900000 | (rn << 16) | (rt << 12) | imm,
    };
  }
  else if (imm >= 0 && imm <= 4095 && rn == R_PC)
  {
    uint32_t u = (puw & 0x2) >> 1;
    THOP_TRACE("ldrb %s, [%s, #%c%d]\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf81f0000 | (u << 23) | (rt << 12) | imm,
    };
  }
  else if (rt != R_SP && imm <= 255)
  {
    {
      const uint32_t p = (puw >> 2) & 1;
      const uint32_t u = (puw >> 1) & 1;
      const uint32_t w = (puw >> 0) & 1;
      if (p && !w)
      {
        THOP_TRACE("ldrb %s, [%s, #%c%d]\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else if (p && w)
      {
        THOP_TRACE("ldrb %s, [%s, #%c%d]!\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else if (!p && w)
      {
        THOP_TRACE("ldrb %s, [%s], #%c%d\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else
      {
        THOP_TRACE("ldrb %s, [%s, #%c%d] (puw=%u)\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm,
                   (unsigned)puw);
      }
    }
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

thumb_opcode th_ldrb_reg(uint32_t rt, uint32_t rn, uint32_t rm, thumb_shift shift, thumb_enforce_encoding encoding)
{
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (shift.type != THUMB_SHIFT_NONE && shift.type != THUMB_SHIFT_LSL)
  {
    tcc_error("compiler_error: 'th_ldr_reg', only LSL shift supported\n");
  }
  if (rm < 8 && rt < 8 && rn < 8 && shift.type == THUMB_SHIFT_NONE && encoding != ENFORCE_ENCODING_32BIT)
  {
    THOP_TRACE("ldrb %s, [%s, %s]\n", th_reg_name(rt), th_reg_name(rn), th_reg_name(rm));
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x5c00 | (rm << 6) | (rn << 3) | rt,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rt != R_SP && rm != R_SP && rm != R_PC)
  {
    THOP_TRACE("ldrb %s, [%s, %s", th_reg_name(rt), th_reg_name(rn), th_reg_name(rm));
    th_trace_shift_suffix(shift);
    THOP_TRACE("]\n");
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf8100000 | (rn << 16) | (rt << 12) | rm | shift.value << 4,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_ldr_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding encoding)
{
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (puw == 6 && rn < 8 && rt < 8 && imm <= 124 && !(imm & 3) && encoding != ENFORCE_ENCODING_32BIT)
  {
    // imm[0] is enforced to be 0, and sould be divided by 4, thus offset is 4
    THOP_TRACE("ldr %s, [%s, #%d]\n", th_reg_name(rt), th_reg_name(rn), imm);
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x6800 | (imm << 4) | (rn << 3) | rt,
    };
  }
  else if (puw == 6 && rn == R_SP && rt < 8 && imm <= 1020 && encoding != ENFORCE_ENCODING_32BIT)
  {
    THOP_TRACE("ldr %s, [%s, #%d]\n", th_reg_name(rt), th_reg_name(rn), imm);
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x9800 | (rt << 8) | (imm >> 2),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (puw == 6 && imm <= 4095 && rn != R_PC)
  {
    uint32_t ins = (0xf8d0 | (rn & 0xf)) << 16;
    ins |= (rt << 12) | imm;
    THOP_TRACE("ldr %s, [%s, #%d]\n", th_reg_name(rt), th_reg_name(rn), imm);
    return (thumb_opcode){
        .size = 4,
        .opcode = ins,
    };
  }
  else if (imm >= 0 && imm <= 4095 && rn == R_PC)
  {
    uint32_t u = (puw & 0x2) >> 1;
    THOP_TRACE("ldr %s, [%s, #%c%d]\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf85f0000 | (u << 23) | (rt << 12) | imm,
    };
  }
  else if (imm <= 255)
  {
    uint32_t ins = (0xf850 | (rn & 0xf)) << 16;
    ins |= (0x0800 | ((rt & 0xf) << 12) | ((puw & 0x7) << 8) | imm);
    {
      const uint32_t p = (puw >> 2) & 1;
      const uint32_t u = (puw >> 1) & 1;
      const uint32_t w = (puw >> 0) & 1;
      if (p && !w)
      {
        THOP_TRACE("ldr %s, [%s, #%c%d]\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else if (p && w)
      {
        THOP_TRACE("ldr %s, [%s, #%c%d]!\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else if (!p && w)
      {
        THOP_TRACE("ldr %s, [%s], #%c%d\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else
      {
        THOP_TRACE("ldr %s, [%s, #%c%d] (puw=%u)\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm,
                   (unsigned)puw);
      }
    }
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

thumb_opcode th_ldr_reg(uint32_t rt, uint32_t rn, uint32_t rm, thumb_shift shift, thumb_enforce_encoding encoding)
{
  if (shift.type != THUMB_SHIFT_NONE && shift.type != THUMB_SHIFT_LSL)
  {
    tcc_error("compiler_error: 'th_ldr_reg', only LSL shift supported\n");
  }
  if (rm < 8 && rt < 8 && rn < 8 && shift.type == THUMB_SHIFT_NONE && encoding != ENFORCE_ENCODING_32BIT)
  {
    THOP_TRACE("ldr %s, [%s, %s]\n", th_reg_name(rt), th_reg_name(rn), th_reg_name(rm));
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x5800 | (rm << 6) | (rn << 3) | rt),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rt != R_SP && rm != R_SP && rm != R_PC)
  {
    THOP_TRACE("ldr %s, [%s, %s", th_reg_name(rt), th_reg_name(rn), th_reg_name(rm));
    th_trace_shift_suffix(shift);
    THOP_TRACE("]\n");
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf8500000 | (rn << 16) | (rt << 12) | rm | shift.value << 4,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_ldr_literal(uint16_t rt, uint32_t imm, uint32_t add)
{
  if (rt < 8 && imm <= 1020)
  {
    THOP_TRACE("ldr %s, [%s, #%c%u]\n", th_reg_name(rt), th_reg_name(R_PC), (add & 1) ? '+' : '-', (unsigned)imm);
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x4800 | (rt << 8) | imm >> 2,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rt != R_PC && imm <= 0xffff)
  {
    THOP_TRACE("ldr %s, [%s, #%c%u]\n", th_reg_name(rt), th_reg_name(R_PC), (add & 1) ? '+' : '-', (unsigned)imm);
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

thumb_opcode th_pop(uint16_t regs)
{
  // T1 encoding R0-R7 + PC only, all armv-m
  // (T2 in armv8-m - inconsistent naming in reference manual)
  if (!(regs & 0x7f00))
  {
    const uint16_t pc = (regs >> 15) & 1;
    THOP_TRACE("pop ");
    th_trace_regset(regs);
    THOP_TRACE("\n");
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xbc00 | (pc << 8) | (regs & 0xff),
    };
  }
// T2 encoding R0-R12 + PC + LR, > armv7-m
// (T1 in armv8-m - inconsistent naming in reference manual)
#if defined(TCC_TARGET_ARM_ARCHV8M) || defined(TCC_TARGET_ARM_ARCHV7M)
  if (!(regs & 0x2000))
  {
    THOP_TRACE("pop ");
    th_trace_regset(regs);
    THOP_TRACE("\n");
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
thumb_opcode th_strh_imm(uint16_t rt, uint16_t rn, int imm, uint16_t puw, thumb_enforce_encoding encoding)
{
  // T1 encoding, on armv6-m this one is the only one available
  if (puw == 6 && rn < 8 && rt < 8 && imm <= 62 && encoding != ENFORCE_ENCODING_32BIT && !(imm & 1))
  {
    // imm[0] is enforced to be 0, and sould be divided by 2, thus offset is 5
    THOP_TRACE("strh %s, [%s, #%d]\n", th_reg_name(rt), th_reg_name(rn), imm);
    imm >>= 1;
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x8000 | (imm << 6) | (rn << 3) | rt),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (puw == 6 && rt != R_SP && imm <= 4095)
  {
    THOP_TRACE("strh %s, [%s, #%d]\n", th_reg_name(rt), th_reg_name(rn), imm);
    return (thumb_opcode){
        .size = 4,
        .opcode = (0xf8a00000 | (rn << 16) | (rt << 12) | imm),
    };
  }
  else if (rt != R_SP && imm <= 255)
  {
    {
      const uint32_t p = (puw >> 2) & 1;
      const uint32_t u = (puw >> 1) & 1;
      const uint32_t w = (puw >> 0) & 1;
      if (p && !w)
      {
        THOP_TRACE("strh %s, [%s, #%c%d]\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else if (p && w)
      {
        THOP_TRACE("strh %s, [%s, #%c%d]!\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else if (!p && w)
      {
        THOP_TRACE("strh %s, [%s], #%c%d\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else
      {
        THOP_TRACE("strh %s, [%s, #%c%d] (puw=%u)\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm,
                   (unsigned)puw);
      }
    }
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf8200800 | (rn << 16) | (rt << 12) | ((puw & 0x7) << 8) | imm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_strh_reg(uint32_t rt, uint32_t rn, uint32_t rm, thumb_shift shift, thumb_enforce_encoding encoding)
{
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rm < 8 && rt < 8 && rn < 8 && encoding != ENFORCE_ENCODING_32BIT && shift.type == THUMB_SHIFT_NONE)
  {
    THOP_TRACE("strh %s, [%s, %s]\n", th_reg_name(rt), th_reg_name(rn), th_reg_name(rm));
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x5200 | (rm << 6) | (rn << 3) | rt,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rt != R_SP && rm != R_SP && rm != R_PC)
  {
    THOP_TRACE("strh %s, [%s, %s", th_reg_name(rt), th_reg_name(rn), th_reg_name(rm));
    th_trace_shift_suffix(shift);
    THOP_TRACE("]\n");
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf8200000 | (rn << 16) | (rt << 12) | rm | shift.value << 4,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_strb_imm(uint16_t rt, uint16_t rn, int imm, uint16_t puw, thumb_enforce_encoding encoding)
{
  // T1 encoding, on armv6-m this one is the only one available
  if (puw == 6 && rn < 8 && rt < 8 && imm <= 31 && encoding != ENFORCE_ENCODING_32BIT)
  {
    // imm[0] is enforced to be 0, and sould be divided by 2, thus offset is 5
    THOP_TRACE("strb %s, [%s, #%d]\n", th_reg_name(rt), th_reg_name(rn), imm);
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x7000 | (imm << 6) | (rn << 3) | rt,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (puw == 6 && rt != R_SP && imm <= 4095)
  {
    THOP_TRACE("strb %s, [%s, #%d]\n", th_reg_name(rt), th_reg_name(rn), imm);
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf8800000 | (rn << 16) | (rt << 12) | imm,
    };
  }
  else if (rt != R_SP && imm <= 255)
  {
    {
      const uint32_t p = (puw >> 2) & 1;
      const uint32_t u = (puw >> 1) & 1;
      const uint32_t w = (puw >> 0) & 1;
      if (p && !w)
      {
        THOP_TRACE("strb %s, [%s, #%c%d]\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else if (p && w)
      {
        THOP_TRACE("strb %s, [%s, #%c%d]!\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else if (!p && w)
      {
        THOP_TRACE("strb %s, [%s], #%c%d\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else
      {
        THOP_TRACE("strb %s, [%s, #%c%d] (puw=%u)\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm,
                   (unsigned)puw);
      }
    }
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf8000800 | (rn << 16) | (rt << 12) | ((puw & 0x7) << 8) | imm,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_strb_reg(uint32_t rt, uint32_t rn, uint32_t rm, thumb_shift shift, thumb_enforce_encoding encoding)
{
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rm < 8 && rt < 8 && rn < 8 && shift.type == THUMB_SHIFT_NONE && encoding != ENFORCE_ENCODING_32BIT)
  {
    THOP_TRACE("strb %s, [%s, %s]\n", th_reg_name(rt), th_reg_name(rn), th_reg_name(rm));
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x5400 | (rm << 6) | (rn << 3) | rt),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rt != R_SP && rm != R_SP && rm != R_PC)
  {
    THOP_TRACE("strb %s, [%s, %s", th_reg_name(rt), th_reg_name(rn), th_reg_name(rm));
    th_trace_shift_suffix(shift);
    THOP_TRACE("]\n");
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf8000000 | (rn << 16) | (rt << 12) | rm | shift.value << 4,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_str_reg(uint32_t rt, uint32_t rn, uint32_t rm, thumb_shift shift, thumb_enforce_encoding encoding)
{
  if (shift.type != THUMB_SHIFT_NONE && shift.type != THUMB_SHIFT_LSL)
  {
    tcc_error("compiler_error: 'th_str_reg', only LSL shift supported\n");
  }

  if (rm < 8 && rt < 8 && rn < 8 && shift.type == THUMB_SHIFT_NONE && encoding != ENFORCE_ENCODING_32BIT)
  {
    THOP_TRACE("str %s, [%s, %s]\n", th_reg_name(rt), th_reg_name(rn), th_reg_name(rm));
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x5000 | (rm << 6) | (rn << 3) | rt),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rt != R_SP && rm != R_SP && rm != R_PC)
  {
    THOP_TRACE("str %s, [%s, %s", th_reg_name(rt), th_reg_name(rn), th_reg_name(rm));
    th_trace_shift_suffix(shift);
    THOP_TRACE("]\n");
    return (thumb_opcode){
        .size = 4,
        .opcode = (0xf8400000 | (rn << 16) | (rt << 12) | rm | shift.value << 4),
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_mul(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_enforce_encoding encoding)
{
  if (rd == rm && rd < 8 && rn < 8 && encoding != ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x4340 | ((rn & 0x7) << 3) | (rm & 0x7)),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else
  {
    return (thumb_opcode){
        .size = 4,
        .opcode = (0xfb00f000 | ((rn & 0xf) << 16) | ((rd & 0xf) << 8) | (rm & 0xf)),
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_umull(uint32_t rdlo, uint32_t rdhi, uint16_t rn, uint16_t rm)
{
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

thumb_opcode th_udiv(uint16_t rd, uint16_t rn, uint16_t rm)
{
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

thumb_opcode th_sdiv(uint16_t rd, uint16_t rn, uint16_t rm)
{
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

thumb_opcode th_add_sp_imm_t4(uint32_t rd, uint32_t imm, thumb_flags_behaviour flags, thumb_enforce_encoding encoding)
{
  if (rd != R_PC && imm <= 4095 && (encoding != ENFORCE_ENCODING_16BIT) && (flags != FLAGS_BEHAVIOUR_SET))
  {
    const uint16_t i = (imm >> 11) & 1;
    const uint32_t imm3 = (imm >> 8) & 7;
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf20d0000 | (i << 26) | (imm3 << 12) | (rd << 8) | (imm & 0xff),
    };
  }
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_add_sp_imm(uint16_t rd, uint32_t imm, thumb_flags_behaviour flags, thumb_enforce_encoding encoding)
{
  // T1 on all armv-m
  if (rd < 8 && imm <= 1020 && !(imm & 0x3) && (flags != FLAGS_BEHAVIOUR_SET) && (encoding != ENFORCE_ENCODING_32BIT))
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = (0xa800 | (rd << 8) | (imm >> 2)),
    };
  }
  // T2 on all armv-m
  else if (rd == R_SP && imm <= 508 && !(imm & 0x3) && (flags != FLAGS_BEHAVIOUR_SET) &&
           (encoding != ENFORCE_ENCODING_32BIT))
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xb000 | (imm >> 2),
    };
  }
#if !defined(TCC_TARGET_ARM_ARCHV6M)
  // T3
  else if (rd != R_PC && (encoding != ENFORCE_ENCODING_16BIT))
  {
    const uint32_t enc = th_pack_const(imm);
    const uint32_t s = (flags == FLAGS_BEHAVIOUR_SET) ? 1 : 0;
    if (enc || imm == 0)
    {
      return (thumb_opcode){
          .size = 4,
          .opcode = 0xf10d0000 | enc | (rd << 8) | (s << 20),
      };
    }
  }
  return th_add_sp_imm_t4(rd, imm, flags, encoding);
#else
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
#endif
}

thumb_opcode th_add_sp_reg(uint32_t rd, uint32_t rm, thumb_flags_behaviour flags, thumb_enforce_encoding encoding,
                           thumb_shift shift)
{
  if (rd == rm && flags != FLAGS_BEHAVIOUR_SET && encoding != ENFORCE_ENCODING_32BIT && shift.type == THUMB_SHIFT_NONE)
  {
    const uint16_t rdm = rd & 7;
    const uint16_t dm = rd >> 3;
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x4468 | (dm << 7) | rdm,
    };
  }

  if (rd == R_SP && flags != FLAGS_BEHAVIOUR_SET && encoding != ENFORCE_ENCODING_32BIT &&
      shift.type == THUMB_SHIFT_NONE)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x4485 | (rm << 3),
    };
  }

  if (encoding != ENFORCE_ENCODING_16BIT)
  {
    const uint32_t s = flags == FLAGS_BEHAVIOUR_SET;
    const uint32_t imm2 = shift.value & 0x3;
    const uint32_t imm3 = (shift.value >> 2) & 0x7;
    const uint32_t sr = th_shift_value_to_sr_type(shift);
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xeb0d0000 | (s << 20) | (imm3 << 12) | (rd << 8) | (imm2 << 6) | (sr << 4) | rm,
    };
  }
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_rsb_imm(uint32_t rd, uint32_t rn, uint32_t imm, thumb_flags_behaviour setflags,
                        thumb_enforce_encoding encoding)
{
  if (rd < 8 && rn < 8 && imm == 0 && setflags == FLAGS_BEHAVIOUR_SET)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x4240 | (rn << 3) | rd,
    };
  }
  else if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC)
  {
    return th_generic_op_imm_with_status(0xf1c0, rd, rn, imm, setflags);
  }
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_shift_armv7m(uint16_t rd, uint16_t rm, uint32_t imm, uint32_t type, thumb_flags_behaviour setflags)
{
  const uint32_t imm3 = (imm >> 2) & 7;
  const uint32_t imm2 = imm & 0x3;
  const uint32_t s = setflags == FLAGS_BEHAVIOUR_SET;
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xea4f0000 | (imm3 << 12) | (rd << 8) | (imm2 << 6) | (type << 4) | rm | s << 20,
  };
}

thumb_opcode th_lsl_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding)
{
  (void)shift; /* shift parameter unused for LSL_reg - shift amount is in rm */
  if (rd == rn && rm < 8 && rn < 8 && encoding != ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x4080 | (rm << 3) | rd,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC && rm != R_SP && rm != R_PC)
  {
    const uint32_t s = flags == FLAGS_BEHAVIOUR_SET;
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xfa00f000 | (rn << 16) | (rd << 8) | rm | s << 20,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_lsl_imm(uint32_t rd, uint32_t rn, uint32_t imm, thumb_flags_behaviour flags,
                        thumb_enforce_encoding encoding)
{
  thumb_shift shift = {
      .type = THUMB_SHIFT_LSL,
      .value = imm,
      .mode = THUMB_SHIFT_IMMEDIATE,
  };
  return th_mov_reg(rd, rn, flags, shift, encoding, false);
}

thumb_opcode th_lsr_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding)
{
  if (rd == rn && rm < 8 && rn < 8 && encoding != ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x40c0 | (rm << 3) | rd,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC && rm != R_SP && rm != R_PC)
  {
    const uint32_t s = flags == FLAGS_BEHAVIOUR_SET;
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xfa20f000 | (rn << 16) | (rd << 8) | rm | s << 20,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_lsr_imm(uint32_t rd, uint32_t rm, uint32_t imm, thumb_flags_behaviour flags,
                        thumb_enforce_encoding encoding)
{
  if (rm < 8 && rd < 8 && encoding != ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x0800 | (imm << 6) | (rm << 3) | rd),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (imm >= 1 && imm <= 31)
  {
    return th_shift_armv7m(rd, rm, imm, 1, flags);
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_asr_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding)
{
  if (rd == rn && rm < 8 && rn < 8 && encoding != ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x4100 | (rm << 3) | rd),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC && rm != R_SP && rm != R_PC)
  {
    const uint32_t s = flags == FLAGS_BEHAVIOUR_SET;
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xfa40f000 | (rn << 16) | (rd << 8) | rm | s << 20,
    };
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_asr_imm(uint32_t rd, uint32_t rm, uint32_t imm, thumb_flags_behaviour flags,
                        thumb_enforce_encoding encoding)
{
  if (rm < 8 && rd < 8 && encoding != ENFORCE_ENCODING_32BIT && flags == FLAGS_BEHAVIOUR_SET && imm != 0)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x1000 | (imm << 6) | (rm << 3) | rd,
    };
  }

  if (rm < 8 && rd < 8 && encoding != ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x1000 | (imm << 6) | (rm << 3) | rd,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (imm >= 1 && imm <= 31)
  {
    return th_shift_armv7m(rd, rm, imm, 2, flags);
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_mov_reg_shift(uint32_t rd, uint32_t rm, uint32_t rs, thumb_flags_behaviour flags, thumb_shift shift,
                              thumb_enforce_encoding encoding)
{
  const uint32_t s = flags == FLAGS_BEHAVIOUR_SET;
  if (rd == rm && rd < 8 && rs < 8 && encoding != ENFORCE_ENCODING_32BIT && shift.type != THUMB_SHIFT_RRX)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x4000 | (rs << 3) | th_shift_type_to_op(shift) << 6 | rd,
    };
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xfa00f000 | th_shift_value_to_sr_type(shift) << 21 | s << 20 | rm << 16 | rd << 8 | rs,
  };
}

thumb_opcode th_ror_imm(uint16_t rd, uint16_t rm, uint32_t imm, thumb_flags_behaviour flags,
                        thumb_enforce_encoding encoding)
{
  if (rm < 8 && rd < 8 && encoding != ENFORCE_ENCODING_32BIT && flags == FLAGS_BEHAVIOUR_SET && imm != 0)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x0000 | (imm << 6) | (rm << 3) | rd,
    };
  }
  if (rm < 8 && rd < 8 && encoding != ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = ((imm << 6) | (rm << 3) | rd),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (imm >= 1 && imm <= 31)
  {
    return th_shift_armv7m(rd, rm, imm, 0, flags);
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_cmp_imm(uint32_t rd, uint32_t rn, uint32_t imm, thumb_flags_behaviour flags,
                        thumb_enforce_encoding encoding)
{
  (void)rd;    /* CMP doesn't use rd - result goes to flags */
  (void)flags; /* CMP always sets flags */
  if (rn < 8 && imm <= 255 && encoding != ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x2800 | (rn << 8) | imm,
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else
  {
    const uint32_t packed = th_pack_const(imm);
    if (packed || imm == 0)
    {
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

/* VFP arithmetic instructions - single and double precision */

/* VADD.F32 Sd, Sn, Sm  or  VADD.F64 Dd, Dn, Dm
 * sz=0 for single (F32), sz=1 for double (F64)
 */
thumb_opcode th_vadd_f(uint32_t vd, uint32_t vn, uint32_t vm, uint32_t sz)
{
  uint32_t D, N, M, Vd, Vn, Vm;
  if (sz)
  {
    /* Double precision: D:Vd, N:Vn, M:Vm where D/N/M are bit 4 */
    D = (vd >> 4) & 1;
    Vd = vd & 0xf;
    N = (vn >> 4) & 1;
    Vn = vn & 0xf;
    M = (vm >> 4) & 1;
    Vm = vm & 0xf;
  }
  else
  {
    /* Single precision: Vd:D, Vn:N, Vm:M where D/N/M are bit 0 */
    D = vd & 1;
    Vd = (vd >> 1) & 0xf;
    N = vn & 1;
    Vn = (vn >> 1) & 0xf;
    M = vm & 1;
    Vm = (vm >> 1) & 0xf;
  }
  /* VADD: 1110 1110 0D11 nnnn dddd 101s N0M0 mmmm */
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xee300a00 | (D << 22) | (Vn << 16) | (Vd << 12) | (sz << 8) | (N << 7) | (M << 5) | Vm,
  };
}

/* VSUB.F32 Sd, Sn, Sm  or  VSUB.F64 Dd, Dn, Dm */
thumb_opcode th_vsub_f(uint32_t vd, uint32_t vn, uint32_t vm, uint32_t sz)
{
  uint32_t D, N, M, Vd, Vn, Vm;
  if (sz)
  {
    D = (vd >> 4) & 1;
    Vd = vd & 0xf;
    N = (vn >> 4) & 1;
    Vn = vn & 0xf;
    M = (vm >> 4) & 1;
    Vm = vm & 0xf;
  }
  else
  {
    D = vd & 1;
    Vd = (vd >> 1) & 0xf;
    N = vn & 1;
    Vn = (vn >> 1) & 0xf;
    M = vm & 1;
    Vm = (vm >> 1) & 0xf;
  }
  /* VSUB: 1110 1110 0D11 nnnn dddd 101s N1M0 mmmm */
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xee300a40 | (D << 22) | (Vn << 16) | (Vd << 12) | (sz << 8) | (N << 7) | (M << 5) | Vm,
  };
}

/* VMUL.F32 Sd, Sn, Sm  or  VMUL.F64 Dd, Dn, Dm */
thumb_opcode th_vmul_f(uint32_t vd, uint32_t vn, uint32_t vm, uint32_t sz)
{
  uint32_t D, N, M, Vd, Vn, Vm;
  if (sz)
  {
    D = (vd >> 4) & 1;
    Vd = vd & 0xf;
    N = (vn >> 4) & 1;
    Vn = vn & 0xf;
    M = (vm >> 4) & 1;
    Vm = vm & 0xf;
  }
  else
  {
    D = vd & 1;
    Vd = (vd >> 1) & 0xf;
    N = vn & 1;
    Vn = (vn >> 1) & 0xf;
    M = vm & 1;
    Vm = (vm >> 1) & 0xf;
  }
  /* VMUL: 1110 1110 0D10 nnnn dddd 101s N0M0 mmmm */
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xee200a00 | (D << 22) | (Vn << 16) | (Vd << 12) | (sz << 8) | (N << 7) | (M << 5) | Vm,
  };
}

/* VDIV.F32 Sd, Sn, Sm  or  VDIV.F64 Dd, Dn, Dm */
thumb_opcode th_vdiv_f(uint32_t vd, uint32_t vn, uint32_t vm, uint32_t sz)
{
  uint32_t D, N, M, Vd, Vn, Vm;
  if (sz)
  {
    D = (vd >> 4) & 1;
    Vd = vd & 0xf;
    N = (vn >> 4) & 1;
    Vn = vn & 0xf;
    M = (vm >> 4) & 1;
    Vm = vm & 0xf;
  }
  else
  {
    D = vd & 1;
    Vd = (vd >> 1) & 0xf;
    N = vn & 1;
    Vn = (vn >> 1) & 0xf;
    M = vm & 1;
    Vm = (vm >> 1) & 0xf;
  }
  /* VDIV: 1110 1110 1D00 nnnn dddd 101s N0M0 mmmm */
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xee800a00 | (D << 22) | (Vn << 16) | (Vd << 12) | (sz << 8) | (N << 7) | (M << 5) | Vm,
  };
}

/* VNEG.F32 Sd, Sm  or  VNEG.F64 Dd, Dm */
thumb_opcode th_vneg_f(uint32_t vd, uint32_t vm, uint32_t sz)
{
  uint32_t D, M, Vd, Vm;
  if (sz)
  {
    D = (vd >> 4) & 1;
    Vd = vd & 0xf;
    M = (vm >> 4) & 1;
    Vm = vm & 0xf;
  }
  else
  {
    D = vd & 1;
    Vd = (vd >> 1) & 0xf;
    M = vm & 1;
    Vm = (vm >> 1) & 0xf;
  }
  /* VNEG: 1110 1110 1D11 0001 dddd 101s 01M0 mmmm */
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xeeb10a40 | (D << 22) | (Vd << 12) | (sz << 8) | (M << 5) | Vm,
  };
}

/* VCMP.F32 Sd, Sm  or  VCMP.F64 Dd, Dm
 * Compares and sets FPSCR flags
 */
thumb_opcode th_vcmp_f(uint32_t vd, uint32_t vm, uint32_t sz)
{
  uint32_t D, M, Vd, Vm;
  if (sz)
  {
    D = (vd >> 4) & 1;
    Vd = vd & 0xf;
    M = (vm >> 4) & 1;
    Vm = vm & 0xf;
  }
  else
  {
    D = vd & 1;
    Vd = (vd >> 1) & 0xf;
    M = vm & 1;
    Vm = (vm >> 1) & 0xf;
  }
  /* VCMP: 1110 1110 1D11 0100 dddd 101s E1M0 mmmm (E=0 for quiet compare) */
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xeeb40a40 | (D << 22) | (Vd << 12) | (sz << 8) | (M << 5) | Vm,
  };
}

/* VCMPE.F32 Sd, Sm  or  VCMPE.F64 Dd, Dm
 * Compares and sets FPSCR flags, signals exception on any NaN
 */
thumb_opcode th_vcmpe_f(uint32_t vd, uint32_t vm, uint32_t sz)
{
  uint32_t D, M, Vd, Vm;
  if (sz)
  {
    D = (vd >> 4) & 1;
    Vd = vd & 0xf;
    M = (vm >> 4) & 1;
    Vm = vm & 0xf;
  }
  else
  {
    D = vd & 1;
    Vd = (vd >> 1) & 0xf;
    M = vm & 1;
    Vm = (vm >> 1) & 0xf;
  }
  /* VCMPE: 1110 1110 1D11 0100 dddd 101s E1M0 mmmm (E=1) */
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xeeb40ac0 | (D << 22) | (Vd << 12) | (sz << 8) | (M << 5) | Vm,
  };
}

thumb_opcode th_vpush(uint32_t regs, uint32_t is_doubleword)
{
  int first_register = 0;
  int register_count = 0;
  uint32_t D = 0;
  uint32_t Vd = 0;
  for (int i = 0; i < 32; i++)
  {
    if (regs & (1 << i))
    {
      first_register = i;
      break;
    }
  }

  register_count = 0;
  for (int i = 0; i < 32; i++)
  {
    if (regs & (1 << i))
    {
      register_count++;
    }
  }

  if (is_doubleword)
  {
    D = first_register >> 4;
    Vd = first_register & 0xf;
    register_count <<= 1;
  }
  else
  {
    D = first_register & 1;
    Vd = first_register >> 1;
  }

  return (thumb_opcode){
      .size = 4,
      .opcode = 0xed2d0a00 | D << 22 | (Vd << 12) | (register_count & 0xff) | (is_doubleword << 8),
  };
}

thumb_opcode th_vpop(uint32_t regs, uint32_t is_doubleword)
{
  int first_register = 0;
  int register_count = 0;
  uint32_t D = 0;
  uint32_t Vd = 0;
  for (int i = 0; i < 32; i++)
  {
    if (regs & (1 << i))
    {
      first_register = i;
      break;
    }
  }

  register_count = 0;
  for (int i = 0; i < 32; i++)
  {
    if (regs & (1 << i))
    {
      register_count++;
    }
  }

  if (is_doubleword)
  {
    D = first_register >> 4;
    Vd = first_register & 0xf;
    register_count <<= 1;
  }
  else
  {
    D = first_register & 1;
    Vd = first_register >> 1;
  }

  return (thumb_opcode){
      .size = 4,
      .opcode = 0xecbd0a00 | D << 22 | (Vd << 12) | (register_count & 0xff) | (is_doubleword << 8),
  };
}

thumb_opcode th_vmov_register(uint16_t vd, uint16_t vm, uint32_t sz)
{
  if (sz == 0)
  {
    /* Single precision: S-register number 0-31, D bit is bit 0 */
    if (vd <= 0x1f && vm <= 0x1f)
    {
      const uint16_t d = vd & 1;
      const uint16_t m = vm & 1;
      vd >>= 1;
      vm >>= 1;
      return (thumb_opcode){
          .size = 4,
          .opcode = 0xeeb00a40 | (d << 22) | (vd << 12) | (m << 5) | vm | (sz << 8),
      };
    }
  }
  else
  {
    /* Double precision: D-register number 0-15, no bit splitting needed */
    if (vd <= 0x0f && vm <= 0x0f)
    {
      return (thumb_opcode){
          .size = 4,
          .opcode = 0xeeb00b40 | (vd << 12) | vm, /* sz=1 -> bit 8 set -> 0xb */
      };
    }
  }
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_vldr(uint32_t rn, uint32_t vd, uint32_t add, uint32_t is_doubleword, uint32_t imm)
{
  const uint32_t D = (vd >> 4) & 1;
  if (imm > 1020 || (imm & 0x3))
  {
    tcc_error("compiler_error: 'th_vldr' imm is outside of range: 0x%x, max "
              "value: 0xff\n",
              imm);
  }
  if (is_doubleword)
  {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xed100b00 | (D << 22) | ((add & 1) << 23) | (rn << 16) | (vd << 12) | (imm >> 2),
    };
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xed100a00 | ((add & 1) << 23) | (D << 22) | (rn << 16) | (vd << 12) | (imm >> 2),
  };
}

thumb_opcode th_vstr(uint32_t rn, uint32_t vd, uint32_t add, uint32_t is_doubleword, uint32_t imm)
{
  const uint32_t D = (vd >> 4) & 1;
  if (imm > 1020 || (imm & 0x3))
  {
    tcc_error("compiler_error: 'th_vstr' imm is outside of range: 0x%x, max "
              "value: 0xff\n",
              imm);
  }
  if (is_doubleword)
  {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xed000b00 | (D << 22) | ((add & 1) << 23) | (rn << 16) | (vd << 12) | (imm >> 2),

    };
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xed000a00 | (D << 22) | ((add & 1) << 23) | (rn << 16) | (vd << 12) | (imm >> 2),
  };
}

// move between core general purpose register and single precision floating
// point register
thumb_opcode th_vmov_gp_sp(uint16_t rt, uint16_t sn, uint16_t to_arm_register)
{
  /* Sn encoding: Vn (bits 19:16) = Sn[4:1], N (bit 7) = Sn[0] */
  const uint16_t Vn = (sn >> 1) & 0xf;
  const uint16_t N = sn & 1;
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xee000a10 | (to_arm_register << 20) | (Vn << 16) | (rt << 12) | (N << 7),
  };
}

// move between two general purpose registers and one doubleword register
thumb_opcode th_vmov_2gp_dp(uint16_t rt, uint16_t rt2, uint16_t dm, uint16_t to_arm_register)
{
  const uint16_t M = (dm >> 4) & 1;
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xec400b10 | (to_arm_register << 20) | (rt2 << 16) | (rt << 12) | (M << 5) | dm,
  };
}

thumb_opcode th_sub_sp_imm_t3(uint32_t rd, uint32_t imm, thumb_flags_behaviour flags, thumb_enforce_encoding encoding)
{
  if (rd != R_PC && imm <= 4095 && encoding != ENFORCE_ENCODING_16BIT && flags != FLAGS_BEHAVIOUR_SET)
  {
    const uint32_t i = (imm >> 11) & 1;
    const uint32_t imm3 = (imm >> 8) & 0x7;
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf2ad0000 | (i << 26) | (imm3 << 12) | (rd << 8) | (imm & 0xff),
    };
  }
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_sub_sp_imm(uint32_t rd, uint32_t imm, thumb_flags_behaviour flags, thumb_enforce_encoding encoding)
{
  // T1 encoding
  if (rd == R_SP && imm <= 508 && !(imm & 0x3) && encoding != ENFORCE_ENCODING_32BIT && flags != FLAGS_BEHAVIOUR_SET)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xb080 | (imm >> 2),
    };
  }

  if (rd != R_PC)
  {
    const uint32_t enc = th_pack_const(imm);
    const uint32_t s = flags == FLAGS_BEHAVIOUR_SET ? 1 : 0;
    if (enc || imm == 0)
    {
      return (thumb_opcode){
          .size = 4,
          .opcode = 0xf1ad0000 | s << 20 | (rd << 8) | enc,
      };
    }
  }

  return th_sub_sp_imm_t3(rd, imm, flags, encoding);
}

thumb_opcode th_vmrs(uint16_t rt)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xeef10a10 | (rt << 12),
  };
}

thumb_opcode th_vcvt_float_to_double(uint32_t vd, uint32_t vm)
{
  /* VCVT.F64.F32 Dd, Sm
   * vd = destination Dd index (0-15), vm = source Sm index (0-31)
   * Sm encoding: M = Sm[0] (bit 5), Vm = Sm[4:1] (bits 3:0)
   */
  uint32_t M = vm & 1;
  uint32_t Vm = (vm >> 1) & 0xf;
  return (thumb_opcode){
      .size = 4,
      .opcode = (0xeeb70ac0 | (vd << 12) | (M << 5) | Vm),
  };
}

thumb_opcode th_vcvt_double_to_float(uint32_t vd, uint32_t vm)
{
  /* VCVT.F32.F64 Sd, Dm
   * vd = destination Sd index (0-31), vm = source Dm index (0-15)
   * Sd encoding: D = Sd[0] (bit 22), Vd = Sd[4:1] (bits 15:12)
   */
  uint32_t D = vd & 1;
  uint32_t Vd = (vd >> 1) & 0xf;
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xeeb70bc0 | (D << 22) | (Vd << 12) | vm,
  };
}

thumb_opcode th_vcvt_fp_int(uint32_t vd, uint32_t vm, uint32_t opc, uint32_t is_double, uint32_t op)
{
  /* VCVT.S32.F32 or VCVT.S32.F64 - floating-point to integer
   * vd = destination Sd (single register index 0-31)
   * vm = source Sm for single, Dm for double
   * opc = operation: 4=unsigned, 5=signed (round toward zero)
   * is_double = 0 for F32 source, 1 for F64 source
   * op = 1 for fp-to-int, 0 for int-to-fp
   */
  uint32_t D = (vd >> 4) & 1;      /* Sd[4] */
  uint32_t Vd = vd & 0xf;          /* Sd[3:0] */
  uint32_t sz = is_double ? 1 : 0; /* bit 8: 0=F32, 1=F64 source */
  uint32_t M, Vm;

  /* Both single and double use Sm/Dm = Vm:M encoding */
  M = vm & 1;
  Vm = (vm >> 1) & 0xf;

  return (thumb_opcode){
      .size = 4,
      .opcode = 0xeeb80a40 | (D << 22) | (opc << 16) | (Vd << 12) | (sz << 8) | (op << 7) | (M << 5) | Vm,
  };
}

thumb_opcode th_vcvt_convert(uint32_t vd, uint32_t vm, const char *dest_type, const char *src_type)
{
  // Helper function for VCVT conversions with type strings
  // Examples: dest_type="s32", src_type="f32" for vcvt.s32.f32

  // Float to int conversion (f32/f64 -> s32/u32)
  if ((strcmp(dest_type, "s32") == 0 || strcmp(dest_type, "u32") == 0) && strcmp(src_type, "f32") == 0)
  {
    int is_unsigned = strcmp(dest_type, "u32") == 0;
    return th_vcvt_fp_int(vd, vm, is_unsigned ? 0x4 : 0x5, 0, 1);
  }
  else if ((strcmp(dest_type, "s32") == 0 || strcmp(dest_type, "u32") == 0) && strcmp(src_type, "f64") == 0)
  {
    int is_unsigned = strcmp(dest_type, "u32") == 0;
    return th_vcvt_fp_int(vd, vm, is_unsigned ? 0x4 : 0x5, 1, 1);
  }
  // Int to float conversion (s32/u32 -> f32/f64)
  else if ((strcmp(dest_type, "f32") == 0 || strcmp(dest_type, "f64") == 0) &&
           (strcmp(src_type, "s32") == 0 || strcmp(src_type, "u32") == 0))
  {
    int dst_is_double = strcmp(dest_type, "f64") == 0;
    int is_unsigned = strcmp(src_type, "u32") == 0;
    return th_vcvt_fp_int(vd, vm, 0, dst_is_double, is_unsigned ? 0 : 1);
  }
  // Float precision conversion (f32 <-> f64)
  else if (strcmp(dest_type, "f64") == 0 && strcmp(src_type, "f32") == 0)
  {
    return th_vcvt_float_to_double(vd / 2, vm);
  }
  else if (strcmp(dest_type, "f32") == 0 && strcmp(src_type, "f64") == 0)
  {
    return th_vcvt_double_to_float(vd, vm / 2);
  }

  // Unsupported conversion
  return (thumb_opcode){.size = 0, .opcode = 0};
}

thumb_opcode th_it(uint16_t cond, uint16_t mask)
{
  return (thumb_opcode){
      .size = 2,
      .opcode = 0xbf00 | (cond << 4) | (mask & 0xf),
  };
}

thumb_opcode th_clrex()
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf3bf8f2f,
  };
}

thumb_opcode th_svc(uint32_t imm)
{
  if (imm <= 0xff)
  {
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

thumb_opcode th_bkpt(uint32_t imm)
{
  if (imm <= 0xff)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xbe00 | imm,
    };
  }
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_bfc(uint32_t rd, uint32_t lsb, uint32_t width)
{
  const uint32_t imm2 = lsb & 0x3;
  const uint32_t imm3 = (lsb >> 2) & 0x7;
  const uint32_t msb = lsb + width - 1;
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf36f0000 | (rd << 8) | (imm3 << 12) | (imm2 << 6) | msb,
  };
}

thumb_opcode th_bfi(uint32_t rd, uint32_t rn, uint32_t lsb, uint32_t width)
{
  const uint32_t imm2 = lsb & 0x3;
  const uint32_t imm3 = (lsb >> 2) & 0x7;
  const uint32_t msb = lsb + width - 1;
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf3600000 | (rn << 16) | (rd << 8) | (imm3 << 12) | (imm2 << 6) | msb,
  };
}

thumb_opcode th_clz(uint32_t rd, uint32_t rm)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xfab0f080 | rm << 16 | rd << 8 | rm,
  };
}

thumb_opcode th_cmn_imm(uint32_t rn, uint32_t imm)
{
#ifndef TCC_TARGET_ARM_ARCHV6M
  if (rn != R_PC)
  {
    const uint32_t packed = th_pack_const(imm);
    if (packed || imm == 0)
    {
      return (thumb_opcode){
          .size = 4,
          .opcode = 0xf1100f00 | packed | (rn << 16),
      };
    }
  }
#endif
  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_cmn_reg(uint32_t rn, uint32_t rm, thumb_shift shift, thumb_enforce_encoding encoding)
{
  if (rn < 8 && rm < 8 && shift.type == THUMB_SHIFT_NONE && encoding != ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x42c0 | (rm << 3) | rn,
    };
  }
  return th_generic_op_reg_shift_with_status(0xeb10, 0xf, rn, rm, FLAGS_BEHAVIOUR_SET, shift);
}

thumb_opcode th_cps(uint32_t enable, uint32_t i, uint32_t f)
{
  return (thumb_opcode){
      .size = 2,
      .opcode = 0xb660 | (enable << 4) | (i << 1) | f,
  };
}

thumb_opcode th_csdb()
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf3af8014,
  };
}

thumb_opcode th_dmb(uint32_t option)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf3bf8f50 | option,
  };
}

thumb_opcode th_dsb(uint32_t option)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf3bf8f40 | option,
  };
}

thumb_opcode th_isb(uint32_t option)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf3bf8f60 | option,
  };
}

thumb_opcode th_eor_imm(uint32_t rd, uint32_t rn, uint32_t imm, thumb_flags_behaviour flags,
                        thumb_enforce_encoding encoding)
{

  uint32_t S = (flags == FLAGS_BEHAVIOUR_SET) ? 1 : 0;
  uint32_t packed = th_pack_const(imm);
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf0800000 | (S << 20) | (rd << 8) | (rn << 16) | packed,
  };
}

thumb_opcode th_eor_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding)
{
  if (rd == rn && rm < 8 && rn < 8 && encoding != ENFORCE_ENCODING_32BIT && shift.type == THUMB_SHIFT_NONE)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x4040 | (rm << 3) | rd),
    };
  }
  return th_generic_op_reg_shift_with_status(0xea80, rd, rn, rm, flags, shift);
}

thumb_opcode th_lda(uint32_t rt, uint32_t rn)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe8d00faf | (rn << 16) | (rt << 12),
  };
}

thumb_opcode th_ldab(uint32_t rt, uint32_t rn)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe8d00f8f | (rn << 16) | (rt << 12),
  };
}

thumb_opcode th_ldaex(uint32_t rt, uint32_t rn)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe8d00fef | (rn << 16) | (rt << 12),
  };
}

thumb_opcode th_ldaexb(uint32_t rt, uint32_t rn)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe8d00fcf | (rn << 16) | (rt << 12),
  };
}

thumb_opcode th_ldaexh(uint32_t rt, uint32_t rn)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe8d00fdf | (rn << 16) | (rt << 12),
  };
}

thumb_opcode th_ldah(uint32_t rt, uint32_t rn)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe8d00f9f | (rn << 16) | (rt << 12),
  };
}

thumb_opcode th_ldm(uint32_t rn, uint32_t regset, uint32_t writeback, thumb_enforce_encoding encoding)
{
  if (rn < 8 && regset <= 0xff && encoding != ENFORCE_ENCODING_32BIT && writeback == 1)
  {
    if (writeback)
    {
      regset &= ~(1 << rn);
    }
    else
    {
      regset |= 1 << rn;
    }
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xc800 | rn << 8 | regset,
    };
  };
  if (rn == R_SP && ((regset & 0x7f00) == 0) && encoding != ENFORCE_ENCODING_32BIT && writeback == 1)
  {
    const uint8_t p = (regset >> R_PC) & 1;
    regset &= 0x00ff;
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xbc00 | regset | (p << 8),
    };
  }

  if (!(writeback && (regset & (1 << rn))))
  {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xe8900000 | (writeback << 21) | (rn << 16) | regset,
    };
  }

  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_ldmdb(uint32_t rn, uint32_t regset, uint32_t writeback)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe9100000 | (writeback << 21) | (rn << 16) | regset,
  };
}

thumb_opcode th_ldrbt(uint32_t rt, uint32_t rn, int imm)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf8100e00 | (rn << 16) | (rt << 12) | (imm & 0xff),
  };
}

thumb_opcode th_ldrd_imm(uint32_t rt, uint32_t rt2, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding encoding)
{
  const uint32_t pu = (puw >> 1) & 0x3;
  const uint32_t w = puw & 0x1;
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe8500000 | (pu << 23) | w << 21 | rn << 16 | rt << 12 | rt2 << 8 | (imm >> 2),
  };
}

thumb_opcode th_ldrex(uint32_t rt, uint32_t rn, int imm)
{
  if (imm < 0 || imm > 1020)
  {
    tcc_error("compiler_error: 'th_ldrex' imm is outside of range: 0x%x, max "
              "value: 0x3fc\n",
              imm);
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe8500f00 | (rn << 16) | (rt << 12) | (imm >> 2),
  };
}

thumb_opcode th_ldrexb(uint32_t rt, uint32_t rn)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe8d00f4f | (rn << 16) | (rt << 12),
  };
}

thumb_opcode th_ldrexh(uint32_t rt, uint32_t rn)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe8d00f5f | (rn << 16) | (rt << 12),
  };
}

thumb_opcode th_ldrht(uint32_t rt, uint32_t rn, int imm)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf8300e00 | (rn << 16) | (rt << 12) | (imm & 0xff),
  };
}

thumb_opcode th_ldrsbt(uint32_t rt, uint32_t rn, int imm)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf9100e00 | (rn << 16) | (rt << 12) | (imm & 0xff),
  };
}

thumb_opcode th_ldrsht(uint32_t rt, uint32_t rn, int imm)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf9300e00 | (rn << 16) | (rt << 12) | (imm & 0xff),
  };
}

thumb_opcode th_ldrt(uint32_t rt, uint32_t rn, int imm)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf8500e00 | (rn << 16) | (rt << 12) | (imm & 0xff),
  };
}

thumb_opcode th_mla(uint32_t rd, uint32_t rn, uint32_t rm, uint32_t ra)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xfb000000 | (rn << 16) | (ra << 12) | (rd << 8) | rm,
  };
}

thumb_opcode th_mls(uint32_t rd, uint32_t rn, uint32_t rm, uint32_t ra)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xfb000010 | (rn << 16) | (ra << 12) | (rd << 8) | rm,
  };
}

thumb_opcode th_mrs(uint32_t rd, uint32_t specreg)
{
  if (rd == R_SP || rd == R_PC)
  {
    tcc_error("compiler_error: 'th_msr', SP or PC can't be used as rd\n");
    return (thumb_opcode){0, 0};
  }
  if (specreg > 0xff)
  {
    tcc_error("compiler_error: 'th_msr', invalid special register\n");
    return (thumb_opcode){0, 0};
  }

  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf3ef8000 | (rd << 8) | specreg,
  };
}

thumb_opcode th_msr(uint32_t specreg, uint32_t rn, uint32_t mask)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf3808000 | (mask << 10) | (rn << 16) | specreg,
  };
}

thumb_opcode th_mvn_imm(uint32_t rd, uint32_t rm, uint32_t imm, thumb_flags_behaviour flags,
                        thumb_enforce_encoding encoding)
{

  uint32_t S = (flags == FLAGS_BEHAVIOUR_SET) ? 1 : 0;
  uint32_t packed = th_pack_const(imm);
  if (packed == 0)
  {
    return (thumb_opcode){
        .size = 0,
        .opcode = 0,
    };
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf06f0000 | (S << 20) | (rd << 8) | packed,
  };
}

thumb_opcode th_mvn_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding)
{
  if (rd == rn && rm < 8 && encoding != ENFORCE_ENCODING_32BIT && shift.type == THUMB_SHIFT_NONE)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = (0x43c0 | (rm << 3) | rd),
    };
  }
  return th_generic_op_reg_shift_with_status(0xea6f, rd, rn, rm, flags, shift);
}

thumb_opcode th_orn_imm(uint32_t rd, uint32_t rn, uint32_t imm, thumb_flags_behaviour flags,
                        thumb_enforce_encoding encoding)
{

  uint32_t S = (flags == FLAGS_BEHAVIOUR_SET) ? 1 : 0;
  uint32_t packed = th_pack_const(imm);
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf0600000 | (S << 20) | (rd << 8) | (rn << 16) | packed,
  };
}

thumb_opcode th_orn_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding)
{
  return th_generic_op_reg_shift_with_status(0xea60, rd, rn, rm, flags, shift);
}

thumb_opcode th_pkhbt(uint32_t rd, uint32_t rn, uint32_t rm, thumb_shift shift)
{
  const uint32_t imm2 = shift.value & 0x3;
  const uint32_t imm3 = (shift.value >> 2) & 0x7;
  uint32_t tb = 0;
  if (shift.type == THUMB_SHIFT_LSL || shift.value == 0)
  {
    tb = 0;
  }
  else if (shift.type == THUMB_SHIFT_ASR)
  {
    tb = 1;
  }
  else
  {
    tcc_error("compiler_error: 'th_pkhbt', invalid shift type\n");
    return (thumb_opcode){0, 0};
  }

  return (thumb_opcode){
      .size = 4,
      .opcode = 0xeac00000 | rn << 16 | imm3 << 12 | rd << 8 | imm2 << 6 | tb << 5 | rm,
  };
}

thumb_opcode th_pld_literal(int imm)
{
  int u = 1;
  if (imm < 0)
  {
    u = 0;
    imm = -imm;
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf81ff000 | u << 23 | imm,
  };
}

thumb_opcode th_pld_imm(uint32_t rn, uint32_t w, int imm)
{
  if (imm >= 0)
  {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf890f000 | w << 22 | rn << 16 | imm,
    };
  }
  imm = -imm;
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf810fc00 | w << 22 | rn << 16 | imm,
  };
}

thumb_opcode th_pld_reg(uint32_t rn, uint32_t rm, uint32_t w, thumb_shift shift)
{
  if (shift.type == THUMB_SHIFT_NONE)
  {
    shift.type = THUMB_SHIFT_LSL;
  }
  if (shift.type != THUMB_SHIFT_LSL || shift.value > 3 || shift.value < 0)
  {
    tcc_error("compiler_error: 'th_pld_reg', invalid shift type\n");
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf810f000 | w << 22 | rn << 16 | rm | shift.value << 4,
  };
}

thumb_opcode th_pli_literal(int imm)
{
  int u = 1;
  if (imm < 0)
  {
    u = 0;
    imm = -imm;
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf91ff000 | u << 23 | imm,
  };
}

thumb_opcode th_pli_imm(uint32_t rn, uint32_t w, int imm)
{
  if (imm >= 0)
  {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf990f000 | w << 22 | rn << 16 | imm,
    };
  }
  imm = -imm;
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf910fc00 | w << 22 | rn << 16 | imm,
  };
}

thumb_opcode th_pli_reg(uint32_t rn, uint32_t rm, uint32_t w, thumb_shift shift)
{
  if (shift.type == THUMB_SHIFT_NONE)
  {
    shift.type = THUMB_SHIFT_LSL;
  }
  if (shift.type != THUMB_SHIFT_LSL || shift.value > 3 || shift.value < 0)
  {
    tcc_error("compiler_error: 'th_pli_reg', invalid shift type\n");
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf910f000 | w << 22 | rn << 16 | rm | shift.value << 4,
  };
}

thumb_opcode th_rbit(uint32_t rd, uint32_t rm)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xfa90f0a0 | (rm << 16) | (rd << 8) | rm,
  };
}

thumb_opcode th_rev(uint32_t rd, uint32_t rm, thumb_enforce_encoding encoding)
{
  if (rd < 8 && rm < 8 && encoding != ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xba00 | (rm << 3) | rd,
    };
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xfa90f080 | (rm << 16) | (rd << 8) | rm,
  };
}

thumb_opcode th_rev16(uint32_t rd, uint32_t rm, thumb_enforce_encoding encoding)
{
  if (rd < 8 && rm < 8 && encoding != ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xba40 | (rm << 3) | rd,
    };
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xfa90f090 | (rm << 16) | (rd << 8) | rm,
  };
}

thumb_opcode th_revsh(uint32_t rd, uint32_t rm, thumb_enforce_encoding encoding)
{
  if (rd < 8 && rm < 8 && encoding != ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xbac0 | (rm << 3) | rd,
    };
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xfa90f0b0 | (rm << 16) | (rd << 8) | rm,
  };
}

thumb_opcode th_sbfx(uint32_t rd, uint32_t rn, uint32_t lsb, uint32_t width)
{
  const uint32_t imm2 = lsb & 0x3;
  const uint32_t imm3 = (lsb >> 2) & 0x7;
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf3400000 | (rn << 16) | (rd << 8) | (imm3 << 12) | (imm2 << 6) | (width - 1),
  };
}

thumb_opcode th_smlal(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xfbc00000 | (rn << 16) | (rdlo << 12) | (rdhi << 8) | rm,
  };
}

thumb_opcode th_smull(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xfb800000 | (rn << 16) | (rdlo << 12) | (rdhi << 8) | rm,
  };
}

thumb_opcode th_ssat(uint32_t rd, uint32_t imm, uint32_t rn, thumb_shift shift)
{
  const uint32_t sh = (shift.type == THUMB_SHIFT_LSL) ? 0 : 1;
  const uint32_t imm2 = shift.value & 0x3;
  const uint32_t imm3 = (shift.value >> 2) & 0x7;

  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf3000000 | (sh << 21) | (rn << 16) | (imm3 << 12) | (rd << 8) | (imm2 << 6) | (imm - 1),
  };
}

thumb_opcode th_usat(uint32_t rd, uint32_t imm, uint32_t rn, thumb_shift shift)
{
  const uint32_t sh = (shift.type == THUMB_SHIFT_LSL) ? 0 : 1;
  const uint32_t imm2 = shift.value & 0x3;
  const uint32_t imm3 = (shift.value >> 2) & 0x7;

  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf3800000 | (sh << 21) | (rn << 16) | (imm3 << 12) | (rd << 8) | (imm2 << 6) | imm,
  };
}

thumb_opcode th_ssbb()
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf3bf8f40,
  };
}

thumb_opcode th_stl(uint32_t rt, uint32_t rn)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe8c00faf | rn << 16 | rt << 12,
  };
}

thumb_opcode th_stlb(uint32_t rt, uint32_t rn)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe8c00f8f | rn << 16 | rt << 12,
  };
}

thumb_opcode th_stlex(uint32_t rd, uint32_t rt, uint32_t rn)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe8c00fe0 | rn << 16 | rt << 12 | rd,
  };
}

thumb_opcode th_stlexb(uint32_t rd, uint32_t rt, uint32_t rn)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe8c00fc0 | rn << 16 | rt << 12 | rd,
  };
}

thumb_opcode th_stlexh(uint32_t rd, uint32_t rt, uint32_t rn)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe8c00fd0 | rn << 16 | rt << 12 | rd,
  };
}

thumb_opcode th_stlh(uint32_t rt, uint32_t rn)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe8c00f9f | rn << 16 | rt << 12,
  };
}

thumb_opcode th_stm(uint32_t rn, uint32_t regset, uint32_t writeback, thumb_enforce_encoding encoding)
{
  if (rn < 8 && regset <= 0xff && encoding != ENFORCE_ENCODING_32BIT && writeback == 1)
  {
    if (writeback)
    {
      regset &= ~(1 << rn);
    }
    else
    {
      regset |= 1 << rn;
    }
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xc000 | rn << 8 | regset,
    };
  };

  if (!(writeback && (regset & (1 << rn))))
  {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xe8800000 | (writeback << 21) | (rn << 16) | regset,
    };
  }

  return (thumb_opcode){
      .size = 0,
      .opcode = 0,
  };
}

thumb_opcode th_stmdb(uint32_t rn, uint32_t regset, uint32_t writeback, thumb_enforce_encoding encoding)
{

  if (rn == R_SP && encoding != ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xb400 | writeback << 8 | (regset & 0xff),
    };
  }

  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe9000000 | (writeback << 21) | (rn << 16) | regset,
  };
}

thumb_opcode th_str_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding encoding)
{
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (puw == 6 && rn < 8 && rt < 8 && imm <= 124 && !(imm & 3) && encoding != ENFORCE_ENCODING_32BIT)
  {
    // imm[0] is enforced to be 0, and sould be divided by 4, thus offset is 4
    THOP_TRACE("str %s, [%s, #%d]\n", th_reg_name(rt), th_reg_name(rn), imm);
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x6000 | (imm << 4) | (rn << 3) | rt,
    };
  }
  else if (puw == 6 && rn == R_SP && rt < 8 && imm <= 1020 && encoding != ENFORCE_ENCODING_32BIT)
  {
    THOP_TRACE("str %s, [%s, #%d]\n", th_reg_name(rt), th_reg_name(rn), imm);
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x9000 | (rt << 8) | (imm >> 2),
    };
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (puw == 6 && imm <= 4095 && rn != R_PC)
  {
    uint32_t ins = (0xf8c0 | (rn & 0xf)) << 16;
    ins |= (rt << 12) | imm;
    THOP_TRACE("str %s, [%s, #%d]\n", th_reg_name(rt), th_reg_name(rn), imm);
    return (thumb_opcode){
        .size = 4,
        .opcode = ins,
    };
  }
  else if (imm >= 0 && imm <= 4095 && rn == R_PC)
  {
    uint32_t u = (puw & 0x2) >> 1;
    THOP_TRACE("str %s, [%s, #%c%d]\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf85f0000 | (u << 23) | (rt << 12) | imm,
    };
  }
  else if (imm <= 255)
  {
    uint32_t ins = (0xf840 | (rn & 0xf)) << 16;
    ins |= (0x0800 | ((rt & 0xf) << 12) | ((puw & 0x7) << 8) | imm);
    {
      const uint32_t p = (puw >> 2) & 1;
      const uint32_t u = (puw >> 1) & 1;
      const uint32_t w = (puw >> 0) & 1;
      if (p && !w)
      {
        THOP_TRACE("str %s, [%s, #%c%d]\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else if (p && w)
      {
        THOP_TRACE("str %s, [%s, #%c%d]!\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else if (!p && w)
      {
        THOP_TRACE("str %s, [%s], #%c%d\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm);
      }
      else
      {
        THOP_TRACE("str %s, [%s, #%c%d] (puw=%u)\n", th_reg_name(rt), th_reg_name(rn), u ? '+' : '-', imm,
                   (unsigned)puw);
      }
    }
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

thumb_opcode th_strbt(uint32_t rt, uint32_t rn, int imm)
{
  THOP_TRACE("strbt %s, [%s], #%d\n", th_reg_name(rt), th_reg_name(rn), imm);
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf8000e00 | (rn << 16) | (rt << 12) | (imm & 0xff),
  };
}

thumb_opcode th_strd_imm(uint32_t rt, uint32_t rt2, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding encoding)
{
  const uint32_t pu = (puw >> 1) & 0x3;
  const uint32_t w = puw & 0x1;
  THOP_TRACE("strd %s, %s, [%s, #%d]%s\n", th_reg_name(rt), th_reg_name(rt2), th_reg_name(rn), imm, w ? "!" : "");
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe8400000 | (pu << 23) | w << 21 | rn << 16 | rt << 12 | rt2 << 8 | (imm >> 2),
  };
}

thumb_opcode th_strex(uint32_t rd, uint32_t rt, uint32_t rn, int imm)
{
  if (imm < 0 || imm > 1020)
  {
    tcc_error("compiler_error: 'th_strex' imm is outside of range: 0x%x, max "
              "value: 0x3fc\n",
              imm);
  }
  THOP_TRACE("strex %s, %s, [%s, #%d]\n", th_reg_name(rd), th_reg_name(rt), th_reg_name(rn), imm);
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe8400000 | (rn << 16) | (rt << 12) | (rd << 8) | (imm >> 2),
  };
}

thumb_opcode th_strexb(uint32_t rd, uint32_t rt, uint32_t rn)
{
  THOP_TRACE("strexb %s, %s, [%s]\n", th_reg_name(rd), th_reg_name(rt), th_reg_name(rn));
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe8c00f40 | (rn << 16) | (rt << 12) | rd,
  };
}

thumb_opcode th_strexh(uint32_t rd, uint32_t rt, uint32_t rn)
{
  THOP_TRACE("strexh %s, %s, [%s]\n", th_reg_name(rd), th_reg_name(rt), th_reg_name(rn));
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe8c00f50 | (rn << 16) | (rt << 12) | rd,
  };
}

thumb_opcode th_strht(uint32_t rt, uint32_t rn, int imm)
{
  THOP_TRACE("strht %s, [%s], #%d\n", th_reg_name(rt), th_reg_name(rn), imm);
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf8200e00 | (rn << 16) | (rt << 12) | (imm & 0xff),
  };
}

thumb_opcode th_strt(uint32_t rt, uint32_t rn, int imm)
{
  THOP_TRACE("strt %s, [%s], #%d\n", th_reg_name(rt), th_reg_name(rn), imm);
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf8400e00 | (rn << 16) | (rt << 12) | (imm & 0xff),
  };
}

thumb_opcode th_sxtb(uint32_t rd, uint32_t rm, thumb_shift shift, thumb_enforce_encoding encoding)
{

  const uint32_t rotate = shift.value >> 3;
  if (shift.type != THUMB_SHIFT_NONE && shift.type != THUMB_SHIFT_ROR)
  {
    tcc_error("compiler_error: 'th_sxtb', invalid shift type\n");
    return (thumb_opcode){0, 0};
  }

  if (shift.value != 0 && shift.value != 8 && shift.value != 16 && shift.value != 24)
  {
    tcc_error("compiler_error: 'th_sxtb', invalid shift value\n");
    return (thumb_opcode){0, 0};
  }

  if (rd < 8 && rm < 8 && encoding != ENFORCE_ENCODING_32BIT && (shift.type == THUMB_SHIFT_NONE || shift.value == 0))
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xb240 | (rm << 3) | rd,
    };
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xfa4ff080 | rd << 8 | rm | rotate << 4,
  };
}

thumb_opcode th_sxth(uint32_t rd, uint32_t rm, thumb_shift shift, thumb_enforce_encoding encoding)
{

  const uint32_t rotate = shift.value >> 3;
  if (shift.type != THUMB_SHIFT_NONE && shift.type != THUMB_SHIFT_ROR)
  {
    tcc_error("compiler_error: 'th_sxth', invalid shift type\n");
    return (thumb_opcode){0, 0};
  }

  if (shift.value != 0 && shift.value != 8 && shift.value != 16 && shift.value != 24)
  {
    tcc_error("compiler_error: 'th_sxth', invalid shift value\n");
    return (thumb_opcode){0, 0};
  }

  if (rd < 8 && rm < 8 && encoding != ENFORCE_ENCODING_32BIT && (shift.type == THUMB_SHIFT_NONE || shift.value == 0))
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xb200 | (rm << 3) | rd,
    };
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xfa0ff080 | rd << 8 | rm | rotate << 4,
  };
}

thumb_opcode th_tbb(uint32_t rn, uint32_t rm, uint32_t h)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe8d0f000 | (rn << 16) | rm | h << 4,
  };
}

thumb_opcode th_teq(uint32_t rn, uint32_t imm)
{
  const uint32_t packed = th_pack_const(imm);
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf0900f00 | (rn << 16) | packed,
  };
}

thumb_opcode th_tst_imm(uint32_t rn, uint32_t imm)
{
  const uint32_t packed = th_pack_const(imm);
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf0100f00 | (rn << 16) | packed,
  };
}

thumb_opcode th_tst_reg(uint32_t rn, uint32_t rm, thumb_shift shift, thumb_enforce_encoding encoding)
{
  if (rn < 8 && rm < 8 && encoding != ENFORCE_ENCODING_32BIT && shift.type == THUMB_SHIFT_NONE)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0x4200 | (rm << 3) | rn,
    };
  }
  return th_generic_op_reg_shift_with_status(0xea10, 0xf, rn, rm, FLAGS_BEHAVIOUR_NOT_IMPORTANT, shift);
}

thumb_opcode th_tt(uint32_t rd, uint32_t rn, uint32_t a, uint32_t t)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xe840f000 | rn << 16 | rd << 8 | a << 7 | t << 6,
  };
}

thumb_opcode th_udf(uint32_t imm, thumb_enforce_encoding encoding)
{
  const uint32_t imm4 = (imm >> 12) & 0xf;
  const uint32_t imm12 = imm & 0xfff;

  if (encoding != ENFORCE_ENCODING_32BIT && imm <= 0xff)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xde00 | imm,
    };
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf7f0a000 | imm4 << 16 | imm12,
  };
}

thumb_opcode th_umlal(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm)
{
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xfbe00000 | (rn << 16) | (rdlo << 12) | (rdhi << 8) | rm,
  };
}

thumb_opcode th_uxtb(uint32_t rd, uint32_t rm, thumb_shift shift, thumb_enforce_encoding encoding)
{

  const uint32_t rotate = shift.value >> 3;
  if (shift.type != THUMB_SHIFT_NONE && shift.type != THUMB_SHIFT_ROR)
  {
    tcc_error("compiler_error: 'th_uxtb', invalid shift type\n");
    return (thumb_opcode){0, 0};
  }

  if (shift.value != 0 && shift.value != 8 && shift.value != 16 && shift.value != 24)
  {
    tcc_error("compiler_error: 'th_uxtb', invalid shift value\n");
    return (thumb_opcode){0, 0};
  }

  if (rd < 8 && rm < 8 && encoding != ENFORCE_ENCODING_32BIT && (shift.type == THUMB_SHIFT_NONE || shift.value == 0))
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xb2c0 | (rm << 3) | rd,
    };
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xfa5ff080 | rd << 8 | rm | rotate << 4,
  };
}

thumb_opcode th_uxth(uint32_t rd, uint32_t rm, thumb_shift shift, thumb_enforce_encoding encoding)
{

  const uint32_t rotate = shift.value >> 3;
  if (shift.type != THUMB_SHIFT_NONE && shift.type != THUMB_SHIFT_ROR)
  {
    tcc_error("compiler_error: 'th_uxth', invalid shift type\n");
    return (thumb_opcode){0, 0};
  }

  if (shift.value != 0 && shift.value != 8 && shift.value != 16 && shift.value != 24)
  {
    tcc_error("compiler_error: 'th_uxth', invalid shift value\n");
    return (thumb_opcode){0, 0};
  }

  if (rd < 8 && rm < 8 && encoding != ENFORCE_ENCODING_32BIT && (shift.type == THUMB_SHIFT_NONE || shift.value == 0))
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xb280 | (rm << 3) | rd,
    };
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xfa1ff080 | rd << 8 | rm | rotate << 4,
  };
}

thumb_opcode th_wfe(thumb_enforce_encoding encoding)
{
  if (encoding != ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xbf20,
    };
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf3af8002,
  };
}

thumb_opcode th_wfi(thumb_enforce_encoding encoding)
{
  if (encoding != ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xbf30,
    };
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf3af8003,
  };
}

thumb_opcode th_yield(thumb_enforce_encoding encoding)
{
  if (encoding != ENFORCE_ENCODING_32BIT)
  {
    return (thumb_opcode){
        .size = 2,
        .opcode = 0xbf10,
    };
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf3af8001,
  };
}

// Thumb ELF management
// Start of T32 instructions
void th_sym_t()
{
  const int info = ELFW(ST_INFO)(STB_LOCAL, STT_NOTYPE);
  set_elf_sym(symtab_section, ind, 0, info, 0, 1, "$t");
}

// Start of A32 instructions
void th_sym_a()
{
  const int info = ELFW(ST_INFO)(STB_LOCAL, STT_NOTYPE);
  set_elf_sym(symtab_section, ind, 0, info, 0, 1, "$a");
}

// Start of data
void th_sym_d()
{
  const int info = ELFW(ST_INFO)(STB_LOCAL, STT_NOTYPE);
  set_elf_sym(symtab_section, ind, 0, info, 0, 1, "$d");
}

#endif // TARGET_DEFS_ONLY
