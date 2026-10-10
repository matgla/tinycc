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

/* emit.c -- Inline/late-reopt function body emission and the inline stash.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"
#include "tccdbgenv.h"

/* parse a function defined by symbol 'sym' and generate its code in
   'cur_text_section' */

/* Remove [start, start+size) bytes from `sec`, shifting trailing data
 * down by a multiple of `unit` (a power of two: the alignment the tail must
 * keep).  Updates symbol values and relocation offsets accordingly so
 * the section stays self-consistent.  Used by gen_late_reopt_functions
 * to reclaim the original code range of a function that is about to be
 * re-emitted, and by gc_unreferenced_statics.  Cross-section relocations resolve via symbol indices, so
 * only this section's own reloc table needs r_offset adjustment — except
 * debug info (DWARF), which records text PCs as section symbol +
 * IN-PLACE addend: those addends are rewritten here (shifted past the
 * erased range, tombstoned inside it), so the erase is safe under -g. */
static void erase_section_range(TCCState *s, Section *sec, addr_t start, addr_t size, addr_t unit)
{
  const int nobits = sec && sec->sh_type == SHT_NOBITS;
  if (size == 0 || !sec || (!sec->data && !nobits))
    return;
  if (start + size > sec->data_offset)
    return;

  /* Thumb literal pools embedded in the trailing functions are addressed by
   * PC-relative LDR (literal), which aligns PC down to 4 bytes.  The tail must
   * therefore keep its 4-byte alignment: shifting it by a non-multiple-of-4
   * amount would move each such pool 2 bytes off from where its LDR reads,
   * loading the wrong word (fuzz-exposed HardFault in gcc.c-torture 20180921-1
   * at -O1: a re-emitted function's odd-halfword size shifted `at()` and
   * misaligned its `&al` pool entry).  Function code is halfword-granular, so
   * `size` is even but may be 2 (mod 4); reclaim only a multiple-of-4 span and
   * leave up to 2 dead filler bytes so the tail's alignment is preserved. */
  addr_t shift = size & ~(unit - 1);
  if (shift == 0)
    return;

  /* What identical code folding recorded about this section describes
   * addresses that are about to move. */
  tcc_icf_section_changed(sec);

  /* Shift the section's tail data down (by `shift`, not `size`). */
  size_t tail_offset = (size_t)(start + size);
  size_t tail_len = sec->data_offset - tail_offset;
  if (tail_len > 0 && !nobits)
    memmove(sec->data + (tail_offset - shift), sec->data + tail_offset, tail_len);
  sec->data_offset -= shift;
  /* Zero the vacated tail.  The next thing appended here takes it as grown
   * section memory, and a string literal relies on that being zero for its
   * terminator: with the old bytes left in place, the second file of a
   * two-file compile (120_alias at -O1) got "in unit2:\n" ending in the tail
   * of the first file's format string instead of a NUL. */
  if (!nobits)
    memset(sec->data + sec->data_offset, 0, shift);

  /* Adjust symbols that point into this section. */
  Section *symtab = s->symtab; /* union alias of symtab_section */
  if (symtab && symtab->data)
  {
    int num_syms = symtab->data_offset / sizeof(ElfW(Sym));
    ElfW(Sym) *syms = (ElfW(Sym) *)symtab->data;
    for (int i = 0; i < num_syms; i++)
    {
      if (syms[i].st_shndx != sec->sh_num)
        continue;
      /* Thumb function symbols carry a LSB tag (st_value odd), so compare
       * after masking. */
      addr_t sv = syms[i].st_value & ~(addr_t)1;
      if (sv >= start + size)
      {
        syms[i].st_value -= shift;
      }
      else if (sv >= start)
      {
        /* Symbol within erased range.
         * - The func sym we're about to re-emit: re-emit's put_extern_sym
         *   will overwrite st_value with the new offset.
         * - $t/$d thumb mapping markers are NOT harmless left stale:
         *   objdump applies them by address, so a stale one marks shifted
         *   code as data (or data as code) and desyncs the whole
         *   disassembly — this is what made regression_disasm mis-count
         *   pr50310::foo by ~90.  Nothing relocates against them, so
         *   retarget them to SHN_ABS (ignored by ARM disassemblers)
         *   instead of deleting, which would renumber the symtab that
         *   relocations reference by index.
         * - Any other local here: leave as-is; setting st_shndx to
         *   SHN_UNDEF would break the link. */
        if (ELFW(ST_BIND)(syms[i].st_info) == STB_LOCAL && symtab->link && symtab->link->data)
        {
          const char *nm = (const char *)symtab->link->data + syms[i].st_name;
          if (nm[0] == '$' && (nm[1] == 't' || nm[1] == 'd') && nm[2] == 0)
            syms[i].st_shndx = SHN_ABS;
        }
      }
    }
  }

  /* Adjust this section's own relocations.  Pack-and-filter in one pass:
   * drop entries whose r_offset fell in the erased range. */
  if (sec->reloc && sec->reloc->data)
  {
    Section *sr = sec->reloc;
    int num_rels = sr->data_offset / sizeof(ElfW_Rel);
    ElfW_Rel *rels = (ElfW_Rel *)sr->data;
    int dst = 0;
    for (int i = 0; i < num_rels; i++)
    {
      addr_t off = rels[i].r_offset;
      if (off >= start + size)
      {
        rels[dst] = rels[i];
        rels[dst].r_offset = off - shift;
        dst++;
      }
      else if (off < start)
      {
        if (dst != i)
          rels[dst] = rels[i];
        dst++;
      }
      /* else: in erased range — drop */
    }
    sr->data_offset = (size_t)dst * sizeof(ElfW_Rel);
  }

  /* Debug (non-SHF_ALLOC) sections record text PCs as section symbol +
   * in-place addend; leaving those addends alone is why this compaction
   * used to be gated off under -g — which made every -g build ship both
   * bodies of every late-reopt function (~13.8% of the device tcc .text).
   * Walk every reloc section whose target is non-alloc: addends past the
   * erased range shift down with the tail; addends inside it are the
   * first compile's records (the erase runs before re-emit, so no new
   * records exist yet) and get retargeted to a local SHN_ABS tombstone.
   * The tombstone base is 0xFF000000, not the gc_sections 0xFFFFFFFF: a
   * dead .debug_line sequence keeps advancing by positive deltas from
   * its set_address, and from -1 those wrap past 0 into real code
   * addresses under -Ttext=0x0, which would poison addr2line.  From
   * 0xFF000000 (+ the record's intra-body offset, preserving sequence
   * monotonicity) nothing reachable is ever aliased.  Named symbols need
   * no addend work: their in-place addends are symbol-relative and the
   * symbol pass above already moved the symbols themselves. */
  int tomb = 0;
  if (symtab && symtab->data)
  {
    for (int sn = 1; sn < s->nb_sections; sn++)
    {
      Section *sr = s->sections[sn];
      if (!sr || sr->sh_type != SHT_RELX || !sr->data || sr->link != symtab)
        continue;
      if (sr->sh_info <= 0 || sr->sh_info >= s->nb_sections)
        continue;
      Section *ts = s->sections[sr->sh_info];
      if (!ts || !ts->data || (ts->sh_flags & SHF_ALLOC))
        continue;
      int num_rels = sr->data_offset / sizeof(ElfW_Rel);
      ElfW_Rel *rels = (ElfW_Rel *)sr->data;
      for (int i = 0; i < num_rels; i++)
      {
        int sym_index = ELFW(R_SYM)(rels[i].r_info);
        int rtype = ELFW(R_TYPE)(rels[i].r_info);
        ElfW(Sym) *syms = (ElfW(Sym) *)symtab->data;
        int num_syms = symtab->data_offset / sizeof(ElfW(Sym));
        if (rtype != R_DATA_32 || sym_index <= 0 || sym_index >= num_syms)
          continue;
        if (syms[sym_index].st_shndx != sec->sh_num ||
            ELFW(ST_TYPE)(syms[sym_index].st_info) != STT_SECTION)
          continue;
        if (rels[i].r_offset + 4 > ts->data_offset)
          continue;
        unsigned char *p = ts->data + rels[i].r_offset;
        addr_t a = read32le(p);
        if (a >= start + size)
        {
          write32le(p, a - shift);
        }
        else if (a >= start)
        {
          if (!tomb)
            tomb = put_elf_sym(symtab, 0xFF000000, 0,
                               ELFW(ST_INFO)(STB_LOCAL, STT_NOTYPE), 0,
                               SHN_ABS, NULL);
          rels[i].r_info = ELFW(R_INFO)(tomb, rtype);
          write32le(p, (uint32_t)(a - start));
        }
      }
    }
  }

  /* The DWARF line program is still buffered in tccdbg at this point (it
   * flushes in tcc_debug_end, after late reopt), so the walk above never
   * saw its set_address anchors — patch the buffered state too. */
  if (s->do_debug && s->dwarf && symtab && symtab->data)
  {
    if (!tomb)
      tomb = put_elf_sym(symtab, 0xFF000000, 0, ELFW(ST_INFO)(STB_LOCAL, STT_NOTYPE), 0, SHN_ABS, NULL);
    tcc_debug_line_erase_range(s, sec, start, size, shift, tomb);
  }
}

/* Code: the shift keeps the tail's 4-byte alignment (see above). */
static void erase_text_range(TCCState *s, Section *sec, addr_t start, addr_t size)
{
  erase_section_range(s, sec, start, size, 4);
}

/* End-of-TU re-optimization pass.  Functions whose IR contained a
 * would-be fold of a non-const `static` global blocked by the
 * VT_CONSTANT gate at first compile are marked func_late_reopt.  Now
 * that decl() has parsed the entire TU, possibly_written is final, and
 * we can re-run the optimizer with the gate bypassed.  Before re-emit,
 * the function's original code range is erased from .text (section
 * compaction with symbol/reloc fixups), so the final binary has no
 * orphan bytes from the first compile. */
void gen_late_reopt_functions(TCCState *s)
{
  int i;
  Sym *sym;
  struct InlineFunc *fn;

  if (s->nb_inline_fns == 0)
    return;

  /* Activate the bypass before the recompile loop. */
  s->ir_late_reopt_phase = 1;
  tcc_open_bf(s, ":late-reopt:", 0);

  /* Compaction rewrites .text data, symbol values, reloc offsets, and
   * (since erase_text_range learned to shift/tombstone the in-place
   * DWARF addends) is safe under -g too — the old gate here shipped
   * both bodies of every late-reopt function in -g builds.  Coverage
   * counters still record first-compile offsets nothing rewrites, so
   * that gate stays. */
  int do_compact = !s->test_coverage;
  int *aliases = NULL, nalias = 0, alias_cap = 0;

  for (i = 0; i < s->nb_inline_fns; ++i)
  {
    fn = s->inline_fns[i];
    sym = fn->sym;
    if (!sym || !sym->type.ref)
      continue;
    if (!sym->type.ref->f.func_late_reopt)
      continue;
    /* Its body is another function's now (tcc_icf_try_fold); re-emitting
     * would erase that one's range and orphan every other caller. */
    if (sym->type.ref->f.func_icf_folded)
      continue;
    /* Must still have saved tokens (the auto-inline post-emit path
     * preserves them when func_late_reopt is set). */
    if (!fn->func_str)
      continue;
    /* nested functions: token-replay cannot reproduce closure/static-chain
     * semantics.  Skip. */
    if (sym->a.nested_func)
      continue;

    /* Re-emit into the section the first compile placed the function in --
     * under -ffunction-sections that is its own .text.<name>, otherwise
     * .text.  Anything else (the old hardcoded text_section) would leave the
     * stale first body orphaned in the per-function section AND miss the
     * erase below, whose gate compares against the same section. */
    Section *late_reopt_sec = text_section;
    {
      ElfSym *esym = elfsym(sym);
      if (esym && esym->st_shndx < s->nb_sections)
      {
        Section *cand = s->sections[esym->st_shndx];
        if (cand && (cand->sh_flags & SHF_EXECINSTR))
          late_reopt_sec = cand;
      }
      /* Erase the original code range so re-emit doesn't leave dead bytes. */
      if (do_compact && esym && esym->st_shndx == late_reopt_sec->sh_num)
      {
        addr_t old_start = esym->st_value & ~(addr_t)1; /* drop thumb LSB tag */
        addr_t old_size = esym->st_size;
        /* Identical code folding may have pointed other statics at this body
         * (tcc_icf_try_fold): they alias it and follow it to where it is
         * re-emitted.  erase_text_range leaves a symbol inside the erased range
         * where it was -- on the range's filler and the code shifted down after
         * it -- and every call to the alias ran into the next function (Zig's
         * link.File.startProgress, folded onto an identical function that late
         * reopt re-emitted). */
        ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
        const int nsyms = symtab_section->data_offset / sizeof(ElfW(Sym));
        for (int k = 1; k < nsyms; k++)
          if (k != sym->c && syms[k].st_shndx == late_reopt_sec->sh_num &&
              ELFW(ST_TYPE)(syms[k].st_info) == STT_FUNC && (syms[k].st_value & ~(addr_t)1) == old_start)
          {
            if (nalias == alias_cap)
            {
              alias_cap = alias_cap ? alias_cap * 2 : 4;
              aliases = tcc_realloc(aliases, sizeof(int) * alias_cap);
            }
            aliases[nalias++] = k;
          }
        erase_text_range(s, late_reopt_sec, old_start, old_size);
      }
    }

    int body_len = fn->func_str->len;
    TokenString *compile_ts = tok_str_alloc();
    if (body_len > 0)
    {
      int *buf = tcc_malloc(body_len * sizeof(int));
      memcpy(buf, tok_str_buf(fn->func_str), body_len * sizeof(int));
      compile_ts->data.str = buf;
      compile_ts->allocated_len = body_len;
      compile_ts->len = body_len;
    }

    int saved_outer_tok = tok;
    CValue saved_outer_tokc = tokc;
    Section *saved_text = cur_text_section;
    cur_text_section = late_reopt_sec;

    int saved_pack[PACK_STACK_SIZE + 1];
    int saved_budget = called_once_budget_begin(body_len);
    tccpp_setfile(fn->filename);
    pp_pack_enter(s, fn->pack, saved_pack);
    begin_macro(compile_ts, 1);
    next();
    gen_function(sym);
    end_macro();
    pp_pack_leave(s, saved_pack);
    called_once_budget_end(saved_budget);
    if (nalias)
    {
      ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
      const ElfW(Sym) *body = &syms[sym->c];
      for (int k = 0; k < nalias; k++)
      {
        syms[aliases[k]].st_shndx = body->st_shndx;
        syms[aliases[k]].st_value = body->st_value;
        syms[aliases[k]].st_size = body->st_size;
      }
      nalias = 0;
    }

    tok = saved_outer_tok;
    tokc = saved_outer_tokc;
    cur_text_section = saved_text;

    /* Clear flag so subsequent passes (gen_inline_functions) see the
     * function as compiled — sym->c is already nonzero. */
    sym->type.ref->f.func_late_reopt = 0;
    /* Detach from the inline-fns list so gen_inline_functions doesn't
     * re-emit (it would compile via the sym->c truthy branch otherwise,
     * undoing our compaction and bumping the symbol forward again). */
    fn->sym = NULL;
    if (fn->func_str)
    {
      tok_str_free(fn->func_str);
      fn->func_str = NULL;
    }
  }

  tcc_free(aliases);
  tcc_close();
  s->ir_late_reopt_phase = 0;
}

/* Under -ffunction-sections, give each function its own .text.<name> input
 * section so the linker's gc_sections() can drop unreferenced bodies
 * individually.  Survivors are folded back into the single text_section
 * before GOT build and layout (coalesce_split_sections in tccelf_output.c), so
 * sbrel classification, the export table and the YAFF writer all still see
 * one text section.  Without the flag this is exactly the old behaviour. */
Section *function_text_section(TCCState *s1, Sym *sym)
{
  char buf[280];
  Section *sec;
  if (!s1->function_sections)
    return text_section;
  snprintf(buf, sizeof(buf), ".text.%s", get_tok_str(sym->v & ~SYM_FIELD, NULL));
  sec = find_section(s1, buf);
  /* find_section creates plain SHF_ALLOC PROGBITS; code needs EXECINSTR,
   * and thumb literal pools are PC-aligned-down-to-4 loads, so the section
   * must keep 4-byte alignment. */
  sec->sh_flags = text_section->sh_flags;
  if (sec->sh_addralign < 4)
    sec->sh_addralign = 4;
  return sec;
}

/* Generate the saved bodies the TU still owes: those referenced (sym->c) or
 * not internal.  With OWED_ONLY, just the `static inline` bodies referenced so
 * far that were never compiled at all -- see gen_owed_inline_functions. */
static void emit_inline_functions(TCCState *s, int owed_only)
{
  Sym *sym;
  int inline_generated, i;
  struct InlineFunc *fn;

  tcc_open_bf(s, ":inline:", 0);
  /* iterate while inline function are referenced */
  do
  {
    inline_generated = 0;
    for (i = 0; i < s->nb_inline_fns; ++i)
    {
      fn = s->inline_fns[i];
      sym = fn->sym;
      if (sym && (sym->type.t & VT_INLINE) && sym->type.ref && sym->type.ref->f.func_alwinl && !sym->a.addrtaken &&
          !sym->type.ref->f.func_outofline_needed)
        continue;
      if (sym && sym->type.ref && (sym->type.ref->f.func_auto_inline || sym->type.ref->f.func_eval_only_inline) &&
          !sym->type.ref->f.func_deferred_inline)
      {
        /* All auto-inline and eval-only-inline functions (static and
         * non-static) are compiled immediately at definition time.
         * Skip here — never re-emit.
         *
         * func_deferred_inline is the exception: those are `static inline`
         * bodies this function still owes the TU, so they must fall through
         * to the sym->c ("was actually referenced") test below.  A call site
         * can always decline — inline asm in the body, address taken, a
         * shadowed identifier — and then the real call needs a real symbol. */
        if (s->verbose >= 2)
          fprintf(stderr, "[auto-inline] gen_inline_functions: skipping %s (compiled at definition)\n",
                  get_tok_str(sym->v & ~SYM_FIELD, NULL));
        continue;
      }
      if (sym && sym->type.ref && sym->type.ref->f.func_keep_tokens_for_noreturn &&
          !sym->type.ref->f.func_late_reopt)
        continue;
      if (s->verbose >= 2)
        fprintf(stderr, "[gen_inline] sym=%s sym->c=%d VT_INLINE=%d addrtaken=%d auto_inline=%d\n",
                sym ? get_tok_str(sym->v & ~SYM_FIELD, NULL) : "<null>", sym ? sym->c : -1,
                sym ? !!(sym->type.t & VT_INLINE) : -1, (sym && sym->a.addrtaken) ? 1 : 0,
                (sym && sym->type.ref && sym->type.ref->f.func_auto_inline) ? 1 : 0);
      if (owed_only && (!sym || !(sym->type.t & VT_INLINE) || !sym->c || !elfsym(sym) ||
                        elfsym(sym)->st_shndx != SHN_UNDEF))
        continue;
      if (sym && (sym->c || !(sym->type.t & VT_INLINE)))
      {
        /* Skip original va_arg_pack functions - only their clones get compiled */
        if (sym->type.ref && sym->type.ref->f.func_va_arg_pack)
          continue;
        /* the function was used or forced (and then not internal):
           generate its code and convert it to a normal function */
        fn->sym = NULL;
        int saved_pack[PACK_STACK_SIZE + 1];
        int saved_budget = called_once_budget_begin(fn->func_str->len);
        tccpp_setfile(fn->filename);
        pp_pack_enter(s, fn->pack, saved_pack);
        begin_macro(fn->func_str, 1);
        next();
        cur_text_section = function_text_section(s, sym);
        gen_function(sym);
        end_macro();
        pp_pack_leave(s, saved_pack);
        called_once_budget_end(saved_budget);

        inline_generated = 1;
      }
    }
  } while (inline_generated);
  tcc_close();
}

void gen_inline_functions(TCCState *s)
{
  emit_inline_functions(s, 0);
}

/* The end-of-TU analyses read facts that parsing a body records: a static's
 * possibly_written/addrtaken (the late_reopt global_init fold) and its readers
 * and writers (tcc_ir_tu_analyze_dead_statics).  A `static inline` body a call
 * site did not expand -- address taken, or a call the inliner declined -- is
 * parsed only when gen_inline_functions emits it, which used to be after both:
 * a static written only there folded to its initializer, and a store read only
 * there was dropped as dead.  Emit those bodies first.  Whether one is owed is
 * already settled by sym->c; a body still undefined in the symbol table was
 * never compiled, unlike the kept-for-reopt entries sharing inline_fns. */
void gen_owed_inline_functions(TCCState *s)
{
  emit_inline_functions(s, 1);
}

void free_inline_functions(TCCState *s)
{
  int i;
  /* free tokens of unused inline functions */
  for (i = 0; i < s->nb_inline_fns; ++i)
  {
    struct InlineFunc *fn = s->inline_fns[i];
    if (fn->sym)
      tok_str_free(fn->func_str);
    tcc_free(fn->pack);
  }
  dynarray_reset(&s->inline_fns, &s->nb_inline_fns);
  tcc_free(s->inline_fn_by_tok);
  s->inline_fn_by_tok = NULL;
  s->inline_fn_by_tok_size = 0;
  s->inline_fn_by_tok_count = 0;
  s->inline_fn_indexed = 0;
}

/* A token-indexed array of InlineFunc pointers was as long as the highest
 * token holding a body -- most of the TU's identifiers, 16 KiB on a 64-bit
 * host for regcomp.c's 60-odd inline bodies.  A table keyed by token holds
 * the same mapping in a few slots per body. */
typedef struct InlineFnSlot
{
  int k;               /* symbol token - TOK_IDENT */
  struct InlineFunc *fn; /* NULL = empty slot */
} InlineFnSlot;

static InlineFnSlot *inline_fn_slot(InlineFnSlot *slots, int size, int k)
{
  unsigned mask = (unsigned)size - 1, h = ((unsigned)k * 2654435761u) & mask;
  while (slots[h].fn && slots[h].k != k)
    h = (h + 1) & mask;
  return &slots[h];
}

/* The inline_fns entry holding `sym`'s body, or NULL.  Entries are indexed by
 * symbol token as they are appended; a call site used to scan the whole list,
 * which with every function body deferred to the end of the TU made each call
 * O(functions). */
InlineFunc *inline_fn_lookup(TCCState *s, Sym *sym)
{
  for (; s->inline_fn_indexed < s->nb_inline_fns; s->inline_fn_indexed++)
  {
    InlineFunc *fn = s->inline_fns[s->inline_fn_indexed];
    if (!fn || !fn->sym)
      continue;
    int k = (fn->sym->v & ~SYM_FIELD) - TOK_IDENT;
    if (k < 0)
      continue;
    if ((s->inline_fn_by_tok_count + 1) * 2 > s->inline_fn_by_tok_size)
    {
      /* at most half full, so a probe ends quickly at an empty slot */
      int n = s->inline_fn_by_tok_size ? s->inline_fn_by_tok_size * 2 : 64;
      InlineFnSlot *grown = tcc_mallocz(sizeof(InlineFnSlot) * n);
      for (int i = 0; i < s->inline_fn_by_tok_size; i++)
        if (s->inline_fn_by_tok[i].fn)
          *inline_fn_slot(grown, n, s->inline_fn_by_tok[i].k) = s->inline_fn_by_tok[i];
      tcc_free(s->inline_fn_by_tok);
      s->inline_fn_by_tok = grown;
      s->inline_fn_by_tok_size = n;
    }
    /* First entry for a symbol wins, as the linear scan it replaces did. */
    InlineFnSlot *slot = inline_fn_slot(s->inline_fn_by_tok, s->inline_fn_by_tok_size, k);
    if (!slot->fn)
    {
      slot->k = k;
      slot->fn = fn;
      s->inline_fn_by_tok_count++;
    }
    else if (slot->fn->sym != fn->sym)
      slot->fn = fn;
  }
  int k = (sym->v & ~SYM_FIELD) - TOK_IDENT;
  if (k < 0 || !s->inline_fn_by_tok_size)
    return NULL;
  InlineFunc *fn = inline_fn_slot(s->inline_fn_by_tok, s->inline_fn_by_tok_size, k)->fn;
  if (!fn)
    return NULL;
  if (fn->sym == sym)
    return fn;
  /* The indexed entry was consumed, or belongs to another symbol of the same
   * name (a nested function): fall back to the scan. */
  for (int i = 0; i < s->nb_inline_fns; i++)
    if (s->inline_fns[i]->sym == sym)
      return s->inline_fns[i];
  return NULL;
}

/* ---- -finline-functions-called-once ------------------------------------
 *
 * Every static function body was saved at its definition (decl.c
 * defer_function_body) instead of generated.  With all of them known, each
 * function's references are counted over the saved bodies: a static function
 * with exactly one call, no other mention and nothing already referencing it
 * (sym->c: code or data emitted before the end of the TU) is inlined at that
 * one call site whatever its own size, as long as the caller stays within its
 * growth budget (called_once_budget_begin).  It becomes a deferred inline body, which
 * gen_inline_functions emits only if some reference survives -- a call site
 * that declined to inline it -- so declining is always safe.  The rest are
 * generated here, callees before callers, so the size-gated auto-inliner also
 * sees every small callee's body before its callers are compiled. */

/* Whether BODY uses alloca or __builtin_frame_address.  alloca memory lives
 * until the function returns: expanded into a caller, a block freed at each
 * return of the callee would pile up in the caller's frame -- every
 * iteration of a loop around the call -- which is why gcc does not inline
 * such functions either.  The frame address would be the caller's.  Both
 * also force a frame pointer on the caller, which loses r7 to allocation. */
static int body_uses_frame_builtins(TokenString *body)
{
  const int *p = tok_str_buf(body), *end = p + body->len;
  while (p < end)
  {
    int t;
    CValue cv;
    tok_get(&t, &p, &cv);
    if (t == TOK_builtin_frame_address || t == TOK_alloca || t == TOK_builtin_alloca)
      return 1;
  }
  return 0;
}

/* Can the token-replay inliner express a called-once body?  (Constructs it
 * cannot reproduce, or whose meaning would change in the caller -- not a
 * size or benefit judgement.) */
/* gcc's large-stack-frame default: a callee with a local array this big stays
 * a call (inline_body_has_large_local_array). */
#define CALLED_ONCE_LARGE_FRAME_ARRAY 256

static int called_once_body_ok(Sym *sym, TokenString *body)
{
  Sym *ref = sym->type.ref;
  if (!ref || ref->f.func_noinline || ref->f.func_ctor || ref->f.func_dtor || sym->a.weak ||
      (ref->type.t & (VT_COMPLEX | VT_VECTOR)))
    return 0;
  for (Sym *p = ref->next; p; p = p->next)
    if (p->type.t & (VT_COMPLEX | VT_VECTOR | VT_VLA))
      return 0;
  int addr_of_label = 0, inline_asm = 0;
  inline_scan_body_features(body, &addr_of_label, &inline_asm);
  if (addr_of_label || inline_asm || inline_body_has_static_local(body) || inline_body_has_apply_args(body))
    return 0;
  /* A big local array would sit in the caller's frame on all its paths. */
  if (inline_body_has_large_local_array(body, CALLED_ONCE_LARGE_FRAME_ARRAY))
    return 0;
  if ((ref->type.t & VT_BTYPE) != VT_VOID && !inline_body_has_return_stmt(body))
    return 0;
  if (body_uses_frame_builtins(body))
    return 0;
  return 1;
}

/* How much a function may grow by expanding called-once functions, in saved
 * body ints: up to CALLED_ONCE_LARGE_BODY in all, or to twice its own length
 * when it is already larger -- gcc's large-function-insns/-growth rule for
 * the same transform.  Every called-once callee is a size win on its own, but
 * a dispatcher calling hundreds of them (zig's Sema.analyzeBodyInner) became
 * one 200,000-instruction function: register allocation there costs more
 * than all the calls it saves, and no device has the memory to compile it.
 * A callee that does not fit stays a call; its body is emitted standalone.
 * Returns the previous budget, for called_once_budget_end. */
/* 16000: zig.c -O2 .text 4,295,178 at 8000 -> 4,283,054; 12000 -4.5 KB, 20000+ back
 * to +4 KB, and a growth factor of 3 or 4 +35..+59 KB -- the losses sit in
 * callers that were already large, where every value the merged body keeps
 * costs a spill and a frame past the 1020-byte short-encoding reach. */
#define CALLED_ONCE_LARGE_BODY 16000

int called_once_budget_begin(int own_len)
{
  int saved = tcc_state->called_once_budget;
  tcc_state->inline_caller_len = own_len;
  tcc_state->inline_caller_gen++;
  /* TCC_CALLED_ONCE_LARGE / TCC_CALLED_ONCE_GROWTH: the two limits, for experiments. */
  static int large = -1, growth = -1;
  if (large < 0)
  {
    const char *e = getenv("TCC_CALLED_ONCE_LARGE");
    large = e ? atoi(e) : CALLED_ONCE_LARGE_BODY;
    e = getenv("TCC_CALLED_ONCE_GROWTH");
    growth = e ? atoi(e) : 2;
  }
  int limit = own_len * growth > large ? own_len * growth : large;
  tcc_state->called_once_budget = limit - own_len;
  return saved;
}

void called_once_budget_end(int saved)
{
  tcc_state->called_once_budget = saved;
}

/* Scratch arrays of gen_deferred_function_bodies, kept where
 * free_deferred_functions can release them: a compile error inside a
 * deferred body longjmps out past the normal frees. */
static void **deferred_scratch;
static int nb_deferred_scratch;

static void *deferred_alloc(size_t size)
{
  void *p = tcc_mallocz(size ? size : 1);
  dynarray_add(&deferred_scratch, &nb_deferred_scratch, p);
  return p;
}

/* Free a deferred_alloc block before the end: the analysis arrays die before
 * the bodies are generated, and kept until then they sat under every
 * function's IR at the peak of an -O2 compile. */
static void deferred_release(void *p)
{
  for (int i = 0; i < nb_deferred_scratch; i++)
    if (deferred_scratch[i] == p)
    {
      deferred_scratch[i] = deferred_scratch[--nb_deferred_scratch];
      tcc_free(p);
      return;
    }
}

/* Release the deferred bodies and the scratch; tccgen_finish calls it on
 * every exit, including an error's. */
void free_deferred_functions(TCCState *s)
{
  for (int i = 0; i < s->nb_deferred_fns; i++)
  {
    if (s->deferred_fns[i]->body)
      tok_str_free(s->deferred_fns[i]->body);
    tcc_free(s->deferred_fns[i]->pack);
  }
  dynarray_reset(&s->deferred_fns, &s->nb_deferred_fns); /* frees the entries too */
  for (int i = 0; i < s->nb_deferred_data; i++)
    if (s->deferred_data[i]->init)
      tok_str_free(s->deferred_data[i]->init);
  dynarray_reset(&s->deferred_data, &s->nb_deferred_data);
  for (int i = 0; i < nb_deferred_scratch; i++)
    tcc_free(deferred_scratch[i]);
  tcc_free(deferred_scratch);
  deferred_scratch = NULL;
  nb_deferred_scratch = 0;
}

/* ---- -fdrop-unused-statics -------------------------------------------
 *
 * At the end of parsing, a static function or initialized static object
 * nothing live refers to is dropped, as gcc does at -O1: generated C
 * (zig's) defines every helper and table it might need, and a program keeps
 * few of them.  The candidates are the saved bodies (defer_function_body),
 * the saved initializers (defer_static_data), the static inline bodies
 * waiting to be emitted, and the static tentative definitions.  Roots are
 * what must be emitted anyway: a non-static function, one marked used,
 * weak, constructor/destructor or placed in a section, an alias target, and
 * any candidate a relocation of already emitted code or data refers to.
 * Liveness then flows along identifier mentions in the saved tokens -- a
 * name is a reference however it is used (called, address taken, sizeof),
 * which can only keep too much.  The live objects are defined here, in
 * declaration order, before the tentative definitions are settled and before
 * any body is generated, so every fold of an initializer sees its bytes. */

static void tombstone_unused_static(Sym *sym)
{
  sym->a.tu_unused = 1;
  ElfSym *esym = sym->c ? elfsym(sym) : NULL;
  if (esym && (esym->st_shndx == SHN_UNDEF || esym->st_shndx == SHN_COMMON))
  {
    esym->st_shndx = SHN_ABS;
    esym->st_value = 0;
    esym->st_size = 0;
  }
}

/* A scratch section for what a check-only parse lays out (check_only in
 * tcc.h): never registered with the TU, so nothing in it is output. */
static void check_scratch_begin(TCCState *s)
{
  if (s->check_scratch)
    return;
  s->check_scratch = tcc_mallocz(sizeof(Section) + 16);
  s->check_scratch->s1 = s;
  s->check_scratch->sh_type = SHT_PROGBITS;
  s->check_scratch->sh_addralign = 1;
}

/* TCC_CHECK_ALL_STATICS: also run the check-only parse over every live
 * deferred body and initializer, just before it is generated -- a stress
 * mode that puts check_only through the whole test corpus. */
TCC_DBG_ENV_FLAG(check_every_static, "TCC_CHECK_ALL_STATICS")

void prune_unused_statics(TCCState *s)
{
  const int nf = s->nb_deferred_fns, nd = s->nb_deferred_data;
  if (!TCC_OPT(s, opt_drop_unused_statics) || (!nf && !nd))
    goto define_all;

  /* Nodes: deferred functions, deferred objects, static inline bodies,
   * static tentative definitions -- in that order. */
  const int ni = s->nb_inline_fns, nt = s->nb_tentative_syms;
  const int cap = nf + nd + ni + nt;
  Sym **node_sym = deferred_alloc(sizeof(Sym *) * cap);
  TokenString **node_toks = deferred_alloc(sizeof(TokenString *) * cap);
  uint8_t *root = deferred_alloc(cap), *live = deferred_alloc(cap);
  int n = 0;
  for (int i = 0; i < nf; i++)
  {
    DeferredFunc *d = s->deferred_fns[i];
    Sym *sym = d->sym;
    node_sym[n] = sym;
    node_toks[n] = d->body;
    root[n] = !(sym->type.t & VT_STATIC) || sym->a.used || sym->a.weak || d->section ||
              (sym->type.ref && (sym->type.ref->f.func_ctor || sym->type.ref->f.func_dtor));
    n++;
  }
  for (int i = 0; i < nd; i++)
  {
    node_sym[n] = s->deferred_data[i]->sym;
    node_toks[n] = s->deferred_data[i]->init;
    root[n] = node_sym[n]->a.used;
    n++;
  }
  for (int i = 0; i < ni; i++)
  {
    InlineFunc *fn = s->inline_fns[i];
    if (!fn || !fn->sym || !fn->func_str)
      continue;
    Sym *sym = fn->sym;
    ElfSym *esym = sym->c ? elfsym(sym) : NULL;
    node_sym[n] = sym;
    node_toks[n] = fn->func_str;
    /* Already compiled (a body kept for re-optimization): what it refers
     * to stays referred to. */
    root[n] = !(sym->type.t & VT_STATIC) || sym->a.used || (esym && esym->st_shndx != SHN_UNDEF);
    n++;
  }
  const int tent_base = n;
  for (int i = 0; i < nt; i++)
  {
    Sym *sym = s->tentative_syms[i];
    if (!(sym->type.t & VT_STATIC) || !sym->a.tentative)
      continue;
    node_sym[n] = sym;
    node_toks[n] = NULL;
    root[n] = sym->a.used;
    n++;
  }

  /* Names -> nodes (a chain per name; an object with a deferred definition
   * is also a tentative node, harmlessly). */
  const int ntok = tok_ident - TOK_IDENT;
  int *head = deferred_alloc(sizeof(int) * (ntok > 0 ? ntok : 1));
  int *next_same = deferred_alloc(sizeof(int) * (n ? n : 1));
  for (int k = 0; k < ntok; k++)
    head[k] = -1;
  for (int i = 0; i < n; i++)
  {
    int k = (node_sym[i]->v & ~SYM_FIELD) - TOK_IDENT;
    next_same[i] = -1;
    if (k >= 0 && k < ntok)
    {
      next_same[i] = head[k];
      head[k] = i;
    }
  }

  /* Relocations of what is already emitted, and alias targets. */
  const int nelf = symtab_section->data_offset / sizeof(ElfW(Sym));
  int *by_elf = deferred_alloc(sizeof(int) * (nelf ? nelf : 1));
  for (int e = 0; e < nelf; e++)
    by_elf[e] = -1;
  for (int i = 0; i < n; i++)
    if (node_sym[i]->c > 0 && node_sym[i]->c < nelf)
      by_elf[node_sym[i]->c] = i;
  for (int sh = 1; sh < s->nb_sections; sh++)
  {
    Section *sec = s->sections[sh];
    if (!sec || !(sec->sh_flags & SHF_ALLOC) || !sec->reloc)
      continue;
    ElfW_Rel *rel = (ElfW_Rel *)sec->reloc->data, *rel_end = (ElfW_Rel *)(sec->reloc->data + sec->reloc->data_offset);
    for (; rel < rel_end; rel++)
    {
      int e = ELFW(R_SYM)(rel->r_info);
      if (e > 0 && e < nelf && by_elf[e] >= 0)
        root[by_elf[e]] = 1;
    }
  }
  mark_pending_alias_targets_used();
  for (int i = 0; i < n; i++)
    if (node_sym[i]->a.used)
      root[i] = 1;

  /* Mentions, breadth first from the roots.  A name after '.' or '->' is a
   * member, not a reference. */
  int *work = deferred_alloc(sizeof(int) * (n ? n : 1)), nw = 0;
  for (int i = 0; i < n; i++)
    if (root[i])
      live[i] = 1, work[nw++] = i;
  while (nw)
  {
    TokenString *str = node_toks[work[--nw]];
    if (!str)
      continue;
    const int *p = tok_str_buf(str), *end = p + str->len;
    int prev = 0;
    while (p < end)
    {
      int t;
      CValue cv;
      tok_get(&t, &p, &cv);
      if (t >= TOK_IDENT && t - TOK_IDENT < ntok && prev != '.' && prev != TOK_ARROW)
        for (int j = head[t - TOK_IDENT]; j >= 0; j = next_same[j])
          if (!live[j])
            live[j] = 1, work[nw++] = j;
      if (t != TOK_LINENUM)
        prev = t;
    }
  }

  /* A symbol reached under any of its nodes is live under all of them. */
  for (int i = 0; i < n; i++)
  {
    int k = (node_sym[i]->v & ~SYM_FIELD) - TOK_IDENT;
    if (live[i] && k >= 0 && k < ntok)
      for (int j = head[k]; j >= 0; j = next_same[j])
        if (node_sym[j] == node_sym[i])
          live[j] = 1;
  }

  /* The dropped ones move to their own lists: check_dropped_statics parses
   * them for their diagnostics once everything live is generated. */
  int kept = 0;
  for (int i = 0; i < nf; i++)
  {
    DeferredFunc *d = s->deferred_fns[i];
    if (live[i])
      s->deferred_fns[kept++] = d;
    else
    {
      dynarray_add(&s->dropped_fns, &s->nb_dropped_fns, d);
      tombstone_unused_static(d->sym);
    }
  }
  s->nb_deferred_fns = kept;
  kept = 0;
  for (int i = 0; i < nd; i++)
  {
    DeferredData *d = s->deferred_data[i];
    if (live[nf + i])
      s->deferred_data[kept++] = d;
    else
    {
      dynarray_add(&s->dropped_data, &s->nb_dropped_data, d);
      tombstone_unused_static(d->sym);
    }
  }
  s->nb_deferred_data = kept;
  for (int i = tent_base; i < n; i++)
    if (!live[i])
      tombstone_unused_static(node_sym[i]);
  /* The graph is done with: free it now rather than under every body
   * gen_deferred_function_bodies generates. */
  deferred_release(node_sym);
  deferred_release(node_toks);
  deferred_release(root);
  deferred_release(live);
  deferred_release(head);
  deferred_release(next_same);
  deferred_release(by_elf);
  deferred_release(work);

define_all:
  if (s->nb_deferred_data)
  {
    const int saved_nocode_wanted = nocode_wanted;
    nocode_wanted = DATA_ONLY_WANTED;
    tcc_open_bf(s, ":deferred:", 0);
    for (int i = 0; i < s->nb_deferred_data; i++)
    {
      DeferredData *d = s->deferred_data[i];
      if (check_every_static())
      {
        check_scratch_begin(s);
        check_dropped_deferred_data(d, tok_str_clone(d->init));
      }
      define_deferred_data(d);
    }
    tcc_close();
    nocode_wanted = saved_nocode_wanted;
  }
}

/* ---- end-of-TU garbage collection ------------------------------------
 *
 * prune_unused_statics decides from the source; what the optimizer does
 * afterwards can leave more behind: a static function whose every call was
 * inlined, an object whose stores dead_static_store removed, and whatever
 * only those referred to.  Here the generated object itself is collected,
 * the way --gc-sections would at link time but per static: each local
 * function or object symbol is a node (symbols whose ranges overlap -- ICF
 * aliases -- share one), relocations are the edges, and the roots are
 * everything else: code or data outside any node, global and weak symbols,
 * statics marked used or aliased.  A relocation against a section symbol
 * names no node, so it pins its whole target section.  The unreachable
 * nodes are erased with erase_section_range, which keeps symbols,
 * relocations and debug addends consistent, their symbols tombstoned.  An
 * undefined global nothing refers to any more -- its callers erased here,
 * or rewritten by dead_static_store or late reopt -- becomes weak, so the
 * link does not go looking for what nothing uses. */

typedef struct GcNode
{
  int sec;
  addr_t start, end;
} GcNode;

static int gc_node_cmp(const void *a, const void *b)
{
  const GcNode *x = a, *y = b;
  if (x->sec != y->sec)
    return x->sec < y->sec ? -1 : 1;
  if (x->start != y->start)
    return x->start < y->start ? -1 : 1;
  return (x->end > y->end) - (x->end < y->end);
}

/* The node of section `sec` containing `addr`, or -1.  Nodes are sorted and
 * disjoint; sec_first/sec_count index each section's run. */
static int gc_node_at(const GcNode *nodes, const int *sec_first, const int *sec_count, int sec, addr_t addr)
{
  int lo = sec_first[sec], hi = lo + sec_count[sec] - 1;
  while (lo <= hi)
  {
    int mid = (lo + hi) / 2;
    if (addr < nodes[mid].start)
      hi = mid - 1;
    else if (addr >= nodes[mid].end)
      lo = mid + 1;
    else
      return mid;
  }
  return -1;
}

void gc_unreferenced_statics(TCCState *s)
{
  if (!TCC_OPT(s, opt_drop_unused_statics) || s->test_coverage || !symtab_section || !symtab_section->data)
    return;
  const int nsyms = symtab_section->data_offset / sizeof(ElfW(Sym));
  const int nsec = s->nb_sections;
  ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;

  /* Candidate ranges: local functions and objects with a size, in an
   * allocated section. */
  GcNode *nodes = tcc_malloc(sizeof(GcNode) * (nsyms ? nsyms : 1));
  int nn = 0;
  for (int i = 1; i < nsyms; i++)
  {
    ElfW(Sym) *e = &syms[i];
    int type = ELFW(ST_TYPE)(e->st_info);
    if (ELFW(ST_BIND)(e->st_info) != STB_LOCAL || (type != STT_FUNC && type != STT_OBJECT) || !e->st_size)
      continue;
    if (e->st_shndx == SHN_UNDEF || e->st_shndx >= nsec || !s->sections[e->st_shndx] ||
        !(s->sections[e->st_shndx]->sh_flags & SHF_ALLOC))
      continue;
    addr_t start = e->st_value & ~(addr_t)1; /* Thumb bit */
    nodes[nn].sec = e->st_shndx;
    nodes[nn].start = start;
    nodes[nn].end = start + e->st_size;
    nn++;
  }
  if (!nn)
  {
    tcc_free(nodes);
    return;
  }
  tcc_qsort(nodes, nn, sizeof *nodes, gc_node_cmp);
  /* Merge overlapping ranges (ICF aliases, a symbol inside another). */
  int m = 0;
  for (int i = 0; i < nn; i++)
  {
    if (m && nodes[m - 1].sec == nodes[i].sec && nodes[i].start < nodes[m - 1].end)
    {
      if (nodes[i].end > nodes[m - 1].end)
        nodes[m - 1].end = nodes[i].end;
    }
    else
      nodes[m++] = nodes[i];
  }
  nn = m;
  int *sec_first = tcc_mallocz(sizeof(int) * nsec), *sec_count = tcc_mallocz(sizeof(int) * nsec);
  for (int i = nn - 1; i >= 0; i--)
    sec_first[nodes[i].sec] = i, sec_count[nodes[i].sec]++;

  uint8_t *live = tcc_mallocz(nn), *pinned = tcc_mallocz(nsec);
  int *edge_count = tcc_mallocz(sizeof(int) * (nn + 1));
  /* A relocation: from the node containing its offset (or a root, -1) to
   * the node containing its target (or nothing, -1). */
  int nrel_total = 0;
  for (int sh = 1; sh < nsec; sh++)
  {
    Section *sec = s->sections[sh];
    if (sec && (sec->sh_flags & SHF_ALLOC) && sec->reloc)
      nrel_total += sec->reloc->data_offset / sizeof(ElfW_Rel);
  }
  int *rel_from = tcc_malloc(sizeof(int) * (nrel_total ? nrel_total : 1));
  int *rel_to = tcc_malloc(sizeof(int) * (nrel_total ? nrel_total : 1));
  int nr = 0;
  for (int sh = 1; sh < nsec; sh++)
  {
    Section *sec = s->sections[sh];
    if (!sec || !(sec->sh_flags & SHF_ALLOC) || !sec->reloc)
      continue;
    ElfW_Rel *rel = (ElfW_Rel *)sec->reloc->data, *rel_end = (ElfW_Rel *)(sec->reloc->data + sec->reloc->data_offset);
    for (; rel < rel_end; rel++)
    {
      int t = ELFW(R_SYM)(rel->r_info);
      if (t <= 0 || t >= nsyms)
        continue;
      ElfW(Sym) *e = &syms[t];
      if (e->st_shndx == SHN_UNDEF || e->st_shndx >= nsec)
        continue;
      if (ELFW(ST_TYPE)(e->st_info) == STT_SECTION)
      {
        pinned[e->st_shndx] = 1;
        continue;
      }
      int to = gc_node_at(nodes, sec_first, sec_count, e->st_shndx, e->st_value & ~(addr_t)1);
      if (to < 0)
        continue;
      int from = gc_node_at(nodes, sec_first, sec_count, sh, rel->r_offset);
      rel_from[nr] = from;
      rel_to[nr] = to;
      nr++;
      if (from >= 0)
        edge_count[from]++;
    }
  }
  /* Edges by source node (CSR). */
  int *edge_start = tcc_mallocz(sizeof(int) * (nn + 1));
  for (int i = 0; i < nn; i++)
    edge_start[i + 1] = edge_start[i] + edge_count[i];
  int *edges = tcc_malloc(sizeof(int) * (edge_start[nn] ? edge_start[nn] : 1));
  memset(edge_count, 0, sizeof(int) * (nn + 1));
  int *work = tcc_malloc(sizeof(int) * nn), nw = 0;
#define GC_MARK(n)                                                                                                     \
  do                                                                                                                   \
  {                                                                                                                    \
    int gc_n_ = (n);                                                                                                   \
    if (gc_n_ >= 0 && !live[gc_n_])                                                                                    \
      live[gc_n_] = 1, work[nw++] = gc_n_;                                                                             \
  } while (0)
  for (int r = 0; r < nr; r++)
  {
    if (rel_from[r] < 0)
      GC_MARK(rel_to[r]);
    else
      edges[edge_start[rel_from[r]] + edge_count[rel_from[r]]++] = rel_to[r];
  }
  /* Roots: pinned sections, global and weak symbols, used statics. */
  for (int i = 0; i < nn; i++)
    if (pinned[nodes[i].sec])
      GC_MARK(i);
  for (int i = 1; i < nsyms; i++)
  {
    ElfW(Sym) *e = &syms[i];
    if (ELFW(ST_BIND)(e->st_info) != STB_LOCAL && e->st_shndx != SHN_UNDEF && e->st_shndx < nsec)
      GC_MARK(gc_node_at(nodes, sec_first, sec_count, e->st_shndx, e->st_value & ~(addr_t)1));
  }
  for (Sym *g = global_stack; g; g = g->prev)
    if (g->a.used && g->c > 0 && g->c < nsyms && syms[g->c].st_shndx < nsec)
      GC_MARK(gc_node_at(nodes, sec_first, sec_count, syms[g->c].st_shndx, syms[g->c].st_value & ~(addr_t)1));
  while (nw)
  {
    int n = work[--nw];
    for (int k = edge_start[n]; k < edge_start[n + 1]; k++)
      GC_MARK(edges[k]);
  }
#undef GC_MARK

  /* Erase the dead nodes, highest address first so the lower ones stay put.
   * Their symbols go first: erase_section_range leaves symbols inside the
   * range where they were. */
  for (int i = nn - 1; i >= 0; i--)
  {
    if (live[i])
      continue;
    Section *sec = s->sections[nodes[i].sec];
    for (int k = 1; k < nsyms; k++)
    {
      ElfW(Sym) *e = &syms[k];
      int type = ELFW(ST_TYPE)(e->st_info);
      if (e->st_shndx != nodes[i].sec || type == STT_SECTION || type == STT_FILE)
        continue;
      addr_t v = e->st_value & ~(addr_t)1;
      if (v < nodes[i].start || v >= nodes[i].end)
        continue;
      const char *nm = symtab_section->link ? (const char *)symtab_section->link->data + e->st_name : "";
      if (nm[0] == '$' && (nm[1] == 't' || nm[1] == 'd') && nm[2] == 0)
        continue; /* mapping markers: erase_section_range retargets them */
      e->st_shndx = SHN_ABS;
      e->st_value = 0;
      e->st_size = 0;
    }
    addr_t unit = sec->sh_addralign > 0 ? sec->sh_addralign : 1;
    if ((sec->sh_flags & SHF_EXECINSTR) && unit < 4)
      unit = 4;
    erase_section_range(s, sec, nodes[i].start, nodes[i].end - nodes[i].start, unit);
    /* Under -g the erase adds a tombstone symbol (put_elf_sym), which can
     * reallocate the table.  Symbols added after nsyms was taken are never
     * candidates, so only the base pointer needs refreshing. */
    syms = (ElfW(Sym) *)symtab_section->data;
  }

  /* Undefined globals nothing refers to any more. */
  int *refs_after = tcc_mallocz(sizeof(int) * nsyms);
  for (int sh = 1; sh < nsec; sh++)
  {
    Section *sr = s->sections[sh];
    if (!sr || sr->sh_type != SHT_RELX || sr->link != symtab_section || !sr->data)
      continue;
    for (ElfW_Rel *rel = (ElfW_Rel *)sr->data; (unsigned char *)rel < sr->data + sr->data_offset; rel++)
      if (ELFW(R_SYM)(rel->r_info) < (unsigned)nsyms)
        refs_after[ELFW(R_SYM)(rel->r_info)]++;
  }
  for (int i = 1; i < nsyms; i++)
    if (syms[i].st_shndx == SHN_UNDEF && ELFW(ST_BIND)(syms[i].st_info) == STB_GLOBAL && !refs_after[i])
      syms[i].st_info = ELFW(ST_INFO)(STB_WEAK, ELFW(ST_TYPE)(syms[i].st_info));
  tcc_free(refs_after);

  tcc_free(work);
  tcc_free(edges);
  tcc_free(edge_start);
  tcc_free(rel_to);
  tcc_free(rel_from);
  tcc_free(edge_count);
  tcc_free(pinned);
  tcc_free(live);
  tcc_free(sec_count);
  tcc_free(sec_first);
  tcc_free(nodes);
}

/* Parse the statics prune_unused_statics dropped, for their diagnostics
 * only, after everything live has been generated -- so what the parse
 * records on symbols (possibly_written, addrtaken) cannot pessimize code.
 * Debug output is off: no line table or DIE may describe them. */
void check_dropped_statics(TCCState *s)
{
  if (!s->nb_dropped_fns && !s->nb_dropped_data)
    return;
  check_scratch_begin(s);
  const char saved_debug_modes = debug_modes;
  debug_modes = 0;
  tcc_open_bf(s, ":dropped:", 0);
  for (int i = 0; i < s->nb_dropped_data; i++)
  {
    DeferredData *d = s->dropped_data[i];
    TokenString *init = d->init;
    d->init = NULL;
    check_dropped_deferred_data(d, init);
  }
  for (int i = 0; i < s->nb_dropped_fns; i++)
  {
    DeferredFunc *d = s->dropped_fns[i];
    TokenString *body = d->body;
    d->body = NULL;
    check_dropped_function(d, body);
  }
  tcc_close();
  debug_modes = saved_debug_modes;
  free_dropped_statics(s);
}

/* Also on every exit, an error's included (tccgen_finish). */
void free_dropped_statics(TCCState *s)
{
  for (int i = 0; i < s->nb_dropped_fns; i++)
  {
    if (s->dropped_fns[i]->body)
      tok_str_free(s->dropped_fns[i]->body);
    tcc_free(s->dropped_fns[i]->pack);
  }
  dynarray_reset(&s->dropped_fns, &s->nb_dropped_fns);
  for (int i = 0; i < s->nb_dropped_data; i++)
    if (s->dropped_data[i]->init)
      tok_str_free(s->dropped_data[i]->init);
  dynarray_reset(&s->dropped_data, &s->nb_dropped_data);
  if (s->check_scratch)
  {
    tcc_free(s->check_scratch->data);
    tcc_free(s->check_scratch);
    s->check_scratch = NULL;
  }
  s->check_only = 0;
}

void gen_deferred_function_bodies(TCCState *s)
{
  const int n = s->nb_deferred_fns;
  if (!n)
    return;
  const int ntok = tok_ident - TOK_IDENT;
  int *by_tok = deferred_alloc(sizeof(int) * (ntok > 0 ? ntok : 1));
  for (int k = 0; k < ntok; k++)
    by_tok[k] = -1;
  for (int i = 0; i < n; i++)
  {
    int k = (s->deferred_fns[i]->sym->v & ~SYM_FIELD) - TOK_IDENT;
    if (k >= 0 && k < ntok)
      by_tok[k] = i;
  }

  /* References to deferred functions from the saved bodies: a name followed
   * by '(' is a call; anything else (address taken, sizeof, a same-named
   * local) is another mention, which rules the function out. */
  int *calls = deferred_alloc(sizeof(int) * n), *mentions = deferred_alloc(sizeof(int) * n);
  int *caller = deferred_alloc(sizeof(int) * n);
  int *edge_start = deferred_alloc(sizeof(int) * (n + 1)), nedges = 0, edge_cap = 0;
  int *edges = NULL;
  for (int i = 0; i < n; i++)
  {
    edge_start[i] = nedges;
    const int *p = tok_str_buf(s->deferred_fns[i]->body);
    int t;
    CValue cv;
    tok_get(&t, &p, &cv);
    while (t != TOK_EOF && t != 0)
    {
      int next_t;
      CValue ncv;
      tok_get(&next_t, &p, &ncv);
      if (t >= TOK_IDENT && t - TOK_IDENT < ntok && by_tok[t - TOK_IDENT] >= 0)
      {
        int j = by_tok[t - TOK_IDENT];
        if (next_t == '(')
        {
          calls[j]++;
          caller[j] = i;
          if (nedges == edge_cap)
          {
            /* Grown by copying into a fresh tracked array: a realloc'd block
             * would go stale in the scratch list. */
            int *grown = deferred_alloc(sizeof(int) * (edge_cap ? edge_cap * 2 : 1024));
            if (nedges)
              memcpy(grown, edges, sizeof(int) * nedges);
            if (edges)
              deferred_release(edges);
            edges = grown;
            edge_cap = edge_cap ? edge_cap * 2 : 1024;
          }
          edges[nedges++] = j;
        }
        else
          mentions[j]++;
      }
      t = next_t;
    }
  }
  edge_start[n] = nedges;

  uint8_t *once = deferred_alloc(n);
  for (int j = 0; j < n; j++)
  {
    DeferredFunc *d = s->deferred_fns[j];
    Sym *sym = d->sym;
    if (!TCC_OPT(s, opt_inline_called_once) || !(sym->type.t & VT_STATIC) || calls[j] != 1 || mentions[j] ||
        caller[j] == j || sym->c || sym->a.addrtaken || !d->body || is_pending_alias_target(sym) ||
        !called_once_body_ok(sym, d->body))
    {
      /* TCC_CALLED_ONCE_DBG: why a function with one call stays a call. */
      static int dbg = -1;
      if (dbg < 0)
        dbg = getenv("TCC_CALLED_ONCE_DBG") != NULL;
      if (dbg && calls[j] == 1 && TCC_OPT(s, opt_inline_called_once))
        fprintf(stderr, "[ONCE] %s len=%d decline=%s caller=%s\n", get_tok_str(sym->v, NULL), d->body ? d->body->len : -1,
                !(sym->type.t & VT_STATIC) ? "not-static"
                : mentions[j]              ? "mentioned"
                : caller[j] == j           ? "self"
                : sym->c                   ? "emitted"
                : sym->a.addrtaken         ? "addrtaken"
                : is_pending_alias_target(sym) ? "alias-target"
                : !d->body                 ? "no-body"
                                           : "body-rules",
                caller[j] >= 0 && caller[j] < n ? get_tok_str(s->deferred_fns[caller[j]]->sym->v, NULL) : "?");
      continue;
    }
    once[j] = 1;
    /* A deferred inline body: the call site replays it, gen_inline_functions
     * emits it only if a reference survives. */
    InlineFunc *fn = tcc_mallocz(sizeof *fn + strlen(d->filename));
    strcpy(fn->filename, d->filename);
    fn->sym = sym;
    fn->func_str = d->body;
    fn->pack = d->pack;
    d->body = NULL;
    d->pack = NULL;
    sym->type.t |= VT_INLINE;
    sym->type.ref->f.func_auto_inline = 1;
    sym->type.ref->f.func_deferred_inline = 1;
    sym->type.ref->f.func_called_once = 1;
    dynarray_add(&s->inline_fns, &s->nb_inline_fns, fn);
  }
  /* Only the call graph and `once` are read from here on. */
  deferred_release(by_tok);
  deferred_release(calls);
  deferred_release(mentions);
  deferred_release(caller);

  /* Generate the others callees first (post-order over the call edges,
   * iteratively: call chains in generated C run deep). */
  uint8_t *state = deferred_alloc(n); /* 0 new, 1 on the stack, 2 done */
  int *stack = deferred_alloc(sizeof(int) * 2 * (n + 1)), sp = 0;
  tcc_open_bf(s, ":deferred:", 0);
  for (int root = 0; root < n; root++)
  {
    if (state[root])
      continue;
    stack[sp++] = root;
    stack[sp++] = edge_start[root];
    state[root] = 1;
    while (sp)
    {
      int i = stack[sp - 2], e = stack[sp - 1];
      if (e < edge_start[i + 1])
      {
        stack[sp - 1] = e + 1;
        int j = edges[e];
        if (!state[j])
        {
          state[j] = 1;
          stack[sp++] = j;
          stack[sp++] = edge_start[j];
        }
        continue;
      }
      sp -= 2;
      state[i] = 2;
      if (!once[i] && s->deferred_fns[i]->body)
      {
        if (check_every_static())
        {
          check_scratch_begin(s);
          check_dropped_function(s->deferred_fns[i], tok_str_clone(s->deferred_fns[i]->body));
        }
        define_deferred_function(s->deferred_fns[i]);
      }
    }
  }
  tcc_close();

  free_deferred_functions(s);
}
