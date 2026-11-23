/*
 *  TCC - stack use per function
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* YasOS process stacks are 4 to 8 KB.  After each function is generated this
 * reads what the backend measured (tcc_gen_machine_stack_end: the prologue's
 * drop -- pushed registers, locals, spills, the outgoing argument area -- and
 * the most SP drops below that in the body) and
 *
 *   -Wframe-larger-than=N  warns when that is over N bytes;
 *   -Wstack-usage=N        the same, and for a frame whose size is only known
 *                          at run time (VLA, alloca): "might be unbounded";
 *   -fstack-usage          writes gcc's <object>.su:
 *                          file:line:col:function<TAB>bytes<TAB>qualifier
 *                          (static, dynamic,bounded, dynamic);
 *   -fcallgraph-info[=su]  writes <object>.ci, the callees, each function's
 *                          type and its indirect calls' types (calling shapes,
 *                          tcc_stack_usage_sig) and the unit's address-taken
 *                          functions (format: scripts/stack_usage.py), which
 *                          scripts/stack_usage.py sums per entry point.
 *
 * Locations are gcc's: the warnings at the body's closing brace, the .su at
 * the declared name's line and column (tcc_stack_usage_declarator).
 *
 * The frame size depends on -O (register allocation, inlining); that is the
 * real frame of the code generated.  Nothing here changes the code: the
 * backend only watches the instructions it emits, and only when one of these
 * options asks (tcc_stack_usage_wanted). */

#define USING_GLOBALS
#include "tcc.h"

typedef struct SuCallee
{
  char *name;
  unsigned char tail; /* a B.W: the callee runs in this frame's place */
} SuCallee;

typedef struct SuRec
{
  Sym *sym; /* a function recompiled at the end of the unit replaces its record */
  char *name;
  char *file;
  int line, col; /* the .su location: the declarator's name (col 1: not known) */
  int sig;       /* its own function type (tcc_stack_usage_sig) */
  unsigned bytes;
  unsigned char qual; /* 0 static, 1 dynamic,bounded, 2 dynamic */
  unsigned char local;
  unsigned char warned;
  int indirect_calls, indirect_tail;
  TCCStackIndirect by_sig[TCC_SU_MAX_INDIRECT_SIGS + 1];
  int nb_sigs;
  SuCallee *callees;
  int nb_callees;
} SuRec;

/* A function's entry in the unit's table, by its Sym: where its name was
 * declared (a body replayed at the end of the unit has no declarator) and
 * its record once generated. */
typedef struct SuSlot
{
  Sym *sym;
  int line, col; /* 0: no declarator seen */
  int rec;       /* 1 + its index in recs, 0 before it is generated */
} SuSlot;

typedef struct StackUsage
{
  SuRec *recs;
  int nb_recs, cap_recs;
  SuSlot *slots; /* open addressing, cap_slots a power of two */
  int nb_slots, cap_slots;
  char **sigs; /* the function types met, interned: sig N is sigs[N - 1] */
  int nb_sigs;
  char **taken; /* "NAME[ local]" of the functions whose address is taken */
  int nb_taken;
} StackUsage;

ST_FUNC int tcc_stack_usage_wanted(TCCState *s1)
{
  return TCC_SU_WANTED(s1);
}

static StackUsage *su_get(TCCState *s1)
{
  if (!s1->su)
    s1->su = tcc_mallocz(sizeof(StackUsage));
  return s1->su;
}

static unsigned su_hash(const Sym *sym)
{
  uintptr_t v = (uintptr_t)sym;
  v ^= v >> 17;
  return (unsigned)(v * 0x9E3779B1u);
}

static SuSlot *su_slot(StackUsage *su, Sym *sym, int create)
{
  if (su->cap_slots)
  {
    for (unsigned i = su_hash(sym) & (su->cap_slots - 1);; i = (i + 1) & (su->cap_slots - 1))
    {
      if (su->slots[i].sym == sym)
        return &su->slots[i];
      if (!su->slots[i].sym)
        break;
    }
  }
  if (!create)
    return NULL;
  if (2 * (su->nb_slots + 1) > su->cap_slots)
  {
    SuSlot *old = su->slots;
    const int old_cap = su->cap_slots;
    su->cap_slots = old_cap ? 2 * old_cap : 64;
    su->slots = tcc_mallocz(su->cap_slots * sizeof(SuSlot));
    for (int k = 0; k < old_cap; k++)
      if (old[k].sym)
      {
        unsigned i = su_hash(old[k].sym) & (su->cap_slots - 1);
        while (su->slots[i].sym)
          i = (i + 1) & (su->cap_slots - 1);
        su->slots[i] = old[k];
      }
    tcc_free(old);
  }
  unsigned i = su_hash(sym) & (su->cap_slots - 1);
  while (su->slots[i].sym)
    i = (i + 1) & (su->cap_slots - 1);
  su->slots[i].sym = sym;
  su->nb_slots++;
  return &su->slots[i];
}

/* The column of the identifier V the tokenizer has just read, 1-based in
 * bytes as gcc's .su; 0 when it did not come straight from the file (a macro
 * expansion, a replayed body).  The file's buffer is refilled IO_BUF_SIZE
 * bytes at a time: col_base is the part of the line before its start. */
ST_FUNC int tcc_stack_usage_name_column(int v)
{
  BufferedFile *bf = file;
  if (macro_ptr || !bf || bf->fd < 0 || v < TOK_IDENT)
    return 0;
  const char *name = get_tok_str(v, NULL);
  const uint8_t *start = bf->buf_ptr - strlen(name);
  if (start + strlen(name) > bf->buf_end || memcmp(start, name, strlen(name)))
    return 0; /* not where the reader stands: do not guess */
  const uint8_t *p = start;
  while (p > bf->buffer && p[-1] != '\n')
    p--;
  return (int)(start - p) + (p == bf->buffer ? bf->col_base : 0) + 1;
}

/* The definition of function SYM begins: its declarator's name V was read at
 * su_name_line / su_name_col (type_decl), else LINE is the declarator's. */
ST_FUNC void tcc_stack_usage_declarator(TCCState *s1, Sym *sym, int v, int line)
{
  SuSlot *sl = su_slot(su_get(s1), sym, 1);
  if (s1->su_name_tok == v && s1->su_name_line)
  {
    sl->line = s1->su_name_line;
    sl->col = s1->su_name_col;
  }
  else
  {
    sl->line = line;
    sl->col = 0;
  }
}

/* A function type's calling shape: what an indirect call and the functions
 * it may reach have in common, also across a cast between pointer types --
 * every pointer, integer up to 32 bits, enum and _Bool is `i`, long long
 * `l`, float `f`, double `d`, a struct or union `s` and its size.  The
 * signature is "RET(ARG,ARG[,...])", "RET(?)" for a type without a
 * prototype (it matches any of the same RET). */
static void su_class(CString *cs, CType *t)
{
  int a;
  if (t->t & (VT_COMPLEX | VT_VECTOR))
  {
    char b[16];
    snprintf(b, sizeof b, "%c%d", t->t & VT_COMPLEX ? 'c' : 'x', type_size(t, &a));
    cstr_cat(cs, b, -1);
    return;
  }
  switch (t->t & VT_BTYPE)
  {
  case VT_VOID:
    cstr_ccat(cs, 'v');
    break;
  case VT_LLONG:
    cstr_ccat(cs, 'l');
    break;
  case VT_FLOAT:
    cstr_ccat(cs, 'f');
    break;
  case VT_DOUBLE:
  case VT_LDOUBLE:
    cstr_ccat(cs, 'd');
    break;
  case VT_STRUCT:
  {
    char b[16];
    snprintf(b, sizeof b, "s%d", type_size(t, &a));
    cstr_cat(cs, b, -1);
    break;
  }
  default:
    cstr_ccat(cs, 'i');
    break;
  }
}

/* The signature of the function type FTYPE (a VT_FUNC type's ref), interned:
 * 1 + its index in the unit's table. */
ST_FUNC int tcc_stack_usage_sig(TCCState *s1, Sym *ftype)
{
  if (!ftype)
    return 0;
  StackUsage *su = su_get(s1);
  CString cs;
  cstr_new(&cs);
  su_class(&cs, &ftype->type);
  cstr_ccat(&cs, '(');
  if (ftype->f.func_type == FUNC_OLD)
    cstr_ccat(&cs, '?');
  else
  {
    int n = 0;
    for (Sym *pa = sym_first(ftype); pa; pa = pa->next)
    {
      if (n++)
        cstr_ccat(&cs, ',');
      su_class(&cs, &pa->type);
    }
    if (ftype->f.func_type == FUNC_ELLIPSIS)
      cstr_cat(&cs, n ? ",..." : "...", -1);
  }
  cstr_ccat(&cs, ')');
  cstr_ccat(&cs, 0);
  int id = 0;
  for (int i = 0; i < su->nb_sigs && !id; i++)
    if (!strcmp(su->sigs[i], cs.data))
      id = i + 1;
  if (!id && su->nb_sigs < 0xFFFF)
  {
    dynarray_add(&su->sigs, &su->nb_sigs, tcc_strdup(cs.data));
    id = su->nb_sigs;
  }
  cstr_free(&cs);
  return id;
}

/* The frontend's call CALL_ID goes through a value of function type FTYPE. */
ST_FUNC void tcc_stack_usage_note_call(TCCState *s1, int call_id, Sym *ftype)
{
  TCCIRState *ir = s1->ir;
  if (!ir || call_id < 0)
    return;
  const int sig = tcc_stack_usage_sig(s1, ftype);
  if (call_id >= ir->su_nb_call_sig)
  {
    const int n = call_id + 16;
    ir->su_call_sig = tcc_realloc(ir->su_call_sig, n * sizeof(unsigned short));
    memset(ir->su_call_sig + ir->su_nb_call_sig, 0, (n - ir->su_nb_call_sig) * sizeof(unsigned short));
    ir->su_nb_call_sig = n;
  }
  ir->su_call_sig[call_id] = (unsigned short)sig;
}

ST_FUNC int tcc_stack_usage_call_sig(TCCState *s1, int call_id)
{
  TCCIRState *ir = s1->ir;
  if (!ir || call_id < 0 || call_id >= ir->su_nb_call_sig)
    return 0;
  return ir->su_call_sig[call_id];
}

static void su_rec_clear(SuRec *r)
{
  tcc_free(r->name);
  tcc_free(r->file);
  for (int i = 0; i < r->nb_callees; i++)
    tcc_free(r->callees[i].name);
  tcc_free(r->callees);
}

ST_FUNC void tcc_stack_usage_free(TCCState *s1)
{
  StackUsage *su = s1->su;
  if (!su)
    return;
  for (int i = 0; i < su->nb_recs; i++)
    su_rec_clear(&su->recs[i]);
  tcc_free(su->recs);
  tcc_free(su->slots);
  dynarray_reset(&su->sigs, &su->nb_sigs);
  dynarray_reset(&su->taken, &su->nb_taken);
  tcc_free(su);
  s1->su = NULL;
}

static const char *su_elf_name(int sym_index)
{
  ElfW(Sym) *esym = &((ElfW(Sym) *)symtab_section->data)[sym_index];
  /* a static function's name reaches strtab only at the end of the unit */
  if (esym->st_name & ELF_NAME_DEFERRED)
    return tok_name_tmp((int)(esym->st_name & ~ELF_NAME_DEFERRED));
  return (const char *)symtab_section->link->data + esym->st_name;
}

/* The direct calls in [start, end): the BL / B.W relocations the body left. */
static void su_callees(SuRec *r, Section *sec, addr_t start, size_t reloc_mark)
{
  Section *sr = sec->reloc;
  if (!sr || !sr->data || reloc_mark > sr->data_offset)
    return;
  for (size_t off = reloc_mark; off + sizeof(ElfW_Rel) <= sr->data_offset; off += sizeof(ElfW_Rel))
  {
    const ElfW_Rel *rel = (const ElfW_Rel *)(sr->data + off);
    const int type = ELFW(R_TYPE)(rel->r_info);
    if ((type != R_ARM_THM_JUMP24 && type != R_ARM_THM_PC22) || rel->r_offset < start ||
        rel->r_offset + 4 > (addr_t)ind) /* the section's end is set after this */
      continue;
    const char *name = su_elf_name(ELFW(R_SYM)(rel->r_info));
    /* -Os shared bodies run in the caller's frame with LR saved: no stack */
    if (!name[0] || !strncmp(name, "__tcc_outlined.", 15))
      continue;
    /* T1 BL has bit 14 of its second halfword set, T4 B.W clear */
    const unsigned char *p = sec->data + rel->r_offset;
    const int tail = !(((p[2] | p[3] << 8) >> 14) & 1);
    int k;
    for (k = 0; k < r->nb_callees; k++)
      if (!strcmp(r->callees[k].name, name))
        break;
    if (k < r->nb_callees)
    {
      r->callees[k].tail &= tail; /* called once with BL: count its frame */
      continue;
    }
    if (!(r->nb_callees & (r->nb_callees - 1)))
      r->callees = tcc_realloc(r->callees, (r->nb_callees ? 2 * r->nb_callees : 4) * sizeof(SuCallee));
    r->callees[r->nb_callees].name = tcc_strdup(name);
    r->callees[r->nb_callees++].tail = tail;
  }
}

static int su_warn_on(unsigned char w)
{
  return (w & WARN_ON) != 0;
}

/* The function SYM was just generated at [start, ind) of SEC (relocations from
 * RELOC_MARK); LINE is where its body began, END_LINE its closing brace. */
ST_FUNC void tcc_stack_usage_function(TCCState *s1, Sym *sym, int line, int end_line, Section *sec, addr_t start,
                                      size_t reloc_mark, int naked)
{
  TCCStackFrame f;
  tcc_gen_machine_stack_end(&f);
  if (!tcc_stack_usage_wanted(s1))
    return;
  StackUsage *su = su_get(s1);

  ElfSym *esym = elfsym(sym);
  const char *name = esym ? su_elf_name(sym->c) : get_tok_str(sym->v, NULL);
  SuSlot *sl = su_slot(su, sym, 1);
  SuRec *r = sl->rec ? &su->recs[sl->rec - 1] : NULL;
  int warned = 0;
  if (r)
  {
    warned = r->warned;
    su_rec_clear(r);
  }
  else
  {
    if (su->nb_recs == su->cap_recs)
    {
      su->cap_recs = su->cap_recs ? 2 * su->cap_recs : 16;
      su->recs = tcc_realloc(su->recs, su->cap_recs * sizeof(SuRec));
    }
    r = &su->recs[su->nb_recs++];
    sl->rec = su->nb_recs;
  }
  memset(r, 0, sizeof *r);
  r->sym = sym;
  r->name = tcc_strdup(name);
  BufferedFile *bf = file;
  while (bf && bf->filename[0] == ':' && bf->prev)
    bf = bf->prev;
  r->file = tcc_strdup(bf ? bf->filename : "?");
  /* gcc's .su location: the name's line and column; a definition whose
     declarator was not seen (none should be) keeps the body's line */
  r->line = sl->line ? sl->line : line;
  r->col = sl->line && sl->col ? sl->col : 1;
  r->bytes = naked ? (unsigned)f.transient : (unsigned)(f.entry + f.transient);
  r->qual = (f.dynamic || s1->func_dynamic_sp) ? 2 : f.transient ? 1 : 0;
  r->local = esym && ELFW(ST_BIND)(esym->st_info) == STB_LOCAL;
  r->indirect_calls = f.indirect_calls;
  r->indirect_tail = f.indirect_tail;
  r->nb_sigs = f.nb_sigs;
  memcpy(r->by_sig, f.by_sig, sizeof r->by_sig);
  if (s1->callgraph_info)
  {
    r->sig = sym->type.ref ? tcc_stack_usage_sig(s1, sym->type.ref) : 0;
    su_callees(r, sec, start, reloc_mark);
  }
  line = end_line; /* the warnings: at the closing brace, as gcc's */

  /* At most one warning a function: a body recompiled at the end of the unit
   * (gen_late_reopt_functions) is reported only if the first one was not. */
  r->warned = warned;
  if (warned || s1->warn_none)
    return;
  s1->diag_noabort++; /* an error here still checks the functions after it */
  if (s1->frame_larger_than_set && su_warn_on(s1->warn_frame_larger_than) && r->bytes > s1->frame_larger_than)
  {
    tcc_warning_c(warn_frame_larger_than)("%i:the frame size of '%s' is %u bytes, larger than %llu bytes", line, name,
                                          r->bytes, s1->frame_larger_than);
    r->warned = 1;
  }
  if (s1->stack_usage_limit_set && su_warn_on(s1->warn_stack_usage))
  {
    if (r->qual == 2)
    {
      tcc_warning_c(warn_stack_usage)("%i:stack usage of '%s' might be unbounded", line, name);
      r->warned = 1;
    }
    else if (r->bytes > s1->stack_usage_limit)
    {
      tcc_warning_c(warn_stack_usage)("%i:stack usage of '%s' is %u bytes, larger than %llu bytes", line, name,
                                      r->bytes, s1->stack_usage_limit);
      r->warned = 1;
    }
  }
  s1->diag_noabort--;
}

static FILE *su_open(const char *object, const char *ext)
{
  char buf[1024];
  pstrcpy(buf, sizeof buf - 4, object);
  char *e = tcc_fileextension(buf);
  strcpy(e, ext);
  FILE *fp = fopen(buf, "w");
  if (!fp)
    tcc_error_noabort("could not write '%s'", buf);
  return fp;
}

static const char *const su_qual[] = {"static", "dynamic,bounded", "dynamic"};

static const char *su_sig_name(StackUsage *su, int sig)
{
  return sig > 0 && sig <= su->nb_sigs ? su->sigs[sig - 1] : "?";
}

/* A relocation that branches to its symbol rather than taking its address. */
static int su_is_branch_reloc(int type)
{
  switch (type)
  {
  case R_ARM_NONE:
  case R_ARM_THM_PC22: /* BL */
  case R_ARM_THM_JUMP24:
  case R_ARM_THM_JUMP19:
  case R_ARM_CALL:
  case R_ARM_JUMP24:
  case R_ARM_PLT32:
  case R_ARM_V4BX:
  case R_ARM_PREL31: /* unwind tables */
  case R_ARM_YASOS_LOCAL_CALL:
    return 1;
  }
  return 0;
}

/* The unit is compiled (before tccelf_end_file merges its symbols): note
 * every function a loaded section refers to by anything but a branch -- a
 * pointer in data, a literal, MOVW/MOVT.  Those are what a call through a
 * pointer of their type may reach (`address-taken NAME [local]` in the .ci). */
ST_FUNC void tcc_stack_usage_unit_end(TCCState *s1)
{
  Section *symtab = symtab_section;
  if (!s1->callgraph_info || !symtab || !symtab->data)
    return;
  StackUsage *su = su_get(s1);
  const int nsyms = (int)(symtab->data_offset / sizeof(ElfW(Sym)));
  unsigned char *seen = tcc_mallocz(nsyms ? nsyms : 1);
  for (int i = 1; i < s1->nb_sections; i++)
  {
    Section *sec = s1->sections[i];
    Section *sr = sec ? sec->reloc : NULL;
    if (!sr || !sr->data || !(sec->sh_flags & SHF_ALLOC) || sec->sh_type == SHT_ARM_EXIDX ||
        !strcmp(sec->name, ".eh_frame"))
      continue;
    for (size_t off = 0; off + sizeof(ElfW_Rel) <= sr->data_offset; off += sizeof(ElfW_Rel))
    {
      const ElfW_Rel *rel = (const ElfW_Rel *)(sr->data + off);
      const int si = ELFW(R_SYM)(rel->r_info);
      if (si <= 0 || si >= nsyms || seen[si] || su_is_branch_reloc(ELFW(R_TYPE)(rel->r_info)))
        continue;
      const ElfW(Sym) *es = &((ElfW(Sym) *)symtab->data)[si];
      if (ELFW(ST_TYPE)(es->st_info) != STT_FUNC)
        continue;
      seen[si] = 1;
      const char *name = su_elf_name(si);
      if (name[0])
      {
        const int local = ELFW(ST_BIND)(es->st_info) == STB_LOCAL;
        char *t = tcc_malloc(strlen(name) + 8);
        sprintf(t, "%s%s", name, local ? " local" : "");
        dynarray_add(&su->taken, &su->nb_taken, t);
      }
    }
  }
  tcc_free(seen);
}

/* Write the unit's records next to OBJECT (its extension replaced by .su /
 * .ci) and forget them. */
ST_FUNC void tcc_stack_usage_write(TCCState *s1, const char *object)
{
  StackUsage *su = s1->su;
  const int n = su ? su->nb_recs : 0;
  if (s1->stack_usage)
  {
    FILE *fp = su_open(object, ".su");
    for (int i = 0; fp && i < n; i++)
    {
      SuRec *r = &su->recs[i];
      fprintf(fp, "%s:%d:%d:%s\t%u\t%s\n", r->file, r->line, r->col, r->name, r->bytes, su_qual[r->qual]);
    }
    if (fp)
      fclose(fp);
  }
  if (s1->callgraph_info)
  {
    FILE *fp = su_open(object, ".ci");
    if (fp)
      fprintf(fp, "# tcc callgraph-info 2\n");
    for (int i = 0; fp && i < n; i++)
    {
      SuRec *r = &su->recs[i];
      fprintf(fp, "function %s %s:%d %u %s%s\n", r->name, r->file, r->line, r->bytes, su_qual[r->qual],
              r->local ? " local" : "");
      fprintf(fp, "type %s\n", su_sig_name(su, r->sig));
      for (int k = 0; k < r->nb_callees; k++)
        fprintf(fp, "%s %s\n", r->callees[k].tail ? "tailcall" : "call", r->callees[k].name);
      for (int k = 0; k < r->nb_sigs; k++)
      {
        const TCCStackIndirect *ic = &r->by_sig[k];
        if (ic->calls)
          fprintf(fp, "indirect %d %s\n", ic->calls, su_sig_name(su, ic->sig));
        if (ic->tail)
          fprintf(fp, "indirect-tailcall %d %s\n", ic->tail, su_sig_name(su, ic->sig));
      }
    }
    for (int i = 0; fp && i < su->nb_taken; i++)
      fprintf(fp, "address-taken %s\n", su->taken[i]);
    if (fp)
      fclose(fp);
  }
  tcc_stack_usage_free(s1);
}
