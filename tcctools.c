/* -------------------------------------------------------------- */
/*
 *  TCC - Tiny C Compiler
 *
 *  tcctools.c - extra tools and and -m32/64 support
 *
 */

/* -------------------------------------------------------------- */
/*
 * This program is for making libtcc1.a without ar
 * tiny_libmaker - tiny elf lib maker
 * usage: tiny_libmaker [lib] files...
 * Copyright (c) 2007 Timppa
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "tcc.h"

// #define ARMAG  "!<arch>\n"
#define ARFMAG "`\n"

typedef struct
{
  char ar_name[16];
  char ar_date[12];
  char ar_uid[6];
  char ar_gid[6];
  char ar_mode[8];
  char ar_size[10];
  char ar_fmag[2];
} ArHdr;

static unsigned long le2belong(unsigned long ul)
{
  return ((ul & 0xFF0000) >> 8) + ((ul & 0xFF000000) >> 24) + ((ul & 0xFF) << 24) + ((ul & 0xFF00) << 8);
}

static int ar_usage(int ret)
{
  fprintf(stderr, "usage: tcc -ar [crstvx] lib [files]\n");
  fprintf(stderr, "create library ([abdiopN] not supported).\n");
  return ret;
}

ST_FUNC int tcc_tool_ar(TCCState *s1, int argc, char **argv)
{
  static const ArHdr arhdr_init = {"/               ", "0           ", "0     ", "0     ",
                                   "0       ",         "0         ",   ARFMAG};

  ArHdr arhdr = arhdr_init;
  ArHdr arhdro = arhdr_init;

  FILE *fi, *fh = NULL, *fo = NULL;
  const char *created_file = NULL; // must delete on error
  ElfW(Ehdr) * ehdr;
  ElfW(Shdr) * shdr;
  ElfW(Sym) * sym;
  int i, fsize, i_lib, i_obj;
  char *buf, *shstr, *symtab, *strtab;
  int symtabsize = 0; //, strtabsize = 0;
  char *anames = NULL;
  int *afpos = NULL;
  int istrlen, strpos = 0, fpos = 0, funccnt = 0, funcmax, hofs;
  char tfile[260], stmp[20];
  char *file, *name;
  int ret = 2;
  const char *ops_conflict = "habdiopN"; // unsupported but destructive if ignored.
  int extract = 0;
  int table = 0;
  int verbose = 0;

  i_lib = 0;
  i_obj = 0; // will hold the index of the lib and first obj
  for (i = 1; i < argc; i++)
  {
    const char *a = argv[i];
    if (*a == '-' && strchr(a, '.'))
      ret = 1; // -x.y is always invalid (same as gnu ar)
    if ((*a == '-') || (i == 1 && !strchr(a, '.')))
    { // options argument
      if (strpbrk(a, ops_conflict))
        ret = 1;
      if (strchr(a, 'x'))
        extract = 1;
      if (strchr(a, 't'))
        table = 1;
      if (strchr(a, 'v'))
        verbose = 1;
    }
    else
    {             // lib or obj files: don't abort - keep validating all args.
      if (!i_lib) // first file is the lib
        i_lib = i;
      else if (!i_obj) // second file is the first obj
        i_obj = i;
    }
  }

  if (!i_lib) // i_obj implies also i_lib.
    ret = 1;
  i_obj = i_obj ? i_obj : argc; // An empty archive will be generated if no
                                // input file is given

  if (ret == 1)
    return ar_usage(ret);

  if (extract || table)
  {
    if ((fh = fopen(argv[i_lib], "rb")) == NULL)
    {
      fprintf(stderr, "tcc: ar: can't open file %s\n", argv[i_lib]);
      goto finish;
    }
    fread(stmp, 1, 8, fh);
    if (memcmp(stmp, ARMAG, 8))
    {
    no_ar:
      fprintf(stderr, "tcc: ar: not an ar archive %s\n", argv[i_lib]);
      goto finish;
    }
    while (fread(&arhdr, 1, sizeof(arhdr), fh) == sizeof(arhdr))
    {
      char *p, *e;

      if (memcmp(arhdr.ar_fmag, ARFMAG, 2))
        goto no_ar;
      p = arhdr.ar_name;
      for (e = p + sizeof arhdr.ar_name; e > p && e[-1] == ' ';)
        e--;
      *e = '\0';
      arhdr.ar_size[sizeof arhdr.ar_size - 1] = 0;
      fsize = atoi(arhdr.ar_size);
      buf = tcc_malloc(fsize + 1);
      fread(buf, fsize, 1, fh);
      if (strcmp(arhdr.ar_name, "/") && strcmp(arhdr.ar_name, "/SYM64/"))
      {
        if (e > p && e[-1] == '/')
          e[-1] = '\0';
        /* tv not implemented */
        if (table || verbose)
          printf("%s%s\n", extract ? "x - " : "", arhdr.ar_name);
        if (extract)
        {
          if ((fo = fopen(arhdr.ar_name, "wb")) == NULL)
          {
            fprintf(stderr, "tcc: ar: can't create file %s\n", arhdr.ar_name);
            tcc_free(buf);
            goto finish;
          }
          fwrite(buf, fsize, 1, fo);
          fclose(fo);
          /* ignore date/uid/gid/mode */
        }
      }
      if (fsize & 1)
        fgetc(fh);
      tcc_free(buf);
    }
    ret = 0;
  finish:
    if (fh)
      fclose(fh);
    return ret;
  }

  if ((fh = fopen(argv[i_lib], "wb")) == NULL)
  {
    fprintf(stderr, "tcc: ar: can't create file %s\n", argv[i_lib]);
    goto the_end;
  }
  created_file = argv[i_lib];

  sprintf(tfile, "%s.tmp", argv[i_lib]);
  if ((fo = fopen(tfile, "wb+")) == NULL)
  {
    fprintf(stderr, "tcc: ar: can't create temporary file %s\n", tfile);
    goto the_end;
  }

  funcmax = 250;
  {
    int alloc_sz = funcmax * sizeof *afpos;
    afpos = tcc_realloc(NULL, alloc_sz);
  } // 250 func
  memcpy(&arhdro.ar_mode, "100644", 6);

  // i_obj = first input object file
  while (i_obj < argc)
  {
    if (*argv[i_obj] == '-')
    { // by now, all options start with '-'
      i_obj++;
      continue;
    }
    if ((fi = fopen(argv[i_obj], "rb")) == NULL)
    {
      fprintf(stderr, "tcc: ar: can't open file %s \n", argv[i_obj]);
      goto the_end;
    }
    if (verbose)
      printf("a - %s\n", argv[i_obj]);

    fseek(fi, 0, SEEK_END);
    fsize = ftell(fi);
    fseek(fi, 0, SEEK_SET);
    buf = tcc_malloc(fsize + 1);
    fread(buf, fsize, 1, fi);
    fclose(fi);

    // elf header
    ehdr = (ElfW(Ehdr) *)buf;
    if (ehdr->e_ident[4] != ELFCLASSW)
    {
      fprintf(stderr, "tcc: ar: Unsupported Elf Class: %s\n", argv[i_obj]);
      goto the_end;
    }

    {
      int shent_off = ehdr->e_shstrndx * ehdr->e_shentsize;
      shdr = (ElfW(Shdr) *)(buf + ehdr->e_shoff + shent_off);
    }
    shstr = (char *)(buf + shdr->sh_offset);
    symtab = strtab = NULL;
    for (i = 0; i < ehdr->e_shnum; i++)
    {
      int shent_off = i * ehdr->e_shentsize;
      shdr = (ElfW(Shdr) *)(buf + ehdr->e_shoff + shent_off);
      if (!shdr->sh_offset)
        continue;
      if (shdr->sh_type == SHT_SYMTAB)
      {
        symtab = (char *)(buf + shdr->sh_offset);
        symtabsize = shdr->sh_size;
      }
      if (shdr->sh_type == SHT_STRTAB)
      {
        if (!strcmp(shstr + shdr->sh_name, ".strtab"))
        {
          strtab = (char *)(buf + shdr->sh_offset);
          // strtabsize = shdr->sh_size;
        }
      }
    }

    if (symtab && strtab)
    {
      int nsym = symtabsize / sizeof(ElfW(Sym));
      // printf("symtab: info size shndx name\n");
      for (i = 1; i < nsym; i++)
      {
        int sym_off = i * sizeof(ElfW(Sym));
        sym = (ElfW(Sym) *)(symtab + sym_off);
        if (sym->st_shndx && (sym->st_info == 0x10 || sym->st_info == 0x11 || sym->st_info == 0x12 ||
                              sym->st_info == 0x20 || sym->st_info == 0x21 || sym->st_info == 0x22))
        {
          // printf("symtab: %2Xh %4Xh %2Xh %s\n", sym->st_info, sym->st_size,
          // sym->st_shndx, strtab + sym->st_name);
          istrlen = strlen(strtab + sym->st_name) + 1;
          anames = tcc_realloc(anames, strpos + istrlen);
          strcpy(anames + strpos, strtab + sym->st_name);
          strpos += istrlen;
          if (++funccnt >= funcmax)
          {
            funcmax += 250;
            {
              int alloc_sz = funcmax * sizeof *afpos;
              afpos = tcc_realloc(afpos, alloc_sz);
            } // 250 func more
          }
          afpos[funccnt] = fpos;
        }
      }
    }

    file = argv[i_obj];
    for (name = strchr(file, 0); name > file && name[-1] != '/' && name[-1] != '\\'; --name)
      ;
    istrlen = strlen(name);
    if (istrlen >= sizeof(arhdro.ar_name))
      istrlen = sizeof(arhdro.ar_name) - 1;
    memset(arhdro.ar_name, ' ', sizeof(arhdro.ar_name));
    memcpy(arhdro.ar_name, name, istrlen);
    arhdro.ar_name[istrlen] = '/';
    sprintf(stmp, "%-10d", fsize);
    memcpy(&arhdro.ar_size, stmp, 10);
    fwrite(&arhdro, sizeof(arhdro), 1, fo);
    fwrite(buf, fsize, 1, fo);
    tcc_free(buf);
    i_obj++;
    fpos += (fsize + sizeof(arhdro));
    if (fpos & 1)
      fputc(0, fo), ++fpos;
  }
  {
    int sym_tbl_sz = (funccnt + 1) * sizeof(int);
    hofs = 8 + sizeof(arhdr) + strpos + sym_tbl_sz;
  }
  fpos = 0;
  if ((hofs & 1)) // align
    hofs++, fpos = 1;
  // write header
  fwrite(ARMAG, 8, 1, fh);
  // create an empty archive
  if (!funccnt)
  {
    ret = 0;
    goto the_end;
  }
  {
    int sym_tbl_sz = (funccnt + 1) * sizeof(int);
    sprintf(stmp, "%-10d", (int)(strpos + sym_tbl_sz) + fpos);
  }
  memcpy(&arhdr.ar_size, stmp, 10);
  fwrite(&arhdr, sizeof(arhdr), 1, fh);
  afpos[0] = le2belong(funccnt);
  for (i = 1; i <= funccnt; i++)
    afpos[i] = le2belong(afpos[i] + hofs);
  {
    int sym_tbl_sz = (funccnt + 1) * sizeof(int);
    fwrite(afpos, sym_tbl_sz, 1, fh);
  }
  fwrite(anames, strpos, 1, fh);
  if (fpos)
    fwrite("", 1, 1, fh);
  // write objects
  fseek(fo, 0, SEEK_END);
  fsize = ftell(fo);
  fseek(fo, 0, SEEK_SET);
  buf = tcc_malloc(fsize + 1);
  fread(buf, fsize, 1, fo);
  fwrite(buf, fsize, 1, fh);
  tcc_free(buf);
  ret = 0;
the_end:
  if (anames)
    tcc_free(anames);
  if (afpos)
    tcc_free(afpos);
  if (fh)
    fclose(fh);
  if (created_file && ret != 0)
    remove(created_file);
  if (fo)
    fclose(fo), remove(tfile);
  return ret;
}

/* -------------------------------------------------------------- */
/*
 *  TCC - Tiny C Compiler
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

ST_FUNC int tcc_tool_cross(TCCState *s1, char **argv, int option)
{
  tcc_error_noabort("-m%d not implemented.", option);
  return 1;
}

/* -------------------------------------------------------------- */
/* generate xxx.d file */

static char *escape_target_dep(const char *s)
{
  char *res = tcc_malloc(strlen(s) * 2 + 1);
  int j;
  for (j = 0; *s; s++, j++)
  {
    if (is_space(*s))
    {
      res[j++] = '\\';
    }
    res[j] = *s;
  }
  res[j] = '\0';
  return res;
}

ST_FUNC int gen_makedeps(TCCState *s1, const char *target, const char *filename)
{
  FILE *depout;
  char buf[1024];
  char **escaped_targets;
  int i, k, num_targets;

  if (!filename)
  {
    /* compute filename automatically: dir/file.o -> dir/file.d */
    snprintf(buf, sizeof buf, "%.*s.d", (int)(tcc_fileextension(target) - target), target);
    filename = buf;
  }

  if (!strcmp(filename, "-"))
    depout = fdopen(1, "w");
  else
    /* XXX return err codes instead of error() ? */
    depout = fopen(filename, "w");
  if (!depout)
    return tcc_error_noabort("could not open '%s'", filename);
  if (s1->verbose)
    printf("<- %s\n", filename);

  escaped_targets = tcc_malloc(s1->nb_target_deps * sizeof(*escaped_targets));
  num_targets = 0;
  for (i = 0; i < s1->nb_target_deps; ++i)
  {
    for (k = 0; k < i; ++k)
      if (0 == strcmp(s1->target_deps[i], s1->target_deps[k]))
        goto next;
    escaped_targets[num_targets++] = escape_target_dep(s1->target_deps[i]);
  next:;
  }

  fprintf(depout, "%s:", target);
  for (i = 0; i < num_targets; ++i)
    fprintf(depout, " \\\n  %s", escaped_targets[i]);
  fprintf(depout, "\n");
  if (s1->gen_phony_deps)
  {
    /* Skip first file, which is the c file.
     * Only works for single file give on command-line,
     * but other compilers have the same limitation */
    for (i = 1; i < num_targets; ++i)
      fprintf(depout, "%s:\n", escaped_targets[i]);
  }
  for (i = 0; i < num_targets; ++i)
    tcc_free(escaped_targets[i]);
  tcc_free(escaped_targets);
  fclose(depout);
  return 0;
}

/* -------------------------------------------------------------- */
