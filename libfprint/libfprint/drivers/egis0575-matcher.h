/*
 * EgisTec EH575 matcher: reverse-engineered from EgisTouchFPEngine0575.dll
 * (0x180009560..0x180015800). See docs/windows-dll-disassembly.md for the
 * full disassembly and RVA map.
 *
 * Pure C, no GLib: also compiled into the standalone cross-validation
 * harness (tools/egis0575-matcher-test.c, tools/egis0575-feedback-eval.c).
 *
 * Copyright (C) 2026 cosct <cosct@outlook.com>
 * Copyright (C) 2026 Himmelschmidt
 * Copyright (C) 2026 fprintdriver research contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <stdint.h>

#define EGIS0575_M_DESC_BYTES 64          /* 486-bit multi-scale rank descriptor + type byte in desc[63] */
#define EGIS0575_M_MAX_FEATURES 120
#define EGIS0575_M_MIN_FEATURES 11
#define EGIS0575_M_MIN_MATCHED 5
#define EGIS0575_M_HAMMING_BUDGET 125     /* of 486 (~243 1-bits per descriptor) */
#define EGIS0575_M_ANGLE_TOL_DEG 14.0
#define EGIS0575_M_POS_TOL_PX 8
#define EGIS0575_M_N_ORIENT 180           /* 0..179 (2-deg steps covering 0..358 deg) */

/* Verdict thresholds. */
#define EGIS0575_M_MATCH_THRESHOLD 280
#define EGIS0575_M_AGREE_SCORE 200
#define EGIS0575_M_AGREE_FRAMES 1

/* Second-chance retry geometric tolerances. */
#define EGIS0575_M_SC_POS_TOL_PX 12
#define EGIS0575_M_SC_ANGLE_TOL_DEG 24.0

typedef struct
{
  uint16_t x, y;
  uint8_t  orient;                       /* dominant angle / 2 (0..179 -> 0..358 deg) */
  uint8_t  desc[EGIS0575_M_DESC_BYTES];  /* 486 bits in [0..60], polarity type in [63] */
} Egis0575MFeature;

typedef struct
{
  int             n;
  Egis0575MFeature f[EGIS0575_M_MAX_FEATURES];
} Egis0575MFeatureSet;

/* Extract features from a processed grayscale image (w×h, row-major). */
void egis0575_m_extract (const uint8_t *img, int w, int h,
                         Egis0575MFeatureSet *out);

/* Symmetric Windows-DLL MatchMinutia score. Returns inlier votes via
 * *votes_out when non-NULL. */
int egis0575_m_score (const Egis0575MFeatureSet *probe,
                      const Egis0575MFeatureSet *gallery,
                      int *votes_out);

/* Same scoring with caller-supplied geometric tolerances. */
int egis0575_m_score_ex (const Egis0575MFeatureSet *probe,
                         const Egis0575MFeatureSet *gallery,
                         int    *votes_out,
                         int     pos_tol_px,
                         double  angle_tol_deg);
