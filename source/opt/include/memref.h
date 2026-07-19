/*
 *  TCC IR - Memory reference model: canonical MemLoc + aliasing
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/*
 * A single, shared vocabulary for "what memory does this operand name?", the
 * foundation the memory optimizer's forwarding / CSE / DSE / equality passes are
 * meant to build on instead of each re-deriving it.  This first cut provides the
 * location model and a conservative may-alias; a callee mod-ref query
 * (`call_writes`/`call_reads`) and memory-SSA are layered on top later.
 */

#pragma once

#include <stdint.h>

#include "tccir_operand.h" /* IROperand */

struct TCCIRState;
struct Sym;

typedef enum
{
  MEMLOC_NONE = 0,   /* not a memory reference */
  MEMLOC_GLOBAL,     /* &sym + off  (a named global / static) */
  MEMLOC_FRAME,      /* StackLoc[off]  (this function's own frame) */
  MEMLOC_UNKNOWN     /* a pointer we could not resolve — may alias anything */
} MemLocKind;

typedef struct
{
  MemLocKind kind;
  struct Sym *sym; /* GLOBAL only */
  int64_t off;     /* GLOBAL: addend from sym; FRAME: frame byte offset */
  int size;        /* access width in bytes (0 = unknown width) */
} MemLoc;

/* Resolve the address an lval load/store operand `op` (appearing at instruction
 * `at_idx`) refers to.  Handles a direct global-symref deref, a direct
 * StackLoc, and a pointer TEMP whose single-def chain is
 * `Addr[StackLoc] | &sym  (+/- #const)* (ASSIGN/LEA/ADD/SUB)*`.  Returns
 * MEMLOC_UNKNOWN for anything else, MEMLOC_NONE for a non-memory operand. */
MemLoc memloc_of(struct TCCIRState *ir, IROperand op, int at_idx);

/* Resolve a pointer *value* operand (a base address, not an lval deref — e.g. a
 * STORE_INDEXED base): a direct `&sym`/StackLoc address, or a TEMP holding such
 * a pointer via `Addr[StackLoc] | &sym (+/- #const)* (ASSIGN/LEA/ADD/SUB)*`.
 * `size` is left 0 (a base address has no intrinsic access width). */
MemLoc memloc_of_pointer(struct TCCIRState *ir, IROperand op, int at_idx);

/* 1 if `a` and `b` may name overlapping bytes.  Globals never alias the frame;
 * distinct globals never alias; same-base ranges alias iff they overlap; an
 * UNKNOWN pointer conservatively aliases any global or frame location. */
int mem_may_alias(MemLoc a, MemLoc b);

/* Mod-ref: can the call at `call_idx` write location `L`?  Conservative — any
 * callee without a summary (external, or not yet compiled in this TU), any
 * unattributable write, and any unresolved callee all answer 1.  Answers 0 only
 * when the whole reachable callee set provably writes neither L's global nor
 * anything unnamed (so it cannot reach the caller's frame either).
 * Backed by the TU function summaries; see tu_func_summary.c. */
int tcc_ir_call_may_write(struct TCCIRState *ir, int call_idx, MemLoc L);
