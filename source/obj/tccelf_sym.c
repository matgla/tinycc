/*
 *  ELF file handling for TCC
 *
 *  Copyright (c) 2001-2004 Fabrice Bellard
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

/* ELF: sections, string and symbol tables, symbol lookup and versions,
 * relocation entries and the GNU hash table. */

#include "tccelf_priv.h"

/* ---- Global section name hash table ---- */

static unsigned int section_name_hash(const char *name)
{
  unsigned int h = 5381;
  const unsigned char *p = (const unsigned char *)name;
  while (*p)
    h = ((h << 5) + h) ^ *p++;
  return h;
}

static void section_ht_grow(TCCState *s1)
{
  unsigned int new_size = s1->section_ht_mask ? (s1->section_ht_mask + 1) * 2 : 64;
  Section **new_ht = tcc_mallocz(new_size * sizeof(Section *));
  unsigned int new_mask = new_size - 1;
  if (s1->section_ht)
  {
    unsigned int i;
    for (i = 0; i <= s1->section_ht_mask; i++)
    {
      Section *s = s1->section_ht[i];
      if (s)
      {
        unsigned int idx = section_name_hash(s->name) & new_mask;
        while (new_ht[idx])
          idx = (idx + 1) & new_mask;
        new_ht[idx] = s;
      }
    }
    tcc_free(s1->section_ht);
  }
  s1->section_ht = new_ht;
  s1->section_ht_mask = new_mask;
}

static void section_ht_insert(TCCState *s1, Section *sec)
{
  unsigned int idx;
  if (s1->section_ht_count * 2 >= s1->section_ht_mask)
    section_ht_grow(s1);
  idx = section_name_hash(sec->name) & s1->section_ht_mask;
  while (s1->section_ht[idx])
    idx = (idx + 1) & s1->section_ht_mask;
  s1->section_ht[idx] = sec;
  s1->section_ht_count++;
}
Section *section_ht_find(TCCState *s1, const char *name)
{
  unsigned int idx;
  Section *s;
  if (!s1->section_ht)
    return NULL;
  idx = section_name_hash(name) & s1->section_ht_mask;
  while ((s = s1->section_ht[idx]) != NULL)
  {
    if (!strcmp(s->name, name))
      return s;
    idx = (idx + 1) & s1->section_ht_mask;
  }
  return NULL;
}

/* ---- End section name hash table ---- */

ST_FUNC Section *new_section(TCCState *s1, const char *name, int sh_type, int sh_flags)
{
  Section *sec;

  sec = tcc_mallocz(sizeof(Section) + strlen(name));
  sec->s1 = s1;
  strcpy(sec->name, name);
  sec->sh_type = sh_type;
  sec->sh_flags = sh_flags;
  switch (sh_type)
  {
  case SHT_GNU_versym:
    sec->sh_addralign = 2;
    break;
  case SHT_HASH:
  case SHT_GNU_HASH:
  case SHT_REL:
  case SHT_RELA:
  case SHT_DYNSYM:
  case SHT_SYMTAB:
  case SHT_DYNAMIC:
  case SHT_GNU_verneed:
  case SHT_GNU_verdef:
    sec->sh_addralign = 8; // PTR_SIZE;
    break;
  case SHT_STRTAB:
    sec->sh_addralign = 1;
    break;
  default:
    sec->sh_addralign = 8; // PTR_SIZE; /* gcc/pcc default alignment */
    break;
  }

  if (sh_flags & SHF_PRIVATE)
  {
    dynarray_add(&s1->priv_sections, &s1->nb_priv_sections, sec);
  }
  else
  {
    sec->sh_num = s1->nb_sections;
    dynarray_add(&s1->sections, &s1->nb_sections, sec);
    section_ht_insert(s1, sec);
  }

  return sec;
}

ST_FUNC void init_symtab(Section *s)
{
  int *ptr, nb_buckets = SYMTAB_INITIAL_HASH_BUCKETS;
  put_elf_str(s->link, "");
  section_ptr_add(s, sizeof(ElfW(Sym)));
  ptr = section_ptr_add(s->hash, (2 + nb_buckets + 1) * sizeof(int));
  ptr[0] = nb_buckets;
  ptr[1] = 1;
  memset(ptr + 2, 0, (nb_buckets + 1) * sizeof(int));
}

ST_FUNC Section *new_symtab(TCCState *s1, const char *symtab_name, int sh_type, int sh_flags, const char *strtab_name,
                            const char *hash_name, int hash_sh_flags)
{
  Section *symtab, *strtab, *hash;
  symtab = new_section(s1, symtab_name, sh_type, sh_flags);
  symtab->sh_entsize = sizeof(ElfW(Sym));
  strtab = new_section(s1, strtab_name, SHT_STRTAB, sh_flags);
  symtab->link = strtab;
  hash = new_section(s1, hash_name, SHT_HASH, hash_sh_flags);
  hash->sh_entsize = sizeof(int);
  symtab->hash = hash;
  hash->link = symtab;
  init_symtab(symtab);
  return symtab;
}

/* realloc section and set its content to zero */
ST_FUNC void section_realloc(Section *sec, unsigned long new_size)
{
  unsigned long size;
  unsigned char *data;

  size = sec->data_allocated;
  if (size == 0)
  {
    /* First allocation: round up to power of 2 with minimum 256 bytes
       to reduce future reallocations */
    size = 256;
    while (size < new_size)
      size = size * 2;
  }
  else
  {
    while (size < new_size)
      size = size * 2;
  }
  data = tcc_realloc(sec->data, size);
  memset(data + sec->data_allocated, 0, size - sec->data_allocated);
  sec->data = data;
  sec->data_allocated = size;
}

/* reserve at least 'size' bytes aligned per 'align' in section
   'sec' from current offset, and return the aligned offset */
ST_FUNC size_t section_add(Section *sec, addr_t size, int align)
{
  size_t offset, offset1;

  offset = (sec->data_offset + align - 1) & -align;
  offset1 = offset + size;
  if (sec->sh_type != SHT_NOBITS && offset1 > sec->data_allocated)
    section_realloc(sec, offset1);
  sec->data_offset = offset1;
  if (align > sec->sh_addralign)
    sec->sh_addralign = align;
  return offset;
}

/* reserve at least 'size' bytes in section 'sec' from
   sec->data_offset. */
ST_FUNC void *section_ptr_add(Section *sec, addr_t size)
{
  size_t offset = section_add(sec, size, 1);
  return sec->data + offset;
}

/* Pre-allocate section capacity without changing data_offset.
   Use this when you know the total size needed to avoid multiple reallocations. */
ST_FUNC void section_prealloc(Section *sec, unsigned long size)
{
  unsigned long needed = sec->data_offset + size;
  if (needed > sec->data_allocated)
    section_realloc(sec, needed);
}

#ifndef ELF_OBJ_ONLY
/* reserve at least 'size' bytes from section start */
void section_reserve(Section *sec, unsigned long size)
{
  if (size > sec->data_allocated)
    section_realloc(sec, size);
  if (size > sec->data_offset)
    sec->data_offset = size;
}
#endif
Section *have_section(TCCState *s1, const char *name)
{
  Section *sec;
  int i;
  for (i = 1; i < s1->nb_sections; i++)
  {
    sec = s1->sections[i];
    if (!strcmp(name, sec->name))
      return sec;
  }
  return NULL;
}

/* return a reference to a section, and create it if it does not
   exists */
ST_FUNC Section *find_section(TCCState *s1, const char *name)
{
  Section *sec = have_section(s1, name);
  if (sec)
    return sec;
  /* sections are created as PROGBITS */
  return new_section(s1, name, SHT_PROGBITS, SHF_ALLOC);
}

ST_FUNC int put_elf_str(Section *s, const char *sym)
{
  int offset, len;
  char *ptr;

  if (!sym)
    sym = "";
  len = strlen(sym) + 1;
  offset = s->data_offset;
  ptr = section_ptr_add(s, len);
  memmove(ptr, sym, len);
  return offset;
}

/* elf symbol hashing function */
ElfW(Word) elf_hash(const unsigned char *name)
{
  ElfW(Word) h = 0, g;

  while (*name)
  {
    h = (h << 4) + *name++;
    g = h & 0xf0000000;
    if (g)
      h ^= g >> 24;
    h &= ~g;
  }
  return h;
}

/* rebuild hash table of section s */
/* NOTE: we do factorize the hash table code to go faster */
static void rebuild_hash(Section *s, unsigned int nb_buckets)
{
  ElfW(Sym) * sym;
  int *ptr, *hash, nb_syms, sym_index, h;
  unsigned char *strtab;
  unsigned int full_hash;
  unsigned int *hcache;

  strtab = s->link->data;
  nb_syms = s->data_offset / sizeof(ElfW(Sym));

  if (!nb_buckets)
    nb_buckets = ((int *)s->hash->data)[0];

  s->hash->data_offset = 0;
  ptr = section_ptr_add(s->hash, (2 + nb_buckets + nb_syms) * sizeof(int));
  ptr[0] = nb_buckets;
  ptr[1] = nb_syms;
  ptr += 2;
  hash = ptr;
  memset(hash, 0, (nb_buckets + 1) * sizeof(int));
  ptr += nb_buckets + 1;

  /* Rebuild hash value cache */
  s->hash->hash_val_cache = tcc_realloc(s->hash->hash_val_cache, nb_syms * sizeof(unsigned int));
  s->hash->hash_val_alloc = nb_syms;
  hcache = s->hash->hash_val_cache;
  hcache[0] = 0;

  sym = (ElfW(Sym) *)s->data + 1;
  for (sym_index = 1; sym_index < nb_syms; sym_index++)
  {
    if (ELFW(ST_BIND)(sym->st_info) != STB_LOCAL)
    {
      full_hash = elf_hash(strtab + sym->st_name);
      h = full_hash % nb_buckets;
      *ptr = hash[h];
      hash[h] = sym_index;
      hcache[sym_index] = full_hash;
    }
    else
    {
      *ptr = 0;
      hcache[sym_index] = 0;
    }
    ptr++;
    sym++;
  }
}

/* return the symbol number */
ST_FUNC int put_elf_sym(Section *s, addr_t value, unsigned long size, int info, int other, int shndx, const char *name)
{
  int name_offset, sym_index;
  int nbuckets, h;
  ElfW(Sym) * sym;
  Section *hs;

  /* Validate name pointer - catch garbage early.
     Accept printable ASCII and valid UTF-8 lead bytes (0xC2-0xF4).
     Reject control characters and bare continuation bytes. */
  if (name && name[0])
  {
    unsigned char first = (unsigned char)name[0];
    if (first < 0x20 || (first > 0x7e && first < 0xc2) || first > 0xf4)
    {
      name = NULL;
    }
  }

  sym = section_ptr_add(s, sizeof(ElfW(Sym)));
  if (name && name[0])
    name_offset = put_elf_str(s->link, name);
  else
    name_offset = 0;
  /* XXX: endianness */
  sym->st_name = name_offset;
  sym->st_value = value;
  sym->st_size = size;
  sym->st_info = info;
  sym->st_other = other;
  sym->st_shndx = shndx;
  sym_index = sym - (ElfW(Sym) *)s->data;
  hs = s->hash;
  if (hs)
  {
    int *ptr, *base;
    ptr = section_ptr_add(hs, sizeof(int));
    base = (int *)hs->data;
    /* only add global or weak symbols. */
    if (ELFW(ST_BIND)(info) != STB_LOCAL)
    {
      /* add another hashing entry */
      unsigned int full_hash;
      nbuckets = base[0];
      full_hash = elf_hash((unsigned char *)s->link->data + name_offset);
      h = full_hash % nbuckets;
      *ptr = base[2 + h];
      base[2 + h] = sym_index;
      base[1]++;
      /* we resize the hash table */
      hs->nb_hashed_syms++;
      if (hs->nb_hashed_syms > 2 * nbuckets)
      {
        rebuild_hash(s, 2 * nbuckets);
      }
      else
      {
        /* Store cached hash value (rebuild_hash handles this when triggered) */
        if (sym_index >= hs->hash_val_alloc)
        {
          int new_alloc = (sym_index + 16) & ~15;
          hs->hash_val_cache = tcc_realloc(hs->hash_val_cache, new_alloc * sizeof(unsigned int));
          memset(hs->hash_val_cache + hs->hash_val_alloc, 0, (new_alloc - hs->hash_val_alloc) * sizeof(unsigned int));
          hs->hash_val_alloc = new_alloc;
        }
        hs->hash_val_cache[sym_index] = full_hash;
      }
    }
    else
    {
      *ptr = 0;
      base[1]++;
    }
  }

  /* Track non-local UNDEF symbols in symtab for fast alacarte lookup */
  if (shndx == SHN_UNDEF && ELFW(ST_BIND)(info) != STB_LOCAL)
  {
    TCCState *ts = s->s1;
    if (s == ts->symtab)
    {
      if (ts->nb_undef_syms >= ts->undef_sym_alloc)
      {
        ts->undef_sym_alloc = ts->undef_sym_alloc ? ts->undef_sym_alloc * 2 : 64;
        ts->undef_sym_list = tcc_realloc(ts->undef_sym_list, ts->undef_sym_alloc * sizeof(int));
      }
      ts->undef_sym_list[ts->nb_undef_syms++] = sym_index;
    }
  }

  return sym_index;
}

static int find_elf_sym_with_hash(Section *s, const char *name, unsigned int full_hash)
{
  ElfW(Sym) * sym;
  Section *hs;
  int nbuckets, sym_index;
  const char *name1;
  unsigned int *hcache;

  hs = s->hash;
  if (!hs)
    return 0;
  nbuckets = ((int *)hs->data)[0];
  sym_index = ((int *)hs->data)[2 + full_hash % nbuckets];
  hcache = hs->hash_val_cache;

  while (sym_index != 0)
  {
    /* Compare cached hash value first to avoid expensive strcmp */
    if (!hcache || hcache[sym_index] == full_hash)
    {
      sym = &((ElfW(Sym) *)s->data)[sym_index];
      name1 = (char *)s->link->data + sym->st_name;
      if (!strcmp(name, name1))
        return sym_index;
    }
    sym_index = ((int *)hs->data)[2 + nbuckets + sym_index];
  }
  return 0;
}

ST_FUNC int find_elf_sym(Section *s, const char *name)
{
  return find_elf_sym_with_hash(s, name, elf_hash((unsigned char *)name));
}

/* Resolve `name` against loaded libraries: first the already-interned
   dynsymtab_section, then (on miss) the loaded YAFF libraries' on-disk hash
   tables (tcc_yaff_resolve), which interns a hit into dynsymtab_section.
   Returns the dynsymtab index, or 0 if no loaded library provides it. */
ST_FUNC int tcc_dynsym_find(TCCState *s1, const char *name)
{
  int idx = find_elf_sym(s1->dynsymtab_section, name);
  if (idx)
    return idx;
  return tcc_yaff_resolve(s1, name);
}

/* return elf symbol value, signal error if 'err' is nonzero, decorate
   name if FORC */
ST_FUNC addr_t get_sym_addr(TCCState *s1, const char *name, int err, int forc)
{
  int sym_index;
  ElfW(Sym) * sym;
  char buf[256];
  if (forc &&
      s1->leading_underscore
#ifdef TCC_TARGET_PE
      /* win32-32bit stdcall symbols always have _ already */
      && !strchr(name, '@')
#endif
  )
  {
    buf[0] = '_';
    pstrcpy(buf + 1, sizeof(buf) - 1, name);
    name = buf;
  }
  sym_index = find_elf_sym(s1->symtab, name);
  sym = &((ElfW(Sym) *)s1->symtab->data)[sym_index];
  if (!sym_index || sym->st_shndx == SHN_UNDEF)
  {
    if (err)
      tcc_error_noabort("%s not defined", name);
    return (addr_t)-1;
  }
  return sym->st_value;
}

/* return elf symbol value */
LIBTCCAPI void *tcc_get_symbol(TCCState *s, const char *name)
{
  addr_t addr = get_sym_addr(s, name, 0, 1);
  return addr == -1 ? NULL : (void *)(uintptr_t)addr;
}

/* list elf symbol names and values */
ST_FUNC void list_elf_symbols(TCCState *s, void *ctx, void (*symbol_cb)(void *ctx, const char *name, const void *val))
{
  ElfW(Sym) * sym;
  Section *symtab;
  int sym_index, end_sym;
  const char *name;
  unsigned char sym_vis, sym_bind;

  symtab = s->symtab;
  end_sym = symtab->data_offset / sizeof(ElfSym);
  for (sym_index = 0; sym_index < end_sym; ++sym_index)
  {
    sym = &((ElfW(Sym) *)symtab->data)[sym_index];
    if (sym->st_value)
    {
      name = (char *)symtab->link->data + sym->st_name;
      sym_bind = ELFW(ST_BIND)(sym->st_info);
      sym_vis = ELFW(ST_VISIBILITY)(sym->st_other);
      if (sym_bind == STB_GLOBAL && sym_vis == STV_DEFAULT)
        symbol_cb(ctx, name, (void *)(uintptr_t)sym->st_value);
    }
  }
}

/* list elf symbol names and values */
LIBTCCAPI void tcc_list_symbols(TCCState *s, void *ctx, void (*symbol_cb)(void *ctx, const char *name, const void *val))
{
  list_elf_symbols(s, ctx, symbol_cb);
}

#ifndef ELF_OBJ_ONLY
void version_add(TCCState *s1)
{
  int i;
  ElfW(Sym) * sym;
  ElfW(Verneed) *vn = NULL;
  Section *symtab;
  int sym_index, end_sym, nb_versions = 2, nb_entries = 0;
  ElfW(Half) * versym;
  const char *name;

  if (0 == nb_sym_versions)
    return;
  versym_section = new_section(s1, ".gnu.version", SHT_GNU_versym, SHF_ALLOC);
  versym_section->sh_entsize = sizeof(ElfW(Half));
  versym_section->link = s1->dynsym;

  /* add needed symbols */
  symtab = s1->dynsym;
  end_sym = symtab->data_offset / sizeof(ElfSym);
  versym = section_ptr_add(versym_section, end_sym * sizeof(ElfW(Half)));
  for (sym_index = 1; sym_index < end_sym; ++sym_index)
  {
    int dllindex, verndx;
    sym = &((ElfW(Sym) *)symtab->data)[sym_index];
    name = (char *)symtab->link->data + sym->st_name;
    dllindex = tcc_dynsym_find(s1, name);
    verndx = (dllindex && dllindex < nb_sym_to_version) ? sym_to_version[dllindex] : -1;
    if (verndx >= 0
        /* XXX: on android, clang refuses to link with a libtcc.so made by tcc
           when defined symbols have a version > 1 or when the version is '0'.
           Whereas version '1' for example for 'signal' in an exe defeats
           bcheck's signal_redir. */
        && (sym->st_shndx == SHN_UNDEF || (s1->output_type & TCC_OUTPUT_EXE)))
    {
      if (!sym_versions[verndx].out_index)
        sym_versions[verndx].out_index = nb_versions++;
      versym[sym_index] = sym_versions[verndx].out_index;
    }
    else
    {
      versym[sym_index] = 1; /* (*global*) */
    }
    // printf("SYM %d %s\n", versym[sym_index], name);
  }
  /* generate verneed section, but not when it will be empty.  Some
     dynamic linkers look at their contents even when DTVERNEEDNUM and
     section size is zero.  */
  if (nb_versions > 2)
  {
    verneed_section = new_section(s1, ".gnu.version_r", SHT_GNU_verneed, SHF_ALLOC);
    verneed_section->link = s1->dynsym->link;
    for (i = nb_sym_versions; i-- > 0;)
    {
      struct sym_version *sv = &sym_versions[i];
      int n_same_libs = 0, prev;
      size_t vnofs;
      ElfW(Vernaux) *vna = 0;
      if (sv->out_index < 1)
        continue;

      /* make sure that a DT_NEEDED tag is put */
      /* abitest-tcc fails on older i386-linux with "ld-linux.so.2" DT_NEEDED
         ret_int_test... Inconsistency detected by ld.so: dl-minimal.c: 148:
         realloc: Assertion `ptr == alloc_last_block' failed! */
      if (strcmp(sv->lib, "ld-linux.so.2"))
        tcc_add_dllref(s1, sv->lib, 0);

      vnofs = section_add(verneed_section, sizeof(*vn), 1);
      vn = (ElfW(Verneed) *)(verneed_section->data + vnofs);
      vn->vn_version = 1;
      vn->vn_file = put_elf_str(verneed_section->link, sv->lib);
      vn->vn_aux = sizeof(*vn);
      do
      {
        prev = sv->prev_same_lib;
        if (sv->out_index > 0)
        {
          vna = section_ptr_add(verneed_section, sizeof(*vna));
          vna->vna_hash = elf_hash((const unsigned char *)sv->version);
          vna->vna_flags = 0;
          vna->vna_other = sv->out_index;
          sv->out_index = -2;
          vna->vna_name = put_elf_str(verneed_section->link, sv->version);
          vna->vna_next = sizeof(*vna);
          // printf("LIB %d %s %s\n", vna->vna_other, sv->lib,
          // verneed_section->link->data + vna->vna_name);
          n_same_libs++;
        }
        if (prev >= 0)
          sv = &sym_versions[prev];
      } while (prev >= 0);
      vna->vna_next = 0;
      vn = (ElfW(Verneed) *)(verneed_section->data + vnofs);
      vn->vn_cnt = n_same_libs;
      vn->vn_next = sizeof(*vn) + n_same_libs * sizeof(*vna);
      nb_entries++;
    }
    if (vn)
      vn->vn_next = 0;
    verneed_section->sh_info = nb_entries;
  }
  dt_verneednum = nb_entries;
}
#endif /* ndef ELF_OBJ_ONLY */

/* add an elf symbol : check if it is already defined and patch
   it. Return symbol index. NOTE that sh_num can be SHN_UNDEF. */
ST_FUNC int set_elf_sym(Section *s, addr_t value, unsigned long size, int info, int other, int shndx, const char *name)
{
  TCCState *s1 = s->s1;
  ElfW(Sym) * esym;
  int sym_bind, sym_index, sym_type, esym_bind;
  unsigned char sym_vis, esym_vis, new_vis;

  sym_bind = ELFW(ST_BIND)(info);
  sym_type = ELFW(ST_TYPE)(info);
  sym_vis = ELFW(ST_VISIBILITY)(other);

  if (sym_bind != STB_LOCAL)
  {
    /* we search global or weak symbols */
    sym_index = find_elf_sym_with_hash(s, name, elf_hash((const unsigned char *)name));
    if (!sym_index)
      goto do_def;
    esym = &((ElfW(Sym) *)s->data)[sym_index];
    if (esym->st_value == value && esym->st_size == size && esym->st_info == info && esym->st_other == other &&
        esym->st_shndx == shndx)
      return sym_index;
    if (esym->st_shndx != SHN_UNDEF)
    {
      esym_bind = ELFW(ST_BIND)(esym->st_info);
      /* propagate the most constraining visibility */
      /* STV_DEFAULT(0)<STV_PROTECTED(3)<STV_HIDDEN(2)<STV_INTERNAL(1) */
      esym_vis = ELFW(ST_VISIBILITY)(esym->st_other);
      if (esym_vis == STV_DEFAULT)
      {
        new_vis = sym_vis;
      }
      else if (sym_vis == STV_DEFAULT)
      {
        new_vis = esym_vis;
      }
      else
      {
        new_vis = (esym_vis < sym_vis) ? esym_vis : sym_vis;
      }
      esym->st_other = (esym->st_other & ~ELFW(ST_VISIBILITY)(-1)) | new_vis;
      if (shndx == SHN_UNDEF)
      {
        /* ignore adding of undefined symbol if the
           corresponding symbol is already defined */
      }
      else if (sym_bind == STB_GLOBAL && esym_bind == STB_WEAK)
      {
        /* global overrides weak, so patch */
        goto do_patch;
      }
      else if (sym_bind == STB_WEAK && esym_bind == STB_GLOBAL)
      {
        /* weak is ignored if already global */
      }
      else if (sym_bind == STB_WEAK && esym_bind == STB_WEAK)
      {
        /* keep first-found weak definition, ignore subsequents */
      }
      else if (sym_vis == STV_HIDDEN || sym_vis == STV_INTERNAL)
      {
        /* ignore hidden symbols after */
      }
      else if ((esym->st_shndx == SHN_COMMON || esym->st_shndx == bss_section->sh_num) &&
               (shndx < SHN_LORESERVE && shndx != bss_section->sh_num))
      {
        /* data symbol gets precedence over common/bss */
        goto do_patch;
      }
      else if (shndx == SHN_COMMON || shndx == bss_section->sh_num)
      {
        /* data symbol keeps precedence over common/bss */
      }
      else if (s->sh_flags & SHF_DYNSYM)
      {
        /* we accept that two DLL define the same symbol */
      }
      else if (esym->st_other & ST_ASM_SET)
      {
        /* If the existing symbol came from an asm .set
           we can override.  */
        goto do_patch;
      }
      else
      {
#if 0
                printf("new_bind=%x new_shndx=%x new_vis=%x old_bind=%x old_shndx=%x old_vis=%x\n",
                       sym_bind, shndx, new_vis, esym_bind, esym->st_shndx, esym_vis);
#endif
        tcc_error_noabort("'%s' defined twice", name);
      }
    }
    else
    {
      esym->st_other = other;
    do_patch:
      esym->st_info = ELFW(ST_INFO)(sym_bind, sym_type);
      esym->st_shndx = shndx;
      esym->st_value = value;
      esym->st_size = size;
    }
  }
  else
  {
  do_def:
    sym_index = put_elf_sym(s, value, size, ELFW(ST_INFO)(sym_bind, sym_type), other, shndx, name);
    /* Signal the --start-group rescan loop only when a genuinely NEW
       undefined symbol appears.  Resolving an existing undef (do_patch)
       does not create new work for other archives. */
    if (shndx == SHN_UNDEF)
      s1->new_undef_sym = 1;
  }
  return sym_index;
}

/* put relocation */
ST_FUNC void put_elf_reloca(Section *symtab, Section *s, unsigned long offset, int type, int symbol, addr_t addend)
{
  TCCState *s1 = s->s1;
  char buf[256];
  Section *sr;
  ElfW_Rel *rel;

  /* Validate symbol index.
     Skip the check for R_RELATIVE relocations to the GOT: put_got_entry()
     smuggles a symtab_section index through a relocation nominally linked
     to s1->dynsym, so the index is valid for symtab_section, not for the
     symtab passed here.  See the "Hack alarm" comment in put_got_entry. */
  if (type != R_RELATIVE)
  {
    int num_syms = symtab->data_offset / sizeof(ElfW(Sym));
    if (symbol < 0 || symbol >= num_syms)
    {
      return; /* Skip invalid symbol index */
    }
  }

  sr = s->reloc;
  if (!sr)
  {
    /* if no relocation section, create it */
    snprintf(buf, sizeof(buf), REL_SECTION_FMT, s->name);
    /* if the symtab is allocated, then we consider the relocation
       are also */
    sr = new_section(s->s1, buf, SHT_RELX, symtab->sh_flags);
    sr->sh_entsize = sizeof(ElfW_Rel);
    sr->link = symtab;
    sr->sh_info = s->sh_num;
    s->reloc = sr;
  }
  rel = section_ptr_add(sr, sizeof(ElfW_Rel));
  rel->r_offset = offset;
  rel->r_info = ELFW(R_INFO)(symbol, type);
#if SHT_RELX == SHT_RELA
  rel->r_addend = addend;
#endif
  if (SHT_RELX != SHT_RELA && addend)
    tcc_error_noabort("non-zero addend on REL architecture");
}

ST_FUNC void put_elf_reloc(Section *symtab, Section *s, unsigned long offset, int type, int symbol)
{
  put_elf_reloca(symtab, s, offset, type, symbol, 0);
}

ST_FUNC struct sym_attr *get_sym_attr(TCCState *s1, int index, int alloc)
{
  int n;
  struct sym_attr *tab;

  if (index >= s1->nb_sym_attrs)
  {
    if (!alloc)
      return s1->sym_attrs;
    /* find immediately bigger power of 2 and reallocate array */
    n = 1;
    while (index >= n)
      n *= 2;
    tab = tcc_realloc(s1->sym_attrs, n * sizeof(*s1->sym_attrs));
    s1->sym_attrs = tab;
    memset(s1->sym_attrs + s1->nb_sym_attrs, 0, (n - s1->nb_sym_attrs) * sizeof(*s1->sym_attrs));
    s1->nb_sym_attrs = n;
  }
  return &s1->sym_attrs[index];
}
void update_relocs(TCCState *s1, Section *s, int *old_to_new_syms, int first_sym)
{
  int i, type, sym_index;
  Section *sr;
  ElfW_Rel *rel;

  for (i = 1; i < s1->nb_sections; i++)
  {
    sr = s1->sections[i];
    if (sr->sh_type == SHT_RELX && sr->link == s)
    {
      for_each_elem(sr, 0, rel, ElfW_Rel)
      {
        sym_index = ELFW(R_SYM)(rel->r_info);
        type = ELFW(R_TYPE)(rel->r_info);
        if ((sym_index -= first_sym) < 0)
          continue; /* zero sym_index in reloc (can happen with asm) */
        sym_index = old_to_new_syms[sym_index];
        rel->r_info = ELFW(R_INFO)(sym_index, type);
      }
    }
  }
}

/* In an ELF file symbol table, the local symbols must appear below
   the global and weak ones. Since TCC cannot sort it while generating
   the code, we must do it after. All the relocation tables are also
   modified to take into account the symbol table sorting */
ST_FUNC void tcc_elf_sort_syms(TCCState *s1, Section *s)
{
  int *old_to_new_syms;
  ElfW(Sym) * new_syms;
  int nb_syms, i;
  ElfW(Sym) * p, *q;

  nb_syms = s->data_offset / sizeof(ElfW(Sym));
  new_syms = tcc_malloc(nb_syms * sizeof(ElfW(Sym)));
  old_to_new_syms = tcc_malloc(nb_syms * sizeof(int));

  /* first pass for local symbols */
  p = (ElfW(Sym) *)s->data;
  q = new_syms;
  for (i = 0; i < nb_syms; i++)
  {
    if (ELFW(ST_BIND)(p->st_info) == STB_LOCAL)
    {
      old_to_new_syms[i] = q - new_syms;
      *q++ = *p;
    }
    p++;
  }
  /* save the number of local symbols in section header */
  if (s->sh_size) /* this 'if' makes IDA happy */
    s->sh_info = q - new_syms;

  /* then second pass for non local symbols */
  p = (ElfW(Sym) *)s->data;
  for (i = 0; i < nb_syms; i++)
  {
    if (ELFW(ST_BIND)(p->st_info) != STB_LOCAL)
    {
      old_to_new_syms[i] = q - new_syms;
      *q++ = *p;
    }
    p++;
  }

  /* we copy the new symbols to the old */
  memcpy(s->data, new_syms, nb_syms * sizeof(ElfW(Sym)));
  tcc_free(new_syms);

  update_relocs(s1, s, old_to_new_syms, 0);
  tcc_free(old_to_new_syms);
}

#ifndef ELF_OBJ_ONLY
/* See: https://flapenguin.me/elf-dt-gnu-hash */
#define ELFCLASS_BITS (PTR_SIZE * 8)
Section *create_gnu_hash(TCCState *s1)
{
  int nb_syms, i, ndef, nbuckets, symoffset, bloom_size, bloom_shift;
  ElfW(Sym) * p;
  Section *gnu_hash;
  Section *dynsym = s1->dynsym;
  Elf32_Word *ptr;

  gnu_hash = new_section(s1, ".gnu.hash", SHT_GNU_HASH, SHF_ALLOC);
  gnu_hash->link = dynsym->hash->link;

  nb_syms = dynsym->data_offset / sizeof(ElfW(Sym));

  /* count def symbols */
  ndef = 0;
  p = (ElfW(Sym) *)dynsym->data;
  for (i = 0; i < nb_syms; i++, p++)
    ndef += p->st_shndx != SHN_UNDEF;

  /* calculate gnu hash sizes and fill header */
  nbuckets = ndef / 4 + 1;
  symoffset = nb_syms - ndef;
  bloom_shift = PTR_SIZE == 8 ? 6 : 5;
  bloom_size = 1; /* must be power of two */
  while (ndef >= bloom_size * (1 << (bloom_shift - 3)))
    bloom_size *= 2;
  ptr = section_ptr_add(gnu_hash, 4 * 4 + PTR_SIZE * bloom_size + nbuckets * 4 + ndef * 4);
  ptr[0] = nbuckets;
  ptr[1] = symoffset;
  ptr[2] = bloom_size;
  ptr[3] = bloom_shift;
  return gnu_hash;
}

static Elf32_Word elf_gnu_hash(const unsigned char *name)
{
  Elf32_Word h = 5381;
  unsigned char c;

  while ((c = *name++))
    h = h * 33 + c;
  return h;
}
void update_gnu_hash(TCCState *s1, Section *gnu_hash)
{
  int *old_to_new_syms;
  ElfW(Sym) * new_syms;
  int nb_syms, i, nbuckets, bloom_size, bloom_shift;
  ElfW(Sym) * p, *q;
  Section *vs;
  Section *dynsym = s1->dynsym;
  Elf32_Word *ptr, *buckets, *chain, *hash;
  unsigned int *nextbuck;
  addr_t *bloom;
  unsigned char *strtab;
  struct
  {
    int first, last;
  } *buck;

  strtab = dynsym->link->data;
  nb_syms = dynsym->data_offset / sizeof(ElfW(Sym));
  new_syms = tcc_malloc(nb_syms * sizeof(ElfW(Sym)));
  old_to_new_syms = tcc_malloc(nb_syms * sizeof(int));
  hash = tcc_malloc(nb_syms * sizeof(Elf32_Word));
  nextbuck = tcc_malloc(nb_syms * sizeof(int));

  /* calculate hashes and copy undefs */
  p = (ElfW(Sym) *)dynsym->data;
  q = new_syms;
  for (i = 0; i < nb_syms; i++, p++)
  {
    if (p->st_shndx == SHN_UNDEF)
    {
      old_to_new_syms[i] = q - new_syms;
      *q++ = *p;
    }
    else
      hash[i] = elf_gnu_hash(strtab + p->st_name);
  }

  ptr = (Elf32_Word *)gnu_hash->data;
  nbuckets = ptr[0];
  bloom_size = ptr[2];
  bloom_shift = ptr[3];
  bloom = (addr_t *)(void *)&ptr[4];
  buckets = (Elf32_Word *)(void *)&bloom[bloom_size];
  chain = &buckets[nbuckets];
  buck = tcc_malloc(nbuckets * sizeof(*buck));

  if (gnu_hash->data_offset != 4 * 4 + PTR_SIZE * bloom_size + nbuckets * 4 + (nb_syms - (q - new_syms)) * 4)
    tcc_error_noabort("gnu_hash size incorrect");

  /* find buckets */
  for (i = 0; i < nbuckets; i++)
    buck[i].first = -1;

  p = (ElfW(Sym) *)dynsym->data;
  for (i = 0; i < nb_syms; i++, p++)
    if (p->st_shndx != SHN_UNDEF)
    {
      int bucket = hash[i] % nbuckets;

      if (buck[bucket].first == -1)
        buck[bucket].first = buck[bucket].last = i;
      else
      {
        nextbuck[buck[bucket].last] = i;
        buck[bucket].last = i;
      }
    }

  /* fill buckets/chains/bloom and sort symbols */
  p = (ElfW(Sym) *)dynsym->data;
  for (i = 0; i < nbuckets; i++)
  {
    int cur = buck[i].first;

    if (cur != -1)
    {
      buckets[i] = q - new_syms;
      for (;;)
      {
        old_to_new_syms[cur] = q - new_syms;
        *q++ = p[cur];
        *chain++ = hash[cur] & ~1;
        bloom[(hash[cur] / ELFCLASS_BITS) % bloom_size] |=
            (addr_t)1 << (hash[cur] % ELFCLASS_BITS) | (addr_t)1 << ((hash[cur] >> bloom_shift) % ELFCLASS_BITS);
        if (cur == buck[i].last)
          break;
        cur = nextbuck[cur];
      }
      chain[-1] |= 1;
    }
  }

  memcpy(dynsym->data, new_syms, nb_syms * sizeof(ElfW(Sym)));
  tcc_free(new_syms);
  tcc_free(hash);
  tcc_free(buck);
  tcc_free(nextbuck);

  update_relocs(s1, dynsym, old_to_new_syms, 0);

  /* modify the versions */
  vs = versym_section;
  if (vs)
  {
    ElfW(Half) * newver, *versym = (ElfW(Half) *)vs->data;

    if (1 /*versym*/)
    {
      newver = tcc_malloc(nb_syms * sizeof(*newver));
      for (i = 0; i < nb_syms; i++)
        newver[old_to_new_syms[i]] = versym[i];
      memcpy(vs->data, newver, nb_syms * sizeof(*newver));
      tcc_free(newver);
    }
  }

  tcc_free(old_to_new_syms);

  /* rebuild hash */
  ptr = (Elf32_Word *)dynsym->hash->data;
  rebuild_hash(dynsym, ptr[0]);
}
#endif /* ELF_OBJ_ONLY */
