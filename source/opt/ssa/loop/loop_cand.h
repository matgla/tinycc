/*
 *  TCC IR - SSA loop passes: shared natural-loop candidate helpers
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include <stdint.h>

struct IRCFG;
struct IRBasicBlock;

typedef struct {
  int header_b;
  int latch_b;
  int size; /* flat instruction span, for smallest-first ordering */
} FieCand;

int fie_cand_cmp(const void *a, const void *b);

FieCand *fie_collect_cands(struct IRCFG *cfg, int *out_nc);

int fie_entry_pred(struct IRBasicBlock *hb, int latch_b, const uint8_t *member);

void fie_collect_members(struct IRCFG *cfg, int header_b, int latch_b,
                         uint8_t *member);

void lcs_collect_header_members(struct IRCFG *cfg, int header_b, uint8_t *member,
                                uint8_t *scratch);

struct TCCIRState;

typedef struct {
  int start;
  int end;
  int header;
  int preheader;
} LcsSpan;

typedef int (*LcsCandFn)(struct TCCIRState *ir, struct IRCFG *cfg, int header_b,
                         uint8_t *member, uint8_t *scratch);

void lcs_mark_headers(struct IRCFG *cfg, uint8_t *is_header);

int lcs_cand_span(struct TCCIRState *ir, struct IRCFG *cfg, int header_b,
                  uint8_t *member, uint8_t *scratch, int max_span, LcsSpan *out);

int lcs_run_outermost(struct TCCIRState *ir, int max_passes, int stop_first,
                      LcsCandFn fn);

int lcs_run_leaf(struct TCCIRState *ir, int max_passes, int stop_first,
                 LcsCandFn fn);
