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

typedef struct {
  int header_b;
  int latch_b;
  int size; /* flat instruction span, for smallest-first ordering */
} FieCand;

int fie_cand_cmp(const void *a, const void *b);

void fie_collect_members(struct IRCFG *cfg, int header_b, int latch_b,
                         uint8_t *member);

void lcs_collect_header_members(struct IRCFG *cfg, int header_b, uint8_t *member,
                                uint8_t *scratch);
