/*
 *  GAS like assembler for TCC
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

#define USING_GLOBALS
#include "tcc.h"
#ifdef CONFIG_TCC_ASM

#include <stdlib.h>

static Section *last_text_section; /* to handle .previous asm directive */
static int asmgoto_n;

/* Assembler macro support */
#define ASM_MACRO_MAX_ARGS 16
typedef struct AsmMacro
{
  int name;                     /* token for macro name */
  int nb_args;                  /* number of arguments */
  int args[ASM_MACRO_MAX_ARGS]; /* argument tokens */
  TokenString *body;            /* macro body tokens */
  struct AsmMacro *next;
} AsmMacro;

static AsmMacro *asm_macros = NULL;

static AsmMacro *asm_macro_find(int name)
{
  AsmMacro *m;
  for (m = asm_macros; m; m = m->next)
  {
    if (m->name == name)
      return m;
  }
  return NULL;
}

static void asm_macros_free(void)
{
  AsmMacro *m, *next;
  for (m = asm_macros; m; m = next)
  {
    next = m->next;
    tok_str_free(m->body);
    tcc_free(m);
  }
  asm_macros = NULL;
}

ST_FUNC void tcc_asm_cleanup(void)
{
  asm_macros_free();
  /* .previous must not find a Section of the previous TCCState: tcc_delete
     has freed its section table, so the static would be dangling. */
  last_text_section = NULL;
}

static int asm_get_prefix_name(TCCState *s1, const char *prefix, unsigned int n)
{
  char buf[64];
  snprintf(buf, sizeof(buf), "%s%u", prefix, n);
  return tok_alloc_const(buf);
}

ST_FUNC int asm_get_local_label_name(TCCState *s1, unsigned int n)
{
  return asm_get_prefix_name(s1, "L..", n);
}

static int tcc_assemble_internal(TCCState *s1, int do_preprocess, int global);
static Sym *asm_new_label(TCCState *s1, int label, int is_local);
static Sym *asm_new_label1(TCCState *s1, int label, int is_local, int sh_num, int value);

/* If a C name has an _ prepended then only asm labels that start
   with _ are representable in C, by removing the first _.  ASM names
   without _ at the beginning don't correspond to C names, but we use
   the global C symbol table to track ASM names as well, so we need to
   transform those into ones that don't conflict with a C name,
   so prepend a '.' for them, but force the ELF asm name to be set.  */
static int asm2cname(int v, int *addeddot)
{
  const char *name;
  *addeddot = 0;
  if (!tcc_state->leading_underscore)
    return v;
  name = get_tok_str(v, NULL);
  if (!name)
    return v;
  if (name[0] == '_')
  {
    v = tok_alloc_const(name + 1);
  }
  else if (!strchr(name, '.'))
  {
    char newname[256];
    snprintf(newname, sizeof newname, ".%s", name);
    v = tok_alloc_const(newname);
    *addeddot = 1;
  }
  return v;
}

static Sym *asm_label_find(int v)
{
  Sym *sym;
  int addeddot;
  v = asm2cname(v, &addeddot);
  sym = sym_find(v);
  while (sym && sym->sym_scope && !(sym->type.t & VT_STATIC))
    sym = sym->prev_tok;
  return sym;
}

static Sym *asm_label_push(int v)
{
  int addeddot, v2 = asm2cname(v, &addeddot);
  /* We always add VT_EXTERN, for sym definition that's tentative
     (for .set, removed for real defs), for mere references it's correct
     as is.  */
  Sym *sym = global_identifier_push(v2, VT_ASM | VT_EXTERN | VT_STATIC, 0);
  if (addeddot)
    sym->asm_label = v;
  return sym;
}

/* Return a symbol we can use inside the assembler, having name NAME.
   Symbols from asm and C source share a namespace.  If we generate
   an asm symbol it's also a (file-global) C symbol, but it's
   either not accessible by name (like "L.123"), or its type information
   is such that it's not usable without a proper C declaration.

   Sometimes we need symbols accessible by name from asm, which
   are anonymous in C, in this case CSYM can be used to transfer
   all information from that symbol to the (possibly newly created)
   asm symbol.  */
ST_FUNC Sym *get_asm_sym(int name, Sym *csym)
{
  Sym *sym = asm_label_find(name);
  if (!sym)
  {
    sym = asm_label_push(name);
    if (csym)
      sym->c = csym->c;
  }
  return sym;
}

static Sym *asm_section_sym(TCCState *s1, Section *sec)
{
  char buf[100];
  int label;
  Sym *sym;
  snprintf(buf, sizeof buf, "L.%s", sec->name);
  label = tok_alloc_const(buf);
  sym = asm_label_find(label);
  return sym ? sym : asm_new_label1(s1, label, 1, sec->sh_num, 0);
}

/* We do not use the C expression parser to handle symbols. Maybe the
   C expression parser could be tweaked to do so. */

static void asm_expr_unary(TCCState *s1, ExprValue *pe)
{
  Sym *sym;
  int op, label;
  uint64_t n;
  const char *p;

  switch (tok)
  {
  case TOK_PPNUM:
    p = tokc.str.data;
    n = strtoull(p, (char **)&p, 0);
    if (*p == 'b' || *p == 'f')
    {
      /* backward or forward label */
      label = asm_get_local_label_name(s1, n);
      sym = asm_label_find(label);
      if (*p == 'b')
      {
        /* backward : find the last corresponding defined label */
        if (sym && (!sym->c || elfsym(sym)->st_shndx == SHN_UNDEF))
          sym = sym->prev_tok;
        if (!sym)
          tcc_error("local label '%d' not found backward", (int)n);
      }
      else
      {
        /* forward */
        if (!sym || (sym->c && elfsym(sym)->st_shndx != SHN_UNDEF))
        {
          /* if the last label is defined, then define a new one */
          sym = asm_label_push(label);
        }
      }
      pe->v = 0;
      pe->sym = sym;
      pe->pcrel = 0;
    }
    else if (*p == '\0')
    {
      pe->v = n;
      pe->sym = NULL;
      pe->pcrel = 0;
    }
    else
    {
      tcc_error("invalid number syntax");
    }
    next();
    break;
  case '=':
    /* GAS-style "=expr". Semantics are target-specific (e.g. ldr pseudo-op).
       At the expression level we treat it as a no-op unary operator. */
    next();
    asm_expr_unary(s1, pe);
    break;
  case '+':
    next();
    asm_expr_unary(s1, pe);
    break;
  case '-':
  case '~':
    op = tok;
    next();
    asm_expr_unary(s1, pe);
    if (pe->sym)
      tcc_error("invalid operation with label");
    if (op == '-')
      pe->v = -pe->v;
    else
      pe->v = ~pe->v;
    break;
  case TOK_CCHAR:
  case TOK_LCHAR:
    pe->v = tokc.i;
    pe->sym = NULL;
    pe->pcrel = 0;
    next();
    break;
  case '(':
    next();
    asm_expr(s1, pe);
    skip(')');
    break;
  case '.':
    pe->v = ind;
    pe->sym = asm_section_sym(s1, cur_text_section);
    pe->pcrel = 0;
    next();
    break;
  default:
    if (tok >= TOK_IDENT)
    {
      ElfSym *esym;
      /* label case : if the label was not found, add one */
      sym = get_asm_sym(tok, NULL);
      esym = elfsym(sym);
      if (esym && esym->st_shndx == SHN_ABS)
      {
        /* if absolute symbol, no need to put a symbol value */
        pe->v = esym->st_value;
        pe->sym = NULL;
        pe->pcrel = 0;
      }
      else
      {
        pe->v = 0;
        pe->sym = sym;
        pe->pcrel = 0;
      }
      next();
    }
    else
    {
      tcc_error("bad expression syntax [%s]", get_tok_str(tok, &tokc));
    }
    break;
  }
}

/* A label's place in its section, for label arithmetic.  On ARM a Thumb
 * function symbol carries bit 0 in st_value (the interworking marker), but
 * GAS measures `. - func` and `end - func` from the label itself: counting
 * the bit made yasld's thunk template size 15 instead of 16. */
static addr_t asm_sym_offset(const ElfSym *esym)
{
#if defined(TCC_TARGET_ARM)
  if (ELFW(ST_TYPE)(esym->st_info) == STT_FUNC)
    return esym->st_value & ~(addr_t)1;
#endif
  return esym->st_value;
}

static void asm_expr_prod(TCCState *s1, ExprValue *pe)
{
  int op;
  ExprValue e2;

  asm_expr_unary(s1, pe);
  for (;;)
  {
    op = tok;
    if (op != '*' && op != '/' && op != '%' && op != TOK_SHL && op != TOK_SAR)
      break;
    next();
    asm_expr_unary(s1, &e2);
    if (pe->sym || e2.sym)
      tcc_error("invalid operation with label");
    switch (op)
    {
    case '*':
      pe->v *= e2.v;
      break;
    case '/':
      if (e2.v == 0)
      {
      div_error:
        tcc_error("division by zero");
      }
      pe->v /= e2.v;
      break;
    case '%':
      if (e2.v == 0)
        goto div_error;
      pe->v %= e2.v;
      break;
    case TOK_SHL:
      pe->v <<= e2.v;
      break;
    default:
    case TOK_SAR:
      pe->v >>= e2.v;
      break;
    }
  }
}

static void asm_expr_logic(TCCState *s1, ExprValue *pe)
{
  int op;
  ExprValue e2;

  asm_expr_prod(s1, pe);
  for (;;)
  {
    op = tok;
    if (op != '&' && op != '|' && op != '^')
      break;
    next();
    asm_expr_prod(s1, &e2);
    if (pe->sym || e2.sym)
      tcc_error("invalid operation with label");
    switch (op)
    {
    case '&':
      pe->v &= e2.v;
      break;
    case '|':
      pe->v |= e2.v;
      break;
    default:
    case '^':
      pe->v ^= e2.v;
      break;
    }
  }
}

static inline void asm_expr_sum(TCCState *s1, ExprValue *pe)
{
  int op;
  ExprValue e2;

  asm_expr_logic(s1, pe);
  for (;;)
  {
    op = tok;
    if (op != '+' && op != '-')
      break;
    next();
    asm_expr_logic(s1, &e2);
    if (op == '+')
    {
      if (pe->sym != NULL && e2.sym != NULL)
        goto cannot_relocate;
      pe->v += e2.v;
      if (pe->sym == NULL && e2.sym != NULL)
        pe->sym = e2.sym;
    }
    else
    {
      pe->v -= e2.v;
      /* NOTE: we are less powerful than gas in that case
         because we store only one symbol in the expression */
      if (!e2.sym)
      {
        /* OK */
      }
      else if (pe->sym == e2.sym)
      {
        /* OK */
        pe->sym = NULL; /* same symbols can be subtracted to NULL */
      }
      else
      {
        ElfSym *esym1, *esym2;
        esym1 = elfsym(pe->sym);
        esym2 = elfsym(e2.sym);
        if (!esym2)
          goto cannot_relocate;
        if (esym1 && esym1->st_shndx == esym2->st_shndx && esym1->st_shndx != SHN_UNDEF)
        {
          /* we also accept defined symbols in the same section */
          pe->v += asm_sym_offset(esym1) - asm_sym_offset(esym2);
          pe->sym = NULL;
        }
        else if (esym2->st_shndx == cur_text_section->sh_num)
        {
          /* When subtracting a defined symbol in current section
             this actually makes the value PC-relative.  */
          pe->v += 0 - asm_sym_offset(esym2);
          pe->pcrel = 1;
          e2.sym = NULL;
        }
        else
        {
        cannot_relocate:
          tcc_error("invalid operation with label");
        }
      }
    }
  }
}

static inline void asm_expr_cmp(TCCState *s1, ExprValue *pe)
{
  int op;
  ExprValue e2;

  asm_expr_sum(s1, pe);
  for (;;)
  {
    op = tok;
    if (op != TOK_EQ && op != TOK_NE && (op > TOK_GT || op < TOK_ULE))
      break;
    next();
    asm_expr_sum(s1, &e2);
    if (pe->sym || e2.sym)
      tcc_error("invalid operation with label");
    switch (op)
    {
    case TOK_EQ:
      pe->v = pe->v == e2.v;
      break;
    case TOK_NE:
      pe->v = pe->v != e2.v;
      break;
    case TOK_LT:
      pe->v = (int64_t)pe->v < (int64_t)e2.v;
      break;
    case TOK_GE:
      pe->v = (int64_t)pe->v >= (int64_t)e2.v;
      break;
    case TOK_LE:
      pe->v = (int64_t)pe->v <= (int64_t)e2.v;
      break;
    case TOK_GT:
      pe->v = (int64_t)pe->v > (int64_t)e2.v;
      break;
    default:
      break;
    }
    /* GAS compare results are -1/0 not 1/0.  */
    pe->v = -(int64_t)pe->v;
  }
}

ST_FUNC void asm_expr(TCCState *s1, ExprValue *pe)
{
  asm_expr_cmp(s1, pe);
}

ST_FUNC int asm_int_expr(TCCState *s1)
{
  ExprValue e;
  asm_expr(s1, &e);
  if (e.sym)
    expect("constant");
  return e.v;
}

static Sym *asm_new_label1(TCCState *s1, int label, int is_local, int sh_num, int value)
{
  Sym *sym;
  ElfSym *esym;

  sym = asm_label_find(label);
  if (sym)
  {
    esym = elfsym(sym);
    /* A VT_EXTERN symbol, even if it has a section is considered
       overridable.  This is how we "define" .set targets.  Real
       definitions won't have VT_EXTERN set.  */
    if (esym && esym->st_shndx != SHN_UNDEF)
    {
      /* the label is already defined */
      if (IS_ASM_SYM(sym) && (is_local == 1 || (sym->type.t & VT_EXTERN)))
        goto new_label;
      if (!(sym->type.t & VT_EXTERN))
        tcc_error("assembler label '%s' already defined", get_tok_str(label, NULL));
    }
  }
  else
  {
  new_label:
    sym = asm_label_push(label);
  }
  if (!sym->c)
    put_extern_sym2(sym, SHN_UNDEF, 0, 0, 1);
  esym = elfsym(sym);

  esym->st_shndx = sh_num;
  esym->st_value = value;
  if (s1->thumb_func == 1 || s1->thumb_func == label)
  {
    esym->st_info |= STT_FUNC;
    s1->thumb_func = 0;
    esym->st_value += 1;
  }
#ifdef TCC_TARGET_ARM_THUMB
  /* ARMv8-M is Thumb-only: a defined function symbol's value must carry
     bit 0 (the Thumb bit). tcc's C backend already sets it; do the same for
     hand-written asm that types a label with `.type X, %function` (e.g.
     lib/arm_string.S and the lib/fp soft-float routines). Otherwise GNU ld -- which links the
     tcc runtime archives into the pico-sdk benchmark -- reports "unknown
     destination type (ARM/Thumb)", and Thumb function pointers to these
     routines would drop into ARM mode. tcc's own linker drops bit 0 when it
     encodes the branch offset, so setting it here is transparent there. */
  else if (sh_num != SHN_UNDEF && ELFW(ST_TYPE)(esym->st_info) == STT_FUNC && (esym->st_value & 1) == 0)
    esym->st_value += 1;
#endif
  if (is_local != 2)
    sym->type.t &= ~VT_EXTERN;
  return sym;
}

static Sym *asm_new_label(TCCState *s1, int label, int is_local)
{
  return asm_new_label1(s1, label, is_local, cur_text_section->sh_num, ind);
}

/* Set the value of LABEL to that of some expression (possibly
   involving other symbols).  LABEL can be overwritten later still.  */
static Sym *set_symbol(TCCState *s1, int label)
{
  long n;
  ExprValue e;
  Sym *sym;
  ElfSym *esym;
  next();
  asm_expr(s1, &e);
  n = e.v;
  esym = elfsym(e.sym);
  if (esym)
    n += esym->st_value;
  sym = asm_new_label1(s1, label, 2, esym ? esym->st_shndx : SHN_ABS, n);
  elfsym(sym)->st_other |= ST_ASM_SET;
  return sym;
}

static void use_section1(TCCState *s1, Section *sec)
{
  cur_text_section->data_offset = ind;
  cur_text_section = sec;
  ind = cur_text_section->data_offset;
}

static void use_section(TCCState *s1, const char *name)
{
  Section *sec;
  sec = find_section(s1, name);
  use_section1(s1, sec);
}

static void push_section(TCCState *s1, const char *name)
{
  Section *sec = find_section(s1, name);
  sec->prev = cur_text_section;
  use_section1(s1, sec);
}

static void pop_section(TCCState *s1)
{
  Section *prev = cur_text_section->prev;
  if (!prev)
    tcc_error(".popsection without .pushsection");
  cur_text_section->prev = NULL;
  use_section1(s1, prev);
}

/* Skip a conditional-assembly branch not taken: to the matching .endif, or,
 * when `to_else`, to a matching .else (whose branch is then assembled).
 * Nested .if blocks inside the skipped text are skipped whole. */
static void asm_skip_conditional(int to_else)
{
  int depth = 0;
  for (;;)
  {
    next();
    if (tok == CH_EOF)
      tcc_error("end of file in a conditional block: missing .endif");
    if (tok == TOK_ASMDIR_if || tok == TOK_ASMDIR_ifdef || tok == TOK_ASMDIR_ifndef)
      depth++;
    else if (tok == TOK_ASMDIR_endif)
    {
      if (depth-- == 0)
        break;
    }
    else if (tok == TOK_ASMDIR_else && depth == 0 && to_else)
      break;
  }
  next();
}

static void asm_parse_directive(TCCState *s1, int global)
{
  int n, offset, v, size, tok1;
  Section *sec;
  uint8_t *ptr;

  /* assembler directive */
  sec = cur_text_section;
  switch (tok)
  {
  case TOK_ASMDIR_align:
  case TOK_ASMDIR_balign:
  case TOK_ASMDIR_p2align:
  case TOK_ASMDIR_skip:
  case TOK_ASMDIR_space:
    tok1 = tok;
    next();
    n = asm_int_expr(s1);
#if defined(TCC_TARGET_ARM) || defined(TCC_TARGET_ARM64)
    /* GAS on ARM: `.align N` is 2^N bytes (`.align 2` is word alignment,
       `.align 7` the 128 bytes a vector table needs); .balign stays bytes. */
    if (tok1 == TOK_ASMDIR_align)
      tok1 = TOK_ASMDIR_p2align;
#endif
    if (tok1 == TOK_ASMDIR_p2align)
    {
      if (n < 0 || n > 30)
        tcc_error("invalid p2align, must be between 0 and 30");
      n = 1 << n;
      tok1 = TOK_ASMDIR_align;
    }
    if (tok1 == TOK_ASMDIR_align || tok1 == TOK_ASMDIR_balign)
    {
      if (n < 0 || (n & (n - 1)) != 0)
        tcc_error("alignment must be a positive power of two");
      offset = (ind + n - 1) & -n;
      size = offset - ind;
      /* the section must have a compatible alignment */
      if (sec->sh_addralign < n)
        sec->sh_addralign = n;
    }
    else
    {
      if (n < 0)
        n = 0;
      size = n;
    }
    v = 0;
    if (tok == ',')
    {
      next();
      v = asm_int_expr(s1);
    }
  zero_pad:
    if (sec->sh_type != SHT_NOBITS)
    {
      sec->data_offset = ind;
      ptr = section_ptr_add(sec, size);
      if (ptr != NULL)
      {
        memset(ptr, v, size);
      }
    }
    ind += size;
    break;
  case TOK_ASMDIR_quad:
    size = 8;
    goto asm_data;
  case TOK_ASMDIR_byte:
    size = 1;
    goto asm_data;
  case TOK_ASMDIR_word:
#if defined(TCC_TARGET_ARM) || defined(TCC_TARGET_ARM64)
    /* GAS on ARM: .word is 32 bits (a vector table of .words of symbols). */
    size = 4;
    goto asm_data;
#endif
  case TOK_ASMDIR_short:
  case TOK_ASMDIR_hword:
    size = 2;
    goto asm_data;
  case TOK_ASMDIR_long:
  case TOK_ASMDIR_int:
    size = 4;
  asm_data:
    next();
    for (;;)
    {
      ExprValue e;
      asm_expr(s1, &e);
      if (sec->sh_type != SHT_NOBITS)
      {
        if (size == 4)
        {
          gen_expr32(&e);
        }
        else if (size == 8)
        {
          gen_expr64(&e);
        }
        else
        {
          if (e.sym)
            expect("constant");
          if (size == 1)
            g(e.v);
          else
            gen_le16(e.v);
        }
      }
      else
      {
        ind += size;
      }
      if (tok != ',')
        break;
      next();
    }
    break;
  case TOK_ASMDIR_fill:
  {
    int repeat, size, val, i, j;
    uint8_t repeat_buf[8];
    next();
    repeat = asm_int_expr(s1);
    if (repeat < 0)
    {
      tcc_error("repeat < 0; .fill ignored");
      break;
    }
    size = 1;
    val = 0;
    if (tok == ',')
    {
      next();
      size = asm_int_expr(s1);
      if (size < 0)
      {
        tcc_error("size < 0; .fill ignored");
        break;
      }
      if (size > 8)
        size = 8;
      if (tok == ',')
      {
        next();
        val = asm_int_expr(s1);
      }
    }
    /* XXX: endianness */
    repeat_buf[0] = val;
    repeat_buf[1] = val >> 8;
    repeat_buf[2] = val >> 16;
    repeat_buf[3] = val >> 24;
    repeat_buf[4] = 0;
    repeat_buf[5] = 0;
    repeat_buf[6] = 0;
    repeat_buf[7] = 0;
    for (i = 0; i < repeat; i++)
    {
      for (j = 0; j < size; j++)
      {
        g(repeat_buf[j]);
      }
    }
  }
  break;
  case TOK_ASMDIR_rept:
  {
    int repeat;
    TokenString *init_str;
    next();
    repeat = asm_int_expr(s1);
    init_str = tok_str_alloc();
    while (next(), tok != TOK_ASMDIR_endr)
    {
      if (tok == CH_EOF)
        tcc_error("we at end of file, .endr not found");
      tok_str_add_tok(init_str);
    }
    tok_str_add(init_str, TOK_EOF);
    begin_macro(init_str, 1);
    while (repeat-- > 0)
    {
      tcc_assemble_internal(s1, (parse_flags & PARSE_FLAG_PREPROCESS), global);
      macro_ptr = tok_str_buf(init_str);
    }
    end_macro();
    next();
    break;
  }
  case TOK_ASMDIR_macro:
  {
    /* .macro name [arg1[, arg2, ...]] */
    AsmMacro *m;
    int macro_name;
    next();
    if (tok < TOK_IDENT)
      expect("macro name");
    macro_name = tok;
    m = tcc_mallocz(sizeof(AsmMacro));
    m->name = macro_name;
    m->nb_args = 0;
    next();
    /* parse optional arguments */
    while (tok != TOK_LINEFEED && tok != ';' && tok != CH_EOF)
    {
      if (m->nb_args >= ASM_MACRO_MAX_ARGS)
        tcc_error("too many macro arguments");
      if (tok < TOK_IDENT)
        expect("argument name");
      m->args[m->nb_args++] = tok;
      next();
      if (tok == ',')
        next();
    }
    /* collect macro body until .endm */
    m->body = tok_str_alloc();
    {
      int saved_parse_flags = parse_flags;
      parse_flags |= PARSE_FLAG_ACCEPT_STRAYS; /* allow \arg syntax */
      while (next(), tok != TOK_ASMDIR_endm)
      {
        if (tok == CH_EOF)
          tcc_error("unexpected end of file in .macro");
        if (tok == '\\')
        {
          /* GAS-style \arg - peek next token */
          next();
          if (tok >= TOK_IDENT)
          {
            /* check if it's a macro argument */
            int i, found = 0;
            for (i = 0; i < m->nb_args; i++)
            {
              if (tok == m->args[i])
              {
                found = 1;
                break;
              }
            }
            if (found)
            {
              /* store argument reference (just the arg token, substitution
               * handles it) */
              tok_str_add_tok(m->body);
            }
            else
            {
              /* not an argument, store backslash and token */
              tok_str_add(m->body, '\\');
              tok_str_add_tok(m->body);
            }
          }
          else
          {
            /* backslash followed by non-identifier */
            tok_str_add(m->body, '\\');
            tok_str_add_tok(m->body);
          }
        }
        else
        {
          tok_str_add_tok(m->body);
        }
      }
      parse_flags = saved_parse_flags;
    }
    tok_str_add(m->body, TOK_EOF);
    /* add macro to list */
    m->next = asm_macros;
    asm_macros = m;
    next();
    break;
  }
  case TOK_ASMDIR_endm:
    tcc_error(".endm without .macro");
    break;
  case TOK_ASMDIR_org:
  {
    unsigned long n;
    ExprValue e;
    ElfSym *esym;
    next();
    asm_expr(s1, &e);
    n = e.v;
    esym = elfsym(e.sym);
    if (esym)
    {
      if (esym->st_shndx != cur_text_section->sh_num)
        expect("constant or same-section symbol");
      n += esym->st_value;
    }
    if (n < ind)
      tcc_error("attempt to .org backwards");
    v = 0;
    size = n - ind;
    goto zero_pad;
  }
  break;
  case TOK_ASMDIR_set:
    next();
    tok1 = tok;
    next();
    /* Also accept '.set stuff', but don't do anything with this.
       It's used in GAS to set various features like '.set mips16'.  */
    if (tok == ',')
      set_symbol(s1, tok1);
    break;
  case TOK_ASMDIR_globl:
  case TOK_ASMDIR_global:
  case TOK_ASMDIR_weak:
  case TOK_ASMDIR_hidden:
    tok1 = tok;
    do
    {
      Sym *sym;
      next();
      sym = get_asm_sym(tok, NULL);
      if (tok1 != TOK_ASMDIR_hidden)
        sym->type.t &= ~VT_STATIC;
      if (tok1 == TOK_ASMDIR_weak)
        sym->a.weak = 1;
      else if (tok1 == TOK_ASMDIR_hidden)
        sym->a.visibility = STV_HIDDEN, sym->a.vis_explicit = 1;
      update_storage(sym);
      next();
    } while (tok == ',');
    break;
  case TOK_ASMDIR_string:
  case TOK_ASMDIR_ascii:
  case TOK_ASMDIR_asciz:
  {
    const char *p;
    int i, size, t;

    t = tok;
    next();
    for (;;)
    {
      if (tok != TOK_STR)
        expect("string constant");
      p = tokc.str.data;
      size = tokc.str.size;
      if (t == TOK_ASMDIR_ascii && size > 0)
        size--;
      for (i = 0; i < size; i++)
        g(p[i]);
      next();
      if (tok == ',')
      {
        next();
      }
      else if (tok != TOK_STR)
      {
        break;
      }
    }
  }
  break;
  case TOK_ASMDIR_text:
  case TOK_ASMDIR_data:
  case TOK_ASMDIR_bss:
  {
    char sname[64];
    tok1 = tok;
    n = 0;
    next();
    if (tok != ';' && tok != TOK_LINEFEED)
    {
      n = asm_int_expr(s1);
      next();
    }
    if (n)
      snprintf(sname, sizeof(sname), "%s%d", get_tok_str(tok1, NULL), n);
    else
      snprintf(sname, sizeof(sname), "%s", get_tok_str(tok1, NULL));
    use_section(s1, sname);
  }
  break;
  case TOK_ASMDIR_file:
  {
    const char *p;
    parse_flags &= ~PARSE_FLAG_TOK_STR;
    next();
    if (tok == TOK_PPNUM)
      next();
    if (tok == TOK_PPSTR && tokc.str.data[0] == '"')
    {
      tokc.str.data[tokc.str.size - 2] = 0;
      p = tokc.str.data + 1;
    }
    else if (tok >= TOK_IDENT)
    {
      p = get_tok_str(tok, &tokc);
    }
    else
    {
      skip_to_eol(0);
      break;
    }
    tccpp_putfile(p);
    next();
  }
  break;
  case TOK_ASMDIR_ident:
  {
    char ident[256];

    ident[0] = '\0';
    next();
    if (tok == TOK_STR)
      pstrcat(ident, sizeof(ident), tokc.str.data);
    else
      pstrcat(ident, sizeof(ident), get_tok_str(tok, NULL));
    tcc_warning_c(warn_unsupported)("ignoring .ident %s", ident);
    next();
  }
  break;
  case TOK_ASMDIR_size:
  {
    Sym *sym;

    next();
    sym = asm_label_find(tok);
    if (!sym)
    {
      tcc_error("label not found: %s", get_tok_str(tok, NULL));
    }
    /* XXX .size name,label2-label1 */
    tcc_warning_c(warn_unsupported)("ignoring .size %s,*", get_tok_str(tok, NULL));
    next();
    skip(',');
    while (tok != TOK_LINEFEED && tok != ';' && tok != CH_EOF)
    {
      next();
    }
  }
  break;
  case TOK_ASMDIR_type:
  {
    Sym *sym;
    const char *newtype;
    int st_type;

    next();
    sym = get_asm_sym(tok, NULL);
    next();
    skip(',');
    if (tok == TOK_STR)
    {
      newtype = tokc.str.data;
    }
    else
    {
      if (tok == '@' || tok == '%')
        next();
      newtype = get_tok_str(tok, NULL);
    }

    if (!strcmp(newtype, "function") || !strcmp(newtype, "STT_FUNC"))
    {
      if (IS_ASM_SYM(sym))
        sym->type.t = (sym->type.t & ~VT_ASM) | VT_ASM_FUNC;
      st_type = STT_FUNC;
    set_st_type:
      if (sym->c)
      {
        ElfSym *esym = elfsym(sym);
        esym->st_info = ELFW(ST_INFO)(ELFW(ST_BIND)(esym->st_info), st_type);
#ifdef TCC_TARGET_ARM_THUMB
        /* `.type X, %function` applied after X is already defined (GAS allows
           either order): set the Thumb bit now too. See asm_new_label1. */
        if (st_type == STT_FUNC && esym->st_shndx != SHN_UNDEF && esym->st_shndx < SHN_LORESERVE
            && (esym->st_value & 1) == 0)
          esym->st_value += 1;
#endif
      }
    }
    else if (!strcmp(newtype, "object") || !strcmp(newtype, "STT_OBJECT"))
    {
      st_type = STT_OBJECT;
      goto set_st_type;
    }
    else
      tcc_warning_c(warn_unsupported)("change type of '%s' from 0x%x to '%s' ignored", get_tok_str(sym->v, NULL),
                                      sym->type.t, newtype);

    next();
  }
  break;
  case TOK_ASMDIR_pushsection:
  case TOK_ASMDIR_section:
  {
    char sname[256];
    int old_nb_section = s1->nb_sections;
    int flags = SHF_ALLOC;

    tok1 = tok;
    /* XXX: support more options */
    next();
    sname[0] = '\0';
    while (tok != ';' && tok != TOK_LINEFEED && tok != ',')
    {
      if (tok == TOK_STR)
        pstrcat(sname, sizeof(sname), tokc.str.data);
      else
        pstrcat(sname, sizeof(sname), get_tok_str(tok, NULL));
      next();
    }
    if (tok == ',')
    {
      const char *p;
      /* skip section options */
      next();
      if (tok != TOK_STR)
        expect("string constant");
      for (p = tokc.str.data; *p; ++p)
      {
        if (*p == 'w')
          flags |= SHF_WRITE;
        if (*p == 'x')
          flags |= SHF_EXECINSTR;
      }
      next();
      if (tok == ',')
      {
        next();
        if (tok == '@' || tok == '%')
          next();
        next();
      }
    }
    last_text_section = cur_text_section;
    if (tok1 == TOK_ASMDIR_section)
      use_section(s1, sname);
    else
      push_section(s1, sname);
    /* If we just allocated a new section reset its alignment to
       1.  new_section normally acts for GCC compatibility and
       sets alignment to PTR_SIZE.  The assembler behaves different. */
    if (old_nb_section != s1->nb_sections)
    {
      cur_text_section->sh_addralign = 1;
      cur_text_section->sh_flags = flags;
    }
  }
  break;
  case TOK_ASMDIR_previous:
  {
    Section *sec;
    next();
    if (!last_text_section)
      tcc_error("no previous section referenced");
    sec = cur_text_section;
    use_section1(s1, last_text_section);
    last_text_section = sec;
  }
  break;
  case TOK_ASMDIR_popsection:
    next();
    pop_section(s1);
    break;
#ifdef TCC_TARGET_I386
  case TOK_ASMDIR_code16:
  {
    next();
    s1->seg_size = 16;
  }
  break;
  case TOK_ASMDIR_code32:
  {
    next();
    s1->seg_size = 32;
  }
  break;
#endif
#ifdef TCC_TARGET_X86_64
  /* added for compatibility with GAS */
  case TOK_ASMDIR_code64:
    next();
    break;
#endif
#ifdef TCC_TARGET_RISCV64
  case TOK_ASMDIR_option:
    next();
    switch (tok)
    {
    case TOK_ASM_rvc:   /* Will be deprecated soon in favor of arch */
    case TOK_ASM_norvc: /* Will be deprecated soon in favor of arch */
    case TOK_ASM_pic:
    case TOK_ASM_nopic:
    case TOK_ASM_relax:
    case TOK_ASM_norelax:
    case TOK_ASM_push:
    case TOK_ASM_pop:
      /* TODO: unimplemented */
      next();
      break;
    case TOK_ASM_arch:
      /* TODO: unimplemented, requires extra parsing */
      tcc_error("unimp .option '.%s'", get_tok_str(tok, NULL));
      break;
    default:
      tcc_error("unknown .option '.%s'", get_tok_str(tok, NULL));
      break;
    }
    break;
#endif
  /* TODO: Implement symvar support. FreeBSD >= 14 needs this */
  case TOK_ASMDIR_symver:
    next();
    next();
    skip(',');
    next();
    skip('@');
    next();
    break;
  case TOK_ASMDIR_syntax:
    next();
    next();
    break;
  case TOK_ASMDIR_thumb:
    next();
    break;
  case TOK_ASMDIR_if:
  case TOK_ASMDIR_ifdef:
  case TOK_ASMDIR_ifndef:
  {
    /* GAS conditional assembly: `.if expr`, `.ifdef sym`, `.ifndef sym`. */
    int cond;
    tok1 = tok;
    next();
    if (tok1 == TOK_ASMDIR_if)
      cond = asm_int_expr(s1) != 0;
    else
    {
      Sym *label = asm_label_find(tok);
      ElfSym *esym = label ? elfsym(label) : NULL;
      cond = esym && esym->st_shndx != SHN_UNDEF;
      if (tok1 == TOK_ASMDIR_ifndef)
        cond = !cond;
      next();
    }
    if (!cond)
      asm_skip_conditional(1);
    break;
  }
  case TOK_ASMDIR_else:
    /* reached at the end of the branch that was taken */
    asm_skip_conditional(0);
    break;
  case TOK_ASMDIR_endif:
    next();
    break;
  case TOK_ASMDIR_cpu:
  case TOK_ASMDIR_arch:
    /* `.cpu cortex-m33` / `.arch armv7-m`: the target is fixed by the
       command line; the name lexes as several tokens because of the hyphen,
       so skip the line. */
    next();
    while (tok != ';' && tok != TOK_LINEFEED && tok != CH_EOF)
      next();
    break;
  case TOK_ASMDIR_incbin:
  {
    /* `.incbin "file"[, skip[, count]]` — the file's bytes, verbatim.  Found
       like GAS does: as given, then beside the including file, then on the
       -I path. */
    char name[1024], path[1024];
    long skip_bytes = 0, count = -1, got;
    FILE *f = NULL;
    int i;
    next();
    if (tok != TOK_STR)
      expect("file name");
    pstrcpy(name, sizeof(name), tokc.str.data);
    next();
    if (tok == ',')
    {
      next();
      skip_bytes = asm_int_expr(s1);
      if (tok == ',')
      {
        next();
        count = asm_int_expr(s1);
      }
    }
    pstrcpy(path, sizeof(path), name);
    f = fopen(name, "rb");
    if (!f && !IS_ABSPATH(name))
    {
      const char *slash = strrchr(file->filename, '/');
      int dl = slash ? (int)(slash + 1 - file->filename) : 0;
      if (dl >= (int)sizeof(path))
        dl = sizeof(path) - 1;
      memcpy(path, file->filename, dl);
      path[dl] = '\0';
      pstrcat(path, sizeof(path), name);
      f = fopen(path, "rb");
    }
    for (i = 0; !f && !IS_ABSPATH(name) && i < s1->nb_include_paths; i++)
    {
      pstrcpy(path, sizeof(path), s1->include_paths[i]);
      pstrcat(path, sizeof(path), "/");
      pstrcat(path, sizeof(path), name);
      f = fopen(path, "rb");
    }
    if (!f)
      tcc_error("can't find .incbin file '%s'", name);
    /* A dependency like an #include (-MD), so an edited blob rebuilds. */
    if (s1->gen_deps)
      dynarray_add(&s1->target_deps, &s1->nb_target_deps, tcc_strdup(path));
    if (skip_bytes > 0 && fseek(f, skip_bytes, SEEK_SET) != 0)
      tcc_error(".incbin: can't skip %ld bytes in '%s'", skip_bytes, path);
    {
      /* Per chunk what g() does per byte: nothing under nocode_wanted, only
         advance ind in a dry run, else write and advance. */
      char *buf = tcc_malloc(65536);
      for (;;)
      {
        long want = 65536;
        if (count >= 0 && count < want)
          want = count;
        if (want == 0)
          break;
        got = (long)fread(buf, 1, want, f);
        if (!nocode_wanted)
        {
          if (!tcc_gen_machine_dry_run_is_active())
          {
            if (ind + got > cur_text_section->data_allocated)
              section_realloc(cur_text_section, ind + got);
            memcpy(cur_text_section->data + ind, buf, got);
          }
          ind += got;
        }
        if (count >= 0)
          count -= got;
        if (got < want)
          break;
      }
      tcc_free(buf);
    }
    fclose(f);
    if (count > 0)
      tcc_error(".incbin: '%s' is shorter than the requested count", path);
    break;
  }
#ifdef TCC_TARGET_ARM
  case TOK_ASMDIR_fpu:
  {
    /* `.fpu <name>` — enable the named FP unit's instruction encodings.
       The name (e.g. fpv5-sp-d16) lexes as several tokens because of the
       hyphens, so rebuild it the same way `.section` rebuilds its name. */
    char fpu_name[64];
    next();
    fpu_name[0] = '\0';
    while (tok != ';' && tok != TOK_LINEFEED && tok != CH_EOF)
    {
      if (tok == TOK_STR)
        pstrcat(fpu_name, sizeof(fpu_name), tokc.str.data);
      else
        pstrcat(fpu_name, sizeof(fpu_name), get_tok_str(tok, NULL));
      next();
    }
    tcc_asm_set_fpu(fpu_name);
  }
  break;
#endif
  case TOK_ASMDIR_thumb_func:
    next();
    /* GAS accepts both `.thumb_func` (affects next label) and
       `.thumb_func <symbol>` (marks the given symbol as Thumb). */
    if (tok == TOK_LINEFEED || tok == ';')
      s1->thumb_func = 1;
    else
    {
      s1->thumb_func = tok;
      next();
    }
    break;
  default:
    tcc_error("unknown assembler directive '.%s'", get_tok_str(tok, NULL));
    break;
  }
}

/* assemble a file */
static int tcc_assemble_internal(TCCState *s1, int do_preprocess, int global)
{
  int opcode;
  int saved_parse_flags = parse_flags;

  parse_flags = PARSE_FLAG_ASM_FILE | PARSE_FLAG_TOK_STR;
  if (do_preprocess)
    parse_flags |= PARSE_FLAG_PREPROCESS;
  for (;;)
  {
    next();

    if (tok == TOK_EOF)
      break;
    tcc_debug_line(s1);
    parse_flags |= PARSE_FLAG_LINEFEED; /* XXX: suppress that hack */
  redo:
    if (tok == '#')
    {
      /* horrible gas comment */
      while (tok != TOK_LINEFEED)
        next();
    }
    else if (tok >= TOK_ASMDIR_FIRST && tok <= TOK_ASMDIR_LAST)
    {
      asm_parse_directive(s1, global);
    }
    else if (tok == TOK_PPNUM)
    {
      const char *p;
      int n;
      p = tokc.str.data;
      n = strtoul(p, (char **)&p, 10);
      if (*p != '\0')
        expect("':'");
      /* new local label */
      asm_new_label(s1, asm_get_local_label_name(s1, n), 1);
      next();
      skip(':');
      goto redo;
    }
    else if (tok >= TOK_IDENT)
    {
      /* instruction or label */
      opcode = tok;
      next();
      if (tok == ':')
      {
        /* new label */
        asm_new_label(s1, opcode, 0);
        next();
        goto redo;
      }
      else if (tok == '=')
      {
        set_symbol(s1, opcode);
        goto redo;
      }
      else
      {
        /* check for macro expansion */
        AsmMacro *m = asm_macro_find(opcode);
        if (m)
        {
          /* expand macro */
          TokenString *arg_strs[ASM_MACRO_MAX_ARGS];
          TokenString *expanded;
          const int *body_ptr;
          int arg_count = 0;
          int i, t;
          CValue cv;

          /* initialize arg_strs */
          for (i = 0; i < ASM_MACRO_MAX_ARGS; i++)
            arg_strs[i] = NULL;

          /* collect arguments - each argument can be multiple tokens */
          while (tok != TOK_LINEFEED && tok != ';' && tok != CH_EOF)
          {
            if (arg_count >= m->nb_args)
              tcc_error("too many arguments for macro '%s'", get_tok_str(m->name, NULL));
            arg_strs[arg_count] = tok_str_alloc();
            /* collect tokens until comma or end of line */
            while (tok != ',' && tok != TOK_LINEFEED && tok != ';' && tok != CH_EOF)
            {
              tok_str_add_tok(arg_strs[arg_count]);
              next();
            }
            tok_str_add(arg_strs[arg_count], TOK_EOF);
            arg_count++;
            if (tok == ',')
              next();
          }
          if (arg_count < m->nb_args)
            tcc_error("not enough arguments for macro '%s'", get_tok_str(m->name, NULL));

          /* build expanded token string with argument substitution */
          expanded = tok_str_alloc();
          body_ptr = tok_str_buf(m->body);
          for (;;)
          {
            tok_get(&t, &body_ptr, &cv);
            if (t == TOK_EOF)
              break;
            /* skip line number tokens */
            if (t == TOK_LINENUM)
            {
              continue;
            }
            /* check if this token is a macro argument */
            for (i = 0; i < m->nb_args; i++)
            {
              if (t == m->args[i])
              {
                /* substitute with argument tokens */
                const int *arg_ptr = tok_str_buf(arg_strs[i]);
                int at;
                CValue acv;
                for (;;)
                {
                  tok_get(&at, &arg_ptr, &acv);
                  if (at == TOK_EOF)
                    break;
                  if (at == TOK_LINENUM)
                    continue;
                  tok_str_add2(expanded, at, &acv);
                }
                goto next_body_tok;
              }
            }
            /* not an argument, copy token as-is */
            tok_str_add2(expanded, t, &cv);
          next_body_tok:;
          }
          tok_str_add(expanded, TOK_EOF);

          /* execute expanded macro */
          begin_macro(expanded, 1);
          tcc_assemble_internal(s1, (parse_flags & PARSE_FLAG_PREPROCESS), global);
          end_macro();

          /* free arg strings */
          for (i = 0; i < arg_count; i++)
          {
            if (arg_strs[i])
              tok_str_free(arg_strs[i]);
          }
          /* skip end-of-line check, continue to next iteration */
          parse_flags &= ~PARSE_FLAG_LINEFEED;
          continue;
        }
        else
        {
          asm_opcode(s1, opcode);
        }
      }
    }
    /* end of line */
    if (tok != ';' && tok != TOK_LINEFEED)
      expect("end of line");
    parse_flags &= ~PARSE_FLAG_LINEFEED; /* XXX: suppress that hack */
  }

  parse_flags = saved_parse_flags;
  return 0;
}

/* Assemble the current file */
ST_FUNC int tcc_assemble(TCCState *s1, int do_preprocess)
{
  int ret;
  arm_init(s1);
  tcc_debug_start(s1);
  /* default section is text */
  cur_text_section = text_section;
  ind = cur_text_section->data_offset;
  nocode_wanted = 0;
  ret = tcc_assemble_internal(s1, do_preprocess, 1);
  cur_text_section->data_offset = ind;
  tcc_debug_end(s1);
  return ret;
}

/********************************************************************/
/* GCC inline asm support */

/* assemble the string 'str' in the current C compilation unit without
   C preprocessing. */
static void tcc_assemble_inline(TCCState *s1, const char *str, int len, int global)
{
  const int *saved_macro_ptr = macro_ptr;
  int dotid = set_idnum('.', IS_ID);
#ifndef TCC_TARGET_RISCV64
  int dolid = set_idnum('$', 0);
#endif

  tcc_open_bf(s1, ":asm:", len);
  memcpy(file->buffer, str, len);
  macro_ptr = NULL;
  tcc_assemble_internal(s1, 0, global);
  tcc_close();

#ifndef TCC_TARGET_RISCV64
  set_idnum('$', dolid);
#endif
  set_idnum('.', dotid);
  macro_ptr = saved_macro_ptr;
}

/* find a constraint by its number or id (gcc 3 extended
   syntax). return -1 if not found. Return in *pp in char after the
   constraint */
ST_FUNC int find_constraint(ASMOperand *operands, int nb_operands, const char *name, const char **pp)
{
  int index;
  TokenSym *ts;
  const char *p;

  if (isnum(*name))
  {
    index = 0;
    while (isnum(*name))
    {
      index = (index * 10) + (*name) - '0';
      name++;
    }
    if ((unsigned)index >= nb_operands)
      index = -1;
  }
  else if (*name == '[')
  {
    name++;
    p = strchr(name, ']');
    if (p)
    {
      ts = tok_alloc(name, p - name);
      for (index = 0; index < nb_operands; index++)
      {
        if (operands[index].id == ts->tok)
          goto found;
      }
      index = -1;
    found:
      name = p + 1;
    }
    else
    {
      index = -1;
    }
  }
  else
  {
    index = -1;
  }
  if (pp)
    *pp = name;
  return index;
}

static void subst_asm_operands(ASMOperand *operands, int nb_operands, CString *out_str, const char *str)
{
  int c, index, modifier;
  ASMOperand *op;
  SValue sv;

  for (;;)
  {
    c = *str++;
    if (c == '%')
    {
      if (*str == '%')
      {
        str++;
        goto add_char;
      }
      modifier = 0;
      if (*str == 'c' || *str == 'n' || *str == 'b' || *str == 'w' || *str == 'h' || *str == 'k' || *str == 'q' ||
          *str == 'l' ||
#ifdef TCC_TARGET_RISCV64
          *str == 'z' ||
#endif
          /* P in GCC would add "@PLT" to symbol refs in PIC mode,
             and make literal operands not be decorated with '$'.  */
          *str == 'P')
        modifier = *str++;
      index = find_constraint(operands, nb_operands, str, &str);
      if (index < 0)
        tcc_error("invalid operand reference after %%");
      op = &operands[index];
      if (modifier == 'l')
      {
        cstr_cat(out_str, get_tok_str(op->is_label, NULL), -1);
      }
      else
      {
        sv = *op->vt;
        if (op->reg >= 0)
        {
          sv.r = op->reg;
          if ((op->vt->r & VT_VALMASK) == VT_LLOCAL && op->is_memory)
            sv.r |= VT_LVAL;
        }
        subst_asm_operand(out_str, &sv, modifier);
      }
    }
    else
    {
    add_char:
      cstr_ccat(out_str, c);
      if (c == '\0')
        break;
    }
  }
}

/* Lower a fully parsed GCC-style inline asm block.
 * This is shared between the classic front-end path and IR codegen.
 */
ST_FUNC void tcc_asm_emit_inline(ASMOperand *operands, int nb_operands, int nb_outputs, int nb_labels,
                                 uint8_t *clobber_regs, const uint8_t *reserved_regs, const char *asm_str, int asm_len,
                                 int must_subst)
{
  int out_reg;
  Section *sec;
  CString astr, astr1;

  if (!operands)
    tcc_error("tcc_asm_emit_inline: NULL operands");
  if (!asm_str || asm_len < 0)
    tcc_error("tcc_asm_emit_inline: invalid asm string");

  /* compute constraints */
  asm_compute_constraints(operands, nb_operands, nb_outputs, clobber_regs, reserved_regs, &out_reg);

  /* generate loads -- before the substitution: the prolog's register saves
   * move SP, and an SP-based memory operand has to account for them */
  asm_gen_code(operands, nb_operands, nb_outputs, 0, clobber_regs, out_reg);

  cstr_new_s(&astr);
  cstr_cat(&astr, asm_str, asm_len + 1);

  /* substitute operands in the asm string */
  if (must_subst)
  {
    cstr_new_s(&astr1);
    cstr_cat(&astr1, astr.data, astr.size);
    cstr_reset(&astr);
    subst_asm_operands(operands, nb_operands + nb_labels, &astr, astr1.data);
    cstr_free_s(&astr1);
  }

  /* We don't allow switching section within inline asm to bleed out. */
  sec = cur_text_section;
  tcc_assemble_inline(tcc_state, astr.data, astr.size - 1, 0);
  if (sec != cur_text_section)
  {
    tcc_warning("inline asm tries to change current section");
    use_section1(tcc_state, sec);
  }

  /* store output values */
  asm_gen_code(operands, nb_operands, nb_outputs, 1, clobber_regs, out_reg);

  cstr_free_s(&astr);
}

static void maybe_substitute_inline_const_arg(SValue *sv)
{
  if (!tcc_state->in_inline_expansion)
    return;
  if ((sv->r & (VT_VALMASK | VT_LVAL)) != (VT_LOCAL | VT_LVAL))
    return;

  for (int i = 0; i < tcc_state->inline_const_arg_count; i++)
  {
    if (tcc_state->inline_const_args[i].vreg == sv->vr && tcc_state->inline_const_args[i].stack_offset == sv->c.i)
    {
      *sv = tcc_state->inline_const_args[i].value;
      return;
    }
  }
}

static void parse_asm_operands(ASMOperand *operands, int *nb_operands_ptr, int is_output)
{
  ASMOperand *op;
  int nb_operands;
  char *astr;

  if (tok != ':')
  {
    nb_operands = *nb_operands_ptr;
    for (;;)
    {
      if (nb_operands >= MAX_ASM_OPERANDS)
        tcc_error("too many asm operands");
      op = &operands[nb_operands++];
      op->id = 0;
      op->reg = -1;
      if (tok == '[')
      {
        next();
        if (tok < TOK_IDENT)
          expect("identifier");
        op->id = tok;
        next();
        skip(']');
      }
      astr = parse_mult_str("string constant")->data;
      pstrcpy(op->constraint, sizeof op->constraint, astr);
      skip('(');
      gexpr();
      /* Only input operands may take the inlined constant argument.  An output
       * operand must stay an lvalue (test_lvalue() below), and the operand's
       * current value is what the asm must see -- the call.c side already drops
       * the map entry when the body writes the parameter, so a surviving entry
       * means the value still equals the argument. */
      if (!is_output)
        maybe_substitute_inline_const_arg(vtop);
      /* Record a local register variable's register now, while its Sym is in
       * scope: by the time the IR lowers the asm the value may live in any
       * register (or be spilled) and the function's locals have been popped. */
      op->regvar = 0;
      if (vtop->sym && (vtop->r & (VT_VALMASK | VT_LVAL)) == (VT_LOCAL | VT_LVAL) && !(vtop->sym->r & VT_PARAM) &&
          (vtop->sym->r & VT_VALMASK) < VT_CONST)
        op->regvar = 1 + (vtop->sym->r & VT_VALMASK);
      if (is_output)
      {
        if (!(vtop->type.t & VT_ARRAY))
          test_lvalue();
      }
      else
      {
        /* we want to avoid LLOCAL case, except when the 'm'
           constraint is used. Note that it may come from
           register storage, so we need to convert (reg)
           case */
        if ((vtop->r & VT_LVAL) && ((vtop->r & VT_VALMASK) == VT_LLOCAL || (vtop->r & VT_VALMASK) < VT_CONST) &&
            !strchr(op->constraint, 'm'))
        {
          gv(RC_INT);
        }
        /* A comparison or &&/|| result lives in the flags or a jump chain, not
         * in a value: `"r"(a > b)` handed the asm the left operand's register
         * (and the next operand's code could clobber the flags).  Make it the
         * 0/1 value now. */
        else if ((vtop->r & VT_VALMASK) == VT_CMP || (vtop->r & VT_VALMASK) == VT_JMP ||
                 (vtop->r & VT_VALMASK) == VT_JMPI)
        {
          gv(RC_INT);
        }
      }
      op->vt = vtop;
      skip(')');
      if (tok == ',')
      {
        next();
      }
      else
      {
        break;
      }
    }
    *nb_operands_ptr = nb_operands;
  }
}

/* parse the GCC asm() instruction */
ST_FUNC void asm_instr(void)
{
  CString astr, *astr1;

  ASMOperand operands[MAX_ASM_OPERANDS];
  int nb_outputs, nb_operands, i, must_subst, out_reg, nb_labels;
  uint8_t clobber_regs[NB_ASM_REGS];
  Section *sec;

  /* since we always generate the asm() instruction, we can ignore
     volatile */
  while (tok == TOK_VOLATILE1 || tok == TOK_VOLATILE2 || tok == TOK_VOLATILE3 || tok == TOK_GOTO)
  {
    next();
  }

  astr1 = parse_asm_str();
  cstr_new_s(&astr);
  cstr_cat(&astr, astr1->data, astr1->size);

  nb_operands = 0;
  nb_outputs = 0;
  nb_labels = 0;
  must_subst = 0;
  memset(clobber_regs, 0, sizeof(clobber_regs));
  if (tok == ':')
  {
    next();
    must_subst = 1;
    /* output args */
    parse_asm_operands(operands, &nb_operands, 1);
    nb_outputs = nb_operands;
    if (tok == ':')
    {
      next();
      if (tok != ')')
      {
        /* input args */
        parse_asm_operands(operands, &nb_operands, 0);
        if (tok == ':')
        {
          /* clobber list */
          /* XXX: handle registers */
          next();
          for (;;)
          {
            if (tok == ':')
              break;
            if (tok == ')')
              break;
            if (tok != TOK_STR)
              expect("string constant");
            asm_clobber(clobber_regs, tokc.str.data);
            next();
            if (tok == ',')
            {
              next();
            }
            else
            {
              break;
            }
          }
        }
        if (tok == ':')
        {
          /* goto labels */
          next();
          for (;;)
          {
            Sym *csym;
            int asmname;
            if (nb_operands + nb_labels >= MAX_ASM_OPERANDS)
              tcc_error("too many asm operands");
            if (tok < TOK_UIDENT)
              expect("label identifier");
            operands[nb_operands + nb_labels++].id = tok;

            csym = label_find(tok);
            if (!csym)
            {
              csym = label_push(&global_label_stack, tok, LABEL_FORWARD);
            }
            else
            {
              if (csym->r == LABEL_DECLARED)
                csym->r = LABEL_FORWARD;
            }
            next();
            asmname = asm_get_prefix_name(tcc_state, "LG.", ++asmgoto_n);
            if (!csym->c)
              put_extern_sym2(csym, SHN_UNDEF, 0, 0, 1);
            get_asm_sym(asmname, csym);
            operands[nb_operands + nb_labels - 1].is_label = asmname;

            if (tok != ',')
              break;
            next();
          }
        }
      }
    }
  }
  skip(')');
  /* NOTE: we do not eat the ';' so that we can restore the current
     token after the assembler parsing */
  if (tok != ';')
    expect("';'");

  /* IR-only mode: inline asm still relies on legacy backend load/store
     operand materialization and physical register state. */
  if (tcc_state->ir)
  {
    /* Record inline asm for IR codegen lowering.
     * Emit marker ops so liveness/regalloc see uses/defs across the barrier.
     */
    int asm_len = astr.size - 1;

    /* asm_compute_constraints() — which derives op->is_rw from a '+' constraint
     * modifier — only runs later at codegen time (tcc_asm_emit_inline).  The IR
     * marker emission below tests operands[i].is_rw to decide whether a "+r"
     * output also needs an ASM_INPUT (read) marker.  Without computing it here,
     * is_rw is read from uninitialized operand stack memory, so the read marker
     * for "+r" operands is dropped intermittently (it survives on the host but
     * vanishes on the self-hosted -O1 build) — leaving the operand load missing
     * and the post-asm store reading an uninitialized pointer slot. */
    for (i = 0; i < nb_outputs; ++i)
    {
      const char *cstr = operands[i].constraint;
      operands[i].is_rw = 0;
      for (; *cstr == '=' || *cstr == '&' || *cstr == '+' || *cstr == '%'; ++cstr)
        if (*cstr == '+')
          operands[i].is_rw = 1;
      /* `"=m"(*p)` / `"=r"(*p)`: the asm (or the store after it) writes
       * through p, so p is an INPUT of the asm, but the only IR mention of it
       * was the ASM_OUTPUT marker's deref destination -- which dead-code
       * passes do not count as a use of p.  `T0 <-- P0` was deleted and the
       * store went through a register nobody set (or, at -O2, one an input
       * had just been loaded into).  Such an output gets a read marker too,
       * like a "+" one: a deref SOURCE is a use everywhere. */
      SValue *ov = operands[i].vt;
      operands[i].addr_read = !operands[i].is_rw && (ov->r & VT_LVAL) && (ov->r & VT_VALMASK) < VT_CONST;
    }

#ifdef TCC_TARGET_ARM
    /* System instructions only: a call the backend emits as them, which no
     * pass has to treat as an asm statement (asm_machine_call_name). */
    char mc_name[64];
    if (!nb_labels && TCC_OPT(tcc_state, optimize) > 0 && !tcc_ir_opt_pass_disabled("asm:machine_call") &&
        asm_machine_call_name(astr.data, operands, nb_operands, nb_outputs, clobber_regs, mc_name, sizeof(mc_name)))
    {
      CType ret = {.t = VT_VOID};
      if (nb_outputs)
      {
        ret = operands[0].vt->type;
        ret.t &= ~(VT_CONSTANT | VT_VOLATILE);
      }
      if (nb_operands > nb_outputs)
      {
        vpushv(operands[nb_outputs].vt);
        gv(RC_INT);
      }
      gen_internal_call(mc_name, nb_operands - nb_outputs, &ret);
      if (nb_outputs)
      {
        vpushv(operands[0].vt);
        vswap();
        vstore();
      }
      vpop();
      cstr_free_s(&astr);
      next();
      for (i = 0; i < nb_operands; i++)
        vpop();
      return;
    }
#endif
    int inline_asm_id = tcc_ir_add_inline_asm(tcc_state->ir, astr.data, asm_len, must_subst, operands, nb_operands,
                                              nb_outputs, nb_labels, clobber_regs);

    /* A memory operand ("m" and kin) hands the asm the OBJECT, not a value:
     * its marker below must not be answered from a tracked store, nor the
     * store before it deleted for want of a reader.  That is exactly a volatile
     * access, which every forwarding and dead-store pass already respects --
     * an "m" input on `buf[1] = x;` was forwarded x and the store dropped, so
     * the asm read a slot nobody had written (and, at -O1, no slot at all).
     * Only the marker operands are marked; the codegen reads ia->values. */
    for (i = 0; i < nb_operands; ++i)
    {
      const char *cstr = operands[i].constraint;
      while (*cstr == '=' || *cstr == '&' || *cstr == '+' || *cstr == '%')
        ++cstr;
      if (!strpbrk(cstr, "mQoV") || !(operands[i].vt->r & VT_LVAL))
        continue;
      operands[i].vt->volatile_access = 1;
      /* ...and the object must HAVE an address: a scalar local or parameter
       * would otherwise live in a register, and `ldr r0, %0` printed `ldr r0,
       * r4`.  Exactly what `&v` does (unary.c). */
      SValue *ov = operands[i].vt;
      if (ov->sym && ((ov->r & VT_VALMASK) == VT_LOCAL || (ov->r & VT_PARAM)))
      {
        ov->sym->a.addrtaken = 1;
        tcc_ir_set_addrtaken(tcc_state->ir, ov->sym->vreg);
      }
    }

    /* Read operands (inputs + read/write outputs) */
    for (i = 0; i < nb_outputs; ++i)
    {
      if (operands[i].is_rw || operands[i].addr_read)
        tcc_ir_put(tcc_state->ir, TCCIR_OP_ASM_INPUT, operands[i].vt, NULL, NULL);
    }
    for (i = nb_outputs; i < nb_operands; ++i)
    {
      tcc_ir_put(tcc_state->ir, TCCIR_OP_ASM_INPUT, operands[i].vt, NULL, NULL);
    }

    tcc_ir_put_inline_asm(tcc_state->ir, inline_asm_id);

    /* Written operands (outputs) */
    for (i = 0; i < nb_outputs; ++i)
    {
      tcc_ir_put(tcc_state->ir, TCCIR_OP_ASM_OUTPUT, NULL, NULL, operands[i].vt);
    }

    cstr_free_s(&astr);

    /* restore the current C token */
    next();

    /* free the value stack entries for asm operands */
    for (i = 0; i < nb_operands; i++)
      vpop();
    return;
  }

  /* compute constraints */
  asm_compute_constraints(operands, nb_operands, nb_outputs, clobber_regs, NULL, &out_reg);

  /* substitute the operands in the asm string. No substitution is
     done if no operands (GCC behaviour) */
#ifdef ASM_DEBUG
  printf("asm: \"%s\"\n", (char *)astr.data);
#endif
  if (must_subst)
  {
    cstr_reset(astr1);
    cstr_cat(astr1, astr.data, astr.size);
    cstr_reset(&astr);
    subst_asm_operands(operands, nb_operands + nb_labels, &astr, astr1->data);
  }

#ifdef ASM_DEBUG
  printf("subst_asm: \"%s\"\n", (char *)astr.data);
#endif

  /* generate loads */
  asm_gen_code(operands, nb_operands, nb_outputs, 0, clobber_regs, out_reg);

  /* We don't allow switching section within inline asm to
     bleed out to surrounding code.  */
  sec = cur_text_section;
  /* assemble the string with tcc internal assembler */
  tcc_assemble_inline(tcc_state, astr.data, astr.size - 1, 0);
  cstr_free_s(&astr);
  if (sec != cur_text_section)
  {
    tcc_warning("inline asm tries to change current section");
    use_section1(tcc_state, sec);
  }

  /* restore the current C token */
  next();

  /* store the output values if needed */
  asm_gen_code(operands, nb_operands, nb_outputs, 1, clobber_regs, out_reg);

  /* free everything */
  for (i = 0; i < nb_operands; i++)
  {
    vpop();
  }
}

ST_FUNC void asm_global_instr(void)
{
  CString *astr;
  int saved_nocode_wanted = nocode_wanted;

  /* Global asm blocks are always emitted.  */
  nocode_wanted = 0;
  next();
  astr = parse_asm_str();
  skip(')');
  /* NOTE: we do not eat the ';' so that we can restore the current
     token after the assembler parsing */
  if (tok != ';')
    expect("';'");

#ifdef ASM_DEBUG
  printf("asm_global: \"%s\"\n", (char *)astr->data);
#endif
  cur_text_section = text_section;
  ind = cur_text_section->data_offset;

  /* assemble the string with tcc internal assembler */
  tcc_assemble_inline(tcc_state, astr->data, astr->size - 1, 1);

  cur_text_section->data_offset = ind;

  /* restore the current C token */
  next();

  nocode_wanted = saved_nocode_wanted;
}

/********************************************************/
#else
ST_FUNC int tcc_assemble(TCCState *s1, int do_preprocess)
{
  tcc_error("asm not supported");
}

ST_FUNC void asm_instr(void)
{
  tcc_error("inline asm() not supported");
}

ST_FUNC void asm_global_instr(void)
{
  tcc_error("inline asm() not supported");
}
#endif /* CONFIG_TCC_ASM */
