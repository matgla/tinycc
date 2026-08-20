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

/* cast.c -- Cast lowering, including vector and char/short narrowing casts.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

#if defined TCC_TARGET_ARM64 || defined TCC_TARGET_RISCV64 || defined TCC_TARGET_ARM
#define gen_cvt_itof1 gen_cvt_itof
#else
/* generic itof for unsigned long long case */
static void gen_cvt_itof1(int t)
{
  if ((vtop->type.t & (VT_BTYPE | VT_UNSIGNED)) == (VT_LLONG | VT_UNSIGNED))
  {

    if (t == VT_FLOAT)
      vpush_helper_func(TOK___floatundisf);
#if LDOUBLE_SIZE != 8
    else if (t == VT_LDOUBLE)
      vpush_helper_func(TOK___floatundixf);
#endif
    else
      vpush_helper_func(TOK___floatundidf);
    vrott(2);
    // gfunc_call(1);
    tcc_error("3 implement me");
    vpushi(0);
    PUT_R_RET(vtop, t);
  }
  else
  {
    gen_cvt_itof(t);
  }
}
#endif

/* special delayed cast for char/short */
void force_charshort_cast(void)
{
  /* VT_MUSTCAST uses bits VT_MUSTCAST (0x0100) and VT_MUSTCAST<<1 (0x0200)
   * as a 2-bit field: value 1 = from int, value 2 = from long long.
   * BFGET(vtop->r, VT_MUSTCAST) doesn't work correctly for the 1-bit mask
   * when the value is 2, so extract manually. */
  int mustcast_bits = (vtop->r & (VT_MUSTCAST | (VT_MUSTCAST << 1)));
  int sbt = (mustcast_bits == BFVAL(VT_MUSTCAST, 2)) ? VT_LLONG : VT_INT;
  int dbt = vtop->type.t;
  vtop->r &= ~(VT_MUSTCAST | (VT_MUSTCAST << 1));
  vtop->type.t = sbt;
  gen_cast_s(dbt == VT_BOOL ? VT_BYTE | VT_UNSIGNED : dbt);
  vtop->type.t = dbt;
}

void gen_cast_s(int t)
{
  CType type;
  type.t = t;
  type.ref = NULL;
  gen_cast(&type);
}

/* Reinterpret-cast involving at least one GCC vector type.
 * GCC vector casts are always bitwise reinterpretations; sizes must match.
 * Three sub-cases:
 *   vec  → vec    (e.g. V2USI→V2SI):   pure type relabeling, same lvalue
 *   vec  → scalar (e.g. V2SI→long long): type relabeling, source already in mem
 *   scalar → vec  (e.g. 0LL→V2SI):     store scalar to temp, return vec lvalue
 */
static void gen_cast_vector(CType *dst_type)
{
  int src_is_vec = is_vector_type(&vtop->type);
  int src_align, dst_align;
  int src_size = type_size(&vtop->type, &src_align);
  int dst_size = type_size(dst_type, &dst_align);

  if (src_size != dst_size)
    tcc_error("cannot reinterpret-cast vector/scalar of different sizes (%d vs %d bytes)", src_size, dst_size);

  if (src_is_vec)
  {
    /* vec→vec or vec→scalar: source is already an lvalue in memory.
     * Just relabel the type; the subsequent LOAD (if any) uses the new width. */
    vtop->type = *dst_type;
    return;
  }

  /* scalar→vec: must materialise the scalar value into a stack slot and
   * hand it back as a vector lvalue.  Skip code emission during size-only
   * passes (DIF_SIZE_ONLY) — a pure type relabel is enough there. */
  if (nocode_wanted)
  {
    vtop->type = *dst_type;
    return;
  }

  /* If the scalar source is already an lvalue in memory and the byte size
   * matches, the bytes are already laid out as the vector representation —
   * a relabel suffices and avoids an extra temp + copy.  This is common in
   * cast chains like (V2SI)(long long)v where the round-trip would otherwise
   * spill through a fresh local. */
  if ((vtop->r & VT_LVAL) && src_size == dst_size)
  {
    vtop->type = *dst_type;
    return;
  }

  int vr_tmp;
  int loc = get_temp_local_var(dst_size, dst_size > 8 ? 8 : dst_size, &vr_tmp);

  /* Push a destination SValue typed as the *scalar* source so vstore() emits
   * the correct-width STORE instruction. */
  SValue dst_sv;
  memset(&dst_sv, 0, sizeof(dst_sv));
  dst_sv.type = vtop->type; /* scalar type — correct store width */
  dst_sv.r = VT_LOCAL | VT_LVAL;
  dst_sv.vr = vr_tmp;
  dst_sv.c.i = loc;

  vpushv(&dst_sv); /* stack: ..., scalar, temp_dst  */
  vswap();         /* stack: ..., temp_dst, scalar   */
  vstore();        /* emit STORE scalar→temp; stack: ..., scalar */
  vtop--;          /* drop scalar; stack: ...        */

  /* Return the temp slot as a vector lvalue. */
  dst_sv.type = *dst_type;
  vpushv(&dst_sv);
}

/* cast 'vtop' to 'type'. Casting to bitfields is forbidden. */
void gen_cast(CType *type)
{
  int sbt, dbt, sf, df, c;
  int dbt_bt, sbt_bt, ds, ss, bits, trunc;

  if (is_transparent_union_type(type))
  {
    CType *member_type = find_assignable_transparent_union_member(type);
    if (member_type)
    {
      gen_cast(member_type);
      return;
    }
  }

  /* special delayed cast for char/short */
  /* VT_MUSTCAST uses bits 0x100-0x200 as a 2-bit field, but VT_NONCONST
     also occupies bit 0x200.  VT_MUSTCAST only applies to register values
     (char/short stored in int registers), never to VT_CONST values.
     Skip when the value is a constant to avoid misinterpreting VT_NONCONST
     as part of the VT_MUSTCAST field. */
  if ((vtop->r & (VT_MUSTCAST | (VT_MUSTCAST << 1))) && (vtop->r & VT_VALMASK) != VT_CONST)
    force_charshort_cast();

  /* bitfields first get cast to ints */
  if (vtop->type.t & VT_BITFIELD)
    gv(RC_INT);

  if (IS_ENUM(type->t) && type->ref->c < 0)
    tcc_error("cast to incomplete type");

  /* GCC vector reinterpret cast: handle before the scalar btype machinery.
   * Skip void casts — (void)vec is handled by the normal path (just pops). */
  if ((type->t & VT_BTYPE) != VT_VOID && (is_vector_type(&vtop->type) || is_vector_type(type)))
  {
    gen_cast_vector(type);
    return;
  }

  dbt = type->t & (VT_BTYPE | VT_UNSIGNED);
  sbt = vtop->type.t & (VT_BTYPE | VT_UNSIGNED);
  if (sbt == VT_FUNC)
    sbt = VT_PTR;

  /* Constant complex float/double cast: intercept before sbt==dbt shortcut.
   * When VT_COMPLEX flag changes but base type is the same (e.g. double → _Complex double),
   * we still need to repack the CValue. Force entry into the main cast body. */
  if (sbt == dbt && ((vtop->type.t ^ type->t) & VT_COMPLEX) &&
      (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && is_float(sbt))
  {
    /* Force sbt != dbt so we enter the main cast body below,
     * where the complex constant cast handler will pick this up. */
    goto process_cast;
  }

  /* Non-constant scalar↔complex cast with matching base type
   * (e.g. int → _Complex int, double → _Complex double).
   * The sbt==dbt shortcut below would just update the type flag without
   * generating any code, leaving the imaginary part uninitialized — so the
   * subsequent complex op would read garbage from memory beyond the scalar. */
  if (sbt == dbt && ((vtop->type.t ^ type->t) & VT_COMPLEX))
  {
    int is_const = (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
    if (is_const)
      goto process_cast; /* constant case handled in the main cast body */

    int src_complex = (vtop->type.t & VT_COMPLEX) != 0;
    int dst_complex = (type->t & VT_COMPLEX) != 0;
    int sbt_bt2 = sbt & VT_BTYPE;
    int is_fp = is_float(sbt_bt2);
    /* btype_size handles only integer types; compute float widths here. */
    int elem_sz;
    if (is_fp)
      elem_sz = (sbt_bt2 == VT_FLOAT) ? 4 : 8; /* VT_DOUBLE / VT_LDOUBLE → 8 on ARM */
    else
      elem_sz = btype_size(sbt_bt2);

    if (!src_complex && dst_complex)
    {
      /* scalar → _Complex: allocate temp, store scalar as real, store 0 as imag.
       * vstore() consumes both its dst and value entries from the vstack, so
       * we save the source SValue and pop it first — then push a fresh entry
       * for the new complex temp at the end. This keeps the vstack balanced
       * and avoids overwriting whatever was below the source.
       *
       * We use vr=-1 on the component dsts so vstore() emits STORE (which
       * honors the c.i stack offset) rather than ASSIGN (which treats the
       * entire vreg as one slot and would collapse the real/imag stores). */
      int complex_sz = elem_sz * 2;
      CType scalar_type;
      scalar_type.t = sbt;
      scalar_type.ref = NULL;

      int tmp_vr;
      int tmp_loc = get_temp_local_var(complex_sz, elem_sz, &tmp_vr);
      (void)tmp_vr; /* tmp_vr only used to keep the temp slot reserved */

      SValue saved_src = *vtop;
      vpop();

      /* Store real part = saved source value */
      {
        SValue dst;
        memset(&dst, 0, sizeof(dst));
        dst.type = scalar_type;
        dst.r = VT_LOCAL | VT_LVAL;
        dst.vr = -1;
        dst.c.i = tmp_loc;
        vpushv(&dst);
        vpushv(&saved_src);
        vstore();
        vpop();
      }

      /* Store imaginary part = 0 (float 0.0 or int 0 per base type) */
      {
        SValue dst;
        memset(&dst, 0, sizeof(dst));
        dst.type = scalar_type;
        dst.r = VT_LOCAL | VT_LVAL;
        dst.vr = -1;
        dst.c.i = tmp_loc + elem_sz;
        vpushv(&dst);
        if (is_fp)
        {
          CValue zero_cv;
          memset(&zero_cv, 0, sizeof(zero_cv));
          if (sbt_bt2 == VT_FLOAT)
            zero_cv.f = 0.0f;
          else if (sbt_bt2 == VT_DOUBLE)
            zero_cv.d = 0.0;
          else /* VT_LDOUBLE */
            zero_cv.ld = 0.0;
          vsetc(&scalar_type, VT_CONST, &zero_cv);
        }
        else
        {
          vpushi(0);
          vtop->type = scalar_type;
        }
        vstore();
        vpop();
      }

      /* Push the new complex temp lvalue as vtop */
      SValue complex_sv;
      memset(&complex_sv, 0, sizeof(complex_sv));
      complex_sv.type = *type;
      complex_sv.r = VT_LOCAL | VT_LVAL;
      complex_sv.vr = -1;
      complex_sv.c.i = tmp_loc;
      vpushv(&complex_sv);
      return;
    }
    else if (src_complex && !dst_complex)
    {
      /* _Complex → scalar: extract real part (at offset 0), discard imaginary */
      vtop->type = *type;
      return;
    }
  }

  /* Complex → complex with a different float base (_Complex float ↔
   * _Complex double): convert component-wise through a temp local.  Without
   * this the generic scalar machinery below reinterprets the complex pair
   * as one scalar — `(_Complex double)a_complex_float` produced garbage. */
  if ((vtop->type.t & VT_COMPLEX) && (type->t & VT_COMPLEX) && (sbt & VT_BTYPE) != (dbt & VT_BTYPE) &&
      is_float(sbt & VT_BTYPE) && is_float(dbt & VT_BTYPE))
  {
    int src_bt2 = sbt & VT_BTYPE;
    int dst_bt2 = dbt & VT_BTYPE;
    int src_sz = (src_bt2 == VT_FLOAT) ? 4 : 8;
    int dst_sz = (dst_bt2 == VT_FLOAT) ? 4 : 8;

    if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
    {
      /* Constant: extract both components from the packed CValue, convert
       * in the compiler, repack for the destination base type. */
      double re, im;
      if (src_bt2 == VT_FLOAT)
      {
        union { float f; uint32_t u; } a, b;
        a.u = (uint32_t)(vtop->c.i & 0xFFFFFFFF);
        b.u = (uint32_t)(vtop->c.i >> 32);
        re = a.f;
        im = b.f;
      }
      else
      {
        memcpy(&re, &vtop->c, 8);
        memcpy(&im, (char *)&vtop->c + 8, 8);
      }
      CValue cv;
      memset(&cv, 0, sizeof(cv));
      if (dst_bt2 == VT_FLOAT)
      {
        union { float f; uint32_t u; } a, b;
        a.f = (float)re;
        b.f = (float)im;
        cv.i = ((uint64_t)b.u << 32) | a.u;
      }
      else
      {
        double dre = re, dim = im;
        memcpy(&cv, &dre, 8);
        memcpy((char *)&cv + 8, &dim, 8);
      }
      vtop->type = *type;
      vtop->c = cv;
      return;
    }

    /* Runtime: the source must be addressable; complex arithmetic results
     * and variables are lvalues already.  Spill a bare rvalue first. */
    if (!(vtop->r & VT_LVAL))
    {
      int sp_vr;
      int sp_loc = get_temp_local_var(2 * src_sz, src_sz, &sp_vr);
      SValue sp;
      memset(&sp, 0, sizeof(sp));
      sp.type = vtop->type;
      sp.r = VT_LOCAL | VT_LVAL;
      sp.vr = sp_vr;
      sp.c.i = sp_loc;
      vpushv(&sp);
      vswap();
      vstore();
      vpop();
      vpushv(&sp);
    }

    SValue src_sv = *vtop;
    vpop();

    int res_vr;
    int res_loc = get_temp_local_var(2 * dst_sz, dst_sz, &res_vr);
    for (int comp = 0; comp < 2; comp++)
    {
      SValue comp_sv = src_sv;
      comp_sv.type.t = src_bt2;
      vpushv(&comp_sv);
      if (comp)
        incr_offset(src_sz);
      gen_cast_s(dst_bt2);
      SValue dst;
      memset(&dst, 0, sizeof(dst));
      dst.type.t = dst_bt2;
      dst.r = VT_LOCAL | VT_LVAL;
      dst.vr = res_vr;
      dst.c.i = res_loc + comp * dst_sz;
      vpushv(&dst);
      vswap();
      vstore();
      vpop();
    }

    SValue result;
    memset(&result, 0, sizeof(result));
    result.type = *type;
    result.r = VT_LOCAL | VT_LVAL;
    result.vr = res_vr;
    result.c.i = res_loc;
    vpushv(&result);
    return;
  }

again:
  if (sbt != dbt)
  {
  process_cast:
    sf = is_float(sbt);
    df = is_float(dbt);
    dbt_bt = dbt & VT_BTYPE;
    sbt_bt = sbt & VT_BTYPE;
    if (dbt_bt == VT_VOID)
    {
      /* `(void)*p` on volatile storage is a mandated read, and this cast is
       * where the type that says so disappears — so perform the load here,
       * before the operand becomes an untyped discard. */
      if ((vtop->r & VT_LVAL) && !nocode_wanted && sbt_bt != VT_STRUCT &&
          ((vtop->type.t & VT_VOLATILE) || vtop->volatile_access ||
           (vtop->sym && (vtop->sym->type.t & VT_VOLATILE))))
        gv(RC_TYPE(vtop->type.t));
      goto done;
    }
    if (sbt_bt == VT_VOID)
    {
    error:
      cast_error(&vtop->type, type);
    }

    c = (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
#if !defined TCC_IS_NATIVE && !defined TCC_IS_NATIVE_387
    /* don't try to convert to ldouble when cross-compiling
       (except when it's '0' which is needed for arm:gen_negf())
       Exception: complex constant casts use memcpy-based repacking that
       doesn't depend on the host's long double representation, so keep
       c=1 for those to avoid falling into the scalar float-to-float path
       which would corrupt the packed {real,imag} CValue. */
    if (dbt_bt == VT_LDOUBLE && !nocode_wanted && (sf || vtop->c.i != 0) && !((vtop->type.t | type->t) & VT_COMPLEX))
      c = 0;
#endif

    /* Handle complex integer constant casts */
    if (c && ((vtop->type.t & VT_COMPLEX) || (type->t & VT_COMPLEX)) && !is_float(vtop->type.t & VT_BTYPE) &&
        !is_float(type->t & VT_BTYPE))
    {
      int src_complex = (vtop->type.t & VT_COMPLEX) != 0;
      int dst_complex = (type->t & VT_COMPLEX) != 0;
      int src_bt = vtop->type.t & VT_BTYPE;
      int dst_bt = type->t & VT_BTYPE;

      if (!src_complex && dst_complex)
      {
        /* int → _Complex int: real = value, imag = 0 */
        uint64_t mask = (dst_bt == VT_LLONG) ? 0xFFFFFFFFFFFFFFFFULL : ((1ULL << (btype_size(dst_bt) * 8)) - 1);
        uint64_t real_val = vtop->c.i & mask;
        vtop->c.i = real_val; /* imag = 0, real = truncated value */
      }
      else if (src_complex && dst_complex)
      {
        /* _Complex int → _Complex int (different sizes): extract, truncate, repack */
        int src_shift = btype_size(src_bt) * 8;
        int dst_shift = btype_size(dst_bt) * 8;
        uint64_t src_mask = (src_bt == VT_LLONG) ? 0xFFFFFFFFFFFFFFFFULL : ((1ULL << src_shift) - 1);
        uint64_t dst_mask = (dst_bt == VT_LLONG) ? 0xFFFFFFFFFFFFFFFFULL : ((1ULL << dst_shift) - 1);
        uint64_t real_val = vtop->c.i & src_mask;
        uint64_t imag_val = (vtop->c.i >> src_shift) & src_mask;
        real_val &= dst_mask;
        imag_val &= dst_mask;
        vtop->c.i = (imag_val << dst_shift) | real_val;
      }
      else if (src_complex && !dst_complex)
      {
        /* _Complex int → int: extract real part only */
        int src_shift = btype_size(src_bt) * 8;
        uint64_t src_mask = (src_bt == VT_LLONG) ? 0xFFFFFFFFFFFFFFFFULL : ((1ULL << src_shift) - 1);
        vtop->c.i = vtop->c.i & src_mask;
      }
      vtop->type = *type;
      goto done;
    }

    /* Handle complex float/double constant casts.
     * Complex float is packed as {real_bits, imag_bits} in CValue.i (64 bits).
     * Complex double is packed as {real, imag} in CValue bytes [0:7] and [8:15].
     * This must be handled before the scalar constant folding code which would
     * corrupt the packed representation. */
    if (c && ((vtop->type.t & VT_COMPLEX) || (type->t & VT_COMPLEX)) &&
        (is_float(vtop->type.t & VT_BTYPE) || is_float(type->t & VT_BTYPE)))
    {
      int src_complex = (vtop->type.t & VT_COMPLEX) != 0;
      int dst_complex = (type->t & VT_COMPLEX) != 0;
      int src_bt = vtop->type.t & VT_BTYPE;
      int dst_bt = type->t & VT_BTYPE;

      /* Helper: extract real and imaginary parts as doubles from source CValue */
      double src_real = 0.0, src_imag = 0.0;
      if (src_complex)
      {
        if (src_bt == VT_FLOAT)
        {
          /* Complex float: packed as {float_real, float_imag} in CValue.i */
          union
          {
            float f;
            uint32_t u;
          } r, i;
          r.u = (uint32_t)(vtop->c.i & 0xFFFFFFFF);
          i.u = (uint32_t)(vtop->c.i >> 32);
          src_real = r.f;
          src_imag = i.f;
        }
        else
        {
          /* Complex double: bytes [0:7] = real, [8:15] = imag */
          memcpy(&src_real, &vtop->c, 8);
          memcpy(&src_imag, (char *)&vtop->c + 8, 8);
        }
      }
      else
      {
        /* Real scalar → complex: imag = 0 */
        if (src_bt == VT_FLOAT)
          src_real = vtop->c.f;
        else if (src_bt == VT_DOUBLE)
          src_real = vtop->c.d;
        else if (src_bt == VT_LDOUBLE)
          src_real = (double)vtop->c.ld;
        else
          src_real = (double)(int64_t)vtop->c.i; /* integer to real */
        src_imag = 0.0;
      }

      if (dst_complex)
      {
        /* Pack into destination complex format */
        memset(&vtop->c, 0, sizeof(CValue));
        if (dst_bt == VT_FLOAT)
        {
          union
          {
            float f;
            uint32_t u;
          } r, i;
          r.f = (float)src_real;
          i.f = (float)src_imag;
          vtop->c.i = (uint64_t)r.u | ((uint64_t)i.u << 32);
        }
        else
        {
          /* Complex double: pack as {real, imag} in CValue */
          double dr = src_real, di = src_imag;
          memcpy(&vtop->c, &dr, 8);
          memcpy((char *)&vtop->c + 8, &di, 8);
        }
      }
      else
      {
        /* Complex → real scalar: extract real part only */
        if (dst_bt == VT_FLOAT)
          vtop->c.f = (float)src_real;
        else if (dst_bt == VT_DOUBLE)
          vtop->c.d = src_real;
        else
          vtop->c.ld = (long double)src_real;
      }
      vtop->type = *type;
      goto done;
    }

    if (c)
    {
      /* constant case: we can do it now */
      /* XXX: in ISOC, cannot do it if error in convert */
      if (sbt == VT_FLOAT)
        vtop->c.ld = vtop->c.f;
      else if (sbt == VT_DOUBLE)
        vtop->c.ld = vtop->c.d;

      if (df)
      {
        if (sbt_bt == VT_LLONG)
        {
          if ((sbt & VT_UNSIGNED) || !(vtop->c.i >> 63))
            vtop->c.ld = vtop->c.i;
          else
            vtop->c.ld = -(long double)-vtop->c.i;
        }
        else if (!sf)
        {
          if ((sbt & VT_UNSIGNED) || !(vtop->c.i >> 31))
            vtop->c.ld = (uint32_t)vtop->c.i;
          else
            vtop->c.ld = -(long double)-(uint32_t)vtop->c.i;
        }

        if (dbt == VT_FLOAT)
          vtop->c.f = (float)vtop->c.ld;
        else if (dbt == VT_DOUBLE)
          vtop->c.d = (double)vtop->c.ld;
      }
      else if (sf && dbt == VT_BOOL)
      {
        vtop->c.i = (vtop->c.ld != 0);
      }
      else
      {
        if (sf)
        {
          if (dbt & VT_UNSIGNED)
          {
            /* Saturate: match ARM VCVT unsigned semantics */
            if (vtop->c.ld < 0)
              vtop->c.i = 0;
            else if (dbt_bt == VT_LLONG)
              vtop->c.i = (vtop->c.ld > 18446744073709551615.0L) ? 0xFFFFFFFFFFFFFFFFULL : (uint64_t)vtop->c.ld;
            else
              vtop->c.i = (vtop->c.ld > 4294967295.0L) ? 0xFFFFFFFFU : (uint64_t)vtop->c.ld;
          }
          else
          {
            /* Saturate: match ARM VCVT signed semantics */
            if (dbt_bt == VT_LLONG)
            {
              if (vtop->c.ld > 9223372036854775807.0L)
                vtop->c.i = 0x7FFFFFFFFFFFFFFFLL;
              else if (vtop->c.ld < -9223372036854775808.0L)
                vtop->c.i = 0x8000000000000000ULL;
              else
                vtop->c.i = (int64_t)vtop->c.ld;
            }
            else
            {
              if (vtop->c.ld > 2147483647.0L)
                vtop->c.i = 0x7FFFFFFF;
              else if (vtop->c.ld < -2147483648.0L)
                vtop->c.i = (uint64_t)(int64_t)-2147483648LL;
              else
                vtop->c.i = (int64_t)vtop->c.ld;
            }
          }
        }
        else if (sbt_bt == VT_LLONG || (PTR_SIZE == 8 && sbt == VT_PTR))
          ;
        else if (sbt & VT_UNSIGNED)
          vtop->c.i = (uint32_t)vtop->c.i;
        else
          vtop->c.i = ((uint32_t)vtop->c.i | -(vtop->c.i & 0x80000000));

        if (dbt_bt == VT_LLONG || (PTR_SIZE == 8 && dbt == VT_PTR))
          ;
        else if (dbt == VT_BOOL)
          vtop->c.i = (vtop->c.i != 0);
        else
        {
          uint32_t m = dbt_bt == VT_BYTE ? 0xff : dbt_bt == VT_SHORT ? 0xffff : 0xffffffff;
          vtop->c.i &= m;
          if (!(dbt & VT_UNSIGNED))
            vtop->c.i |= -(vtop->c.i & ((m >> 1) + 1));
        }
      }
      goto done;
    }
    else if (dbt == VT_BOOL && (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == (VT_CONST | VT_SYM))
    {
      /* addresses are considered non-zero (see tcctest.c:sinit23) */
      vtop->r = VT_CONST;
      vtop->c.i = 1;
      goto done;
    }

    /* cannot generate code for global or static initializers */
    if (nocode_wanted & DATA_ONLY_WANTED)
      goto done;

    /* non constant case: generate code */
    if (dbt == VT_BOOL)
    {
      gen_test_zero(TOK_NE);
      goto done;
    }

    if (sf || df)
    {
      if (sf && df)
      {
        /* convert from fp to fp - emit IR operation */
        SValue dest;
        int dst_is_double = (dbt == VT_DOUBLE || dbt == VT_LDOUBLE);
        dest.type.t = dbt;
        dest.type.ref = NULL;
        dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
        dest.r = 0;
        dest.c.i = 0;
        /* Mark the temp vreg as float/double for register allocation */
        tcc_ir_set_float_type(tcc_state->ir, dest.vr, 1, dst_is_double);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_CVT_FTOF, vtop, NULL, &dest);
        vtop->vr = dest.vr;
        vtop->r = 0;
      }
      else if (df)
      {
        /* convert int to fp - emit IR operation */
        SValue dest;
        int dst_is_double = (dbt == VT_DOUBLE || dbt == VT_LDOUBLE);
        dest.type.t = dbt;
        dest.type.ref = NULL;
        dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
        /* Mark the temp vreg as float/double for register allocation */
        tcc_ir_set_float_type(tcc_state->ir, dest.vr, 1, dst_is_double);
        dest.r = 0;
        dest.c.i = 0;
        tcc_ir_put(tcc_state->ir, TCCIR_OP_CVT_ITOF, vtop, NULL, &dest);
        vtop->vr = dest.vr;
        vtop->r = 0;
      }
      else
      {
        /* convert fp to int - emit IR operation */
        SValue dest;
        sbt = dbt;
        if (dbt_bt != VT_LLONG && dbt_bt != VT_INT)
          sbt = VT_INT;
        dest.type.t = sbt;
        dest.type.ref = NULL;
        dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
        dest.r = 0;
        dest.c.i = 0;
        tcc_ir_put(tcc_state->ir, TCCIR_OP_CVT_FTOI, vtop, NULL, &dest);
        vtop->vr = dest.vr;
        vtop->r = 0;
        goto again; /* may need char/short cast */
      }
      goto done;
    }

    ds = btype_size(dbt_bt);
    ss = btype_size(sbt_bt);
    if (ds == 0 || ss == 0)
      goto error;

    /* same size and no sign conversion needed */
    if (ds == ss && ds >= 4)
      goto done;
    if (dbt_bt == VT_PTR || sbt_bt == VT_PTR)
    {
      tcc_warning("cast between pointer and integer of different size");
      if (sbt_bt == VT_PTR)
      {
        /* put integer type to allow logical operations below */
        vtop->type.t = (PTR_SIZE == 8 ? VT_LLONG : VT_INT);
      }
    }

/* processor allows { int a = 0, b = *(char*)&a; }
   That means that if we cast to less width, we can just
   change the type and read it still later. */
#define ALLOW_SUBTYPE_ACCESS 1

    if (ALLOW_SUBTYPE_ACCESS && (vtop->r & VT_LVAL) && !tcc_state->ir)
    {
      /* value still in memory.
       * NOTE: This optimization is disabled in IR mode because the IR
       * backend may promote stack lvalues to registers during register
       * allocation.  When that happens the byte/halfword memory load
       * that would have done the extension is replaced by a plain
       * register-to-register move, silently dropping the extension.
       * Falling through to the SHL+SAR path below generates explicit
       * IR instructions for the extension which survive regalloc. */
      if (ds <= ss)
      {
        /* For IR mode: when casting from long long to smaller type,
         * we need to generate a proper load of just the low word,
         * not rely on implicit truncation */
        if (ss == 8 && ds <= 4 && vtop->vr < 0)
        {
          /* Generate LOAD IR for the low word only by changing type first */
          vtop->type.t = (vtop->type.t & ~VT_BTYPE) | dbt_bt;
        }
        goto done;
      }
      /* ss <= 4 here */
      if (ds <= 4 && !(dbt == (VT_SHORT | VT_UNSIGNED) && sbt == VT_BYTE))
      {
        gv(RC_INT);
        goto done; /* no 64bit envolved */
      }
    }
    gv(RC_INT);

    trunc = 0;
#if PTR_SIZE == 4
    if (ds == 8)
    {
      /* generate high word */
      if (sbt & VT_UNSIGNED)
      {
        /* IR mode: leave the high word as a constant 0 so lbuild's
         * high_is_const fast path fires (emits a single ZEXT op).
         * Non-IR mode still needs the value materialized in a register. */
        vpushi(0);
        if (!tcc_state->ir)
          gv(RC_INT);
      }
      else
      {
        gv_dup();
        vpushi(31);
        gen_op(TOK_SAR);
      }
      lbuild(dbt);
    }
    else if (ss == 8)
    {
      /* from long long: take low order word
       * IMPORTANT (IR mode): do NOT retag the existing 64-bit vreg as 32-bit.
       * That would break subsequent uses that still need the full 64-bit value
       * (e.g. high-word extraction via SHR #32), causing 32-bit shifts and
       * lost high words. Instead, materialize a new 32-bit temp. */
      if (tcc_state->ir && TCCIR_DECODE_VREG_TYPE(vtop->vr) > 0)
      {
        SValue low32;
        memset(&low32, 0, sizeof(low32));
        low32.type.t = VT_INT | (vtop->type.t & VT_UNSIGNED);
        low32.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
        low32.r = 0;
        int old_prevent_coalescing = tcc_state->ir->prevent_coalescing;
        tcc_state->ir->prevent_coalescing = 1;
        tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, vtop, NULL, &low32);
        tcc_state->ir->prevent_coalescing = old_prevent_coalescing;
        /* Prevent the NEXT ASSIGN from coalescing with this truncation.
         * Without this, a subsequent gv_dup() (e.g. from gen_cast widening
         * in __builtin_mul_overflow) would coalesce its ASSIGN with the
         * truncation ASSIGN, erasing low32's vreg definition while other
         * vstack entries still reference it. */
        tcc_state->ir->basic_block_start = 1;
        vtop->type.t = low32.type.t;
        vtop->vr = low32.vr;
        vtop->r = 0;
      }
      else
      {
        lexpand();
        vpop();
      }
    }
    ss = 4;

#elif PTR_SIZE == 8
    if (ds == 8)
    {
      /* need to convert from 32bit to 64bit */
      if (sbt & VT_UNSIGNED)
      {
#if defined(TCC_TARGET_RISCV64)
        /* RISC-V keeps 32bit vals in registers sign-extended.
           So here we need a zero-extension.  */
        trunc = 32;
#else
        goto done;
#endif
      }
      else
      {
        gen_cvt_sxtw();
        goto done;
      }
      ss = ds, ds = 4, dbt = sbt;
    }
    else if (ss == 8)
    {
      /* RISC-V keeps 32bit vals in registers sign-extended.
         So here we need a sign-extension for signed types and
         zero-extension. for unsigned types. */
#if !defined(TCC_TARGET_RISCV64)
      trunc = 32; /* zero upper 32 bits for non RISC-V targets */
#endif
    }
    else
    {
      ss = 4;
    }
#endif

    if (ds >= ss)
      goto done;
#if defined TCC_TARGET_I386 || defined TCC_TARGET_X86_64 || defined TCC_TARGET_ARM64
    if (ss == 4)
    {
      gen_cvt_csti(dbt);
      goto done;
    }
#endif
    bits = (ss - ds) * 8;
    /* for unsigned, gen_op will convert SAR to SHR */
    vtop->type.t = (ss == 8 ? VT_LLONG : VT_INT) | (dbt & VT_UNSIGNED);
    vpushi(bits);
    gen_op(TOK_SHL);
    vpushi(bits - trunc);
    gen_op(TOK_SAR);
    vpushi(trunc);
    gen_op(TOK_SHR);
  }
done:
  /* The strip below is where a volatile access stops being recognisable: the
   * qualifier is gone before the IR operand is built, and a deref through a
   * register has no Sym to consult afterwards.  Record it first, from the old
   * type and the new one alike. */
  if ((vtop->type.t | type->t) & VT_VOLATILE)
    vtop->volatile_access = 1;
  vtop->type = *type;
  vtop->type.t &= ~(VT_CONSTANT | VT_VOLATILE | VT_ARRAY);
}
