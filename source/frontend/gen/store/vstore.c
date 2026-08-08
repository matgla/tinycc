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

/* vstore.c -- Assignment lowering (vstore).
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* store vtop in lvalue pushed on stack */
ST_FUNC void vstore(void)
{
  int sbt, dbt, ft, r, size, align, bit_size, bit_pos, delayed_cast;
  SValue orig_src = *vtop;
  SValue orig_dst = vtop[-1];

  /* Eagerly snapshot source const_init_data before gaddrof in the memmove
   * path invalidates it.  Used at vstore_done to propagate through struct
   * copies. */
  /* Snapshot storage is a fixed stack buffer, not a heap alloc: vstore has many
   * early-return / branch exits (complex, bitfield, scalar, void) that bypass
   * the vstore_done cleanup below, so a tcc_malloc'd snapshot leaked on every
   * non-struct-copy path (LeakSanitizer, e.g. a 4-byte local-to-local scalar
   * store).  The guard bounds the size to 256, and both consumers — the memcpy
   * into dst_sym->const_init_data and attach_const_init_to_temp — copy the
   * bytes rather than retain the pointer, so stack storage is safe and needs no
   * free on any exit path. */
  unsigned char vstore_src_cid_buf[256];
  unsigned char *vstore_src_cid = NULL;
  int vstore_src_cid_size = 0;
  if ((orig_src.r & (VT_VALMASK | VT_LVAL | VT_SYM)) == (VT_LOCAL | VT_LVAL) &&
      (orig_dst.r & (VT_VALMASK | VT_LVAL | VT_SYM)) == (VT_LOCAL | VT_LVAL))
  {
    int src_size = type_size(&vtop->type, &(int){0});
    unsigned char *sd = find_sv_const_init(&orig_src, src_size);
    if (sd && src_size > 0 && src_size <= 256)
    {
      memcpy(vstore_src_cid_buf, sd, src_size);
      vstore_src_cid = vstore_src_cid_buf;
      vstore_src_cid_size = src_size;
    }
  }

  /* Invalidate captured const_init_data for any tracked local sym whose
   * frame range overlaps the destination of this store. Catches direct
   * writes like `m[3] = x`; writes through derived pointers are not
   * tracked here, so const_init_data must be consumed eagerly by callers
   * before the variable's address escapes. */
  if ((vtop[-1].r & (VT_VALMASK | VT_LVAL)) == (VT_LOCAL | VT_LVAL))
  {
    int dst_off = (int)vtop[-1].c.i;
    int dst_align;
    int dst_size = type_size(&vtop[-1].type, &dst_align);
    Sym *s;
    for (s = local_stack; s; s = s->prev)
    {
      if (!s->const_init_data || !s->const_init_valid)
        continue;
      if (s->const_init_in_progress)
        continue;
      int base = (int)s->c;
      if (dst_off + dst_size > base && dst_off < base + s->const_init_size)
      {
        s->const_init_valid = 0;
      }
    }
  }

  /* Track writes to static-storage globals so that inline-eval can decide
   * whether `*&g` in a callee body may fold to the initializer. Two cases
   * poison a sym g for future folds:
   *   (a) a direct store whose lvalue still carries VT_SYM → g
   *   (b) &g is stored into a non-const pointer lvalue — the pointer
   *       could later be used to write g indirectly. */
  if (!nocode_wanted)
  {
    if ((vtop[-1].r & VT_SYM) && vtop[-1].sym && (vtop[-1].r & VT_VALMASK) == VT_CONST)
      vtop[-1].sym->a.possibly_written = 1;
    if ((vtop->r & (VT_VALMASK | VT_SYM | VT_LVAL)) == (VT_CONST | VT_SYM) && vtop->sym &&
        (vtop[-1].type.t & VT_BTYPE) == VT_PTR)
    {
      CType *pointed = pointed_type(&vtop[-1].type);
      if (pointed && !(pointed->t & VT_CONSTANT))
        vtop->sym->a.possibly_written = 1;
    }
  }

  ft = vtop[-1].type.t;
  sbt = vtop->type.t & VT_BTYPE;
  dbt = ft & VT_BTYPE;

  verify_assign_cast(&vtop[-1].type);

  /* If destination is complex but source is not, cast source to complex first
   * so the complex store path below handles both components (real + imag). */
  if ((ft & VT_COMPLEX) && !(vtop->type.t & VT_COMPLEX))
    gen_cast(&vtop[-1].type);

  /* Complex-to-complex assignment: decompose into component-wise stores.
   * When base types differ (e.g. float complex → double complex), each
   * component is individually cast.  When they match, we use memcpy.
   * When base types differ, first convert to a local temp, then memcpy.
   * When the source is a constant, decompose into two scalar stores
   * to avoid gaddrof() on a constant (which can't produce a valid address). */
  if ((ft & VT_COMPLEX) && (vtop->type.t & VT_COMPLEX))
  {
    int src_bt = vtop->type.t & VT_BTYPE;
    int dst_bt = ft & VT_BTYPE;
    int src_is_const = (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;

    /* Constant complex float/double: materialize to a temp local first,
     * then let the memcpy path below copy it to the destination.
     * We can't gaddrof() a VT_CONST complex directly.
     *
     * Fast path: when the destination is a stack local AND base types
     * match, materialize the constant directly into the destination
     * slots — skips the temp + copy entirely. */
    if (src_is_const && is_float(src_bt))
    {
      double src_real = 0.0, src_imag = 0.0;
      int src_elem_size = (src_bt == VT_DOUBLE || src_bt == VT_LDOUBLE) ? 8 : 4;
      int src_total = src_elem_size * 2;

      /* Extract components from constant */
      if (src_bt == VT_FLOAT)
      {
        union
        {
          float f;
          uint32_t u;
        } r, im;
        r.u = (uint32_t)(vtop->c.i & 0xFFFFFFFF);
        im.u = (uint32_t)(vtop->c.i >> 32);
        src_real = r.f;
        src_imag = im.f;
      }
      else
      {
        memcpy(&src_real, &vtop->c, 8);
        memcpy(&src_imag, (char *)&vtop->c + 8, 8);
      }

      if (src_bt == dst_bt && tcc_state->ir && !NOEVAL_WANTED &&
          (vtop[-1].r & (VT_VALMASK | VT_LVAL)) == (VT_LOCAL | VT_LVAL))
      {
        /* Direct materialization into dst — emit two scalar stores and skip
         * the convert/memcpy path entirely. */
        SValue dst_save = vtop[-1];
        vpop();        /* pop constant */
        vtop--;        /* drop dst from the stack (we own it via dst_save) */

        CType elem_type;
        elem_type.t = src_bt;
        elem_type.ref = NULL;

        /* Store real part to dst */
        {
          SValue elem_dst = dst_save;
          elem_dst.type = elem_type;
          vpushv(&elem_dst);
          CValue cv;
          memset(&cv, 0, sizeof(cv));
          if (src_bt == VT_FLOAT)
            cv.f = (float)src_real;
          else
            cv.d = src_real;
          vsetc(&elem_type, VT_CONST, &cv);
          vstore();
          vpop();
        }

        /* Store imag part to dst + elem_size */
        {
          SValue elem_dst = dst_save;
          elem_dst.type = elem_type;
          elem_dst.c.i = dst_save.c.i + src_elem_size;
          vpushv(&elem_dst);
          CValue cv;
          memset(&cv, 0, sizeof(cv));
          if (src_bt == VT_FLOAT)
            cv.f = (float)src_imag;
          else
            cv.d = src_imag;
          vsetc(&elem_type, VT_CONST, &cv);
          vstore();
          vpop();
        }

        /* Push dst back as the assignment expression result */
        vpushv(&dst_save);
        return;
      }

      /* Allocate a temp local to hold the complex constant */
      int tmp_vr;
      int tmp_loc = get_temp_local_var(src_total, src_elem_size, &tmp_vr);

      /* Replace vtop (the constant) with two scalar stores into the temp */
      vpop(); /* remove the complex constant */

      /* Store real part to temp */
      {
        CType elem_type;
        elem_type.t = src_bt;
        elem_type.ref = NULL;
        SValue tmp_dst;
        memset(&tmp_dst, 0, sizeof(tmp_dst));
        tmp_dst.type = elem_type;
        tmp_dst.r = VT_LOCAL | VT_LVAL;
        tmp_dst.vr = tmp_vr;
        tmp_dst.c.i = tmp_loc;
        vpushv(&tmp_dst);
        CValue cv;
        memset(&cv, 0, sizeof(cv));
        if (src_bt == VT_FLOAT)
          cv.f = (float)src_real;
        else
          cv.d = src_real;
        vsetc(&elem_type, VT_CONST, &cv);
        vstore();
        vpop();
      }

      /* Store imag part to temp+offset */
      {
        CType elem_type;
        elem_type.t = src_bt;
        elem_type.ref = NULL;
        SValue tmp_dst;
        memset(&tmp_dst, 0, sizeof(tmp_dst));
        tmp_dst.type = elem_type;
        tmp_dst.r = VT_LOCAL | VT_LVAL;
        tmp_dst.vr = tmp_vr;
        tmp_dst.c.i = tmp_loc + src_elem_size;
        vpushv(&tmp_dst);
        CValue cv;
        memset(&cv, 0, sizeof(cv));
        if (src_bt == VT_FLOAT)
          cv.f = (float)src_imag;
        else
          cv.d = src_imag;
        vsetc(&elem_type, VT_CONST, &cv);
        vstore();
        vpop();
      }

      /* Push temp local as the new source (complex lvalue) */
      {
        SValue src_sv;
        memset(&src_sv, 0, sizeof(src_sv));
        src_sv.type = vtop->type; /* use dest type since they match at this point */
        src_sv.type.t = (src_sv.type.t & ~VT_BTYPE) | src_bt | VT_COMPLEX;
        src_sv.r = VT_LOCAL | VT_LVAL;
        src_sv.vr = tmp_vr;
        src_sv.c.i = tmp_loc;
        vpushv(&src_sv);
      }
      /* Fall through to the memcpy path below with the temp as source */
    }

    /* Constant complex integer: materialize to a temp local first,
     * then let the memcpy path below copy it to the destination.
     * We can't gaddrof() a VT_CONST integer complex directly. */
    if (src_is_const && !is_float(src_bt))
    {
      int src_elem_size = btype_size(src_bt);
      int src_total = src_elem_size * 2;
      int shift = src_elem_size * 8;
      uint64_t packed = vtop->c.i;
      uint64_t mask = (src_bt == VT_LLONG) ? 0xFFFFFFFFFFFFFFFFULL : ((1ULL << shift) - 1);
      int64_t src_real = (int64_t)(packed & mask);
      int64_t src_imag = (int64_t)((packed >> shift) & mask);

      /* Allocate a temp local to hold the complex constant */
      int tmp_vr;
      int tmp_loc = get_temp_local_var(src_total, src_elem_size, &tmp_vr);

      /* Replace vtop (the constant) with two scalar stores into the temp */
      vpop(); /* remove the complex constant */

      /* Store real part to temp */
      {
        CType elem_type;
        elem_type.t = src_bt;
        elem_type.ref = NULL;
        SValue tmp_dst;
        memset(&tmp_dst, 0, sizeof(tmp_dst));
        tmp_dst.type = elem_type;
        tmp_dst.r = VT_LOCAL | VT_LVAL;
        tmp_dst.vr = tmp_vr;
        tmp_dst.c.i = tmp_loc;
        vpushv(&tmp_dst);
        CValue cv;
        memset(&cv, 0, sizeof(cv));
        cv.i = src_real;
        vsetc(&elem_type, VT_CONST, &cv);
        vstore();
        vpop();
      }

      /* Store imag part to temp+offset */
      {
        CType elem_type;
        elem_type.t = src_bt;
        elem_type.ref = NULL;
        SValue tmp_dst;
        memset(&tmp_dst, 0, sizeof(tmp_dst));
        tmp_dst.type = elem_type;
        tmp_dst.r = VT_LOCAL | VT_LVAL;
        tmp_dst.vr = tmp_vr;
        tmp_dst.c.i = tmp_loc + src_elem_size;
        vpushv(&tmp_dst);
        CValue cv;
        memset(&cv, 0, sizeof(cv));
        cv.i = src_imag;
        vsetc(&elem_type, VT_CONST, &cv);
        vstore();
        vpop();
      }

      /* Push temp local as the new source (complex lvalue) */
      {
        SValue src_sv;
        memset(&src_sv, 0, sizeof(src_sv));
        src_sv.type = vtop->type; /* use dest type since they match at this point */
        src_sv.type.t = (src_sv.type.t & ~VT_BTYPE) | src_bt | VT_COMPLEX;
        src_sv.r = VT_LOCAL | VT_LVAL;
        src_sv.vr = tmp_vr;
        src_sv.c.i = tmp_loc;
        vpushv(&src_sv);
      }
      /* Fall through to the memcpy path below with the temp as source */
    }

    /* Non-lvalue complex vreg source (computed expression, e.g., a + b):
     * The value lives in a register pair, not in memory. We can't take
     * its address for memcpy. Generate a direct STORE/ASSIGN instead.
     * The backend's STORE handler already supports 64-bit pair stores. */
    if (!(vtop->r & VT_LVAL) && !src_is_const && is_float(src_bt) && src_bt == dst_bt)
    {
      int op = TCCIR_OP_STORE;
      if ((vtop[-1].r & VT_VALMASK) == VT_LOCAL && vtop[-1].vr != -1)
        op = TCCIR_OP_ASSIGN;

      /* Ensure destination type matches for a complex pair store. */
      vtop[-1].type.t = (vtop[-1].type.t & ~VT_BTYPE) | src_bt;

      tcc_ir_put(tcc_state->ir, op, vtop, NULL, &vtop[-1]);

      if (op == TCCIR_OP_ASSIGN)
      {
        vtop->vr = vtop[-1].vr;
        vtop->r = 0;
      }
      vswap();
      vtop--; /* remove destination, keep assignment result */
      return;
    }

    /* If base types differ, convert component-wise into a temp first */
    if (src_bt != dst_bt)
    {
      int src_elem_size = (src_bt == VT_DOUBLE || src_bt == VT_LDOUBLE) ? 8 : 4;
      int dst_elem_size = (dst_bt == VT_DOUBLE || dst_bt == VT_LDOUBLE) ? 8 : 4;
      int dst_total = dst_elem_size * 2;

      CType src_elem_type;
      src_elem_type.t = (vtop->type.t & ~VT_BTYPE & ~VT_COMPLEX) | src_bt;
      src_elem_type.ref = vtop->type.ref;

      CType dst_elem_type;
      dst_elem_type.t = (ft & ~VT_BTYPE & ~VT_COMPLEX) | dst_bt;
      dst_elem_type.ref = vtop[-1].type.ref;

      CType dst_complex_type;
      dst_complex_type.t = (ft & ~VT_BTYPE) | dst_bt; /* keeps VT_COMPLEX */
      dst_complex_type.ref = vtop[-1].type.ref;

      /* Allocate temporary for the converted complex value */
      int res_vr;
      int res_loc = get_temp_local_var(dst_total, dst_elem_size, &res_vr);

      /* Save original source */
      SValue orig_src = *vtop;
      vpop();

      /* Convert real part */
      vpushv(&orig_src);
      vtop->type = src_elem_type;
      gen_cast(&dst_elem_type);
      {
        SValue tmp_dst;
        memset(&tmp_dst, 0, sizeof(tmp_dst));
        tmp_dst.type = dst_elem_type;
        tmp_dst.r = VT_LOCAL | VT_LVAL;
        tmp_dst.vr = res_vr;
        tmp_dst.c.i = res_loc;
        vpushv(&tmp_dst);
        vswap();
        vstore();
        vpop();
      }

      /* Convert imag part */
      vpushv(&orig_src);
      vtop->type = src_elem_type;
      vtop->c.i += src_elem_size;
      gen_cast(&dst_elem_type);
      {
        SValue tmp_dst;
        memset(&tmp_dst, 0, sizeof(tmp_dst));
        tmp_dst.type = dst_elem_type;
        tmp_dst.r = VT_LOCAL | VT_LVAL;
        tmp_dst.vr = res_vr;
        tmp_dst.c.i = res_loc + dst_elem_size;
        vpushv(&tmp_dst);
        vswap();
        vstore();
        vpop();
      }

      /* Replace source with the converted temp */
      SValue conv_src;
      memset(&conv_src, 0, sizeof(conv_src));
      conv_src.type = dst_complex_type;
      conv_src.r = VT_LOCAL | VT_LVAL;
      conv_src.vr = res_vr;
      conv_src.c.i = res_loc;
      vpushv(&conv_src);
      /* Fall through: now src and dst have the same base type,
       * use the struct-copy path below. */
    }

    /* Same base type: use memcpy (struct-copy path).
     * Complex types are laid out as {real, imag} in memory, so
     * a byte-for-byte copy is correct. */
    {
      int complex_size, complex_align;
      complex_size = type_size(&vtop->type, &complex_align);

      /* For small, word-aligned complex copies between stack locals,
       * expand to individual word LOAD/STORE pairs in the IR — mirrors
       * the small-struct optimization in the VT_STRUCT branch.  Skipping
       * the memmove call exposes the stores to store-load forwarding and
       * DCE, which is critical for eliminating dead complex assignments
       * (e.g. `_Complex float z = test_add(x,y);` when z is unused). */
      if (tcc_state->ir && complex_size <= 32 && !(complex_size & 3) &&
          !(complex_align & 3) &&
          (vtop[0].r & (VT_VALMASK | VT_LVAL)) == (VT_LOCAL | VT_LVAL) &&
          (vtop[-1].r & (VT_VALMASK | VT_LVAL)) == (VT_LOCAL | VT_LVAL) &&
          !NOEVAL_WANTED)
      {
        CType saved_complex_type = vtop->type;
        SValue src = vtop[0];
        SValue dst = vtop[-1];
        vtop--; /* pop src; vtop = dst (kept as result lvalue) */

        /* NRVO same-slot fast-path: when the source and destination refer
         * to the same stack slot (e.g. NRVO redirected a call's sret
         * buffer into the destination), the copy is a no-op.  Skip it. */
        if (src.c.i == dst.c.i)
        {
          vtop->type = saved_complex_type;
          return;
        }

        CType word_type;
        word_type.t = VT_INT;
        word_type.ref = NULL;

        for (int off = 0; off < complex_size; off += 4)
        {
          SValue s, d, tmp;
          svalue_init(&s);
          s.type = word_type;
          s.r = VT_LOCAL | VT_LVAL;
          s.vr = src.vr;
          s.c.i = src.c.i + off;

          svalue_init(&tmp);
          tmp.type = word_type;
          tmp.r = 0;
          tmp.vr = tcc_ir_get_vreg_temp(tcc_state->ir);

          tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, &s, NULL, &tmp);

          svalue_init(&d);
          d.type = word_type;
          d.r = VT_LOCAL | VT_LVAL;
          d.vr = dst.vr;
          d.c.i = dst.c.i + off;

          tcc_ir_put(tcc_state->ir, TCCIR_OP_STORE, &tmp, NULL, &d);
        }

        vtop->type = saved_complex_type;
        return;
      }

      /* destination */
      vpushv(vtop - 1);
      vtop->type.t = VT_PTR;
      gaddrof();
      /* source */
      vswap();
      vtop->type.t = VT_PTR;
      gaddrof();
      /* size */
      vpushi(complex_size);
#ifdef TCC_ARM_EABI
      if (!(complex_align & 3))
        vpush_helper_func(TOK_memmove4);
      else
#endif
        vpush_helper_func(TOK_memmove);
      {
        SValue param_num;
        const int call_id = tcc_state->ir ? tcc_state->ir->next_call_id++ : 0;
        svalue_init(&param_num);
        param_num.vr = -1;
        param_num.r = VT_CONST;

        param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 0);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[-3], &param_num, NULL);
        param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 1);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[-2], &param_num, NULL);
        param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 2);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[-1], &param_num, NULL);

        SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, 3);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVOID, &vtop[0], &call_id_sv, NULL);
        vtop -= 4;
      }
    }
    return;
  }

  if (sbt == VT_STRUCT)
  {
    /* if structure, only generate pointer */
    /* structure assignment : generate memcpy */
    int has_vla = struct_has_vla_member(&vtop->type);
    CType saved_struct_type = vtop->type; /* save before gaddrof destroys it */
    size = type_size(&vtop->type, &align);

    /* Self-copy elision: source and destination are the same register-deref
     * lvalue (same address vreg, same offset).  This is the post-call copy of
     * a register-deref NRVO claim (`local.field = sret_call(...)`), where the
     * call already wrote its result directly through the destination address —
     * making the copy a no-op.  Also catches genuine struct self-assignment.
     * Placed before the size-gated inline-copy paths so it applies to any
     * struct size. */
    if (tcc_state->ir && !NOEVAL_WANTED &&
        (vtop[0].r & VT_LVAL) && (vtop[0].r & VT_VALMASK) < VT_CONST &&
        (vtop[-1].r & VT_LVAL) && (vtop[-1].r & VT_VALMASK) < VT_CONST &&
        vtop[0].vr >= 0 && vtop[0].vr == vtop[-1].vr &&
        vtop[0].c.i == vtop[-1].c.i)
    {
      vtop--; /* pop src; vtop = dst (kept as result lvalue) */
      vtop->type = saved_struct_type;
      goto vstore_done;
    }

    /* For small struct copies between stack locals/globals, expand to scalar
     * LOAD/STORE pairs in the IR.  This makes the stores visible to the
     * optimizer (store-load forwarding, constant propagation, DCE) instead of
     * hiding them behind an opaque memmove call.  Keep the existing word-copy
     * case, and only add a narrow path for packed 2-byte single-field
     * local-to-local wrappers; broader non-word copies can perturb
     * return-in-register and global bitfield cases where later passes still
     * prefer the memmove form. */
#define IS_GLOBAL_LVAL(r) \
  (((r) & (VT_VALMASK | VT_LVAL | VT_SYM)) == (VT_CONST | VT_LVAL | VT_SYM))
#define IS_LOCAL_LVAL(r) \
  (((r) & (VT_VALMASK | VT_LVAL)) == (VT_LOCAL | VT_LVAL))
    int is_vec_small = (vtop->type.t & VT_VECTOR) && (size == 1 || size == 2);
    /* Single-element vector results from gen_op_vector are rvalues in a
     * vreg.  Accept them as a source here so we can emit a direct STORE
     * to the dst lvalue without spilling through a temp slot. */
    int src_is_vec_rvalue = is_vec_small &&
                            (vtop[0].r & VT_LVAL) == 0 &&
                            ((vtop[0].r & VT_VALMASK) < VT_CONST) &&
                            vtop[0].vr >= 0;
    int is_local_copy = (IS_LOCAL_LVAL(vtop[0].r) || src_is_vec_rvalue) &&
                         IS_LOCAL_LVAL(vtop[-1].r);
    int is_global_copy = IS_GLOBAL_LVAL(vtop[0].r) && IS_GLOBAL_LVAL(vtop[-1].r);
    /* Mixed global<->local word copies (`struct y = global;` and the reverse)
     * are the dominant struct-init idiom.  Expanding them to scalar LOAD/STORE
     * (instead of an opaque memmove) exposes the bytes to store-load forwarding
     * and DSE, which lets the optimizer fold a copied-then-field-read aggregate
     * directly into the field access.  Restricted to the word-aligned path
     * below (the size==1/2 narrow paths stay local-only). */
    int is_mixed_copy =
        (IS_GLOBAL_LVAL(vtop[0].r) && IS_LOCAL_LVAL(vtop[-1].r)) ||
        (IS_LOCAL_LVAL(vtop[0].r) && IS_GLOBAL_LVAL(vtop[-1].r));
    /* Mixed copies are only a win when the inline LOAD/STOREs are no larger
     * than the memmove call they replace (~4 insns) AND/OR the optimizer can
     * forward+DCE them.  Beyond two words the inline form just bloats code
     * that a memmove handled in one call (e.g. structret's 24-byte structs),
     * so cap mixed copies at 8 bytes.  Local/global copies keep their
     * previously-tuned wider limits. */
    int mixed_limit = struct_has_bitfield_member(&vtop->type) ? 16 : 4;
    int size_limit = is_local_copy ? 64 : (is_mixed_copy ? mixed_limit : 32);
    if (tcc_state->ir && !has_vla && size > 0 && size <= size_limit &&
        ((!(size & 3) && !(align & 3)) ||
         (size == 2 && (align == 1 || is_vec_small) &&
          (struct_is_single_2byte_scalar_member(&vtop->type) || is_vec_small) &&
          !((vtop[0].c.i | vtop[-1].c.i) & 1) &&
          is_local_copy) ||
         /* Packed all-bitfield struct that fits one 2- or 4-byte storage unit:
          * copy as a single (possibly unaligned) halfword/word whose width
          * matches how the bitfields are read back.  Allow mixed global<->local
          * (the `struct y = global;` init and the identity-`retme` round-trip
          * in the 20040709 bitfield idiom): exposing the value to store-load
          * forwarding is what lets the downstream bitfield insert/extract fold
          * collapse the whole copy.  Offsets must be aligned to the access
          * width so the half/word access stays in-bounds of its slot. */
         (size == 2 && align == 1 &&
          struct_is_small_bitfield_word(&vtop->type) &&
          !((vtop[0].c.i | vtop[-1].c.i) & 1) &&
          (is_local_copy || is_mixed_copy)) ||
         (size == 4 && align == 1 &&
          struct_is_small_bitfield_word(&vtop->type) &&
          !((vtop[0].c.i | vtop[-1].c.i) & 3) &&
          (is_local_copy || is_mixed_copy)) ||
         (size == 1 && align == 1 &&
          (struct_is_single_1byte_scalar_member(&vtop->type) || is_vec_small) &&
          is_local_copy)) &&
        (is_local_copy || is_global_copy || is_mixed_copy) &&
        !NOEVAL_WANTED)
    {
      SValue src = vtop[0];
      SValue dst = vtop[-1];
      vtop--; /* pop src; vtop = dst (kept as result lvalue) */

      /* NRVO same-slot fast-path (locals only): src and dst at the
       * same stack offset means the call already wrote into dst. */
      if ((src.r & VT_VALMASK) == VT_LOCAL &&
          (dst.r & VT_VALMASK) == VT_LOCAL && src.c.i == dst.c.i)
      {
        vtop->type = saved_struct_type;
        goto vstore_done;
      }

      if (size == 1 && align == 1)
      {
        SValue d, tmp;
        CType copy_type;

        copy_type.ref = NULL;
        copy_type.t = VT_BYTE | VT_UNSIGNED;

        svalue_init(&tmp);
        tmp.type = copy_type;
        tmp.r = 0;

        if (src_is_vec_rvalue)
        {
          /* Value already in src.vr — emit STORE directly. */
          tmp.vr = src.vr;
        }
        else
        {
          SValue s;
          svalue_init(&s);
          s.type = copy_type;
          s.r = src.r;
          s.vr = src.vr;
          s.sym = src.sym;
          s.c.i = src.c.i;
          tmp.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
          tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, &s, NULL, &tmp);
        }

        svalue_init(&d);
        d.type = copy_type;
        d.r = dst.r;
        d.vr = dst.vr;
        d.sym = dst.sym;
        d.c.i = dst.c.i;

        tcc_ir_put(tcc_state->ir, TCCIR_OP_STORE, &tmp, NULL, &d);
      }
      else if (size == 2 && align == 1)
      {
        SValue d, tmp;
        CType copy_type;

        copy_type.ref = NULL;
        copy_type.t = VT_SHORT | VT_UNSIGNED;

        svalue_init(&tmp);
        tmp.type = copy_type;
        tmp.r = 0;

        if (src_is_vec_rvalue)
        {
          tmp.vr = src.vr;
        }
        else
        {
          SValue s;
          svalue_init(&s);
          s.type = copy_type;
          s.r = src.r;
          s.vr = src.vr;
          s.sym = src.sym;
          s.c.i = src.c.i;
          tmp.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
          tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, &s, NULL, &tmp);
        }

        svalue_init(&d);
        d.type = copy_type;
        d.r = dst.r;
        d.vr = dst.vr;
        d.sym = dst.sym;
        d.c.i = dst.c.i;

        tcc_ir_put(tcc_state->ir, TCCIR_OP_STORE, &tmp, NULL, &d);
      }
      else
      {
        CType word_type;
        word_type.t = VT_INT;
        word_type.ref = NULL;

        for (int off = 0; off < size; off += 4)
        {
          SValue s, d, tmp;
          svalue_init(&s);
          s.type = word_type;
          s.r = src.r;
          s.vr = src.vr;
          s.sym = src.sym;
          s.c.i = src.c.i + off;

          svalue_init(&tmp);
          tmp.type = word_type;
          tmp.r = 0;
          tmp.vr = tcc_ir_get_vreg_temp(tcc_state->ir);

          tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, &s, NULL, &tmp);

          svalue_init(&d);
          d.type = word_type;
          d.r = dst.r;
          d.vr = dst.vr;
          d.sym = dst.sym;
          d.c.i = dst.c.i + off;

          tcc_ir_put(tcc_state->ir, TCCIR_OP_STORE, &tmp, NULL, &d);
        }
      }

      vtop->type = saved_struct_type;
      goto vstore_done;
    }

    /* Parallel inline path: source is a LOCAL or GLOBAL lvalue, destination is
     * a deref-through-vreg lvalue (e.g. the LHS of `u.z = sret_call(...)` has
     * had its address captured into a vreg before the call).  Word-copy
     * through explicit ADDs on the destination vreg — like the complex sret
     * return inline copy — avoids a memmove for small word-aligned struct
     * assignments. */
#define IS_REG_DEREF_LVAL(r) \
  (((r) & VT_LVAL) && ((r) & VT_VALMASK) < VT_CONST)
    /* Cap at 8 bytes: see same-named cap in gfunc_return's struct path
     * — store-forwarding width-mismatch in the optimizer would feed stale
     * zero-init bytes to the LOADs we emit otherwise.
     *
     * That hazard needs a LOCAL/GLOBAL source: it is an earlier store to the
     * source's own storage, at a different width than our LOAD, that the
     * forwarder mis-narrows (pr92618's 16-byte vector literal built from
     * scalar components).  When BOTH sides are register-deref pointers the
     * source is opaque memory with no such store in view, so the plain
     * `*d = *s` shape — the common one, and the one that otherwise costs a
     * __aeabi_memmove4 call — is allowed up to 16 bytes.  Sixteen is also the
     * point where an LDM/STM pair stops fitting the scratch budget. */
    int both_reg_deref = IS_REG_DEREF_LVAL(vtop[-1].r) && IS_REG_DEREF_LVAL(vtop[0].r);
    int inline_copy_max = both_reg_deref ? 16 : 8;
    if (tcc_state->ir && !has_vla && size > 0 && size <= inline_copy_max &&
        !(size & 3) && !(align & 3) && !NOEVAL_WANTED &&
        IS_REG_DEREF_LVAL(vtop[-1].r) &&
        (IS_LOCAL_LVAL(vtop[0].r) || IS_GLOBAL_LVAL(vtop[0].r) ||
         IS_REG_DEREF_LVAL(vtop[0].r)))
    {
      SValue src = vtop[0];
      SValue dst = vtop[-1];
      vtop--; /* pop src; vtop = dst (kept as result lvalue) */

      CType word_type;
      word_type.t = VT_INT;
      word_type.ref = NULL;

      /* Snapshot the destination pointer in its own vreg so we can compute
       * dst_ptr + off via ADD for non-zero offsets without disturbing the
       * caller's view of vtop[-1] (which we keep around as the assignment
       * result).  At off == 0 we use dst.vr directly.
       *
       * The codegen for VT_LVAL register-deref ignores c.i (it's not a
       * frame offset like VT_LOCAL).  For non-zero offsets we MUST compute
       * the address explicitly with ADD — both for src and dst when they
       * are register-deref lvalues. */
      int src_is_reg_deref = ((src.r & VT_VALMASK) < VT_CONST);

      SValue dst_base;
      memset(&dst_base, 0, sizeof(dst_base));
      dst_base.type.t = VT_PTR;
      dst_base.vr = dst.vr;
      dst_base.r = 0;

      SValue src_base;
      memset(&src_base, 0, sizeof(src_base));
      src_base.type.t = VT_PTR;
      src_base.vr = src.vr;
      src_base.r = 0;

      /* Emit all LOADs first, then all STOREs.  An interleaved LOAD/STORE
       * pattern lets a subsequent STORE through *dst_base act as an alias
       * barrier and stop store-forwarding from reaching the second LOAD
       * from the source (e.g. parameter spill slot).  Front-loading the
       * LOADs lets each load see the param/spill stores cleanly. */
      int n_words = size / 4;
      int tmp_vregs[32 / 4];
      for (int i = 0; i < n_words; ++i)
      {
        int off = i * 4;
        SValue s, tmp;
        svalue_init(&s);
        s.type = word_type;

        if (src_is_reg_deref && off != 0)
        {
          /* Compute src_base + off via ADD; LOAD through the new vreg.
           * c.i on a register-deref lvalue is not honored by the codegen. */
          SValue off_imm;
          svalue_init(&off_imm);
          off_imm.type.t = VT_INT;
          off_imm.r = VT_CONST;
          off_imm.vr = -1;
          off_imm.c.i = off;

          SValue src_ptr;
          svalue_init(&src_ptr);
          src_ptr.type.t = VT_PTR;
          src_ptr.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
          src_ptr.r = 0;

          tcc_ir_put(tcc_state->ir, TCCIR_OP_ADD, &src_base, &off_imm, &src_ptr);

          s.r = VT_LVAL;
          s.vr = src_ptr.vr;
        }
        else
        {
          s.r = src.r;
          s.vr = src.vr;
          s.sym = src.sym;
          s.c.i = src.c.i + off;
        }

        svalue_init(&tmp);
        tmp.type = word_type;
        tmp.r = 0;
        tmp.vr = tcc_ir_get_vreg_temp(tcc_state->ir);

        tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, &s, NULL, &tmp);
        tmp_vregs[i] = tmp.vr;
      }

      for (int i = 0; i < n_words; ++i)
      {
        int off = i * 4;
        SValue tmp;
        svalue_init(&tmp);
        tmp.type = word_type;
        tmp.r = 0;
        tmp.vr = tmp_vregs[i];

        SValue dst_ptr;
        if (off == 0)
        {
          dst_ptr = dst_base;
        }
        else
        {
          SValue off_imm;
          svalue_init(&off_imm);
          off_imm.type.t = VT_INT;
          off_imm.r = VT_CONST;
          off_imm.vr = -1;
          off_imm.c.i = off;

          svalue_init(&dst_ptr);
          dst_ptr.type.t = VT_PTR;
          dst_ptr.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
          dst_ptr.r = 0;

          tcc_ir_put(tcc_state->ir, TCCIR_OP_ADD, &dst_base, &off_imm, &dst_ptr);
        }

        SValue store_dst;
        svalue_init(&store_dst);
        store_dst.type = word_type;
        store_dst.r = VT_LVAL;
        store_dst.vr = dst_ptr.vr;

        tcc_ir_put(tcc_state->ir, TCCIR_OP_STORE, &tmp, NULL, &store_dst);
      }

      vtop->type = saved_struct_type;
      goto vstore_done;
    }

    /* Member-wise copy for a small packed struct that holds a bitfield in a
     * 1/2/4-byte storage unit (the 20040709 idiom: `struct y = global;` and the
     * identity-`retme` round-trip).  Packed structs have align 1, so they miss
     * every word-aligned scalar-expansion path above and would memmove — which
     * hides the value from store-load forwarding.  Copy each storage unit at
     * its natural access width instead, so the copied bitfield word forwards
     * into the later read and the insert/extract fold collapses the copy.
     * Restricted to local<->local and mixed global<->local copies with a
     * word-aligned base offset (so each chunk lands naturally aligned), and to
     * structs where every member access fits a single <=4-byte unit
     * (struct_member_copy_safe — a correctness guard against partial-forwarding
     * a wider read; pure 64-bit-unit/straddling-bitfield structs stay memmove). */
    if (tcc_state->ir && !has_vla && !NOEVAL_WANTED && size > 0 && size <= 16 &&
        (align & 3) && struct_member_copy_safe(&saved_struct_type) &&
        (is_local_copy || is_mixed_copy) &&
        !((vtop[0].c.i | vtop[-1].c.i) & 3))
    {
      SValue src = vtop[0];
      SValue dst = vtop[-1];
      vtop--; /* pop src; vtop = dst (kept as result lvalue) */

      if ((src.r & VT_VALMASK) == VT_LOCAL && (dst.r & VT_VALMASK) == VT_LOCAL &&
          src.c.i == dst.c.i)
      {
        vtop->type = saved_struct_type;
        goto vstore_done;
      }

      ir_emit_struct_unit_copy(&src, &dst, &saved_struct_type, size);
      vtop->type = saved_struct_type;
      goto vstore_done;
    }
#undef IS_REG_DEREF_LVAL
#undef IS_LOCAL_LVAL
#undef IS_GLOBAL_LVAL

    /* destination, keep on stack() as result */
    vpushv(vtop - 1);
#ifdef CONFIG_TCC_BCHECK
    if (vtop->r & VT_MUSTBOUND)
      gbound(); /* check would be wrong after gaddrof() */
#endif
    if (has_vla && (vtop->r & VT_VALMASK) == VT_LOCAL)
    {
      /* VLA struct stored via pointer indirection: the stack slot
         contains a pointer to the actual data.  We load that pointer
         instead of computing its address.
         Works whether VT_LVAL is already set (normal variable reference)
         or not (e.g. from declaration context). */
      vtop->type.t = VT_PTR;
      vtop->r |= VT_LVAL;
    }
    else
    {
      vtop->type.t = VT_PTR;
      gaddrof();
    }
    /* source */
    vswap();
#ifdef CONFIG_TCC_BCHECK
    if (vtop->r & VT_MUSTBOUND)
      gbound();
#endif
    if (has_vla && (vtop->r & VT_VALMASK) == VT_LOCAL)
    {
      vtop->type.t = VT_PTR;
      vtop->r |= VT_LVAL;
    }
    else
    {
      vtop->type.t = VT_PTR;
      gaddrof();
    }

#ifdef TCC_TARGET_NATIVE_STRUCT_COPY
    if (1 && !has_vla
#ifdef CONFIG_TCC_BCHECK
        && !tcc_state->do_bounds_check
#endif
    )
    {
      gen_struct_copy(size);
    }
    else
#endif
    {
      /* type size */
      if (has_vla)
        vpush_type_size(&saved_struct_type, &align);
      else
        vpushi(size);
      /* Use memmove, rather than memcpy, as dest and src may be same: */
#ifdef TCC_ARM_EABI
      if (!(align & 7))
        vpush_helper_func(TOK_memmove8);
      else if (!(align & 3))
        vpush_helper_func(TOK_memmove4);
      else
#endif
        vpush_helper_func(TOK_memmove);
      {
        /* Stack is now: dest_lval, dest_ptr, src_ptr, size, func
         * IR uses 0-based parameter indices. */
        SValue param_num;
        const int call_id = tcc_state->ir ? tcc_state->ir->next_call_id++ : 0;
        svalue_init(&param_num);
        param_num.vr = -1;

        param_num.r = VT_CONST;
        /* memmove(dest, src, size) */
        param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 0);
        LOG_CODEGEN("FUNCPARAMVAL push: site=memmove call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d", call_id,
                    TCCIR_DECODE_PARAM_IDX((uint32_t)param_num.c.i), vtop[-3].r, vtop[-3].vr);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[-3], &param_num, NULL);
        param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 1);
        LOG_CODEGEN("FUNCPARAMVAL push: site=memmove call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d", call_id,
                    TCCIR_DECODE_PARAM_IDX((uint32_t)param_num.c.i), vtop[-2].r, vtop[-2].vr);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[-2], &param_num, NULL);
        param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 2);
        LOG_CODEGEN("FUNCPARAMVAL push: site=memmove call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d", call_id,
                    TCCIR_DECODE_PARAM_IDX((uint32_t)param_num.c.i), vtop[-1].r, vtop[-1].vr);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[-1], &param_num, NULL);

        SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, 3);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVOID, &vtop[0], &call_id_sv, NULL);
        /* Pop func + 3 args; keep the saved destination lvalue as result */
        vtop -= 4;
      }
    }
  vstore_done:
    if (vstore_src_cid)
    {
      int dst_addr = (int)orig_dst.c.i;
      Sym *dst_sym = NULL;
      for (Sym *s = local_stack; s; s = s->prev)
      {
        if ((int)s->c == dst_addr && s->const_init_size >= size)
        {
          dst_sym = s;
          break;
        }
      }
      if (dst_sym)
      {
        if (!dst_sym->const_init_data)
          dst_sym->const_init_data = tcc_malloc(vstore_src_cid_size);
        memcpy(dst_sym->const_init_data, vstore_src_cid, vstore_src_cid_size);
        dst_sym->const_init_size = vstore_src_cid_size;
        dst_sym->const_init_valid = 1;
      }
      else
      {
        attach_const_init_to_temp(dst_addr, vstore_src_cid_size, vstore_src_cid);
      }
      /* vstore_src_cid points at the stack buffer above — no free needed. */
    }
    ;
  }
  else if (ft & VT_BITFIELD)
  {
    /* bitfield store handling */

    /* save lvalue as expression result (example: s.b = s.a = n;) */
    vdup(), vtop[-1] = vtop[-2];

    bit_pos = BIT_POS(ft);
    bit_size = BIT_SIZE(ft);
    /* remove bit field info to avoid loops */
    vtop[-1].type.t = ft & ~VT_STRUCT_MASK;

    if (dbt == VT_BOOL)
    {
      gen_cast(&vtop[-1].type);
      vtop[-1].type.t = (vtop[-1].type.t & ~VT_BTYPE) | (VT_BYTE | VT_UNSIGNED);
    }
    r = adjust_bf(vtop - 1, bit_pos, bit_size);
    if (dbt != VT_BOOL)
    {
      gen_cast(&vtop[-1].type);
      dbt = vtop[-1].type.t & VT_BTYPE;
    }
    if (r == VT_STRUCT)
    {
      store_packed_bf(bit_pos, bit_size);
    }
    else
    {
      unsigned long long mask = (1ULL << bit_size) - 1;
      if (dbt != VT_BOOL)
      {
        /* mask source */
        if (dbt == VT_LLONG)
          vpushll(mask);
        else
          vpushi((unsigned)mask);
        gen_op('&');
      }
      /* shift source */
      vpushi(bit_pos);
      gen_op(TOK_SHL);
      vswap();
      /* duplicate destination */
      vdup();
      vrott(3);
      /* load destination, mask and or with source */
      if (dbt == VT_LLONG)
        vpushll(~(mask << bit_pos));
      else
        vpushi(~((unsigned)mask << bit_pos));
      gen_op('&');
      gen_op('|');
      /* store result */
      vstore();
      /* ... and discard */
      vpop();
    }
  }
  else if (dbt == VT_VOID)
  {
    --vtop;
    print_vstack("vstore: void");
  }
  else
  {
    /* If the source is a bitfield lvalue in IR mode, extract the bitfield
       value (SHL/SAR shifts) now — before the delayed-cast or gen_cast paths
       overwrite vtop->type with the destination type, which loses VT_BITFIELD
       and the bit position/size information needed for the extraction. */
    if (tcc_state->ir && (vtop->type.t & VT_BITFIELD))
    {
      gv(RC_INT);
      /* After extraction, vtop is a plain int value; recompute sbt. */
      sbt = vtop->type.t & VT_BTYPE;
    }

    /* optimize char/short casts */
    delayed_cast = 0;
    if ((dbt == VT_BYTE || dbt == VT_SHORT) && is_integer_btype(sbt))
    {
      if ((vtop->r & (VT_MUSTCAST | (VT_MUSTCAST << 1))) && btype_size(dbt) > btype_size(sbt))
        force_charshort_cast();
      delayed_cast = 1;
    }
    else
    {
      gen_cast(&vtop[-1].type);
    }

    // gv(RC_TYPE(dbt)); /* generate value */

    if (delayed_cast)
    {
      vtop->r |= BFVAL(VT_MUSTCAST, (sbt == VT_LLONG) + 1);
      // tcc_warning("deley cast %x -> %x", sbt, dbt);
      vtop->type.t = ft & VT_TYPE;
    }

    /* if lvalue was saved on stack, must read it */
    if ((vtop[-1].r & VT_VALMASK) == VT_LLOCAL)
    {
      if (tcc_state->ir)
      {
        /* IR mode: load the saved pointer value into a vreg, and keep the
         * destination as a dereferenced address (***DEREF***).
         */
        SValue ptr_location;
        memset(&ptr_location, 0, sizeof(ptr_location));
        ptr_location.type.t = VT_PTRDIFF_T;
        ptr_location.r = VT_LOCAL | VT_LVAL;
        ptr_location.c.i = vtop[-1].c.i;

        SValue loaded_ptr;
        memset(&loaded_ptr, 0, sizeof(loaded_ptr));
        loaded_ptr.type.t = VT_PTRDIFF_T;
        loaded_ptr.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, &ptr_location, NULL, &loaded_ptr);

        vtop[-1].r &= ~VT_VALMASK;
        vtop[-1].r |= VT_LVAL;
        vtop[-1].vr = loaded_ptr.vr;
        vtop[-1].c.i = 0;
        vtop[-1].sym = NULL;
      }
      else
      {
        if (!nocode_wanted)
          tcc_error("IR-only: VT_LLOCAL reload requires IR");
      }
    }

    r = vtop->r & VT_VALMASK;
    /* two word case handling :
       store second register at word + 4 (or +8 for x86-64)  */
    /* On 32-bit systems, doubles are 64-bit and need two-word handling like long long */
    int is_64bit_type = (PTR_SIZE == 4 && (dbt == VT_DOUBLE || dbt == VT_LDOUBLE || dbt == VT_LLONG)) ||
                        (PTR_SIZE == 8 && dbt == VT_LLONG);
    if (is_64bit_type)
    {
      /* IR generation: handle long long as a single 64-bit value, and always
       * emit IR STORE/ASSIGN instead of calling the backend store() twice.
       *
       * Calling backend store() here is unsafe in IR mode because register
       * allocation/spilling can turn the low bits (VT_VALMASK) into VT_LOCAL
       * (0x32), which is not a physical register.
       */
      if (tcc_state->ir)
      {
        int op = TCCIR_OP_STORE;

        /* Keep the original destination type for a 64-bit store. */
        vtop[-1].type.t = dbt;

        /* Match the single-word behavior: local vreg destinations use ASSIGN. */
        if ((vtop[-1].r & VT_VALMASK) == VT_LOCAL && vtop[-1].vr != -1)
          op = TCCIR_OP_ASSIGN;

        /* If source is an lvalue (memory reference), emit LOAD first to get
         * the value, so STORE doesn't try to store memory-to-memory.
         */
        if (vtop->r & VT_LVAL)
        {
          SValue load_dest;
          load_dest.type = vtop->type;
          load_dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
          load_dest.r = 0;
          load_dest.c.i = 0;
          tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, vtop, NULL, &load_dest);
          vtop->vr = load_dest.vr;
          vtop->r = 0;
        }

        tcc_ir_codegen_cmp_jmp_set(tcc_state->ir);
        tcc_ir_put(tcc_state->ir, op, vtop, NULL, &vtop[-1]);

        if (op == TCCIR_OP_ASSIGN)
        {
          /* Assignment expression evaluates to the assigned value. For VT_LOCAL
           * destinations with vregs, return the destination vreg (now updated)
           * so later uses see the correct value.
           *
           * Preserve VT_LOCAL | VT_LVAL for stack-resident destinations so that
           * subsequent dereferences (e.g. *++ptr) properly load the pointer
           * value from the stack slot before dereferencing it.  Without this,
           * r=0 makes the result look like a register rvalue and indir() skips
           * the necessary LOAD, generating e.g. ldrb [stack_addr] instead of
           * ldr tmp,[stack_addr]; ldrb result,[tmp].
           */
          vtop->vr = vtop[-1].vr;
          vtop->r = 0;
        }
      }
    }
    else
    {
      /* single word */
      // store(r, vtop - 1);
      int op = TCCIR_OP_STORE;
      /* Use ASSIGN only for VT_LOCAL destinations that have a valid vreg.
       * Array elements initialized via init_putv have vr=-1 and need STORE. */
      if ((vtop[-1].r & VT_VALMASK) == VT_LOCAL && vtop[-1].vr != -1)
      {
        op = TCCIR_OP_ASSIGN;
      }
      /* If source is an lvalue (memory reference), emit LOAD first to get the value.
       * This is required for correctness when both source and destination live
       * in memory (e.g. range initializer replication copies element[lo] into
       * element[lo+1..hi]).
       *
       * Previously we skipped VT_LOCAL lvalues, assuming the backend would
       * handle it implicitly; that loses the load and can store garbage/zero. */
      if (vtop->r & VT_LVAL)
      {
        /* Save the delayed char/short cast bits before clearing r.
         * BFVAL(VT_MUSTCAST, 2) uses bit 0x0200 (for long long source)
         * in addition to 0x0100 (for int source), so preserve both. */
        int saved_mustcast = vtop->r & (VT_MUSTCAST | (VT_MUSTCAST << 1));

        /* When delayed_cast is active, vtop->type was already changed to
         * the destination type (e.g. unsigned short) while the actual
         * memory being loaded is still the original source type (e.g.
         * unsigned char).  The LOAD source operand must carry the original
         * type so the backend selects the correct load width (LDRB vs
         * LDRH vs LDR).  Temporarily restore the original source type for
         * the LOAD instruction, then switch back. */
        CType saved_type;
        int restore_type = 0;
        if (delayed_cast && (sbt & VT_BTYPE) != (vtop->type.t & VT_BTYPE))
        {
          saved_type = vtop->type;
          vtop->type.t = (vtop->type.t & ~(VT_BTYPE | VT_UNSIGNED)) | (sbt & (VT_BTYPE | VT_UNSIGNED));
          restore_type = 1;
        }

        SValue load_dest;
        load_dest.type = vtop->type;
        load_dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
        load_dest.r = 0;
        load_dest.c.i = 0;
        tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, vtop, NULL, &load_dest);

        if (restore_type)
          vtop->type = saved_type;

        vtop->vr = load_dest.vr;
        vtop->r = saved_mustcast; /* no longer an lvalue; keep delayed char/short cast */
      }
      /* If source is a VT_CMP (comparison result stored in flags), we need to
       * materialize it as a 0/1 value before storing. */
      tcc_ir_codegen_cmp_jmp_set(tcc_state->ir);
      /* In IR mode, ASSIGN is vreg-to-vreg with no implicit truncation
       * (unlike STORE which uses strb/strh).  If a delayed char/short cast
       * is pending (VT_MUSTCAST), resolve it now — after comparison results
       * have been materialized — so the vreg carries the correctly
       * wrapped value (e.g. unsigned char 0x18+0xe8 → 0x00, not 0x100).
       * Note: MUSTCAST=2 (from long long) stores in the bit above VT_MUSTCAST,
       * so check both bits. */
      if (op == TCCIR_OP_ASSIGN && (vtop->r & (VT_MUSTCAST | (VT_MUSTCAST << 1))))
        force_charshort_cast();
      tcc_ir_put(tcc_state->ir, op, vtop, NULL, &vtop[-1]);
      if (op == TCCIR_OP_ASSIGN)
      {
        /* See comment above in the two-word case. */
        vtop->vr = vtop[-1].vr;
        vtop->r = 0;
      }

      update_local_scalar_max_bound(&orig_dst, &orig_src);
    }
    vswap();
    vtop--; /* NOT vpop() because on x86 it would flush the fp stack */
    print_vstack("vstore: store");
  }
}
