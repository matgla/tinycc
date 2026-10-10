/*
 *  TCC - Thumb-2 machine outliner (-Os)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Runs of IR ops that compile to the same machine code in many functions --
 * Zig's error returns, its slice and hash-map accessors -- are emitted once as
 * a shared body and called with BL everywhere else.  The work is done per IR
 * op, never inside one: the rehearsal pass (ir/codegen.c) records the bytes
 * each op produced, a window of consecutive ops whose code is position
 * independent and leaves LR and SP alone is looked up among the windows of the
 * functions compiled before, and a match is emitted as a call.
 *
 * Legality is decided on the machine code itself, by a decoder that
 * recognises the Thumb-2 forms the backend emits and rejects everything else:
 * a branch, a PC-relative access, a write to SP, any use of LR (the BL
 * clobbers it) or of PC.  An op any of whose bytes did not come through ot()
 * -- a literal-pool dump, a branch placeholder -- is rejected as well.
 *
 * The table is filled as the TU compiles, so a window's first occurrences stay
 * inline: the body is created from the rehearsal code of the function whose
 * occurrence makes it pay (outline_body_pays), and that function and every
 * later one call it.  The real pass (pass 2) still generates every op of a
 * window it calls instead: ot() records the instructions rather than writing
 * them, so every backend cache ot() maintains leaves the window exactly as if
 * the code had been inline, and at the window's end they are compared with
 * the body and the BL is written in their place.  Should they differ from
 * what the rehearsal produced -- a cache the pool dumps reset at other points
 * -- they are written instead, where they were generated
 * (TCC_OUTLINE_STATS=1 counts those, =2 prints them).  TCC_OUTLINE_LIMIT=N
 * outlines only the first N windows of the compile, and names the function
 * of the Nth: a miscompile bisects to one window.
 *
 * The real pass can only get shorter than the rehearsal by this, as it does by
 * branch narrowing; tcc_gen_machine_outline_shrink_between() gives CBZ's lower
 * bound the bytes it can lose.  A CMP #0 inside a window is not fused with the
 * branch after it into CBZ (the rehearsal never fuses).
 *
 * Only functions whose prologue saves LR and that run the rehearsal: making a
 * straight-line function run it too found 126 bytes in the Zig compiler. */

#define USING_GLOBALS
#include "tcc.h"
#include "ir.h"

/* ------------------------------------------------------------ capture */

typedef struct
{
  int active;
  int base;         /* ind at the start of the captured pass */
  int cap;          /* halfwords allocated */
  uint16_t *hw;     /* captured halfwords, by (ind - base) / 2 */
  uint8_t *set;     /* 1 where hw[] came through ot() */
  int *own;         /* the IR op that emitted it */
  int nrel, relcap; /* GOT relocations the pass asked for */
  int32_t *rel_off;
  int *rel_esym; /* ELF symbol index, -1 if the symbol has none */
} OutlineCapture;

static OutlineCapture cap;

/* The real pass inside a planned window: its instructions go on advancing
 * `ind` from the window's address, as if emitted, but land in hw[] -- a pair
 * peephole may still rewind over them -- and are compared with the body when
 * the window ends.  Only then is the BL written there, or, should the code
 * differ, the code itself, at the addresses it was generated for. */
static struct
{
  int active;
  int body;
  int end_op; /* first op after the window */
  int vbase;  /* the window's address */
  int cap;
  uint16_t *hw;
  uint8_t *set;
  int nrel, relcap;
  int32_t *rel_off;
  int *rel_esym;
} sup;

static int outline_stats_on(void)
{
  static int v = -1;
  if (v < 0)
  {
    const char *e = getenv("TCC_OUTLINE_STATS");
    v = e ? atoi(e) : 0; /* 2: also print each window that went back inline */
  }
  return v;
}

/* Whether this compile outlines at all: -Os, not disabled, and -- like
 * identical code folding -- not when debugging or instrumenting, which would
 * see code of one function run from another's.  TCC_NO_OUTLINE=1 turns it off. */
ST_FUNC int tcc_gen_machine_outline_enabled(void)
{
  static int off = -1;
  if (off < 0)
    off = getenv("TCC_NO_OUTLINE") != NULL;
  const TCCState *s1 = tcc_state;
  return TCC_OPT(s1, optimize_size) && TCC_OPT(s1, optimize) > 0 && !off && !s1->check_only && !s1->do_debug && !s1->test_coverage &&
         !s1->do_backtrace;
}

ST_FUNC void tcc_gen_machine_outline_capture_begin(void)
{
  cap.active = 1;
  cap.base = ind;
  cap.nrel = 0;
  if (cap.hw)
    memset(cap.set, 0, (size_t)cap.cap);
}

ST_FUNC void tcc_gen_machine_outline_capture_end(void)
{
  cap.active = 0;
}

static void cap_reserve(int n)
{
  if (n <= cap.cap)
    return;
  int nc = cap.cap ? cap.cap : 4096;
  while (nc < n)
    nc *= 2;
  cap.hw = tcc_realloc(cap.hw, sizeof(uint16_t) * (size_t)nc);
  cap.set = tcc_realloc(cap.set, (size_t)nc);
  cap.own = tcc_realloc(cap.own, sizeof(int) * (size_t)nc);
  memset(cap.set + cap.cap, 0, (size_t)(nc - cap.cap));
  cap.cap = nc;
}

/* Called by ot() for every instruction, before it is written (or, in a dry
 * pass, counted). */
ST_FUNC void tcc_gen_machine_outline_capture(uint32_t opcode, int size, int at)
{
  if (!cap.active || at < cap.base || (size != 2 && size != 4))
    return;
  int k = (at - cap.base) / 2;
  cap_reserve(k + 2);
  int op = tcc_state->ir ? tcc_state->ir->codegen_instruction_idx : -1;
  if (size == 4)
  {
    cap.hw[k] = (uint16_t)(opcode >> 16);
    cap.hw[k + 1] = (uint16_t)opcode;
    cap.set[k] = cap.set[k + 1] = 1;
    cap.own[k] = cap.own[k + 1] = op;
  }
  else
  {
    cap.hw[k] = (uint16_t)opcode;
    cap.set[k] = 1;
    cap.own[k] = op;
  }
}

/* The GOT_SBREL12 load of `sym` at `at`.  The key is the ELF symbol, which
 * the real pass would create here anyway: a Sym can be freed with its scope
 * and its address reused by another. */
static void sup_reloc(Sym *sym, int at);

ST_FUNC void tcc_gen_machine_outline_capture_reloc(Sym *sym, int at)
{
  if (sup.active)
    sup_reloc(sym, at);
  if (!cap.active)
    return;
  if (cap.nrel == cap.relcap)
  {
    cap.relcap = cap.relcap ? cap.relcap * 2 : 64;
    cap.rel_off = tcc_realloc(cap.rel_off, sizeof(int32_t) * (size_t)cap.relcap);
    cap.rel_esym = tcc_realloc(cap.rel_esym, sizeof(int) * (size_t)cap.relcap);
  }
  if (sym && sym->c <= 0)
    put_extern_sym(sym, NULL, 0, 0);
  cap.rel_off[cap.nrel] = at - cap.base;
  cap.rel_esym[cap.nrel] = sym && sym->c > 0 ? sym->c : -1;
  cap.nrel++;
}

/* ------------------------------------------------------------ decoder */

#define R_IS_SP_LR_PC(r) ((r) >= 13)

/* Whether the instruction starting with halfword `h1` (and `h2` for a 32-bit
 * one) may run from a shared body reached by BL: no branch, no PC-relative
 * access, no write to SP, no reference to LR or PC.  Anything not positively
 * recognised is rejected. */
static int outline_insn_ok(uint16_t h1, uint16_t h2, int size)
{
  if (size == 2)
  {
    unsigned top5 = h1 >> 11;
    if (top5 <= 0x07) /* shift imm, add/sub reg/imm3, mov/cmp/add/sub imm8: low regs */
      return 1;
    if ((h1 & 0xFC00) == 0x4000) /* data processing, low regs */
      return 1;
    if ((h1 & 0xFC00) == 0x4400) /* ADD/CMP/MOV high, BX/BLX */
    {
      unsigned op = (h1 >> 8) & 3;
      if (op == 3)
        return 0;
      unsigned rdn = ((h1 >> 4) & 8) | (h1 & 7), rm = (h1 >> 3) & 15;
      return !R_IS_SP_LR_PC(rdn) && !R_IS_SP_LR_PC(rm);
    }
    if ((h1 & 0xF800) == 0x4800) /* LDR literal */
      return 0;
    if ((h1 & 0xF000) == 0x5000) /* load/store register offset, low regs */
      return 1;
    if (top5 >= 0x0C && top5 <= 0x11) /* LDR/STR(B/H) imm, low regs */
      return 1;
    if ((h1 & 0xF000) == 0x9000) /* LDR/STR Rt, [SP, #imm]: reads SP */
      return 1;
    if ((h1 & 0xF800) == 0xA000) /* ADR */
      return 0;
    if ((h1 & 0xF800) == 0xA800) /* ADD Rd, SP, #imm: reads SP */
      return 1;
    if ((h1 & 0xFF00) == 0xB200) /* SXTH/SXTB/UXTH/UXTB */
      return 1;
    if ((h1 & 0xFF00) == 0xBA00 && ((h1 >> 6) & 3) != 2) /* REV/REV16/REVSH */
      return 1;
    if ((h1 & 0xFF00) == 0xBF00) /* IT / hints: IT and NOP only */
      return (h1 & 0x000F) != 0 || h1 == 0xBF00;
    if ((h1 & 0xF000) == 0xC000) /* STM/LDM low base, low list */
      return 1;
    return 0; /* SP adjust, CBZ, PUSH/POP, B, SVC, UDF, ... */
  }

  /* 32-bit */
  unsigned rn = h1 & 15;
  if ((h1 & 0xFE00) == 0xE800) /* load/store multiple, dual, exclusive */
  {
    if (h1 & 0x0040) /* dual / exclusive / table branch */
    {
      unsigned p = (h1 >> 8) & 1, w = (h1 >> 5) & 1;
      if (!p && !w)
        return 0; /* LDREX/STREX/TBB */
      unsigned rt = h2 >> 12, rt2 = (h2 >> 8) & 15;
      if (rn == 15 || R_IS_SP_LR_PC(rt) || R_IS_SP_LR_PC(rt2))
        return 0;
      return !(rn == 13 && w);
    }
    unsigned op = (h1 >> 7) & 3;
    if (op != 1 && op != 2)
      return 0;
    if (rn >= 13 || (h2 & 0xE000))
      return 0; /* PUSH/POP-like, or SP/LR/PC in the list */
    return 1;
  }
  if ((h1 & 0xFE00) == 0xEA00) /* data processing, shifted register */
  {
    unsigned rd = (h2 >> 8) & 15, rm = h2 & 15, s = (h1 >> 4) & 1;
    if (R_IS_SP_LR_PC(rm) || rn == 13 || rn == 14)
      return 0;
    if (rd == 15)
      return s; /* TST/TEQ/CMP/CMN */
    return !R_IS_SP_LR_PC(rd);
  }
  if ((h1 & 0xF800) == 0xF000) /* data processing immediate / branches */
  {
    if (h2 & 0x8000)
      return 0; /* branches, misc control */
    unsigned rd = (h2 >> 8) & 15;
    if (!(h1 & 0x0200)) /* modified immediate */
    {
      unsigned s = (h1 >> 4) & 1;
      if (rn == 14)
        return 0;
      if (rd == 15)
        return s;
      return !R_IS_SP_LR_PC(rd); /* Rn = SP only read (ADD Rd, SP, #imm) */
    }
    unsigned op = (h1 >> 4) & 0x1F;
    if (op == 0x04 || op == 0x0C) /* MOVW / MOVT: no Rn */
      return !R_IS_SP_LR_PC(rd);
    if (rn == 15 && (op == 0x00 || op == 0x0A))
      return 0; /* ADR */
    if (rn == 14 || (rn == 15 && op != 0x16))
      return 0; /* BFC is BFI with Rn = PC */
    return !R_IS_SP_LR_PC(rd);
  }
  if ((h1 & 0xFF00) == 0xF800 || (h1 & 0xFF00) == 0xF900) /* load/store single */
  {
    unsigned rt = h2 >> 12;
    if (rn == 15 || R_IS_SP_LR_PC(rt))
      return 0;
    if (rn == 13 && !(h1 & 0x0080) && (h2 & 0x0800) && (h2 & 0x0100))
      return 0; /* pre/post-indexed writeback of SP */
    return 1;
  }
  if ((h1 & 0xFF00) == 0xFA00) /* data processing register, extends, misc */
  {
    unsigned rd = (h2 >> 8) & 15, rm = h2 & 15;
    if ((h2 & 0xF000) != 0xF000)
      return 0;
    return !R_IS_SP_LR_PC(rd) && !R_IS_SP_LR_PC(rm) && rn != 13 && rn != 14;
  }
  if ((h1 & 0xFF00) == 0xFB00) /* multiply, divide */
  {
    unsigned ra = h2 >> 12, rd = (h2 >> 8) & 15, rm = h2 & 15;
    return !R_IS_SP_LR_PC(rn) && !R_IS_SP_LR_PC(rd) && !R_IS_SP_LR_PC(rm) && (ra == 15 || !R_IS_SP_LR_PC(ra));
  }
  return 0; /* coprocessor / FP / anything else */
}

/* ------------------------------------------------------------ windows */

#define OUTLINE_MAX_OPS 12
#define OUTLINE_MIN_BYTES 6
/* A window's body is created at the occurrence where the ones seen so far,
 * all called, would pay for it one and a half times over: n calls save n *
 * (s - 4) bytes, the body costs s + 2, and the earlier occurrences are already
 * emitted inline.  Swept on the Zig compiler built as C at -Os against a fixed
 * third occurrence (-54 KB): 1x -52, 1.2x -58, 1.5x -61, 1.7x -59, 2x -60 KB.
 * Window lengths of 8 / 20 / 32 ops were within 2 KB of 12. */
static int outline_body_pays(int seen, int s)
{
  return seen >= 2 && 2 * seen * (s - 4) >= 3 * (s + 2);
}

static int outline_op_kind_ok(int op)
{
  switch (op)
  {
  case TCCIR_OP_ADD: case TCCIR_OP_SUB: case TCCIR_OP_MUL: case TCCIR_OP_MLA: case TCCIR_OP_UMULL:
  case TCCIR_OP_AND: case TCCIR_OP_OR: case TCCIR_OP_XOR: case TCCIR_OP_SHL: case TCCIR_OP_SAR:
  case TCCIR_OP_SHR: case TCCIR_OP_CMP: case TCCIR_OP_SETIF: case TCCIR_OP_TEST_ZERO:
  case TCCIR_OP_LOAD: case TCCIR_OP_STORE: case TCCIR_OP_ASSIGN: case TCCIR_OP_LEA:
  case TCCIR_OP_LOAD_INDEXED: case TCCIR_OP_STORE_INDEXED: case TCCIR_OP_UBFX: case TCCIR_OP_SBFX:
  case TCCIR_OP_BFI: case TCCIR_OP_ZEXT: case TCCIR_OP_PACK64: case TCCIR_OP_BOOL_OR: case TCCIR_OP_BOOL_AND:
    return 1;
  default:
    return 0;
  }
}

static uint64_t fnv_mix(uint64_t h, uint64_t v)
{
  for (int i = 0; i < 8; i++)
  {
    h ^= (v >> (8 * i)) & 0xFF;
    h *= 1099511628211ull;
  }
  return h;
}

typedef struct
{
  uint64_t key;
  uint32_t count;
  int32_t body; /* index into bodies[] + 1, 0 while it has none */
} OutlineEntry;

/* A shared body: its code is the window's halfwords plus BX LR. */
typedef struct
{
  int esym;     /* local STT_FUNC symbol */
  uint32_t hw0; /* first halfword in body_hw[] */
  uint32_t rel0;
  uint16_t nhw;
  uint16_t nrel;
} OutlineBody;

static OutlineEntry *tab;
static uint32_t tab_cap, tab_n;
static OutlineBody *bodies;
static int nbodies, bodies_cap;
static uint16_t *body_hw;
static uint32_t body_hw_n, body_hw_cap;
static int32_t *body_rel_off; /* bytes from the body start */
static int *body_rel_esym;
static uint32_t body_rel_n, body_rel_cap;
static Section *outline_sec;
static int64_t st_saved;
static uint64_t st_bodies, st_calls, st_fallbacks;

static OutlineEntry *tab_find(uint64_t key, int insert)
{
  if (!key)
    key = 1;
  if (insert && (tab_n + 1) * 4 >= tab_cap * 3)
  {
    uint32_t oc = tab_cap;
    OutlineEntry *ot_ = tab;
    tab_cap = tab_cap ? tab_cap * 2 : (1u << 12);
    tab = tcc_mallocz(sizeof(OutlineEntry) * tab_cap);
    tab_n = 0;
    for (uint32_t i = 0; i < oc; i++)
      if (ot_[i].key)
      {
        OutlineEntry *e = tab_find(ot_[i].key, 1);
        *e = ot_[i];
      }
    tcc_free(ot_);
  }
  if (!tab_cap)
    return NULL;
  uint32_t m = tab_cap - 1, i = (uint32_t)(key ^ (key >> 29)) & m;
  while (tab[i].key && tab[i].key != key)
    i = (i + 1) & m;
  if (!tab[i].key)
  {
    if (!insert)
      return NULL;
    tab[i].key = key;
    tab_n++;
  }
  return &tab[i];
}

static void outline_stats_report(void)
{
  fprintf(stderr, "[OUTLINE] bodies %llu, calls %llu, fallbacks %llu, saved %lld bytes\n",
          (unsigned long long)st_bodies, (unsigned long long)st_calls, (unsigned long long)st_fallbacks,
          (long long)st_saved);
}

/* The plan of the function being compiled: plan_body[k] >= 0 where a window
 * starts, the ops [k, plan_end[k]) it covers.  Built after the rehearsal,
 * used by the real pass. */
static int *plan_body, *plan_end, *plan_save;
static int plan_n;


/* Identical code folding merges a static function with an identical earlier
 * one, and Zig instantiates many such twins.  Outlining must not tell them
 * apart -- the first copy inlines a window its twin would call -- so a
 * rehearsal identical to an earlier one reuses that function's plan, and its
 * windows are not counted again.  Keyed by a hash of the rehearsal; every
 * window is checked again before it is reused. */
typedef struct
{
  uint64_t key;
  uint32_t first, count; /* triples (op, end op, body) in memo_plan[] */
} OutlineMemo;
static OutlineMemo *memo;
static uint32_t memo_cap, memo_n;
static int *memo_plan;
static uint32_t memo_plan_n, memo_plan_cap;

static OutlineMemo *memo_find(uint64_t key, int insert)
{
  if (!key)
    key = 1;
  if (insert && (memo_n + 1) * 4 >= memo_cap * 3)
  {
    uint32_t oc = memo_cap;
    OutlineMemo *om = memo;
    memo_cap = memo_cap ? memo_cap * 2 : 1024;
    memo = tcc_mallocz(sizeof(OutlineMemo) * memo_cap);
    memo_n = 0;
    for (uint32_t i = 0; i < oc; i++)
      if (om[i].key)
        *memo_find(om[i].key, 1) = om[i];
    tcc_free(om);
  }
  if (!memo_cap)
    return NULL;
  uint32_t m = memo_cap - 1, i = (uint32_t)(key ^ (key >> 31)) & m;
  while (memo[i].key && memo[i].key != key)
    i = (i + 1) & m;
  if (!memo[i].key)
  {
    if (!insert)
      return NULL;
    memo[i].key = key;
    memo[i].first = memo[i].count = 0;
    memo_n++;
  }
  return &memo[i];
}

static void memo_push(int v)
{
  if (memo_plan_n == memo_plan_cap)
  {
    memo_plan_cap = memo_plan_cap ? memo_plan_cap * 2 : 1024;
    memo_plan = tcc_realloc(memo_plan, sizeof(int) * memo_plan_cap);
  }
  memo_plan[memo_plan_n++] = v;
}

static void plan_clear(void)
{
  tcc_free(plan_body);
  tcc_free(plan_end);
  tcc_free(plan_save);
  plan_body = plan_end = plan_save = NULL;
  plan_n = 0;
}

/* End of a TU: bodies are local to the object file. */
ST_FUNC void tcc_gen_machine_outline_reset(void)
{
  tcc_free(tab);
  tab = NULL;
  tab_cap = tab_n = 0;
  tcc_free(bodies);
  bodies = NULL;
  nbodies = bodies_cap = 0;
  tcc_free(body_hw);
  body_hw = NULL;
  body_hw_n = body_hw_cap = 0;
  tcc_free(body_rel_off);
  tcc_free(body_rel_esym);
  body_rel_off = NULL;
  body_rel_esym = NULL;
  body_rel_n = body_rel_cap = 0;
  outline_sec = NULL;
  tcc_free(memo);
  memo = NULL;
  memo_cap = memo_n = 0;
  tcc_free(memo_plan);
  memo_plan = NULL;
  memo_plan_n = memo_plan_cap = 0;
  plan_clear();
  tcc_free(sup.hw);
  tcc_free(sup.set);
  tcc_free(sup.rel_off);
  tcc_free(sup.rel_esym);
  memset(&sup, 0, sizeof sup);
  tcc_free(cap.hw);
  tcc_free(cap.set);
  tcc_free(cap.own);
  tcc_free(cap.rel_off);
  tcc_free(cap.rel_esym);
  memset(&cap, 0, sizeof cap);
}

/* Emit the captured bytes [a, b) of the current function as a new body. */
static int body_create(int a, int b)
{
  TCCState *s1 = tcc_state;
  int nhw = (b - a) / 2, k0 = (a - cap.base) / 2;
  if (!outline_sec)
  {
    outline_sec = find_section(s1, ".text.tcc_outlined");
    outline_sec->sh_flags = text_section->sh_flags;
    if (outline_sec->sh_addralign < 4)
      outline_sec->sh_addralign = 4;
  }
  if (nbodies == bodies_cap)
  {
    bodies_cap = bodies_cap ? bodies_cap * 2 : 256;
    bodies = tcc_realloc(bodies, sizeof(OutlineBody) * (size_t)bodies_cap);
  }
  while (body_hw_n + (uint32_t)nhw > body_hw_cap)
  {
    body_hw_cap = body_hw_cap ? body_hw_cap * 2 : 4096;
    body_hw = tcc_realloc(body_hw, sizeof(uint16_t) * body_hw_cap);
  }
  OutlineBody *bd = &bodies[nbodies];
  bd->hw0 = body_hw_n;
  bd->nhw = (uint16_t)nhw;
  bd->rel0 = body_rel_n;
  bd->nrel = 0;
  memcpy(body_hw + body_hw_n, cap.hw + k0, sizeof(uint16_t) * (size_t)nhw);
  body_hw_n += (uint32_t)nhw;

  unsigned long off = outline_sec->data_offset;
  uint8_t *p = section_ptr_add(outline_sec, (addr_t)(2 * nhw + 2));
  for (int x = 0; x < nhw; x++)
  {
    p[2 * x] = (uint8_t)cap.hw[k0 + x];
    p[2 * x + 1] = (uint8_t)(cap.hw[k0 + x] >> 8);
  }
  p[2 * nhw] = 0x70; /* BX LR */
  p[2 * nhw + 1] = 0x47;
  for (int r = 0; r < cap.nrel; r++)
  {
    int ro = cap.rel_off[r] - (a - cap.base);
    if (ro < 0 || ro >= b - a)
      continue;
    if (body_rel_n == body_rel_cap)
    {
      body_rel_cap = body_rel_cap ? body_rel_cap * 2 : 256;
      body_rel_off = tcc_realloc(body_rel_off, sizeof(int32_t) * body_rel_cap);
      body_rel_esym = tcc_realloc(body_rel_esym, sizeof(int) * body_rel_cap);
    }
    body_rel_off[body_rel_n] = ro;
    body_rel_esym[body_rel_n] = cap.rel_esym[r];
    body_rel_n++;
    bd->nrel++;
    put_elf_reloc(symtab_section, outline_sec, off + (unsigned long)ro, R_ARM_GOT_SBREL12, cap.rel_esym[r]);
  }
  char name[40];
  snprintf(name, sizeof name, "__tcc_outlined.%d", nbodies);
  /* A Thumb function's value carries bit 0. */
  bd->esym = put_elf_sym(symtab_section, off + 1, (unsigned long)(2 * nhw + 2), ELFW(ST_INFO)(STB_LOCAL, STT_FUNC), 0,
                         outline_sec->sh_num, name);
  return nbodies++;
}

/* Whether the captured bytes [a, b) and their relocations are body `bi`:
 * the table is keyed by a hash. */
static int body_matches(int bi, int a, int b)
{
  OutlineBody *bd = &bodies[bi];
  int k0 = (a - cap.base) / 2;
  if ((b - a) / 2 != bd->nhw || memcmp(body_hw + bd->hw0, cap.hw + k0, sizeof(uint16_t) * bd->nhw))
    return 0;
  int nrel = 0;
  for (int r = 0; r < cap.nrel; r++)
  {
    int ro = cap.rel_off[r] - (a - cap.base);
    if (ro < 0 || ro >= b - a)
      continue;
    if (nrel >= bd->nrel || body_rel_off[bd->rel0 + nrel] != ro || body_rel_esym[bd->rel0 + nrel] != cap.rel_esym[r])
      return 0;
    nrel++;
  }
  return nrel == bd->nrel;
}

/* Instructions an IT at halfword `h` makes conditional, 0 if `h` is no IT. */
static int it_length(uint16_t h)
{
  if ((h & 0xFF00) != 0xBF00 || !(h & 0xF))
    return 0;
  int n = 4;
  for (unsigned m = h & 0xF; !(m & 1); m >>= 1)
    n--;
  return n;
}

/* TCC_OUTLINE_LIMIT=N: only the first N windows of the compile are called. */
static int bisect_ok(void)
{
  static long lim = -2;
  static long cnt;
  if (lim == -2)
    lim = getenv("TCC_OUTLINE_LIMIT") ? atol(getenv("TCC_OUTLINE_LIMIT")) : -1;
  if (lim < 0)
    return 1;
  if (cnt++ < lim)
  {
    if (cnt == lim)
      fprintf(stderr, "[OUTLINE-LAST] %s\n", funcname);
    return 1;
  }
  return 0;
}

static void plan_add(int n, int k, int end, int bi, int save)
{
  if (!bisect_ok())
    return;
  if (!plan_body)
  {
    plan_n = n;
    plan_body = tcc_malloc(sizeof(int) * (size_t)n);
    plan_end = tcc_mallocz(sizeof(int) * (size_t)n);
    plan_save = tcc_mallocz(sizeof(int) * (size_t)n);
    for (int x = 0; x < n; x++)
      plan_body[x] = -1;
  }
  plan_body[k] = bi;
  plan_end[k] = end;
  plan_save[k] = save;
  st_saved += save;
  st_calls++;
}

/* After the rehearsal of a function whose prologue saves LR: pick the windows
 * of its code that match a body -- or earn one now -- plan them for the real
 * pass, and enter all its windows into the table.  `map[i]` is where op i
 * starts, `end` where the body ends (the epilogue).  Returns how many windows
 * were planned. */
ST_FUNC int tcc_gen_machine_outline_analyze(TCCIRState *ir, const uint32_t *map, int end)
{
  static int reg;
  if (!reg && outline_stats_on())
  {
    reg = 1;
    atexit(outline_stats_report);
  }
  plan_clear();
  const int n = ir->next_instruction_index;
  const uint8_t *btr = ir->codegen_branch_target_reset;
  int *ok = tcc_mallocz(sizeof(int) * (size_t)(n + 1));
  uint64_t *h = tcc_mallocz(sizeof(uint64_t) * (size_t)(n + 1));
  int *sz = tcc_mallocz(sizeof(int) * (size_t)(n + 1));
  int it_left = 0; /* conditional instructions still owed to an IT, across ops */
  int walk = 0;    /* where the instruction after the last op's ends */
  uint64_t fkey = fnv_mix(0xCBF29CE484222325ull, (uint64_t)n);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int a = (int)map[i], b = (int)(i + 1 < n ? map[i + 1] : (uint32_t)end);
    sz[i] = b > a ? b - a : 0;
    if (b <= a)
      continue;
    /* Code of an op still inside an IT block opened by an earlier one: that
     * op cannot start a window (the BL would be conditional). */
    int good = it_left == 0 && q->op != TCCIR_OP_NOP && outline_op_kind_ok(q->op);
    uint64_t hh = 1469598103934665603ull;
    int x = a;
    /* An instruction may straddle two ops: the frame-word pair peephole
     * rewinds over the previous op's LDR/STR to make an LDRD/STRD. */
    if (walk > a)
    {
      good = 0;
      x = walk;
    }
    for (; x < b;)
    {
      int k = (x - cap.base) / 2;
      if (k < 0 || k >= cap.cap || !cap.set[k])
      {
        good = 0;
        it_left = 0;
        x += 2;
        continue;
      }
      uint16_t h1 = cap.hw[k];
      int isz = ((h1 >> 11) >= 0x1D) ? 4 : 2;
      if (isz == 4 && (k + 1 >= cap.cap || !cap.set[k + 1]))
      {
        good = 0;
        it_left = 0;
        x += 2;
        continue;
      }
      uint16_t h2 = isz == 4 ? cap.hw[k + 1] : 0;
      /* A later op may rewrite this one's code: the frame-word pair peephole
       * turns its LDR into the LDRD that also does the next op's load. */
      if (!outline_insn_ok(h1, h2, isz) || cap.own[k] != i)
        good = 0;
      if (it_left > 0)
        it_left--;
      else
        it_left = it_length(h1);
      hh = fnv_mix(hh, ((uint64_t)h1 << 16) | h2);
      x += isz;
    }
    walk = x;
    /* An instruction or an IT block running past the op's end. */
    if (x > b || it_left > 0)
      good = 0;
    for (int r = 0; r < cap.nrel && good; r++)
      if (cap.rel_off[r] >= a - cap.base && cap.rel_off[r] < b - cap.base)
      {
        if (cap.rel_esym[r] <= 0)
          good = 0;
        hh = fnv_mix(hh, (uint64_t)(uint32_t)cap.rel_esym[r] ^ ((uint64_t)(cap.rel_off[r] - (a - cap.base)) << 40));
      }
    ok[i] = good;
    h[i] = hh;
    fkey = fnv_mix(fkey, hh ^ ((uint64_t)(uint32_t)(a - (int)map[0]) << 32) ^ ((uint64_t)good << 63) ^ (uint64_t)i);
  }
  /* Ops [k, end) as a window of this function: its size, or 0. */
#define WINDOW_SIZE(k, end, out)                                                                                       \
  do                                                                                                                   \
  {                                                                                                                    \
    int s_ = 0;                                                                                                        \
    for (int j_ = (k); j_ < (end) && s_ >= 0; j_++)                                                                    \
    {                                                                                                                  \
      IRQuadCompact *q_ = &ir->compact_instructions[j_];                                                               \
      if (j_ > (k) && (q_->is_jump_target || (btr && btr[j_])))                                                        \
        s_ = -1;                                                                                                       \
      else if (q_->op == TCCIR_OP_NOP && !sz[j_])                                                                      \
        continue;                                                                                                      \
      else if (!ok[j_])                                                                                                \
        s_ = -1;                                                                                                       \
      else                                                                                                             \
        s_ += sz[j_];                                                                                                  \
    }                                                                                                                  \
    (out) = s_ > 0 ? s_ : 0;                                                                                           \
  } while (0)
  /* Only a function identical code folding may merge: static, not weak. */
  const Sym *fsym = tcc_state->cur_func_sym;
  const int foldable = fsym && (fsym->type.t & VT_STATIC) && !fsym->a.weak && !fsym->a.nested_func;
  OutlineMemo *fm = foldable ? memo_find(fkey, 0) : NULL;
  if (fm)
  {
    int all = 1;
    for (uint32_t t = 0; t < fm->count && all; t++)
    {
      int k = memo_plan[fm->first + 3 * t], end = memo_plan[fm->first + 3 * t + 1], bi = memo_plan[fm->first + 3 * t + 2];
      int s = 0;
      if (end <= n)
        WINDOW_SIZE(k, end, s);
      all = s > 0 && body_matches(bi, (int)map[k], (int)map[k] + s);
    }
    if (all)
    {
      int planned = 0;
      for (uint32_t t = 0; t < fm->count; t++)
      {
        int k = memo_plan[fm->first + 3 * t], end = memo_plan[fm->first + 3 * t + 1], bi = memo_plan[fm->first + 3 * t + 2];
        plan_add(n, k, end, bi, 2 * bodies[bi].nhw - 4);
        planned++;
      }
      tcc_free(ok);
      tcc_free(h);
      tcc_free(sz);
      return planned;
    }
  }
  /* Ops [k, j) of a window, as long as `pass` non-NOP ones: no jump lands
   * after its first op. */
#define WINDOW_EXTEND(k, j, L, s, key, limit, taken)                                                                  \
  for (; j < n && L < (limit); j++)                                                                                    \
  {                                                                                                                    \
    IRQuadCompact *q_ = &ir->compact_instructions[j];                                                                  \
    if (L > 0 && (q_->is_jump_target || (btr && btr[j])))                                                             \
      break;                                                                                                           \
    if (q_->op == TCCIR_OP_NOP && !sz[j])                                                                              \
      continue;                                                                                                        \
    if (!ok[j] || (taken && taken[j]))                                                                                 \
      break;                                                                                                           \
    key = fnv_mix(key, h[j]);                                                                                          \
    s += sz[j];                                                                                                        \
    L++;                                                                                                               \
    if (L == (limit))                                                                                                  \
    {                                                                                                                  \
      j++;                                                                                                             \
      break;                                                                                                           \
    }                                                                                                                  \
  }
  /* windows starting at each op: greedy, largest first, non-overlapping */
  int planned = 0;
  uint8_t *taken = tcc_mallocz((size_t)(n + 1));
  for (int pass = OUTLINE_MAX_OPS; pass >= 1; pass--)
    for (int k = 0; k < n; k++)
    {
      if (!ok[k] || taken[k])
        continue;
      uint64_t key = 0x9E3779B97F4A7C15ull;
      int s = 0, L = 0, j = k;
      WINDOW_EXTEND(k, j, L, s, key, pass, taken);
      if (L != pass || s < OUTLINE_MIN_BYTES)
        continue;
      OutlineEntry *e = tab_find(fnv_mix(key, (uint64_t)s), 0);
      if (!e || (!e->body && !outline_body_pays((int)e->count + 1, s)))
        continue;
      int a = (int)map[k];
      int bi;
      if (e->body)
      {
        bi = e->body - 1;
        if (!body_matches(bi, a, a + s))
          continue; /* a hash collision */
      }
      else
      {
        /* This occurrence earns the body; the ones before stay inline. */
        bi = body_create(a, a + s);
        e->body = bi + 1;
        st_bodies++;
        st_saved -= s + 2;
      }
      plan_add(n, k, j, bi, s - 4);
      planned++;
      for (int x = k; x < j; x++)
        taken[x] = 1;
    }
  /* enter this function's windows */
  for (int k = 0; k < n; k++)
  {
    if (!ok[k])
      continue;
    uint64_t key = 0x9E3779B97F4A7C15ull;
    int s = 0, L = 0, j = k;
    uint8_t *no_taken = NULL;
    while (j < n && L < OUTLINE_MAX_OPS)
    {
      int L0 = L;
      WINDOW_EXTEND(k, j, L, s, key, L0 + 1, no_taken);
      if (L == L0)
        break;
      if (s >= OUTLINE_MIN_BYTES)
      {
        OutlineEntry *e = tab_find(fnv_mix(key, (uint64_t)s), 1);
        e->count++;
      }
    }
  }
#undef WINDOW_EXTEND
#undef WINDOW_SIZE
  /* remember the plan for this function's twins */
  if (foldable)
  {
    fm = memo_find(fkey, 1);
    fm->first = memo_plan_n;
    fm->count = 0;
    for (int k = 0; k < n && plan_body; k++)
      if (plan_body[k] >= 0)
      {
        memo_push(k);
        memo_push(plan_end[k]);
        memo_push(plan_body[k]);
        fm->count++;
      }
  }
  tcc_free(taken);
  tcc_free(ok);
  tcc_free(h);
  tcc_free(sz);
  return planned;
}

/* ------------------------------------------------------------ real pass */

ST_FUNC int tcc_gen_machine_outline_suppressing(void)
{
  return sup.active;
}

static void sup_reloc(Sym *sym, int at)
{
  if (sup.nrel == sup.relcap)
  {
    sup.relcap = sup.relcap ? sup.relcap * 2 : 16;
    sup.rel_off = tcc_realloc(sup.rel_off, sizeof(int32_t) * (size_t)sup.relcap);
    sup.rel_esym = tcc_realloc(sup.rel_esym, sizeof(int) * (size_t)sup.relcap);
  }
  sup.rel_off[sup.nrel] = at - sup.vbase;
  sup.rel_esym[sup.nrel] = sym && sym->c > 0 ? sym->c : -1;
  sup.nrel++;
}

/* Called by ot() for every instruction of the real pass: inside a window the
 * instruction is recorded instead of written (returns 1; ot() still advances
 * `ind`). */
ST_FUNC int tcc_gen_machine_outline_suppress(uint32_t opcode, int size)
{
  if (!sup.active)
    return 0;
  int k = (ind - sup.vbase) / 2;
  if (k < 0 || (size != 2 && size != 4))
    tcc_error("internal error: outlined window instruction at 0x%x", ind);
  if (k + 2 > sup.cap)
  {
    int nc = sup.cap ? sup.cap * 2 : 64;
    while (nc < k + 2)
      nc *= 2;
    sup.hw = tcc_realloc(sup.hw, sizeof(uint16_t) * (size_t)nc);
    sup.set = tcc_realloc(sup.set, (size_t)nc);
    memset(sup.set + sup.cap, 0, (size_t)(nc - sup.cap));
    sup.cap = nc;
  }
  if (size == 4)
  {
    sup.hw[k] = (uint16_t)(opcode >> 16);
    sup.hw[k + 1] = (uint16_t)opcode;
    sup.set[k] = sup.set[k + 1] = 1;
  }
  else
  {
    sup.hw[k] = (uint16_t)opcode;
    sup.set[k] = 1;
  }
  return 1;
}

/* The window's ops are done.  If they produced the body, `ind` goes back to
 * the window's address and the BL is written there.  If not -- a cache the
 * pool dumps reset at other points than in the rehearsal -- the code they
 * produced is written where it was generated, exactly as without the
 * outliner. */
static void outline_window_close(void)
{
  if (!sup.active)
    return;
  sup.active = 0;
  OutlineBody *bd = &bodies[sup.body];
  const int bytes = ind - sup.vbase, nh = bytes / 2;
  int all_set = bytes >= 0 && !(bytes & 1);
  for (int x = 0; x < nh && all_set; x++)
    all_set = sup.set[x];
  int same = all_set && nh == bd->nhw && !memcmp(sup.hw, body_hw + bd->hw0, sizeof(uint16_t) * (size_t)nh);
  int nrel = 0;
  for (int r = 0; r < sup.nrel && same; r++)
  {
    if (sup.rel_off[r] < 0 || sup.rel_off[r] >= bytes)
      continue;
    if (nrel >= bd->nrel || body_rel_off[bd->rel0 + nrel] != sup.rel_off[r] ||
        body_rel_esym[bd->rel0 + nrel] != sup.rel_esym[r])
      same = 0;
    nrel++;
  }
  if (same && nrel == bd->nrel)
  {
    tcc_gen_machine_outline_window_closed(bytes);
    ind = sup.vbase;
    tcc_gen_machine_outline_emit_bl(bd->esym);
    return;
  }

  st_fallbacks++;
  if (outline_stats_on() > 1)
  {
    fprintf(stderr, "[OUTLINE-FB] %s at 0x%x: %d/%d halfwords, same %d:", funcname, sup.vbase, nh, bd->nhw, same);
    for (int x = 0; x < nh && all_set; x++)
      fprintf(stderr, " %04x", sup.hw[x]);
    fprintf(stderr, " | body");
    for (int x = 0; x < bd->nhw; x++)
      fprintf(stderr, " %04x", body_hw[bd->hw0 + x]);
    fprintf(stderr, " | rel %d/%d\n", sup.nrel, bd->nrel);
  }
  if (!all_set)
    tcc_error("internal error: outlined window at 0x%x was not all emitted through ot()", sup.vbase);
  ind = sup.vbase;
  for (int x = 0; x < nh; x++)
    tcc_gen_machine_outline_emit_raw(sup.hw[x]);
  for (int r = 0; r < sup.nrel; r++)
    if (sup.rel_off[r] >= 0 && sup.rel_off[r] < bytes && sup.rel_esym[r] > 0)
      put_elf_reloc(symtab_section, cur_text_section, (unsigned long)(sup.vbase + sup.rel_off[r]), R_ARM_GOT_SBREL12,
                    sup.rel_esym[r]);
}

/* The real pass reaches op i, before recording its address. */
ST_FUNC void tcc_gen_machine_outline_op_end(int i)
{
  if (sup.active && i >= sup.end_op)
    outline_window_close();
}

/* The real pass recorded op i's address: a planned window starts there (and
 * the BL will be there). */
ST_FUNC void tcc_gen_machine_outline_op_start(int i)
{
  if (!plan_body || i >= plan_n || plan_body[i] < 0 || sup.active)
    return;
  sup.body = plan_body[i];
  sup.end_op = plan_end[i];
  sup.nrel = 0;
  if (sup.cap)
    memset(sup.set, 0, (size_t)sup.cap);
  tcc_gen_machine_outline_window_open();
  sup.vbase = ind;
  sup.active = 1;
}

/* The real pass is done with the function. */
ST_FUNC void tcc_gen_machine_outline_function_end(void)
{
  outline_window_close();
  plan_clear();
}

/* Bytes the real pass can shed, against the rehearsal, by windows starting
 * strictly between two ops. */
ST_FUNC int tcc_gen_machine_outline_shrink_between(int from_ir, int to_ir)
{
  if (!plan_body)
    return 0;
  int s = 0;
  for (int k = from_ir + 1; k < to_ir && k < plan_n; k++)
    if (k >= 0 && plan_body[k] >= 0)
      s += plan_save[k];
  return s;
}
