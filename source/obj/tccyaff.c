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

#include "tcc.h"

#include <stdbool.h>

#include "memory/unique_ptr.h"
#include "tccyaff.h"

#define TCC_YAFF_MAX_SYMBOL_ENTRY_SIZE 255

#define SHF_DYNSYM 0x40000000

// This implementation is based on elf hashing function
uint32_t tcc_yaff_hash(const char *name)
{
  uint32_t h = 0, g;
  while (*name)
  {
    h = (h << 4) + *name++;
    if ((g = h & 0xf0000000))
    {
      h ^= g >> 24;
      h &= ~g;
    }
  }
  return h;
}

void tcc_allocate_hash_table(YaffHashTable *ht, uint32_t number_of_buckets, uint32_t count)
{
  ht->nbucket = number_of_buckets;
  ht->nchain = count;
  unique_ptr_reset(ht->bucket, tcc_mallocz(ht->nbucket * sizeof(uint32_t)));
  unique_ptr_reset(ht->chain, tcc_mallocz(ht->nchain * sizeof(uint32_t)));
}

void tcc_add_hash_entry(YaffHashTable *ht, const char *name, uint32_t i)
{
  uint32_t h = tcc_yaff_hash(name);
  uint32_t b = h % ht->nbucket;
  if (ht->bucket[b] == 0)
  {
    ht->bucket[b] = i;
  }
  else
  {
    uint32_t idx = ht->bucket[b];
    while (ht->chain[idx] != 0)
      idx = ht->chain[idx];
    ht->chain[idx] = i;
  }
}

void tcc_free_hash_table(YaffHashTable *ht)
{
  unique_ptr_reset(ht->bucket, NULL);
  unique_ptr_reset(ht->chain, NULL);
  ht->nbucket = 0;
  ht->nchain = 0;
}

void tcc_write_hash_table(YaffHashTable *ht, FILE *f)
{
  fwrite(&ht->nbucket, 1, sizeof(uint32_t), f);
  fwrite(&ht->nchain, 1, sizeof(uint32_t), f);
  fwrite(ht->bucket, sizeof(uint32_t), ht->nbucket, f);
  fwrite(ht->chain, sizeof(uint32_t), ht->nchain, f);
}

uint32_t tcc_yaff_align(YaffHeader *header, uint32_t size)
{
  return (size + header->alignment - 1) & ~(header->alignment - 1);
}

/* Predicate: is `sym` an exported (defined, externally-visible) symbol?
 *
 * Deliberately written as a sequence of early returns rather than one folded
 * boolean expression.  The inline `st_shndx==UNDEF || (bind!=...) || (vis!=...)`
 * form is miscompiled by the self-hosting armv8m cross: at -O1 it tail-merges
 * the short-circuit skip branches onto the final `vis != PROTECTED` compare's
 * conditional branch, so the UNDEF case reaches that branch with stale flags
 * (Z=1) and falls through to "keep" instead of skipping.  That kept imported
 * (UNDEF) symbols in tcc_yaff_write_exported_symbols_lookup, computing the
 * exported-symbol lookup offsets from the wrong (imported) name lengths and
 * corrupting symbol resolution at load time (see tests2/104_inline).  Each
 * `return 0` materializes the result and branches unconditionally, so even if
 * the cross merges them the shared block carries no flag dependency. */
static int tcc_yaff_sym_is_exported(ElfW(Sym) *sym)
{
  unsigned vis, bind;
  if (sym->st_shndx == SHN_UNDEF)
    return 0;
  bind = ELFW(ST_BIND)(sym->st_info);
  if (bind != STB_GLOBAL && bind != STB_WEAK)
    return 0;
  vis = ELFW(ST_VISIBILITY)(sym->st_other);
  if (vis != STV_DEFAULT && vis != STV_PROTECTED)
    return 0;
  return 1;
}

const char *tcc_parse_object_name(YaffHeader *header)
{
  return (const char *)(header) + sizeof(YaffHeader);
}

uint32_t tcc_get_offset_to_imported_libraries(YaffHeader *header)
{
  const uint32_t name_length = strlen(tcc_parse_object_name(header)) + 1;
  /* header, name (padded), architecture section (padded), then the table. */
  return sizeof(YaffHeader) + tcc_yaff_align(header, name_length) +
         tcc_yaff_align(header, (uint32_t)sizeof(YaffArchSection));
}

/* Describe the machine the emitted code needs, for the architecture section.
 *
 * The mapping is keyed on what -mfpu named rather than on which operations the
 * backend currently inlines: the module also carries the FP runtime it links
 * against (librp2350fp / libvfpv4sp), whose instructions are part of the
 * image's requirements, and "which ops are inline today" moves between
 * compiler versions while "this part has a DCP" does not.
 *
 * -mfloat-abi=soft emits no FP instructions whatever -mfpu says, and an
 * unresolved ARM_FPU_AUTO lands on the soft tables in the backend
 * (arm_determine_fpu_config); both mean "requires nothing". */
static void tcc_yaff_fill_arch_section(TCCState *s1, const YaffHeader *header, YaffArchSection *out)
{
  memset(out, 0, sizeof(*out));
  out->size = (uint16_t)sizeof(YaffArchSection);
  out->arch = (uint8_t)header->arch;

#if !defined(TCC_TARGET_ARM) && !defined(TCC_TARGET_ARM_THUMB)
  /* Only the ARM backends carry an FP configuration; elsewhere the section
   * still exists (so the layout is uniform) but requires nothing. */
  (void)s1;
  out->fpu = YAFF_FPU_NONE;
  out->float_abi = YAFF_FLOAT_ABI_SOFT;
  out->required_features = 0;
#else
  switch (s1->float_abi)
  {
  case ARM_HARD_FLOAT:
    out->float_abi = YAFF_FLOAT_ABI_HARD;
    break;
  case ARM_SOFT_FLOAT:
    out->float_abi = YAFF_FLOAT_ABI_SOFT;
    break;
  default:
    out->float_abi = YAFF_FLOAT_ABI_SOFTFP;
    break;
  }

  if (s1->float_abi == ARM_SOFT_FLOAT)
  {
    out->fpu = YAFF_FPU_NONE;
    out->required_features = 0;
    return;
  }

  switch (s1->fpu_type)
  {
  case ARM_FPU_FPV4_SP_D16:
    out->fpu = YAFF_FPU_FPV4_SP_D16;
    out->required_features = YAFF_ARCH_FEATURE_FPU_SP;
    break;
  case ARM_FPU_FPV5_SP_D16:
    out->fpu = YAFF_FPU_FPV5_SP_D16;
    out->required_features = YAFF_ARCH_FEATURE_FPU_SP;
    break;
  case ARM_FPU_FPV5_D16:
    out->fpu = YAFF_FPU_FPV5_D16;
    out->required_features = YAFF_ARCH_FEATURE_FPU_SP | YAFF_ARCH_FEATURE_FPU_DP;
    break;
  case ARM_FPU_RP2350:
    out->fpu = YAFF_FPU_RP2350;
    out->required_features = YAFF_ARCH_FEATURE_FPU_SP | YAFF_ARCH_FEATURE_DCP;
    break;
  case ARM_FPU_VFP:
    out->fpu = YAFF_FPU_VFP;
    out->required_features = YAFF_ARCH_FEATURE_FPU_SP | YAFF_ARCH_FEATURE_FPU_DP;
    break;
  case ARM_FPU_VFPV3:
    out->fpu = YAFF_FPU_VFPV3;
    out->required_features = YAFF_ARCH_FEATURE_FPU_SP | YAFF_ARCH_FEATURE_FPU_DP;
    break;
  case ARM_FPU_VFPV4:
    out->fpu = YAFF_FPU_VFPV4;
    out->required_features = YAFF_ARCH_FEATURE_FPU_SP | YAFF_ARCH_FEATURE_FPU_DP;
    break;
  case ARM_FPU_NEON:
    out->fpu = YAFF_FPU_NEON;
    out->required_features = YAFF_ARCH_FEATURE_FPU_SP | YAFF_ARCH_FEATURE_FPU_DP;
    break;
  case ARM_FPU_NEON_VFPV4:
    out->fpu = YAFF_FPU_NEON_VFPV4;
    out->required_features = YAFF_ARCH_FEATURE_FPU_SP | YAFF_ARCH_FEATURE_FPU_DP;
    break;
  case ARM_FPU_NEON_FP_ARMV8:
    out->fpu = YAFF_FPU_NEON_FP_ARMV8;
    out->required_features = YAFF_ARCH_FEATURE_FPU_SP | YAFF_ARCH_FEATURE_FPU_DP;
    break;
  case ARM_FPU_NONE:
  case ARM_FPU_AUTO:
  default:
    out->fpu = YAFF_FPU_NONE;
    out->required_features = 0;
    break;
  }
#endif
}

/* Load a YAFF shared library WITHOUT interning any of its exported symbols.
   Instead read the library's own on-disk exported-symbol tables — the name/
   value region, the index->offset lookup table, and the name hash — and keep
   them so tcc_yaff_resolve can look a symbol up by name on demand, interning
   only the handful the link actually references.  All reads are bounded by
   header offsets (no lseek(SEEK_END), which broke on-device). */
ST_FUNC int tcc_load_yaff(TCCState *s1, int fd, const char *filename, int level)
{
  const char *soname = tcc_basename(filename);
  YaffHeader header;
  full_read(fd, &header, sizeof(YaffHeader));
  if (memcmp(header.magic, "YAFF", 4) != 0)
    return tcc_error_noabort("not a valid YAFF file");

  if (header.exported_symbols_amount > 0)
  {
    YaffLib *lib;
    unsigned int region_size, hdr2[2], nbucket, nchain, lookup_size, chain_bytes;

    s1->yaff_libs = tcc_realloc(s1->yaff_libs, (s1->nb_yaff_libs + 1) * sizeof(YaffLib));
    lib = &s1->yaff_libs[s1->nb_yaff_libs++];
    memset(lib, 0, sizeof(*lib));
    lib->nsyms = header.exported_symbols_amount;

    /* name/value region: [exported_symbols_offset, imported_symbols_lookup_offset) */
    region_size = header.imported_symbols_lookup_offset - header.exported_symbols_offset;
    lib->region = tcc_malloc(region_size);
    lseek(fd, header.exported_symbols_offset, SEEK_SET);
    if ((unsigned)full_read(fd, lib->region, region_size) != region_size)
      return tcc_error_noabort("short read of YAFF export region");

    /* index -> region byte-offset lookup table (one u16 per exported symbol) */
    lookup_size = lib->nsyms * sizeof(unsigned short);
    lib->lookup = tcc_malloc(lookup_size);
    lseek(fd, header.exported_symbols_lookup_offset, SEEK_SET);
    if ((unsigned)full_read(fd, lib->lookup, lookup_size) != lookup_size)
      return tcc_error_noabort("short read of YAFF export lookup");

    /* name hash: [nbucket, nchain, bucket[nbucket], chain[nchain]] (self-sized) */
    lseek(fd, header.exported_symbols_hash_table_offset, SEEK_SET);
    if ((unsigned)full_read(fd, hdr2, sizeof(hdr2)) != sizeof(hdr2))
      return tcc_error_noabort("short read of YAFF hash header");
    nbucket = hdr2[0];
    nchain = hdr2[1];
    chain_bytes = (nbucket + nchain) * sizeof(unsigned int);
    lib->hash = tcc_malloc((2 + nbucket + nchain) * sizeof(unsigned int));
    lib->hash[0] = nbucket;
    lib->hash[1] = nchain;
    if ((unsigned)full_read(fd, lib->hash + 2, chain_bytes) != chain_bytes)
      return tcc_error_noabort("short read of YAFF hash table");
  }

  /* if the dll is already loaded, do not load it */
  tcc_add_dllref(s1, soname, level);

  return 0;
}

/* Resolve `name` against the loaded YAFF libraries via their on-disk hash
   tables, interning a hit into dynsymtab_section.  Returns the dynsymtab index
   (>0) or 0 if no loaded library exports it. */
ST_FUNC int tcc_yaff_resolve(TCCState *s1, const char *name)
{
  unsigned int h = tcc_yaff_hash(name);
  int li;
  for (li = 0; li < s1->nb_yaff_libs; li++)
  {
    YaffLib *lib = &s1->yaff_libs[li];
    unsigned int nbucket = lib->hash[0];
    unsigned int *bucket = lib->hash + 2;
    unsigned int *chain = lib->hash + 2 + nbucket;
    unsigned int i;
    for (i = bucket[h % nbucket]; i != 0; i = chain[i])
    {
      YaffSymbolEntry *e;
      const char *ename;
      if (i >= lib->nsyms)
        break; /* corrupt chain guard */
      e = (YaffSymbolEntry *)(lib->region + lib->lookup[i]);
      ename = (const char *)e + sizeof(YaffSymbolEntry);
      if (strcmp(ename, name) == 0)
        return set_elf_sym(s1->dynsymtab_section, e->offset, 1,
                           ELFW(ST_INFO)(e->weak ? STB_WEAK : STB_GLOBAL,
                                         e->section == YAFF_SECTION_CODE ? STT_FUNC : STT_NOTYPE),
                           STV_DEFAULT, 1, ename);
    }
  }
  return 0;
}

ST_FUNC void tcc_yaff_libs_free(TCCState *s1)
{
  int i;
  for (i = 0; i < s1->nb_yaff_libs; i++)
  {
    tcc_free(s1->yaff_libs[i].region);
    tcc_free(s1->yaff_libs[i].lookup);
    tcc_free(s1->yaff_libs[i].hash);
  }
  tcc_free(s1->yaff_libs);
  s1->yaff_libs = NULL;
  s1->nb_yaff_libs = 0;
}

/* Write local relocations for GOT entries that reference local symbols.
 *
 * When a local (STB_LOCAL) symbol's address is taken, put_got_entry()
 * creates a GOT slot and an R_RELATIVE relocation in .rel.got.
 * fill_local_got_entries() resolves these by writing the symbol's final
 * address into the GOT slot (first 4 bytes) and zeroing the sym_index.
 *
 * The dynamic loader needs to know about these GOT entries so it can:
 *  - Relocate the address at load time (text/data can be loaded anywhere)
 *  - Wrap function pointers in thunks that set up R9 (GOT base) when the
 *    function is called back from another module
 *
 * We detect "code" vs "data" by checking whether the resolved address
 * falls within the text section.  Code-section local relocations trigger
 * thunk generation in the loader.
 */
static int tcc_yaff_write_local_relocations(TCCState *s1, FILE *f)
{
  ElfW_Rel *rel;
  int count = 0;

  if (!s1->got || !s1->got->reloc)
  {
    LOG_YAFF("no GOT or no GOT relocs (got=%p, reloc=%p)", s1->got, s1->got ? s1->got->reloc : NULL);
    return 0;
  }

  LOG_YAFF("scanning .rel.got: got->sh_addr=0x%x, text=0x%x..0x%x, rodata=0x%x..0x%x",
             (unsigned)s1->got->sh_addr, (unsigned)text_section->sh_addr,
             (unsigned)(text_section->sh_addr + text_section->sh_size), (unsigned)rodata_section->sh_addr,
             (unsigned)(rodata_section->sh_addr + rodata_section->sh_size));

  for_each_elem(s1->got->reloc, 0, rel, ElfW_Rel)
  {
    int rtype = ELFW(R_TYPE)(rel->r_info);
    LOG_YAFF("rel: r_offset=0x%x, type=%d, sym=%d", (unsigned)rel->r_offset, rtype,
               ELFW(R_SYM)(rel->r_info));

    if (rtype != R_RELATIVE)
      continue;

    /* GOT entry offset within .got section */
    uint32_t got_offset = rel->r_offset - s1->got->sh_addr;
    /* Resolved address written by fill_local_got_entries() */
    uint32_t sym_value = read32le(s1->got->data + got_offset);
    /* Symbol type saved by fill_local_got_entries() in the second word */
    uint32_t sym_type = read32le(s1->got->data + got_offset + PTR_SIZE);

    LOG_YAFF("R_RELATIVE: got_offset=0x%x, sym_value=0x%x, sym_type=%u", got_offset, sym_value, sym_type);

    /* Determine which section this address belongs to */
    int section;
    uint32_t target_offset;
    if (sym_value >= text_section->sh_addr && sym_value < text_section->sh_addr + text_section->sh_size)
    {
      section = YAFF_SECTION_CODE;
      target_offset = sym_value - text_section->sh_addr;
    }
    else if (sym_value >= rodata_section->sh_addr && sym_value < rodata_section->sh_addr + rodata_section->sh_size)
    {
      section = YAFF_SECTION_DATA;
      target_offset = sym_value - rodata_section->sh_addr;
    }
    else if (sym_value >= data_section->sh_addr && sym_value < data_section->sh_addr + data_section->sh_size)
    {
      section = YAFF_SECTION_DATA;
      target_offset = sym_value - data_section->sh_addr + rodata_section->sh_size;
    }
    else if (sym_value >= bss_section->sh_addr && sym_value < bss_section->sh_addr + bss_section->sh_size)
    {
      section = YAFF_SECTION_DATA;
      target_offset = sym_value - bss_section->sh_addr + rodata_section->sh_size + data_section->sh_size;
    }
    else
    {
      LOG_YAFF("WARNING: sym_value 0x%x doesn't fall in any known section!", sym_value);
      section = YAFF_SECTION_DATA;
      target_offset = sym_value;
    }

    /* Code-section entries that are NOT function pointers (e.g. labels from
       goto *&&label) must not be wrapped in thunks by the dynamic loader.
       Skip emitting a local relocation for them — the loader's Phase A
       (GOT value resolution) will resolve the raw file offset stored in
       the GOT data and set the Thumb bit for code addresses. */
    if (section == YAFF_SECTION_CODE && sym_type != STT_FUNC)
    {
      LOG_YAFF("skipping non-function code address (sym_type=%u) for GOT[%u]", sym_type, got_offset / 8);
      continue;
    }

    LOG_YAFF("-> section=%s, index=%u, target_offset=0x%x", section == YAFF_SECTION_CODE ? "CODE" : "DATA",
               got_offset / 8, target_offset);

    /* Pack the (section:2, index:30) word manually rather than via a packed
       bitfield designated initializer.  The native (self-hosted) armv8m-tcc
       miscompiles the bitfield insert here: because `index` derives from a
       shift (`got_offset / 8`), the field's positional `<< 2` shift is dropped
       and the stored word becomes `(got_offset >> 3) | section` instead of
       `section | (index << 2)` — i.e. section reads back as garbage (3 =
       YAFF_SECTION_UNKNOWN) and the loader rejects the module with
       UnknownSection.  Manual packing into a plain uint32_t compiles
       correctly. */
    uint32_t reloc_index = got_offset / 8;
    uint32_t reloc_words[2];
    reloc_words[0] = ((uint32_t)section & 0x3u) | (reloc_index << 2);
    reloc_words[1] = target_offset;
    fwrite(reloc_words, 1, sizeof(reloc_words), f);
    ++count;
  }

  /* RELRO: emit the reserved rodata anchor slot (GOT index 3) so the loader
   * fills it with the runtime base of .rodata. Code addresses shared .rodata
   * symbols as anchor + R_ARM_RODATA_OFF(sym). Phase 2a points the anchor at
   * the per-process rodata (offset 0 of the data segment, where .rodata still
   * lives) — behaviour-preserving while the new codegen path is validated;
   * Phase 2b retargets it to YAFF_SECTION_RODATA (the shared XIP segment). */
  if (s1->share_rodata && s1->got)
  {
    uint32_t anchor_words[2];
    anchor_words[0] =
        ((uint32_t)YAFF_SECTION_DATA & 0x3u) | ((uint32_t)YAFF_RODATA_ANCHOR_GOT_INDEX << 2);
    anchor_words[1] = 0; /* .rodata is at offset 0 of the data segment */
    fwrite(anchor_words, 1, sizeof(anchor_words), f);
    ++count;
    LOG_YAFF("emitted rodata anchor local reloc: got index %d -> DATA off 0",
             YAFF_RODATA_ANCHOR_GOT_INDEX);
  }

  LOG_YAFF("total local relocations: %d", count);
  return count;
}

static int tcc_yaff_write_data_relocations(TCCState *s1, FILE *f)
{
  int i;
  Section *s;
  int number_of_data_relocations = 0;
  for (i = 0; i < s1->nb_sections; ++i)
  {
    if (i)
    {
      s = s1->sections[i];
      if (s->sh_type == SHT_REL)
      {
        /* Skip .rel.got — its R_RELATIVE entries are already emitted as
           local relocations by tcc_yaff_write_local_relocations().
           Processing them here as well would produce duplicate data
           relocations that overwrite the thunk addresses the dynamic
           loader places into GOT slots for function-pointer callbacks. */
        if (s1->got && s == s1->got->reloc)
          continue;
        ElfW_Rel *rel;
        for_each_elem(s, 0, rel, ElfW_Rel)
        {
          int type = ELFW(R_TYPE)(rel->r_info);
          switch (type)
          {
          // same as R_ARM_GOT_BREL
          case R_ARM_ABS32:
          case R_ARM_RELATIVE:
          {
            // first data section is rodata
            uint32_t abs_from_address = rel->r_offset;
            uint32_t from_address;
            uint32_t original_offset = 0;
            bool towards_code = false;
            YaffDataRelocationEntry entry;

            /* Check for imported symbol (e.g. fprintfptr = &fprintf).
               For imported symbols, the inline .data value is 0 because
               relocate() skips patching for dynamic symbols.  Emit a
               GOT-indirect data relocation (section=UNKNOWN) so the
               loader can resolve through the GOT entry and create a
               thunk for cross-module function pointers. */
            {
              int sym_idx = ELFW(R_SYM)(rel->r_info);
              if (sym_idx != 0 && s->link)
              {
                ElfW(Sym) *rel_sym = &((ElfW(Sym) *)s->link->data)[sym_idx];
                if (rel_sym->st_shndx == SHN_UNDEF)
                {
                  uint32_t imp_to = rel->r_offset;
                  if (!(s->sh_flags & SHF_ALLOC))
                  {
                    Section *target_sec = s1->sections[s->sh_info];
                    imp_to += target_sec->sh_addr;
                  }
                  if (imp_to >= data_section->sh_addr && imp_to < data_section->sh_addr + data_section->sh_size)
                  {
                    imp_to = (imp_to - data_section->sh_addr) + rodata_section->sh_size;
                  }
                  else if (imp_to >= bss_section->sh_addr && imp_to < bss_section->sh_addr + bss_section->sh_size)
                  {
                    imp_to = (imp_to - bss_section->sh_addr) + rodata_section->sh_size + data_section->sh_size;
                  }
                  else
                  {
                    imp_to -= rodata_section->sh_addr;
                  }

                  struct sym_attr *attr = get_sym_attr(s1, sym_idx, 0);
                  uint32_t got_offset = 0;
                  if (attr->got_offset)
                  {
                    got_offset = attr->got_offset;
                  }
                  else if (attr->plt_offset)
                  {
                    got_offset = read32le(s1->plt->data + attr->plt_offset + 4);
                  }
                  uint32_t got_index = got_offset / (PTR_SIZE * 2);

                  entry = (YaffDataRelocationEntry){
                      .to = imp_to,
                      .section = YAFF_SECTION_UNKNOWN, /* GOT-indirect */
                      .from = got_index,
                  };
                  fwrite(&entry, 1, sizeof(entry), f);
                  ++number_of_data_relocations;
                  break;
                }
              }
            }

            /* If the relocation section does not have SHF_ALLOC,
               r_offset is section-relative. Convert to absolute
               virtual address by adding the target section base. */
            if (!(s->sh_flags & SHF_ALLOC))
            {
              Section *target_sec = s1->sections[s->sh_info];
              abs_from_address += target_sec->sh_addr;
            }
            if (abs_from_address < rodata_section->sh_addr)
            {
              tcc_error_noabort("R_ARM_ABS32 relocation outside of data sections");
              continue;
            }
            from_address = abs_from_address - rodata_section->sh_addr;
            if (abs_from_address < rodata_section->sh_addr + rodata_section->sh_size)
            {
              // relocation inside .rodata
              original_offset = *(uint32_t *)(rodata_section->data + from_address);
            }
            else if (abs_from_address >= data_section->sh_addr && abs_from_address < data_section->sh_addr + data_section->sh_size)
            {
              original_offset = *(uint32_t *)(data_section->data + (abs_from_address - data_section->sh_addr));
              from_address = (abs_from_address - data_section->sh_addr) + rodata_section->sh_size;
            }
            else if (abs_from_address >= bss_section->sh_addr && abs_from_address < bss_section->sh_addr + bss_section->sh_size)
            {
              tcc_error_noabort("R_ARM_ABS32 relocation inside bss");
              continue;
            }
            else if (abs_from_address < s1->got->sh_addr + s1->got->sh_size)
            {
              uint32_t got_data_offset = abs_from_address - s1->got->sh_addr;
              if (got_data_offset + sizeof(uint32_t) > s1->got->data_offset)
              {
                tcc_error_noabort("R_ARM_ABS32 relocation outside allocated GOT data");
                continue;
              }
              original_offset = *(uint32_t *)(s1->got->data + got_data_offset);
            }
            else
            {
              tcc_error_noabort("R_ARM_ABS32 relocation outside of data "
                                "sections or GOT");
              continue;
            }

            towards_code = original_offset < rodata_section->sh_addr;
            if (!towards_code)
            {
              if (original_offset >= bss_section->sh_addr && original_offset < bss_section->sh_addr + bss_section->sh_size)
              {
                original_offset = (original_offset - bss_section->sh_addr) + rodata_section->sh_size + data_section->sh_size;
              }
              else if (original_offset >= data_section->sh_addr && original_offset < data_section->sh_addr + data_section->sh_size)
              {
                original_offset = (original_offset - data_section->sh_addr) + rodata_section->sh_size;
              }
              else
              {
                original_offset -= rodata_section->sh_addr;
              }
            }

            entry = (YaffDataRelocationEntry){
                .to = from_address,
                .section = towards_code ? YAFF_SECTION_CODE : YAFF_SECTION_DATA,
                .from = original_offset,
            };
            fwrite(&entry, 1, sizeof(entry), f);
            ++number_of_data_relocations;
          }
          break;
          case R_ARM_CALL:
          case R_ARM_JUMP24:
          case R_ARM_GOT32:
          case R_ARM_GLOB_DAT:
          case R_ARM_JUMP_SLOT:
          case R_ARM_REL32:
          case R_ARM_THM_JUMP24:
          case R_ARM_PREL31:
          case R_ARM_TARGET1:
          case R_ARM_NONE:
          case R_ARM_GOTOFF:
          case R_ARM_RODATA_OFF:
          case R_ARM_GOT_SBREL12:
          case R_ARM_GOTPC:
          case R_ARM_GOT_PREL:
          case R_ARM_PC24:
          case R_ARM_PLT32:
          case R_ARM_THM_PC22:
          case R_ARM_THM_JUMP19:
          case R_ARM_V4BX:
          case R_ARM_THM_ALU_PREL_11_0:
          case R_ARM_THM_JUMP6:
          case R_ARM_THM_PC12:
          case R_ARM_THM_PC8:
          case R_ARM_COPY:
          case R_ARM_MOVT_ABS:
          case R_ARM_MOVW_ABS_NC:
          case R_ARM_THM_MOVT_ABS:
          case R_ARM_THM_MOVW_ABS_NC:
          case R_ARM_MOVT_PREL:
          case R_ARM_MOVW_PREL_NC:
          {
            // relocations resolved at link time - no runtime data relocation
            // needed (PC-relative, GOT-relative, or already patched inline)
            continue;
          }
          default:
          {
            tcc_warning("unknown relocation type %d", type);
            break;
          }
          }
        }
      }
    }
  }
  return number_of_data_relocations;
}

static int tcc_yaff_write_symbol_table_relocations(TCCState *s1, FILE *f)
{
  int i;
  Section *s;
  int number_of_symbol_table_relocations = 0;

  int dynsym_count = s1->dynsym->data_offset / sizeof(ElfW(Sym));

  /* Pre-build index maps: for each dynsym entry, compute its 1-based
   * index in the imported or exported symbols table.  This correctly
   * handles interleaved import/export ordering in dynsym, where the
   * old formula (symbol_index - 1 - number_of_imported) assumed all
   * imports precede all exports. */
  int *imported_idx = tcc_mallocz(dynsym_count * sizeof(int));
  int *exported_idx = tcc_mallocz(dynsym_count * sizeof(int));
  {
    int imp_count = 0, exp_count = 0;
    for (int idx = 1; idx < dynsym_count; idx++)
    {
      ElfW(Sym) *ds = &((ElfW(Sym) *)s1->dynsym->data)[idx];
      if (ds->st_shndx == SHN_UNDEF)
      {
        imported_idx[idx] = ++imp_count;
      }
      else if (tcc_yaff_sym_is_exported(ds))
      {
        exported_idx[idx] = ++exp_count;
      }
    }
  }

  /* Pre-scan: build a set of symbol indices that have JUMP_SLOT (PLT)
   * relocations. These are known function symbols. This is needed because
   * tccelf.c strips STT_FUNC to STT_NOTYPE for undefined symbols in
   * TCC_OUTPUT_OBJ mode, so the GLOB_DAT handler below can no longer
   * rely solely on st_info to detect function pointers.  Symbols that
   * appear in both JUMP_SLOT (direct call) and GLOB_DAT (address taken)
   * are functions whose GLOB_DAT entry needs a thunk.
   *
   * NOTE: This heuristic is only applied to imported (undefined) symbols.
   * Exported symbols retain their correct st_info type, so STT_FUNC alone
   * suffices.  Without this guard, linker boundary symbols (__start_xxx,
   * __stop_xxx) which are STT_NOTYPE but may share a dynsym index space
   * with JUMP_SLOT entries get incorrectly marked as function pointers. */
  unsigned char *has_jump_slot = tcc_mallocz(dynsym_count);
  for (int j = 0; j < s1->nb_sections; ++j)
  {
    Section *sec;
    if (!j)
      continue;
    sec = s1->sections[j];
    if (sec->sh_type != SHT_REL || sec->link != s1->dynsym)
      continue;
    ElfW_Rel *r;
    for_each_elem(sec, 0, r, ElfW_Rel)
    {
      if (ELFW(R_TYPE)(r->r_info) == R_ARM_JUMP_SLOT)
      {
        int idx = ELFW(R_SYM)(r->r_info);
        if (idx >= 0 && idx < dynsym_count)
          has_jump_slot[idx] = 1;
      }
    }
  }

  for (i = 0; i < s1->nb_sections; ++i)
  {
    if (i)
    {
      s = s1->sections[i];
      if (s->sh_type == SHT_REL)
      {
        /* Only process relocation sections linked to dynsym.
           Sections linked to symtab_section (e.g. .rel.data, .rel.rodata)
           contain static relocations with symbol indices into the static
           symbol table - those are handled by tcc_yaff_write_data_relocations
           instead. */
        if (s->link != s1->dynsym)
          continue;
        ElfW_Rel *rel;
        for_each_elem(s, 0, rel, ElfW_Rel)
        {
          int symbol_index = ELFW(R_SYM)(rel->r_info);
          int type = ELFW(R_TYPE)(rel->r_info);
          // index is not necessarily the same in the symtab and imported
          // symbols table
          ElfW(Sym) *sym = &((ElfW(Sym) *)s1->dynsym->data)[symbol_index];
          YaffSymbolTableRelocationEntry entry;
          switch (type)
          {
          // same as R_ARM_GOT_BREL
          case R_ARM_GLOB_DAT:
          case R_ARM_JUMP_SLOT:
          case R_ARM_GOT32:
          {
            // this is exported symbol
            int is_exported = (sym->st_shndx != SHN_UNDEF);
            int is_function_pointer = 0;
            if (type == R_ARM_GLOB_DAT &&
                (ELFW(ST_TYPE)(sym->st_info) == STT_FUNC ||
                 (!is_exported && has_jump_slot && symbol_index < dynsym_count && has_jump_slot[symbol_index])))
            {
              is_function_pointer = 1;
            }
            int is_plt_call = (type == R_ARM_JUMP_SLOT) ? 1 : 0;
            entry = (YaffSymbolTableRelocationEntry){
                .is_exported_symbol = is_exported,
                .index = (rel->r_offset - s1->got->sh_addr) / 8,
                .function_pointer = is_function_pointer,
                .plt_call = is_plt_call,
                .symbol_index = is_exported ? exported_idx[symbol_index] : imported_idx[symbol_index],
            };
            fwrite(&entry, 1, sizeof(entry), f);
            ++number_of_symbol_table_relocations;
          }
          break;
          case R_ARM_REL32:
          case R_ARM_RELATIVE:
          case R_ARM_CALL:
          case R_ARM_JUMP24:
          case R_ARM_THM_JUMP24:
          case R_ARM_ABS32:
          case R_ARM_COPY:
          case R_ARM_PREL31:
          case R_ARM_TARGET1:
          case R_ARM_NONE:
          case R_ARM_RODATA_OFF:
          case R_ARM_GOT_SBREL12:
          {
            // relocations that are safe to ignore due to their PC relative
            // nature (R_ARM_RODATA_OFF is resolved at link time into the .text
            // literal, like R_ARM_GOTOFF — no runtime YAFF relocation needed;
            // R_ARM_GOT_SBREL12 likewise bakes the slot offset into the imm12,
            // and the slot itself is relocated via GLOB_DAT/JUMP_SLOT)
            continue;
          }
          default:
          {
            tcc_warning("unknown relocation type %d", type);
            break;
          }
          }
        }
      }
    }
  }
  tcc_free(has_jump_slot);
  tcc_free(imported_idx);
  tcc_free(exported_idx);
  return number_of_symbol_table_relocations;
}

static int tcc_yaff_write_imported_symbols(TCCState *s1, FILE *f, YaffHeader *h)
{
  ElfW(Sym) * sym;
  int number_of_imported_symbols = 1;
  /* Allocate common symbols in BSS.  */
  YaffSymbolEntry entry = {
      0,
      0,
  };
  fwrite(&entry, 1, sizeof(entry), f);
  for (int j = 0; j < 4; ++j)
  {
    fputc(0, f);
  }
  for_each_elem(s1->dynsym, 1, sym, ElfW(Sym))
  {
    YaffSymbolEntry entry = {};
    int name_len = 0, aligned_name_len = 0;
    char *name = NULL;
    if (sym->st_shndx != SHN_UNDEF)
    {
      continue;
    }
    entry = (YaffSymbolEntry){
        .section = 0,
        .weak = (ELFW(ST_BIND)(sym->st_info) == STB_WEAK) ? 1 : 0,
        .offset = sym->st_value,
    };

    number_of_imported_symbols++;
    fwrite(&entry, 1, sizeof(entry), f);
    name = (char *)s1->dynsym->link->data + sym->st_name;
    name_len = strlen(name) + 1;

    fwrite(name, 1, name_len, f);
    aligned_name_len = tcc_yaff_align(h, name_len);
    for (int j = 0; j < aligned_name_len - name_len; ++j)
    {
      fputc(0, f);
    }
  }
  return number_of_imported_symbols;
}

static int tcc_yaff_write_exported_symbols(TCCState *s1, FILE *f, YaffHeader *h)
{
  ElfW(Sym) * sym;
  int number_of_exported_symbols = 1;
  YaffSymbolEntry entry = {
      0,
      0,
  };
  fwrite(&entry, 1, sizeof(entry), f);
  for (int j = 0; j < 4; ++j)
  {
    fputc(0, f);
  }
  for_each_elem(s1->dynsym, 1, sym, ElfW(Sym))
  {
    int section_code = 0;
    YaffSymbolEntry entry = {};
    int name_len = 0, aligned_name_len = 0;
    char *name = NULL;
    uint32_t offset = 0;
    unsigned bind = ELFW(ST_BIND)(sym->st_info);
    if (!tcc_yaff_sym_is_exported(sym))
    {
      continue;
    }
    if (sym->st_shndx == text_section->sh_num)
    {
      section_code = YAFF_SECTION_CODE;
    }
    else if (sym->st_shndx == data_section->sh_num || sym->st_shndx == rodata_section->sh_num ||
             sym->st_shndx == bss_section->sh_num)
    {
      section_code = YAFF_SECTION_DATA;
    }
    else
    {
      section_code = YAFF_SECTION_CODE;
    }
    offset = sym->st_value;
    if (section_code == YAFF_SECTION_DATA)
    {
      if (sym->st_shndx == bss_section->sh_num)
      {
        offset = (offset - bss_section->sh_addr) + rodata_section->sh_size + data_section->sh_size;
      }
      else if (sym->st_shndx == data_section->sh_num)
      {
        offset = (offset - data_section->sh_addr) + rodata_section->sh_size;
      }
      else
      {
        offset -= rodata_section->sh_addr;
      }
    }
    entry = (YaffSymbolEntry){
        .section = section_code,
        .weak = (bind == STB_WEAK) ? 1 : 0,
        .offset = offset,
    };
    number_of_exported_symbols++;
    fwrite(&entry, 1, sizeof(entry), f);
    name = (char *)s1->dynsym->link->data + sym->st_name;
    name_len = strlen(name) + 1;
    fwrite(name, 1, name_len, f);
    aligned_name_len = tcc_yaff_align(h, name_len);
    for (int j = 0; j < aligned_name_len - name_len; ++j)
    {
      fputc(0, f);
    }
  }
  return number_of_exported_symbols;
}

static void tcc_yaff_write_imported_symbols_lookup(TCCState *s1, FILE *f, YaffHeader *h, YaffHashTable *hashtable)
{
  int i = 0;
  ElfW(Sym) * sym;
  int current_offset = 0;
  YaffLookupEntry entry = {
      .symbol_offset = current_offset,
  };

  tcc_add_hash_entry(hashtable, "", i++);
  fwrite(&entry, sizeof(entry), 1, f);
  current_offset += sizeof(uint32_t) + 4;
  /* Allocate common symbols in BSS.  */
  for_each_elem(s1->dynsym, 1, sym, ElfW(Sym))
  {
    int name_len = 0, aligned_name_len = 0;
    char *name = NULL;
    entry.symbol_offset = current_offset;
    if (sym->st_shndx != SHN_UNDEF)
    {
      continue;
    }
    /* The symbol table, hashtable and header.imported_symbols_amount were all
     * sized from tcc_yaff_write_imported_symbols' count.  This lookup pass
     * re-filters s1->dynsym independently; if the two passes ever disagree on
     * the matching-symbol count (they should be identical, but a self-host
     * codegen miscompile of one loop can make them differ), adding more than
     * `amount` entries overflows the nchain-sized chain[]/bucket[] arrays
     * (tcc_add_hash_entry writes chain[idx]=i for idx==amount), corrupting the
     * heap and faulting the chain walk.  Never reference a symbol the symbol
     * table doesn't contain. */
    if (i >= (int)h->imported_symbols_amount)
      break;
    name = (char *)s1->dynsym->link->data + sym->st_name;
    tcc_add_hash_entry(hashtable, name, i++);
    name_len = strlen(name) + 1;
    aligned_name_len = tcc_yaff_align(h, name_len);
    current_offset += sizeof(uint32_t) + aligned_name_len;
    fwrite(&entry, sizeof(entry), 1, f);
  }
}

static void tcc_yaff_write_exported_symbols_lookup(TCCState *s1, FILE *f, YaffHeader *h, YaffHashTable *hashtable)
{
  int i = 0;
  ElfW(Sym) * sym;
  int current_offset = 0; // sizeof(uint32_t) + 4;
  YaffLookupEntry entry = {
      .symbol_offset = current_offset,
  };
  tcc_add_hash_entry(hashtable, "", i++);
  fwrite(&entry, sizeof(entry), 1, f);
  current_offset += sizeof(uint32_t) + 4;
  for_each_elem(s1->dynsym, 1, sym, ElfW(Sym))
  {
    int name_len = 0, aligned_name_len = 0;
    char *name = NULL;
    if (!tcc_yaff_sym_is_exported(sym))
    {
      continue;
    }

    entry.symbol_offset = current_offset;
    /* See tcc_yaff_write_imported_symbols_lookup: bound entries to the count the
     * sizing pass produced so the hashtable can never reference a symbol the
     * exported symbol table doesn't contain (prevents the chain[]/bucket[]
     * overflow + heap corruption when the two filter passes disagree). */
    if (i >= (int)h->exported_symbols_amount)
      break;
    name = (char *)s1->dynsym->link->data + sym->st_name;
    tcc_add_hash_entry(hashtable, name, i++);
    name_len = strlen(name) + 1;
    aligned_name_len = tcc_yaff_align(h, name_len);
    current_offset += sizeof(uint32_t) + aligned_name_len;
    fwrite(&entry, sizeof(entry), 1, f);
  }
}

/* Merge .init_array and .fini_array sections into .data for YAFF output.
 *
 * After relocate_sections() has resolved all relocations, the .init_array
 * and .fini_array sections contain absolute ELF virtual addresses of
 * constructor/destructor functions.  We append this data to the .data
 * section so that:
 *  1. The existing YAFF data-relocation mechanism produces runtime fixups
 *     for each function pointer.
 *  2. Boundary symbols (__init_array_start/end, __fini_array_start/end)
 *     let the CRT iterate the arrays at startup/shutdown.
 */
/* Merge .init_array / .fini_array into .data BEFORE GOT building and
 * relocation so that:
 *   - relocations are applied naturally by relocate_sections()
 *   - YAFF data relocation entries are generated automatically
 *   - the __yaff_initfini symbol uses the R_RELATIVE (local) GOT path
 *
 * Layout appended to data_section:
 *   [uint32_t init_count][uint32_t fini_count][init func ptrs...][fini func ptrs...]
 *
 * A LOCAL symbol __yaff_initfini is defined pointing to this struct so that
 * crt1.c can find it via a GOT-indirect access through a local relocation. */
ST_FUNC void tcc_yaff_prepare_init_fini(TCCState *s1)
{
  Section *ia = NULL, *fa = NULL;
  int i;

  /* Find .init_array / .fini_array by section type */
  for (i = 1; i < s1->nb_sections; ++i)
  {
    if (s1->sections[i]->sh_type == SHT_INIT_ARRAY)
      ia = s1->sections[i];
    else if (s1->sections[i]->sh_type == SHT_FINI_ARRAY)
      fa = s1->sections[i];
  }

  uint32_t ia_count = ia ? ia->data_offset / PTR_SIZE : 0;
  uint32_t fa_count = fa ? fa->data_offset / PTR_SIZE : 0;

  /* Record where the struct will land inside data_section */
  uint32_t struct_offset = data_section->data_offset;

  /* Write header: init_count, fini_count */
  {
    uint8_t *hdr = section_ptr_add(data_section, 2 * sizeof(uint32_t));
    write32le(hdr, ia_count);
    write32le(hdr + sizeof(uint32_t), fa_count);
  }

  /* Append .init_array function pointers */
  uint32_t init_data_offset = data_section->data_offset;
  if (ia && ia->data_offset)
  {
    uint8_t *dst = section_ptr_add(data_section, ia->data_offset);
    memcpy(dst, ia->data, ia->data_offset);
    /* Copy relocations with adjusted offsets */
    if (ia->reloc)
    {
      ElfW_Rel *rel;
      for_each_elem(ia->reloc, 0, rel, ElfW_Rel)
      {
        put_elf_reloc(s1->symtab, data_section, init_data_offset + rel->r_offset, ELFW(R_TYPE)(rel->r_info),
                      ELFW(R_SYM)(rel->r_info));
      }
    }
  }

  /* Append .fini_array function pointers */
  uint32_t fini_data_offset = data_section->data_offset;
  if (fa && fa->data_offset)
  {
    uint8_t *dst = section_ptr_add(data_section, fa->data_offset);
    memcpy(dst, fa->data, fa->data_offset);
    if (fa->reloc)
    {
      ElfW_Rel *rel;
      for_each_elem(fa->reloc, 0, rel, ElfW_Rel)
      {
        put_elf_reloc(s1->symtab, data_section, fini_data_offset + rel->r_offset, ELFW(R_TYPE)(rel->r_info),
                      ELFW(R_SYM)(rel->r_info));
      }
    }
  }

  /* Define __yaff_initfini as a LOCAL symbol in symtab.
   * If crt1.o already declared it as extern (STB_GLOBAL, SHN_UNDEF),
   * convert it to LOCAL + defined so that build_got_entries() will
   * use the R_RELATIVE (local relocation) path for its GOT entry.
   *
   * We do a linear scan instead of find_elf_sym() because the hash
   * table only indexes non-LOCAL symbols and we need to catch every
   * entry (there may be more than one if multiple object files
   * reference the name). */
  {
    int nb_syms = s1->symtab->data_offset / sizeof(ElfW(Sym));
    int found = 0;
    for (i = 1; i < nb_syms; ++i)
    {
      ElfW(Sym) *sym = &((ElfW(Sym) *)s1->symtab->data)[i];
      const char *sname = (char *)s1->symtab->link->data + sym->st_name;
      if (!strcmp(sname, "__yaff_initfini"))
      {
        sym->st_info = ELFW(ST_INFO)(STB_LOCAL, STT_OBJECT);
        sym->st_value = struct_offset;
        sym->st_size = data_section->data_offset - struct_offset;
        sym->st_shndx = data_section->sh_num;
        found = 1;
      }
    }
    if (!found)
    {
      put_elf_sym(s1->symtab, struct_offset, data_section->data_offset - struct_offset,
                  ELFW(ST_INFO)(STB_LOCAL, STT_OBJECT), 0, data_section->sh_num, "__yaff_initfini");
    }
  }

  /* Suppress the original .init_array / .fini_array sections so they
   * don't get laid out or produce duplicate relocations.  Clear the
   * type so they're ignored by section iterators. */
  if (ia)
  {
    ia->sh_type = SHT_NULL;
    ia->sh_flags = 0;
    if (ia->reloc)
    {
      ia->reloc->sh_type = SHT_NULL;
      ia->reloc->sh_flags = 0;
    }
  }
  if (fa)
  {
    fa->sh_type = SHT_NULL;
    fa->sh_flags = 0;
    if (fa->reloc)
    {
      fa->reloc->sh_type = SHT_NULL;
      fa->reloc->sh_flags = 0;
    }
  }
}

ST_FUNC int tcc_output_yaff(TCCState *s1, FILE *f, const char *filename)
{
  int i, file_type;
  YaffHeader header = {};
  char *name;
  uint32_t aligned_name_len = 0;
  uint32_t text_offset = 0, aligned_text_offset = 0;
  scoped_yaff_hash_table imported_symbols_hashtable = {0};
  scoped_yaff_hash_table exported_symbols_hashtable = {0};
  fflush(stdout);

  /* Materialize lazy sections before accessing their data pointers */
  section_materialize(s1, text_section);
  section_materialize(s1, rodata_section);
  section_materialize(s1, data_section);
  if (s1->got)
    section_materialize(s1, s1->got);
  if (s1->plt)
    section_materialize(s1, s1->plt);

  name = tcc_basename(filename);

  file_type = s1->output_type;

  memcpy(header.magic, YAFFMAG, sizeof(YAFFMAG) - 1);
  header.module_type = file_type == TCC_OUTPUT_DYN ? YAFF_MODULE_TYPE_SHARED_LIBRARY : YAFF_MODULE_TYPE_EXECUTABLE;
  if (s1->elf_entryname)
  {
    header.entry = get_sym_addr(s1, s1->elf_entryname, 1, 0);
  }
  else
  {
    header.entry = get_sym_addr(s1, "_start", !!(file_type & TCC_OUTPUT_EXE), 0);
  }
  if (header.entry == (addr_t)-1)
  {
    header.entry = 0;
  }
  if (s1->nb_errors)
  {
    return -1;
  }

  header.yaff_version = YAFF_VERSION;
#if defined(TCC_TARGET_ARM_ARCHV8M)
  header.arch = YAFF_ARCH_ARMV8_M;
#elif defined(TCC_TARGET_ARM_ARCHV6M)
  header.arch = YAFF_ARCH_ARMV6_M;
#else
  header.arch = YAFF_ARCH_UNKNOWN;
#endif
  header.code_length = text_section->sh_size;
  header.init_length = 0;
  /* data_length must include any alignment padding between rodata and data
   * so that GOTOFF offsets remain consistent at runtime. */
  {
    addr_t rodata_end = rodata_section->sh_addr + rodata_section->sh_size;
    addr_t data_start = data_section->sh_addr;
    addr_t rd_padding = (data_start > rodata_end) ? (data_start - rodata_end) : 0;
    header.data_length = rodata_section->sh_size + rd_padding + data_section->sh_size;
    if (s1->share_rodata)
    {
      /* RELRO: the first rodata_size bytes of the data segment are pure-const
       * .rodata, shared XIP. The loader maps them once (borrowed) and only
       * allocates/copies the remaining data per process, resolving a DATA
       * target with offset < const_rodata_length to the shared rodata.
       *
       * SOUNDNESS GATE: this is only valid if .rodata is genuinely relocation
       * -free. The frontend split (-share-rodata) moves pointer-bearing const
       * *objects* to the writable data segment, but COMPILER-GENERATED const
       * with relocations (e.g. switch jump tables holding code addresses)
       * bypasses that and stays in .rodata. A relocation patch site lands in
       * the shared XIP rodata, which the loader cannot write -> fault. So only
       * share when .rodata carries no relocations (and rd_padding == 0, since
       * the data cascade assumes it). Modules that fail the gate keep rodata
       * per-process (const_rodata_length stays 0 -> legacy behaviour). */
      int rodata_has_relocs = (rodata_section->reloc != NULL && rodata_section->reloc->data_offset > 0);
      if (rd_padding != 0 || rodata_has_relocs)
      {
        LOG_YAFF("share-rodata: NOT sharing (.rodata has %u reloc bytes, rd_padding=%u)",
                 rodata_section->reloc ? (unsigned)rodata_section->reloc->data_offset : 0u,
                 (unsigned)rd_padding);
      }
      else
      {
        header.const_rodata_length = (uint32_t)rodata_section->sh_size;
      }
    }
  }
  /* bss_length must include any alignment padding between data and bss,
   * AND between bss and GOT, so that the loader reproduces the exact
   * same distance between rodata and GOT that the linker used for
   * R_ARM_GOTOFF relocations. */
  {
    addr_t data_end = data_section->sh_addr + data_section->sh_size;
    addr_t bss_start = bss_section->sh_addr;
    addr_t bss_end = bss_section->sh_addr + bss_section->sh_size;
    addr_t got_start = s1->got->sh_addr;
    addr_t pad_before_bss = (bss_start > data_end) ? (bss_start - data_end) : 0;
    addr_t pad_after_bss = (got_start > bss_end) ? (got_start - bss_end) : 0;
    header.bss_length = pad_before_bss + bss_section->sh_size + pad_after_bss;
  }

  header.external_libraries_amount = 0;
  header.alignment = 4;
  header.version_major = 0;
  header.version_minor = 0;
  header.external_libraries_amount = s1->nb_loaded_dlls;
  if (s1->text_and_data_separation)
  {
    header.text_and_data_separation = 1;
  }
  else
  {
    header.text_and_data_separation = 0;
  }
  /* Per-image stack/heap hints (bytes). 0xFFFFFFFF = "use the OS default"
   * (kernel-driven stack size; heap free to grow in the shared paged pool).
   * A concrete value lets the kernel bound the process to a fixed footprint
   * (the basis for MPU-guarded, profile-limited processes). The internal
   * TCCState fields default to 0 (option not given) which we map to the
   * default sentinel here. */
  header.stack_size = s1->yaff_stack_size ? s1->yaff_stack_size : 0xFFFFFFFFu;
  header.heap_size = s1->yaff_heap_size ? s1->yaff_heap_size : 0xFFFFFFFFu;
  /* header.const_rodata_length was set in the data_length block above
   * (rodata_size when -share-rodata, else 0). */
  if (!s1->share_rodata)
    header.const_rodata_length = 0;

  fwrite(&header, 1, sizeof(YaffHeader), f);
  aligned_name_len = strlen(name) + 1;
  aligned_name_len = tcc_yaff_align(&header, aligned_name_len);
  fwrite(name, 1, strlen(name) + 1, f);
  for (i = 0; i < aligned_name_len - strlen(name) - 1; ++i)
  {
    fputc(0, f);
  }

  /* Architecture section: what the loader has to be able to provide before it
   * may run this code. Kept ahead of everything else it would have to parse so
   * an image for another part is rejected before any of it is trusted. */
  header.arch_section_offset = ftell(f);
  {
    YaffArchSection arch_section;
    uint32_t aligned_arch_len = tcc_yaff_align(&header, (uint32_t)sizeof(YaffArchSection));
    tcc_yaff_fill_arch_section(s1, &header, &arch_section);
    fwrite(&arch_section, 1, sizeof(YaffArchSection), f);
    for (i = 0; i < (int)(aligned_arch_len - sizeof(YaffArchSection)); ++i)
    {
      fputc(0, f);
    }
  }

  header.imported_libraries_offset = ftell(f);
  for (i = 0; i < s1->nb_loaded_dlls; ++i)
  {
    DLLReference *dll = s1->loaded_dlls[i];
    aligned_name_len = strlen(dll->name) + 1;
    aligned_name_len = tcc_yaff_align(&header, aligned_name_len);
    fwrite(dll->name, 1, strlen(dll->name) + 1, f);
    for (int j = 0; j < aligned_name_len - strlen(dll->name) - 1; ++j)
    {
      fputc(0, f);
    }
  }

  // tcc_elf_sort_syms(s1, s1->symtab);
  header.relocations_offset = ftell(f);
  header.symbol_table_relocations_amount = tcc_yaff_write_symbol_table_relocations(s1, f);
  header.local_relocations_amount = tcc_yaff_write_local_relocations(s1, f);
  header.data_relocations_amount = tcc_yaff_write_data_relocations(s1, f);
  header.copy_relocations_amount = 0;

  header.imported_symbols_offset = ftell(f);
  header.imported_symbols_amount = tcc_yaff_write_imported_symbols(s1, f, &header);
  header.exported_symbols_offset = ftell(f);
  header.exported_symbols_amount = tcc_yaff_write_exported_symbols(s1, f, &header);
  header.imported_symbols_lookup_offset = ftell(f);

  tcc_allocate_hash_table(&imported_symbols_hashtable, header.imported_symbols_amount + 1,
                          header.imported_symbols_amount);
  tcc_yaff_write_imported_symbols_lookup(s1, f, &header, &imported_symbols_hashtable);
  header.exported_symbols_lookup_offset = ftell(f);

  tcc_allocate_hash_table(&exported_symbols_hashtable, header.exported_symbols_amount + 1,
                          header.exported_symbols_amount);
  tcc_yaff_write_exported_symbols_lookup(s1, f, &header, &exported_symbols_hashtable);

  header.imported_symbols_hash_table_offset = ftell(f);
  if (header.imported_symbols_hash_table_offset % header.alignment != 0)
  {
    for (i = 0; i < header.alignment - (header.imported_symbols_hash_table_offset % header.alignment); ++i)
    {
      fputc(0, f);
    }
    header.imported_symbols_hash_table_offset = ftell(f); // align after padding
  }
  tcc_write_hash_table(&imported_symbols_hashtable, f);
  header.exported_symbols_hash_table_offset = ftell(f);
  if (header.exported_symbols_hash_table_offset % header.alignment != 0)
  {
    for (i = 0; i < header.alignment - (header.exported_symbols_hash_table_offset % header.alignment); ++i)
    {
      fputc(0, f);
    }
    header.exported_symbols_hash_table_offset = ftell(f); // align after padding
  }

  tcc_write_hash_table(&exported_symbols_hashtable, f);

  header.got_length = s1->got->sh_size;
  text_offset = ftell(f);
  aligned_text_offset = (text_offset + 15) & ~15;
  for (i = 0; i < aligned_text_offset - text_offset; ++i)
  {
    fputc(0, f);
  }
  header.text_offset = aligned_text_offset;

  fwrite(text_section->data, 1, text_section->sh_size, f);
  if (s1->plt)
  {
    fwrite(s1->plt->data, 1, s1->plt->sh_size, f);
    header.plt_length = s1->plt->sh_size;
  }

  fwrite(rodata_section->data, 1, rodata_section->sh_size, f);
  /* Write alignment padding between rodata and data (if any) */
  {
    addr_t rodata_end = rodata_section->sh_addr + rodata_section->sh_size;
    addr_t data_start = data_section->sh_addr;
    if (data_start > rodata_end) {
      unsigned pad = (unsigned)(data_start - rodata_end);
      for (i = 0; i < pad; ++i)
        fputc(0, f);
    }
  }
  fwrite(data_section->data, 1, data_section->sh_size, f);
  /* No file padding between data and GOT — the bss region (including
   * any alignment padding before it) is zero-initialized by the loader.
   * bss_length already accounts for the alignment gap. */
  fwrite(s1->got->data, 1, s1->got->sh_size, f);
  fseek(f, 0, SEEK_SET);
  fwrite(&header, 1, sizeof(YaffHeader), f);
  fflush(f);

  return 0;
}
