/*
 *  TCC IR - TU-wide function summary: shared types + summary list
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_OPT_FLAT_IPA_TU_SUMMARY_H
#define TCC_OPT_FLAT_IPA_TU_SUMMARY_H

typedef struct TuSymSet
{
  Sym **items;
  int count;
  int capacity;
} TuSymSet;

typedef struct TuFuncSummary
{
  Sym *func_sym;
  TuSymSet calls;          /* static (intra-TU) functions called */
  TuSymSet static_reads;   /* static globals read or address-taken */
  TuSymSet static_writes;  /* static globals written */
  int body_elide_blocker;  /* obvious non-call side effect in the body */
  struct TuFuncSummary *next;
} TuFuncSummary;

extern TuFuncSummary *tu_summary_head;

/* Statics read anywhere in the PRE-optimization IR. */
extern TuSymSet tu_source_reads;

void tu_symset_add(TuSymSet *s, Sym *sym);
int tu_symset_contains(const TuSymSet *s, const Sym *sym);
void tu_symset_free(TuSymSet *s);

#endif
