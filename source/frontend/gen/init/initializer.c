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

/* initializer.c -- Initializer parsing, designators and element emission.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* This skips over a stream of tokens containing balanced {} and ()
   pairs, stopping at outer ',' ';' and '}' (or matching '}' if we started
   with a '{').  If STR then allocates and stores the skipped tokens
   in *STR.  This doesn't check if () and {} are nested correctly,
   i.e. "({)}" is accepted.  */
void skip_or_save_block(TokenString **str)
{
  int braces = tok == '{';
  int level = 0;
  /* While recording (str != NULL), redirect #pragma pack directives consumed by
     next() into the saved stream as deferred TOK_PACK_REPLAY actions rather than
     letting them mutate pack_stack now — the body's structs are laid out later
     during replay, so the pack state must travel with the tokens. */
  TokenString *saved_capture = pp_pragma_capture;
  if (str)
    *str = tok_str_alloc();
  pp_pragma_capture = str ? *str : saved_capture;

  int recorded_len = 0;

  while (1)
  {
    int t = tok;
    if (level == 0 && (t == ',' || t == ';' || t == '}' || t == ')' || t == ']'))
      break;
    if (t == TOK_EOF)
    {
      if (str || level > 0)
      {
        pp_pragma_capture = saved_capture;
        tcc_error("unexpected end of file");
      }
      else
        break;
    }
    if (str)
    {
      tok_str_add_tok(*str);
      recorded_len = (*str)->len;
    }
    next();
    if (t == '{' || t == '(' || t == '[')
    {
      level++;
    }
    else if (t == '}' || t == ')' || t == ']')
    {
      level--;
      if (level == 0 && braces && t == '}')
        break;
    }
  }
  pp_pragma_capture = saved_capture;
  if (str)
  {
    /* The lookahead read after the last recorded token (the token after a
       body's closing brace, or the terminator) ran any #pragma pack in
       between with capture still on.  Those directives are outside the saved
       tokens: apply them now, in file order, and drop them from the stream.
       Left in, `f(){...} #pragma pack(1) struct S {...};` laid S out unpacked
       and replayed pack(1) whenever f's body was. */
    if ((*str)->len > recorded_len)
    {
      const int *buf = tok_str_buf(*str);
      pp_apply_pack_replays(tcc_state, buf + recorded_len, buf + (*str)->len);
      (*str)->len = recorded_len;
    }
    tok_str_add(*str, TOK_EOF);
    /* A saved block is complete: give back the doubling slack.  At -O2 every
       function body of the TU waits here as tokens until the end of the TU,
       a quarter of it unused capacity on average. */
    if ((*str)->allocated_len > (*str)->len)
    {
      (*str)->data.str = tcc_realloc((*str)->data.str, (*str)->len * sizeof(int));
      (*str)->allocated_len = (*str)->len;
    }
  }
}

#define EXPR_CONST 1
#define EXPR_ANY 2

void parse_init_elem(int expr_type)
{
  int saved_global_expr;
  switch (expr_type)
  {
  case EXPR_CONST:
    /* compound literals must be allocated globally in this case */
    saved_global_expr = global_expr;
    global_expr = 1;
    expr_const1();
    global_expr = saved_global_expr;
    /* NOTE: symbols are accepted, as well as lvalue for anon symbols
       (compound literals).  */
    if (((vtop->r & (VT_VALMASK | VT_LVAL)) != VT_CONST &&
         ((vtop->r & (VT_SYM | VT_LVAL)) != (VT_SYM | VT_LVAL) || vtop->sym->v < SYM_FIRST_ANOM))
#ifdef TCC_TARGET_PE
        || ((vtop->r & VT_SYM) && vtop->sym->a.dllimport)
#endif
    )
      tcc_error("initializer element is not constant");
    break;
  case EXPR_ANY:
    expr_eq();
    break;
  }
}

#if 1
void init_assert(init_params *p, int offset)
{
  if (p->sec ? !NOSTATIC_WANTED && offset > p->sec->data_offset : !nocode_wanted && offset > p->local_offset)
    tcc_internal_error("initializer overflow");
}
#else
#define init_assert(sec, offset)
#endif

/* put zeros for variable based init */
void init_putz(init_params *p, unsigned long c, int size)
{
  init_assert(p, c + size);
  if (p->sec)
  {
    /* nothing to do because globals are already set to zero */
  }
  else if (tcc_state->ir && size <= 32 && !(size & 3))
  {
    /* Small, word-aligned zero-init: expand to individual word stores
     * of #0 so the optimizer can see (and eliminate) them when
     * subsequent field stores overwrite every word. */
    CType word_type;
    word_type.t = VT_INT;
    word_type.ref = NULL;

    SValue zero;
    svalue_init(&zero);
    zero.type = word_type;
    zero.r = VT_CONST;
    zero.vr = -1;
    zero.c.i = 0;

    for (int off = 0; off < size; off += 4)
    {
      SValue d;
      svalue_init(&d);
      d.type = word_type;
      d.r = VT_LOCAL | VT_LVAL;
      d.vr = -1;
      d.c.i = c + off;
      tcc_ir_put(tcc_state->ir, TCCIR_OP_STORE, &zero, NULL, &d);
    }
  }
  else if (tcc_state->ir && size > 0 && size <= 16)
  {
    /* Small non-word-aligned zero-init: expand to individual byte stores
     * of #0.  Byte-granular stores let downstream byte loads match
     * exactly (no partial-overlap forwarding required), which is the
     * shape produced by partially-initialized char arrays / packed
     * structs (e.g. `const char X[10] = { 'A', 'B', 'C', 'D', 'E' };`).
     * Size capped at 16 to stay within the contributing-store limit of
     * tcc_ir_opt_memmove_to_indexed_stores; larger sizes fall through
     * to the memset call below so the optimizer can still recognize the
     * memset-shift pattern. */
    CType byte_type;
    byte_type.t = VT_BYTE | VT_UNSIGNED;
    byte_type.ref = NULL;

    SValue zero;
    svalue_init(&zero);
    zero.type = byte_type;
    zero.r = VT_CONST;
    zero.vr = -1;
    zero.c.i = 0;

    for (int off = 0; off < size; off++)
    {
      SValue d;
      svalue_init(&d);
      d.type = byte_type;
      d.r = VT_LOCAL | VT_LVAL;
      d.vr = -1;
      d.c.i = c + off;
      tcc_ir_put(tcc_state->ir, TCCIR_OP_STORE, &zero, NULL, &d);
    }
  }
  else
  {
    SValue src1;
    SValue dest;

    vseti(VT_LOCAL, c);
    vpushi(0);
    vpushs(size);

    svalue_init(&src1);
    src1.vr = -1;
    const int call_id = tcc_state->ir ? tcc_state->ir->next_call_id++ : 0;
    /* __aeabi_memset(dest, n, c) on ARM EABI; memset(dest, c, n) elsewhere.
     * TOK_memset maps to __aeabi_memset when TCC_ARM_EABI is defined.
     * Stack is: dest, c, n */
    src1.r = VT_CONST;
    src1.c.i = TCCIR_ENCODE_PARAM(call_id, 0);
    LOG_CODEGEN("FUNCPARAMVAL push: site=init_putz call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d", call_id,
                TCCIR_DECODE_PARAM_IDX((uint32_t)src1.c.i), vtop[-2].r, vtop[-2].vr);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[-2], &src1, NULL);
    src1.c.i = TCCIR_ENCODE_PARAM(call_id, 2);
    LOG_CODEGEN("FUNCPARAMVAL push: site=init_putz call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d", call_id,
                TCCIR_DECODE_PARAM_IDX((uint32_t)src1.c.i), vtop[-1].r, vtop[-1].vr);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[-1], &src1, NULL);
    src1.c.i = TCCIR_ENCODE_PARAM(call_id, 1);
    LOG_CODEGEN("FUNCPARAMVAL push: site=init_putz call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d", call_id,
                TCCIR_DECODE_PARAM_IDX((uint32_t)src1.c.i), vtop[0].r, vtop[0].vr);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[0], &src1, NULL);

    vpush_helper_func(TOK_memset);
    svalue_init(&dest);
    dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
    dest.type.t = vtop[-3].type.t;
    dest.r = 0;
    SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, 3);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVOID, &vtop[0], &call_id_sv, &dest);
    vtop -= 4;

    // vtop -= 4;
    // vtop->r = 0;
    // vtop->vr = dest.vr;
    // vtop->r = 0;
    // vtop->vr = dest.vr;

#if defined(TCC_TARGET_ARM) && defined TCC_ARM_EABI
    // vswap(); /* using __aeabi_memset(void*, size_t, int) */
#endif
    // gfunc_call(3);
  }
}

#define DIF_FIRST 1
#define DIF_SIZE_ONLY 2
#define DIF_HAVE_ELEM 4
#define DIF_CLEAR 8

/* delete relocations for specified range c ... c + size. Unfortunatly
   in very special cases, relocations may occur unordered */
static void decl_design_delrels(Section *sec, int c, int size)
{
  ElfW_Rel *rel, *rel2, *rel_end;
  if (!sec || !sec->reloc)
    return;
  rel = rel2 = (ElfW_Rel *)sec->reloc->data;
  rel_end = (ElfW_Rel *)(sec->reloc->data + sec->reloc->data_offset);
  while (rel < rel_end)
  {
    if (rel->r_offset >= c && rel->r_offset < c + size)
    {
      sec->reloc->data_offset -= sizeof *rel;
    }
    else
    {
      if (rel2 != rel)
        memcpy(rel2, rel, sizeof *rel);
      ++rel2;
    }
    ++rel;
  }
}

void decl_design_flex(init_params *p, Sym *ref, int index)
{
  if (ref == p->flex_array_ref)
  {
    if (p->flex_array_bounded && index >= p->flex_array_max)
      tcc_error("initializer of flexible array member does not fit in the union");
    if (index >= ref->c)
      ref->c = index + 1;
  }
  else if (ref->c < 0)
  {
    if (p->flex_array_ref)
      tcc_error("initialization of flexible array member in a nested context");
    tcc_error("flexible array has zero size in this context");
  }
}

/* t is the array or struct type. c is the array or struct
   address. cur_field is the pointer to the current
   field, for arrays the 'c' member contains the current start
   index.  'flags' is as in decl_initializer.
   'al' contains the already initialized length of the
   current container (starting at c).  This returns the new length of that.  */
int decl_designator(init_params *p, CType *type, unsigned long c, Sym **cur_field, int flags, int al)
{
  Sym *s, *f;
  int index, index_last, align, l, nb_elems, elem_size;
  unsigned long corig = c;
  /* an array element keeps the byte order of its array; a member takes its
     own (big-endian scalar_storage_order marks every member) */
  int sso_be = p->sso_be;

  elem_size = 0;
  nb_elems = 1;

  if (flags & DIF_HAVE_ELEM)
    goto no_designator;

  if (gnu_ext && tok >= TOK_UIDENT)
  {
    l = tok, next();
    if (tok == ':')
      goto struct_field;
    unget_tok(l);
  }

  /* NOTE: we only support ranges for last designator */
  while (nb_elems == 1 && (tok == '[' || tok == '.'))
  {
    if (tok == '[')
    {
      if (!(type->t & VT_ARRAY))
        expect("array type");
      next();
      index = index_last = expr_const();
      if (tok == TOK_DOTS && gnu_ext)
      {
        next();
        index_last = expr_const();
      }
      skip(']');
      s = type->ref;
      decl_design_flex(p, s, index_last);
      if (index < 0 || index_last >= s->c || index_last < index)
        tcc_error("index exceeds array bounds or range is empty");
      if (cur_field)
        (*cur_field)->c = index_last;
      type = pointed_type(type);
      elem_size = type_size(type, &align);
      c += index * elem_size;
      nb_elems = index_last - index + 1;
    }
    else
    {
      int cumofs;
      next();
      l = tok;
    struct_field:
      next();
      f = find_field(type, l, &cumofs);
      if (cur_field)
        *cur_field = f;
      type = &f->type;
      c += cumofs;
      sso_be = f->a.sso_be;
    }
    cur_field = NULL;
  }
  if (!cur_field)
  {
    if (tok == '=')
    {
      next();
    }
    else if (!gnu_ext)
    {
      expect("=");
    }
  }
  else
  {
  no_designator:
    if (type->t & VT_ARRAY)
    {
      index = (*cur_field)->c;
      s = type->ref;
      decl_design_flex(p, s, index);
      if (index >= s->c)
        tcc_error("too many initializers");
      type = pointed_type(type);
      elem_size = type_size(type, &align);
      c += index * elem_size;
    }
    else
    {
      f = *cur_field;
      /* Skip bitfield padding. Also with size 32 and 64. */
      while (f && (f->v & SYM_FIRST_ANOM) && is_integer_btype(f->type.t & VT_BTYPE))
        *cur_field = f = f->next;
      if (!f)
        tcc_error("too many initializers");
      type = &f->type;
      c += f->c;
      sso_be = f->a.sso_be;
    }
  }

  if (!elem_size) /* for structs */
    elem_size = type_size(type, &align);

  /* Using designators the same element can be initialized more
     than once.  In that case we need to delete possibly already
     existing relocations. */
  if (!(flags & DIF_SIZE_ONLY) && c - corig < al)
  {
    decl_design_delrels(p->sec, c, elem_size * nb_elems);
    flags &= ~DIF_CLEAR; /* mark stack dirty too */
  }

  const int sso_be_outer = p->sso_be;
  p->sso_be = sso_be;
  decl_initializer(p, type, c, flags & ~DIF_FIRST, -1);
  p->sso_be = sso_be_outer;

  if (!(flags & DIF_SIZE_ONLY) && nb_elems > 1)
  {
    Sym aref = {0};
    CType t1;
    int i;
    if (p->sec || (type->t & VT_ARRAY))
    {
      /* make init_putv/vstore believe it were a struct */
      aref.c = elem_size;
      t1.t = VT_STRUCT, t1.ref = &aref;
      type = &t1;
    }
    if (p->sec)
    {
      vpush_ref(type, p->sec, c, elem_size);
      for (i = 1; i < nb_elems; i++)
      {
        vdup();
        init_putv(p, type, c + elem_size * i, -1);
      }
      vpop();
    }
    else
    {
      /* Local range designators: copy the first element's value into each
         subsequent slot using vstore, so stack-relative addressing stays
         correct. */
      for (i = 1; i < nb_elems; i++)
      {
        vset(type, VT_LOCAL | VT_LVAL, c + elem_size * i); /* dest */
        vset(type, VT_LOCAL | VT_LVAL, c);                 /* src */
        vstore();
        vpop(); /* drop dest/result left by vstore */
      }
    }
  }

  c += nb_elems * elem_size;
  if (c - corig > al)
    al = c - corig;
  return al;
}

/* The value just written at sec+c is `&&lab1 - &&lab0` (vtop carries the label
 * difference marker): remember to add the label offset difference, in `size`
 * bytes, once the function's code is laid out. */
static void label_diff_record(Section *sec, unsigned long c, int size)
{
  LabelDiffFixup *fixup = tcc_malloc(sizeof(LabelDiffFixup));
  fixup->sec = sec;
  fixup->offset = c;
  fixup->size = size;
  fixup->sym_plus = pending_label_diff_plus;
  fixup->sym_minus = pending_label_diff_minus;
  fixup->next = tcc_state->label_diff_fixups;
  tcc_state->label_diff_fixups = fixup;
  pending_label_diff_plus = NULL;
  pending_label_diff_minus = NULL;
}

static void sso_reverse_bytes(unsigned char *b, int n)
{
  for (int i = 0; i < n / 2; i++)
  {
    unsigned char t = b[i];
    b[i] = b[n - 1 - i];
    b[n - 1 - i] = t;
  }
}

/* store a value or an expression directly in global data or in local array */
void init_putv(init_params *p, CType *type, unsigned long c, int vreg)
{
  int bt;
  void *ptr;
  CType dtype;
  int size, align;
  Section *sec = p->sec;
  uint64_t val;

  dtype = *type;
  dtype.t &= ~VT_CONSTANT; /* need to do that to avoid false warning */
  if (type->t & VT_RODATA_REL)
  {
    /* The relocation encodes it (rodata_rel_init); only a static object has
       one, an automatic one would be filled by a store. */
    if (!sec)
      rodata_rel_store_error();
    dtype.t &= ~VT_RODATA_REL;
  }

  size = type_size(type, &align);
  if (type->t & VT_BITFIELD)
    size = (BIT_POS(type->t) + BIT_SIZE(type->t) + 7) / 8;
  init_assert(p, c + size);
  /* bytes of a byte-reversed scalar (big-endian scalar_storage_order) */
  const int sso_size = p->sso_be ? sso_scalar_size(type) : 0;

  if (p->const_probe)
  {
    /* Template probe (see init_params): capture a plain load-time-constant
     * integer/pointer scalar into the host-side template buffer, emit nothing.
     * Anything else (runtime value, symbol/relocation, bitfield, float/struct,
     * out-of-range offset) aborts the templating attempt. */
    int pbt = type->t & VT_BTYPE;
    int rel = (int)c - p->const_probe_base;
    /* Convert to the element type first, as vstore does: a float constant into
       an int element, or any non-zero constant into a _Bool, must not be
       written raw.  The probe fails below unless the result is still a plain
       constant. */
    if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && !(type->t & VT_BITFIELD) &&
        !(type->t & VT_COMPLEX) && pbt != VT_STRUCT)
      gen_assign_cast(&dtype);
    if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && !(type->t & VT_BITFIELD) &&
        !(type->t & VT_COMPLEX) &&
        (vtop->type.t & VT_BTYPE) != VT_FLOAT && (vtop->type.t & VT_BTYPE) != VT_DOUBLE &&
        (vtop->type.t & VT_BTYPE) != VT_LDOUBLE &&
        (pbt == VT_BOOL || pbt == VT_BYTE || pbt == VT_SHORT || pbt == VT_INT || pbt == VT_LLONG || pbt == VT_PTR) &&
        rel >= 0 && rel + size <= p->const_probe_size)
    {
      uint64_t cval = (uint64_t)vtop->c.i;
      unsigned char *d = p->const_probe_data + rel;
      switch (size)
      {
      case 1:
        d[0] = (unsigned char)cval;
        break;
      case 2:
        write16le(d, (uint16_t)cval);
        break;
      case 4:
        write32le(d, (uint32_t)cval);
        break;
      case 8:
        write64le(d, cval);
        break;
      default:
        p->const_probe_failed = 1;
        break;
      }
      sso_reverse_bytes(d, sso_size);
    }
    else
    {
      p->const_probe_failed = 1;
    }
    vtop--;
    return;
  }

  if (sec)
  {
    /* XXX: not portable */
    /* XXX: generate error if incorrect relocation */
    gen_assign_cast(&dtype);
    bt = type->t & VT_BTYPE;

    int is_label_diff = (vtop->r & VT_SYM) && vtop->sym == &label_diff_marker;
    if (is_label_diff && (type->t & VT_BITFIELD || (bt != VT_BYTE && bt != VT_SHORT && bt != VT_INT && bt != VT_PTR)))
      tcc_error("initializer element is not computable at load time");
    if (!is_label_diff && (vtop->r & VT_SYM) && bt != VT_PTR &&
        (bt != (PTR_SIZE == 8 ? VT_LLONG : VT_INT) || (type->t & VT_BITFIELD)) &&
        !((vtop->r & VT_CONST) && vtop->sym->v >= SYM_FIRST_ANOM))
      tcc_error("initializer element is not computable at load time");

    if (NOSTATIC_WANTED)
    {
      vtop--;
      print_vstack("init_putv");
      return;
    }

    ptr = sec->data + c;
    val = vtop->c.i;

    /* XXX: make code faster ? */
    if ((vtop->r & (VT_SYM | VT_CONST)) == (VT_SYM | VT_CONST) && vtop->sym->v >= SYM_FIRST_ANOM &&
        /* XXX This rejects compound literals like
           '(void *){ptr}'.  The problem is that '&sym' is
           represented the same way, which would be ruled out
           by the SYM_FIRST_ANOM check above, but also '"string"'
           in 'char *p = "string"' is represented the same
           with the type being VT_PTR and the symbol being an
           anonymous one.  That is, there's no difference in vtop
           between '(void *){x}' and '&(void *){x}'.  Ignore
           pointer typed entities here.  Hopefully no real code
           will ever use compound literals with scalar type.  */
        (vtop->type.t & VT_BTYPE) != VT_PTR)
    {
      /* These come from compound literals, memcpy stuff over.  */
      Section *ssec;
      ElfSym *esym;
      ElfW_Rel *rel;
      esym = elfsym(vtop->sym);
      ssec = tcc_state->sections[esym->st_shndx];
      const unsigned long base = esym->st_value + (int)vtop->c.i;
      memmove(ptr, ssec->data + base, size);
      if (ssec->reloc)
      {
        /* We need to copy over all memory contents, and that
           includes relocations.  Use the fact that relocs are
           created it order, so look from the end of relocs
           until we hit one before the copied region.  */
        unsigned long relofs = ssec->reloc->data_offset;
        while (relofs >= sizeof(*rel))
        {
          relofs -= sizeof(*rel);
          rel = (ElfW_Rel *)(ssec->reloc->data + relofs);
          if (rel->r_offset >= base + size)
            continue;
          if (rel->r_offset < base)
            break;
          put_elf_reloca(symtab_section, sec, c + rel->r_offset - base, ELFW(R_TYPE)(rel->r_info),
                         ELFW(R_SYM)(rel->r_info),
#if PTR_SIZE == 8
                         rel->r_addend
#else
                         0
#endif
          );
        }
      }
    }
    else
    {
      if (type->t & VT_BITFIELD)
      {
        int bit_pos, bit_size, bits, n;
        unsigned char *p, v, m;
        /* a big-endian scalar_storage_order unit: its bit position counts
           from the LSB of the swapped value */
        const Sym *bf = type->ref;
        const int unit = bf && bf->a.sso_be ? bf->r : 0;
        sso_reverse_bytes(ptr, unit);
        bit_pos = BIT_POS(vtop->type.t);
        bit_size = BIT_SIZE(vtop->type.t);
        p = (unsigned char *)ptr + (bit_pos >> 3);
        bit_pos &= 7, bits = 0;
        while (bit_size)
        {
          n = 8 - bit_pos;
          if (n > bit_size)
            n = bit_size;
          v = val >> bits << bit_pos;
          m = ((1 << n) - 1) << bit_pos;
          *p = (*p & ~m) | (v & m);
          bits += n, bit_size -= n, bit_pos = 0, ++p;
        }
        sso_reverse_bytes(ptr, unit);
      }
      else if (type->t & VT_COMPLEX)
      {
        /* Complex integer types: write packed representation directly.
         * The value is packed as [real | imag] in CValue.i,
         * matching little-endian memory layout. */
        int complex_size = type_size(type, &align);
        if (complex_size == 2)
          write16le(ptr, val);
        else if (complex_size == 4)
          write32le(ptr, val);
        else if (complex_size == 8)
          write64le(ptr, val);
        else
          memcpy(ptr, &vtop->c, complex_size);
      }
      else
        switch (bt)
        {
        case VT_BOOL:
          *(char *)ptr = val != 0;
          break;
        case VT_BYTE:
          *(char *)ptr = val;
          if (is_label_diff)
            label_diff_record(sec, c, 1);
          break;
        case VT_SHORT:
          write16le(ptr, val);
          if (is_label_diff)
            label_diff_record(sec, c, 2);
          break;
        case VT_FLOAT:
          write32le(ptr, val);
          break;
        case VT_DOUBLE:
          write64le(ptr, val);
          break;
        case VT_LDOUBLE:
#if defined TCC_IS_NATIVE_387
          /* Host and target platform may be different but both have x87.
             On windows, tcc does not use VT_LDOUBLE, except when it is a
             cross compiler.  In this case a mingw gcc as host compiler
             comes here with 10-byte long doubles, while msvc or tcc won't.
             tcc itself can still translate by asm.
             In any case we avoid possibly random bytes 11 and 12.
          */
          if (sizeof(long double) >= 10)
            memcpy(ptr, &vtop->c.ld, 10);
#ifdef __TINYC__
          else if (sizeof(long double) == sizeof(double))
            __asm__("fldl %1\nfstpt %0\n" : "=m"(*ptr) : "m"(vtop->c.ld));
#endif
          else
#endif
            /* For other platforms it should work natively, but may not work
               for cross compilers */
            if (sizeof(long double) == LDOUBLE_SIZE)
              memcpy(ptr, &vtop->c.ld, LDOUBLE_SIZE);
            else if (sizeof(double) == LDOUBLE_SIZE)
              *(double *)ptr = (double)vtop->c.ld;
            else if (0 == memcmp(ptr, &vtop->c.ld, LDOUBLE_SIZE))
              ; /* nothing to do for 0.0 */
#ifndef TCC_CROSS_TEST
            else
              tcc_error("can't cross compile long double constants");
#endif
          break;

#if PTR_SIZE == 8
        /* intptr_t may need a reloc too, see tcctest.c:relocation_test() */
        case VT_LLONG:
        case VT_PTR:
          if (vtop->r & VT_SYM)
            greloca(sec, vtop->sym, c, R_DATA_PTR, val);
          else
            write64le(ptr, val);
          break;
        case VT_INT:
          write32le(ptr, val);
          break;
#else
        case VT_LLONG:
          write64le(ptr, val);
          break;
        case VT_PTR:
        case VT_INT:
          if (type->t & VT_RODATA_REL)
          {
            rodata_rel_init(sec, c);
            break;
          }
          if ((vtop->r & VT_SYM) && vtop->sym == &label_diff_marker)
          {
            /* &&lab1 - &&lab0: the value written is the addend difference;
               the label offsets are added once the function is generated. */
            label_diff_record(sec, c, 4);
          }
          else if (vtop->r & VT_SYM)
          {
            /* Debug check for garbage symbol */
            if (!vtop->sym || vtop->sym->v >= SYM_FIRST_ANOM + 100000)
            {
              tcc_error("internal error: init_putv has garbage sym (v=0x%x, r=0x%x)", vtop->sym ? vtop->sym->v : 0,
                        vtop->r);
            }
            greloc(sec, vtop->sym, c, R_DATA_PTR);
          }
          write32le(ptr, val);
          break;
#endif
        default:
          // tcc_internal_error("unexpected type");
          break;
        }
      if (sso_size)
      {
        if (vtop->r & VT_SYM)
          tcc_error("initializer element is not computable at load time");
        sso_reverse_bytes(ptr, sso_size);
      }
    }
    vtop--;
    print_vstack("init_putv(2)");
  }
  else
  {
    /* Capture scalar constant into the tracked sym's const_init_data
     * before vstore (which pops the value). Buffer was zeroed at
     * allocation time, so zero values can be silently dropped. */
    SymLocalFacts *cf = p->const_init_sym ? sym_facts_peek(p->const_init_sym) : NULL;
    if (cf && cf->const_init_valid)
    {
      int rel_off = (int)c - p->const_init_base;
      int bt = type->t & VT_BTYPE;
      /* An element the buffer cannot record (a whole struct or vector, a
       * bitfield) leaves its bytes unknown.  A copy from a source with known
       * bytes re-validates the buffer in vstore. */
      if (rel_off + size > 0 && rel_off < cf->const_init_size && ((type->t & VT_BITFIELD) || bt == VT_STRUCT))
        cf->const_init_valid = 0;
      if (rel_off >= 0 && rel_off + size <= cf->const_init_size && !(type->t & VT_BITFIELD) &&
          bt != VT_STRUCT)
      {
        /* Convert to the element type first, as vstore does: the buffer must
           hold the stored representation, not the source constant's bits. */
        if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && !(type->t & VT_COMPLEX))
          gen_assign_cast(&dtype);
        if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
        {
          uint64_t cval = (uint64_t)vtop->c.i;
          unsigned char *dst = cf->const_init_data + rel_off;
          switch (size)
          {
          case 1:
            dst[0] = (unsigned char)cval;
            break;
          case 2:
            write16le(dst, (uint16_t)cval);
            break;
          case 4:
            write32le(dst, (uint32_t)cval);
            break;
          case 8:
            write64le(dst, cval);
            break;
          default:
            cf->const_init_valid = 0;
            break;
          }
          sso_reverse_bytes(dst, sso_size);
        }
        else
        {
          cf->const_init_valid = 0;
        }
      }
    }
    vset(&dtype, VT_LOCAL | VT_LVAL, c);
    if (vreg == -1)
    {
      /* Array element initialization: do NOT create a new vreg.
       * Instead, keep vr = -1 so that vstore() will recognize this
       * as a memory store, not a variable assignment.
       * The stack offset 'c' in vtop->c.i identifies the destination. */
      vtop->vr = -1;
    }
    else
    {
      vtop->vr = vreg;
      /* Mark long long variables for proper register allocation */
      if ((dtype.t & VT_BTYPE) == VT_LLONG)
      {
        tcc_ir_set_llong_type(tcc_state->ir, vtop->vr);
      }
    }
    vtop->sso_reversed = sso_size != 0;
    vswap();
    vstore();
    vpop();
  }
}

