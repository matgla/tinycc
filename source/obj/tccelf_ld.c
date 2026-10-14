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

/* ELF: loading shared libraries and linker scripts, and linker-script
 * symbol assignment. */

#include "tccelf_priv.h"

#ifndef ELF_OBJ_ONLY
/* Set LV[I] to the global index of sym-version (LIB,VERSION).  Maybe resizes
   LV, maybe create a new entry for (LIB,VERSION).  */
static void set_ver_to_ver(TCCState *s1, int *n, int **lv, int i, char *lib, char *version)
{
  while (i >= *n)
  {
    *lv = tcc_realloc(*lv, (*n + 1) * sizeof(**lv));
    (*lv)[(*n)++] = -1;
  }
  if ((*lv)[i] == -1)
  {
    int v, prev_same_lib = -1;
    for (v = 0; v < nb_sym_versions; v++)
    {
      if (strcmp(sym_versions[v].lib, lib))
        continue;
      prev_same_lib = v;
      if (!strcmp(sym_versions[v].version, version))
        break;
    }
    if (v == nb_sym_versions)
    {
      sym_versions = tcc_realloc(sym_versions, (v + 1) * sizeof(*sym_versions));
      sym_versions[v].lib = tcc_strdup(lib);
      sym_versions[v].version = tcc_strdup(version);
      sym_versions[v].out_index = 0;
      sym_versions[v].prev_same_lib = prev_same_lib;
      nb_sym_versions++;
    }
    (*lv)[i] = v;
  }
}

/* Associates symbol SYM_INDEX (in dynsymtab) with sym-version index
   VERNDX.  */
static void set_sym_version(TCCState *s1, int sym_index, int verndx)
{
  if (sym_index >= nb_sym_to_version)
  {
    int newelems = sym_index ? sym_index * 2 : 1;
    sym_to_version = tcc_realloc(sym_to_version, newelems * sizeof(*sym_to_version));
    memset(sym_to_version + nb_sym_to_version, -1, (newelems - nb_sym_to_version) * sizeof(*sym_to_version));
    nb_sym_to_version = newelems;
  }
  if (sym_to_version[sym_index] < 0)
    sym_to_version[sym_index] = verndx;
}

struct versym_info
{
  int nb_versyms;
  ElfW(Verdef) * verdef;
  ElfW(Verneed) * verneed;
  ElfW(Half) * versym;
  int nb_local_ver, *local_ver;
};

static void store_version(TCCState *s1, struct versym_info *v, char *dynstr)
{
  char *lib, *version;
  uint32_t next;
  int i;

#define DEBUG_VERSION 0

  if (v->versym && v->verdef)
  {
    ElfW(Verdef) *vdef = v->verdef;
    lib = NULL;
    do
    {
      ElfW(Verdaux) *verdaux = (ElfW(Verdaux) *)(((char *)vdef) + vdef->vd_aux);

#if DEBUG_VERSION
      printf("verdef: version:%u flags:%u index:%u, hash:%u\n", vdef->vd_version, vdef->vd_flags, vdef->vd_ndx,
             vdef->vd_hash);
#endif
      if (vdef->vd_cnt)
      {
        version = dynstr + verdaux->vda_name;

        if (lib == NULL)
          lib = version;
        else
          set_ver_to_ver(s1, &v->nb_local_ver, &v->local_ver, vdef->vd_ndx, lib, version);
#if DEBUG_VERSION
        printf("  verdaux(%u): %s\n", vdef->vd_ndx, version);
#endif
      }
      next = vdef->vd_next;
      vdef = (ElfW(Verdef) *)(((char *)vdef) + next);
    } while (next);
  }
  if (v->versym && v->verneed)
  {
    ElfW(Verneed) *vneed = v->verneed;
    do
    {
      ElfW(Vernaux) *vernaux = (ElfW(Vernaux) *)(((char *)vneed) + vneed->vn_aux);

      lib = dynstr + vneed->vn_file;
#if DEBUG_VERSION
      printf("verneed: %u %s\n", vneed->vn_version, lib);
#endif
      for (i = 0; i < vneed->vn_cnt; i++)
      {
        if ((vernaux->vna_other & 0x8000) == 0)
        { /* hidden */
          version = dynstr + vernaux->vna_name;
          set_ver_to_ver(s1, &v->nb_local_ver, &v->local_ver, vernaux->vna_other, lib, version);
#if DEBUG_VERSION
          printf("  vernaux(%u): %u %u %s\n", vernaux->vna_other, vernaux->vna_hash, vernaux->vna_flags, version);
#endif
        }
        vernaux = (ElfW(Vernaux) *)(((char *)vernaux) + vernaux->vna_next);
      }
      next = vneed->vn_next;
      vneed = (ElfW(Verneed) *)(((char *)vneed) + next);
    } while (next);
  }

#if DEBUG_VERSION
  for (i = 0; i < v->nb_local_ver; i++)
  {
    if (v->local_ver[i] > 0)
    {
      printf("%d: lib: %s, version %s\n", i, sym_versions[v->local_ver[i]].lib, sym_versions[v->local_ver[i]].version);
    }
  }
#endif
}
/* load a library / DLL
   'level = 0' means that the DLL is referenced by the user
   (so it should be added as DT_NEEDED in the generated ELF file) */
ST_FUNC int tcc_load_dll(TCCState *s1, int fd, const char *filename, int level)
{
  ElfW(Ehdr) ehdr;
  ElfW(Shdr) * shdr, *sh, *sh1;
  int i, nb_syms, nb_dts, sym_bind, ret = -1;
  ElfW(Sym) * sym, *dynsym;
  ElfW(Dyn) * dt, *dynamic;

  char *dynstr;
  int sym_index;
  const char *name, *soname;
  struct versym_info v;
  unsigned dll_start = 0;

  if (s1->do_bench)
    dll_start = tcc_getclock_us();

  full_read(fd, &ehdr, sizeof(ehdr));

  /* test CPU specific stuff */
  if (ehdr.e_ident[5] != ELFDATA2LSB || ehdr.e_machine != EM_TCC_TARGET)
  {
    return tcc_error_noabort("bad architecture");
  }

  /* read sections */
  shdr = load_data(fd, ehdr.e_shoff, sizeof(ElfW(Shdr)) * ehdr.e_shnum);

  /* load dynamic section and dynamic symbols */
  nb_syms = 0;
  nb_dts = 0;
  dynamic = NULL;
  dynsym = NULL; /* avoid warning */
  dynstr = NULL; /* avoid warning */
  memset(&v, 0, sizeof v);

  for (i = 0, sh = shdr; i < ehdr.e_shnum; i++, sh++)
  {
    switch (sh->sh_type)
    {
    case SHT_DYNAMIC:
      nb_dts = sh->sh_size / sizeof(ElfW(Dyn));
      dynamic = load_data(fd, sh->sh_offset, sh->sh_size);
      break;
    case SHT_DYNSYM:
      nb_syms = sh->sh_size / sizeof(ElfW(Sym));
      dynsym = load_data(fd, sh->sh_offset, sh->sh_size);
      sh1 = &shdr[sh->sh_link];
      dynstr = load_data(fd, sh1->sh_offset, sh1->sh_size);
      break;
    case SHT_GNU_verdef:
      v.verdef = load_data(fd, sh->sh_offset, sh->sh_size);
      break;
    case SHT_GNU_verneed:
      v.verneed = load_data(fd, sh->sh_offset, sh->sh_size);
      break;
    case SHT_GNU_versym:
      v.nb_versyms = sh->sh_size / sizeof(ElfW(Half));
      v.versym = load_data(fd, sh->sh_offset, sh->sh_size);
      break;
    default:
      break;
    }
  }

  if (!dynamic)
    goto the_end;

  /* compute the real library name */
  soname = tcc_basename(filename);
  for (i = 0, dt = dynamic; i < nb_dts; i++, dt++)
    if (dt->d_tag == DT_SONAME)
      soname = dynstr + dt->d_un.d_val;

  /* if the dll is already loaded, do not load it */
  if (tcc_add_dllref(s1, soname, level)->found)
    goto ret_success;

  if (v.nb_versyms != nb_syms)
    tcc_free(v.versym), v.versym = NULL;
  else
    store_version(s1, &v, dynstr);

  /* add dynamic symbols in dynsym_section */
  for (i = 1, sym = dynsym + 1; i < nb_syms; i++, sym++)
  {
    sym_bind = ELFW(ST_BIND)(sym->st_info);
    if (sym_bind == STB_LOCAL)
      continue;
    name = dynstr + sym->st_name;
    sym_index = set_elf_sym(s1->dynsymtab_section, sym->st_value, sym->st_size, sym->st_info, sym->st_other,
                            sym->st_shndx, name);
    if (v.versym)
    {
      ElfW(Half) vsym = v.versym[i];
      if ((vsym & 0x8000) == 0 && vsym > 0 && vsym < v.nb_local_ver)
        set_sym_version(s1, sym_index, v.local_ver[vsym]);
    }
  }

  /* do not load all referenced libraries
     (recursive loading can break linking of libraries) */
  /* following DT_NEEDED is needed for the dynamic loader (libdl.so),
     but it is no longer needed, when linking a library or a program.
     When tcc output mode is OUTPUT_MEM,
     tcc calls dlopen, which handles DT_NEEDED for us */

#if 0
    for(i = 0, dt = dynamic; i < nb_dts; i++, dt++)
        if (dt->d_tag == DT_RPATH)
            tcc_add_library_path(s1, dynstr + dt->d_un.d_val);

    /* load all referenced DLLs */
    for(i = 0, dt = dynamic; i < nb_dts; i++, dt++) {
        switch(dt->d_tag) {
        case DT_NEEDED:
            name = dynstr + dt->d_un.d_val;
            if (tcc_add_dllref(s1, name, -1))
                continue;
            if (tcc_add_dll(s1, name, AFF_REFERENCED_DLL) < 0) {
                ret = tcc_error_noabort("referenced dll '%s' not found", name);
                goto the_end;
            }
        }
    }
#endif

ret_success:
  ret = 0;
the_end:
  if (s1->do_bench)
  {
    unsigned elapsed = tcc_getclock_us() - dll_start;
    s1->bench_dll_load_time += elapsed;
    s1->bench_dll_load_count++;
    tcc_bench_log(s1, "load-dll", filename, elapsed);
  }
  tcc_free(dynstr);
  tcc_free(dynsym);
  tcc_free(dynamic);
  tcc_free(shdr);
  tcc_free(v.local_ver);
  tcc_free(v.verdef);
  tcc_free(v.verneed);
  tcc_free(v.versym);
  return ret;
}

#define LD_TOK_NAME 256
#define LD_TOK_EOF (-1)

static int ld_inp(TCCState *s1)
{
  char b;
  if (s1->cc != -1)
  {
    int c = s1->cc;
    s1->cc = -1;
    return c;
  }
  if (1 == read(s1->fd, &b, 1))
    return b;
  return CH_EOF;
}

/* return next ld script token */
static int ld_next(TCCState *s1, char *name, int name_size)
{
  int c, d, ch;
  char *q;

redo:
  ch = ld_inp(s1);
  switch (ch)
  {
  case ' ':
  case '\t':
  case '\f':
  case '\v':
  case '\r':
  case '\n':
    goto redo;
  case '/':
    ch = ld_inp(s1);
    if (ch == '*')
    { /* comment */
      for (d = 0;; d = ch)
      {
        ch = ld_inp(s1);
        if (ch == CH_EOF || (ch == '/' && d == '*'))
          break;
      }
      goto redo;
    }
    else
    {
      q = name;
      *q++ = '/';
      goto parse_name;
    }
    break;
  case '\\':
  /* case 'a' ... 'z': */
  case 'a':
  case 'b':
  case 'c':
  case 'd':
  case 'e':
  case 'f':
  case 'g':
  case 'h':
  case 'i':
  case 'j':
  case 'k':
  case 'l':
  case 'm':
  case 'n':
  case 'o':
  case 'p':
  case 'q':
  case 'r':
  case 's':
  case 't':
  case 'u':
  case 'v':
  case 'w':
  case 'x':
  case 'y':
  case 'z':
  /* case 'A' ... 'z': */
  case 'A':
  case 'B':
  case 'C':
  case 'D':
  case 'E':
  case 'F':
  case 'G':
  case 'H':
  case 'I':
  case 'J':
  case 'K':
  case 'L':
  case 'M':
  case 'N':
  case 'O':
  case 'P':
  case 'Q':
  case 'R':
  case 'S':
  case 'T':
  case 'U':
  case 'V':
  case 'W':
  case 'X':
  case 'Y':
  case 'Z':
  case '_':
  case '.':
  case '$':
  case '~':
    q = name;
  parse_name:
    for (;;)
    {
      if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            strchr("/.-_+=$:\\,~", ch)))
        break;
      if ((q - name) < name_size - 1)
      {
        *q++ = ch;
      }
      ch = ld_inp(s1);
    }
    s1->cc = ch;
    *q = '\0';
    c = LD_TOK_NAME;
    break;
  case CH_EOF:
    c = LD_TOK_EOF;
    break;
  default:
    c = ch;
    break;
  }
  return c;
}

static int ld_add_file(TCCState *s1, const char filename[])
{
  if (filename[0] == '/')
  {
    if (CONFIG_SYSROOT[0] == '\0' && tcc_add_file_internal(s1, filename, AFF_TYPE_BIN) == 0)
      return 0;
    filename = tcc_basename(filename);
  }
  return tcc_add_dll(s1, filename, AFF_PRINT_ERROR);
}

static int ld_add_file_list(TCCState *s1, const char *cmd, int as_needed)
{
  char filename[1024], libname[1016];
  int t, group, nblibs = 0, ret = 0;
  char **libs = NULL;

  group = !strcmp(cmd, "GROUP");
  if (!as_needed)
    s1->new_undef_sym = 0;
  t = ld_next(s1, filename, sizeof(filename));
  if (t != '(')
  {
    ret = tcc_error_noabort("( expected");
    goto lib_parse_error;
  }
  t = ld_next(s1, filename, sizeof(filename));
  for (;;)
  {
    libname[0] = '\0';
    if (t == LD_TOK_EOF)
    {
      ret = tcc_error_noabort("unexpected end of file");
      goto lib_parse_error;
    }
    else if (t == ')')
    {
      break;
    }
    else if (t == '-')
    {
      t = ld_next(s1, filename, sizeof(filename));
      if ((t != LD_TOK_NAME) || (filename[0] != 'l'))
      {
        ret = tcc_error_noabort("library name expected");
        goto lib_parse_error;
      }
      pstrcpy(libname, sizeof libname, &filename[1]);
      if (s1->static_link)
      {
        snprintf(filename, sizeof filename, "lib%s.a", libname);
      }
      else
      {
        snprintf(filename, sizeof filename, "lib%s.so", libname);
      }
    }
    else if (t != LD_TOK_NAME)
    {
      ret = tcc_error_noabort("filename expected");
      goto lib_parse_error;
    }
    if (!strcmp(filename, "AS_NEEDED"))
    {
      ret = ld_add_file_list(s1, cmd, 1);
      if (ret)
        goto lib_parse_error;
    }
    else
    {
      /* TODO: Implement AS_NEEDED support. */
      /*       DT_NEEDED is not used any more so ignore as_needed */
      if (1 || !as_needed)
      {
        ret = ld_add_file(s1, filename);
        if (ret)
          goto lib_parse_error;
        if (group)
        {
          /* Add the filename *and* the libname to avoid future conversions */
          dynarray_add(&libs, &nblibs, tcc_strdup(filename));
          if (libname[0] != '\0')
            dynarray_add(&libs, &nblibs, tcc_strdup(libname));
        }
      }
    }
    t = ld_next(s1, filename, sizeof(filename));
    if (t == ',')
    {
      t = ld_next(s1, filename, sizeof(filename));
    }
  }
  if (group && !as_needed)
  {
    /* Same fast check as the CLI --start-group loop: only continue
       rescanning while some current undef is satisfiable by these
       cached archives. */
    if (s1->new_undef_sym)
    {
      s1->new_undef_sym = tcc_group_has_satisfiable_undefs(s1);
    }
    while (s1->new_undef_sym)
    {
      int i;
      s1->new_undef_sym = 0;
      for (i = 0; i < nblibs; i++)
        ld_add_file(s1, libs[i]);
    }
  }
lib_parse_error:
  dynarray_reset(&libs, &nblibs);
  return ret;
}

/* interpret a subset of GNU ldscripts to handle the dummy libc.so
   files */
ST_FUNC int tcc_load_ldscript(TCCState *s1, int fd)
{
  char cmd[64];
  char filename[1024];
  int t, ret;
  unsigned ldscript_start = 0;

  if (s1->do_bench)
    ldscript_start = tcc_getclock_us();

  s1->fd = fd;
  s1->cc = -1;
  for (;;)
  {
    t = ld_next(s1, cmd, sizeof(cmd));
    if (t == LD_TOK_EOF)
    {
      if (s1->do_bench)
      {
        unsigned elapsed = tcc_getclock_us() - ldscript_start;
        s1->bench_ldscript_load_time += elapsed;
        s1->bench_ldscript_load_count++;
        tcc_bench_log(s1, "ldscript", s1->current_filename, elapsed);
      }
      return 0;
    }
    else if (t != LD_TOK_NAME)
      return -1;
    if (!strcmp(cmd, "INPUT") || !strcmp(cmd, "GROUP"))
    {
      ret = ld_add_file_list(s1, cmd, 0);
      if (ret)
        return ret;
    }
    else if (!strcmp(cmd, "OUTPUT_FORMAT") || !strcmp(cmd, "TARGET"))
    {
      /* ignore some commands */
      t = ld_next(s1, cmd, sizeof(cmd));
      if (t != '(')
        return tcc_error_noabort("( expected");
      for (;;)
      {
        t = ld_next(s1, filename, sizeof(filename));
        if (t == LD_TOK_EOF)
        {
          return tcc_error_noabort("unexpected end of file");
        }
        else if (t == ')')
        {
          break;
        }
      }
    }
    else
    {
      return -1;
    }
  }
  return 0;
}

/* Load and parse a linker script file */
ST_FUNC int tcc_load_linker_script(TCCState *s1, const char *filename)
{
  int fd;
  int ret;

  fd = open(filename, O_RDONLY | O_BINARY);
  if (fd < 0)
  {
    return tcc_error_noabort("linker script '%s' not found", filename);
  }

  /* Allocate linker script structure if not already done */
  if (!s1->ld_script)
  {
    s1->ld_script = tcc_malloc(sizeof(LDScript)); /* ld_script_init clears it */
    ld_script_init(s1->ld_script);
  }

  ret = ld_script_parse(s1, s1->ld_script, fd);
  close(fd);

#if TCCELF_DUMP_LD_SCRIPT
  if (ret == 0 && s1->verbose)
  {
    printf("Loaded linker script: %s\n", filename);
    ld_script_dump(s1->ld_script);
  }
#endif

  /* Add standard symbols */
  if (ret == 0)
  {
    ld_script_add_standard_symbols(s1, s1->ld_script);
  }

  return ret;
}

/* Apply linker script symbols to the ELF symbol table */
void ld_apply_symbols(TCCState *s1, LDScript *ld)
{
  int i;
  for (i = 0; i < ld->nb_symbols; i++)
  {
    LDSymbol *sym = &ld->symbols[i];
    if (sym->defined)
    {
      int sym_idx;
      int vis =
          (sym->visibility == LD_SYM_HIDDEN || sym->visibility == LD_SYM_PROVIDE_HIDDEN) ? STV_HIDDEN : STV_DEFAULT;

      /* Look up once, reuse for both PROVIDE check and update */
      sym_idx = find_elf_sym(s1->symtab, sym->name);

      /* For PROVIDE symbols, only define if no input defines it.  A
       * definition this function made on the early pass (before layout) is
       * the script's own and must take the final address on the late pass;
       * skipping it left every PROVIDEd symbol at its placeholder value. */
      if (sym->visibility == LD_SYM_PROVIDE || sym->visibility == LD_SYM_PROVIDE_HIDDEN)
      {
        if (sym_idx && !sym->applied)
        {
          ElfW(Sym) *esym = &((ElfW(Sym) *)s1->symtab->data)[sym_idx];
          if (esym->st_shndx != SHN_UNDEF)
            continue; /* Already defined, skip */
        }
      }
      sym->applied = 1;

      /* Update existing symbol, or create new */
      if (sym_idx)
      {
        ElfW(Sym) *esym = &((ElfW(Sym) *)s1->symtab->data)[sym_idx];
        esym->st_value = sym->value;
        esym->st_shndx = SHN_ABS;
      }
      else
      {
        /* Use set_elf_sym directly with SHN_ABS to ensure symbols with value 0
         * are still defined as absolute (set_global_sym treats value 0 as
         * UNDEF)
         */
        set_elf_sym(s1->symtab, sym->value, 0, ELFW(ST_INFO)(STB_GLOBAL, STT_NOTYPE), vis, SHN_ABS, sym->name);
      }

      /* Also update in dynsym if it exists there */
      if (s1->dynsym)
      {
        sym_idx = find_elf_sym(s1->dynsym, sym->name);
        if (sym_idx)
        {
          ElfW(Sym) *esym = &((ElfW(Sym) *)s1->dynsym->data)[sym_idx];
          esym->st_value = sym->value;
          esym->st_shndx = SHN_ABS;
        }
      }
    }
  }
}

/* The memory region (VMA) the linker script places input section `name` in,
 * or -1; *origin gets the region's start. */
int ld_section_memory_region(TCCState *s1, const char *name, addr_t *origin)
{
  LDScript *ld = s1->ld_script;
  int pat = -1, os, mr;
  if (!ld)
    return -1;
  os = ld_find_output_section_idx(s1, name, &pat);
  if (os < 0 || os >= ld->nb_output_sections)
    return -1;
  mr = ld->output_sections[os].memory_region_idx;
  if (mr < 0 || mr >= ld->nb_memory_regions)
    return -1;
  *origin = ld->memory_regions[mr].origin;
  return mr;
}

/* Whether the linker script assigns `name` (plainly or through PROVIDE). */
int ld_script_defines_symbol(TCCState *s1, const char *name)
{
  LDScript *ld = s1->ld_script;
  int i;
  if (!ld)
    return 0;
  for (i = 0; i < ld->nb_symbols; i++)
    if (ld->symbols[i].defined && !strcmp(ld->symbols[i].name, name))
      return 1;
  return 0;
}

/* Update linker script symbol values based on actual section layout */
void ld_update_symbol_values(TCCState *s1, LDScript *ld)
{
  Section *s;
  addr_t bss_start = 0, bss_end = 0;
  addr_t data_start = 0, data_end = 0;
  addr_t text_start = 0, text_end = 0;
  addr_t rodata_start = 0, rodata_end = 0;
  addr_t end_addr = 0;
  addr_t sec_end;
  int i, j;
  addr_t output_section_addrs[LD_MAX_OUTPUT_SECTIONS] = {0};
  int output_section_has_addr[LD_MAX_OUTPUT_SECTIONS] = {0};
  addr_t output_section_loadaddrs[LD_MAX_OUTPUT_SECTIONS] = {0};
  addr_t output_section_sizes[LD_MAX_OUTPUT_SECTIONS] = {0};
  addr_t output_section_align[LD_MAX_OUTPUT_SECTIONS] = {0};
  addr_t output_section_vma_end[LD_MAX_OUTPUT_SECTIONS] = {0};

  /* Find section addresses and map output sections to actual addresses */
  for (i = 1; i < s1->nb_sections; i++)
  {
    s = s1->sections[i];
    if (!s->sh_addr)
      continue;

    sec_end = s->sh_addr + s->sh_size;
    if (sec_end > end_addr)
      end_addr = sec_end;

    /* Match .bss and .bss.* sections */
    if (!strcmp(s->name, ".bss") || !strncmp(s->name, ".bss.", 5))
    {
      if (bss_start == 0 || s->sh_addr < bss_start)
        bss_start = s->sh_addr;
      if (sec_end > bss_end)
        bss_end = sec_end;
    }
    /* Match .data and .data.* sections */
    else if (!strcmp(s->name, ".data") || !strncmp(s->name, ".data.", 6))
    {
      if (data_start == 0 || s->sh_addr < data_start)
        data_start = s->sh_addr;
      if (sec_end > data_end)
        data_end = sec_end;
    }
    /* Match .text and .text.* sections */
    else if (!strcmp(s->name, ".text") || !strncmp(s->name, ".text.", 6))
    {
      if (text_start == 0 || s->sh_addr < text_start)
        text_start = s->sh_addr;
      if (sec_end > text_end)
        text_end = sec_end;
    }
    /* Match .rodata and .rodata.* sections */
    else if (!strcmp(s->name, ".rodata") || !strncmp(s->name, ".rodata.", 8))
    {
      if (rodata_start == 0 || s->sh_addr < rodata_start)
        rodata_start = s->sh_addr;
      if (sec_end > rodata_end)
        rodata_end = sec_end;
    }

    /* An output section starts at the lowest input section placed in it,
     * not at the input section sharing its name: `.text` also holds .vectors
     * and .bootmeta, which come first. */
    if (s->sh_flags & SHF_ALLOC)
    {
      int pat_idx = -1;
      int out = ld_find_output_section_idx(s1, s->name, &pat_idx);
      if (out >= 0 && out < ld->nb_output_sections &&
          (!output_section_has_addr[out] || s->sh_addr < output_section_addrs[out]))
      {
        output_section_addrs[out] = s->sh_addr;
        output_section_has_addr[out] = 1;
      }
    }

    /* Accumulate sizes/alignments for load address computation */
    if (ld)
    {
      int pat_idx = -1;
      int ld_idx = ld_find_output_section_idx(s1, s->name, &pat_idx);
      if (ld_idx >= 0 && ld_idx < ld->nb_output_sections)
      {
        addr_t align = s->sh_addralign ? s->sh_addralign : 1;
        if (align > output_section_align[ld_idx])
          output_section_align[ld_idx] = align;
        if (s->sh_type != SHT_NOBITS)
          output_section_sizes[ld_idx] += s->sh_size;
        /* Track VMA end address (includes alignment between sections) */
        if (s->sh_addr + s->sh_size > output_section_vma_end[ld_idx])
          output_section_vma_end[ld_idx] = s->sh_addr + s->sh_size;
      }
    }
  }

  /* Compute output section load addresses (LMA) */
  if (ld)
  {
    addr_t lma_cur[LD_MAX_MEMORY_REGIONS] = {0};
    if (ld->nb_memory_regions > 0)
    {
      for (i = 0; i < ld->nb_memory_regions; i++)
        lma_cur[i] = ld->memory_regions[i].origin;
      for (j = 0; j < ld->nb_output_sections; j++)
      {
        int load_mr = ld->output_sections[j].load_memory_region_idx;
        int vma_mr = ld->output_sections[j].memory_region_idx;
        int mr = (load_mr >= 0) ? load_mr : vma_mr;
        if (mr < 0)
          mr = 0;
        if (mr >= 0 && mr < ld->nb_memory_regions)
        {
          addr_t align = output_section_align[j] ? output_section_align[j] : 1;
          addr_t cur = lma_cur[mr];
          addr_t lma_start = (cur + align - 1) & ~(align - 1);
          output_section_loadaddrs[j] = lma_start;
          /* For sections where VMA region == LMA region (no AT > directive),
             use the actual VMA end address which includes alignment padding
             between input sections. Otherwise use the raw content size. */
          if (load_mr < 0 && output_section_vma_end[j] > lma_start)
            lma_cur[mr] = output_section_vma_end[j];
          else if (load_mr >= 0 && output_section_has_addr[j] && output_section_vma_end[j] > output_section_addrs[j])
            /* AT>: the image keeps the VMA layout, gaps between input
               sections included, so it needs the whole span */
            lma_cur[mr] = lma_start + (output_section_vma_end[j] - output_section_addrs[j]);
          else
            lma_cur[mr] = lma_start + output_section_sizes[j];
        }
      }
    }
    else
    {
      for (j = 0; j < ld->nb_output_sections; j++)
        output_section_loadaddrs[j] = output_section_addrs[j];
    }

    /* Save computed LMA values in LDScript for p_paddr fixup */
    for (j = 0; j < ld->nb_output_sections; j++)
    {
      ld->output_section_loadaddrs[j] = output_section_loadaddrs[j];
      ld->output_section_vmas[j] = output_section_has_addr[j] ? output_section_addrs[j] : 0;
      ld->output_section_vma_ends[j] = output_section_vma_end[j];
    }
    ld->has_loadaddrs = 1;
  }

  /* For NOLOAD sections (like .heap, .stack) that don't have actual ELF
   * sections, compute their addresses based on the memory region they're
   * assigned to in the linker script. Track per-region end addresses. */
  addr_t mr_end[LD_MAX_MEMORY_REGIONS] = {0};

  /* Initialize memory region end addresses from the laid-out sections: the
   * real end of every allocated section inside the region, including ones
   * the script does not name (.got lands after .bss).  The script's own
   * current_offset is a parse-time position that knows nothing of input
   * sizes, and placed .heap -- and __heap_start__ -- on top of .bss. */
  for (i = 1; i < s1->nb_sections; i++)
  {
    s = s1->sections[i];
    if (!s->sh_addr || !(s->sh_flags & SHF_ALLOC))
      continue;
    for (j = 0; j < ld->nb_memory_regions; j++)
    {
      LDMemoryRegion *r = &ld->memory_regions[j];
      if (s->sh_addr >= r->origin && s->sh_addr < r->origin + r->length)
      {
        if (s->sh_addr + s->sh_size > mr_end[j])
          mr_end[j] = s->sh_addr + s->sh_size;
        break;
      }
    }
  }

  /* For regions with no sections, start from their origin */
  for (i = 0; i < ld->nb_memory_regions; i++)
  {
    if (mr_end[i] == 0)
      mr_end[i] = ld->memory_regions[i].origin;
  }

  /* Place NOLOAD sections in their assigned memory regions */
  for (j = 0; j < ld->nb_output_sections; j++)
  {
    if (output_section_addrs[j] == 0)
    {
      /* This output section has no matching ELF section (NOLOAD) */
      int mr = ld->output_sections[j].memory_region_idx;
      if (mr < 0)
        mr = 0;
      if (mr >= 0 && mr < ld->nb_memory_regions)
      {
        output_section_addrs[j] = mr_end[mr];
        output_section_has_addr[j] = 1;
        mr_end[mr] += ld->output_sections[j].current_offset;
      }
    }
  }

  /* First pass: update symbols that are defined in output sections using
   * section_offset. This handles all symbols generically. */
  for (j = 0; j < ld->nb_symbols; j++)
  {
    LDSymbol *sym = &ld->symbols[j];
    if (sym->defined && sym->section_idx >= 0 && sym->section_idx < ld->nb_output_sections)
    {
      addr_t section_addr = output_section_addrs[sym->section_idx];
      if (output_section_has_addr[sym->section_idx])
      {
        /* Symbol value = section base address + offset within section.  The
         * offset was taken at parse time, when input sections had no size:
         * a symbol after input statements (`KEEP(*(.romfs)) __romfs_end__ =
         * .;`) is at least past the inputs those statements placed --
         * otherwise __romfs_end__ equalled __romfs_start__. */
        addr_t v = section_addr + sym->section_offset;
        if (sym->patterns_before > 0)
        {
          int k;
          for (k = 1; k < s1->nb_sections; k++)
          {
            Section *in = s1->sections[k];
            int pat = -1;
            if (!in->sh_addr || !(in->sh_flags & SHF_ALLOC))
              continue;
            /* data_offset, not sh_size: layout pads sh_size to the
             * section alignment, and an end symbol counting the pad made
             * __init_array_end one (null) entry long. */
            if (ld_find_output_section_idx(s1, in->name, &pat) == sym->section_idx && pat >= 0 &&
                pat < sym->patterns_before && in->sh_addr + in->data_offset > v)
              v = in->sh_addr + in->data_offset;
          }
        }
        sym->value = v;
      }
    }
  }

  /* Resolve LOADADDR() symbols using computed LMA addresses */
  for (j = 0; j < ld->nb_symbols; j++)
  {
    LDSymbol *sym = &ld->symbols[j];
    if (sym->has_loadaddr && sym->loadaddr_section_idx < 0 && sym->loadaddr_name[0])
      sym->loadaddr_section_idx = ld_script_find_output_section(ld, sym->loadaddr_name);
    if (sym->has_loadaddr && sym->loadaddr_section_idx >= 0 && sym->loadaddr_section_idx < ld->nb_output_sections)
    {
      addr_t lma = output_section_loadaddrs[sym->loadaddr_section_idx];
      sym->value = lma;
      sym->defined = 1;
    }
  }

  /* Second pass: update standard section boundary symbols.
   * For boundary symbols like __data_start__/__data_end__, always use
   * the computed values based on actual section layout, because the
   * linker script values are only relative offsets that don't account
   * for all input sections. */
  for (j = 0; j < ld->nb_symbols; j++)
  {
    LDSymbol *sym = &ld->symbols[j];

    /* Assigned inside an output section, the first pass placed it from the
     * real layout; these name-based guesses would undo that (__data_start__
     * at the first .data input left .time_critical out of the RAM copy). */
    if (sym->section_idx >= 0)
      continue;

    /* Update standard section symbols - these ALWAYS use computed values */
    if (!strcmp(sym->name, "__bss_start__") || !strcmp(sym->name, "__bss_start"))
    {
      sym->value = bss_start;
      sym->defined = 1;
    }
    else if (!strcmp(sym->name, "__bss_end__") || !strcmp(sym->name, "_bss_end__"))
    {
      sym->value = bss_end;
      sym->defined = 1;
    }
    else if (!strcmp(sym->name, "__data_start__"))
    {
      sym->value = data_start;
      sym->defined = 1;
    }
    else if (!strcmp(sym->name, "__data_end__") || !strcmp(sym->name, "_edata"))
    {
      sym->value = data_end;
      sym->defined = 1;
    }
    else if (!strcmp(sym->name, "__text_start__") || !strcmp(sym->name, "_stext"))
    {
      sym->value = text_start;
      sym->defined = 1;
    }
    else if (!strcmp(sym->name, "__text_end__") || !strcmp(sym->name, "_etext"))
    {
      sym->value = text_end;
      sym->defined = 1;
    }
    else if (!strcmp(sym->name, "__rodata_start__"))
    {
      sym->value = rodata_start;
      sym->defined = 1;
    }
    else if (!strcmp(sym->name, "__rodata_end__"))
    {
      sym->value = rodata_end;
      sym->defined = 1;
    }
    else if (!strcmp(sym->name, "__end__") || !strcmp(sym->name, "_end") || !strcmp(sym->name, "end"))
    {
      sym->value = end_addr;
      sym->defined = 1;
    }
  }
}

/* Set or update a global symbol. If the symbol already exists, update its value
   instead of trying to add it again (which would trigger "defined twice" error). */
static void set_or_update_global_sym(TCCState *s1, const char *name, addr_t value)
{
  int sym_index = find_elf_sym(symtab_section, name);
  if (sym_index)
  {
    ElfW(Sym) *esym = &((ElfW(Sym) *)symtab_section->data)[sym_index];
    if (esym->st_shndx != SHN_UNDEF)
    {
      /* Symbol already defined - update its value */
      esym->st_value = value;
      esym->st_shndx = SHN_ABS;
      return;
    }
  }
  /* not set_global_sym: it makes a section-less value 0 an undefined symbol */
  set_elf_sym(symtab_section, value, 0, ELFW(ST_INFO)(STB_GLOBAL, STT_NOTYPE), 0, SHN_ABS, name);
}

/* Export standard end/heap symbols based on section layout */
ST_FUNC void ld_export_standard_symbols(TCCState *s1)
{
  Section *s;
  addr_t bss_start = 0, bss_end = 0;
  addr_t data_start = 0, data_end = 0;
  addr_t text_start = 0, text_end = 0;
  addr_t end_addr = 0;
  addr_t sec_end;
  int have_bss = 0, have_data = 0, have_text = 0, have_end = 0;
  int i;

  /* Find section addresses */
  for (i = 1; i < s1->nb_sections; i++)
  {
    s = s1->sections[i];
    /* address 0 is a valid placement (YAFF modules, -Ttext=0) */
    if (!(s->sh_flags & SHF_ALLOC) || !s->sh_size)
      continue;

    sec_end = s->sh_addr + s->sh_size;
    if (!have_end || sec_end > end_addr)
      end_addr = sec_end;
    have_end = 1;

    /* Match .bss and .bss.* sections */
    if (!strcmp(s->name, ".bss") || !strncmp(s->name, ".bss.", 5))
    {
      if (!have_bss || s->sh_addr < bss_start)
        bss_start = s->sh_addr;
      if (!have_bss || sec_end > bss_end)
        bss_end = sec_end;
      have_bss = 1;
    }
    /* Match .data and .data.* sections */
    else if (!strcmp(s->name, ".data") || !strncmp(s->name, ".data.", 6))
    {
      if (!have_data || s->sh_addr < data_start)
        data_start = s->sh_addr;
      if (!have_data || sec_end > data_end)
        data_end = sec_end;
      have_data = 1;
    }
    /* Match .text and .text.* sections */
    else if (!strcmp(s->name, ".text") || !strncmp(s->name, ".text.", 6))
    {
      if (!have_text || s->sh_addr < text_start)
        text_start = s->sh_addr;
      if (!have_text || sec_end > text_end)
        text_end = sec_end;
      have_text = 1;
    }
  }

  /* Set standard symbols, updating existing ones if already defined
     (e.g. by tcc_add_linker_symbols) */
  if (have_bss)
  {
    set_or_update_global_sym(s1, "__bss_start__", bss_start);
    set_or_update_global_sym(s1, "__bss_start", bss_start);
  }
  if (have_bss)
  {
    set_or_update_global_sym(s1, "__bss_end__", bss_end);
    set_or_update_global_sym(s1, "_bss_end__", bss_end);
  }
  if (have_data)
  {
    set_or_update_global_sym(s1, "__data_start__", data_start);
  }
  if (have_data)
  {
    set_or_update_global_sym(s1, "_edata", data_end);
    set_or_update_global_sym(s1, "__data_end__", data_end);
  }
  if (have_text)
  {
    set_or_update_global_sym(s1, "__text_start__", text_start);
    set_or_update_global_sym(s1, "_stext", text_start);
  }
  if (have_text)
  {
    set_or_update_global_sym(s1, "_etext", text_end);
    set_or_update_global_sym(s1, "__text_end__", text_end);
  }
  if (have_end)
  {
    set_or_update_global_sym(s1, "__end__", end_addr);
    set_or_update_global_sym(s1, "_end", end_addr);
    set_or_update_global_sym(s1, "end", end_addr);
    /* Heap typically starts at end */
    set_or_update_global_sym(s1, "__heap_start__", end_addr);
  }
}

#endif /* !ELF_OBJ_ONLY */
