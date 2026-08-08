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

/* simd.c -- __builtin_shuffle and __builtin_convertvector.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* Extracted from unary() to reduce stack frame size. */
void __attribute__((noinline)) unary_builtin_shuffle(void)
{
  switch (tok)
  {
  case TOK_builtin_shuffle:
  case TOK_builtin_shufflevector:
  {
    int tok1 = tok;
    /* __builtin_shuffle(vec, mask) — 2-arg shuffle
     * __builtin_shuffle(vec1, vec2, mask) — 3-arg shuffle
     *
     * Returns a vector where result[i] = source[mask[i] % N].
     * For 3-arg form, source is the concatenation of vec1 and vec2 (size 2N),
     * and mask values are taken modulo 2N.
     */
    next();
    skip('(');
    expr_eq(); /* first vector (vec1) */
    skip(',');
    expr_eq(); /* second arg (vec2 or mask) */

    if (tok1 == TOK_builtin_shufflevector)
    {
      SValue vec1_sv, vec2_sv;
      CType vec1_type, vec2_type, src_elem_type, result_vec_type;
      int src_elem_size, src_elem_align;
      int vec1_elem_count, vec2_elem_count;
      int total_src_elems, result_elem_count;
      int result_size, res_vr, res_loc;
      int *indices = tcc_malloc(64 * sizeof(int));
      int i;

      result_elem_count = 0;
      while (tok == ',')
      {
        if (result_elem_count >= 64)
          tcc_error("too many __builtin_shufflevector indices");
        skip(',');
        indices[result_elem_count++] = expr_const();
      }
      skip(')');

      vec2_sv = *vtop;
      vtop--;
      vec1_sv = *vtop;
      vtop--;

      if (!is_vector_type(&vec1_sv.type) || !is_vector_type(&vec2_sv.type))
        tcc_error("__builtin_shufflevector arguments must be vectors");

      vec1_type = vec1_sv.type;
      vec2_type = vec2_sv.type;
      if (!is_compatible_unqualified_types(&vec1_type.ref->type, &vec2_type.ref->type))
        tcc_error("__builtin_shufflevector argument vectors must have the same element type");

      src_elem_type = vec1_type.ref->type;
      src_elem_size = type_size(&src_elem_type, &src_elem_align);
      vec1_elem_count = vector_elem_count(&vec1_type);
      vec2_elem_count = vector_elem_count(&vec2_type);
      total_src_elems = vec1_elem_count + vec2_elem_count;

      if (result_elem_count < 1 || (result_elem_count & (result_elem_count - 1)) != 0)
        tcc_error("__builtin_shufflevector result element count must be a power of two");

      result_size = result_elem_count * src_elem_size;
      if (result_size > 64)
        tcc_error("__builtin_shufflevector result too large");

      make_vector_type(&result_vec_type, &src_elem_type, result_size);

      /* Constant fold: when both source vectors have compile-time known data,
       * compute the shuffle result entirely in compiler memory. */
      if (!NOEVAL_WANTED)
      {
        int vec1_size = vec1_type.ref->c;
        int vec2_size = vec2_type.ref->c;
        unsigned char *vec1_data = find_sv_const_init(&vec1_sv, vec1_size);
        unsigned char *vec2_data = find_sv_const_init(&vec2_sv, vec2_size);
        if (vec1_data && vec2_data)
        {
          unsigned char result_buf[64];
          memset(result_buf, 0, sizeof(result_buf));

          for (i = 0; i < result_elem_count; i++)
          {
            int src_index = indices[i];
            if (src_index == -1)
              continue;
            const unsigned char *src;
            int elem_off;
            if (src_index < vec1_elem_count)
            {
              src = vec1_data;
              elem_off = src_index * src_elem_size;
            }
            else
            {
              src = vec2_data;
              elem_off = (src_index - vec1_elem_count) * src_elem_size;
            }
            memcpy(result_buf + i * src_elem_size, src + elem_off, src_elem_size);
          }

          res_loc = get_temp_local_var(result_size, result_size > 8 ? 8 : result_size, &res_vr);
          int is_unsigned = (src_elem_type.t & VT_UNSIGNED) != 0;

          for (i = 0; i < result_elem_count; i++)
          {
            int64_t val = read_vec_const_elem(result_buf, src_elem_size, i, is_unsigned);
            SValue res_base;

            vpush64(src_elem_type.t & VT_BTYPE, (unsigned long long)val);

            memset(&res_base, 0, sizeof(res_base));
            res_base.type = result_vec_type;
            res_base.r = VT_LOCAL | VT_LVAL;
            res_base.vr = res_vr;
            res_base.c.i = res_loc;

            vpushv(&res_base);
            gaddrof();
            vtop->type = char_pointer_type;
            vpushi(i * src_elem_size);
            gen_op('+');
            vtop->type = src_elem_type;
            vtop->r |= VT_LVAL;

            vswap();
            vstore();
            vpop();
          }

          attach_const_init_to_temp(res_loc, result_size, result_buf);

          {
            SValue result;
            memset(&result, 0, sizeof(result));
            result.type = result_vec_type;
            result.r = VT_LOCAL | VT_LVAL;
            result.vr = res_vr;
            result.c.i = res_loc;
            vpushv(&result);
          }
          tcc_free(indices);
          break;
        }
      }

      res_loc = get_temp_local_var(result_size, result_size > 8 ? 8 : result_size, &res_vr);

      for (i = 0; i < result_elem_count; ++i)
      {
        int src_index = indices[i];

        if (src_index < -1 || src_index >= total_src_elems)
          tcc_error("__builtin_shufflevector index %d is out of range", src_index);

        if (src_index == -1)
        {
          vpushi(0);
          gen_cast(&src_elem_type);
        }
        else if (src_index < vec1_elem_count)
        {
          vpushv(&vec1_sv);
          gaddrof();
          vtop->type = char_pointer_type;
          vpushi(src_index * src_elem_size);
          gen_op('+');
          vtop->type = src_elem_type;
          vtop->r |= VT_LVAL;
        }
        else
        {
          vpushv(&vec2_sv);
          gaddrof();
          vtop->type = char_pointer_type;
          vpushi((src_index - vec1_elem_count) * src_elem_size);
          gen_op('+');
          vtop->type = src_elem_type;
          vtop->r |= VT_LVAL;
        }

        {
          SValue res_base;
          memset(&res_base, 0, sizeof(res_base));
          res_base.type = result_vec_type;
          res_base.r = VT_LOCAL | VT_LVAL;
          res_base.vr = res_vr;
          res_base.c.i = res_loc;

          vpushv(&res_base);
          gaddrof();
          vtop->type = char_pointer_type;
          vpushi(i * src_elem_size);
          gen_op('+');
          vtop->type = src_elem_type;
          vtop->r |= VT_LVAL;
        }

        vswap();
        vstore();
        vpop();
      }

      {
        SValue result;
        memset(&result, 0, sizeof(result));
        result.type = result_vec_type;
        result.r = VT_LOCAL | VT_LVAL;
        result.vr = res_vr;
        result.c.i = res_loc;
        vpushv(&result);
      }
      tcc_free(indices);
      break;
    }

    int has_two_sources = 0;
    if (tok == ',')
    {
      has_two_sources = 1;
      skip(',');
      expr_eq(); /* third arg (mask) */
    }
    skip(')');

    /* Pop args from vstack */
    SValue mask_sv, vec1_sv, vec2_sv;
    mask_sv = *vtop;
    vtop--;
    if (has_two_sources)
    {
      vec2_sv = *vtop;
      vtop--;
    }
    vec1_sv = *vtop;
    vtop--;

    /* Type validation */
    if (!is_vector_type(&vec1_sv.type))
      tcc_error("__builtin_shuffle arguments must be vectors");
    if (has_two_sources && !is_vector_type(&vec2_sv.type))
      tcc_error("__builtin_shuffle argument vectors must be of the same type");
    if (!is_vector_type(&mask_sv.type))
      tcc_error("__builtin_shuffle last argument must be an integer vector");

    CType src_vec_type = vec1_sv.type;
    CType src_elem_type = src_vec_type.ref->type;
    int src_elem_size, src_elem_align;
    src_elem_size = type_size(&src_elem_type, &src_elem_align);
    int elem_count = vector_elem_count(&src_vec_type);
    int vec_size = src_vec_type.ref->c;

    CType mask_elem_type = mask_sv.type.ref->type;
    int mask_elem_size, mask_elem_align;
    mask_elem_size = type_size(&mask_elem_type, &mask_elem_align);
    int mask_elem_count = vector_elem_count(&mask_sv.type);

    if (elem_count != mask_elem_count)
      tcc_error("__builtin_shuffle element count mismatch");

    int total_src_elems = has_two_sources ? elem_count * 2 : elem_count;

    /* For 3-arg form: concatenate vec1 and vec2 into a contiguous temp */
    SValue concat_sv;
    int concat_vr = 0;
    if (has_two_sources)
    {
      int concat_loc;
      int concat_size = vec_size * 2;
      concat_loc = get_temp_local_var(concat_size, concat_size > 8 ? 8 : concat_size, &concat_vr);

      memset(&concat_sv, 0, sizeof(concat_sv));
      concat_sv.type = src_vec_type;
      concat_sv.r = VT_LOCAL | VT_LVAL;
      concat_sv.vr = concat_vr;
      concat_sv.c.i = concat_loc;

      /* Copy vec1 elements to concat[0..N-1] */
      for (int i = 0; i < elem_count; i++)
      {
        vpushv(&vec1_sv);
        gaddrof();
        vtop->type = char_pointer_type;
        vpushi(i * src_elem_size);
        gen_op('+');
        vtop->type = src_elem_type;
        vtop->r |= VT_LVAL;

        vpushv(&concat_sv);
        gaddrof();
        vtop->type = char_pointer_type;
        vpushi(i * src_elem_size);
        gen_op('+');
        vtop->type = src_elem_type;
        vtop->r |= VT_LVAL;

        vswap();
        vstore();
        vpop();
      }

      /* Copy vec2 elements to concat[N..2N-1] */
      for (int i = 0; i < elem_count; i++)
      {
        vpushv(&vec2_sv);
        gaddrof();
        vtop->type = char_pointer_type;
        vpushi(i * src_elem_size);
        gen_op('+');
        vtop->type = src_elem_type;
        vtop->r |= VT_LVAL;

        vpushv(&concat_sv);
        gaddrof();
        vtop->type = char_pointer_type;
        vpushi((elem_count + i) * src_elem_size);
        gen_op('+');
        vtop->type = src_elem_type;
        vtop->r |= VT_LVAL;

        vswap();
        vstore();
        vpop();
      }
    }

    /* Fast path: when the mask is a local var whose captured const_init_data
     * is still valid, all indices are known at compile time. Emit direct
     * indexed loads (like __builtin_shufflevector) and skip the runtime
     * mask-load + AND. */
    unsigned char *mask_const = NULL;
    if ((mask_sv.r & (VT_VALMASK | VT_LVAL | VT_SYM)) == (VT_LOCAL | VT_LVAL))
    {
      int mask_addr = (int)mask_sv.c.i;
      Sym *s;
      for (s = local_stack; s; s = s->prev)
      {
        if (s->const_init_data && s->const_init_valid && (int)s->c == mask_addr &&
            s->const_init_size >= elem_count * mask_elem_size)
        {
          mask_const = s->const_init_data;
          break;
        }
      }
    }

    /* Identity shortcut: single-source shuffle whose constant mask is
     * exactly {0,1,...,N-1}. Result is vec1_sv directly — no temp slot,
     * no per-element byte copy. Catches e.g. pr52750.c. */
    int identity_result = 0;
    if (mask_const && !has_two_sources)
    {
      int idx_mask = total_src_elems - 1;
      int is_identity = 1;
      for (int i = 0; i < elem_count; i++)
      {
        uint64_t mv = 0;
        switch (mask_elem_size)
        {
        case 1: mv = mask_const[i]; break;
        case 2: mv = read16le(mask_const + i * 2); break;
        case 4: mv = read32le(mask_const + i * 4); break;
        case 8: mv = read64le(mask_const + i * 8); break;
        }
        if ((int)(mv & (uint64_t)idx_mask) != i) { is_identity = 0; break; }
      }
      identity_result = is_identity;
    }

    /* Allocate result vector temp (skipped on identity path) */
    int res_vr = 0, res_loc = 0;
    if (!identity_result)
      res_loc = get_temp_local_var(vec_size, vec_size > 8 ? 8 : vec_size, &res_vr);

    if (identity_result)
      goto shuffle_done;

    if (mask_const)
    {
      int idx_mask = total_src_elems - 1;
      for (int i = 0; i < elem_count; i++)
      {
        uint64_t mv = 0;
        switch (mask_elem_size)
        {
        case 1:
          mv = mask_const[i];
          break;
        case 2:
          mv = read16le(mask_const + i * 2);
          break;
        case 4:
          mv = read32le(mask_const + i * 4);
          break;
        case 8:
          mv = read64le(mask_const + i * 8);
          break;
        }
        int src_index = (int)(mv & (uint64_t)idx_mask);

        /* Load source[src_index] */
        if (has_two_sources)
          vpushv(&concat_sv);
        else
          vpushv(&vec1_sv);
        gaddrof();
        vtop->type = char_pointer_type;
        vpushi(src_index * src_elem_size);
        gen_op('+');
        vtop->type = src_elem_type;
        vtop->r |= VT_LVAL;

        /* Store to result[i] */
        {
          SValue res_base;
          memset(&res_base, 0, sizeof(res_base));
          res_base.type = src_vec_type;
          res_base.r = VT_LOCAL | VT_LVAL;
          res_base.vr = res_vr;
          res_base.c.i = res_loc;

          vpushv(&res_base);
          gaddrof();
          vtop->type = char_pointer_type;
          vpushi(i * src_elem_size);
          gen_op('+');
          vtop->type = src_elem_type;
          vtop->r |= VT_LVAL;
        }

        vswap();
        vstore();
        vpop();
      }
      goto shuffle_done;
    }

    /* For each output element i: result[i] = source[mask[i] % total_src_elems] */
    for (int i = 0; i < elem_count; i++)
    {
      /* Load mask[i] */
      vpushv(&mask_sv);
      gaddrof();
      vtop->type = char_pointer_type;
      vpushi(i * mask_elem_size);
      gen_op('+');
      vtop->type = mask_elem_type;
      vtop->r |= VT_LVAL;

      /* Cast to unsigned int for index computation */
      {
        CType uint_type;
        uint_type.t = VT_INT | VT_UNSIGNED;
        uint_type.ref = NULL;
        gen_cast(&uint_type);
      }

      /* Compute index = mask_val & (total_src_elems - 1)
       * This is equivalent to % total_src_elems when total_src_elems is
       * a power of 2, which is always the case for GCC vector types. */
      vpushi(total_src_elems - 1);
      gen_op('&');

      /* Compute byte_offset = index * src_elem_size */
      if (src_elem_size > 1)
      {
        vpushi(src_elem_size);
        gen_op('*');
      }
      /* vtop = byte_offset */

      /* Compute source base address + byte_offset */
      if (has_two_sources)
      {
        vpushv(&concat_sv);
        gaddrof();
        vtop->type = char_pointer_type;
      }
      else
      {
        vpushv(&vec1_sv);
        gaddrof();
        vtop->type = char_pointer_type;
      }
      /* Stack: byte_offset, base_addr */
      vswap();
      gen_op('+');
      vtop->type = src_elem_type;
      vtop->r |= VT_LVAL;
      /* vtop = source[index] (lvalue) */

      /* Store to result[i] */
      {
        SValue res_base;
        memset(&res_base, 0, sizeof(res_base));
        res_base.type = src_vec_type;
        res_base.r = VT_LOCAL | VT_LVAL;
        res_base.vr = res_vr;
        res_base.c.i = res_loc;

        vpushv(&res_base);
        gaddrof();
        vtop->type = char_pointer_type;
        vpushi(i * src_elem_size);
        gen_op('+');
        vtop->type = src_elem_type;
        vtop->r |= VT_LVAL;
      }

      vswap();
      vstore();
      vpop();
    }
  shuffle_done:;

    /* Push result vector as a local lvalue.
     * On the identity path, vec1_sv IS the result — push it directly. */
    if (identity_result)
    {
      vpushv(&vec1_sv);
    }
    else
    {
      SValue result;
      memset(&result, 0, sizeof(result));
      result.type = src_vec_type;
      result.r = VT_LOCAL | VT_LVAL;
      result.vr = res_vr;
      result.c.i = res_loc;
      vpushv(&result);
    }
    break;
  }
  }
}

/* __builtin_convertvector(vec, type) — element-wise type conversion.
 * The source and target vectors must have the same element count.  Each
 * destination element is the C cast of the matching source element. */
void __attribute__((noinline)) unary_builtin_convertvector(void)
{
  CType dst_vec_type, dst_elem_type, src_vec_type, src_elem_type;
  SValue src_sv;
  int src_elem_count, dst_elem_count;
  int dst_vec_size, dst_elem_size, dst_elem_align;
  int src_elem_size, src_elem_align;
  int res_vr, res_loc;
  int i;

  next();
  skip('(');
  expr_eq();
  skip(',');
  parse_type(&dst_vec_type);
  skip(')');

  src_sv = *vtop;
  vtop--;

  if (!is_vector_type(&src_sv.type))
    tcc_error("__builtin_convertvector first argument must be a vector");
  if (!is_vector_type(&dst_vec_type))
    tcc_error("__builtin_convertvector second argument must be a vector type");

  src_vec_type = src_sv.type;
  src_elem_type = src_vec_type.ref->type;
  dst_elem_type = dst_vec_type.ref->type;
  src_elem_count = vector_elem_count(&src_vec_type);
  dst_elem_count = vector_elem_count(&dst_vec_type);

  if (src_elem_count != dst_elem_count)
    tcc_error("__builtin_convertvector source and target must have same element count");

  src_elem_size = type_size(&src_elem_type, &src_elem_align);
  dst_elem_size = type_size(&dst_elem_type, &dst_elem_align);
  dst_vec_size = dst_vec_type.ref->c;

  res_loc = get_temp_local_var(dst_vec_size, dst_vec_size > 8 ? 8 : dst_vec_size, &res_vr);

  for (i = 0; i < dst_elem_count; i++)
  {
    int src_offset = i * src_elem_size;
    int dst_offset = i * dst_elem_size;
    SValue res_base;

    /* Load src element [i] */
    vpushv(&src_sv);
    gaddrof();
    vtop->type = char_pointer_type;
    vpushi(src_offset);
    gen_op('+');
    vtop->type = src_elem_type;
    vtop->r |= VT_LVAL;

    /* Cast to dst element type */
    gen_cast(&dst_elem_type);

    /* Store to dst[i] */
    memset(&res_base, 0, sizeof(res_base));
    res_base.type = dst_vec_type;
    res_base.r = VT_LOCAL | VT_LVAL;
    res_base.vr = res_vr;
    res_base.c.i = res_loc;

    vpushv(&res_base);
    gaddrof();
    vtop->type = char_pointer_type;
    vpushi(dst_offset);
    gen_op('+');
    vtop->type = dst_elem_type;
    vtop->r |= VT_LVAL;

    vswap();
    vstore();
    vpop();
  }

  {
    SValue result;
    memset(&result, 0, sizeof(result));
    result.type = dst_vec_type;
    result.r = VT_LOCAL | VT_LVAL;
    result.vr = res_vr;
    result.c.i = res_loc;
    vpushv(&result);
  }
}
