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

/* primary.c -- Primary expression parsing.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* Primary expression parser - extracted from unary() to reduce stack frame
   size on the recursive path.  Returns 1 for early-return (sizeof/alignof
   type-only operand), 0 otherwise. */
__attribute__((noinline)) int unary_primary(void)
{
  int n, t, align, r;
  CType type;
  Sym *s;
  AttributeDef ad;

  type.ref = NULL;
  /* XXX: GCC 2.95.3 does not generate a table although it should be
     better here */
tok_next:
  switch (tok)
  {
  case TOK_EXTENSION:
    next();
    goto tok_next;
  case TOK_LCHAR:
#ifdef TCC_TARGET_PE
    t = VT_SHORT | VT_UNSIGNED;
    goto push_tokc;
#endif
  case TOK_CINT:
  case TOK_CCHAR:
    t = VT_INT;
  push_tokc:
    type.t = t;
    vsetc(&type, VT_CONST, &tokc);
    next();
    break;
  case TOK_CINT_I:
  {
    /* GNU extension: integer imaginary constant (e.g., 200i).
     * Creates a _Complex int constant with real=0, imag=value.
     * Packed representation: real in low 32, imag in high 32 bits of CValue.i */
    CValue cv;
    cv.i = (uint64_t)(uint32_t)tokc.i << 32;
    type.t = VT_INT | VT_COMPLEX;
    vsetc(&type, VT_CONST, &cv);
    next();
    break;
  }
  case TOK_CFLOAT_I:
  {
    /* GNU extension: float imaginary constant (e.g., 1.0fi).
     * Creates a _Complex float constant with real=0, imag=value.
     * Packed: two floats in CValue.i (real at low 32, imag at high 32) */
    CValue cv;
    union
    {
      float f;
      uint32_t u;
    } imag_bits;
    imag_bits.f = tokc.f;
    cv.i = (uint64_t)imag_bits.u << 32;
    type.t = VT_FLOAT | VT_COMPLEX;
    vsetc(&type, VT_CONST, &cv);
    next();
    break;
  }
  case TOK_CDOUBLE_I:
  {
    /* GNU extension: double imaginary constant (e.g., 1.0i).
     * Creates a _Complex double with real=0.0, imag=value.
     * Packed representation: bytes [0:7] = real (double), bytes [8:15] = imag (double).
     * This matches the C memory layout {real, imag} and fits in CValue (16 bytes on x86_64). */
    CValue cv;
    memset(&cv, 0, sizeof(cv));
    double _real = 0.0, _imag = tokc.d;
    memcpy(&cv, &_real, 8);
    memcpy((char *)&cv + 8, &_imag, 8);
    type.t = VT_DOUBLE | VT_COMPLEX;
    vsetc(&type, VT_CONST, &cv);
    next();
    break;
  }
  case TOK_CLDOUBLE_I:
  {
    CValue cv;
    memset(&cv, 0, sizeof(cv));
#ifdef TCC_USING_DOUBLE_FOR_LDOUBLE
    {
      double _real = 0.0, _imag = tokc.d;
      memcpy(&cv, &_real, 8);
      memcpy((char *)&cv + 8, &_imag, 8);
    }
    type.t = VT_DOUBLE | VT_LONG | VT_COMPLEX;
#else
    cv.ld = tokc.ld;
    type.t = VT_LDOUBLE | VT_COMPLEX;
#endif
    vsetc(&type, VT_CONST, &cv);
    next();
    break;
  }
  case TOK_CUINT:
    t = VT_INT | VT_UNSIGNED;
    goto push_tokc;
  case TOK_CLLONG:
    t = VT_LLONG;
    goto push_tokc;
  case TOK_CULLONG:
    t = VT_LLONG | VT_UNSIGNED;
    goto push_tokc;
  case TOK_CFLOAT:
    t = VT_FLOAT;
    goto push_tokc;
  case TOK_CDOUBLE:
    t = VT_DOUBLE;
    goto push_tokc;
  case TOK_CLDOUBLE:
#ifdef TCC_USING_DOUBLE_FOR_LDOUBLE
    t = VT_DOUBLE | VT_LONG;
#else
    t = VT_LDOUBLE;
#endif
    goto push_tokc;
  case TOK_CLONG:
    t = (LONG_SIZE == 8 ? VT_LLONG : VT_INT) | VT_LONG;
    goto push_tokc;
  case TOK_CULONG:
    t = (LONG_SIZE == 8 ? VT_LLONG : VT_INT) | VT_LONG | VT_UNSIGNED;
    goto push_tokc;
  case TOK___FUNCTION__:
    if (!gnu_ext)
      goto tok_identifier;
    /* fall thru */
  case TOK___FUNC__:
    tok = TOK_STR;
    cstr_reset(&tokcstr);
    cstr_cat(&tokcstr, funcname, 0);
    tokc.str.size = tokcstr.size;
    tokc.str.data = tokcstr.data;
    goto case_TOK_STR;
  case TOK_LSTR:
#ifdef TCC_TARGET_PE
    t = VT_SHORT | VT_UNSIGNED;
#else
    t = VT_INT;
#endif
    goto str_init;
  case TOK_STR:
  case_TOK_STR:
    /* string parsing */
    t = char_type.t;
  str_init:
    if (tcc_state->warn_write_strings & WARN_ON)
      t |= VT_CONSTANT;
    type.t = t;
    mk_pointer(&type);
    type.t |= VT_ARRAY;
    memset(&ad, 0, sizeof(AttributeDef));
    ad.section = rodata_section;
    /* Word-align string literals so word-granular consumers (the ssa:const_string
     * fold's strcpy->BLOCK_COPY, which reads the literal with LDM) never hit an
     * unaligned base.  aligned is log2+1, so 3 == 4-byte alignment. */
    ad.a.aligned = 3;
    {
      /* Force DATA_ONLY_WANTED so the IR backend (which defers code generation)
       * can still allocate the string in rodata now, before the actual code
       * referring to it is emitted.
       *
       * However, do NOT set DATA_ONLY_WANTED when CODE_OFF_BIT is active
       * (dead code after unconditional jump / if(0)).  DATA_ONLY_WANTED
       * (0x80000000) combined with CODE_OFF_BIT (0x20000000) gives 0xA0000000
       * which is negative, defeating NODATA_WANTED (nocode_wanted > 0) and
       * causing string data to leak into rodata for dead branches.  With
       * CODE_OFF_BIT alone, NODATA_WANTED is already true so
       * decl_initializer_alloc correctly allocates size=0.  The dead IR
       * instructions that reference these symbols are removed by DCE. */
      int saved_nocode = nocode_wanted;
      addr_t str_pre_off = rodata_section->data_offset;
      if (!(nocode_wanted & CODE_OFF_BIT))
        nocode_wanted |= DATA_ONLY_WANTED;
      decl_initializer_alloc(&type, &ad, VT_CONST, 2, 0, 0);
      nocode_wanted = saved_nocode;
      str_lit_pool_merge(str_pre_off);
    }
    break;
  case TOK_SOTYPE:
  case '(':
    if (unary_paren())
      return 1;
    break;
  case '*':
    next();
    unary();
    indir();
    break;
  case '&':
    next();
    unary();
    /* functions names must be treated as function pointers,
       except for unary '&' and sizeof. Since we consider that
       functions are not lvalues, we only have to handle it
       there and in function calls. */
    /* arrays can also be used although they are not lvalues */
    if ((vtop->type.t & VT_BTYPE) != VT_FUNC && !(vtop->type.t & (VT_ARRAY | VT_VLA)))
    {
      /* If a const global was folded to an immediate (r=VT_CONST, no VT_LVAL),
       * but the symbol is still available, restore the original lvalue form so
       * that '&var' correctly takes the address of the global. This handles
       * cases like 'if (tcc_state->optimize > 0) return &const_global;' where the read is folded
       * but the address-of must still be valid. (Only VT_SYM is not in r
       * because we preserved sym without setting the VT_SYM flag in r.) */
      if (!(vtop->r & VT_LVAL) && (vtop->r & VT_VALMASK) == VT_CONST && vtop->sym != NULL)
      {
        vtop->r = VT_LVAL | VT_CONST | VT_SYM;
        vtop->c.i = 0;
        vtop->type = vtop->sym->type;
        vtop->vr = -1;
      }
      test_lvalue();
    }
    if (vtop->sym && ((vtop->r & VT_SYM) || (vtop->r & VT_LOCAL) || (vtop->r & VT_PARAM)))
    {
      vtop->sym->a.addrtaken = 1;
      /* Mark vreg as address-taken in IR so it gets spilled to stack */
      tcc_ir_set_addrtaken(tcc_state->ir, vtop->sym->vreg);

      /* Check if this is a nested function - need trampoline for address-of.
       * Note: setup_nested_func_trampoline replaces vtop->sym with the
       * trampoline symbol, so after this call vtop->sym no longer points
       * to the nested function symbol. */
      if (vtop->sym->a.nested_func)
        setup_nested_func_trampoline(vtop->sym);
    }
    {
      /* Check for VLA struct local BEFORE mk_pointer changes the type.
       * VLA struct locals store a pointer to the actual data in their
       * stack slot.  &a must return that data pointer (by loading it),
       * not the address of the pointer slot itself. */
      int is_vla_struct_local = struct_has_vla_member(&vtop->type) && (vtop->r & VT_VALMASK) == VT_LOCAL;
      mk_pointer(&vtop->type);
      if (is_vla_struct_local)
      {
        /* Leave VT_LVAL set so the pointer value stored in the
         * stack slot is loaded when the result is materialized. */
      }
      else
      {
        gaddrof();
      }
    }
    break;
  case '!':
    next();
    unary();
    gen_test_zero(TOK_EQ);
    break;
  case '~':
    next();
    unary();
    if (vtop->type.t & VT_COMPLEX)
    {
      /* GCC extension: ~ on complex types means complex conjugate */
      gen_complex_conjugate();
    }
    else
    {
      vpushi(-1);
      gen_op('^');
    }
    break;
  case '+':
    next();
    unary();
    if ((vtop->type.t & VT_BTYPE) == VT_PTR)
      tcc_error("pointer not accepted for unary plus");
    /* In order to force cast, we add zero, except for floating point
       where we really need an noop (otherwise -0.0 will be transformed
       into +0.0).  */
    if (!is_float(vtop->type.t))
    {
      vpushi(0);
      gen_op('+');
    }
    break;
  case TOK_REAL:
  case TOK_REAL_GCC:
  case TOK_IMAG:
  case TOK_IMAG_GCC:
    /* Phase 4 - __real__ and __imag__ operators */
    t = tok;
    next();
    unary();
    if (!(vtop->type.t & VT_COMPLEX))
    {
      if (t == TOK_REAL || t == TOK_REAL_GCC)
      {
        /* __real__ on non-complex is a no-op */
      }
      else
      {
        /* __imag__ on non-complex returns 0 */
        vpop();
        vpushi(0);
      }
    }
    else
    {
      /* Extract real or imaginary part from complex value.
       * Complex types are stored as { real, imag } — two consecutive
       * elements of the base type in memory. */
      int is_real = (t == TOK_REAL || t == TOK_REAL_GCC);
      int base_type = vtop->type.t & VT_BTYPE;
      int result_type;
      int elem_size;
      int is_int_complex = !is_float(base_type);

      /* Determine the result type (scalar component type) */
      if (is_int_complex)
      {
        /* Integer complex: _Complex char → char, _Complex int → int, etc. */
        result_type = base_type;
        elem_size = btype_size(base_type);
      }
      else if (base_type == VT_DOUBLE || base_type == VT_LDOUBLE)
      {
        result_type = base_type;
        elem_size = 8;
      }
      else
      {
        result_type = VT_FLOAT;
        elem_size = 4;
      }

      /* Handle constant complex integers: extract component from packed value */
      if (is_int_complex && (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
      {
        int shift = elem_size * 8;
        uint64_t mask = (shift >= 64) ? ~0ULL : (1ULL << shift) - 1;
        if (is_real)
          vtop->c.i = vtop->c.i & mask;
        else
          vtop->c.i = (shift >= 64) ? 0 : ((vtop->c.i >> shift) & mask);
        vtop->type.t = (vtop->type.t & ~VT_BTYPE & ~VT_COMPLEX) | result_type;
      }
      /* The complex value is on the stack, we need to access its components */
      else if ((vtop->r & VT_VALMASK) == VT_LOCAL)
      {
        /* Stack variable: adjust offset to access real or imag part */
        if (!is_real)
          vtop->c.i += elem_size;
        /* Change type to the base scalar type */
        vtop->type.t = (vtop->type.t & ~VT_BTYPE & ~VT_COMPLEX) | result_type;
      }
      else if (vtop->r & VT_LVAL)
      {
        /* L-value (global or indirect): adjust offset to access real or imag part.
         * Complex types are { real, imag } in memory. For imag, add elem_size
         * to the address offset directly (not via gen_op which would do float math). */
        if (!is_real)
          vtop->c.i += elem_size;

        /* Change type to the base scalar type */
        vtop->type.t = (vtop->type.t & ~VT_BTYPE & ~VT_COMPLEX) | result_type;
      }
      else
      {
        /* Register value: the complex value is packed in a single register
         * (for small types like _Complex char or _Complex short that fit
         * in 4 bytes) or in a register pair.  On ARM32 with gfunc_sret()
         * returning ret_nregs=1 for sizes <= 4, the value is packed:
         *   real part in the low bits, imag part in the upper bits.
         * Extract __imag__ by shifting right by elem_size*8. */
        if (is_real)
        {
          /* Real part is in the low bits — just change type to scalar */
          vtop->type.t = (vtop->type.t & ~VT_BTYPE & ~VT_COMPLEX) | result_type;
        }
        else
        {
          /* Imaginary part: shift right by elem_size*8 bits to
           * bring imag to the low bits, then truncate to base type. */
          vtop->type.t = (vtop->type.t & ~VT_BTYPE & ~VT_COMPLEX) | VT_INT;
          vpushi(elem_size * 8);
          gen_op(TOK_SHR);
          vtop->type.t = (vtop->type.t & ~VT_BTYPE) | result_type;
        }
      }
    }
    break;
  case TOK_SIZEOF:
  case TOK_ALIGNOF1:
  case TOK_ALIGNOF2:
  case TOK_ALIGNOF3:
    t = tok;
    next();
    if (tok == '(')
      tok = TOK_SOTYPE;
    expr_type(&type, unary);
    if (t == TOK_SIZEOF)
    {
      vpush_type_size(&type, &align);
      gen_cast_s(VT_SIZE_T);
    }
    else
    {
      type_size(&type, &align);
      s = NULL;
      if (vtop[1].r & VT_SYM)
        s = vtop[1].sym; /* hack: accessing previous vtop */
      if (s && s->a.aligned)
        align = 1 << (s->a.aligned - 1);
      vpushs(align);
    }
    break;

  case TOK_builtin_expect:
    /* __builtin_expect is a no-op for now */
    parse_builtin_params(0, "ee");
    vpop();
    break;
  case TOK_builtin_abs:
  {
    /* __builtin_abs(int x) - compute absolute value using branchless formula:
     * sign = x >> 31; result = (x ^ sign) - sign
     */
    parse_builtin_params(0, "e");
    /* vtop now holds the argument x */
    /* If x is a condition code (VT_CMP), materialize it into a register
     * first. The abs formula uses x twice (via vdup), and intervening
     * operations (like SAR) would clobber the CPU flags before the
     * second use. */
    if ((vtop->r & VT_VALMASK) == VT_CMP)
      gv(RC_INT);
    /* Generate: sign = x >> 31 */
    vdup();          /* Stack: x x */
    vpushi(31);      /* Stack: x x 31 */
    gen_op(TOK_SAR); /* Stack: x sign (sign = x >> 31) */
    /* Generate: result = (x ^ sign) - sign */
    vdup();      /* Stack: x sign sign */
    vrott(3);    /* Stack: sign x sign */
    gen_op('^'); /* Stack: sign (x ^ sign) */
    vswap();     /* Stack: (x ^ sign) sign */
    gen_op('-'); /* Stack: result */
    break;
  }
  case TOK_builtin_labs:
  case TOK_builtin_llabs:
  case TOK_builtin_imaxabs:
  case TOK_builtin_uabs:
  case TOK_builtin_ulabs:
  case TOK_builtin_ullabs:
  case TOK_builtin_umaxabs:
  {
    int builtin_tok = tok;

    /* Inline signed and unsigned abs-family builtins using the same
       branchless formula as __builtin_abs, with a type-dependent shift. */
    parse_builtin_params(0, "e");
    if ((vtop->r & VT_VALMASK) == VT_CMP)
      gv(RC_INT);
    int shift = (vtop->type.t & VT_BTYPE) == VT_LLONG ? 63 : 31;
    int is_unsigned = (builtin_tok == TOK_builtin_uabs || builtin_tok == TOK_builtin_ulabs ||
                       builtin_tok == TOK_builtin_ullabs || builtin_tok == TOK_builtin_umaxabs);
    gen_inline_abs_from_vtop(shift, is_unsigned);
    break;
  }
  case TOK_builtin_types_compatible_p:
    parse_builtin_params(0, "tt");
    vtop[-1].type.t &= ~(VT_CONSTANT | VT_VOLATILE);
    vtop[0].type.t &= ~(VT_CONSTANT | VT_VOLATILE);
    n = is_compatible_types(&vtop[-1].type, &vtop[0].type);
    vtop -= 2;
    print_vstack("unary, builtin_types_compatible_p");
    vpushi(n);
    break;
  case TOK_builtin_choose_expr:
  {
    int64_t c;
    next();
    skip('(');
    c = expr_const64();
    skip(',');
    if (!c)
    {
      nocode_wanted++;
    }
    expr_eq();
    if (!c)
    {
      vpop();
      nocode_wanted--;
    }
    skip(',');
    if (c)
    {
      nocode_wanted++;
    }
    expr_eq();
    if (c)
    {
      vpop();
      nocode_wanted--;
    }
    skip(')');
  }
  break;
  case TOK_builtin_constant_p:
    parse_builtin_params(1, "e");
    n = 1;
    if ((vtop->r & (VT_VALMASK | VT_LVAL)) != VT_CONST || ((vtop->r & VT_SYM) && vtop->sym->a.addrtaken))
      n = 0;
    /* Recognize compile-time-constant lvalue accesses to read-only data.
     * For example, string literal subscript "hi"[0] is a compile-time
     * constant even though it presents as an lvalue (VT_LVAL set). */
    if (n == 0 && (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == (VT_CONST | VT_LVAL | VT_SYM) && vtop->sym)
    {
      ElfSym *esym = elfsym(vtop->sym);
      if (esym && esym->st_shndx > 0 && esym->st_shndx < tcc_state->nb_sections)
      {
        Section *sec = tcc_state->sections[esym->st_shndx];
        if (sec && !(sec->sh_flags & SHF_WRITE))
        {
          /* Constant-indexed access to read-only section data */
          long offset = esym->st_value + vtop->c.i;
          int sz, al;
          sz = type_size(&vtop->type, &al);
          if (sz > 0 && offset >= 0 && (unsigned long)(offset + sz) <= sec->data_offset && sec->data)
            n = 1;
        }
      }
    }
    /* When optimizing in IR mode, check if a local variable's vreg has
     * exactly one definition and that definition is a constant.  This
     * lets __builtin_constant_p see through simple cases like:
     *   int size = sizeof(int);  // single constant assignment
     *   __builtin_constant_p(size) -> 1
     * Only valid when the variable's address is never taken (no aliasing). */
    if (n == 0 && tcc_state->ir && tcc_state->optimize && vtop->vr >= 0 && (!vtop->sym || !vtop->sym->a.addrtaken))
    {
      TCCIRState *ir = tcc_state->ir;
      int target_vr = vtop->vr;
      int def_count = 0;
      int is_const_def = 0;
      for (int i = 0; i < ir->next_instruction_index; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (!irop_config[q->op].has_dest)
          continue;
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        if (irop_get_vreg(dest) != target_vr)
          continue;
        def_count++;
        if (def_count > 1)
          break; /* multiple definitions — not provably constant */
        if (q->op == TCCIR_OP_ASSIGN)
        {
          IROperand src1 = tcc_ir_op_get_src1(ir, q);
          if (src1.tag == IROP_TAG_IMM32 || src1.tag == IROP_TAG_I64 || src1.tag == IROP_TAG_F32 ||
              src1.tag == IROP_TAG_F64)
            is_const_def = 1;
        }
      }
      if (def_count == 1 && is_const_def)
        n = 1;
    }
    vtop--;
    print_vstack("unary, builtin_constant_p");
    vpushi(n);
    break;
  case TOK_builtin_unreachable:
    parse_builtin_params(0, ""); /* just skip '()' */
    type.t = VT_VOID;
    vpush(&type);
    CODE_OFF();
    break;
  case TOK_builtin_trap:
    parse_builtin_params(0, ""); /* just skip '()' */
    /* Generate a trap instruction through the IR */
    tcc_ir_put(tcc_state->ir, TCCIR_OP_TRAP, NULL, NULL, NULL);
    type.t = VT_VOID;
    vpush(&type);
    break;
  case TOK_builtin_clear_padding:
  {
    /* __builtin_clear_padding(ptr) — zero the padding bytes of *ptr while
     * preserving value bytes.  We walk the target type to compute the
     * padding-byte ranges, then emit byte stores of 0 for each padding byte.
     * For zero-size or padding-free targets we emit nothing — matching GCC's
     * behavior of folding the call away when there's no padding to clear. */
    parse_builtin_params(0, "e");
    if ((vtop->type.t & VT_BTYPE) != VT_PTR)
      tcc_error("__builtin_clear_padding requires a pointer argument");

    CType target = *pointed_type(&vtop->type);
    int t_align, t_size;
    t_size = type_size(&target, &t_align);

    /* Zero/negative (e.g. unknown VLA): nothing to clear. */
    if (t_size <= 0)
    {
      vpop();
      type.t = VT_VOID;
      vpush(&type);
      break;
    }

    /* Sanity cap: refuse to scan absurdly large types. */
    const int MAX_CLEAR_PADDING_SIZE = 4096;
    if (t_size > MAX_CLEAR_PADDING_SIZE)
    {
      tcc_warning("__builtin_clear_padding: object too large (%d bytes), "
                  "treated as no-op",
                  t_size);
      vpop();
      type.t = VT_VOID;
      vpush(&type);
      break;
    }

    unsigned char *vmap = tcc_mallocz(t_size);
    int rc = mark_value_bytes(&target, 0, vmap, t_size);
    if (rc < 0)
    {
      /* Type contains a VLA member or other unsupported shape — emit no
       * stores rather than risk clobbering live bytes. */
      tcc_free(vmap);
      vpop();
      type.t = VT_VOID;
      vpush(&type);
      break;
    }

    int padding_count = 0;
    for (int i = 0; i < t_size; i++)
      if (!vmap[i])
        padding_count++;

    if (padding_count == 0)
    {
      tcc_free(vmap);
      vpop();
      type.t = VT_VOID;
      vpush(&type);
      break;
    }

    /* Save the pointer SValue so we can reuse it for each store, then
     * remove it from vtop. */
    SValue ptr_sv = *vtop;
    vpop();

    /* For each contiguous padding range, emit zero-stores using the widest
     * naturally-aligned store at each offset (1-, 2-, or 4-byte).  This
     * keeps the store count low for typical trailing-padding ranges. */
    int off = 0;
    while (off < t_size)
    {
      if (vmap[off])
      {
        off++;
        continue;
      }
      int run_start = off;
      while (off < t_size && !vmap[off])
        off++;
      int run_end = off;

      int p = run_start;
      while (p < run_end)
      {
        int remaining = run_end - p;
        int sz;
        if ((p & 3) == 0 && remaining >= 4)
          sz = 4;
        else if ((p & 1) == 0 && remaining >= 2)
          sz = 2;
        else
          sz = 1;

        /* Build (T*)((char*)ptr_sv + p), then *result = 0. */
        vpushv(&ptr_sv);
        vtop->type = char_pointer_type;
        vpushi(p);
        gen_op('+');

        CType store_type, store_ptr_type;
        store_type.ref = NULL;
        switch (sz)
        {
        case 1: store_type.t = VT_BYTE | VT_UNSIGNED; break;
        case 2: store_type.t = VT_SHORT | VT_UNSIGNED; break;
        default: store_type.t = VT_INT; break;
        }
        store_ptr_type = store_type;
        mk_pointer(&store_ptr_type);
        gen_cast(&store_ptr_type);
        indir();
        vpushi(0);
        vstore();
        vpop();

        p += sz;
      }
    }

    tcc_free(vmap);
    type.t = VT_VOID;
    vpush(&type);
    break;
  }
  case TOK_builtin_setjmp:
  {
    /* __builtin_setjmp(void **buf) - returns 0 on initial call, 1 on longjmp return.
     *
     * GCC's ABI gives this builtin a 5-WORD buffer and callers really do
     * pass `void *buf[5]` (gcc.c-torture pr84521), so the 40-byte
     * NL_SETJMP layout previously used here overflowed the caller's
     * buffer and smashed its stack.  The callee-saved register file
     * (r4-r11) still must be restored on longjmp — the register
     * allocator keeps VARs and the R9 GOT base in r4-r11 across the
     * setjmp — so SETJMP saves those 8 words into a hidden 32-byte area
     * in this function's frame (alive for as long as a longjmp to this
     * buffer is legal) and records the area address in buf[3]. */
    parse_builtin_params(0, "e");
    loc = (loc - 32) & -8;
    SValue area;
    memset(&area, 0, sizeof(area));
    area.type.t = VT_PTR;
    area.r = VT_LOCAL; /* no VT_LVAL: address-of-local (frame-slot operand) */
    area.c.i = loc;
    area.vr = -1;
    SValue dest;
    dest.type.t = VT_INT;
    dest.type.ref = NULL;
    dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
    dest.r = 0;
    dest.c.i = 0;
    tcc_ir_put(tcc_state->ir, TCCIR_OP_SETJMP, vtop, &area, &dest);
    vtop->vr = dest.vr;
    vtop->r = 0;
    vtop->type.t = VT_INT;
    vtop->type.ref = NULL;
    vtop->c.i = 0;
    break;
  }
  case TOK_builtin_longjmp:
  {
    /* __builtin_longjmp(void **buf, int val) - does not return */
    parse_builtin_params(0, "ee");
    /* Stack: buf, val (val is on top).  val is ignored (__builtin_longjmp
     * always forces the return value to 1). */
    vpop(); /* pop val */
    /* vtop now has buf - emit LONGJMP (see TOK_builtin_setjmp above) */
    tcc_ir_put(tcc_state->ir, TCCIR_OP_LONGJMP, vtop, NULL, NULL);
    vpop(); /* pop buf */
    /* longjmp does not return - mark as void and noreturn */
    type.t = VT_VOID;
    vpush(&type);
    CODE_OFF();
    break;
  }
/* See unary_builtin_alloca: TOK_alloca is an enum, #ifdef never matched. */
#if defined TCC_TARGET_I386 || defined TCC_TARGET_X86_64 || defined TCC_TARGET_ARM
  case TOK_alloca:
#endif
  case TOK_builtin_alloca:
  case TOK_builtin_apply_args:
  case TOK_builtin_apply:
  case TOK_builtin_return:
    unary_builtin_alloca();
    break;
  case TOK_builtin_classify_type:
    parse_builtin_params(1, "e"); /* nc=1: nocode, "e": one expression */
    n = gcc_classify_type(&vtop->type);
    vtop--;
    vpushi(n);
    break;
  case TOK_builtin_signbit:
  case TOK_builtin_signbitf:
  case TOK_builtin_isinf:
  case TOK_builtin_isinff:
  case TOK_builtin_isinfl:
  case TOK_builtin_copysign:
  case TOK_builtin_copysignf:
  case TOK_builtin_isnan:
  case TOK_builtin_isnanf:
  case TOK_builtin_isnanl:
  case TOK_builtin_inf:
  case TOK_builtin_inff:
  case TOK_builtin_infl:
  case TOK_builtin_nan:
  case TOK_builtin_nanf:
  case TOK_builtin_nanl:
  case TOK_builtin_huge_val:
  case TOK_builtin_huge_valf:
  case TOK_builtin_huge_vall:
  case TOK_builtin_isunordered:
  case TOK_builtin_isless:
  case TOK_builtin_isgreater:
  case TOK_builtin_islessequal:
  case TOK_builtin_isgreaterequal:
  case TOK_builtin_islessgreater:
    unary_builtin_fp();
    break;
  case TOK_builtin_fabs:
  case TOK_builtin_fabsf:
  case TOK_builtin_fabsl:
  case TOK_builtin_copysignl:
  case TOK_builtin_isfinite:
  case TOK_builtin_isfinitef:
  case TOK_builtin_isinf_sign:
  case TOK_builtin_fmax:
  case TOK_builtin_fmaxf:
  case TOK_builtin_fmaxl:
  case TOK_builtin_fmin:
  case TOK_builtin_fminf:
  case TOK_builtin_fminl:
  case TOK_builtin_isnormal:
  case TOK_builtin_fpclassify:
  case TOK_builtin_bswap16:
  case TOK_builtin_bswap32:
  case TOK_builtin_bswap64:
    unary_builtin_fp2();
    break;
  case TOK_builtin_modff:
  case TOK_builtin_modf:
  case TOK_builtin_modfl:
    unary_builtin_modf();
    break;
  case TOK_builtin_add_overflow:
  case TOK_builtin_sub_overflow:
  case TOK_builtin_mul_overflow:
  case TOK_builtin_sadd_overflow:
  case TOK_builtin_uadd_overflow:
  case TOK_builtin_ssub_overflow:
  case TOK_builtin_usub_overflow:
  case TOK_builtin_umul_overflow:
  case TOK_builtin_add_overflow_p:
  case TOK_builtin_sub_overflow_p:
  case TOK_builtin_mul_overflow_p:
    unary_builtin_overflow();
    break;
  case TOK_builtin_shuffle:
  case TOK_builtin_shufflevector:
    unary_builtin_shuffle();
    break;
  case TOK_builtin_convertvector:
    unary_builtin_convertvector();
    break;
  case TOK_builtin_conjf:
  case TOK_builtin_conj:
  case TOK_builtin_conjl:
  {
    int tok1 = tok;
    parse_builtin_params(0, "e");

    /* Verify the argument is a complex type */
    if (!(vtop->type.t & VT_COMPLEX))
    {
      tcc_error("__builtin_conj%s expects a complex argument", (tok1 == TOK_builtin_conjf)   ? "f"
                                                               : (tok1 == TOK_builtin_conjl) ? "l"
                                                                                             : "");
    }

    gen_complex_conjugate();
    break;
  }
  case TOK_builtin_crealf:
  case TOK_builtin_creal:
  case TOK_builtin_creall:
  case TOK_builtin_cimagf:
  case TOK_builtin_cimag:
  case TOK_builtin_cimagl:
  {
    int tok1 = tok;
    int is_real = (tok1 == TOK_builtin_crealf || tok1 == TOK_builtin_creal || tok1 == TOK_builtin_creall);
    parse_builtin_params(0, "e");

    if (!(vtop->type.t & VT_COMPLEX))
    {
      if (is_real)
      {
        /* creal on non-complex is identity */
      }
      else
      {
        /* cimag on non-complex returns 0 */
        vpop();
        vpushi(0);
      }
    }
    else
    {
      /* Reuse the __real__ / __imag__ logic via the unary operator handler.
       * We push a synthetic TOK_REAL or TOK_IMAG operation on the vtop value. */
      int base_type = vtop->type.t & VT_BTYPE;
      int is_int_complex = !is_float(base_type);
      int elem_size, result_type;

      if (is_int_complex)
      {
        result_type = base_type;
        elem_size = btype_size(base_type);
      }
      else if (base_type == VT_DOUBLE || base_type == VT_LDOUBLE)
      {
        result_type = base_type;
        elem_size = 8;
      }
      else
      {
        result_type = VT_FLOAT;
        elem_size = 4;
      }

      /* Handle constant complex integers */
      if (is_int_complex && (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
      {
        int shift = elem_size * 8;
        uint64_t mask = (shift >= 64) ? ~0ULL : (1ULL << shift) - 1;
        if (is_real)
          vtop->c.i = vtop->c.i & mask;
        else
          vtop->c.i = (shift >= 64) ? 0 : ((vtop->c.i >> shift) & mask);
        vtop->type.t = (vtop->type.t & ~VT_BTYPE & ~VT_COMPLEX) | result_type;
      }
      else if ((vtop->r & VT_VALMASK) == VT_LOCAL)
      {
        if (!is_real)
          vtop->c.i += elem_size;
        vtop->type.t = (vtop->type.t & ~VT_BTYPE & ~VT_COMPLEX) | result_type;
      }
      else if (vtop->r & VT_LVAL)
      {
        if (!is_real)
          vtop->c.i += elem_size;
        vtop->type.t = (vtop->type.t & ~VT_BTYPE & ~VT_COMPLEX) | result_type;
      }
      else
      {
        /* Handle constant complex floats */
        int is_const = (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
        if (is_const && is_float(base_type))
        {
          CValue cv;
          memset(&cv, 0, sizeof(cv));
          if (base_type == VT_FLOAT)
          {
            union
            {
              float f;
              uint32_t u;
            } r, im;
            r.u = (uint32_t)(vtop->c.i & 0xFFFFFFFF);
            im.u = (uint32_t)(vtop->c.i >> 32);
            if (is_real)
              cv.f = r.f;
            else
              cv.f = im.f;
            vpop();
            CType ft;
            ft.t = VT_FLOAT;
            ft.ref = NULL;
            vsetc(&ft, VT_CONST, &cv);
          }
          else
          {
            double src_real, src_imag;
            memcpy(&src_real, &vtop->c, 8);
            memcpy(&src_imag, (char *)&vtop->c + 8, 8);
            if (is_real)
              cv.d = src_real;
            else
              cv.d = src_imag;
            vpop();
            CType dt;
            dt.t = base_type;
            dt.ref = NULL;
            vsetc(&dt, VT_CONST, &cv);
          }
        }
        else
        {
          /* Register value: small integer complex packed in register */
          if (is_real)
          {
            vtop->type.t = (vtop->type.t & ~VT_BTYPE & ~VT_COMPLEX) | result_type;
          }
          else
          {
            vtop->type.t = (vtop->type.t & ~VT_BTYPE & ~VT_COMPLEX) | VT_INT;
            vpushi(elem_size * 8);
            gen_op(TOK_SHR);
            vtop->type.t = (vtop->type.t & ~VT_BTYPE) | result_type;
          }
        }
      }
    }
    break;
  }
  case TOK_builtin_prefetch:
  {
    /* __builtin_prefetch(address, rw, locality)
     *   address: pointer to memory to prefetch
     *   rw: 0 for read (default), 1 for write
     *   locality: 0-3, with 3 being highest locality (default)
     *
     * On ARM, we emit PLD (Preload Data) for read hints and PLDW (Preload Data with
     * intent to Write) for write hints. The locality hint is currently ignored
     * as ARM PLD/PLDW don't have locality levels like x86.
     */
    next();
    skip('(');
    expr_eq(); /* address - required */

    int rw = 0;       /* default: read */
    int locality = 3; /* default: high locality */

    if (tok == ',')
    {
      next();
      expr_eq(); /* rw - optional */
      rw = vtop->c.i != 0;
      vpop();
    }
    if (tok == ',')
    {
      next();
      expr_eq(); /* locality - optional */
      locality = (int)vtop->c.i;
      if (locality < 0)
        locality = 0;
      if (locality > 3)
        locality = 3;
      vpop();
    }
    skip(')');

    /* Ensure address is a pointer type */
    convert_parameter_type(&vtop->type);

    if (tcc_state->ir)
    {
      /* Emit PREFETCH IR instruction - backend will generate PLD/PLDW */
      /* Store rw hint in src2.c.i (0=read, 1=write) */
      SValue rw_hint;
      svalue_init(&rw_hint);
      rw_hint.type.t = VT_INT;
      rw_hint.r = VT_CONST;
      rw_hint.c.i = rw;
      rw_hint.vr = -1;

      tcc_ir_put(tcc_state->ir, TCCIR_OP_PREFETCH, vtop, &rw_hint, NULL);
    }

    /* Pop the address and push void (prefetch returns nothing) */
    vpop();
    type.t = VT_VOID;
    vpush(&type);
    break;
  }
  case TOK_builtin_frame_address:
  case TOK_builtin_return_address:
  {
    int tok1 = tok;
    int level;
    next();
    skip('(');
    level = expr_const();
    if (level < 0)
      tcc_error("%s only takes positive integers", get_tok_str(tok1, 0));
    skip(')');
    type.t = VT_VOID;
    mk_pointer(&type);
#ifdef TCC_TARGET_ARM
    if (level > 0)
    {
      /* ARM Thumb: frame chain walking for level>0 is not supported.
       * Return NULL, which is a valid implementation
       * (GCC torture tests accept NULL for unsupported levels). */
      vpushi(0);
      vtop->type = type;
    }
    else
    {
      /* level == 0: force standard frame record {FP, LR} */
      tcc_state->force_frame_pointer = 1;
      if (tok1 == TOK_builtin_return_address)
        tcc_state->force_lr_save = 1;
      vset(&type, VT_LOCAL, 0); /* FP value */
      if (tok1 == TOK_builtin_return_address)
      {
        /* LR is at [FP + PTR_SIZE] in the standard frame record */
        vpushi(PTR_SIZE);
        gen_op('+');
        mk_pointer(&vtop->type);
        indir();
      }
    }
#else
    /* Non-ARM targets: original chain-walking implementation */
    tcc_state->force_frame_pointer = 1;
    vset(&type, VT_LOCAL, 0); /* local frame */
    while (level--)
    {
#ifdef TCC_TARGET_RISCV64
      vpushi(2 * PTR_SIZE);
      gen_op('-');
#endif
      mk_pointer(&vtop->type);
      indir(); /* -> parent frame */
    }
    if (tok1 == TOK_builtin_return_address)
    {
#ifdef TCC_TARGET_RISCV64
      vpushi(PTR_SIZE);
      gen_op('-');
#else
      vpushi(PTR_SIZE);
      gen_op('+');
#endif
      mk_pointer(&vtop->type);
      indir();
    }
#endif
  }
  break;
#ifdef TCC_TARGET_RISCV64
  case TOK_builtin_va_start:
    parse_builtin_params(0, "ee");
    r = vtop->r & VT_VALMASK;
    if (r == VT_LLOCAL)
      r = VT_LOCAL;
    if (r != VT_LOCAL)
      tcc_error("__builtin_va_start expects a local variable");
    gen_va_start();
    vstore();
    break;
#endif
#ifdef TCC_TARGET_X86_64
#ifdef TCC_TARGET_PE
  case TOK_builtin_va_start:
    parse_builtin_params(0, "ee");
    r = vtop->r & VT_VALMASK;
    if (r == VT_LLOCAL)
      r = VT_LOCAL;
    if (r != VT_LOCAL)
      tcc_error("__builtin_va_start expects a local variable");
    vtop->r = r;
    vtop->type = char_pointer_type;
    vtop->c.i += 8;
    vstore();
    break;
#else
  case TOK_builtin_va_arg_types:
    parse_builtin_params(0, "t");
    vpushi(classify_x86_64_va_arg(&vtop->type));
    vswap();
    vpop();
    break;
#endif
#endif

#ifdef TCC_TARGET_ARM
  case TOK_builtin_va_start:
  {
    /* ARM32 __builtin_va_start intrinsic: `ap = <first anonymous argument>`.
     *
     * The AAPCS prologue pushes r0-r3 immediately below the caller's stack
     * arguments, so the whole argument area is contiguous and the first
     * anonymous argument sits at a frame offset the backend already knows:
     *
     *   ap = FP + offset_to_args - 16 + named_reg_bytes + named_stack_bytes
     *
     * `offset_to_args` is only fixed once the prologue's push set is decided,
     * so the address is emitted as a LEA of a PARAM-relative stack slot (the
     * backend folds offset_to_args in via tcc_machine_addr_of_stack_slot) —
     * two instructions instead of a call to __tcc_va_start plus the three
     * frame-metadata stores that helper needed to rediscover the same value. */
    parse_builtin_params(0, "ee");
    vpop(); /* `last` is only a syntactic marker — nothing to evaluate */

    if (!nocode_wanted)
    {
      TCCIRState *ir = tcc_state->ir;
      SValue src, dest;

      svalue_init(&src);
      src.type = char_pointer_type;
      src.r = VT_LOCAL | VT_PARAM;
      src.vr = -1;
      src.c.i = -16 + (ir ? ir->named_arg_reg_bytes + ir->named_arg_stack_bytes : 0);

      svalue_init(&dest);
      dest.type = char_pointer_type;
      dest.r = 0;
      dest.vr = tcc_ir_get_vreg_temp(ir);

      tcc_ir_put(ir, TCCIR_OP_LEA, &src, NULL, &dest);

      /* Push the computed address as a plain vreg value and store it into ap. */
      vpushi(0);
      vtop->type = char_pointer_type;
      vtop->r = 0;
      vtop->vr = dest.vr;
      vtop->c.i = 0;
      vstore();
    }
    vpop(); /* drop the stored value (or `ap` itself under nocode_wanted) */

    vpushi(0);
    vtop->type.t = VT_VOID;
    break;
  }

  case TOK_builtin_va_arg:
  {
    /* ARM32 __builtin_va_arg intrinsic.
     * va_list is now a simple char pointer (GCC-compatible ABI).
     * For normal types:   *(type *)__tcc_va_arg(&ap, sizeof(type), __alignof__(type))
     * For VLA structs:    *(type *)(*(void **)__tcc_va_arg(&ap, sizeof(void*), __alignof__(void*)))
     *
     * VLA structs are passed by invisible reference (a pointer) by the
     * caller, so va_arg reads a 4-byte pointer and dereferences it. */
    parse_builtin_params(0, "et");
    type = vtop->type;
    vpop(); /* pop type placeholder; vtop = ap */

    {
      int type_align_dummy;
      if ((type.t & VT_BTYPE) == VT_VOID || type_size(&type, &type_align_dummy) < 0)
        tcc_error("second argument to 'va_arg' is of incomplete type 'void'");
    }

    int is_vla_struct = ((type.t & VT_BTYPE) == VT_STRUCT) && struct_has_vla_member(&type);
    int va_size, va_align;

    if (is_vla_struct)
    {
      /* VLA struct: read a pointer (4 bytes) from the va arg area */
      va_size = PTR_SIZE;
      va_align = PTR_SIZE;
    }
    else
    {
      va_size = type_size(&type, &va_align);
      /* Use AAPCS natural alignment for va_arg — only the alignment
       * coming from fundamental member types counts for double-word
       * alignment, not __attribute__((aligned)) on the struct. */
      va_align = compute_aapcs_natural_alignment(&type);
    }

    /* Inline what __tcc_va_arg used to do at runtime:
     *
     *   p  = align_up(ap, max(va_align, 4));
     *   ap = p + ((va_size + 3) & ~3);
     *   result = p
     *
     * `ap` is invariantly 4-byte aligned — va_start lands it on a word
     * boundary and every bump above is a word multiple — so the align step is
     * dead for everything but 8-byte-aligned types, exactly as the helper's
     * `if (align < 4) align = 4` made it.  Emitting this inline replaces a
     * 4-instruction call sequence (address + two constants + bl) per va_arg,
     * and lets the optimizer see the pointer bump instead of an opaque call.
     *
     * vtop is `ap` as an lvalue (its address is never taken here, unlike the
     * old helper form, so `ap` can stay in a register). */
    const int va_step = (va_size + 3) & ~3;

    vdup();     /* [ap_lval, ap_lval] — one copy is the store destination */
    gv(RC_INT); /* [ap_lval, ap_value] */
    /* Reinterpret the pointer as an unsigned word so the mask below is a plain
     * integer op (gen_op would reject `&` on a pointer type). */
    vtop->type.t = VT_INT | VT_UNSIGNED;
    vtop->type.ref = NULL;
    if (va_align > 4)
    {
      vpushi(va_align - 1);
      gen_op('+');
      vpushi(-va_align);
      gen_op('&');
    }

    /* The aligned pointer is va_arg's result; keep it while the incremented
     * value is written back.  gen_op() below defines a fresh vreg for its
     * result, so this snapshot stays valid. */
    SValue va_result = *vtop;

    vpushi(va_step);
    gen_op('+');
    vtop->type = char_pointer_type;
    vstore(); /* ap = p + step; leaves the stored value on the stack */
    vpop();

    vpushv(&va_result);
    vtop->type.t = VT_PTR;
    vtop->type.ref = NULL;

    /* vtop = void* pointing into the va arg area.
     * For VLA struct: the arg area contains a pointer to the actual data.
     * For normal types: the arg area contains the data directly. */
    if (is_vla_struct)
    {
      /* Double indirection: read the data pointer from the va arg area,
       * then dereference it to get the VLA struct data.
       * Equivalent to: *(type *)(*(void **)result) */
      mk_pointer(&vtop->type); /* void* → void** */
      indir();                 /* *(void **) → void* (data ptr), sets VT_LVAL */
      /* Now vtop->type = void* with VT_LVAL: will load the data pointer.
       * Change type to (type *) and dereference to get the struct. */
      vtop->type = type;
      mk_pointer(&vtop->type);
      indir(); /* *(type *) → type with VT_LVAL */
    }
    else
    {
      /* Simple: *(type *)result */
      vtop->type = type;
      mk_pointer(&vtop->type);
      indir();
    }

    vtop->type = type;
    break;
  }
#endif

#ifdef TCC_TARGET_ARM64
  case TOK_builtin_va_start:
  {
    parse_builtin_params(0, "ee");
    // xx check types
    gen_va_start();
    vpushi(0);
    vtop->type.t = VT_VOID;
    break;
  }
  case TOK_builtin_va_arg:
  {
    parse_builtin_params(0, "et");
    type = vtop->type;
    vpop();
    // xx check types
    gen_va_arg(&type);
    vtop->type = type;
    break;
  }
  case TOK___arm64_clear_cache:
  {
    parse_builtin_params(0, "ee");
    gen_clear_cache();
    vpushi(0);
    vtop->type.t = VT_VOID;
    break;
  }
#endif

  /* __builtin_object_size(ptr, type) — compute remaining bytes from ptr to end
   * of its enclosing object.  Returns (size_t)-1 when the size cannot be
   * determined at compile time. */
  case TOK_builtin_object_size:
  case TOK_builtin___memcpy_chk:
  case TOK_builtin___memmove_chk:
  case TOK_builtin___memset_chk:
  case TOK_builtin___mempcpy_chk:
  case TOK_builtin___strcpy_chk:
  case TOK_builtin___stpcpy_chk:
  case TOK_builtin___strcat_chk:
  case TOK_builtin___strncpy_chk:
  case TOK_builtin___stpncpy_chk:
  case TOK_builtin___strncat_chk:
  case TOK_builtin___sprintf_chk:
  case TOK_builtin___snprintf_chk:
  case TOK_builtin___vsprintf_chk:
  case TOK_builtin___vsnprintf_chk:
  case TOK_builtin_abort:
  case TOK_builtin_malloc:
  case TOK_builtin_free:
  case TOK_builtin_calloc:
  case TOK_builtin_realloc:
  case TOK_builtin_ffs:
  case TOK_builtin_ffsl:
  case TOK_builtin_ffsll:
  case TOK_builtin_clzll:
  case TOK_builtin_ctzll:
  case TOK_builtin_popcount:
  case TOK_builtin_popcountl:
  case TOK_builtin_popcountll:
  case TOK_builtin_parity:
  case TOK_builtin_parityl:
  case TOK_builtin_parityll:
    unary_builtin_chk();
    break;

  /* 32-bit clz/ctz: when the core encodes CLZ/RBIT, emit the instructions
   * instead of calling __clzsi2/__ctzsi2.  `long` is 32-bit here, so the `l`
   * variants share the path; the `ll` variants stay with the helper above. */
  case TOK_builtin_clz:
  case TOK_builtin_clzl:
  case TOK_builtin_ctz:
  case TOK_builtin_ctzl:
  {
    if (!tcc_state->ir || !tcc_machine_has_bit_ops())
    {
      unary_builtin_chk();
      break;
    }
    int want_ctz = (tok == TOK_builtin_ctz || tok == TOK_builtin_ctzl);
    parse_builtin_params(0, "e");
    inline_subst_const_arg(vtop);
    CType uint_type;
    uint_type.t = VT_INT | VT_UNSIGNED;
    uint_type.ref = NULL;
    gen_cast(&uint_type);
    if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST && !(vtop->r & VT_SYM))
    {
      /* clz(0) and ctz(0) are undefined; leave those to the instruction. */
      uint32_t cval = (uint32_t)vtop->c.i;
      if (cval != 0)
      {
        int folded = want_ctz ? __builtin_ctz(cval) : __builtin_clz(cval);
        vtop--;
        vpushi(folded);
        break;
      }
    }
    if (want_ctz)
      gen_bitop1(TCCIR_OP_RBIT);
    gen_bitop1(TCCIR_OP_CLZ);
    vtop->type.t = VT_INT; /* __builtin_clz/ctz return int */
    break;
  }

  /* String and memory builtins - redirect to library functions */
  case TOK_builtin_strlen:
  case TOK_builtin_strcpy:
  case TOK_builtin_strncpy:
  case TOK_builtin_strcat:
  case TOK_builtin_strncat:
  case TOK_builtin_strcmp:
  case TOK_builtin_strncmp:
  case TOK_builtin_memcpy:
  case TOK_builtin_memmove:
  case TOK_builtin_memset:
  case TOK_builtin_bzero:
  case TOK_builtin_memcmp:
  case TOK_builtin_memcmp_eq:
  case TOK_builtin_memchr:
  case TOK_builtin_strchr:
  case TOK_builtin_strrchr:
  case TOK_builtin_strstr:
  case TOK_builtin_strpbrk:
  case TOK_builtin_strspn:
  case TOK_builtin_strcspn:
  case TOK_builtin_strnlen:
  case TOK_builtin_mempcpy:
  case TOK_builtin_stpcpy:
  case TOK_builtin_stpncpy:
  case TOK_builtin_fputs:
  case TOK_builtin_fprintf:
  {
    /* Map builtin to corresponding library function name */
    const char *func_name;
    CType *func_type = &func_old_type;
    switch (tok)
    {
    case TOK_builtin_strlen:
      func_name = "strlen";
      func_type = &func_old_size_t_type;
      break;
    case TOK_builtin_strcpy:
      func_name = "strcpy";
      func_type = &func_old_char_pointer_type;
      break;
    case TOK_builtin_strncpy:
      func_name = "strncpy";
      func_type = &func_old_char_pointer_type;
      break;
    case TOK_builtin_strcat:
      func_name = "strcat";
      func_type = &func_old_char_pointer_type;
      break;
    case TOK_builtin_strncat:
      func_name = "strncat";
      func_type = &func_old_char_pointer_type;
      break;
    case TOK_builtin_strcmp:
      func_name = "strcmp";
      break;
    case TOK_builtin_strncmp:
      func_name = "strncmp";
      break;
    case TOK_builtin_memcpy:
      func_name = "memcpy";
      func_type = &func_old_void_pointer_type;
      break;
    case TOK_builtin_memmove:
      func_name = "memmove";
      func_type = &func_old_void_pointer_type;
      break;
    case TOK_builtin_memset:
      func_name = "memset";
      func_type = &func_old_void_pointer_type;
      break;
    case TOK_builtin_bzero:
      func_name = "bzero";
      func_type = &func_old_void_type;
      break;
    case TOK_builtin_memcmp:
      func_name = "memcmp";
      break;
    case TOK_builtin_memcmp_eq:
      func_name = "__builtin_memcmp_eq";
      break;
    case TOK_builtin_memchr:
      func_name = "memchr";
      func_type = &func_old_void_pointer_type;
      break;
    case TOK_builtin_strchr:
      func_name = "strchr";
      func_type = &func_old_char_pointer_type;
      break;
    case TOK_builtin_strrchr:
      func_name = "strrchr";
      func_type = &func_old_char_pointer_type;
      break;
    case TOK_builtin_strstr:
      func_name = "strstr";
      func_type = &func_old_char_pointer_type;
      break;
    case TOK_builtin_strpbrk:
      func_name = "strpbrk";
      func_type = &func_old_char_pointer_type;
      break;
    case TOK_builtin_strspn:
      func_name = "strspn";
      func_type = &func_old_size_t_type;
      break;
    case TOK_builtin_strcspn:
      func_name = "strcspn";
      func_type = &func_old_size_t_type;
      break;
    case TOK_builtin_strnlen:
      func_name = "strnlen";
      func_type = &func_old_size_t_type;
      break;
    case TOK_builtin_mempcpy:
      func_name = "mempcpy";
      func_type = &func_old_void_pointer_type;
      break;
    case TOK_builtin_stpcpy:
      func_name = "stpcpy";
      func_type = &func_old_char_pointer_type;
      break;
    case TOK_builtin_stpncpy:
      func_name = "stpncpy";
      func_type = &func_old_char_pointer_type;
      break;
    case TOK_builtin_fputs:
      func_name = "fputs";
      break;
    case TOK_builtin_fprintf:
      func_name = "fprintf";
      break;
    default:
      func_name = NULL;
      break;
    }
    if (func_name)
    {
      int func_tok = tok_alloc_const(func_name);
      vpush_typed_helper_func(func_tok, func_type);
    }
    /* Consume the builtin token; the caller will handle the following '(' */
    next();
    break;
  }

  case TOK_builtin_ilogb:
  {
    CType dt;
    parse_builtin_params(0, "e");
    dt.t = VT_DOUBLE;
    dt.ref = NULL;
    gen_cast(&dt);
    gen_builtin_libcall(tok_alloc_const("ilogb"), 1, VT_INT);
    break;
  }

  /* atomic operations */
  case TOK___atomic_store:
  case TOK___atomic_load:
  case TOK___atomic_exchange:
  case TOK___atomic_compare_exchange:
  case TOK___atomic_fetch_add:
  case TOK___atomic_fetch_sub:
  case TOK___atomic_fetch_or:
  case TOK___atomic_fetch_xor:
  case TOK___atomic_fetch_and:
  case TOK___atomic_fetch_nand:
  case TOK___atomic_add_fetch:
  case TOK___atomic_sub_fetch:
  case TOK___atomic_or_fetch:
  case TOK___atomic_xor_fetch:
  case TOK___atomic_and_fetch:
  case TOK___atomic_nand_fetch:
    parse_atomic(tok);
    break;

  /* pre operations */
  case TOK_INC:
  case TOK_DEC:
    t = tok;
    next();
    unary();
    inc(0, t);
    break;
  case '-':
    next();
    unary();
    if (is_float(vtop->type.t))
    {
      gen_opif(TOK_NEG);
    }
    else
    {
      vpushi(0);
      vswap();
      gen_op('-');
    }
    break;
  case TOK_LAND:
    if (!gnu_ext)
      goto tok_identifier;
    next();
    /* allow to take the address of a label */
    if (tok < TOK_UIDENT)
      expect("label identifier");
    s = label_find(tok);
    if (!s)
    {
      s = label_push(&global_label_stack, tok, LABEL_FORWARD);
    }
    else
    {
      if (s->r == LABEL_DECLARED)
        s->r = LABEL_FORWARD;
    }
    /* Mark that this label's address is taken (&&label). In IR mode, the
       symbol definition is deferred until after code generation when the
       final code offsets are known.
       Use -3 as special marker (distinct from valid ELF indices >= 0,
       and from -1/-2 used for type descriptors and struct definitions).
       Only set if not already marked/having an ELF symbol. */
    if (s->c <= 0)
      s->c = -3; /* LABEL_ADDR_TAKEN marker */
    func_has_label_addr = 1;
    if (tcc_state->ir)
      tcc_state->ir->func_has_label_addr = 1; /* mirror for the IR layer (regalloc) */
    if ((s->type.t & VT_BTYPE) != VT_PTR)
    {
      s->type.t = VT_VOID;
      mk_pointer(&s->type);
      s->type.t |= VT_STATIC;
    }
    vpushsym(&s->type, s);
    next();
    break;

  case TOK_GENERIC:
    unary_generic();
    break;
  // special qnan , snan and infinity values
  case TOK___NAN__:
    n = 0x7fc00000;
  special_math_val:
    vpushi(n);
    vtop->type.t = VT_FLOAT;
    next();
    break;
  case TOK___SNAN__:
    n = 0x7f800001;
    goto special_math_val;
  case TOK___INF__:
    n = 0x7f800000;
    goto special_math_val;

  default:
  tok_identifier:
    if (tok < TOK_UIDENT)
      tcc_error("expression expected before '%s'", get_tok_str(tok, &tokc));
    t = tok;
    next();
    /* Inline-eval overlay: if we're inside try_inline_const_eval and t is
     * a parameter token, push the caller's SValue directly. This preserves
     * the original sym reference + offset + type, which is essential for
     * VT_SYM pointer args so that `*p` can fold to the underlying global's
     * initializer later in the body. */
    if (tcc_state->inline_eval_overlay_n > 0)
    {
      int oi2;
      for (oi2 = 0; oi2 < tcc_state->inline_eval_overlay_n; oi2++)
      {
        if (tcc_state->inline_eval_overlay_tok[oi2] == t)
        {
          vpushv(&tcc_state->inline_eval_overlay_sv[oi2]);
          break;
        }
      }
      if (oi2 < tcc_state->inline_eval_overlay_n)
        break;
    }
    s = sym_find(t);
    if (!s || IS_ASM_SYM(s))
    {
      /* Check if this identifier is a captured variable from an enclosing function */
      NestedFunc *nf = tcc_state->current_nested_func;
      if (nf && nf->nb_captured > 0)
      {
        /* Search captured_offsets for matching token */
        for (int i = 0; i < nf->nb_captured; i++)
        {
          if (nf->captured_tokens[i] == t)
          {
            /* Found a match - create a fake symbol for this captured variable.
             * The offset is the parent's FP-relative offset (resolved after
             * parent's register allocation). Access goes through R10 (static chain). */
            s = sym_malloc();
            memset(s, 0, sizeof(*s));
            s->v = t;
            s->type = nf->captured_types[i]; /* Use actual captured variable type */
            s->r = VT_LOCAL | VT_LVAL;       /* LOCAL + LVAL so it works as both value and assignment target */
            s->c = nf->captured_offsets[i];  /* Parent's FP offset */
            s->vreg = -1;                    /* No vreg in nested function's IR — pure stack offset via chain */
            s->sym_scope = 0;
            goto found_captured_var;
          }
        }
      }

      const char *name = get_tok_str(t, NULL);
      if (tok != '(')
        tcc_error("'%s' undeclared", name);
      /* for simple function calls, we tolerate undeclared
         external reference to int() function */
      tcc_warning_c(warn_implicit_function_declaration)("implicit declaration of function '%s'", name);
      s = external_global_sym(t, &func_old_type);
    }
  found_captured_var:

    r = s->r;
    /* A symbol that has a register is a local register variable,
       which starts out as VT_LOCAL value.  */
    if ((r & VT_VALMASK) < VT_CONST)
    {
      // parameter is always a local value
      if (!(r & VT_PARAM))
      {
        r = (r & ~VT_VALMASK) | VT_LOCAL;
      }
    }

    vset(&s->type, r, s->c);
    /* Point to s as backpointer (even without r&VT_SYM).
       Will be used by at least the x86 inline asm parser for
       regvars.  */
    vtop->sym = s;
    vtop->vr = s->vreg;

    /* Array-to-pointer decay for captured variables (nested functions).
     * Captured arrays have VT_ARRAY type and VT_LVAL set. They need to
     * decay to pointers for subscript and pointer arithmetic to work. */
    if ((vtop->type.t & VT_ARRAY) && (vtop->r & VT_LVAL))
    {
      gaddrof();
      vtop->type.t &= ~VT_ARRAY;
    }

    if (r & VT_SYM)
    {
      vtop->c.i = 0;

      /* Fold reads from const-qualified scalar globals with known initializers.
       * If the variable is const (not volatile), has a simple scalar type,
       * and the initializer data is available in the section, replace the
       * lvalue reference with the compile-time constant value. This is needed
       * even at -O0 for constant-evaluation contexts such as static
       * initializers. */
      if ((s->type.t & VT_CONSTANT) && !(s->type.t & VT_VOLATILE) && !(s->type.t & VT_ARRAY) && !(s->type.t & VT_VLA) &&
          (s->type.t & VT_BTYPE) != VT_FUNC && (s->type.t & VT_BTYPE) != VT_STRUCT &&
          (s->type.t & VT_BTYPE) != VT_PTR && s->c > 0)
      {
        ElfSym *esym = elfsym(s);
        if (esym && esym->st_shndx != SHN_UNDEF && esym->st_shndx != SHN_COMMON &&
            esym->st_shndx < tcc_state->nb_sections)
        {
          Section *sec = tcc_state->sections[esym->st_shndx];
          int btype = s->type.t & VT_BTYPE;
          int align;
          int sz = type_size(&s->type, &align);
          if (sec && sec->data && sz > 0 && esym->st_value + sz <= sec->data_offset)
          {
            const unsigned char *ptr = sec->data + esym->st_value;
            if (btype == VT_DOUBLE || btype == VT_LDOUBLE)
            {
              double val;
              memcpy(&val, ptr, sizeof(double));
              vtop->c.d = val;
              vtop->r = VT_CONST;
              vtop->type.t = (s->type.t & ~(VT_CONSTANT | VT_VOLATILE)) & (VT_BTYPE | VT_UNSIGNED | VT_LONG);
              /* Preserve sym so &var can restore lvalue form if needed */
              vtop->vr = -1;
            }
            else if (btype == VT_FLOAT)
            {
              float val;
              memcpy(&val, ptr, sizeof(float));
              vtop->c.f = val;
              vtop->r = VT_CONST;
              vtop->type.t = VT_FLOAT;
              /* Preserve sym so &var can restore lvalue form if needed */
              vtop->vr = -1;
            }
            else if (btype == VT_LLONG)
            {
              int64_t val;
              memcpy(&val, ptr, sizeof(int64_t));
              vtop->c.i = val;
              vtop->r = VT_CONST;
              vtop->type.t = (s->type.t & VT_UNSIGNED) ? (VT_LLONG | VT_UNSIGNED) : VT_LLONG;
              /* Preserve sym so &var can restore lvalue form if needed */
              vtop->vr = -1;
            }
            else if (btype == VT_INT || btype == VT_BYTE || btype == VT_SHORT || btype == VT_BOOL)
            {
              int64_t val = 0;
              memcpy(&val, ptr, sz);
              /* Sign-extend for signed types */
              if (!(s->type.t & VT_UNSIGNED) && sz < 8)
              {
                int shift = (8 - sz) * 8;
                val = (int64_t)(val << shift) >> shift;
              }
              vtop->c.i = val;
              vtop->r = VT_CONST;
              vtop->type.t = (s->type.t & ~(VT_CONSTANT | VT_VOLATILE)) & (VT_BTYPE | VT_UNSIGNED | VT_LONG);
              /* Preserve sym so &var can restore lvalue form if needed */
              vtop->vr = -1;
            }
          }
        }
      }

#ifdef TCC_TARGET_PE
      if (s->a.dllimport)
      {
        mk_pointer(&vtop->type);
        vtop->r |= VT_LVAL;
        indir();
      }
#endif
    }
    else if (r == VT_CONST && IS_ENUM_VAL(s->type.t))
    {
      vtop->c.i = s->enum_val;
    }

    /* Implicit function-to-pointer: if a nested function name is used in
     * a non-call context (next token is NOT '('), it needs a trampoline. */
    if (s->a.nested_func && tok != '(')
      setup_nested_func_trampoline(s);

    break;
  }
  return 0;
}
