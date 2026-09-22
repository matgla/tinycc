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

/* parse a function defined by symbol 'sym' and generate its code in
   'cur_text_section' */

void ir_inline_stash_flush(TCCState *s1)
{
  for (int i = 0; i < s1->nb_stashed_func_irs; i++)
  {
    if (s1->stashed_func_irs[i].ir)
      tcc_ir_free(s1->stashed_func_irs[i].ir);
  }
  tcc_free(s1->stashed_func_irs);
  s1->stashed_func_irs = NULL;
  s1->nb_stashed_func_irs = 0;
  s1->stashed_func_irs_capacity = 0;
}

/* Remove [start, start+size) bytes from `sec`, shifting trailing data
 * down.  Updates symbol values and relocation offsets accordingly so
 * the section stays self-consistent.  Used by gen_late_reopt_functions
 * to reclaim the original code range of a function that is about to be
 * re-emitted.  Cross-section relocations resolve via symbol indices, so
 * only this section's own reloc table needs r_offset adjustment — except
 * debug info (DWARF), which records text PCs as section symbol +
 * IN-PLACE addend: those addends are rewritten here (shifted past the
 * erased range, tombstoned inside it), so the erase is safe under -g. */
static void erase_text_range(TCCState *s, Section *sec, addr_t start, addr_t size)
{
  if (size == 0 || !sec || !sec->data)
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
  addr_t shift = size & ~(addr_t)3;
  if (shift == 0)
    return;

  /* Shift the section's tail data down (by `shift`, not `size`). */
  size_t tail_offset = (size_t)(start + size);
  size_t tail_len = sec->data_offset - tail_offset;
  if (tail_len > 0)
    memmove(sec->data + (tail_offset - shift), sec->data + tail_offset, tail_len);
  sec->data_offset -= shift;

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

  for (i = 0; i < s->nb_inline_fns; ++i)
  {
    fn = s->inline_fns[i];
    sym = fn->sym;
    if (!sym || !sym->type.ref)
      continue;
    if (!sym->type.ref->f.func_late_reopt)
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
    tccpp_putfile(fn->filename);
    pp_pack_enter(s, fn->pack, saved_pack);
    begin_macro(compile_ts, 1);
    next();
    gen_function(sym);
    end_macro();
    pp_pack_leave(s, saved_pack);
    called_once_budget_end(saved_budget);

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

  tcc_close();
  s->ir_late_reopt_phase = 0;
}

/* Under -ffunction-sections, give each function its own .text.<name> input
 * section so the linker's gc_sections() can drop unreferenced bodies
 * individually.  Survivors are folded back into the single text_section
 * before GOT build and layout (coalesce_function_sections in tccelf.c), so
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
        tccpp_putfile(fn->filename);
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
  s->inline_fn_indexed = 0;
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
    if (k >= s->inline_fn_by_tok_size)
    {
      int n = s->inline_fn_by_tok_size ? s->inline_fn_by_tok_size : 256;
      while (n <= k)
        n *= 2;
      s->inline_fn_by_tok = tcc_realloc(s->inline_fn_by_tok, sizeof(InlineFunc *) * n);
      memset(s->inline_fn_by_tok + s->inline_fn_by_tok_size, 0,
             sizeof(InlineFunc *) * (n - s->inline_fn_by_tok_size));
      s->inline_fn_by_tok_size = n;
    }
    /* First entry for a symbol wins, as the linear scan it replaces did. */
    InlineFunc *old = s->inline_fn_by_tok[k];
    if (!old || old->sym != fn->sym)
      s->inline_fn_by_tok[k] = fn;
  }
  int k = (sym->v & ~SYM_FIELD) - TOK_IDENT;
  if (k < 0 || k >= s->inline_fn_by_tok_size || !s->inline_fn_by_tok[k])
    return NULL;
  InlineFunc *fn = s->inline_fn_by_tok[k];
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

/* Whether BODY forces a frame pointer on whatever function it ends up in:
 * __builtin_frame_address returns it, and alloca moves SP under the locals.
 * (__builtin_return_address(0) reads the saved LR off SP and does not.) */
static int body_forces_frame_pointer(TokenString *body)
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

/* Can the token-replay inliner express a called-once body, and is expanding
 * it no worse than the call?  The one cost rule: a body that forces a frame
 * pointer would force it on the caller, and a Thumb frame-pointer function
 * addresses its locals at negative offsets from r7, where a load or store
 * only encodes -255..0 -- every other access becomes movw + rsb + ldr.  When
 * __builtin_return_address still forced one, zig's allocator helpers
 * (@returnAddress()) expanded into large callers cost zig.c 289 KB of .text,
 * more than all called-once inlining saved.  Lift this once the frame pointer
 * addresses the frame from its bottom, as gcc's does. */
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
  if ((ref->type.t & VT_BTYPE) != VT_VOID && !inline_body_has_return_stmt(body))
    return 0;
  if (body_forces_frame_pointer(body))
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
#define CALLED_ONCE_LARGE_BODY 8000

int called_once_budget_begin(int own_len)
{
  int saved = tcc_state->called_once_budget;
  int limit = own_len * 2 > CALLED_ONCE_LARGE_BODY ? own_len * 2 : CALLED_ONCE_LARGE_BODY;
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
  for (int i = 0; i < nb_deferred_scratch; i++)
    tcc_free(deferred_scratch[i]);
  tcc_free(deferred_scratch);
  deferred_scratch = NULL;
  nb_deferred_scratch = 0;
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
    if (!(sym->type.t & VT_STATIC) || calls[j] != 1 || mentions[j] || caller[j] == j || sym->c ||
        sym->a.addrtaken || !d->body || !called_once_body_ok(sym, d->body))
      continue;
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
        define_deferred_function(s->deferred_fns[i]);
    }
  }
  tcc_close();

  free_deferred_functions(s);
}
