/*
 *  TCC IR - Dead-Store Elimination for LEA-Deref STOREs
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* dead_local_slot_elim only NOPs STOREs whose dest is a direct
 * `StackLoc[X]` operand.  After known_bits collapses bitfield extract chains,
 * the remaining STOREs to the bitfield container are in the temp-deref form:
 *
 *     T0 <-- Addr[StackLoc[-4]]
 *     T0***DEREF*** <-- T2 [STORE]
 *
 * This pass eliminates those STOREs when no later instruction reads the same
 * slot (via direct StackLoc[Y] lval, via T'_DEREF where T' also points at Y,
 * or via a memcpy/memset/memmove read PARAM).
 *
 * Conservative bails:
 *   - Function contains IJUMP, SETJMP, LONGJMP, INLINE_ASM, VLA_ALLOC, or a
 *     nested-function frame pointer.
 *   - Address of slot escapes to any context other than: STORE dest, CMP src,
 *     ASSIGN/LEA/ADD/SUB propagation, memcpy/memset/memmove PARAM with known
 *     constant size.
 *   - Function contains any CALL whose target is not one of the recognized
 *     mem* helpers (a generic call may dereference an escaped address).
 *
 * Why these bails: this pass approximates per-slot escape analysis; the
 * tameness reasoning in `dead_local_slot_elim` is the same idea but built for
 * the direct-stack-ref form only.  Rather than tunnel its temp-deref handling
 * through that 1500-line pass, this one bails wide so it remains obviously
 * sound, and runs only when known_bits has already done the bulk of the
 * chain collapse.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_alias.h"
#include "opt_engine.h"
#include "opt_utils.h"

typedef struct
{
  int has_off;       /* 1 = this temp is a single-def LEA Addr[StackLoc[off]] */
  int32_t off;
  int def_count;     /* counts ALL defs (cap 2 — single-def required) */
} TmpAddr;

static int is_recognized_mem_call(TCCIRState *ir, IRQuadCompact *q,
                                  int *out_size_at_idx)
{
  Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
  if (!callee)
    return 0;
  const char *name = get_tok_str(callee->v, NULL);
  if (!name)
    return 0;
  if (strcmp(name, "memset") == 0 || strcmp(name, "__aeabi_memset") == 0)
  {
    *out_size_at_idx = 2;
    return 1;
  }
  if (strcmp(name, "memcpy") == 0 || strcmp(name, "memmove") == 0 ||
      strcmp(name, "__aeabi_memcpy") == 0 || strcmp(name, "__aeabi_memmove") == 0 ||
      strcmp(name, "__aeabi_memcpy4") == 0 || strcmp(name, "__aeabi_memcpy8") == 0 ||
      strcmp(name, "__aeabi_memmove4") == 0 || strcmp(name, "__aeabi_memmove8") == 0)
  {
    *out_size_at_idx = 2;
    return 1;
  }
  return 0;
}

int tcc_ir_opt_dead_lea_store_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* Nested function: a parent-frame StackLoc could be read via the static
   * chain — give up. */
  if (ir->captured_count > 0 || ir->has_static_chain)
    return 0;

  /* Bail on opcodes whose memory effects / control flow we don't model.
   * SWITCH_TABLE has indirect targets we can't range-check for back-edges. */
  for (int i = 0; i < n; i++)
  {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SETJMP || op == TCCIR_OP_LONGJMP ||
        op == TCCIR_OP_INLINE_ASM || op == TCCIR_OP_VLA_ALLOC ||
        op == TCCIR_OP_SET_CHAIN || op == TCCIR_OP_INIT_CHAIN_SLOT ||
        op == TCCIR_OP_SWITCH_TABLE)
      return 0;
  }

  /* Bail on calls to anything but recognized mem* helpers — a generic call
   * may dereference any escaped address. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVOID && q->op != TCCIR_OP_FUNCCALLVAL)
      continue;
    int sz_idx;
    if (!is_recognized_mem_call(ir, q, &sz_idx))
      return 0;
  }

  /* Find max temp position. */
  int max_tmp = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(dest);
    if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos > max_tmp)
      max_tmp = pos;
  }
  if (max_tmp == 0)
    return 0;

  TmpAddr *tmp_addr = tcc_mallocz(sizeof(TmpAddr) * (max_tmp + 1));

  /* Collect loop back-edges (a JUMP/JUMPIF to an earlier-or-equal position).
   * Pass 3's liveness is position-based (`read.pos > store.pos`), which is only
   * sound in straight-line code: inside a loop, a store whose slot is read
   * elsewhere in the same loop body is loop-carried-live even when every read
   * is at an EARLIER position (it re-executes next iteration via the back-edge).
   * Without this, the `c.v--` write-back in `while (c.v-- > 0)` was wrongly
   * dropped (miscompile).  Freed at `done` with the other buffers. */
  int be_cap = 8, be_n = 0;
  struct DlsBackEdge { int t, b; } *backedges = tcc_malloc(sizeof(struct DlsBackEdge) * be_cap);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    int tg = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
    if (tg >= 0 && tg <= i)
    {
      if (be_n >= be_cap)
      {
        be_cap *= 2;
        backedges = tcc_realloc(backedges, sizeof(struct DlsBackEdge) * be_cap);
      }
      backedges[be_n].t = tg;
      backedges[be_n].b = i;
      be_n++;
    }
  }

  /* Pass 1: identify single-def TEMPs holding Addr[StackLoc[off]].
   * STOREs and other lval-dest ops use the dest as the memory address —
   * they don't redefine the temp's value, so they don't count as defs. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (dest.is_lval)
      continue; /* address-of use, not a temp def */
    int32_t vr = irop_get_vreg(dest);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos > max_tmp)
      continue;
    tmp_addr[pos].def_count++;
    if (tmp_addr[pos].def_count > 1)
    {
      tmp_addr[pos].has_off = 0;
      continue;
    }
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LEA)
      continue;
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    if (irop_get_tag(s1) != IROP_TAG_STACKOFF || !s1.is_local || s1.is_lval ||
        irop_get_vreg(s1) != -1)
      continue;
    tmp_addr[pos].has_off = 1;
    tmp_addr[pos].off = irop_get_stack_offset(s1);
  }

  /* Helper closure: resolve an lval operand to a stack-slot offset, either
   * direct StackLoc[X] or via a TEMP that holds Addr[StackLoc[X]]. */
  int slot_off = 0;
#define RESOLVE_LVAL_SLOT(_op)                                              \
  ({                                                                       \
    int _ok = 0;                                                           \
    if ((_op).is_lval)                                                     \
    {                                                                      \
      if (irop_get_tag(_op) == IROP_TAG_STACKOFF && (_op).is_local &&      \
          irop_get_vreg(_op) == -1)                                        \
      {                                                                    \
        slot_off = irop_get_stack_offset(_op);                             \
        _ok = 1;                                                           \
      }                                                                    \
      else                                                                 \
      {                                                                    \
        int32_t _vr = irop_get_vreg(_op);                                  \
        if (_vr >= 0 &&                                                    \
            TCCIR_DECODE_VREG_TYPE(_vr) == TCCIR_VREG_TYPE_TEMP)           \
        {                                                                  \
          int _p = TCCIR_DECODE_VREG_POSITION(_vr);                        \
          if (_p <= max_tmp && tmp_addr[_p].has_off)                       \
          {                                                                \
            slot_off = tmp_addr[_p].off;                                   \
            _ok = 1;                                                       \
          }                                                                \
        }                                                                  \
      }                                                                    \
    }                                                                      \
    _ok;                                                                   \
  })

  /* Pass 2: per slot, collect set of "live" positions where the slot is
   * either read directly, read via a temp deref, or its address escapes to
   * something we can't bound (we bailed on most of those already).  A
   * memset/memcpy PARAM0 write doesn't count as a read; a memcpy PARAM1
   * with bounded size counts as a read AT the call instruction position. */

  /* We use a simple linear-collected list of (slot_off, pos) read events.
   * Functions handled by this pass are small (post-known_bits), so O(reads
   * * stores) is fine. */
  typedef struct
  {
    int32_t off;   /* inclusive start byte offset */
    int32_t width; /* number of bytes read */
    int pos;
  } ReadEvent;
  int reads_cap = 32;
  int reads_n = 0;
  ReadEvent *reads = tcc_malloc(sizeof(ReadEvent) * reads_cap);
#define ADD_READ(off_, width_, pos_)                                        \
  do                                                                        \
  {                                                                         \
    if (reads_n >= reads_cap)                                               \
    {                                                                       \
      reads_cap *= 2;                                                       \
      reads = tcc_realloc(reads, sizeof(ReadEvent) * reads_cap);            \
    }                                                                       \
    reads[reads_n].off = (off_);                                            \
    reads[reads_n].width = (width_);                                        \
    reads[reads_n].pos = (pos_);                                            \
    reads_n++;                                                              \
  } while (0)

  int bail = 0;
  for (int i = 0; i < n && !bail; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Handle PARAM operands for mem* calls separately so we can map
     * PARAM1 to a sized READ.  PARAM0 is a write-only destination here
     * (already known: we bailed on any non-mem* call). */
    if (q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID)
    {
      uint32_t enc = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
      int cid = TCCIR_DECODE_CALL_ID(enc);
      int pidx = TCCIR_DECODE_PARAM_IDX(enc);
      IROperand s1 = tcc_ir_op_get_src1(ir, q);

      /* PARAM0 of memset/memcpy/memmove: destination; not a read.
       * PARAM1 of memcpy/memmove: source; treat as a read. */
      if (pidx == 0)
        continue;
      if (pidx != 1)
      {
        /* size or unknown idx — operand is integer, not an address. */
        continue;
      }
      /* PARAM1: locate the matching call, look up the size param, mark
       * the read range. */
      int call_pos = -1;
      int read_size = 0;
      int size_ok = 0;
      for (int j = i + 1; j < n; j++)
      {
        IRQuadCompact *qj = &ir->compact_instructions[j];
        if (qj->op == TCCIR_OP_NOP)
          continue;
        if (qj->op == TCCIR_OP_FUNCPARAMVAL || qj->op == TCCIR_OP_FUNCPARAMVOID)
        {
          uint32_t encj = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, qj));
          if (TCCIR_DECODE_CALL_ID(encj) != cid)
            continue;
          if (TCCIR_DECODE_PARAM_IDX(encj) == 2)
          {
            IROperand sz_op = tcc_ir_op_get_src1(ir, qj);
            if (irop_get_tag(sz_op) == IROP_TAG_IMM32)
            {
              int sz = (int)irop_get_imm64_ex(ir, sz_op);
              if (sz > 0 && sz < (1 << 24))
              {
                read_size = sz;
                size_ok = 1;
              }
            }
          }
          continue;
        }
        if (qj->op != TCCIR_OP_FUNCCALLVOID && qj->op != TCCIR_OP_FUNCCALLVAL)
          continue;
        uint32_t cenc = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, qj));
        if (TCCIR_DECODE_CALL_ID(cenc) == cid)
        {
          call_pos = j;
          break;
        }
      }
      if (call_pos < 0 || !size_ok)
      {
        /* Can't bound the read precisely — bail conservatively. */
        bail = 1;
        continue;
      }
      /* Resolve source operand to a stack slot, if it's one. */
      int32_t off;
      int got_off = 0;
      if (s1.is_local && !s1.is_lval && irop_get_tag(s1) == IROP_TAG_STACKOFF &&
          irop_get_vreg(s1) == -1)
      {
        off = irop_get_stack_offset(s1);
        got_off = 1;
      }
      else
      {
        int32_t vr = irop_get_vreg(s1);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
        {
          int p = TCCIR_DECODE_VREG_POSITION(vr);
          if (p <= max_tmp && tmp_addr[p].has_off)
          {
            off = tmp_addr[p].off;
            got_off = 1;
          }
        }
      }
      if (got_off)
        ADD_READ(off, read_size, call_pos);
      else if (!s1.is_local && !s1.is_lval && irop_get_tag(s1) == IROP_TAG_SYMREF)
      {
        /* mem* source is a global/static symbol address — it reads that
         * object, never one of our stack slots, so it is not a read of any
         * tracked local.  (PARAM0, the destination, is what writes a local.)
         * This is the `local = global_struct;` init copy: without this the
         * pass bailed on the whole function and left a poke-store to a
         * since-dead local alive. */
      }
      else
      {
        /* PARAM1 to a mem* call from an unknown source: bail — could read
         * any of our slots. */
        bail = 1;
      }
      continue;
    }

    /* STORE: dest is a write to a slot; src1 (the value) is harmless. */
    if (q->op == TCCIR_OP_STORE)
    {
      /* We'll handle STOREs in the elimination pass below.  Their src1
       * (value) might be an address being stored *into* memory — which
       * would escape it.  If so, bail. */
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      if (s1.is_local && !s1.is_lval && irop_get_tag(s1) == IROP_TAG_STACKOFF)
      {
        /* Address-of-local being stored into memory: it's escaping. */
        bail = 1;
        continue;
      }
      int32_t s1_vr = irop_get_vreg(s1);
      if (s1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int p = TCCIR_DECODE_VREG_POSITION(s1_vr);
        if (p <= max_tmp && tmp_addr[p].has_off && !s1.is_lval)
        {
          /* A LEA-temp value (the address) is being stored — escape. */
          bail = 1;
          continue;
        }
      }
      continue;
    }

    /* Walk operands; record reads of known slots and bail on any non-tame
     * use of a known-address vreg.  k==3 is MLA's accumulator (4th operand):
     * `T <-- Ta MLA Tb + Tacc***DEREF***` reads the slot through Tacc, a use
     * src1/src2 never surface (struct_byval seed 11651). */
    for (int k = 0; k < 4; k++)
    {
      IROperand op;
      int has;
      if (k == 0) { has = irop_config[q->op].has_dest;
                    if (has) op = tcc_ir_op_get_dest(ir, q); }
      else if (k == 1) { has = irop_config[q->op].has_src1;
                         if (has) op = tcc_ir_op_get_src1(ir, q); }
      else if (k == 2) { has = irop_config[q->op].has_src2;
             if (has) op = tcc_ir_op_get_src2(ir, q); }
      else { has = (q->op == TCCIR_OP_MLA);
             if (has) op = tcc_ir_op_get_accum(ir, q); }
      if (!has)
        continue;
      /* Lval reference: it's a read of the slot.  We treat any lval-src use
       * as a read (a write via STORE was already handled above; a non-STORE
       * dest-lval is rare, and counting it as a read keeps us conservative). */
      if (op.is_lval && RESOLVE_LVAL_SLOT(op))
      {
        if (k != 0)
        {
          int w = ir_opt_store_btype_size_bytes(irop_get_btype(op));
          if (w <= 0)
            w = irop_is_64bit(op) ? 8 : 4;
          if (op.is_complex)
            w *= 2;
          ADD_READ(slot_off, w, i);
        }
        continue;
      }

      /* Helper: locate dest TEMP position so we can later check whether it
       * tracks the same slot offset (i.e., this is a tame propagation). */
      int dest_temp_pos = -1;
      if (irop_config[q->op].has_dest)
      {
        IROperand qd = tcc_ir_op_get_dest(ir, q);
        if (!qd.is_lval)
        {
          int32_t dvr = irop_get_vreg(qd);
          if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
          {
            dest_temp_pos = TCCIR_DECODE_VREG_POSITION(dvr);
            if (dest_temp_pos > max_tmp)
              dest_temp_pos = -1;
          }
        }
      }

      /* Dest role (k=0) is a definition, not a use — skip address-use
       * classification. (STOREs were already handled in the STORE branch.) */
      if (k == 0)
        continue;

      /* Non-lval address use (the address as a value).  Allowed shapes:
       *   ASSIGN/LEA dest=tracked-TEMP with same off (Pass 1 propagation).
       *   CMP either operand.
       *   mem* PARAM0 / PARAM1 (handled in the PARAM branch above).
       * Anything else lets the address escape into a context we can't
       * follow — bail. */
      int is_direct_addr =
          (irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_local && !op.is_lval &&
           irop_get_vreg(op) == -1);
      int addr_off = 0;
      int has_addr = 0;
      if (is_direct_addr)
      {
        addr_off = irop_get_stack_offset(op);
        has_addr = 1;
      }
      else
      {
        int32_t vr = irop_get_vreg(op);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP &&
            !op.is_lval)
        {
          int p = TCCIR_DECODE_VREG_POSITION(vr);
          if (p <= max_tmp && tmp_addr[p].has_off)
          {
            addr_off = tmp_addr[p].off;
            has_addr = 1;
          }
        }
      }
      if (!has_addr)
        continue;

      int tame_here = 0;
      switch (q->op)
      {
      case TCCIR_OP_ASSIGN:
      case TCCIR_OP_LEA:
        /* Propagating the address into another TEMP. Allow only if the
         * dest TEMP is tracked with the same slot offset, otherwise the
         * address escapes into an untracked vreg / VAR / PARAM and our
         * deref-side reads might miss it — bail. */
        if (dest_temp_pos >= 0 && tmp_addr[dest_temp_pos].has_off &&
            tmp_addr[dest_temp_pos].off == addr_off)
          tame_here = 1;
        break;
      case TCCIR_OP_CMP:
        tame_here = 1;
        break;
      default:
        break;
      }
      if (!tame_here)
        bail = 1;
    }
  }

  int changes = 0;
  if (bail)
    goto done;

  /* Pass 3: eliminate STOREs to a slot when no later read of that slot
   * exists. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!RESOLVE_LVAL_SLOT(dest))
      continue;
    int dest_w = ir_opt_store_btype_size_bytes(irop_get_btype(dest));
    if (dest_w <= 0)
      dest_w = irop_is_64bit(dest) ? 8 : 4;
    if (dest.is_complex)
      dest_w *= 2;
    int store_off = slot_off;

    /* Write-after-write: if a later store in the same straight-line run fully
     * overwrites this store's byte range with no read of those bytes in
     * between, S1's value is never observed — eliminate it even though the slot
     * is read further on (that read sees the overwriting store's value).
     * Restricting to a straight-line run (break at any control-flow op or jump
     * target) keeps the proof sound: the covering store unconditionally runs
     * after S1 before any branch could route to a read.  Intermediate stores
     * never *read* R1 (their value operands were escape-checked in Pass 2), so
     * they cannot keep S1 alive — only a recorded read can. */
    int waw_dead = 0;
    for (int j = i + 1; j < n; j++)
    {
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op == TCCIR_OP_NOP)
        continue;
      if (qj->is_jump_target)
        break; /* control-flow merge — straight-line run ends */
      if (qj->op == TCCIR_OP_JUMP || qj->op == TCCIR_OP_JUMPIF ||
          qj->op == TCCIR_OP_IJUMP || qj->op == TCCIR_OP_SWITCH_TABLE ||
          qj->op == TCCIR_OP_RETURNVALUE || qj->op == TCCIR_OP_RETURNVOID ||
          qj->op == TCCIR_OP_FUNCCALLVAL || qj->op == TCCIR_OP_FUNCCALLVOID)
        break; /* leaves the straight-line run */
      if (qj->op != TCCIR_OP_STORE)
        continue;
      IROperand d2 = tcc_ir_op_get_dest(ir, qj);
      if (!RESOLVE_LVAL_SLOT(d2))
        continue; /* writes a non-tracked location (no escapes survived Pass 2) */
      int off2 = slot_off;
      int w2 = ir_opt_store_btype_size_bytes(irop_get_btype(d2));
      if (w2 <= 0)
        w2 = irop_is_64bit(d2) ? 8 : 4;
      if (d2.is_complex)
        w2 *= 2;
      if (off2 <= store_off && store_off + dest_w <= off2 + w2)
      {
        /* Full cover: S1 is dead unless its bytes are read before j. */
        int read_between = 0;
        for (int r = 0; r < reads_n; r++)
          if (store_off < reads[r].off + reads[r].width &&
              reads[r].off < store_off + dest_w &&
              reads[r].pos > i && reads[r].pos < j)
          {
            read_between = 1;
            break;
          }
        if (!read_between)
          waw_dead = 1;
        break;
      }
      if (store_off < off2 + w2 && off2 < store_off + dest_w)
        break; /* partial overlap — cannot prove S1 fully dead */
      /* disjoint slot — keep scanning for a covering store */
    }
    if (waw_dead)
    {
      LOG_IR_GEN("DEAD LEA-STORE (WAW): nop STORE to StackLoc[%d] at i=%d w=%d",
                 store_off, i, dest_w);
      q->op = TCCIR_OP_NOP;
      changes++;
      continue;
    }

    int alive = 0;
    for (int r = 0; r < reads_n; r++)
    {
      /* Byte-range overlap: [reads[r].off, reads[r].off+width) vs
       * [store_off, store_off+dest_w).  Only stores whose bytes are
       * never read later may be eliminated. */
      if (!(store_off < reads[r].off + reads[r].width &&
            reads[r].off < store_off + dest_w))
        continue;
      if (reads[r].pos > i)
      {
        alive = 1; /* straight-line later read */
        break;
      }
      /* Loop-carried: a read at an earlier-or-equal position re-executes after
       * this store if both sit inside the body of the same back-edge loop. */
      for (int e = 0; e < be_n; e++)
        if (backedges[e].t <= reads[r].pos && reads[r].pos <= backedges[e].b &&
            backedges[e].t <= i && i <= backedges[e].b)
        {
          alive = 1;
          break;
        }
      if (alive)
        break;
    }
    if (alive)
      continue;
    LOG_IR_GEN("DEAD LEA-STORE: nop STORE to StackLoc[%d] at i=%d w=%d",
               store_off, i, dest_w);
    q->op = TCCIR_OP_NOP;
    changes++;
  }

done:
  tcc_free(backedges);
  tcc_free(reads);
  tcc_free(tmp_addr);
  return changes;
#undef RESOLVE_LVAL_SLOT
#undef ADD_READ
}

int tcc_ir_opt_dead_lea_store_elim_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_dead_lea_store_elim(ctx->ir);
}
