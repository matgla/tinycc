#include "source/backend/arch/arm/thumb/thumb.h"
#include "source/backend/arch/arm/thumb/thop_alu_reg.h"
#include "source/backend/arch/arm/thumb/thop_branch.h"
#include "source/backend/arch/arm/thumb/thop_cmp.h"
#include "source/backend/arch/arm/thumb/thop_mem_imm.h"
#include "tcc.h"

#ifdef NEED_RELOC_TYPE
/* Returns 1 for a code relocation, 0 for a data relocation. For unknown
   relocations, returns -1. */
ST_FUNC int code_reloc(int reloc_type)
{
  switch (reloc_type)
  {
  case R_ARM_MOVT_ABS:
  case R_ARM_MOVW_ABS_NC:
  case R_ARM_THM_MOVT_ABS:
  case R_ARM_THM_MOVW_ABS_NC:
  case R_ARM_ABS32:
  case R_ARM_REL32:
  case R_ARM_GOTPC:
  case R_ARM_GOTOFF:
  case R_ARM_RODATA_OFF:
  case R_ARM_GOT32:
  case R_ARM_GOT_SBREL12:
  case R_ARM_GOT_PREL:
  case R_ARM_COPY:
  case R_ARM_GLOB_DAT:
  case R_ARM_NONE:
  case R_ARM_TARGET1:
  case R_ARM_MOVT_PREL:
  case R_ARM_MOVW_PREL_NC:
    return 0;

  case R_ARM_PC24:
  case R_ARM_CALL:
  case R_ARM_JUMP24:
  case R_ARM_PLT32:
  case R_ARM_THM_PC22:
  case R_ARM_THM_JUMP24:
  case R_ARM_THM_JUMP19:
  case R_ARM_PREL31:
  case R_ARM_V4BX:
  case R_ARM_JUMP_SLOT:
  case R_ARM_THM_ALU_PREL_11_0:
  case R_ARM_THM_JUMP6:
  case R_ARM_THM_PC12:
  case R_ARM_THM_PC8:
  case R_ARM_YASOS_LOCAL_CALL:
    return 1;
  }
  return -1;
}

/* Returns an enumerator to describe whether and when the relocation needs a
   GOT and/or PLT entry to be created. See tcc.h for a description of the
   different values. */
ST_FUNC int gotplt_entry_type(int reloc_type)
{
  switch (reloc_type)
  {
  case R_ARM_NONE:
  case R_ARM_COPY:
  case R_ARM_GLOB_DAT:
  case R_ARM_JUMP_SLOT:
  case R_ARM_YASOS_LOCAL_CALL: /* a check on the BL beside it, which owns the PLT decision */
    return NO_GOTPLT_ENTRY;

  case R_ARM_PC24:
  case R_ARM_CALL:
  case R_ARM_JUMP24:
  case R_ARM_PLT32:
  case R_ARM_THM_PC22:
  case R_ARM_THM_ALU_PREL_11_0:
  case R_ARM_THM_JUMP6:
  case R_ARM_THM_JUMP19:
  case R_ARM_THM_JUMP24:
  case R_ARM_MOVT_ABS:
  case R_ARM_MOVW_ABS_NC:
  case R_ARM_THM_MOVT_ABS:
  case R_ARM_THM_MOVW_ABS_NC:
  case R_ARM_PREL31:
  case R_ARM_ABS32:
  case R_ARM_REL32:
  case R_ARM_V4BX:
  case R_ARM_TARGET1:
  case R_ARM_MOVT_PREL:
  case R_ARM_MOVW_PREL_NC:
  case R_ARM_THM_PC12:
  case R_ARM_THM_PC8:
    return AUTO_GOTPLT_ENTRY;

  case R_ARM_GOTPC:
  case R_ARM_GOTOFF:
  case R_ARM_RODATA_OFF:
    /* RODATA_OFF needs the GOT to exist (for the reserved rodata anchor slot)
     * but no per-symbol GOT entry — same as GOTOFF. */
    return BUILD_GOT_ONLY;

  case R_ARM_GOT32:
  case R_ARM_GOT_SBREL12:
  case R_ARM_GOT_PREL:
    return ALWAYS_GOTPLT_ENTRY;
  }
  return -1;
}

void write_thumb_instruction(uint8_t *p, thumb_opcode op)
{
  if (op.size != 2 && op.size != 4)
  {
    return;
  }
  if (op.size == 4)
  {
    write16le(p, op.opcode >> 16);
    p += 2;
  }
  write16le(p, op.opcode);
}

#ifdef NEED_BUILD_GOT
ST_FUNC unsigned create_plt_entry(TCCState *s1, unsigned got_offset, struct sym_attr *attr)
{
  Section *plt = s1->plt;
  uint8_t *p;
  unsigned plt_offset;

  /* when building a DLL, GOT entry accesses must be done relative to
     start of GOT (see x86_64 example above)  */

  /* empty PLT: create PLT0 entry that push address of call site and
     jump to ld.so resolution routine (GOT + 8) */
  if (plt->data_offset == 0)
  {
    p = section_ptr_add(plt, 32);
  }
  plt_offset = plt->data_offset;
  /* save GOT offset for relocate_plt */
  // I can't know if library will use text_and_data separation or not
  // so I have to implement r9 loading in both cases
  p = section_ptr_add(plt, 32);
  write32le(p + 4, got_offset);
  return plt_offset;
}
/* relocate the PLT: compute addresses and offsets in the PLT now that final
   address for PLT and GOT are known (see fill_program_header) */
ST_FUNC void relocate_plt(TCCState *s1)
{
  uint8_t *p, *p_end;

  if (!s1->plt)
    return;

  if (!thop_feat_bits(arm_target_dependent.feat))
    arm_init(s1);

  p = s1->plt->data;
  p_end = p + s1->plt->data_offset;
  p += 32;

  if (p < p_end)
  {
    // int x = s1->got->sh_addr - s1->plt->sh_addr - 12;
    if (s1->text_and_data_separation)
    {
      // p += 48;
    }
    else
    {
      // p += 20;
      // write32le(p + 16, x - 4);
    }
    while (p < p_end)
    {
      unsigned off = read32le(p + 4);
      if (s1->text_and_data_separation != 1)
      {
        // calculate PC relative offset to the got start from p + 4 instruction
        // entries from 0 to 2 inclusive are reserved for the dynamic linker
        off += s1->got->sh_addr - s1->plt->sh_addr - (p - s1->plt->data) - 8;
        // calculate address of got entry
      }
      write32le(p + 28, off);
      // when mno-pic-data-is-text relative GOT entries have 8 bytes, to keep
      // the base register and offset to the symbol
      // push R9 to restore it when getting back to the caller
      // I can't modify stack in this function, so how can I restore R9?
      write_thumb_instruction(p, th_ldr_imm(R_IP, R_PC, 24, 6, ENFORCE_ENCODING_NONE));

      if (s1->text_and_data_separation)
      {
        // calculate address relative to the base
        write_thumb_instruction(p + 4, th_add_reg(R_IP, R_IP, R9, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                                  ENFORCE_ENCODING_NONE));
      }
      else
      {
        // calculate address relative to the PC
        write_thumb_instruction(p + 4, th_add_reg(R_IP, R_IP, R_PC, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                                  ENFORCE_ENCODING_NONE));
      }
      // load R9 value from first got entry
      write_thumb_instruction(p + 6, th_ldr_imm(R9, R_IP, 4, 6, ENFORCE_ENCODING_NONE));
      // update R9
      // get address of the symbol
      // load the address of the symbol
      write_thumb_instruction(p + 10, th_ldr_imm(R_IP, R_IP, 0, 6, ENFORCE_ENCODING_NONE));
      write_thumb_instruction(p + 14, th_cmp_imm(R_IP, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_32BIT));
      // if 0 then call resolver, else move one instruction further
      write_thumb_instruction(p + 18, th_b_t1(1, 0));
      write_thumb_instruction(p + 22, th_bx_reg(R_IP));

      p += 32;
    }
  }

  if (s1->plt->reloc)
  {
    ElfW_Rel *rel;
    p = s1->got->data;
    for_each_elem(s1->plt->reloc, 0, rel, ElfW_Rel)
    {
      write32le(p + rel->r_offset, s1->plt->sh_addr);
    }
  }
}
#endif
#endif

ST_FUNC void relocate(TCCState *s1, ElfW_Rel *rel, int type, unsigned char *ptr, addr_t addr, addr_t val)
{
  ElfW(Sym) * sym;
  int sym_index, esym_index;

  sym_index = ELFW(R_SYM)(rel->r_info);
  sym = &((ElfW(Sym) *)symtab_section->data)[sym_index];
  switch (type)
  {
  case R_ARM_PC24:
  case R_ARM_CALL:
  case R_ARM_JUMP24:
  case R_ARM_PLT32:
  {
    int x, is_thumb, is_call, h, blx_avail, is_bl, th_ko;
    x = read32le(ptr) & 0xffffff;
    LOG_RELOC("reloc %d: x=0x%x val=0x%x ", type, x, val);
    write32le(ptr, read32le(ptr) & 0xff000000);
    if (x & 0x800000)
      x -= 0x1000000;
    x <<= 2;
    blx_avail = (CONFIG_TCC_CPUVER >= 5);
    is_thumb = val & 1;
    is_bl = read32le(ptr) >> 24 == 0xeb;
    is_call = (type == R_ARM_CALL || (type == R_ARM_PC24 && is_bl));
    x += val - addr;
    LOG_RELOC(" newx=0x%x name=%s", x, (char *)symtab_section->link->data + sym->st_name);
    h = x & 2;
    th_ko = (x & 3) && (!blx_avail || !is_call);
    if (th_ko || x >= 0x2000000 || x < -0x2000000)
      tcc_error_noabort("can't relocate value at %x,%d", addr, type);
    x >>= 2;
    x &= 0xffffff;
    /* Only reached if blx is avail and it is a call */
    if (is_thumb)
    {
      x |= h << 24;
      write32le(ptr, 0xfa << 24); /* bl -> blx */
    }
    write32le(ptr, read32le(ptr) | x);
  }
    return;
  case R_ARM_THM_JUMP6:
  {
    int x, orig, i, imm5;
    /* weak reference */
    if (sym->st_shndx == SHN_UNDEF && ELFW(ST_BIND)(sym->st_info) == STB_WEAK)
      return;

    /* Get initial offset */
    orig = (*(uint16_t *)ptr);
    x = (val - addr - 4);
    /* CBZ/CBNZ only reach 0..126 bytes forward; never turn it into a NOP. */
    if (x < 0 || x > 126 || (x & 1))
    {
      tcc_error_noabort("R_ARM_THM_JUMP6 (cbz/cbnz) target out of range or misaligned: offset %d (0..126, even)", x);
      return;
    }
    x = (x >> 1);
    /* Compute and store final offset */
    i = (x >> 5) & 1;
    imm5 = x & 0x1f;
    (*(uint16_t *)ptr) = orig | (i << 9) | (imm5 << 3);
    return;
  }
  case R_ARM_THM_ALU_PREL_11_0:
  {
    int x, addend, hi, lo, s, i, imm3, imm8;
    /* weak reference */
    if (sym->st_shndx == SHN_UNDEF && ELFW(ST_BIND)(sym->st_info) == STB_WEAK)
      return;

    /* Get initial offset */
    hi = (*(uint16_t *)ptr);
    lo = (*(uint16_t *)(ptr + 2));
    i = (hi >> 10) & 1;
    imm3 = (lo >> 12) & 0x7;
    imm8 = lo & 0xff;
    addend = i << 11 | imm3 << 8 | imm8;
    if (hi & 0x0080)
      addend = -addend;

    /* R_ARM_THM_ALU_PREL_11_0 is a REL relocation.  The immediate in the
       instruction is therefore S + A - (Align(P, 4) + 4), with A already
       encoded by the assembler. */
    addr &= -4;
    x = (int)val + addend - (int)addr - 4;

    s = 0;
    if (x < 0)
    {
      s = 0xa;
      x = -x;
    }

    if (x > 0xfff)
    {
      tcc_error_noabort("ADR relocation out of range: %x,%d", addr, type);
      return;
    }

    /* Compute and store final offset */
    i = (x >> 11) & 1;
    imm3 = (x >> 8) & 0x7;
    imm8 = x & 0xff;
    (*(uint16_t *)ptr) = (uint16_t)((hi & 0xfb0f) | (i << 10)) | (s << 4);
    (*(uint16_t *)(ptr + 2)) = (uint16_t)((lo & 0x8f00) | (imm3 << 12) | imm8);
  }
    return;
  case R_ARM_THM_PC12:
  {
    int x, addend, hi, orig;
    /* weak reference */
    if (sym->st_shndx == SHN_UNDEF && ELFW(ST_BIND)(sym->st_info) == STB_WEAK)
      return;

    /* Get initial offset */
    hi = (*(uint16_t *)ptr);
    orig = (*(uint16_t *)(ptr + 2));
    addend = orig & 0xfff;
    if (!(hi & 0x0080))
      addend = -addend;

    addr &= -4;
    x = (int)val + addend - (int)addr - 4;
    if (x < -0xfff || x > 0xfff)
    {
      tcc_error_noabort("literal relocation out of range: %x,%d", addr, type);
      return;
    }

    /* Compute and store final offset */
    if (x < 0)
    {
      hi &= 0xff7f;
      x = -x;
    }
    else
    {
      hi |= 0x0080;
    }
    (*(uint16_t *)ptr) = hi;
    (*(uint16_t *)(ptr + 2)) = (uint16_t)((orig & 0xf000) | x);
  }
    return;
  case R_ARM_THM_PC8:
  {
    int x, addend, hi, orig;
    /* weak reference */
    if (sym->st_shndx == SHN_UNDEF && ELFW(ST_BIND)(sym->st_info) == STB_WEAK)
      return;

    /* Get initial offset */
    hi = (*(uint16_t *)ptr);
    orig = (*(uint16_t *)(ptr + 2));
    addend = (orig & 0xff) << 2;
    if (!(hi & 0x0080))
      addend = -addend;

    addr &= -4;
    x = (int)val + addend - (int)addr - 4;
    if (x < -0x3fc || x > 0x3fc || (x & 3))
    {
      tcc_error_noabort("literal pair relocation out of range: %x,%d", addr, type);
      return;
    }

    /* Compute and store final offset */
    if (x < 0)
    {
      hi &= 0xff7f;
      x = -x;
    }
    else
    {
      hi |= 0x0080;
    }
    (*(uint16_t *)ptr) = hi;
    (*(uint16_t *)(ptr + 2)) = (uint16_t)((orig & 0xff00) | (x >> 2));
  }
    return;

  case R_ARM_THM_JUMP19:
  {
    int x, hi, lo, s, j1, j2, imm6, imm11;
    /* weak reference */
    if (sym->st_shndx == SHN_UNDEF && ELFW(ST_BIND)(sym->st_info) == STB_WEAK)
      return;

    /* Get initial offset from T3 encoding */
    hi = (*(uint16_t *)ptr);
    lo = (*(uint16_t *)(ptr + 2));
    s = (hi >> 10) & 1;
    j1 = (lo >> 13) & 1;
    j2 = (lo >> 11) & 1;
    imm6 = hi & 0x3f;
    imm11 = lo & 0x7ff;
    /* T3: offset = SignExtend(S:J2:J1:imm6:imm11:'0', 21) */
    x = (s << 20) | (j2 << 19) | (j1 << 18) | (imm6 << 12) | (imm11 << 1);
    if (x & 0x100000) /* sign extend from bit 20 */
      x -= 0x200000;

    /* Compute final offset */
    x += val - addr;

    /* Check range (±1MB) */
    if (x >= 0x100000 || x < -0x100000)
      tcc_error_noabort("conditional branch target out of range: %x,%d", addr, type);

    /* Encode back into T3 format (preserve condition code in hi[9:6]) */
    s = (x >> 20) & 1;
    j2 = (x >> 19) & 1;
    j1 = (x >> 18) & 1;
    imm6 = (x >> 12) & 0x3f;
    imm11 = (x >> 1) & 0x7ff;
    (*(uint16_t *)ptr) = (uint16_t)((hi & 0xfbc0) | (s << 10) | imm6);
    (*(uint16_t *)(ptr + 2)) = (uint16_t)((lo & 0xd000) | (j1 << 13) | (j2 << 11) | imm11);
  }
    return;

    /* Since these relocations only concern Thumb-2 and blx instruction was
     introduced before Thumb-2, we can assume blx is available and not
     guard its use */
  case R_ARM_THM_PC22:
  case R_ARM_THM_JUMP24:
  {
    int x, hi, lo, s, j1, j2, i1, i2, imm10, imm11;
    int is_call, to_plt = 0, blx_bit = 1 << 12;
    Section *plt;
    /* weak reference */
    if (sym->st_shndx == SHN_UNDEF && ELFW(ST_BIND)(sym->st_info) == STB_WEAK)
      return;

    /* Get initial offset */
    hi = (*(uint16_t *)ptr);
    lo = (*(uint16_t *)(ptr + 2));
    s = (hi >> 10) & 1;
    j1 = (lo >> 13) & 1;
    j2 = (lo >> 11) & 1;
    i1 = (j1 ^ s) ^ 1;
    i2 = (j2 ^ s) ^ 1;
    imm10 = hi & 0x3ff;
    imm11 = lo & 0x7ff;
    x = (s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1);
    if (x & 0x01000000)
      x -= 0x02000000;

    /* Relocation infos */
    if (s1->plt)
    {
      plt = s1->plt;
      to_plt = (val >= plt->sh_addr) && (val < plt->sh_addr + plt->data_offset);
    }
    is_call = (type == R_ARM_THM_PC22);

    /* Compute final offset */
    x += val - addr;
    if (is_call)
    {
      blx_bit = 0; /* bl -> blx */
      // x = (x + 3) & -4; /* Compute offset from aligned PC */
    }

    /* Check that relocation is possible
       * offset must not be out of range
       * if target is to be entered in arm mode:
         - bit 1 must not set
         - instruction must be a call (bl) or a jump to PLT */
    /* Out of BL/B.W range (+-16 MB) is an error for a call too: encoding it
       anyway wrapped the offset and branched somewhere else entirely.  Far
       targets the linker script separates get veneers before layout. */
    if (x >= 0x1000000 || x < -0x1000000)
      if ((val & 2) || !to_plt)
        tcc_error_noabort("can't relocate value at %x,%d (branch out of range)", addr, type);
    /* Compute and store final offset */
    s = (x >> 24) & 1;
    i1 = (x >> 23) & 1;
    i2 = (x >> 22) & 1;
    j1 = s ^ (i1 ^ 1);
    j2 = s ^ (i2 ^ 1);
    imm10 = (x >> 12) & 0x3ff;
    imm11 = (x >> 1) & 0x7ff;
    (*(uint16_t *)ptr) = (uint16_t)((hi & 0xf800) | (s << 10) | imm10);
    (*(uint16_t *)(ptr + 2)) = (uint16_t)((lo & 0xd000) | (j1 << 13) | blx_bit | (j2 << 11) | imm11);
  }
    return;
  case R_ARM_YASOS_LOCAL_CALL:
  {
    /* The compiler dropped the R9 reload after the BL at this offset because
       -fmodule-local-calls classified the callee as module-local (no shared
       library in the library paths exported it).  If the call binds to the
       PLT after all, the callee returns with its own R9 and this function
       carries on addressing its data through the wrong GOT: refuse the link
       rather than produce that. */
    int to_plt = s1->plt && val >= s1->plt->sh_addr && val < s1->plt->sh_addr + s1->plt->data_offset;
    if (sym->st_shndx == SHN_UNDEF && ELFW(ST_BIND)(sym->st_info) == STB_WEAK)
      return; /* an unresolved weak callee is the caller's problem, as for the BL */
    if (to_plt || sym->st_shndx == SHN_UNDEF)
      tcc_error_noabort("call to '%s' at %x was compiled as module-local (its R9 reload dropped) but binds "
                        "to an import; compile with the library's -L path or with -fno-module-local-calls",
                        (char *)symtab_section->link->data + sym->st_name, (unsigned)addr);
  }
    return;
  case R_ARM_MOVT_ABS:
  case R_ARM_MOVW_ABS_NC:
  {
    /* REL relocations keep the addend in the instruction's immediate fields.
       Decode it, add it to the symbol value, and rewrite only those fields.
       The Thumb variants are handled by the separate R_ARM_THM_MOVT_ABS /
       R_ARM_THM_MOVW_ABS_NC case below. */
    uint32_t insn = read32le(ptr);
    uint32_t addend = (((insn >> 4) & 0xf000u) | (insn & 0xfffu));
    if (addend & 0x8000u)
      addend -= 0x10000u;
    val += addend;
    if (type == R_ARM_MOVT_ABS)
      val >>= 16;
    write32le(ptr, (insn & 0xfff0f000u) | ((val & 0xf000u) << 4) | (val & 0xfffu));
  }
    return;
  case R_ARM_MOVT_PREL:
  case R_ARM_MOVW_PREL_NC:
  {
    int insn = read32le(ptr);
    int addend = ((insn >> 4) & 0xf000) | (insn & 0xfff);

    addend = (addend ^ 0x8000) - 0x8000;
    val += addend - addr;
    if (type == R_ARM_MOVT_PREL)
      val >>= 16;
    write32le(ptr, (insn & 0xfff0f000) | ((val & 0xf000) << 4) | (val & 0xfff));
  }
    return;
  case R_ARM_THM_MOVT_ABS:
  case R_ARM_THM_MOVW_ABS_NC:
  {
    /* REL relocations keep the addend in the instruction's immediate fields.
       Decode it, add it to the symbol value, and rewrite only those fields. */
    uint32_t insn = read32le(ptr);
    uint32_t addend = ((insn & 0xfu) << 12) | (((insn >> 10) & 1u) << 11) |
                      (((insn >> 28) & 7u) << 8) | ((insn >> 16) & 0xffu);
    if (addend & 0x8000u)
      addend -= 0x10000u;
    val += addend;
    if (type == R_ARM_THM_MOVT_ABS)
      val >>= 16;
    uint32_t x = (((val >> 12) & 0xfu)) | (((val >> 11) & 1u) << 10) |
                 (((val >> 8) & 7u) << 28) | ((val & 0xffu) << 16);
    write32le(ptr, (insn & ~0x70ff040fu) | x);
  }
    return;
  case R_ARM_PREL31:
  {
    int x;
    x = read32le(ptr) & 0x7fffffff;
    x = (x * 2) / 2;
    x += val - addr;
    if ((x ^ (x >> 1)) & 0x40000000)
      tcc_error_noabort("can't relocate value at %x,%d", addr, type);
    write32le(ptr, (read32le(ptr) & 0x80000000) | (x & 0x7fffffff));
  }
    return;
  case R_ARM_ABS32:
  case R_ARM_TARGET1:
    if (s1->output_type & TCC_OUTPUT_DYN)
    {
      esym_index = get_sym_attr(s1, sym_index, 0)->dyn_index;
      qrel->r_offset = rel->r_offset;
      if (esym_index)
      {
        qrel->r_info = ELFW(R_INFO)(esym_index, R_ARM_ABS32);
        qrel++;
        /* For absolute symbols, still apply the value now */
        if (sym->st_shndx != SHN_ABS)
        {
          return;
        }
      }
      else
      {
        qrel->r_info = ELFW(R_INFO)(0, R_ARM_RELATIVE);
        qrel++;
      }
    }

    add32le(ptr, val);

    return;
  case R_ARM_REL32:
    add32le(ptr, val - addr);
    return;
  case R_ARM_GOTPC:
    add32le(ptr, s1->got->sh_addr - addr);
    return;
  case R_ARM_GOTOFF:
    add32le(ptr, val - s1->got->sh_addr);
    return;
  case R_ARM_RODATA_OFF:
    /* Offset of the symbol within .rodata: anchor (rodata runtime base, from
     * the reserved GOT slot) + this value = the symbol's address.  Also a
     * __rodata_relative pointer's word, whose target the compiler could not
     * see (rodata_rel.c): anything outside .rodata has no such offset.  The
     * addend is in the word, so `val` is where the symbol starts: at the end
     * of .rodata is the next section (only __tcc_rodata_base of an empty
     * .rodata starts there). */
    if (val != rodata_section->sh_addr &&
        (val < rodata_section->sh_addr || val >= rodata_section->sh_addr + rodata_section->sh_size))
    {
      const char *name = (const char *)symtab_section->link->data + sym->st_name;
      tcc_error_noabort("'%s' is not in .rodata: a __rodata_relative pointer (or a "
                        "share-rodata reference) cannot reach it",
                        name);
      return;
    }
    add32le(ptr, val - rodata_section->sh_addr);
    return;
  case R_ARM_GOT32:
    /* we load the got offset */
    write32le(ptr, get_sym_attr(s1, sym_index, 0)->got_offset);
    return;
  case R_ARM_GOT_SBREL12:
  {
    /* Patch the GOT slot offset into the imm12 of `ldr.w Rt,[r9,#imm12]`
     * (T3: hw1 = 0xF8D0|Rn, hw2 = Rt<<12|imm12, each stored little-endian). */
    unsigned long got_offset = get_sym_attr(s1, sym_index, 0)->got_offset;
    if (got_offset > 0xfff)
    {
      const char *name = (const char *)symtab_section->link->data + sym->st_name;
      tcc_error_noabort("GOT slot for '%s' at offset %lu exceeds the 4096-byte "
                        "SB-relative range; rebuild this module with "
                        "-mno-sb-relative-got",
                        name, got_offset);
      return;
    }
    /* imm12 is bits 0-11 of hw2, i.e. bits 16-27 of the little-endian word. */
    write32le(ptr, (read32le(ptr) & ~(0xfffu << 16)) | ((uint32_t)got_offset << 16));
    return;
  }
  case R_ARM_GOT_PREL:
    /* we load the pc relative got offset */
    write32le(ptr, s1->got->sh_addr + get_sym_attr(s1, sym_index, 0)->got_offset - addr - 8);
    return;
  case R_ARM_COPY:
    return;
  case R_ARM_V4BX:
    /* trade Thumb support for ARMv4 support */
    if ((0x0ffffff0 & read32le(ptr)) == 0x012FFF10)
      write32le(ptr, read32le(ptr) ^ (0xE12FFF10 ^ 0xE1A0F000)); /* BX Rm -> MOV PC, Rm */
    return;
  case R_ARM_GLOB_DAT:
  case R_ARM_JUMP_SLOT:
    *(addr_t *)ptr = val;
    return;
  case R_ARM_NONE:
    /* Nothing to do.  Normally used to indicate a dependency
       on a certain symbol (like for exception handling under EABI).  */
    return;
  case R_ARM_RELATIVE:
#ifdef TCC_TARGET_PE
    add32le(ptr, val - s1->pe_imagebase);
#endif
    /* do nothing */
    return;
  default:
    LOG_RELOC("FIXME: handle reloc type %d at %x [%p] to %x", type, (unsigned)addr, ptr, (unsigned)val);
    return;
  }
}

/* Range-extension veneers for Thumb BL / B.W (R_ARM_THM_PC22 / JUMP24).
 *
 * A branch reaches +-16 MB.  When the linker script puts the caller's section
 * and the callee's in memory regions further apart than that (the RP2350's
 * .time_critical code runs from SRAM at 0x2000_0000 and calls into flash at
 * 0x1000_0000), the branch is pointed at a veneer appended to the caller's
 * section instead -- `ldr.w pc, [pc, #0]` and the callee's address -- which
 * sits within reach and loads the full 32-bit target.  One veneer per
 * (section, callee).  Runs before layout so the veneers are sized in. */
ST_FUNC void arm_add_range_veneers(TCCState *s1)
{
  ElfW(Sym) *symtab;
  int i, nb_syms, n_veneers = 0;
  for (i = 1; i < s1->nb_sections; i++)
  {
    Section *sr = s1->sections[i], *s;
    ElfW_Rel *rel;
    addr_t src_origin, dst_origin;
    int src_mr;
    int *veneer_of; /* symbol index -> veneer symbol, for this section */
    if (!sr || sr->sh_type != SHT_RELX || !sr->data_offset)
      continue;
    s = s1->sections[sr->sh_info];
    if (!s || !(s->sh_flags & SHF_EXECINSTR) || !(s->sh_flags & SHF_ALLOC))
      continue;
    src_mr = ld_section_memory_region(s1, s->name, &src_origin);
    if (src_mr < 0)
      continue;
    nb_syms = symtab_section->data_offset / sizeof(ElfW(Sym));
    veneer_of = tcc_mallocz(nb_syms * sizeof(int));
    /* By index: adding a veneer's literal relocation appends to this very
     * section and may move its data. */
    int k, nb_rels = sr->data_offset / sizeof(ElfW_Rel);
    for (k = 0; k < nb_rels; k++)
    {
      rel = (ElfW_Rel *)sr->data + k;
      int type = ELFW(R_TYPE)(rel->r_info), sym_index = ELFW(R_SYM)(rel->r_info), dst_mr;
      ElfW(Sym) *sym;
      long diff;
      if (type != R_ARM_THM_PC22 && type != R_ARM_THM_JUMP24)
        continue;
      symtab = (ElfW(Sym) *)symtab_section->data;
      if (sym_index <= 0 || sym_index >= nb_syms)
        continue;
      sym = &symtab[sym_index];
      if (sym->st_shndx == SHN_UNDEF || sym->st_shndx >= SHN_LORESERVE || sym->st_shndx >= s1->nb_sections)
        continue;
      dst_mr = ld_section_memory_region(s1, s1->sections[sym->st_shndx]->name, &dst_origin);
      if (dst_mr < 0 || dst_mr == src_mr)
        continue;
      diff = (long)dst_origin - (long)src_origin;
      if (diff < 0x800000 && diff > -0x800000) /* regions close enough for a direct branch */
        continue;
      if (!veneer_of[sym_index])
      {
        char name[48];
        unsigned char *p;
        addr_t off;
        section_ptr_add(s, (-s->data_offset) & 3); /* the literal load needs a word-aligned PC */
        off = s->data_offset;
        p = section_ptr_add(s, 8);
        p[0] = 0xdf; p[1] = 0xf8; p[2] = 0x00; p[3] = 0xf0; /* ldr.w pc, [pc, #0] */
        p[4] = p[5] = p[6] = p[7] = 0;
        put_elf_reloc(symtab_section, s, off + 4, R_ARM_ABS32, sym_index);
        snprintf(name, sizeof(name), "__tcc_veneer_%d", n_veneers++);
        veneer_of[sym_index] = set_elf_sym(symtab_section, off | 1, 8, ELFW(ST_INFO)(STB_GLOBAL, STT_FUNC),
                                           STV_HIDDEN, s->sh_num, name);
        rel = (ElfW_Rel *)sr->data + k;
      }
      rel->r_info = ELFW(R_INFO)(veneer_of[sym_index], type);
    }
    tcc_free(veneer_of);
  }
}
