/*
 * TinyCC - Tiny C Compiler
 *
 * Scalar replacement of small frame objects.
 *
 * A struct local the frontend put in the frame stays there: every field read
 * is a load from the stack and every field write a store, and each call that
 * takes it by value copies it first.  The Zig C backend builds everything out
 * of such locals -- `Type`, `Value`, `InternPool.Index` are one-word structs
 * -- so most of the Zig compiler's stack traffic is to objects whose address
 * never leaves the function.
 *
 * An object qualifies when every reference to it is a LOAD or ASSIGN reading
 * an 8-, 16- or 32-bit field of it, a STORE writing one, or the whole of a
 * four-byte, float-free object passed to or returned from a call -- which the
 * AAPCS moves in a core register exactly like an int -- and the fields it is
 * accessed as do not overlap: each byte offset is always accessed with one
 * width, and a narrow one with one signedness.  A field may also be reached
 * through an address TEMP (`T <- &s; *(T + 4) = x`, the frontend's way to a
 * member) when that TEMP is used for nothing but such accesses.  Each field
 * then becomes a fresh VAR, which SSA renames like any other scalar (a narrow
 * one like a `short` local), and the address arithmetic goes.  A word-aligned
 * block copy to or from another pointer, and a struct-returning call's result
 * buffer, access it whole and become field copies (SraWhole).  Any other
 * reference -- an address that escapes, a 64-bit or overlapping access --
 * leaves the object in memory, untouched.
 *
 * The rule is kept narrow on purpose, and nothing is rewritten for an object
 * that stays in memory.  Accepting a field in any operand (an ALU op reading
 * it straight from the frame) promoted more objects but made the Zig compiler
 * larger, and so did folding `*(&obj + k)` into direct frame accesses for every
 * object: the extra promoted values mostly end up spilled, with the spill
 * slots nearest sp, and a direct access loses the short base-register form
 * and some later forwarding.
 *
 * Runs on the flat IR just before SSA construction.
 */

#include <stdint.h>

#include "ir.h"
#include "cfg.h"
#include "opt.h"
#include "opt_utils.h"

#define SRA_MAX_BYTES 36 /* 32 bytes, plus the guard word frame_record adds */

/* TCC_SRA_PTRVAR=0 keeps objects reached through pointer VARs in memory. */
TCC_DBG_ENV_INT(sra_ptrvar, "TCC_SRA_PTRVAR", 1)
/* TCC_SRA_PTRVAR_DF=0 follows pointer VARs within a block only. */
TCC_DBG_ENV_INT(sra_ptrvar_df, "TCC_SRA_PTRVAR_DF", 1)
/* TCC_SRA_ADDR_ALU=0 keeps objects a field of which is read inside address
 * arithmetic (`&obj.field + index`) in memory. */
TCC_DBG_ENV_INT(sra_addr_alu, "TCC_SRA_ADDR_ALU", 1)
/* TCC_SRA_PARAM_MAX: the largest struct argument split into words, in bytes. */
TCC_DBG_ENV_INT(sra_param_max, "TCC_SRA_PARAM_MAX", SRA_MAX_BYTES - 4)
/* TCC_SRA_NARROW_IDX_OFF=1 keeps objects with a narrow indexed store in memory. */
TCC_DBG_ENV_FLAG(sra_narrow_idx_off, "TCC_SRA_NARROW_IDX_OFF")
/* TCC_SRA_MIXED_OFF=1 keeps objects whose narrow fields are also read wider in memory. */
TCC_DBG_ENV_FLAG(sra_mixed_off, "TCC_SRA_MIXED_OFF")
/* TCC_SRA_COMBINE_OFF=1 keeps objects whose wide loads span several narrow fields in memory. */
TCC_DBG_ENV_FLAG(sra_combine_off, "TCC_SRA_COMBINE_OFF")
/* TCC_SRA_PARTIAL_OFF=1 keeps objects with a field updated or read in part in memory. */
TCC_DBG_ENV_FLAG(sra_partial_off, "TCC_SRA_PARTIAL_OFF")
/* TCC_SRA_PARTIAL_GATE: 0 always; 1 only objects a call copies whole; 2 (default) those, and objects whose
 * partial stores are all constants. */
TCC_DBG_ENV_INT(sra_partial_gate, "TCC_SRA_PARTIAL_GATE", 2)
/* TCC_SRA_BFI=0 inserts with AND/SHL/OR instead of BFI. */
TCC_DBG_ENV_INT(sra_bfi, "TCC_SRA_BFI", 1)
/* TCC_SRA_PAD_OFF=1 keeps a partial store's other bytes even when they are padding. */
TCC_DBG_ENV_FLAG(sra_pad_off, "TCC_SRA_PAD_OFF")
/* TCC_SRA_PAIR_OFF=1 keeps one object of a copy between two promotable ones in memory. */
TCC_DBG_ENV_FLAG(sra_pair_off, "TCC_SRA_PAIR_OFF")
/* TCC_SRA_PARTIAL_ONLY=<lo>,<lo>,...: only objects at these frame offsets may use partial fields
 * (bisecting).  Not a tuning knob. */
static int sra_partial_only(int lo)
{
  static const char *list;
  static int init;
  if (!init)
    init = 1, list = getenv("TCC_SRA_PARTIAL_ONLY");
  if (!list)
    return 1;
  char buf[16];
  int n = snprintf(buf, sizeof buf, "%d", lo);
  for (const char *p = list; (p = strstr(p, buf)) != NULL; p++)
    if ((p == list || p[-1] == ',') && (p[n] == 0 || p[n] == ','))
      return 1;
  return 0;
}
/* TCC_SRA_VAR_BUDGET: past this many field VARs in one function SSA promotes none of them
 * (ssa.c TCC_SSA_PROM_SRA_MAX), and a field updated in part or copied VAR to VAR costs more in memory
 * than the slot access it replaced; such objects then keep their memory. */
TCC_DBG_ENV_INT(sra_var_budget, "TCC_SRA_VAR_BUDGET", 2000)

typedef struct
{
  int lo, hi;  /* the object's extent, from tcc_ir_frame_object_at */
  int refs;    /* every operand a live instruction names it with */
  int ok_refs; /* those a field VAR can replace */
  int bad;     /* two accesses overlap, or one byte offset is read two ways */
  uint8_t width[SRA_MAX_BYTES]; /* per field start: bytes accessed, 0 = none */
  uint8_t sign[SRA_MAX_BYTES];  /* per narrow field: 1 signed, 2 unsigned */
  uint8_t has64[SRA_MAX_BYTES]; /* a 64-bit access starts here */
  uint8_t split[SRA_MAX_BYTES]; /* a 64-bit field kept as two word VARs */
  int32_t var[SRA_MAX_BYTES];
  int param_words; /* words of it a call receives by value (sra_struct_param_words) */
  int field_refs;  /* accesses of single fields */
  uint8_t whole_w; /* a call writes a range of it whole (SraWhole) */
  uint8_t whole_r; /* a call reads a range of it whole */
  const char *why; /* TCC_SRA_DBG: the first reason the unit keeps its memory */
  int why_op;
  int why_at; /* TCC_SRA_DBG: that reference's instruction, or -1 */
  uint8_t acc[SRA_MAX_BYTES]; /* per offset: widths loaded (bits 0-2), stored (bits 3-5) */
  uint8_t sgn[SRA_MAX_BYTES]; /* per offset: narrow accesses signed (1) / unsigned (2), bytes then halves */
  uint8_t nofit[SRA_MAX_BYTES];  /* per offset: widths of narrow stores whose value may not fit the width */
  uint8_t stvar[SRA_MAX_BYTES];  /* per offset: widths of narrow stores of a non-constant */
  uint8_t partial;               /* some access is a part of a wider field */
  uint8_t pmask[SRA_MAX_BYTES];  /* per field start: a narrow field stored in part -- its VAR's upper
                                  * bits are not zero, so a read of the whole field masks them */
  uint16_t nvst1[SRA_MAX_BYTES]; /* per offset: byte stores of a non-constant */
  uint16_t nvst2[SRA_MAX_BYTES]; /* per offset: halfword stores of a non-constant */
  int nloads;                    /* field loads */
  int npart;                     /* partial stores of a non-constant (each a read-modify-write) */
  uint8_t fstart[SRA_MAX_BYTES]; /* per byte: the start of the field holding it, 0xFF none (sra_index_fields) */
  uint64_t pad;                  /* bytes no member of its type covers (tcc_ir_frame_padding) */
} SraUnit;

/* TCC_SRA_DBG: one line per frame object SRA leaves in memory, with the first
 * reference that kept it there. */
TCC_DBG_ENV_FLAG(sra_dbg, "TCC_SRA_DBG")

static int sra_dbg_at = -1;
static void sra_why(SraUnit *u, const char *why, int op)
{
  if (!u->why)
    u->why = why, u->why_op = op, u->why_at = sra_dbg_at;
}

/* Why operand `o` in slot s of an instruction with opcode `op` is no field. */
static const char *sra_ref_kind(int op, int s, IROperand o)
{
  int bt = irop_get_btype(o);
  if (bt == IROP_BTYPE_STRUCT)
    return op == TCCIR_OP_FUNCPARAMVAL ? "struct-param"
           : op == TCCIR_OP_FUNCCALLVAL ? "struct-callret"
           : op == TCCIR_OP_RETURNVALUE ? "struct-return"
           : op == TCCIR_OP_BLOCK_COPY  ? "block-copy"
                                        : "struct-other";
  if (op == TCCIR_OP_LEA || !o.is_lval)
    return "address";
  if (irop_is_64bit(o))
    return op == TCCIR_OP_LOAD || op == TCCIR_OP_ASSIGN || op == TCCIR_OP_STORE ? "i64-ldst" : "i64-alu";
  if (op == TCCIR_OP_LOAD || op == TCCIR_OP_ASSIGN || op == TCCIR_OP_STORE)
    return s == 0 ? "ldst-dest?" : "ldst-src?";
  return "alu-operand";
}

/* A frame slot the backend addresses directly, the same test frame.c uses. */
static int sra_concrete(IROperand op, int *off)
{
  if (irop_is_none(op) || irop_get_vreg(op) >= 0 || op.is_param)
    return 0;
  int tag = irop_get_tag(op);
  if (tag != IROP_TAG_STACKOFF && !(tag == IROP_TAG_VREG && (op.is_local || op.is_llocal)))
    return 0;
  *off = irop_get_stack_offset(op);
  return 1;
}

static int sra_type_has_float(CType *t, int depth)
{
  if (depth > 8)
    return 1;
  int bt = t->t & VT_BTYPE;
  if (bt == VT_FLOAT || bt == VT_DOUBLE || bt == VT_LDOUBLE)
    return 1;
  if ((t->t & VT_ARRAY) && t->ref)
    return sra_type_has_float(&t->ref->type, depth + 1);
  if (bt == VT_STRUCT && t->ref)
    for (Sym *f = t->ref->next; f; f = f->next)
      if (sra_type_has_float(&f->type, depth + 1))
        return 1;
  return 0;
}

/* A four-byte struct operand that travels in a core register like an int. */
static int sra_struct_is_word(IROperand op)
{
  if (irop_get_btype(op) != IROP_BTYPE_STRUCT || op.is_llocal || op.is_complex)
    return 0;
  CType *ct = irop_get_ctype(op);
  if (!ct)
    return 0;
  int align;
  return type_size(ct, &align) == 4 && align <= 4 && !sra_type_has_float(ct, 0);
}

/* A struct passed BY VALUE whose words can come from field VARs: whole words,
 * 8 bytes up to TCC_SRA_PARAM_MAX, float-free, AAPCS alignment <= 4.  Its
 * words then occupy exactly the registers / stack words N consecutive word
 * arguments would for the base (soft/softfp) ABI.  Hard-float is deliberately
 * excluded: after a VFP argument spills, AAPCS C.6 keeps the next composite
 * whole on the stack, while replacing it with scalar words would make the
 * call-site ABI classifier lose that aggregate boundary.  The number of
 * words, or 0. */
static int sra_struct_param_words(IROperand op)
{
  if (irop_get_btype(op) != IROP_BTYPE_STRUCT || !op.is_lval || op.is_llocal || op.is_complex)
    return 0;
#if defined(TCC_TARGET_ARM_THUMB)
  if (tcc_state && tcc_state->float_abi == ARM_HARD_FLOAT)
    return 0;
#endif
  CType *ct = irop_get_ctype(op);
  if (!ct)
    return 0;
  int align, size = type_size(ct, &align);
  if (size < 8 || size > sra_param_max() || (size & 3) || irop_aapcs_alignment(op) > 4 || align > 4 ||
      sra_type_has_float(ct, 0))
    return 0;
  return size / 4;
}

static int sra_btype_bytes(int bt)
{
  return bt == IROP_BTYPE_INT32 ? 4 : bt == IROP_BTYPE_INT16 ? 2 : bt == IROP_BTYPE_INT8 ? 1 : 0;
}

/* Record a `bytes`-wide access at `off` of the unit, a store or a load; the
 * fields are laid out once every access has been seen (sra_resolve_fields). */
static void sra_note_field(SraUnit *u, int off, int bytes, int is_unsigned, int is_store)
{
  int at = off - u->lo;
  u->field_refs++;
  if (at < 0 || at + bytes > SRA_MAX_BYTES || at + bytes > u->hi - u->lo || (at & (bytes - 1)))
  {
    u->bad = 1;
    sra_why(u, at + bytes > SRA_MAX_BYTES ? "too-big" : "misaligned", 0);
    return;
  }
  u->acc[at] |= bytes << (is_store ? 3 : 0);
  if (bytes < 4)
    u->sgn[at] |= (is_unsigned ? 2 : 1) << (bytes == 1 ? 0 : 2);
  if (!is_store)
    u->nloads++;
}

/* The field starting at each accessed byte.  Accesses all of one width make a
 * field of it.  So do stores all of one width with loads of other widths:
 *  - narrower loads read the field's low bytes (sra_extract);
 *  - wider ones, of a narrow field -- a 2-byte struct returned as a word, an
 *    optional `{ payload; is_null; }` of bytes returned the same way -- read
 *    its VAR, ORed with those of the other narrow fields they cover shifted
 *    into place (sra_combine).  No whole-range call may touch the object:
 *    then the bytes no field covers are never written, so they hold nothing
 *    a read could depend on, and zero bits (the VARs hold their fields
 *    zero-extended) serve for them.
 * Any other mix -- stores at two widths at one offset (an error union whose
 * `u16 error` is written by itself and copied as a word), an access that
 * starts inside another's bytes, a result buffer read at two widths -- makes
 * the WIDEST access at the offset the field, and every access it does not
 * match a PARTIAL one of that field's VAR: a narrower load extracts its bytes
 * (a shift and a mask), a narrower store inserts them (mask, shift, OR --
 * one BFI once fused), each such store a fresh definition of the VAR.  The
 * bytes an access starts in must all be inside the field.
 * A signed or two-way-signed narrow field keeps the object in memory. */
/* The widths (bit 1 = byte, bit 2 = halfword) of a store at `b` into the word
 * field at `at` that leave only padding unwritten: sra_partial_store_padded
 * lowers them to a move, so they are no read-modify-write. */
static int sra_padded_widths(const SraUnit *u, int at, int w, int b)
{
  int ws = 0;
  if (w != 4 || at + 4 > 64)
    return 0;
  for (int sw = 1; sw <= 2; sw <<= 1)
  {
    int ok = b + sw <= at + 4;
    for (int x = at; ok && x < at + 4; x++)
      if ((x < b || x >= b + sw) && !((u->pad >> x) & 1))
        ok = 0;
    if (ok)
      ws |= sw;
  }
  return ws;
}

static void sra_resolve_fields(SraUnit *u, int whole)
{
  uint8_t inner[SRA_MAX_BYTES] = {0}; /* bytes inside a field, accessed in part */
  for (int at = 0; at < SRA_MAX_BYTES && !u->bad; at++)
  {
    int ld = u->acc[at] & 7, st = u->acc[at] >> 3, all = ld | st;
    if (!all || inner[at])
      continue;
    int W = all & 4 ? 4 : all & 2 ? 2 : 1;
    int w = W, combine = 0, partial = 0;
    if (all & (all - 1))
    {
      w = st;
      int ok = st && !(st & (st - 1)) && !sra_mixed_off();
      if (ok && (ld & ~(2 * st - 1)))
      {
        /* Wider loads: in the bytes they add, nothing but narrow fields of
         * their own, stored and loaded at one width (sra_combine). */
        int top = ld & 4 ? 4 : 2;
        ok = !(ld & (st - 1)) && !whole;
        for (int b = at + 1; ok && b < at + top; b++)
        {
          int fb = u->acc[b] >> 3;
          ok = !u->has64[b] && (!u->acc[b] || (fb && !(fb & (fb - 1)) && !(u->acc[b] & 7 & ~fb) &&
                                               b + fb <= at + top && !sra_combine_off()));
        }
        combine = ok;
      }
      else
        ok = 0;
      if (!combine)
      {
        w = W;
        partial = 1;
      }
    }
    if (!combine)
      for (int b = at + 1; b < at + w && b < SRA_MAX_BYTES; b++)
      {
        int a = u->acc[b], mw = a & 0x24 ? 4 : a & 0x12 ? 2 : a ? 1 : 0;
        if (u->has64[b] || b + mw > at + w)
        {
          u->bad = 1;
          sra_why(u, "overlap", 0);
          return;
        }
        if (a)
          inner[b] = 1, partial = 1;
      }
    if (partial)
    {
      u->partial = 1;
      if (w < 4)
      {
        int pst = st & (w - 1);
        for (int b = at + 1; b < at + w && b < SRA_MAX_BYTES; b++)
          pst |= u->acc[b] >> 3;
        u->pmask[at] = pst != 0;
      }
      for (int b = at; b < at + w && b < SRA_MAX_BYTES; b++)
      {
        const int pw = sra_pad_off() ? 0 : sra_padded_widths(u, at, w, b);
        u->npart += ((b == at && w == 1) || (pw & 1) ? 0 : u->nvst1[b]) + ((b == at && w == 2) || (pw & 2) ? 0 : u->nvst2[b]);
      }
      /* Worth it when a call copies the object whole (the copy's loads and
       * stores go), or when the partial stores are constants (they fold into
       * the field's value); a variable stored in part costs a BFI where a
       * narrow store cost the same, and its object's reads were already one
       * instruction each. */
      int stores_var = 0;
      for (int b = at; b < at + w && b < SRA_MAX_BYTES; b++)
        stores_var |= u->stvar[b] & (b == at ? (uint8_t)~w : 0xFF) &
                      (uint8_t)~(sra_pad_off() ? 0 : sra_padded_widths(u, at, w, b));
      if (sra_partial_off() || !sra_partial_only(u->lo) || (sra_partial_gate() == 1 && !whole) ||
          (sra_partial_gate() == 2 && !whole && stores_var))
      {
        u->bad = 1;
        sra_why(u, "mixed-width", 0);
        return;
      }
    }
    u->width[at] = w;
    if (w < 4)
    {
      int sg = (u->sgn[at] >> (w == 1 ? 0 : 2)) & 3;
      if (sg == 3)
      {
        u->bad = 1;
        sra_why(u, "narrow-sign-mix", 0);
        return;
      }
      u->sign[at] = sg;
      /* A narrow field's own-width store is its definition and must fit
       * (sra_store_as_def); a partial one is masked. */
      if (u->nofit[at] & w)
      {
        u->bad = 1;
        sra_why(u, "narrow-store", 0);
        return;
      }
    }
  }
}

/* A partial store of a variable is a read-modify-write of the field's VAR --
 * a load, a BFI and a store when the allocator keeps the VAR in memory, as it
 * does for a value defined in every arm of a large switch (Zig's
 * analyzeBodyInner writes its result's error code in 100+ arms and reads it
 * once) -- where the narrow store into the slot was one instruction.  Such an
 * object pays only when its loads (each a slot access saved) outnumber them. */
static void sra_partial_cost(SraUnit *u, int whole)
{
  if (!u->bad && u->partial && u->npart > u->nloads + 4 * whole)
  {
    u->bad = 1;
    sra_why(u, "partial-cost", 0);
  }
}

/* fstart: for every byte a field covers, the field's start. */
static void sra_index_fields(SraUnit *u)
{
  memset(u->fstart, 0xFF, sizeof u->fstart);
  for (int at = 0; at < SRA_MAX_BYTES; at++)
    for (int b = at; u->width[at] && b < at + u->width[at] && b < SRA_MAX_BYTES; b++)
      u->fstart[b] = (uint8_t)at;
}

/* A 64-bit access at `off`: an 8-byte aligned doubleword of the unit.  Whether
 * it becomes one 64-bit VAR or two word VARs is settled once every access has
 * been seen (sra_resolve_wide). */
static void sra_note_wide(SraUnit *u, int off)
{
  int at = off - u->lo;
  u->field_refs++;
  if (at < 0 || at + 8 > SRA_MAX_BYTES || at + 8 > u->hi - u->lo || (at & 7))
  {
    u->bad = 1;
    sra_why(u, at + 8 > SRA_MAX_BYTES ? "too-big" : "misaligned", 0);
    return;
  }
  u->has64[at] = 1;
}

/* Lay out the doublewords.  One touched only 64 bits at a time is a 64-bit
 * field.  One ALSO touched a word at a time -- the Zig C backend's zig_u128,
 * `{ uint64_t lo, hi; }`, is zero-filled and copied a word at a time and its
 * halves read and written whole -- becomes two word fields, and its 64-bit
 * accesses are rewritten to pack or split them (sra_rewrite_wide).  A narrow
 * access inside a doubleword keeps the object in memory. */
static void sra_resolve_wide(SraUnit *u)
{
  for (int at = 0; at + 8 <= SRA_MAX_BYTES; at += 8)
  {
    if (!u->has64[at])
      continue;
    for (int b = 1; b < 8; b++)
      if (b != 4 && u->width[at + b])
      {
        u->bad = 1;
        sra_why(u, "wide-overlap", 0);
        return;
      }
    int w0 = u->width[at], w1 = u->width[at + 4];
    if ((w0 && w0 != 4) || (w1 && w1 != 4))
    {
      u->bad = 1;
      sra_why(u, "wide-narrow", 0);
      return;
    }
    if (!w0 && !w1)
      u->width[at] = 8;
    else
    {
      u->width[at] = u->width[at + 4] = 4;
      u->split[at] = 1;
    }
  }
}

/* A narrow field's write has to be a DEFINITION of its VAR, and of the whole
 * of it.
 *
 * SSA only takes a full-width slot STORE as a definition of a variable (ssa.c,
 * ssa_store_slot_def_pos); a narrower one updates whatever name is current, in
 * place.  A field written only by narrow stores would therefore get a VAR that
 * is never defined at all, while its reads -- which rename turns into
 * whole-register copies -- take that undefined name (struct_byval fuzz seed
 * 1962: a `struct { unsigned char a; }` built and passed by value read back 3
 * bytes of whatever was under it at -O2).  The frontend's own narrow locals do
 * not have this problem: it writes those with ASSIGN.
 *
 * So a narrow field's STORE is rewritten to `ASSIGN var <- src`, which is one,
 * and every access to the field gets a 32-bit operand.  That drops the
 * truncation the store's width used to do -- the frontend does NOT truncate
 * ahead of a narrow store, `s.a = x` stores x whole and lets the width cut it
 * -- so the object is promoted only when every stored value ALREADY fits the
 * field.  There is nowhere to put a mask otherwise: a STORE's operand block
 * holds two operands and an AND needs three, and widening it in place would
 * write over the next instruction's.
 *
 * A SIGNED narrow field would need its reads sign-extended, which a plain
 * register copy does not do, so those keep their memory too. */
static int sra_narrow_fields_unsigned(const SraUnit *u)
{
  for (int at = 0; at < SRA_MAX_BYTES; at++)
    if (u->width[at] && u->width[at] < 4 && u->sign[at] != 2)
      return 0;
  return 1;
}

static int sra_temp_pos(IROperand o, int ntemp)
{
  int32_t vr = irop_get_vreg(o);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP || TCCIR_DECODE_VREG_POSITION(vr) >= ntemp)
    return -1;
  return TCCIR_DECODE_VREG_POSITION(vr);
}

/* The mask a `bytes`-wide unsigned field holds. */
static int32_t sra_field_mask(int bytes)
{
  return bytes == 1 ? 0xff : bytes == 2 ? 0xffff : -1;
}

/* Is every bit of `op` outside `mask` already zero, so that storing it whole
 * is what the narrow store would have stored?  `op` must also be a plain
 * value: SSA only makes a slot store a definition for one of those
 * (ssa_store_slot_def_pos).  Only the forms the frontend puts ahead of a
 * narrow store are recognised -- a constant, a mask by a subset of it, and a
 * value that is itself a narrow unsigned one.  `def` maps a TEMP to its only
 * defining instruction, or -1 when it has none or several. */
static int sra_narrow_store_fits(TCCIRState *ir, IRQuadCompact *q, int bytes,
                                 const int32_t *def, int ntemp)
{
  const int32_t mask = sra_field_mask(bytes);
  IROperand op = tcc_ir_op_get_src1(ir, q);
  if (irop_get_tag(op) == IROP_TAG_IMM32 && !op.is_lval)
    return (irop_get_imm32(op) & ~mask) == 0;
  if (irop_get_tag(op) != IROP_TAG_VREG || op.is_lval || op.is_llocal)
    return 0;
  int ob = sra_btype_bytes(irop_get_btype(op));
  if (ob && ob <= bytes && op.is_unsigned)
    return 1;
  int t = sra_temp_pos(op, ntemp);
  if (t < 0 || !def || def[t] < 0)
    return 0;
  IRQuadCompact *dq = &ir->compact_instructions[def[t]];
  if (dq->op == TCCIR_OP_AND)
  {
    IROperand a = tcc_ir_op_get_src1(ir, dq), b = tcc_ir_op_get_src2(ir, dq);
    return (irop_get_tag(b) == IROP_TAG_IMM32 && !b.is_lval && (irop_get_imm32(b) & ~mask) == 0) ||
           (irop_get_tag(a) == IROP_TAG_IMM32 && !a.is_lval && (irop_get_imm32(a) & ~mask) == 0);
  }
  if (dq->op == TCCIR_OP_LOAD || dq->op == TCCIR_OP_ASSIGN)
  {
    IROperand a = tcc_ir_op_get_src1(ir, dq);
    int ab = sra_btype_bytes(irop_get_btype(a));
    return ab && ab <= bytes && a.is_unsigned;
  }
  return 0;
}

/* A narrow field's STORE becomes an ASSIGN -- a definition of the VAR -- with
 * the same two operands and the VAR widened to 32 bits. */
static void sra_store_as_def(TCCIRState *ir, int i, IROperand rep)
{
  rep.is_lval = 0; /* a value definition of the VAR, not a write through it */
  ir->compact_instructions[i].op = TCCIR_OP_ASSIGN;
  tcc_ir_set_dest(ir, i, rep);
}

/* No two fields share a byte. */
static int sra_fields_disjoint(const SraUnit *u)
{
  int end = 0;
  for (int at = 0; at < SRA_MAX_BYTES; at++)
  {
    if (!u->width[at])
      continue;
    if (at < end)
      return 0;
    end = at + u->width[at];
  }
  return 1;
}

static int sra_find_unit(SraUnit *units, int nu, int off)
{
  int a = 0, b = nu - 1;
  while (a <= b)
  {
    int m = (a + b) / 2;
    if (off < units[m].lo)
      b = m - 1;
    else if (off >= units[m].hi)
      a = m + 1;
    else
      return m;
  }
  return -1;
}

/* Slot s (0 dest, 1 src1, 2 src2) of an instruction: the width in bytes of the
 * field a VAR can stand in for there, or 0.  A whole four-byte struct moved
 * like an int counts as its 32-bit field at offset 0. */
static int sra_slot_bytes(TCCIRState *ir, TccIrOp op, int s, IROperand o)
{
  if (!o.is_lval || o.is_llocal || o.is_complex || tcc_ir_access_is_volatile(ir, o))
    return 0;
  int bt = irop_get_btype(o);
  if (sra_btype_bytes(bt) &&
      ((s == 1 && (op == TCCIR_OP_LOAD || op == TCCIR_OP_ASSIGN)) || (s == 0 && op == TCCIR_OP_STORE)))
    return sra_btype_bytes(bt);
  if (bt == IROP_BTYPE_STRUCT && sra_struct_is_word(o) &&
      ((s == 1 && (op == TCCIR_OP_FUNCPARAMVAL || op == TCCIR_OP_RETURNVALUE)) ||
       (s == 0 && op == TCCIR_OP_FUNCCALLVAL)))
    return 4;
  return 0;
}

/* The ops whose src1 / src2 is a plain 64-bit value read, one a field VAR (or
 * a PACK64 of two) can stand in for. */
static int sra_wide_reader(int op)
{
  switch (op)
  {
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  case TCCIR_OP_MUL:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SAR:
  case TCCIR_OP_SHR:
  case TCCIR_OP_CMP:
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_LOAD:
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_STORE:
    return 1;
  default:
    return 0;
  }
}

/* A 64-bit integer access in slot s (0 dest, 1 src1, 2 src2) of q that a field
 * can take over: a value read by sra_wide_reader, or a STORE of a plain value
 * -- a TEMP or a constant, which the rewrite turns into a definition (or two)
 * of the field.  Doubles live in VFP registers and are left alone. */
static int sra_wide_access(TCCIRState *ir, IRQuadCompact *q, int s, IROperand o)
{
  if (!o.is_lval || o.is_llocal || o.is_complex || irop_get_btype(o) != IROP_BTYPE_INT64 ||
      tcc_ir_access_is_volatile(ir, o))
    return 0;
  if (s == 0)
  {
    if (q->op != TCCIR_OP_STORE)
      return 0;
    IROperand v = tcc_ir_op_get_src1(ir, q);
    int tag = irop_get_tag(v);
    if (v.is_lval || v.is_complex)
      return 0;
    return (tag == IROP_TAG_VREG && irop_get_vreg(v) >= 0) || tag == IROP_TAG_IMM32 || tag == IROP_TAG_I64;
  }
  return (s == 1 || s == 2) && sra_wide_reader(q->op);
}

/* The ops that keep a fourth operand at operand_base + 3 (frame.c's list). */
static int sra_op_has_slot3(int op)
{
  return ir_op_has(op, IROP_A_SLOT3) && !ir_opset_has(IR_LEGACY_GAP_OPS(TCCIR_OP_UMAAL), op);
}

/* Operand slot s (0-3) of q, or IROP_NONE. */
static IROperand sra_operand(TCCIRState *ir, IRQuadCompact *q, int s)
{
  if (s == 0 && irop_config[q->op].has_dest)
    return tcc_ir_op_get_dest(ir, q);
  if (s == 1 && irop_config[q->op].has_src1)
    return tcc_ir_op_get_src1(ir, q);
  if (s == 2 && irop_config[q->op].has_src2)
    return tcc_ir_op_get_src2(ir, q);
  if (s == 3 && sra_op_has_slot3(q->op) && q->operand_base + 3 < (uint32_t)ir->iroperand_pool_count)
    return ir->iroperand_pool[q->operand_base + 3];
  return IROP_NONE;
}

static int sra_function_eligible(TCCIRState *ir)
{
  if (tcc_state->do_debug || tcc_bounds_checking(tcc_state))
    return 0;
  if (ir->has_static_chain || ir->captured_count > 0 || tcc_state->nb_nested_funcs > 0)
    return 0;
  if (tcc_ir_calls_returns_twice(ir))
    return 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    switch (ir->compact_instructions[i].op)
    {
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
      return 0;
    default:
      break;
    }
  }
  return 1;
}

#define SRA_NO_ADDR INT32_MIN

/* The frame offset a by-value struct argument is read from: its slot, or,
 * beyond the 16-bit reach of a struct slot operand, `T***DEREF***` through an
 * address TEMP (toff) -- the form tcc_ir_put gives far struct sources. */
static int sra_param_at(IROperand o, int ntemp, const int32_t *toff, int *off)
{
  if (sra_concrete(o, off))
    return 1;
  int t;
  if (!o.is_lval || o.is_llocal || o.is_local || irop_get_tag(o) != IROP_TAG_VREG ||
      (t = sra_temp_pos(o, ntemp)) < 0 || toff[t] == SRA_NO_ADDR)
    return 0;
  *off = toff[t];
  return 1;
}

/* Instructions the rewrite of a split doubleword adds: a PACK64 ahead of a
 * reader of the 64-bit value, the high half of a split store after it.  They
 * are collected and spliced in by one pass over the function at the end. */
typedef struct
{
  int anchor; /* the instruction they belong to */
  int after;  /* 0: ahead of it -- a jump to it runs them; 1: after it */
  IRQuadCompact q;
} SraIns;

typedef struct
{
  SraIns *v;
  int n, cap;
} SraInsList;

/* A new instruction with only the operand slots `op` uses, as codegen reads
 * them (write_instr_at_nop's layout). */
static void sra_ins_add(TCCIRState *ir, SraInsList *l, int anchor, int after, TccIrOp op, IROperand dest,
                        IROperand src1, IROperand src2)
{
  if (l->n == l->cap)
  {
    l->cap = l->cap ? 2 * l->cap : 16;
    l->v = tcc_realloc(l->v, sizeof(SraIns) * l->cap);
  }
  SraIns *e = &l->v[l->n++];
  memset(e, 0, sizeof(*e));
  e->anchor = anchor;
  e->after = after;
  e->q.op = op;
  e->q.orig_index = ++ir->max_orig_index;
  e->q.line_num = ir->compact_instructions[anchor].line_num;
  e->q.operand_base = ir->iroperand_pool_count;
  if (irop_config[op].has_dest)
    tcc_ir_pool_add(ir, dest);
  if (irop_config[op].has_src1)
    tcc_ir_pool_add(ir, src1);
  if (irop_config[op].has_src2)
    tcc_ir_pool_add(ir, src2);
  if (sra_op_has_slot3(op))
    tcc_ir_pool_add(ir, irop_make_imm32(-1, 0, IROP_BTYPE_INT32)); /* an indexed access's scale */
}

/* Give q a fresh operand block for `op`: the old one may be too short. */
static void sra_relay(TCCIRState *ir, IRQuadCompact *q, TccIrOp op, IROperand dest, IROperand src1,
                      IROperand src2)
{
  q->op = op;
  q->operand_base = ir->iroperand_pool_count;
  if (irop_config[op].has_dest)
    tcc_ir_pool_add(ir, dest);
  if (irop_config[op].has_src1)
    tcc_ir_pool_add(ir, src1);
  if (irop_config[op].has_src2)
    tcc_ir_pool_add(ir, src2);
}

/* A `bytes`-wide load of a wider field's low bytes, from its 32-bit VAR `var`:
 * `dest <- var AND #mask`, or, sign-extending, `T <- var SHL #s` ahead of it
 * and `dest <- T SAR #s`.  The load at i becomes the last of them. */
static void sra_extract(TCCIRState *ir, SraInsList *ins, int i, IROperand var, int bytes, int is_unsigned)
{
  IRQuadCompact *q = &ir->compact_instructions[i];
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  if (is_unsigned)
  {
    sra_relay(ir, q, TCCIR_OP_AND, dest, var, irop_make_imm32(-1, sra_field_mask(bytes), IROP_BTYPE_INT32));
    return;
  }
  const int sh = 32 - 8 * bytes;
  IROperand t = irop_make_vreg(tcc_ir_vreg_alloc_temp(ir), IROP_BTYPE_INT32);
  sra_ins_add(ir, ins, i, 0, TCCIR_OP_SHL, t, var, irop_make_imm32(-1, sh, IROP_BTYPE_INT32));
  sra_relay(ir, &ir->compact_instructions[i], TCCIR_OP_SAR, dest, t, irop_make_imm32(-1, sh, IROP_BTYPE_INT32));
}

static IROperand sra_field_op(TCCIRState *ir, SraUnit *u, int at, int bt, int is_unsigned);
static IROperand sra_new_temp(TCCIRState *ir, int bt);

/* A `bytes`-wide load into `dest` of bytes [d, d + bytes) of the field at `fs`
 * of u, from its VAR: the bytes shifted down and masked (`dest <- V SHR #8d`
 * alone when they are the field's top bytes, `dest <- V AND #mask` alone when
 * its bottom ones), or, sign-extending, shifted up to the top and back down
 * arithmetically.  The load at i becomes the last instruction. */
static void sra_partial_load(TCCIRState *ir, SraInsList *ins, int i, SraUnit *u, int fs, int d, int bytes,
                             int is_unsigned, IROperand dest)
{
  IROperand v = sra_field_op(ir, u, fs, IROP_BTYPE_INT32, 1);
  IRQuadCompact *q = &ir->compact_instructions[i];
  const int W = u->width[fs];
  if (is_unsigned)
  {
    if (d == 0)
      sra_relay(ir, q, TCCIR_OP_AND, dest, v, irop_make_imm32(-1, sra_field_mask(bytes), IROP_BTYPE_INT32));
    else if (d + bytes == W && W == 4)
      sra_relay(ir, q, TCCIR_OP_SHR, dest, v, irop_make_imm32(-1, 8 * d, IROP_BTYPE_INT32));
    else
    {
      IROperand t = sra_new_temp(ir, IROP_BTYPE_INT32);
      sra_ins_add(ir, ins, i, 0, TCCIR_OP_SHR, t, v, irop_make_imm32(-1, 8 * d, IROP_BTYPE_INT32));
      sra_relay(ir, &ir->compact_instructions[i], TCCIR_OP_AND, dest, t,
                irop_make_imm32(-1, sra_field_mask(bytes), IROP_BTYPE_INT32));
    }
    return;
  }
  const int up = 32 - 8 * (d + bytes), down = 32 - 8 * bytes;
  IROperand src = v;
  if (up)
  {
    IROperand t = irop_make_vreg(tcc_ir_vreg_alloc_temp(ir), IROP_BTYPE_INT32);
    sra_ins_add(ir, ins, i, 0, TCCIR_OP_SHL, t, v, irop_make_imm32(-1, up, IROP_BTYPE_INT32));
    src = t;
  }
  sra_relay(ir, &ir->compact_instructions[i], TCCIR_OP_SAR, dest, src, irop_make_imm32(-1, down, IROP_BTYPE_INT32));
}

/* A store into bytes [d, d + bytes) of the word field at `fs` whose other
 * bytes are all padding: it may leave anything in them (C11 6.2.6.1p6), so
 * the field's VAR is just the value, shifted into place -- no read of the old
 * word and no insert.  A Zig error union's `u16 error` in the low half of a
 * word copied whole: `ldr; mov; bfi` per store became a move.  Word fields
 * only: a narrow field's VAR is read zero-extended (sra_combine). */
static int sra_partial_store_padded(TCCIRState *ir, int i, SraUnit *u, int fs, int d, int bytes, IROperand val)
{
  if (u->width[fs] != 4 || fs + 4 > 64)
    return 0;
  for (int b = fs; b < fs + 4; b++)
    if ((b < fs + d || b >= fs + d + bytes) && !((u->pad >> b) & 1))
      return 0;
  IROperand def = sra_field_op(ir, u, fs, IROP_BTYPE_INT32, 1);
  def.is_lval = 0;
  IROperand x;
  if (irop_is_plain_imm(val))
    x = irop_make_imm32(-1, (int32_t)(((uint32_t)irop_get_imm64_ex(ir, val) & (uint32_t)sra_field_mask(bytes)) << (8 * d)),
                        IROP_BTYPE_INT32);
  else
  {
    x = irop_retype_scalar(val, IROP_BTYPE_INT32);
    x.is_unsigned = 1;
  }
  IRQuadCompact *q = &ir->compact_instructions[i];
  if (d && !irop_is_plain_imm(val))
    sra_relay(ir, q, TCCIR_OP_SHL, def, x, irop_make_imm32(-1, 8 * d, IROP_BTYPE_INT32));
  else
    sra_relay(ir, q, TCCIR_OP_ASSIGN, def, x, IROP_NONE);
  return 1;
}

/* A `bytes`-wide store of `val` into bytes [d, d + bytes) of the field at `fs`
 * of u: `T <- V AND #~mask` ahead of i, and i becomes `V <- T OR x`, a fresh
 * definition of the VAR, where x is val masked to its width (unless it fits
 * already) and shifted into place; a constant is folded on the spot. */
static void sra_partial_store(TCCIRState *ir, SraInsList *ins, int i, SraUnit *u, int fs, int d, int bytes,
                              IROperand val, int fits)
{
  IROperand v = sra_field_op(ir, u, fs, IROP_BTYPE_INT32, 1);
  IROperand def = v;
  def.is_lval = 0;
  const int32_t mask = (int32_t)((uint32_t)sra_field_mask(bytes) << (8 * d));
  IROperand x;
  if (irop_is_plain_imm(val))
    x = irop_make_imm32(-1, (int32_t)(((uint32_t)irop_get_imm64_ex(ir, val) << (8 * d)) & (uint32_t)mask),
                        IROP_BTYPE_INT32);
  else
  {
    x = irop_retype_scalar(val, IROP_BTYPE_INT32);
    x.is_unsigned = 1;
    if (!fits)
    {
      IROperand t = sra_new_temp(ir, IROP_BTYPE_INT32);
      sra_ins_add(ir, ins, i, 0, TCCIR_OP_AND, t, x, irop_make_imm32(-1, sra_field_mask(bytes), IROP_BTYPE_INT32));
      x = t;
    }
    if (d)
    {
      IROperand t = sra_new_temp(ir, IROP_BTYPE_INT32);
      sra_ins_add(ir, ins, i, 0, TCCIR_OP_SHL, t, x, irop_make_imm32(-1, 8 * d, IROP_BTYPE_INT32));
      x = t;
    }
  }
  IROperand kept = sra_new_temp(ir, IROP_BTYPE_INT32);
  sra_ins_add(ir, ins, i, 0, TCCIR_OP_AND, kept, v, irop_make_imm32(-1, ~mask, IROP_BTYPE_INT32));
  sra_relay(ir, &ir->compact_instructions[i], TCCIR_OP_OR, def, kept, x);
}

/* The same as one BFI: `V <- BFI(V, val)` inserting val's low 8*bytes bits at
 * bit 8d (the machine op takes only those bits, so no mask is needed).  Its
 * lsb and width travel in ir->bfi_params by orig_index (bitfield.c). */
static void sra_partial_store_bfi(TCCIRState *ir, int i, SraUnit *u, int fs, int d, int bytes, IROperand val)
{
  IROperand v = sra_field_op(ir, u, fs, IROP_BTYPE_INT32, 1);
  IROperand def = v;
  def.is_lval = 0;
  IROperand x = irop_retype_scalar(val, IROP_BTYPE_INT32);
  x.is_unsigned = 1;
  IRQuadCompact *q = &ir->compact_instructions[i];
  sra_relay(ir, q, TCCIR_OP_BFI, def, v, x);
  const int need = ir->max_orig_index + 1;
  if (!ir->bfi_params || ir->bfi_params_len < need)
  {
    int len = need + 64;
    ir->bfi_params = tcc_realloc(ir->bfi_params, (size_t)len * sizeof(uint16_t));
    memset(ir->bfi_params + ir->bfi_params_len, 0, (size_t)(len - ir->bfi_params_len) * sizeof(uint16_t));
    ir->bfi_params_len = len;
  }
  ir->bfi_params[q->orig_index] = (uint16_t)(((8 * d) & 0xFF) | (((8 * bytes) & 0xFF) << 8));
}

static int sra_ins_cmp(const void *a, const void *b)
{
  const SraIns *x = a, *y = b;
  if (x->anchor != y->anchor)
    return x->anchor < y->anchor ? -1 : 1;
  if (x->after != y->after)
    return x->after - y->after;
  return x->q.orig_index < y->q.orig_index ? -1 : x->q.orig_index > y->q.orig_index;
}

/* Splice the collected instructions in.  A jump to an instruction lands on the
 * first one added ahead of it (a PACK64 its reader needs); one added after an
 * instruction only runs after it. */
static void sra_ins_apply(TCCIRState *ir, SraInsList *l)
{
  if (l->n == 0)
    return;
  tcc_qsort(l->v, l->n, sizeof(SraIns), sra_ins_cmp);
  const int n = ir->next_instruction_index, nn = n + l->n;
  IRQuadCompact *out = tcc_malloc(sizeof(IRQuadCompact) * (nn + 1));
  int32_t *lead = tcc_malloc(sizeof(int32_t) * (n + 1));
  int j = 0, e = 0;
  for (int i = 0; i < n; i++)
  {
    lead[i] = j;
    int flag = ir->compact_instructions[i].is_jump_target;
    int first = 1;
    for (; e < l->n && l->v[e].anchor == i && !l->v[e].after; e++, first = 0)
    {
      out[j] = l->v[e].q;
      out[j++].is_jump_target = first ? flag : 0;
    }
    out[j] = ir->compact_instructions[i];
    if (!first)
      out[j].is_jump_target = 0;
    j++;
    for (; e < l->n && l->v[e].anchor == i; e++)
      out[j++] = l->v[e].q;
  }
  lead[n] = j;
  for (int i = 0; i < nn; i++)
  {
    IRQuadCompact *q = &out[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int target = (int)tcc_ir_op_dest_imm(ir, q);
      if (target >= 0 && target <= n)
        tcc_ir_op_set_dest_imm32(ir, q, lead[target], IROP_BTYPE_INT32);
    }
  }
  for (int t = 0; t < ir->num_switch_tables; t++)
  {
    TCCIRSwitchTable *table = &ir->switch_tables[t];
    if (table->default_target >= 0 && table->default_target <= n)
      table->default_target = lead[table->default_target];
    for (int k = 0; table->targets && k < table->num_entries; k++)
      if (table->targets[k] >= 0 && table->targets[k] <= n)
        table->targets[k] = lead[table->targets[k]];
  }
  if (ir->compact_instructions_size < nn + 1)
  {
    ir->compact_instructions = tcc_realloc(ir->compact_instructions, sizeof(IRQuadCompact) * (nn + 1));
    ir->compact_instructions_size = nn + 1;
  }
  memcpy(ir->compact_instructions, out, sizeof(IRQuadCompact) * nn);
  ir->next_instruction_index = nn;
  tcc_ir_frame_scope_remap(ir, (const int *)lead, n, nn);
  tcc_free(lead);
  tcc_free(out);
}

/* The field VAR at byte `at` of unit u, as an operand of type `bt`. */
static IROperand sra_field_op(TCCIRState *ir, SraUnit *u, int at, int bt, int is_unsigned)
{
  if (u->var[at] < 0)
    u->var[at] = tcc_ir_vreg_alloc_var(ir);
  IRLiveInterval *iv = tcc_ir_get_live_interval(ir, u->var[at]);
  IROperand rep = irop_make_stackoff(u->var[at], iv ? iv->original_offset : 0, 1, 0, 0, bt);
  rep.is_unsigned = is_unsigned;
  rep.aux = IROP_AUX_NONVOLATILE;
  return rep;
}

/* Rewrite the 64-bit access `o` in slot s of instruction i to the doubleword
 * at byte `at` of u (sra_resolve_wide laid it out).
 *
 * A 64-bit field is a 64-bit VAR: a read names it, and a STORE becomes an
 * ASSIGN -- SSA takes only a 32-bit slot STORE for a definition
 * (ssa_store_slot_def_pos), exactly the narrow-field case.
 *
 * A split doubleword is two word VARs, lo at `at` and hi at `at + 4`:
 *  - a LOAD / ASSIGN of it becomes `dest <- lo PACK64 hi`;
 *  - any other reader gets `T <- lo PACK64 hi` ahead of it and reads T;
 *  - a STORE of a constant stores its two words, one of a value x becomes
 *    `lo <- x` (the ASSIGN truncates, as the frontend's own u64 -> u32 cast
 *    does) followed by `T <- x SHR #32; hi <- T`. */
static void sra_rewrite_wide(TCCIRState *ir, SraInsList *ins, int i, int s, IROperand o, SraUnit *u, int at)
{
  IRQuadCompact *q = &ir->compact_instructions[i];
  if (u->width[at] == 8)
  {
    IROperand rep = sra_field_op(ir, u, at, IROP_BTYPE_INT64, o.is_unsigned);
    if (s == 0)
      sra_store_as_def(ir, i, rep);
    else if (s == 1)
      tcc_ir_set_src1(ir, i, rep);
    else
      tcc_ir_set_src2(ir, i, rep);
    return;
  }
  IROperand lo = sra_field_op(ir, u, at, IROP_BTYPE_INT32, 1);
  IROperand hi = sra_field_op(ir, u, at + 4, IROP_BTYPE_INT32, 1);
  if (s == 0)
  {
    IROperand v = tcc_ir_op_get_src1(ir, q);
    int tag = irop_get_tag(v);
    if (tag == IROP_TAG_IMM32 || tag == IROP_TAG_I64)
    {
      int64_t c = irop_get_imm64_ex(ir, v);
      tcc_ir_set_dest(ir, i, lo);
      tcc_ir_set_src1_imm32(ir, i, (int32_t)(uint32_t)c, IROP_BTYPE_INT32);
      sra_ins_add(ir, ins, i, 1, TCCIR_OP_STORE, hi, irop_make_imm32(-1, (int32_t)(uint32_t)((uint64_t)c >> 32), IROP_BTYPE_INT32),
                  IROP_NONE);
      return;
    }
    lo.is_lval = 0;
    hi.is_lval = 0;
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_dest(ir, i, lo);
    int32_t t = tcc_ir_vreg_alloc_temp(ir);
    IROperand th = irop_make_vreg(t, IROP_BTYPE_INT64);
    th.is_unsigned = 1;
    IROperand x = v;
    x.is_unsigned = 1;
    sra_ins_add(ir, ins, i, 1, TCCIR_OP_SHR, th, x, irop_make_imm32(-1, 32, IROP_BTYPE_INT32));
    sra_ins_add(ir, ins, i, 1, TCCIR_OP_ASSIGN, hi, th, IROP_NONE);
    return;
  }
  IROperand d = irop_config[q->op].has_dest ? tcc_ir_op_get_dest(ir, q) : IROP_NONE;
  if (s == 1 && (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ASSIGN) && !d.is_lval &&
      irop_get_btype(d) == IROP_BTYPE_INT64)
  {
    sra_relay(ir, q, TCCIR_OP_PACK64, d, lo, hi);
    return;
  }
  int32_t t = tcc_ir_vreg_alloc_temp(ir);
  IROperand tp = irop_make_vreg(t, IROP_BTYPE_INT64);
  tp.is_unsigned = o.is_unsigned;
  sra_ins_add(ir, ins, i, 0, TCCIR_OP_PACK64, tp, lo, hi);
  if (q->op == TCCIR_OP_LOAD)
    q->op = TCCIR_OP_ASSIGN; /* reads the packed TEMP's value, not memory */
  if (s == 1)
    tcc_ir_set_src1(ir, i, tp);
  else
    tcc_ir_set_src2(ir, i, tp);
}

/* A LOAD or STORE through a TEMP base, plain or indexed by a constant: the
 * base's TEMP, with the displacement and the operand that carries the access's
 * width, sign and volatility (ssa:stack_deref_fold reads it the same way).
 * -1 for anything else. */
static int sra_access_through(TCCIRState *ir, IRQuadCompact *q, int ntemp, int *disp, IROperand *acc)
{
  int op = q->op;
  if (op != TCCIR_OP_STORE && op != TCCIR_OP_LOAD && op != TCCIR_OP_STORE_INDEXED && op != TCCIR_OP_LOAD_INDEXED)
    return -1;
  int is_store = op == TCCIR_OP_STORE || op == TCCIR_OP_STORE_INDEXED;
  int indexed = op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_LOAD_INDEXED;
  IROperand base = tcc_ir_op_get_dest_or_src1(ir, q, !is_store);
  int t = sra_temp_pos(base, ntemp);
  if (t < 0 || irop_get_tag(base) != IROP_TAG_VREG || base.is_local || base.is_llocal || base.is_lval == indexed)
    return -1;
  *disp = 0;
  *acc = base;
  if (indexed)
  {
    IROperand scale = tcc_ir_op_get_scale(ir, q);
    if (tcc_ir_op_src2_tag(ir, q) != IROP_TAG_IMM32 || tcc_ir_op_src2_is_lval(ir, q) ||
        (!irop_is_none(scale) && (irop_get_tag(scale) != IROP_TAG_IMM32 || irop_get_imm32(scale) != 0)))
      return -1;
    *disp = tcc_ir_op_src2_imm32(ir, q);
    *acc = tcc_ir_op_get_dest_or_src1(ir, q, is_store);
    acc->aux = base.aux;
  }
  if (irop_is_64bit(*acc) || acc->is_complex || !sra_btype_bytes(irop_get_btype(*acc)) ||
      tcc_ir_access_is_volatile(ir, *acc))
    return -1;
  return t;
}

/* Is q address arithmetic whose result feeds nothing but memory accesses:
 * an ADD / SUB into an address-only TEMP (sra_addr_only_temps). */
static int sra_addr_arith(TCCIRState *ir, IRQuadCompact *q, int ntemp, const uint8_t *aok)
{
  if ((q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB) || !irop_config[q->op].has_dest)
    return 0;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  int t = sra_temp_pos(d, ntemp);
  return !d.is_lval && t >= 0 && aok && aok[t];
}

/* aok[t]: every use of TEMP t is the base of a load or a store, or a source
 * of address arithmetic whose own result is address-only -- so a field read
 * that lands in such a chain is a base pointer a loop would otherwise reload
 * from the frame each iteration.  Accepting a field in ANY operand was tried
 * before and made the Zig compiler larger (the header's note); this stays
 * narrow to uses that only ever address memory. */
static uint8_t *sra_addr_only_temps(TCCIRState *ir, int ntemp)
{
  const int n = ir->next_instruction_index;
  uint8_t *aok = ntemp > 0 ? tcc_malloc(ntemp) : NULL;
  if (!aok)
    return NULL;
  memset(aok, 1, ntemp);
  for (int round = 0, changed = 1; changed && round < 8; round++)
  {
    changed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      int op = q->op;
      if (op == TCCIR_OP_NOP)
        continue;
      int is_access = op == TCCIR_OP_LOAD || op == TCCIR_OP_STORE ||
                      op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_STORE_INDEXED;
      int indexed = op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_STORE_INDEXED;
      int base_slot = op == TCCIR_OP_STORE || op == TCCIR_OP_STORE_INDEXED ? 0 : 1;
      int is_addsub = op == TCCIR_OP_ADD || op == TCCIR_OP_SUB;
      for (int s = 0; s < 3; s++)
      {
        IROperand o = sra_operand(ir, q, s);
        int t = sra_temp_pos(o, ntemp);
        if (t < 0 || !aok[t])
          continue;
        if (s == 0 && !o.is_lval)
          continue; /* a definition, not a use */
        int ok_use = 0;
        if (is_access && s == base_slot && o.is_lval != indexed && !o.is_local && !o.is_llocal)
          ok_use = 1; /* the base of a memory access */
        else if (s >= 1 && o.is_lval)
          ok_use = 1; /* *T fused into an operand: a load or store through T */
        else if (is_addsub && (s == 1 || s == 2) && !o.is_lval)
          ok_use = sra_addr_arith(ir, q, ntemp, aok);
        if (!ok_use)
        {
          aok[t] = 0;
          changed = 1;
        }
      }
    }
  }
  return aok;
}

/* A call that reads or writes a range of an object whole.
 *
 * The frontend copies an aggregate over 16 bytes with an __aeabi_memmove4/8
 * call, and a struct-returning call writes its result through a buffer
 * address passed as argument 0.  Either names the object's address in a
 * call, which would keep it in memory.  But a word-aligned copy between the
 * object and another pointer is a set of field copies -- when the object is
 * replaced it becomes one LOAD (copy in) or STORE (copy out) per field, at
 * the call, and the call goes.  And a result buffer is only written: the
 * callee cannot keep its address, so the object keeps its slot as the buffer
 * and each field VAR is loaded from it right after the call.
 *
 * Bytes a call writes and a later whole read passes on (a copy out, a by-value
 * argument) need a field even when nothing accesses them one at a time; bytes
 * only one side touches are indeterminate on the other and need none.  An
 * object reached by nothing but such calls stays in memory: moving it field
 * by field costs more than the block copy it replaces. */
typedef struct
{
  int call;       /* the call instruction */
  int cid;        /* its call id */
  int param;      /* the argument naming the object's address */
  int unit, off;  /* the object, and the range's frame offset */
  int n;          /* the range's bytes */
  int dir;        /* 0: the call writes the range, 1: it reads it */
  int sret;       /* a result buffer: the call stays, the fields load after it */
  int other;      /* a copy: the other pointer's argument */
  int other_t;    /* its TEMP, or -1 */
  int t;          /* the address TEMP the argument reads, or -1 */
  IROperand slot; /* a result buffer: an address operand of the object's slot */
  int pair;       /* a copy between two replaced objects: the other's unit, or -1 */
  int pair_off;   /* and the range's frame offset in it */
} SraWhole;

enum
{
  SRA_CALL_OTHER,
  SRA_CALL_COPY, /* (dst, src, n), both word aligned */
};

static int sra_call_kind(TCCIRState *ir, IRQuadCompact *q)
{
  if (q->op != TCCIR_OP_FUNCCALLVOID)
    return SRA_CALL_OTHER;
  Sym *cs = tcc_ir_op_src1_sym(ir, q);
  const char *nm = cs ? get_tok_str(cs->v, NULL) : NULL;
  if (nm && ir_opt_name_in(nm, "__aeabi_memmove4\0__aeabi_memmove8\0__aeabi_memcpy4\0__aeabi_memcpy8\0"))
    return SRA_CALL_COPY;
  return SRA_CALL_OTHER;
}

/* Argument i (a FUNCPARAMVAL) of a call that accesses a range at the address
 * it passes whole: fills w's call, direction, length and copy partner.
 * call_at / cparam index the calls by id: the instruction, and arguments 0-2
 * (-1 none, -2 twice). */
static int sra_whole_use(TCCIRState *ir, int i, const int32_t *call_at, const int32_t (*cparam)[3],
                         const uint8_t *ckind, int ncall, SraWhole *w)
{
  IRQuadCompact *q = &ir->compact_instructions[i];
  uint32_t e = (uint32_t)tcc_ir_op_src2_imm(ir, q);
  int c = TCCIR_DECODE_CALL_ID(e), p = TCCIR_DECODE_PARAM_IDX(e);
  if (c < 0 || c >= ncall || call_at[c] <= i)
    return 0;
  memset(w, 0, sizeof(*w));
  w->call = call_at[c];
  w->cid = c;
  w->param = i;
  w->other = w->other_t = w->t = w->pair = -1;
  if (ckind[c] == SRA_CALL_COPY && (p == 0 || p == 1))
  {
    if (cparam[c][0] < 0 || cparam[c][1] < 0 || cparam[c][2] < 0)
      return 0;
    IROperand nop = tcc_ir_op_get_src1(ir, &ir->compact_instructions[cparam[c][2]]);
    if (!irop_is_plain_imm(nop))
      return 0;
    int64_t nn = irop_get_imm64_ex(ir, nop);
    if (nn <= 0 || nn > SRA_MAX_BYTES)
      return 0;
    /* The other pointer is a register value, read where the copy happens:
     * nothing but the call's own arguments may sit between them. */
    IROperand o = tcc_ir_op_get_src1(ir, &ir->compact_instructions[cparam[c][1 - p]]);
    if (irop_get_tag(o) != IROP_TAG_VREG || irop_get_vreg(o) < 0 || o.is_lval || o.is_llocal || o.is_local)
      return 0;
    /* Replacing the object turns the copy into one access per FIELD it needs,
     * so the other side is read or written only where a field is: wrong when
     * that side is volatile -- copying `*(volatile struct S *)p` reads all of
     * it.  The frontend proves the copy's source non-volatile (vstore); an
     * unproven side declines, as everywhere else. */
    if (tcc_ir_access_is_volatile(ir, o))
      return 0;
    int first = cparam[c][0];
    for (int k = 1; k < 3; k++)
      if (cparam[c][k] < first)
        first = cparam[c][k];
    for (int j = first; j < w->call; j++)
    {
      IRQuadCompact *jq = &ir->compact_instructions[j];
      if (jq->op != TCCIR_OP_NOP && j != cparam[c][0] && j != cparam[c][1] && j != cparam[c][2])
        return 0;
    }
    w->n = (int)nn;
    w->dir = p;
    w->other = cparam[c][1 - p];
    return 1;
  }
  if (p == 0 && c < ir->sret_calls_size && ir->sret_calls[c] > 4)
  {
    w->n = ir->sret_calls[c];
    w->sret = 1;
    return 1;
  }
  return 0;
}

/* Fields for the bytes of [at0, at0 + n) no access covers: aligned words, or
 * narrow unsigned pieces where a word does not fit. */
static void sra_fill_gaps(SraUnit *u, int at0, int n)
{
  uint8_t cov[SRA_MAX_BYTES] = {0};
  for (int at = 0; at < SRA_MAX_BYTES; at++)
    for (int b = at; b < at + u->width[at] && b < SRA_MAX_BYTES; b++)
      cov[b] = 1;
  for (int b = at0; b < at0 + n;)
  {
    if (cov[b])
    {
      b++;
      continue;
    }
    int w = 1;
    if (!(b & 3) && b + 4 <= at0 + n && !cov[b + 1] && !cov[b + 2] && !cov[b + 3])
      w = 4;
    else if (!(b & 1) && b + 2 <= at0 + n && !cov[b + 1])
      w = 2;
    u->width[b] = w;
    if (w < 4)
      u->sign[b] = 2;
    for (int k = b; k < b + w; k++)
      cov[k] = 1;
    b += w;
  }
}

/* No field of u crosses an end of [at0, at0 + n). */
static int sra_range_clean(const SraUnit *u, int at0, int n)
{
  for (int at = 0; at < SRA_MAX_BYTES; at++)
  {
    int end = at + u->width[at];
    if (u->width[at] && ((at < at0 && end > at0) || (at < at0 + n && end > at0 + n)))
      return 0;
  }
  return 1;
}

static int sra_width_btype(int w)
{
  return w == 1 ? IROP_BTYPE_INT8 : w == 2 ? IROP_BTYPE_INT16 : IROP_BTYPE_INT32;
}

static IROperand sra_new_temp(TCCIRState *ir, int bt)
{
  IROperand t = irop_make_vreg(tcc_ir_vreg_alloc_temp(ir), bt);
  t.is_unsigned = 1;
  return t;
}

/* The object's slot at frame offset off, as an lvalue of type bt. */
static IROperand sra_slot_op(IROperand addr, int off, int bt)
{
  IROperand s = irop_retype_scalar(addr, bt);
  s.u.imm32 = off;
  s.is_lval = 1;
  s.is_unsigned = 1;
  s.aux = IROP_AUX_NONVOLATILE;
  return s;
}

static IROperand sra_field_op(TCCIRState *ir, SraUnit *u, int at, int bt, int is_unsigned);

/* The `bytes`-wide value at byte `at` of u, whose narrow fields cover only
 * part of it (sra_resolve_fields): their VARs shifted into place and ORed, by
 * instructions ahead of i -- and a signed halfword's sign extended, since a
 * field now fills its top byte. */
static IROperand sra_combine(TCCIRState *ir, SraInsList *ins, int i, SraUnit *u, int at, int bytes,
                             int is_unsigned)
{
  IROperand v = sra_field_op(ir, u, at, IROP_BTYPE_INT32, 1);
  if (u->pmask[at])
  {
    IROperand m = sra_new_temp(ir, IROP_BTYPE_INT32);
    sra_ins_add(ir, ins, i, 0, TCCIR_OP_AND, m, v, irop_make_imm32(-1, sra_field_mask(u->width[at]), IROP_BTYPE_INT32));
    v = m;
  }
  for (int b = at + 1; b < at + bytes && b < SRA_MAX_BYTES; b++)
  {
    if (!u->width[b])
      continue;
    IROperand f = sra_field_op(ir, u, b, IROP_BTYPE_INT32, 1);
    if (u->pmask[b])
    {
      IROperand m = sra_new_temp(ir, IROP_BTYPE_INT32);
      sra_ins_add(ir, ins, i, 0, TCCIR_OP_AND, m, f, irop_make_imm32(-1, sra_field_mask(u->width[b]), IROP_BTYPE_INT32));
      f = m;
    }
    IROperand t = sra_new_temp(ir, IROP_BTYPE_INT32), o = sra_new_temp(ir, IROP_BTYPE_INT32);
    sra_ins_add(ir, ins, i, 0, TCCIR_OP_SHL, t, f, irop_make_imm32(-1, 8 * (b - at), IROP_BTYPE_INT32));
    sra_ins_add(ir, ins, i, 0, TCCIR_OP_OR, o, v, t);
    v = o;
  }
  if (!is_unsigned && bytes < 4)
  {
    const int sh = 32 - 8 * bytes;
    IROperand t = sra_new_temp(ir, IROP_BTYPE_INT32), o = sra_new_temp(ir, IROP_BTYPE_INT32);
    t.is_unsigned = o.is_unsigned = 0;
    sra_ins_add(ir, ins, i, 0, TCCIR_OP_SHL, t, v, irop_make_imm32(-1, sh, IROP_BTYPE_INT32));
    sra_ins_add(ir, ins, i, 0, TCCIR_OP_SAR, o, t, irop_make_imm32(-1, sh, IROP_BTYPE_INT32));
    v = o;
  }
  return v;
}

/* Does a `bytes`-wide read at `at` of u cover a field past the one there? */
static int sra_covers_more(const SraUnit *u, int at, int bytes)
{
  for (int b = at + 1; b < at + bytes && b < SRA_MAX_BYTES; b++)
    if (u->width[b])
      return 1;
  return 0;
}

/* Copies of the fields of [s0, s0 + n) of unit src into those of [d0, d0 + n)
 * of unit dst, laid out alike (checked where the copy was accepted), ahead of
 * the copy call at `anchor`.  A narrow or 64-bit field's definition is an
 * ASSIGN (sra_store_as_def, sra_rewrite_wide). */
static void sra_expand_pair(TCCIRState *ir, SraInsList *ins, SraUnit *dst, int d0, SraUnit *src, int s0, int n,
                            int anchor)
{
  for (int d = 0; d < n; d++)
  {
    const int w = dst->width[d0 + d];
    if (!w)
      continue;
    const int bt = w == 8 ? IROP_BTYPE_INT64 : IROP_BTYPE_INT32;
    IROperand x = sra_new_temp(ir, bt);
    sra_ins_add(ir, ins, anchor, 0, TCCIR_OP_LOAD, x, sra_field_op(ir, src, s0 + d, bt, 1), IROP_NONE);
    if (w < 4 && src->pmask[s0 + d])
    {
      IROperand m = sra_new_temp(ir, bt);
      sra_ins_add(ir, ins, anchor, 0, TCCIR_OP_AND, m, x, irop_make_imm32(-1, sra_field_mask(w), IROP_BTYPE_INT32));
      x = m;
    }
    IROperand rep = sra_field_op(ir, dst, d0 + d, bt, 1);
    if (w != 4)
      rep.is_lval = 0;
    sra_ins_add(ir, ins, anchor, 0, w != 4 ? TCCIR_OP_ASSIGN : TCCIR_OP_STORE, rep, x, IROP_NONE);
    d += w - 1;
  }
}

/* The field copies that replace e: loads into the field VARs of the range
 * (from the other pointer ahead of the call, or from the object's own slot
 * after it), or stores of them through the other pointer. */
static void sra_expand_whole(TCCIRState *ir, SraInsList *ins, SraUnit *u, const SraWhole *e, IROperand base)
{
  const int a0 = e->off - u->lo, anchor = e->call, after = e->sret;
  for (int at = a0; at < a0 + e->n; at++)
  {
    const int w = u->width[at];
    if (!w)
      continue;
    const int d = at - a0;
    IROperand imm = irop_make_imm32(-1, d, IROP_BTYPE_INT32);
    IROperand imm4 = irop_make_imm32(-1, d + 4, IROP_BTYPE_INT32);
    if (e->dir == 0 && w == 8)
    {
      IROperand x = sra_new_temp(ir, IROP_BTYPE_INT64);
      if (e->sret)
        sra_ins_add(ir, ins, anchor, after, TCCIR_OP_LOAD, x, sra_slot_op(e->slot, u->lo + at, IROP_BTYPE_INT64),
                    IROP_NONE);
      else
      {
        IROperand lo = sra_new_temp(ir, IROP_BTYPE_INT32), hi = sra_new_temp(ir, IROP_BTYPE_INT32);
        sra_ins_add(ir, ins, anchor, after, TCCIR_OP_LOAD_INDEXED, lo, base, imm);
        sra_ins_add(ir, ins, anchor, after, TCCIR_OP_LOAD_INDEXED, hi, base, imm4);
        sra_ins_add(ir, ins, anchor, after, TCCIR_OP_PACK64, x, lo, hi);
      }
      IROperand rep = sra_field_op(ir, u, at, IROP_BTYPE_INT64, 1);
      rep.is_lval = 0;
      sra_ins_add(ir, ins, anchor, after, TCCIR_OP_ASSIGN, rep, x, IROP_NONE);
    }
    else if (e->dir == 0)
    {
      IROperand x = sra_new_temp(ir, sra_width_btype(w));
      if (e->sret)
        sra_ins_add(ir, ins, anchor, after, TCCIR_OP_LOAD, x, sra_slot_op(e->slot, u->lo + at, sra_width_btype(w)),
                    IROP_NONE);
      else
        sra_ins_add(ir, ins, anchor, after, TCCIR_OP_LOAD_INDEXED, x, base, imm);
      /* A narrow field's definition is an ASSIGN (sra_store_as_def). */
      IROperand rep = sra_field_op(ir, u, at, IROP_BTYPE_INT32, 1);
      if (w < 4)
        rep.is_lval = 0;
      sra_ins_add(ir, ins, anchor, after, w < 4 ? TCCIR_OP_ASSIGN : TCCIR_OP_STORE, rep, x, IROP_NONE);
    }
    else if (w == 8)
    {
      IROperand x = sra_new_temp(ir, IROP_BTYPE_INT64), th = sra_new_temp(ir, IROP_BTYPE_INT64);
      IROperand lo = sra_new_temp(ir, IROP_BTYPE_INT32), hi = sra_new_temp(ir, IROP_BTYPE_INT32);
      sra_ins_add(ir, ins, anchor, after, TCCIR_OP_LOAD, x, sra_field_op(ir, u, at, IROP_BTYPE_INT64, 1), IROP_NONE);
      sra_ins_add(ir, ins, anchor, after, TCCIR_OP_ASSIGN, lo, x, IROP_NONE);
      sra_ins_add(ir, ins, anchor, after, TCCIR_OP_SHR, th, x, irop_make_imm32(-1, 32, IROP_BTYPE_INT32));
      sra_ins_add(ir, ins, anchor, after, TCCIR_OP_ASSIGN, hi, th, IROP_NONE);
      sra_ins_add(ir, ins, anchor, after, TCCIR_OP_STORE_INDEXED, base, lo, imm);
      sra_ins_add(ir, ins, anchor, after, TCCIR_OP_STORE_INDEXED, base, hi, imm4);
    }
    else
    {
      IROperand x = sra_new_temp(ir, IROP_BTYPE_INT32);
      sra_ins_add(ir, ins, anchor, after, TCCIR_OP_LOAD, x, sra_field_op(ir, u, at, IROP_BTYPE_INT32, 1), IROP_NONE);
      IROperand v = irop_make_vreg(irop_get_vreg(x), sra_width_btype(w));
      v.is_unsigned = 1;
      sra_ins_add(ir, ins, anchor, after, TCCIR_OP_STORE_INDEXED, base, v, imm);
    }
    at += w - 1;
  }
}

#define SRA_VMIX (INT32_MIN + 1)     /* a pointer VAR with more than one value */
#define SRA_UNKNOWN (INT32_MIN + 2)  /* a value that may or may not be a frame address */
#define SRA_NOT_ADDR (INT32_MIN + 3) /* a value that is no frame address */
#define SRA_FIRST_ADDR (INT32_MIN + 4)
#define SRA_DF_MAX (4 << 20) /* blocks x pointer VARs the dataflow tracks */

/* The position of a VAR operand below nvar, or -1. */
static int sra_var_pos(IROperand o, int nvar)
{
  int32_t vr = irop_get_vreg(o);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR || TCCIR_DECODE_VREG_POSITION(vr) >= nvar)
    return -1;
  return TCCIR_DECODE_VREG_POSITION(vr);
}

/* The VAR q defines, or -1: a value dest, or a STORE into the VAR's own slot
 * (an inlined parameter's binding, ssa.c's reading). */
static int sra_var_def(TCCIRState *ir, IRQuadCompact *q, int nvar)
{
  if (!irop_config[q->op].has_dest || q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC)
    return -1;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  if (d.is_llocal || (q->op == TCCIR_OP_STORE ? !d.is_local : d.is_lval))
    return -1;
  return sra_var_pos(d, nvar);
}

/* An address copy or constant offset: LEA, ASSIGN, a LOAD of a VAR's value, or
 * ADD / SUB (whose second operand sra_offset_by checks). */
static int sra_is_copy_op(TCCIRState *ir, IRQuadCompact *q, int nvar)
{
  if (q->op == TCCIR_OP_LOAD)
  {
    IROperand a = tcc_ir_op_get_src1(ir, q);
    return sra_var_pos(a, nvar) >= 0 && !a.is_llocal;
  }
  return q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB;
}

/* base +/- q's constant second operand, or SRA_VMIX when it is not one. */
static int32_t sra_offset_by(TCCIRState *ir, IRQuadCompact *q, int32_t base)
{
  if (tcc_ir_op_src2_tag(ir, q) != IROP_TAG_IMM32 || tcc_ir_op_src2_is_lval(ir, q))
    return SRA_VMIX;
  int64_t x = (int64_t)base + (q->op == TCCIR_OP_ADD ? 1 : -1) * (int64_t)tcc_ir_op_src2_imm32(ir, q);
  return x > -(1 << 24) && x < (1 << 24) ? (int32_t)x : SRA_VMIX;
}

/* Is q an address computation of a TEMP -- `T <- Addr[slot]`, a copy, or a
 * constant offset of another TEMP?  Returns T's position, or -1. */
static int sra_addr_def(TCCIRState *ir, IRQuadCompact *q, int ntemp)
{
  /* A LOAD of a VAR's value is a copy too: `T <- V1 [LOAD]` (a pointer VAR). */
  if (q->op == TCCIR_OP_LOAD)
  {
    IROperand a = tcc_ir_op_get_src1(ir, q);
    int32_t avr = irop_get_vreg(a);
    if (avr < 0 || TCCIR_DECODE_VREG_TYPE(avr) != TCCIR_VREG_TYPE_VAR || a.is_llocal)
      return -1;
  }
  else if (q->op != TCCIR_OP_LEA && q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB)
    return -1;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  return d.is_lval ? -1 : sra_temp_pos(d, ntemp);
}

/* A VAR only ever written with one constant reads as that constant.
 *
 * Any read either follows a write, which stored that constant, or sees a value
 * nothing wrote -- so no dominance question arises, and no phi.  Two kinds of
 * VAR are trusted to show every write:
 *  - the field VARs this run made: every access to a replaced object was
 *    rewritten to them, SRA leaving an object alone unless it saw all of them.
 *    The Zig C backend zero-fills each zig_u128 it builds and then writes both
 *    halves, so `zig_make_u128(0, x)`'s high word is written 0, then 0;
 *  - any other VAR written only by slot STOREs: an inlined function's
 *    parameter is bound that way, and zig.h passes `bits` (128, 64) to every
 *    u128 helper.  VARs written by an ASSIGN are left out: the short-circuit
 *    boolean lowering writes one arm into a TEMP the allocator later merges
 *    with the VAR (const_var_prop.c), a write this scan cannot see.
 * SSA would rename such a VAR only when its write's block has a non-empty
 * dominance frontier (ssa_var_promotable), and a constant nothing forwards
 * keeps the sign tests and shift-by-zero branches of every u128 helper alive.
 * Address-taken and volatile VARs are skipped, and a VAR read at a width
 * other than its writes' keeps its reads. */
static int sra_forward_const_vars(TCCIRState *ir, int first_sra_var)
{
  const int nv = ir->next_local_variable, n = ir->next_instruction_index;
  if (nv <= 0)
    return 0;
  uint8_t *st = tcc_mallocz(nv); /* 0 unwritten, 1 always val[k], 2 no */
  int64_t *val = tcc_malloc(sizeof(int64_t) * nv);
  uint8_t *bt = tcc_mallocz(nv);
  for (int k = 0; k < nv; k++)
  {
    IRLiveInterval *iv = tcc_ir_get_live_interval(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, k));
    if (!iv || iv->addrtaken || iv->is_volatile)
      st[k] = 2;
  }
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    /* A LEA takes the VAR's address: whatever is written through it is not
     * seen here. */
    if (q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_ASM_INPUT || q->op == TCCIR_OP_ASM_OUTPUT)
    {
      IROperand a = tcc_ir_op_get_dest_or_src1(ir, q, q->op != TCCIR_OP_ASM_OUTPUT);
      int32_t avr = irop_get_vreg(a);
      if (avr >= 0 && TCCIR_DECODE_VREG_TYPE(avr) == TCCIR_VREG_TYPE_VAR && TCCIR_DECODE_VREG_POSITION(avr) < nv)
        st[TCCIR_DECODE_VREG_POSITION(avr)] = 2;
      continue;
    }
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(d);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
      continue;
    int k = TCCIR_DECODE_VREG_POSITION(vr);
    if (k >= nv || st[k] == 2)
      continue;
    /* A STORE through the VAR's value writes somewhere else. */
    if (q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC ||
        (q->op == TCCIR_OP_STORE && k < first_sra_var && !d.is_local))
      continue;
    IROperand v = irop_config[q->op].has_src1 ? tcc_ir_op_get_src1(ir, q) : IROP_NONE;
    int tag = irop_get_tag(v);
    int btype = irop_get_btype(d);
    int ok_op = q->op == TCCIR_OP_STORE || (q->op == TCCIR_OP_ASSIGN && k >= first_sra_var);
    if (!ok_op || v.is_lval || v.is_sym || (tag != IROP_TAG_IMM32 && tag != IROP_TAG_I64) ||
        (btype != IROP_BTYPE_INT8 && btype != IROP_BTYPE_INT16 && btype != IROP_BTYPE_INT32 &&
         btype != IROP_BTYPE_INT64) ||
        (st[k] == 1 && bt[k] != btype))
    {
      st[k] = 2;
      continue;
    }
    int64_t c = irop_get_imm64_ex(ir, v);
    if (btype == IROP_BTYPE_INT8)
      c = d.is_unsigned ? (int64_t)(uint8_t)c : (int64_t)(int8_t)c;
    else if (btype == IROP_BTYPE_INT16)
      c = d.is_unsigned ? (int64_t)(uint16_t)c : (int64_t)(int16_t)c;
    else if (btype != IROP_BTYPE_INT64)
      c = (int64_t)(int32_t)(uint32_t)c;
    st[k] = st[k] == 0 || val[k] == c ? 1 : 2;
    val[k] = c;
    bt[k] = (uint8_t)btype;
  }
  /* Every read must be a plain value read at the writes' width. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int s = 0; s < 4; s++)
    {
      IROperand o = sra_operand(ir, q, s);
      int32_t vr = irop_get_vreg(o);
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
        continue;
      int k = TCCIR_DECODE_VREG_POSITION(vr);
      if (k >= nv || st[k] != 1)
        continue;
      if (s == 0)
      {
        /* The writes seen above; a STORE through the VAR's value reads it. */
        if (q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC ||
            (q->op == TCCIR_OP_STORE && !o.is_local && k < first_sra_var))
          st[k] = 2;
        continue;
      }
      if (s == 3 || !o.is_lval || o.is_llocal || irop_get_btype(o) != bt[k])
        st[k] = 2;
    }
  }
  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    /* A fused barrel shift names its operand registers: it keeps its reads
     * (and the VAR its STOREs). */
    if (q->op == TCCIR_OP_NOP || tcc_ir_barrel_shift_at(ir, q))
      continue;
    for (int s = 1; s <= 2; s++)
    {
      if (!(s == 1 ? irop_config[q->op].has_src1 : irop_config[q->op].has_src2))
        continue;
      IROperand o = tcc_ir_op_get_src1_or_2(ir, q, s != 1);
      int32_t vr = irop_get_vreg(o);
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
        continue;
      int k = TCCIR_DECODE_VREG_POSITION(vr);
      if (k >= nv || st[k] != 1)
        continue;
      int64_t c = val[k];
      int obt = irop_get_btype(o);
      IROperand imm = c == (int64_t)(int32_t)c ? irop_make_imm32(-1, (int32_t)c, obt)
                                               : irop_make_i64(-1, tcc_ir_pool_add_i64(ir, c), obt);
      imm.is_unsigned = o.is_unsigned;
      if (s == 1 && q->op == TCCIR_OP_LOAD)
        q->op = TCCIR_OP_ASSIGN;
      if (s == 1)
        tcc_ir_set_src1(ir, i, imm);
      else
        tcc_ir_set_src2(ir, i, imm);
      changes++;
    }
  }
  tcc_free(bt);
  tcc_free(val);
  tcc_free(st);
  return changes;
}

static int sra_objects(TCCIRState *ir)
{
  if (ir->frame_relaid || ir->frame_obj_count == 0)
    return 0;

  /* Units: the frame objects' merged extents, sorted by start. */
  int lo, hi;
  tcc_ir_frame_object_at(ir, ir->frame_objs[0], &lo, &hi); /* builds ir->frame_index */
  int nu = ir->frame_index_n;
  if (nu <= 0)
    return 0;
  SraUnit *u = tcc_mallocz(sizeof(SraUnit) * nu);
  for (int k = 0; k < nu; k++)
  {
    u[k].lo = ir->frame_index[2 * k];
    u[k].hi = ir->frame_index[2 * k + 1];
    u[k].pad = sra_pad_off() ? 0 : tcc_ir_frame_padding(ir, u[k].lo, u[k].hi);
    for (int b = 0; b < SRA_MAX_BYTES; b++)
      u[k].var[b] = -1;
  }
  const int n = ir->next_instruction_index;
  /* A split doubleword's rewrite adds instructions; a computed goto's targets
   * are addresses taken in the frontend, which that would move. */
  int no_wide = 0;
  for (int i = 0; i < n && !no_wide; i++)
    no_wide = ir->compact_instructions[i].op == TCCIR_OP_IJUMP;

  /* Address TEMPs.  The frontend reaches a field through one
   * (`T <- &s; *(T + 4) = x`).  A TEMP defined once, from `Addr[slot]` or a
   * copy / constant offset of such a TEMP, holds that frame address wherever it
   * is read; when every use is the base of a load or store at a constant
   * displacement, or feeds another such TEMP, the accesses are the object's
   * fields.  Nothing is rewritten unless the object is replaced. */
  /* By-value struct arguments may become word arguments (sra_struct_param_words). */
  const int struct_param = !tcc_ir_opt_pass_disabled("sra_struct_param");
  const int ntemp = ir->next_temporary_variable;
  uint8_t *defs = ntemp > 0 ? tcc_mallocz(ntemp) : NULL;
  /* Each TEMP's only defining instruction, or -1: what a narrow store's value
   * has to be looked up through (sra_narrow_store_fits). */
  int32_t *def_idx = ntemp > 0 ? tcc_malloc(sizeof(int32_t) * ntemp) : NULL;
  for (int t = 0; t < ntemp; t++)
    def_idx[t] = -1;
  int32_t *toff = ntemp > 0 ? tcc_malloc(sizeof(int32_t) * ntemp) : NULL;
  int32_t *tfrom = ntemp > 0 ? tcc_malloc(sizeof(int32_t) * ntemp) : NULL; /* TEMP it was derived from, or -1 */
  int32_t *troot = ntemp > 0 ? tcc_malloc(sizeof(int32_t) * ntemp) : NULL; /* unit of the root Addr */
  uint8_t *tbad = ntemp > 0 ? tcc_mallocz(ntemp) : NULL;
  int16_t *tbad_op = ntemp > 0 ? tcc_mallocz(sizeof(int16_t) * ntemp) : NULL;
  int32_t *tbad_at = ntemp > 0 ? tcc_mallocz(sizeof(int32_t) * ntemp) : NULL;
  for (int t = 0; t < ntemp; t++)
    toff[t] = SRA_NO_ADDR, tfrom[t] = -1, troot[t] = -1;
  for (int i = 0; i < n && ntemp > 0; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int t;
    /* Definitions: a value dest, or a pointer a post-increment writes back.  A
     * STORE_INDEXED dest is the base pointer, read and not written. */
    if (irop_config[q->op].has_dest && q->op != TCCIR_OP_STORE_INDEXED)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if ((!d.is_lval || q->op == TCCIR_OP_STORE_POSTINC) && (t = sra_temp_pos(d, ntemp)) >= 0 && defs[t] < 2)
      {
        def_idx[t] = defs[t] == 0 ? i : -1;
        defs[t]++;
      }
    }
    if (q->op == TCCIR_OP_LOAD_POSTINC && (t = sra_temp_pos(tcc_ir_op_get_src1(ir, q), ntemp)) >= 0)
      defs[t] = 2, def_idx[t] = -1;
  }
  for (int round = 0, changed = 1; round < 8 && changed && ntemp > 0; round++)
  {
    changed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      int t = sra_addr_def(ir, q, ntemp);
      if (t < 0 || defs[t] != 1 || toff[t] != SRA_NO_ADDR)
        continue;
      IROperand a = tcc_ir_op_get_src1(ir, q);
      int off, at, k;
      if (q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_ASSIGN)
      {
        if (!a.is_lval && !a.is_llocal && sra_concrete(a, &off) && (k = sra_find_unit(u, nu, off)) >= 0)
          toff[t] = off, troot[t] = k;
        else if (q->op == TCCIR_OP_ASSIGN && !a.is_lval && (at = sra_temp_pos(a, ntemp)) >= 0 &&
                 toff[at] != SRA_NO_ADDR)
          toff[t] = toff[at], troot[t] = troot[at], tfrom[t] = at;
      }
      else
      {
        IROperand c = tcc_ir_op_get_src2(ir, q);
        if (!a.is_lval && (at = sra_temp_pos(a, ntemp)) >= 0 && toff[at] != SRA_NO_ADDR &&
            irop_get_tag(c) == IROP_TAG_IMM32 && !c.is_lval)
        {
          int64_t v = (int64_t)toff[at] + (q->op == TCCIR_OP_ADD ? 1 : -1) * (int64_t)irop_get_imm32(c);
          if (v > -(1 << 24) && v < (1 << 24))
            toff[t] = (int32_t)v, troot[t] = troot[at], tfrom[t] = at;
        }
      }
      changed |= toff[t] != SRA_NO_ADDR;
    }
  }
  /* Pointer VARs.  The Zig C backend keeps an object's address in a C local
   * -- `t1 = &t0; t5 = &t1->len; x = *t5` -- which the frontend makes a VAR,
   * copied into a TEMP at each use (`T <- V1 [ASSIGN]` or `[LOAD]`), and it
   * reuses one local for unrelated pointers of the same type.
   *
   * A value is followed through such VARs in three states: a known frame
   * address, NOT a frame address, or unknown.  Within a block a VAR holds what
   * its last definition there gave it -- `Addr[slot]`, a copy or constant
   * offset of a known address, or, from any other source (a load from memory,
   * a call's result, a parameter, arithmetic), not a frame address: an
   * object's address reaches memory, a call or arithmetic only by a use that
   * pins that object below, so such a value cannot point into an object that
   * stays promotable.  Across blocks a VAR that can never hold a frame
   * address holds none, and one whose every definition gives the same address
   * (`&obj`, a copy of such a VAR, a constant offset of one) holds it
   * everywhere; otherwise its value there is unknown.
   *
   * A TEMP copied from a known address becomes an address root like one
   * defined from the slot itself.  A read of an unknown value, or of an
   * address in any use but a copy or constant offset -- a compare, a call
   * argument, a store, a dereference -- pins every object the VAR ever held
   * ("ptrvar-*" in TCC_SRA_DBG). */
  const int nvar = sra_ptrvar() ? ir->next_local_variable : 0;
  int32_t *vglob = nvar ? tcc_malloc(sizeof(int32_t) * nvar) : NULL; /* the one address, SRA_NO_ADDR, SRA_VMIX */
  uint8_t *vmay = nvar ? tcc_mallocz(nvar) : NULL; /* can hold a frame address at all */
  /* 1 read with no known value, 2 an address read other than as one, 3 address-taken or volatile */
  uint8_t *vbad = nvar ? tcc_mallocz(nvar) : NULL;
  int16_t *vbad_op = nvar ? tcc_mallocz(sizeof(int16_t) * nvar) : NULL;
  int32_t *vbad_at = nvar ? tcc_mallocz(sizeof(int32_t) * nvar) : NULL; /* TCC_SRA_DBG: that read's instruction */
  int32_t *vcur = nvar ? tcc_malloc(sizeof(int32_t) * nvar) : NULL;
  int32_t *vcurroot = nvar ? tcc_malloc(sizeof(int32_t) * nvar) : NULL;
  int *vcurgen = nvar ? tcc_mallocz(sizeof(int) * nvar) : NULL;
  int32_t *vdef = nvar && n ? tcc_malloc(sizeof(int32_t) * n) : NULL; /* per instruction: the address a VAR def holds */
  int32_t *vdefroot = nvar && n ? tcc_malloc(sizeof(int32_t) * n) : NULL;
  if (nvar)
  {
    for (int v = 0; v < nvar; v++)
      vglob[v] = SRA_NO_ADDR;
    for (int i = 0; i < n; i++)
      vdef[i] = SRA_NO_ADDR, vdefroot[i] = -1;
    /* vmay: a definition from a frame address, or a copy (or offset) of a
     * VAR or TEMP that can hold one.  TEMPs take part: `T <- V0; V3 <- T + 4`
     * passes V0's address to V3 through a TEMP whose own address is only
     * found by the walk below, so a VAR fed by it must count as a pointer
     * VAR here already. */
    uint8_t *tmay = ntemp > 0 ? tcc_mallocz(ntemp) : NULL;
    for (int t = 0; t < ntemp; t++)
      if (toff[t] != SRA_NO_ADDR)
        tmay[t] = 1;
    for (int changed = 1; changed;)
    {
      changed = 0;
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        int v = sra_var_def(ir, q, nvar);
        int t = v < 0 && ntemp > 0 ? sra_addr_def(ir, q, ntemp) : -1;
        if ((v < 0 || vmay[v]) && (t < 0 || tmay[t]))
          continue;
        if (!sra_is_copy_op(ir, q, nvar))
          continue;
        IROperand a = tcc_ir_op_get_src1(ir, q);
        int off, at, w;
        if ((!a.is_lval && !a.is_llocal && sra_concrete(a, &off) && sra_find_unit(u, nu, off) >= 0 &&
             q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB && q->op != TCCIR_OP_LOAD) ||
            (!a.is_lval && (at = sra_temp_pos(a, ntemp)) >= 0 && tmay[at]) ||
            (!a.is_llocal && (w = sra_var_pos(a, nvar)) >= 0 && vmay[w]))
        {
          if (v >= 0)
            vmay[v] = 1;
          else
            tmay[t] = 1;
          changed = 1;
        }
      }
    }
    /* vglob: the one address a VAR holds wherever it is read.  Optimistic over
     * copy cycles: a VAR that can hold an address but is not known yet
     * contributes nothing this round. */
    for (int round = 0, changed = 1; changed && round < 16; round++)
    {
      changed = 0;
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        int v = sra_var_def(ir, q, nvar);
        if (v < 0 || !vmay[v] || vglob[v] == SRA_VMIX)
          continue;
        int32_t base = SRA_VMIX;
        if (sra_is_copy_op(ir, q, nvar))
        {
          IROperand a = tcc_ir_op_get_src1(ir, q);
          int off, at, w;
          if (!a.is_lval && !a.is_llocal && sra_concrete(a, &off) && sra_find_unit(u, nu, off) >= 0 &&
              q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB && q->op != TCCIR_OP_LOAD)
            base = off;
          else if (!a.is_lval && (at = sra_temp_pos(a, ntemp)) >= 0 && toff[at] != SRA_NO_ADDR && troot[at] >= 0)
            base = toff[at];
          else if (!a.is_llocal && (w = sra_var_pos(a, nvar)) >= 0 && vmay[w])
          {
            if (vglob[w] == SRA_NO_ADDR)
              continue; /* not known yet */
            base = vglob[w];
          }
          if (base != SRA_VMIX && (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB))
            base = sra_offset_by(ir, q, base);
          if (base != SRA_VMIX && sra_find_unit(u, nu, base) < 0)
            base = SRA_VMIX;
        }
        int32_t nv2 = vglob[v] == SRA_NO_ADDR ? base : vglob[v] == base ? base : SRA_VMIX;
        if (nv2 != vglob[v])
          vglob[v] = nv2, changed = 1;
      }
    }
    for (int v = 0; v < nvar; v++)
    {
      IRLiveInterval *iv = tcc_ir_get_live_interval(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, v));
      if (!iv || iv->addrtaken || iv->is_volatile)
        vbad[v] = 3, vglob[v] = SRA_VMIX, vmay[v] = 1;
    }
    /* Across blocks, by dataflow: a pointer VAR's value entering a block is
     * the meet of its value leaving every predecessor -- the same frame
     * address, or no frame address, on all of them, else unknown.  Blocks
     * the entry does not reach start unknown.  Past SRA_DF_MAX cells the
     * block-local rule above stands. */
    int npv = 0;
    int32_t *pv = tcc_malloc(sizeof(int32_t) * nvar);
    for (int v = 0; v < nvar; v++)
      pv[v] = vmay[v] ? npv++ : -1;
    IRCFG *cfg = npv && sra_ptrvar_df() ? tcc_ir_cfg_build(ir) : NULL;
    int32_t *entry = NULL;
    if (cfg && cfg->num_blocks > 0 && (int64_t)cfg->num_blocks * npv <= SRA_DF_MAX)
    {
      tcc_ir_cfg_compute_rpo(cfg);
      const int nb = cfg->num_blocks;
      entry = tcc_malloc(sizeof(int32_t) * nb * npv);
      for (int k = 0; k < nb * npv; k++)
        entry[k] = SRA_NO_ADDR; /* top: no path seen yet */
      for (int k = 0; k < npv; k++)
        entry[k] = SRA_NOT_ADDR; /* block 0: nothing assigned yet */
      int32_t *st = tcc_malloc(sizeof(int32_t) * npv);
      int32_t *tv = ntemp > 0 ? tcc_malloc(sizeof(int32_t) * ntemp) : NULL;
      int *tstamp = ntemp > 0 ? tcc_mallocz(sizeof(int) * ntemp) : NULL;
      int stamp = 0, changed = 1;
      for (int round = 0; changed && round < 64; round++)
      {
        changed = 0;
        for (int r = 0; r < cfg->rpo_count; r++)
        {
          const int b = cfg->rpo_order[r];
          IRBasicBlock *bb = &cfg->blocks[b];
          memcpy(st, entry + (size_t)b * npv, sizeof(int32_t) * npv);
          stamp++;
          for (int i = bb->start_idx; i < bb->end_idx; i++)
          {
            IRQuadCompact *q = &ir->compact_instructions[i];
            if (q->op == TCCIR_OP_NOP)
              continue;
            int vd = sra_var_def(ir, q, nvar);
            int td = ntemp > 0 ? sra_addr_def(ir, q, ntemp) : -1;
            if (td >= 0 && defs[td] != 1)
              td = -1;
            if ((vd < 0 || pv[vd] < 0) && td < 0)
              continue;
            int32_t val = SRA_NOT_ADDR;
            if (sra_is_copy_op(ir, q, nvar))
            {
              IROperand a = tcc_ir_op_get_src1(ir, q);
              int off, at, w;
              int32_t base = SRA_NOT_ADDR;
              if (!a.is_lval && !a.is_llocal && sra_concrete(a, &off) && q->op != TCCIR_OP_ADD &&
                  q->op != TCCIR_OP_SUB && q->op != TCCIR_OP_LOAD)
                base = off;
              else if (!a.is_lval && (at = sra_temp_pos(a, ntemp)) >= 0 && toff[at] != SRA_NO_ADDR)
                base = toff[at];
              else if (!a.is_lval && (at = sra_temp_pos(a, ntemp)) >= 0 && tmay[at])
                base = tstamp[at] == stamp ? tv[at] : SRA_UNKNOWN;
              else if (!a.is_llocal && (w = sra_var_pos(a, nvar)) >= 0)
                base = pv[w] >= 0 ? st[pv[w]] : SRA_NOT_ADDR;
              if (base >= SRA_FIRST_ADDR && (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB))
                base = sra_offset_by(ir, q, base);
              val = base == SRA_VMIX ? SRA_UNKNOWN
                    : base >= SRA_FIRST_ADDR && sra_find_unit(u, nu, base) < 0 ? SRA_UNKNOWN
                                                                              : base;
            }
            if (vd >= 0 && pv[vd] >= 0)
              st[pv[vd]] = val;
            else if (td >= 0 && ntemp > 0)
              tv[td] = val, tstamp[td] = stamp;
          }
          for (int e = 0; e < bb->num_succs; e++)
          {
            int32_t *dst = entry + (size_t)bb->succs[e] * npv;
            for (int k = 0; k < npv; k++)
            {
              int32_t m = dst[k] == SRA_NO_ADDR ? st[k] : st[k] == SRA_NO_ADDR || st[k] == dst[k] ? dst[k] : SRA_UNKNOWN;
              if (m != dst[k])
                dst[k] = m, changed = 1;
            }
          }
        }
      }
      tcc_free(st);
      tcc_free(tv);
      tcc_free(tstamp);
      if (changed) /* did not settle: fall back */
      {
        tcc_free(entry);
        entry = NULL;
      }
    }

    int *bs = tcc_mallocz(sizeof(int) * (n + 1));
    ir_opt_mark_block_starts(ir, bs, 1, n);
    int gen = 1;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (entry && cfg->instr_to_block[i] >= 0 && cfg->blocks[cfg->instr_to_block[i]].start_idx == i)
      {
        /* Enter the block with the dataflow's values. */
        gen++;
        const int32_t *e = entry + (size_t)cfg->instr_to_block[i] * npv;
        for (int v = 0; v < nvar; v++)
          if (pv[v] >= 0)
          {
            int32_t x = e[pv[v]] == SRA_NO_ADDR ? SRA_UNKNOWN : e[pv[v]];
            vcur[v] = x, vcurgen[v] = gen;
            vcurroot[v] = x >= SRA_FIRST_ADDR ? sra_find_unit(u, nu, x) : -1;
          }
      }
      else if (!entry && i && bs[i] == 1)
        gen++;
      if (q->op == TCCIR_OP_NOP)
        continue;
      int vd = sra_var_def(ir, q, nvar);
      int td = ntemp > 0 ? sra_addr_def(ir, q, ntemp) : -1;
      if (td >= 0 && defs[td] != 1)
        td = -1;
      /* What the instruction's destination holds. */
      int32_t val = SRA_NOT_ADDR, root = -1;
      int src_var = -1; /* the VAR a followed copy reads */
      if ((vd >= 0 || td >= 0) && sra_is_copy_op(ir, q, nvar))
      {
        IROperand a = tcc_ir_op_get_src1(ir, q);
        int off, at, w;
        int32_t base = SRA_NOT_ADDR, broot = -1;
        if (!a.is_lval && !a.is_llocal && sra_concrete(a, &off) && q->op != TCCIR_OP_ADD &&
            q->op != TCCIR_OP_SUB && q->op != TCCIR_OP_LOAD)
          base = off, broot = sra_find_unit(u, nu, off);
        else if (!a.is_lval && (at = sra_temp_pos(a, ntemp)) >= 0 && toff[at] != SRA_NO_ADDR)
          base = toff[at], broot = troot[at];
        else if (!a.is_lval && (at = sra_temp_pos(a, ntemp)) >= 0 && tmay[at])
          base = SRA_UNKNOWN; /* may hold an address the walk did not resolve */
        else if (!a.is_llocal && (w = sra_var_pos(a, nvar)) >= 0)
        {
          if (vcurgen[w] == gen)
            base = vcur[w], broot = vcurroot[w];
          else if (!vmay[w])
            base = SRA_NOT_ADDR;
          else if (vglob[w] != SRA_NO_ADDR && vglob[w] != SRA_VMIX)
            base = vglob[w], broot = sra_find_unit(u, nu, vglob[w]);
          else
            base = SRA_UNKNOWN;
          src_var = w;
        }
        if (base >= SRA_FIRST_ADDR && broot >= 0)
        {
          if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
            base = sra_offset_by(ir, q, base);
          if (base == SRA_VMIX)
            val = SRA_UNKNOWN; /* a variable offset of an address: pinned below */
          else
            val = base, root = broot;
        }
        else
          val = base == SRA_UNKNOWN ? SRA_UNKNOWN : SRA_NOT_ADDR;
      }
      /* Reads of VARs that may hold an address. */
      for (int s2 = 0; s2 < 4; s2++)
      {
        IROperand o = sra_operand(ir, q, s2);
        int v = sra_var_pos(o, nvar);
        if (v < 0 || (s2 == 0 && ((q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED && !o.is_llocal) ||
                                  sra_var_def(ir, q, nvar) == v)))
          continue; /* a definition, not a read */
        int32_t st = vcurgen[v] == gen ? vcur[v]
                     : !vmay[v]       ? SRA_NOT_ADDR
                     : vglob[v] != SRA_NO_ADDR && vglob[v] != SRA_VMIX ? vglob[v]
                                                                         : SRA_UNKNOWN;
        if (st == SRA_NOT_ADDR)
          continue;
        if (st != SRA_UNKNOWN && s2 == 1 && v == src_var && val >= SRA_FIRST_ADDR)
          continue; /* a followed copy or offset */
        if (!vbad[v])
        {
          vbad[v] = st == SRA_UNKNOWN ? 1 : 2;
          vbad_op[v] = (int16_t)q->op;
          vbad_at[v] = i;
        }
      }
      if (vd >= 0)
      {
        vcur[vd] = val, vcurroot[vd] = root, vcurgen[vd] = gen;
        if (val >= SRA_FIRST_ADDR)
          vdef[i] = val, vdefroot[i] = root;
      }
      else if (td >= 0 && toff[td] == SRA_NO_ADDR && val >= SRA_FIRST_ADDR)
        toff[td] = val, troot[td] = root, tfrom[td] = -1;
    }
    tcc_free(bs);
    tcc_free(tmay);
    tcc_free(entry);
    tcc_free(pv);
    if (cfg)
      tcc_ir_cfg_free(cfg);
    /* A VAR pinned: every object it held keeps its memory. */
    for (int i = 0; i < n; i++)
    {
      int v = vdef[i] != SRA_NO_ADDR ? sra_var_def(ir, &ir->compact_instructions[i], nvar) : -1;
      if (v >= 0 && vbad[v] && vdefroot[i] >= 0)
      {
        u[vdefroot[i]].bad = 1;
        sra_dbg_at = vbad[v] == 3 ? -1 : vbad_at[v];
        sra_why(&u[vdefroot[i]], vbad[v] == 1 ? "ptrvar-unknown" : vbad[v] == 2 ? "ptrvar-use" : "ptrvar-addrtaken",
                vbad_op[v]);
        sra_dbg_at = -1;
      }
    }
  }

  /* Calls by id: the instruction, arguments 0-2, and whether it is a copy
   * helper -- for the calls that access an object whole (SraWhole). */
  const int nwcall = ir->next_call_id + 1;
  int32_t *call_at = tcc_malloc(sizeof(int32_t) * nwcall);
  int32_t(*cparam)[3] = tcc_malloc(sizeof(*cparam) * nwcall);
  uint8_t *ckind = tcc_mallocz(nwcall);
  for (int c = 0; c < nwcall; c++)
    call_at[c] = cparam[c][0] = cparam[c][1] = cparam[c][2] = -1;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_FUNCPARAMVAL)
    {
      uint32_t e = (uint32_t)tcc_ir_op_src2_imm(ir, q);
      int c = TCCIR_DECODE_CALL_ID(e), p = TCCIR_DECODE_PARAM_IDX(e);
      if (c >= 0 && c < nwcall && p >= 0 && p < 3)
        cparam[c][p] = cparam[c][p] == -1 ? i : -2;
    }
    else if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
    {
      int c = TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, q));
      if (c >= 0 && c < nwcall)
      {
        call_at[c] = call_at[c] == -1 ? i : INT32_MAX; /* twice: never matches */
        ckind[c] = (uint8_t)sra_call_kind(ir, q);
      }
    }
  }
  SraWhole *whole = NULL;
  int nwhole = 0;

  /* A use other than as an access base or the source of a derived address
   * lets the address escape. */
  uint8_t *aok = sra_addr_alu() ? sra_addr_only_temps(ir, ntemp) : NULL;
  for (int i = 0; i < n && ntemp > 0; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int disp, dt = sra_addr_def(ir, q, ntemp);
    IROperand acc;
    int bt = sra_access_through(ir, q, ntemp, &disp, &acc);
    int base_slot = q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ? 0 : 1;
    for (int s = 0; s < 4; s++)
    {
      IROperand po = sra_operand(ir, q, s);
      int t = sra_temp_pos(po, ntemp);
      if (t < 0 || toff[t] == SRA_NO_ADDR)
        continue;
      if (s == 0 && dt == t)
        continue; /* its own definition */
      if (s == base_slot && bt == t)
        continue;
      if (s == 1 && dt >= 0 && tfrom[dt] == t)
        continue;
      if (s == 1 && vdef && vdef[i] != SRA_NO_ADDR)
        continue; /* derives a pointer VAR's address */
      /* `(*T) + x` reading the field at T's address, when the sum feeds
       * nothing but memory accesses: the offset-0 load of such a chain. */
      if (s == 1 && (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && po.is_lval && !po.is_llocal &&
          !po.is_complex && sra_btype_bytes(irop_get_btype(po)) == 4 &&
          !tcc_ir_access_is_volatile(ir, po) && sra_addr_arith(ir, q, ntemp, aok))
      {
        sra_note_field(&u[troot[t]], toff[t], 4, po.is_unsigned, 0);
        u[troot[t]].refs++;
        u[troot[t]].ok_refs++;
        continue;
      }
      int pw;
      if (s == 1 && q->op == TCCIR_OP_FUNCPARAMVAL && struct_param && troot[t] >= 0 &&
          toff[t] == u[troot[t]].lo && po.is_lval && (pw = sra_struct_param_words(po)) > 0 &&
          !tcc_ir_access_is_volatile(ir, po))
      {
        /* A by-value struct argument read through the object's address. */
        if (pw > u[troot[t]].param_words)
          u[troot[t]].param_words = pw;
        continue;
      }
      SraWhole w;
      if (s == 1 && q->op == TCCIR_OP_FUNCPARAMVAL && troot[t] >= 0 &&
          sra_whole_use(ir, i, call_at, (const int32_t(*)[3])cparam, ckind, nwcall, &w))
      {
        int ok = 1;
        if (w.sret)
        {
          /* The buffer's address must still be computed for the call: from
           * the slot itself, through TEMPs only. */
          int r = t;
          while (tfrom[r] >= 0)
            r = tfrom[r];
          IRQuadCompact *rq = def_idx[r] >= 0 ? &ir->compact_instructions[def_idx[r]] : NULL;
          IROperand a = rq ? tcc_ir_op_get_src1(ir, rq) : IROP_NONE;
          int aoff;
          if (!rq || (rq->op != TCCIR_OP_LEA && rq->op != TCCIR_OP_ASSIGN) || a.is_lval || a.is_llocal ||
              !sra_concrete(a, &aoff))
            ok = 0;
          else
            w.slot = a;
        }
        else
        {
          w.other_t = sra_temp_pos(tcc_ir_op_get_src1(ir, &ir->compact_instructions[w.other]), ntemp);
          if (w.other_t >= 0 && toff[w.other_t] != SRA_NO_ADDR && troot[w.other_t] == troot[t])
            ok = 0; /* within one object */
        }
        if (ok)
        {
          w.unit = troot[t];
          w.off = toff[t];
          w.t = t;
          whole = tcc_realloc(whole, sizeof(SraWhole) * (nwhole + 1));
          whole[nwhole++] = w;
          continue;
        }
      }
      if (!tbad[t] && tbad_op)
        tbad_op[t] = (int16_t)(q->op * 4 + s), tbad_at[t] = i; /* TCC_SRA_DBG: op and operand slot */
      tbad[t] = 1;
    }
  }
  for (int changed = 1; changed && ntemp > 0;)
  {
    changed = 0;
    for (int t = 0; t < ntemp; t++)
      if (tbad[t] && tfrom[t] >= 0 && !tbad[tfrom[t]])
      {
        tbad[tfrom[t]] = 1, changed = 1;
        if (tbad_op)
          tbad_op[tfrom[t]] = tbad_op[t], tbad_at[tfrom[t]] = tbad_at[t];
      }
  }
  /* A root TEMP copied from a pointer VAR has no slot operand to carry the
   * escape into the object's reference count: pin the object directly. */
  for (int t = 0; nvar && t < ntemp; t++)
    if (tbad[t] && tfrom[t] < 0 && toff[t] != SRA_NO_ADDR && troot[t] >= 0 && def_idx[t] >= 0)
    {
      int off;
      IROperand a = tcc_ir_op_get_src1(ir, &ir->compact_instructions[def_idx[t]]);
      if (!sra_concrete(a, &off))
      {
        u[troot[t]].bad = 1;
        sra_dbg_at = tbad_at ? tbad_at[t] : -1;
        sra_why(&u[troot[t]], "ptrvar-escape", tbad_op ? tbad_op[t] : 0);
        sra_dbg_at = -1;
      }
    }

  /* Every reference a live instruction makes, the fourth operand slot of
   * MLA / indexed accesses / SELECT included: an object keeps its memory
   * unless all of them can be replaced. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int dt = ntemp > 0 ? sra_addr_def(ir, q, ntemp) : -1;
    for (int s = 0; s < 4; s++)
    {
      IROperand o = sra_operand(ir, q, s);
      int off, k;
      if (!sra_concrete(o, &off) || (k = sra_find_unit(u, nu, off)) < 0)
        continue;
      u[k].refs++;
      if (s == 1 && vdef && vdef[i] != SRA_NO_ADDR)
      {
        u[k].ok_refs++; /* a pointer VAR's root; sra's walk pinned it if it escapes */
        continue;
      }
      if (s == 1 && dt >= 0 && toff[dt] != SRA_NO_ADDR && tfrom[dt] < 0)
      {
        if (!tbad[dt])
          u[k].ok_refs++; /* the root of an address used only for field accesses */
        else
        {
          sra_dbg_at = tbad_at ? tbad_at[dt] : -1;
          sra_why(&u[k], "addr-escape", tbad_op ? tbad_op[dt] : q->op);
          sra_dbg_at = -1;
        }
        continue;
      }
      if (s < 3 && !no_wide && sra_wide_access(ir, q, s, o))
      {
        sra_note_wide(&u[k], off);
        u[k].ok_refs++;
        continue;
      }
      int pw;
      if (s == 1 && q->op == TCCIR_OP_FUNCPARAMVAL && struct_param && off == u[k].lo &&
          (pw = sra_struct_param_words(o)) > 0 && !tcc_ir_access_is_volatile(ir, o))
      {
        if (pw > u[k].param_words)
          u[k].param_words = pw;
        u[k].ok_refs++;
        continue;
      }
      SraWhole w;
      if (s == 1 && q->op == TCCIR_OP_FUNCPARAMVAL && !o.is_lval && !o.is_llocal &&
          sra_whole_use(ir, i, call_at, (const int32_t(*)[3])cparam, ckind, nwcall, &w))
      {
        if (!w.sret)
        {
          w.other_t = ntemp > 0 ? sra_temp_pos(tcc_ir_op_get_src1(ir, &ir->compact_instructions[w.other]), ntemp) : -1;
          if (w.other_t >= 0 && toff[w.other_t] != SRA_NO_ADDR && troot[w.other_t] == k)
            w.other_t = -2; /* within one object */
        }
        if (w.other_t != -2)
        {
          w.unit = k;
          w.off = off;
          w.slot = o;
          whole = tcc_realloc(whole, sizeof(SraWhole) * (nwhole + 1));
          whole[nwhole++] = w;
          u[k].ok_refs++;
          continue;
        }
      }
      int bytes = s < 3 ? sra_slot_bytes(ir, q->op, s, o) : 0;
      /* A field read inside address arithmetic (`T <-- slot ADD index`),
       * when the sum feeds nothing but memory accesses: the base pointer
       * the field holds, which the loop below would reload each pass. */
      if (!bytes && s >= 1 && s <= 2 && o.is_lval && !o.is_llocal && !o.is_complex &&
          !tcc_ir_access_is_volatile(ir, o) && sra_addr_arith(ir, q, ntemp, aok))
      {
        int fb = sra_btype_bytes(irop_get_btype(o));
        if (fb && fb <= 4)
          bytes = fb;
      }
      if (bytes && (irop_get_btype(o) != IROP_BTYPE_STRUCT || off == u[k].lo))
      {
        if (bytes < 4 && s == 0 && q->op == TCCIR_OP_STORE && off - u[k].lo >= 0 && off - u[k].lo < SRA_MAX_BYTES)
        {
          if (!sra_narrow_store_fits(ir, q, bytes, def_idx, ntemp))
            u[k].nofit[off - u[k].lo] |= (uint8_t)bytes;
          if (!irop_is_plain_imm(tcc_ir_op_get_src1(ir, q)))
          {
            u[k].stvar[off - u[k].lo] |= (uint8_t)bytes;
            (bytes == 1 ? u[k].nvst1 : u[k].nvst2)[off - u[k].lo]++;
          }
        }
        sra_note_field(&u[k], off, bytes, o.is_unsigned, s == 0);
        u[k].ok_refs++;
      }
      else
      {
        sra_dbg_at = i;
        sra_why(&u[k], s == 3 ? "slot3" : bytes ? "struct-offset" : sra_ref_kind(q->op, s, o), q->op);
        sra_dbg_at = -1;
      }
    }
    int disp;
    IROperand acc;
    int t = ntemp > 0 ? sra_access_through(ir, q, ntemp, &disp, &acc) : -1;
    if (t >= 0 && toff[t] != SRA_NO_ADDR && !tbad[t] && troot[t] >= 0)
    {
      int off = toff[t] + disp, k = sra_find_unit(u, nu, off);
      if (k != troot[t])
      {
        u[troot[t]].bad = 1; /* strays outside the object */
        sra_why(&u[troot[t]], "stray", q->op);
      }
      else
      {
        u[k].refs++;
        u[k].ok_refs++;
        int abytes = sra_btype_bytes(irop_get_btype(acc));
        /* A narrow store must already fit its field (sra_narrow_store_fits).
         * An indexed one by a constant is the same store: its value is src1
         * too, and the ASSIGN it becomes reads only dest and src1. */
        if (abytes < 4 && (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) && sra_narrow_idx_off() &&
            q->op == TCCIR_OP_STORE_INDEXED)
        {
          u[k].bad = 1;
          sra_why(&u[k], "narrow-store", q->op);
        }
        else if (abytes < 4 && (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) && off - u[k].lo >= 0 &&
                 off - u[k].lo < SRA_MAX_BYTES)
        {
          if (!sra_narrow_store_fits(ir, q, abytes, def_idx, ntemp))
            u[k].nofit[off - u[k].lo] |= (uint8_t)abytes;
          if (!irop_is_plain_imm(tcc_ir_op_get_src1(ir, q)))
          {
            u[k].stvar[off - u[k].lo] |= (uint8_t)abytes;
            (abytes == 1 ? u[k].nvst1 : u[k].nvst2)[off - u[k].lo]++;
          }
        }
        sra_note_field(&u[k], off, abytes, acc.is_unsigned,
                       q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED);
      }
    }
  }
  {
    uint8_t *has_whole = tcc_mallocz(nu);
    for (int x = 0; x < nwhole; x++)
      has_whole[whole[x].unit] = 1;
    for (int k = 0; k < nu; k++)
      if (!u[k].bad)
      {
        sra_resolve_fields(&u[k], has_whole[k]);
        sra_partial_cost(&u[k], has_whole[k]);
      }
    tcc_free(has_whole);
  }
  for (int k = 0; k < nu; k++)
    if (!u[k].bad)
      sra_resolve_wide(&u[k]);
  /* Ranges accessed whole: inside the object, with a field for each byte a
   * call writes and a later whole read passes on, and no field across an
   * end. */
  for (int x = 0; x < nwhole; x++)
  {
    SraUnit *uk = &u[whole[x].unit];
    int a0 = whole[x].off - uk->lo;
    if (a0 < 0 || a0 + whole[x].n > uk->hi - uk->lo || a0 + whole[x].n > SRA_MAX_BYTES)
    {
      uk->bad = 1;
      sra_why(uk, "whole-range", 0);
    }
    else if (whole[x].dir)
      uk->whole_r = 1;
    else
      uk->whole_w = 1;
  }
  for (int x = 0; x < nwhole; x++)
  {
    SraUnit *uk = &u[whole[x].unit];
    if (!uk->bad && !whole[x].dir && (uk->whole_r || uk->param_words))
      sra_fill_gaps(uk, whole[x].off - uk->lo, whole[x].n);
  }
  for (int x = 0; x < nwhole; x++)
  {
    SraUnit *uk = &u[whole[x].unit];
    if (!uk->bad && !sra_range_clean(uk, whole[x].off - uk->lo, whole[x].n))
    {
      uk->bad = 1;
      sra_why(uk, "whole-straddle", 0);
    }
  }
  /* Worth it only when the field copies (a load or store per field) cost no
   * more than the block copies they replace (an LDM/STM pair per 16 bytes
   * and the setup), plus the field accesses that stop going to memory. */
  if (nwhole)
  {
    int32_t *wcost = tcc_mallocz(sizeof(int32_t) * nu);
    for (int x = 0; x < nwhole; x++)
    {
      SraUnit *uk = &u[whole[x].unit];
      if (uk->bad)
        continue;
      int a0 = whole[x].off - uk->lo, fields = 0;
      for (int at = a0; at < a0 + whole[x].n; at++)
        fields += uk->width[at] == 8 ? 2 : uk->width[at] ? 1 : 0;
      wcost[whole[x].unit] += fields - (whole[x].sret ? 0 : 2 + 2 * ((whole[x].n + 15) / 16));
    }
    for (int k = 0; k < nu; k++)
      if (!u[k].bad && (u[k].whole_w || u[k].whole_r) && wcost[k] > u[k].field_refs)
      {
        u[k].bad = 1;
        sra_why(&u[k], "whole-cost", 0);
      }
    tcc_free(wcost);
  }
  for (int k = 0; k < nu; k++)
  {
    if (!sra_fields_disjoint(&u[k]))
      sra_why(&u[k], "overlap", 0);
    else if (!sra_narrow_fields_unsigned(&u[k]))
      sra_why(&u[k], "narrow-signed", 0);
    if (u[k].ok_refs != u[k].refs || !sra_fields_disjoint(&u[k]) || !sra_narrow_fields_unsigned(&u[k]))
      u[k].bad = 1;
    /* Each word a call receives by value must be one field VAR: a 32-bit
     * field, or one narrow unsigned field at its start (its VAR holds it
     * zero-extended; the other bytes are padding or never written, so what
     * the callee sees there is indeterminate either way).  No 64-bit field. */
    for (int w = 0; w < u[k].param_words && !u[k].bad; w++)
    {
      int fields = 0;
      for (int b = 4 * w; b < 4 * w + 4; b++)
      {
        if (u[k].has64[b] || u[k].split[b] || u[k].width[b] > 4)
          fields += 2;
        else if (u[k].width[b])
          fields += b == 4 * w ? 1 : 2;
      }
      if (fields > 1)
      {
        u[k].bad = 1;
        sra_why(&u[k], "param-word", TCCIR_OP_FUNCPARAMVAL);
      }
    }
  }
  /* A copy between two objects that would both be replaced becomes copies
   * between their field VARs when the two lay their fields out alike over
   * the copied bytes (a field of one is never split across two of the
   * other's); otherwise one of them keeps its memory. */
  for (int x = 0; x < nwhole; x++)
  {
    int ot = whole[x].other_t, k = whole[x].unit;
    if (!whole[x].sret && ot >= 0 && toff[ot] != SRA_NO_ADDR && troot[ot] >= 0 && !u[troot[ot]].bad && !u[k].bad)
    {
      const int k2 = troot[ot], a1 = whole[x].off - u[k].lo, a2 = toff[ot] - u[k2].lo, n2 = whole[x].n;
      int alike = !sra_pair_off() && a2 >= 0 && a2 + n2 <= u[k2].hi - u[k2].lo && a2 + n2 <= SRA_MAX_BYTES &&
                  sra_range_clean(&u[k2], a2, n2);
      for (int d = 0; alike && d < n2; d++)
        alike = u[k].width[a1 + d] == u[k2].width[a2 + d];
      if (alike)
      {
        whole[x].pair = k2;
        whole[x].pair_off = toff[ot];
        continue;
      }
      u[k].bad = 1;
      sra_why(&u[k], "copy-pair", 0);
    }
  }
  /* The function's field VARs, against the budget SSA promotion has. */
  {
    int nvars = 0;
    for (int k = 0; k < nu; k++)
      for (int at = 0; !u[k].bad && at < SRA_MAX_BYTES; at++)
        nvars += u[k].width[at] ? 1 : 0;
    if (nvars > sra_var_budget())
    {
      for (int k = 0; k < nu; k++)
        if (!u[k].bad && u[k].partial)
        {
          u[k].bad = 1;
          sra_why(&u[k], "budget", 0);
        }
      for (int x = 0; x < nwhole; x++)
        if (whole[x].pair >= 0 && !u[whole[x].unit].bad && !u[whole[x].pair].bad)
        {
          u[whole[x].unit].bad = 1;
          sra_why(&u[whole[x].unit], "budget", 0);
        }
    }
  }
  TCC_DBG_BLOCK(sra_dbg)
  {
    extern const char *funcname;
    for (int k = 0; k < nu; k++)
    {
      const char *callee = "-";
      int pidx = -1;
      int at = u[k].bad && u[k].why ? u[k].why_at : -1;
      if (at >= 0 && at < n && ir->compact_instructions[at].op == TCCIR_OP_FUNCPARAMVAL)
      {
        uint32_t e = (uint32_t)tcc_ir_op_src2_imm(ir, &ir->compact_instructions[at]);
        int c = TCCIR_DECODE_CALL_ID(e);
        pidx = TCCIR_DECODE_PARAM_IDX(e);
        for (int j = at + 1; j < n; j++)
        {
          IRQuadCompact *cq = &ir->compact_instructions[j];
          if ((cq->op == TCCIR_OP_FUNCCALLVAL || cq->op == TCCIR_OP_FUNCCALLVOID) &&
              TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, cq)) == c)
          {
            Sym *cs = tcc_ir_op_src1_sym(ir, cq);
            callee = cs ? get_tok_str(cs->v, NULL) : "(indirect)";
            break;
          }
        }
      }
      fprintf(stderr, "[SRA] %s size=%d refs=%d %s op=%d callee=%s param=%d lo=%d acc=", funcname, u[k].hi - u[k].lo,
              u[k].refs, !u[k].bad ? "PROMOTED" : u[k].why ? u[k].why : "bad", u[k].why_op, callee, pidx, u[k].lo);
      for (int at = 0; at < SRA_MAX_BYTES; at++)
        if (u[k].acc[at] || u[k].has64[at])
          fprintf(stderr, "%d:L%d/S%d%s,", at, u[k].acc[at] & 7, u[k].acc[at] >> 3, u[k].has64[at] ? "/W" : "");
      fprintf(stderr, " whole=%d%d pw=%d pad=%llx\n", u[k].whole_w, u[k].whole_r, u[k].param_words,
              (unsigned long long)u[k].pad);
    }
  }

  for (int k = 0; k < nu; k++)
    if (!u[k].bad)
      sra_index_fields(&u[k]);

  int changes = 0;
  SraInsList ins = {0};

  /* By-value struct arguments of replaced objects become one argument per
   * word: the call's later arguments shift up and its argc grows.  Collected
   * first, so every index is final when an instruction is rewritten. */
  const int ncall = ir->next_call_id + 1;
  int32_t *call_extra = NULL; /* per call: extra arguments in total */
  int32_t *split_first = NULL;  /* per call: its first split argument in splits[], or -1 */
  int32_t(*splits)[3] = NULL;   /* { argument index, extra words, next in this call } */
  int nsplit = 0;
  if (struct_param)
  {
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      IROperand o;
      int off, k, pw;
      if (q->op != TCCIR_OP_FUNCPARAMVAL || !sra_param_at(o = tcc_ir_op_get_src1(ir, q), ntemp, toff, &off) ||
          (k = sra_find_unit(u, nu, off)) < 0 || u[k].bad || off != u[k].lo || !(pw = sra_struct_param_words(o)))
        continue;
      int c = TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, q));
      if (c < 0 || c >= ncall)
        continue;
      if (!call_extra)
      {
        call_extra = tcc_mallocz(sizeof(int32_t) * ncall);
        split_first = tcc_malloc(sizeof(int32_t) * ncall);
        for (int x = 0; x < ncall; x++)
          split_first[x] = -1;
      }
      call_extra[c] += pw - 1;
      splits = tcc_realloc(splits, sizeof(*splits) * (nsplit + 1));
      splits[nsplit][0] = TCCIR_DECODE_PARAM_IDX((uint32_t)tcc_ir_op_src2_imm(ir, q));
      splits[nsplit][1] = pw - 1;
      splits[nsplit][2] = split_first[c];
      split_first[c] = nsplit++;
    }
  }

  /* Whole-range calls of replaced objects: a copy becomes its field copies, a
   * result buffer keeps its address and is read back into the fields. */
  uint8_t *tkeep = ntemp > 0 ? tcc_mallocz(ntemp) : NULL;
  for (int x = 0; x < nwhole; x++)
  {
    SraWhole *e = &whole[x];
    if (u[e->unit].bad)
      continue;
    if (e->sret)
    {
      for (int t = e->t; t >= 0; t = tfrom[t])
        tkeep[t] = 1;
      sra_expand_whole(ir, &ins, &u[e->unit], e, IROP_NONE);
    }
    else if (e->pair >= 0 && !u[e->pair].bad)
    {
      /* Field VARs of the source to those of the destination; the entry of
       * the side the call reads adds nothing. */
      if (e->dir == 0)
        sra_expand_pair(ir, &ins, &u[e->unit], e->off - u[e->unit].lo, &u[e->pair], e->pair_off - u[e->pair].lo, e->n,
                        e->call);
      for (int k = 0; k < 3; k++)
        ir->compact_instructions[cparam[e->cid][k]].op = TCCIR_OP_NOP;
      ir->compact_instructions[e->call].op = TCCIR_OP_NOP;
    }
    else
    {
      IROperand base = tcc_ir_op_get_src1(ir, &ir->compact_instructions[e->other]);
      sra_expand_whole(ir, &ins, &u[e->unit], e, base);
      for (int k = 0; k < 3; k++)
        ir->compact_instructions[cparam[e->cid][k]].op = TCCIR_OP_NOP;
      ir->compact_instructions[e->call].op = TCCIR_OP_NOP;
    }
    changes++;
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    /* Address arithmetic of a replaced object: nothing reads it any more. */
    if (vdef && vdef[i] != SRA_NO_ADDR && vdefroot[i] >= 0 && !u[vdefroot[i]].bad)
    {
      q->op = TCCIR_OP_NOP;
      changes++;
      continue;
    }
    int dt = ntemp > 0 ? sra_addr_def(ir, q, ntemp) : -1;
    if (dt >= 0 && toff[dt] != SRA_NO_ADDR && troot[dt] >= 0 && !tbad[dt] && !u[troot[dt]].bad && !tkeep[dt])
    {
      q->op = TCCIR_OP_NOP;
      changes++;
      continue;
    }
    int disp;
    IROperand acc;
    int t = ntemp > 0 ? sra_access_through(ir, q, ntemp, &disp, &acc) : -1;
    if (t >= 0 && toff[t] != SRA_NO_ADDR && !tbad[t] && troot[t] >= 0 && !u[troot[t]].bad)
    {
      int k = troot[t], at = toff[t] + disp - u[k].lo;
      int is_store = q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED;
      const int abytes = sra_btype_bytes(irop_get_btype(acc));
      const int fs = u[k].fstart[at];
      if (fs != 0xFF && (fs != at || (is_store && abytes < u[k].width[fs]) ||
                         (!is_store && abytes == u[k].width[fs] && u[k].pmask[fs])))
      {
        /* Part of a wider field (sra_resolve_fields). */
        if (is_store && sra_partial_store_padded(ir, i, &u[k], fs, at - fs, abytes, tcc_ir_op_get_src1(ir, q)))
          ;
        else if (is_store && sra_bfi() && !irop_is_plain_imm(tcc_ir_op_get_src1(ir, q)))
          sra_partial_store_bfi(ir, i, &u[k], fs, at - fs, abytes, tcc_ir_op_get_src1(ir, q));
        else if (is_store)
          sra_partial_store(ir, &ins, i, &u[k], fs, at - fs, abytes, tcc_ir_op_get_src1(ir, q),
                            !(u[k].nofit[at] & abytes));
        else
          sra_partial_load(ir, &ins, i, &u[k], fs, at - fs, abytes, acc.is_unsigned, tcc_ir_op_get_dest(ir, q));
        changes++;
        continue;
      }
      if (u[k].var[at] < 0)
        u[k].var[at] = tcc_ir_vreg_alloc_var(ir);
      IRLiveInterval *iv = tcc_ir_get_live_interval(ir, u[k].var[at]);
      const int narrow = u[k].width[at] && u[k].width[at] < 4;
      const int extract = !is_store && abytes && abytes < u[k].width[at] && u[k].width[at] <= 4;
      const int rbt = narrow || extract ? IROP_BTYPE_INT32 : irop_get_btype(acc);
      IROperand rep = irop_make_stackoff(u[k].var[at], iv ? iv->original_offset : 0, 1, 0, 0, rbt);
      rep.is_unsigned = narrow || extract ? 1 : acc.is_unsigned;
      rep.aux = IROP_AUX_NONVOLATILE;
      if (extract)
      {
        sra_extract(ir, &ins, i, rep, abytes, acc.is_unsigned);
        changes++;
        continue;
      }
      if (!is_store && narrow && sra_covers_more(&u[k], at, abytes))
      {
        IROperand v = sra_combine(ir, &ins, i, &u[k], at, abytes, acc.is_unsigned);
        q = &ir->compact_instructions[i];
        sra_relay(ir, q, TCCIR_OP_ASSIGN, tcc_ir_op_get_dest(ir, q), v, IROP_NONE);
        changes++;
        continue;
      }
      if (is_store && narrow)
      {
        sra_store_as_def(ir, i, rep);
        changes++;
        continue;
      }
      q->op = is_store ? TCCIR_OP_STORE : TCCIR_OP_LOAD;
      if (is_store)
        tcc_ir_set_dest(ir, i, rep);
      else
        tcc_ir_set_src1(ir, i, rep);
      changes++;
      continue;
    }
    /* Address arithmetic reading a field through an address TEMP:
     * `T9 <-- (*T8) ADD x` becomes `T9 <-- V_field ADD x`. */
    if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && sra_addr_arith(ir, q, ntemp, aok))
    {
      for (int s2 = 1; s2 <= 2; s2++)
      {
        IROperand o2 = sra_operand(ir, q, s2);
        int t2 = sra_temp_pos(o2, ntemp);
        if (t2 < 0 || !o2.is_lval || o2.is_llocal || o2.is_complex || sra_btype_bytes(irop_get_btype(o2)) != 4 ||
            toff[t2] == SRA_NO_ADDR || troot[t2] < 0 || tbad[t2] || u[troot[t2]].bad ||
            (tkeep && tkeep[t2]))
          continue;
        int k2 = troot[t2], at2 = toff[t2] - u[k2].lo;
        if (at2 < 0 || at2 >= SRA_MAX_BYTES || !u[k2].width[at2])
          continue;
        IROperand rep = sra_field_op(ir, &u[k2], at2, IROP_BTYPE_INT32, 1);
        if (s2 == 1)
          tcc_ir_set_src1(ir, i, rep);
        else
          tcc_ir_set_src2(ir, i, rep);
        changes++;
      }
      /* fall through: a raw-slot operand of the same ADD is swapped by the
       * slot loop below. */
    }
    /* A call with split arguments takes the extra words in its argc. */
    if (call_extra && (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID))
    {
      IROperand enc = tcc_ir_op_get_src2(ir, q);
      uint32_t e = (uint32_t)irop_get_imm64_ex(ir, enc);
      int c = TCCIR_DECODE_CALL_ID(e);
      if (c >= 0 && c < ncall && call_extra[c])
      {
        enc.u.imm32 = (int32_t)TCCIR_ENCODE_CALL(c, TCCIR_DECODE_CALL_ARGC(e) + call_extra[c]);
        tcc_ir_set_src2(ir, i, enc);
        changes++;
      }
    }
    /* Its arguments: a split one becomes its words, the later ones shift up. */
    if (call_extra && q->op == TCCIR_OP_FUNCPARAMVAL)
    {
      IROperand enc = tcc_ir_op_get_src2(ir, q);
      uint32_t e = (uint32_t)irop_get_imm64_ex(ir, enc);
      int c = TCCIR_DECODE_CALL_ID(e);
      if (c >= 0 && c < ncall && call_extra[c])
      {
        /* This argument's new index: every split argument ahead of it adds
         * its extra words.  The frontend numbers a call's arguments 0..argc-1,
         * each once, so scanning for the lower ones is enough. */
        int idx = TCCIR_DECODE_PARAM_IDX(e), shift = 0;
        for (int x = split_first[c]; x >= 0; x = splits[x][2])
          if (splits[x][0] < idx)
            shift += splits[x][1];
        IROperand o = tcc_ir_op_get_src1(ir, q);
        int off, k, pw;
        if (sra_param_at(o, ntemp, toff, &off) && (k = sra_find_unit(u, nu, off)) >= 0 && !u[k].bad &&
            off == u[k].lo && (pw = sra_struct_param_words(o)))
        {
          /* Word w of the struct, from its field VAR (0 when no field). */
          for (int w = 0; w < pw; w++)
          {
            IROperand wv;
            if (u[k].width[4 * w])
            {
              if (u[k].var[4 * w] < 0)
                u[k].var[4 * w] = tcc_ir_vreg_alloc_var(ir);
              IRLiveInterval *iv = tcc_ir_get_live_interval(ir, u[k].var[4 * w]);
              wv = irop_make_stackoff(u[k].var[4 * w], iv ? iv->original_offset : 0, 1, 0, 0, IROP_BTYPE_INT32);
              wv.is_unsigned = 1;
              wv.aux = IROP_AUX_NONVOLATILE;
            }
            else
              wv = irop_make_imm32(-1, 0, IROP_BTYPE_INT32);
            IROperand we = enc;
            we.u.imm32 = (int32_t)TCCIR_ENCODE_PARAM(c, idx + shift + w);
            if (w == 0)
            {
              tcc_ir_set_src1(ir, i, wv);
              tcc_ir_set_src2(ir, i, we);
            }
            else
              sra_ins_add(ir, &ins, i, 1, TCCIR_OP_FUNCPARAMVAL, wv, wv, we);
          }
          changes++;
          continue;
        }
        if (shift)
        {
          enc.u.imm32 = (int32_t)TCCIR_ENCODE_PARAM(c, idx + shift);
          tcc_ir_set_src2(ir, i, enc);
          changes++;
        }
      }
    }
    for (int s = 0; s < 3; s++)
    {
      IROperand o = sra_operand(ir, q, s);
      int off, k;
      if (!sra_concrete(o, &off) || (k = sra_find_unit(u, nu, off)) < 0 || u[k].bad)
        continue;
      if (!o.is_lval)
        continue; /* a result buffer's address, or a kept TEMP's root */
      int at = off - u[k].lo;
      if (irop_get_btype(o) == IROP_BTYPE_INT64 && (u[k].width[at] == 8 || u[k].split[at]))
      {
        sra_rewrite_wide(ir, &ins, i, s, o, &u[k], at);
        changes++;
        continue;
      }
      const int obytes = sra_btype_bytes(irop_get_btype(o));
      const int fs = u[k].fstart[at];
      if (fs != 0xFF && (fs != at || (s == 0 && q->op == TCCIR_OP_STORE && obytes && obytes < u[k].width[fs]) ||
                         (s == 1 && (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ASSIGN) && obytes &&
                          obytes == u[k].width[fs] && u[k].pmask[fs])))
      {
        /* Part of a wider field (sra_resolve_fields). */
        if (s == 0 && q->op == TCCIR_OP_STORE &&
            sra_partial_store_padded(ir, i, &u[k], fs, at - fs, obytes, tcc_ir_op_get_src1(ir, q)))
          ;
        else if (s == 0 && sra_bfi() && !irop_is_plain_imm(tcc_ir_op_get_src1(ir, q)))
          sra_partial_store_bfi(ir, i, &u[k], fs, at - fs, obytes, tcc_ir_op_get_src1(ir, q));
        else if (s == 0)
          sra_partial_store(ir, &ins, i, &u[k], fs, at - fs, obytes, tcc_ir_op_get_src1(ir, q),
                            !(u[k].nofit[at] & obytes));
        else
          sra_partial_load(ir, &ins, i, &u[k], fs, at - fs, obytes ? obytes : 4, obytes ? o.is_unsigned : 1,
                           tcc_ir_op_get_dest(ir, q));
        changes++;
        break; /* the instruction is rewritten whole */
      }
      if (u[k].var[at] < 0)
        u[k].var[at] = tcc_ir_vreg_alloc_var(ir);
      int32_t v = u[k].var[at];
      const int narrow = u[k].width[at] && u[k].width[at] < 4;
      const int extract = s == 1 && (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ASSIGN) && obytes &&
                          obytes < u[k].width[at] && u[k].width[at] <= 4;
      int bt = narrow || extract || irop_get_btype(o) == IROP_BTYPE_STRUCT ? IROP_BTYPE_INT32 : irop_get_btype(o);
      IRLiveInterval *iv = tcc_ir_get_live_interval(ir, v);
      IROperand rep = irop_make_stackoff(v, iv ? iv->original_offset : 0, 1, 0, 0, bt);
      rep.is_unsigned = narrow || extract || irop_get_btype(o) == IROP_BTYPE_STRUCT ? 1 : o.is_unsigned;
      rep.aux = IROP_AUX_NONVOLATILE;
      if (extract)
      {
        sra_extract(ir, &ins, i, rep, obytes, o.is_unsigned);
        changes++;
        break; /* the instruction is rewritten whole */
      }
      if (s == 1 && narrow && sra_covers_more(&u[k], at, obytes ? obytes : 4))
      {
        /* A LOAD / ASSIGN becomes an ASSIGN of the value; a word struct
         * argument or return value takes it as it is. */
        rep = sra_combine(ir, &ins, i, &u[k], at, obytes ? obytes : 4,
                          obytes ? o.is_unsigned : 1);
        q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_LOAD)
          q->op = TCCIR_OP_ASSIGN;
      }
      if (s == 0 && narrow && q->op == TCCIR_OP_STORE)
      {
        sra_store_as_def(ir, i, rep);
        changes++;
        continue;
      }
      if (s == 0)
        tcc_ir_set_dest(ir, i, rep);
      else if (s == 1)
        tcc_ir_set_src1(ir, i, rep);
      else
        tcc_ir_set_src2(ir, i, rep);
      changes++;
    }
  }

  sra_ins_apply(ir, &ins);
  tcc_free(ins.v);
  tcc_free(tkeep);
  tcc_free(whole);
  tcc_free(call_at);
  tcc_free(cparam);
  tcc_free(ckind);
  tcc_free(call_extra);
  tcc_free(split_first);
  tcc_free(splits);

  tcc_free(vglob);
  tcc_free(vmay);
  tcc_free(vbad);
  tcc_free(vbad_op);
  tcc_free(vbad_at);
  tcc_free(vcur);
  tcc_free(vcurroot);
  tcc_free(vcurgen);
  tcc_free(vdef);
  tcc_free(vdefroot);
  tcc_free(def_idx);
  tcc_free(tbad);
  tcc_free(tbad_op);
  tcc_free(tbad_at);
  tcc_free(troot);
  tcc_free(tfrom);
  tcc_free(toff);
  tcc_free(aok);
  tcc_free(defs);
  tcc_free(u);
  return changes;
}

int tcc_ir_opt_sra(TCCIRState *ir)
{
  if (!ir || !tcc_state || TCC_OPT(tcc_state, optimize) <= 0 || !sra_function_eligible(ir))
    return 0;
  const int first_var = ir->next_local_variable;
  int changes = sra_objects(ir);
  /* The field VARs this run made, for ssa_var_promotable. */
  if (ir->next_local_variable > first_var)
    ir->sra_var_lo = first_var, ir->sra_var_hi = ir->next_local_variable;
  return changes + sra_forward_const_vars(ir, first_var);
}
