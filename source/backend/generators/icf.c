/*
 *  TCC - Identical code folding
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* A generic function instantiated over types that lower the same way compiles
 * to the same machine code, once per instantiation: the Zig compiler's C has
 * 3,254 such bodies, 10.7% of its .text -- ten copies of one hash-map routine,
 * twenty-seven of one error path.  A body identical to one already emitted is
 * dropped here and its symbol points at the survivor, which is what gcc's
 * -fipa-icf does.
 *
 * The fold happens as the body is generated, while it is still the last thing
 * in the section, so nothing that exists already moves: the section and its
 * relocation table are rolled back to where this body began, exactly as
 * str_lit_pool_merge rolls back a duplicate string literal.  A call emitted
 * earlier reaches the function through a relocation on its symbol, which now
 * resolves to the survivor.
 *
 * Only a static function whose address nothing takes is folded, so no program
 * can observe that two functions became one address.  A function designator
 * decays to a pointer without `&`, so three things are asked: the `&` flag,
 * whether any relocation so far refers to the symbol other than by calling it,
 * and whether a body still to be generated names it anywhere but in a direct
 * call.  Only after the whole TU is parsed, where those answers are complete.
 */

#include "tcc.h"

/* A body generated in this TU, by fingerprint. */
typedef struct IcfEntry
{
  uint64_t hash;
  Section *sec;
  addr_t start, size;
  int nrel;          /* relocations the body carries */
  size_t rel_off;    /* where they begin in sec->reloc */
  addr_t value;      /* the symbol's value (thumb tag included) */
  int self_sym;      /* its own symbol, so a self-call matches a self-call */
  struct IcfEntry *next;
} IcfEntry;

/* Symbols some relocation refers to other than by calling them, and the
 * relocation sections scanned so far (relocations arrive as bodies are
 * generated, so each is looked at once). */
static uint8_t *icf_value_sym;
static int icf_value_sym_size;
static size_t *icf_scanned;
static int icf_scanned_size;
/* Identifiers a body still to be generated uses other than as a direct call. */
static uint8_t *icf_value_tok;
static int icf_value_tok_size;
static int icf_value_tok_done;

#define ICF_BUCKETS 4096
static IcfEntry *icf_table[ICF_BUCKETS];
static IcfEntry *icf_entries;
static int icf_nb_entries, icf_entries_size;

/* Code in `sec` moved (gen_late_reopt_functions erases a body before
 * re-emitting it), so every body recorded there is at an address that no
 * longer holds it: forget them.  Without this, the re-emitted body matches
 * the record of its own erased self. */
void tcc_icf_section_changed(Section *sec)
{
  for (int i = 0; i < icf_nb_entries; i++)
    if (icf_entries[i].sec == sec)
      icf_entries[i].sec = NULL;
}

void tcc_icf_reset(void)
{
  memset(icf_table, 0, sizeof(icf_table));
  tcc_free(icf_entries);
  icf_entries = NULL;
  icf_nb_entries = icf_entries_size = 0;
  tcc_free(icf_value_sym);
  icf_value_sym = NULL;
  icf_value_sym_size = 0;
  tcc_free(icf_scanned);
  icf_scanned = NULL;
  icf_scanned_size = 0;
  tcc_free(icf_value_tok);
  icf_value_tok = NULL;
  icf_value_tok_size = 0;
  icf_value_tok_done = 0;
}

/* A relocation that calls a function does not take its address; any other one
 * does (a pointer in a table, a pointer loaded into a register). */
static int icf_reloc_is_call(int type)
{
  switch (type)
  {
  case R_ARM_CALL:
  case R_ARM_JUMP24:
  case R_ARM_THM_PC22: /* the Thumb BL/BLX tcc emits for a call */
  case R_ARM_THM_JUMP24:
  case R_ARM_THM_JUMP19:
  case R_ARM_PLT32:
    return 1;
  default:
    return 0;
  }
}

/* Relocations added since the last look, by symbol. */
static void icf_scan_new_relocs(TCCState *s1)
{
  if (icf_scanned_size < s1->nb_sections)
  {
    icf_scanned = tcc_realloc(icf_scanned, sizeof(*icf_scanned) * s1->nb_sections);
    memset(icf_scanned + icf_scanned_size, 0, sizeof(*icf_scanned) * (s1->nb_sections - icf_scanned_size));
    icf_scanned_size = s1->nb_sections;
  }
  const int nsyms = s1->symtab ? (int)(s1->symtab->data_offset / sizeof(ElfW(Sym))) : 0;
  if (icf_value_sym_size < nsyms)
  {
    icf_value_sym = tcc_realloc(icf_value_sym, (size_t)nsyms);
    memset(icf_value_sym + icf_value_sym_size, 0, (size_t)(nsyms - icf_value_sym_size));
    icf_value_sym_size = nsyms;
  }
  for (int sn = 1; sn < s1->nb_sections; sn++)
  {
    Section *sr = s1->sections[sn];
    if (!sr || sr->sh_type != SHT_RELX || !sr->data)
      continue;
    for (size_t off = icf_scanned[sn]; off + sizeof(ElfW_Rel) <= sr->data_offset; off += sizeof(ElfW_Rel))
    {
      const ElfW_Rel *r = (const ElfW_Rel *)(sr->data + off);
      const int si = ELFW(R_SYM)(r->r_info);
      if (si > 0 && si < icf_value_sym_size && !icf_reloc_is_call(ELFW(R_TYPE)(r->r_info)))
        icf_value_sym[si] = 1;
    }
    icf_scanned[sn] = sr->data_offset;
  }
}

/* Identifiers the bodies still to be generated use other than as `name (`. */
static void icf_scan_pending_bodies(TCCState *s1)
{
  if (icf_value_tok_done)
    return;
  icf_value_tok_done = 1;
  icf_value_tok_size = tok_ident - TOK_IDENT + 1;
  if (icf_value_tok_size < 1)
    icf_value_tok_size = 1;
  icf_value_tok = tcc_mallocz((size_t)icf_value_tok_size);
  for (int pass = 0; pass < 2; pass++)
  {
    const int n = pass == 0 ? s1->nb_deferred_fns : s1->nb_inline_fns;
    for (int i = 0; i < n; i++)
    {
      TokenString *ts = pass == 0 ? s1->deferred_fns[i]->body : s1->inline_fns[i]->func_str;
      if (!ts)
        continue;
      const int *p = tok_str_buf(ts), *end = p + ts->len;
      int prev = 0, prev_is_ident = 0;
      while (p < end)
      {
        int t;
        CValue cv;
        tok_get(&t, &p, &cv);
        if (t == TOK_LINENUM)
          continue;
        if (prev_is_ident && t != '(')
        {
          const int idx = prev - TOK_IDENT;
          if (idx >= 0 && idx < icf_value_tok_size)
            icf_value_tok[idx] = 1;
        }
        prev_is_ident = t >= TOK_IDENT;
        prev = t;
      }
      if (prev_is_ident)
      {
        const int idx = prev - TOK_IDENT;
        if (idx >= 0 && idx < icf_value_tok_size)
          icf_value_tok[idx] = 1;
      }
    }
  }
}

static int icf_address_may_be_taken(TCCState *s1, Sym *sym)
{
  if (sym->a.addrtaken)
    return 1;
  icf_scan_new_relocs(s1);
  if (sym->c > 0 && sym->c < icf_value_sym_size && icf_value_sym[sym->c])
    return 1;
  icf_scan_pending_bodies(s1);
  const int idx = (sym->v & ~SYM_FIELD) - TOK_IDENT;
  if (idx >= 0 && idx < icf_value_tok_size && icf_value_tok[idx])
    return 1;
  return 0;
}

/* A branch's bytes hold the displacement to its target as it stood when the
 * instruction was written, so they differ between two identical bodies sitting
 * at different addresses and say nothing about sameness -- the relocation
 * entry names the target instead.  Every other relocation keeps its addend
 * there (ELF REL), which is part of what the body does: `return p;` and
 * `return g;` differ only in the addend on their section's symbol. */
static int icf_reloc_skips_bytes(int type)
{
  return icf_reloc_is_call(type);
}

static int icf_reloc_width(int type)
{
  (void)type;
  return 4; /* every ARM relocation tcc emits covers a word or a pair of halfwords */
}

static uint64_t icf_hash_body(Section *sec, addr_t start, addr_t size, const ElfW_Rel *rel, int nrel, int self_sym)
{
  uint64_t h = 1469598103934665603ull; /* FNV-1a */
#define ICF_MIX(b)                                                                                                     \
  do                                                                                                                   \
  {                                                                                                                    \
    h = (h ^ (unsigned char)(b)) * 1099511628211ull;                                                                   \
  } while (0)
  unsigned char *masked = tcc_malloc(size ? size : 1);
  memcpy(masked, sec->data + start, size);
  for (int i = 0; i < nrel; i++)
  {
    const int type = ELFW(R_TYPE)(rel[i].r_info);
    addr_t off = rel[i].r_offset - start;
    int w = icf_reloc_width(type);
    if (icf_reloc_skips_bytes(type) && off + w <= size)
      memset(masked + off, 0, w);
  }
  for (addr_t i = 0; i < size; i++)
    ICF_MIX(masked[i]);
  tcc_free(masked);
  ICF_MIX(nrel);
  for (int i = 0; i < nrel; i++)
  {
    const addr_t off = rel[i].r_offset - start;
    const int type = ELFW(R_TYPE)(rel[i].r_info);
    const int sym = ELFW(R_SYM)(rel[i].r_info);
    /* A body calling itself matches one calling itself, not one calling the
     * other: the reference is to whichever body it belongs to. */
    const int s = (sym == self_sym) ? -1 : sym;
    for (unsigned k = 0; k < sizeof(off); k++)
      ICF_MIX(off >> (8 * k));
    ICF_MIX(type);
    for (unsigned k = 0; k < sizeof(s); k++)
      ICF_MIX((unsigned)s >> (8 * k));
  }
#undef ICF_MIX
  return h;
}

/* Byte-for-byte, relocation-for-relocation, so a hash collision cannot fold
 * two different bodies. */
static int icf_same_body(Section *sec, addr_t a_start, addr_t size, const ElfW_Rel *a_rel, int a_nrel, int a_self,
                         const IcfEntry *b, int b_self)
{
  if (b->size != size || b->nrel != a_nrel)
    return 0;
  if (b->nrel && !(b->sec->reloc && b->sec->reloc->data))
    return 0;
  const ElfW_Rel *b_rel = (const ElfW_Rel *)(b->sec->reloc ? b->sec->reloc->data + b->rel_off : NULL);
  for (int i = 0; i < a_nrel; i++)
  {
    if (a_rel[i].r_offset - a_start != b_rel[i].r_offset - b->start)
      return 0;
    if (ELFW(R_TYPE)(a_rel[i].r_info) != ELFW(R_TYPE)(b_rel[i].r_info))
      return 0;
    int sa = ELFW(R_SYM)(a_rel[i].r_info), sb = ELFW(R_SYM)(b_rel[i].r_info);
    if (sa == a_self)
      sa = -1;
    if (sb == b_self)
      sb = -1;
    if (sa != sb)
      return 0;
  }
  /* Compare every byte but a branch's displacement. */
  addr_t pos = 0;
  for (int i = 0; i <= a_nrel; i++)
  {
    addr_t next = (i < a_nrel) ? a_rel[i].r_offset - a_start : size;
    if (next > size)
      next = size;
    if (next > pos && memcmp(sec->data + a_start + pos, b->sec->data + b->start + pos, next - pos) != 0)
      return 0;
    if (i < a_nrel)
    {
      const int type = ELFW(R_TYPE)(a_rel[i].r_info);
      addr_t end = next + icf_reloc_width(type);
      if (end > size)
        end = size;
      if (!icf_reloc_skips_bytes(type) && end > next &&
          memcmp(sec->data + a_start + next, b->sec->data + b->start + next, end - next) != 0)
        return 0;
      pos = end;
    }
  }
  return 1;
}

static void icf_record(uint64_t hash, Section *sec, addr_t start, addr_t size, size_t rel_off, int nrel, addr_t value,
                       int self_sym)
{
  if (icf_nb_entries >= icf_entries_size)
  {
    icf_entries_size = icf_entries_size ? icf_entries_size * 2 : 256;
    IcfEntry *grown = tcc_realloc(icf_entries, sizeof(*grown) * icf_entries_size);
    /* The table holds indices, not pointers, so a move is free. */
    icf_entries = grown;
    memset(icf_table, 0, sizeof(icf_table));
    for (int i = 0; i < icf_nb_entries; i++)
    {
      IcfEntry *e = &icf_entries[i];
      unsigned b = (unsigned)(e->hash % ICF_BUCKETS);
      e->next = icf_table[b];
      icf_table[b] = e;
    }
  }
  IcfEntry *e = &icf_entries[icf_nb_entries++];
  e->hash = hash;
  e->sec = sec;
  e->start = start;
  e->size = size;
  e->rel_off = rel_off;
  e->nrel = nrel;
  e->value = value;
  e->self_sym = self_sym;
  unsigned b = (unsigned)(hash % ICF_BUCKETS);
  e->next = icf_table[b];
  icf_table[b] = e;
}

/* A local symbol inside the dropped range ($t/$d mapping markers) would
 * otherwise mark the next function's code as data: objdump applies them by
 * address.  Nothing relocates against them, so retarget them to SHN_ABS
 * rather than delete, which would renumber the symtab. */
static void icf_drop_range_symbols(TCCState *s1, Section *sec, addr_t start, int keep_sym)
{
  Section *symtab = s1->symtab;
  if (!symtab || !symtab->data)
    return;
  int n = symtab->data_offset / sizeof(ElfW(Sym));
  ElfW(Sym) *syms = (ElfW(Sym) *)symtab->data;
  for (int i = 1; i < n; i++)
  {
    if (i == keep_sym || syms[i].st_shndx != sec->sh_num)
      continue;
    if ((syms[i].st_value & ~(addr_t)1) >= start)
      syms[i].st_shndx = SHN_ABS;
  }
}

/* Called with the body of `sym` occupying [start, start+size) at the end of
 * `sec`.  Returns 1 when it was dropped for an identical one, having pointed
 * the symbol at it; the caller then rewinds `ind`. */
int tcc_icf_try_fold(TCCState *s1, Sym *sym, Section *sec, addr_t start, addr_t size, size_t reloc_mark)
{
  if (!s1->tu_parsed || s1->optimize == 0 || s1->do_debug || s1->test_coverage || s1->do_backtrace)
    return 0;
  if (tcc_ir_opt_pass_disabled("icf"))
    return 0;
  if (!sym || !sec || !sec->data || size == 0 || start + size != sec->data_offset)
    return 0;
  /* Internal linkage and an address nothing takes: folding is unobservable. */
  if (!(sym->type.t & VT_STATIC) || sym->a.weak || sym->a.nested_func)
    return 0;
  if (icf_address_may_be_taken(s1, sym))
    return 0;
  if (sym->asm_label || !sym->c)
    return 0;
  ElfSym *esym = elfsym(sym);
  if (!esym || esym->st_shndx != sec->sh_num)
    return 0;

  const ElfW_Rel *rel = NULL;
  int nrel = 0;
  if (sec->reloc && sec->reloc->data)
  {
    if (reloc_mark > sec->reloc->data_offset)
      return 0;
    rel = (const ElfW_Rel *)(sec->reloc->data + reloc_mark);
    nrel = (int)((sec->reloc->data_offset - reloc_mark) / sizeof(ElfW_Rel));
    /* Relocations are appended as the body is generated; a body whose
     * relocations are not in address order is rare and not worth sorting. */
    for (int i = 0; i < nrel; i++)
      if (rel[i].r_offset < start || rel[i].r_offset >= start + size ||
          (i > 0 && rel[i].r_offset < rel[i - 1].r_offset))
        return 0;
  }

  const int self_sym = sym->c;
  const uint64_t hash = icf_hash_body(sec, start, size, rel, nrel, self_sym);
  for (IcfEntry *e = icf_table[hash % ICF_BUCKETS]; e; e = e->next)
  {
    if (e->hash != hash || !e->sec)
      continue;
    /* The survivor's own symbol index: its self-references were recorded
     * against it, so ask which symbol sits at its address. */
    int b_self = e->self_sym;
    if (!icf_same_body(sec, start, size, rel, nrel, self_sym, e, b_self))
      continue;

    /* Drop this body: the section and its relocations end where it began. */
    icf_drop_range_symbols(s1, sec, start, sym->c);
    if (sec->reloc)
      sec->reloc->data_offset = reloc_mark;
    sec->data_offset = start;
    esym->st_shndx = e->sec->sh_num;
    esym->st_value = e->value;
    esym->st_size = e->size;
    if (sym->type.ref)
      sym->type.ref->f.func_icf_folded = 1;
    return 1;
  }

  icf_record(hash, sec, start, size, reloc_mark, nrel, esym->st_value, self_sym);
  return 0;
}
