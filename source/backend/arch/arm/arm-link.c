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
    if (x < 0)
    {
      (*(uint16_t *)ptr) = 0xbf00;
      return;
    }
    else
    {
      x = (x >> 1);
    }
    /* Compute and store final offset */
    i = (x >> 5) & 1;
    imm5 = x & 0x1f;
    (*(uint16_t *)ptr) = orig | (i << 9) | (imm5 << 3);
    return;
  }
  case R_ARM_THM_ALU_PREL_11_0:
  {
    int x, hi, lo, s, i, imm3, imm8;
    /* weak reference */
    if (sym->st_shndx == SHN_UNDEF && ELFW(ST_BIND)(sym->st_info) == STB_WEAK)
      return;

    /* Get initial offset */
    hi = (*(uint16_t *)ptr);
    lo = (*(uint16_t *)(ptr + 2));
    i = (hi >> 10) & 1;
    imm3 = (lo >> 12) & 0x7;
    imm8 = lo & 0xff;
    x = i << 11 | imm3 << 8 | imm8;

    if (hi & 0x00a0)
    {
      x = -x;
    }

    addr &= -4;
    if (val < addr)
    {
      x = val - addr - 4;
    }
    else
    {
      s = 0;
      x = val - (addr + 4);
    }

    if (x < 0)
    {
      s = 0xa;
      x = -x;
    }

    /* Compute and store final offset */
    i = (x >> 11) & 1;
    imm3 = (x >> 12) & 0x3;
    imm8 = x & 0xff;
    (*(uint16_t *)ptr) = (uint16_t)((hi & 0xfb0f) | (i << 10)) | (s << 4);
    (*(uint16_t *)(ptr + 2)) = (uint16_t)((lo & 0x8f00) | (imm3 << 12) | imm8);
  }
    return;
  case R_ARM_THM_PC12:
  {
    int x, orig;
    /* weak reference */
    if (sym->st_shndx == SHN_UNDEF && ELFW(ST_BIND)(sym->st_info) == STB_WEAK)
      return;

    /* Get initial offset */
    orig = (*(uint16_t *)(ptr + 2));
    addr &= -4;
    if (val > addr)
    {
      x = val - addr - 4;
    }
    else
    {
      uint32_t original_instruction = (*(uint16_t *)ptr);
      (*(uint16_t *)ptr) = original_instruction & 0xff7f;
      x = addr + 4 - val;
    }
    /* Compute and store final offset */
    (*(uint16_t *)(ptr + 2)) = orig | (x & 0xfff);
  }
    return;
  case R_ARM_THM_PC8:
  {
    int x, orig;
    /* weak reference */
    if (sym->st_shndx == SHN_UNDEF && ELFW(ST_BIND)(sym->st_info) == STB_WEAK)
      return;

    /* Get initial offset */
    orig = (*(uint16_t *)(ptr + 2));
    addr &= -4;
    if (val > addr)
    {
      x = val - addr - 4;
    }
    else
    {
      uint32_t original_instruction = (*(uint16_t *)ptr);
      (*(uint16_t *)ptr) = original_instruction & 0xff7f;
      x = addr + 4 - val;
    }
    x >>= 2;
    /* Compute and store final offset */
    (*(uint16_t *)(ptr + 2)) = orig | (x & 0xff);
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
    if (x >= 0x1000000 || x < -0x1000000)
      if ((val & 2) || (!is_call && !to_plt))
        tcc_error_noabort("can't relocate value at %x,%d", addr, type);
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
  case R_ARM_MOVT_ABS:
  case R_ARM_MOVW_ABS_NC:
  {
    int x, imm4, imm12;
    if (type == R_ARM_MOVT_ABS)
      val >>= 16;
    imm12 = val & 0xfff;
    imm4 = (val >> 12) & 0xf;
    x = (imm4 << 16) | imm12;
    /* The Thumb variants are handled by the separate R_ARM_THM_MOVT_ABS /
       R_ARM_THM_MOVW_ABS_NC case below, so `type` here is always one of the
       two ARM (A32) relocations -- never R_ARM_THM_MOVT_ABS.  A stray
       `if (type == R_ARM_THM_MOVT_ABS)` check used to guard this add and was
       therefore dead code (see docs/bugs.md #10).  add32le matches upstream
       tinycc's handling of these relocations. */
    add32le(ptr, x);
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
    int x, i, imm4, imm3, imm8;
    if (type == R_ARM_THM_MOVT_ABS)
      val >>= 16;
    imm8 = val & 0xff;
    imm3 = (val >> 8) & 0x7;
    i = (val >> 11) & 1;
    imm4 = (val >> 12) & 0xf;
    x = (imm3 << 28) | (imm8 << 16) | (i << 10) | imm4;
    if (type == R_ARM_THM_MOVT_ABS)
      write32le(ptr, read32le(ptr) | x);
    else
      add32le(ptr, x);
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
     * the reserved GOT slot) + this value = the symbol's address. */
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
