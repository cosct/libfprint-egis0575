/*
 * EgisTec EH575 matcher: port of the Windows-engine replica validated in
 * the fprintdriver research project. The Python reference lives in
 * scripts/egis_matcher.py; provenance of the extracted coefficients is
 * documented in docs/windows-engine-tables.md, both at
 * https://github.com/cosct/fprintdriver
 *
 * Pipeline: flat-field → 11-orientation ridge matched filter bank (taps
 * extracted from EgisTouchFPEngine0575.dll) → interest points on filter
 * response → 512-bit orientation-histogram descriptors (DLL pyramid
 * weights) → Hamming NN + translation-cluster + angle-mode scoring.
 *
 * Pure C, no GLib: also compiled into the standalone cross-validation
 * harness (tools/egis0575-matcher-test.c).
 *
 * Copyright (C) 2026 cosct <cosct@outlook.com>
 * Copyright (C) 2026 fprintdriver research contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include <stdint.h>

#define EGIS0575_M_DESC_BYTES 64          /* 512-bit top-tier descriptor */
#define EGIS0575_M_MAX_FEATURES 120
#define EGIS0575_M_MIN_FEATURES 11
#define EGIS0575_M_MIN_MATCHED 5
#define EGIS0575_M_HAMMING_BUDGET 115     /* of 512 */
#define EGIS0575_M_ANGLE_TOL_DEG 12.0
#define EGIS0575_M_POS_TOL_PX 8
#define EGIS0575_M_N_ORIENT 11

/* Verdict thresholds. The C engine's descriptor bits differ slightly from
 * the Python reference (border handling of the flat-field mean plus the
 * interpolation path in the oriented filter), so its score scale differs.
 *
 * Recalibrated 2026-09-13 on 23 verify-run datasets (125 probes) after the
 * orientation-4 FIR kernel and tie-break fixes changed the score scale.
 * Verdicts are run-level (up to 6 probes per press, any passing probe
 * suffices): confident-genuine runs bottomed at a max probe score of 349,
 * confident-impostor runs peaked at 288 (including ambiguous runs: 321).
 * Threshold 335 sits at the midpoint of that gap; agree floor 150 × 2
 * frames unchanged
 * (https://github.com/cosct/fprintdriver/blob/master/docs/windows-engine-tables.md). */
#define EGIS0575_M_MATCH_THRESHOLD 335
#define EGIS0575_M_AGREE_SCORE 150
#define EGIS0575_M_AGREE_FRAMES 2

/* Second-chance retry (Windows engine tail of FUN_1800067c0: a probe whose
 * score clears the threshold but whose consistent-pair count falls short
 * gets one rotation-compensated re-match before failing — the off-angle
 * press case). Our analogue re-scores the gallery with doubled geometric
 * tolerances and lets that pass count towards the agree floor; the best
 * score itself still comes from the strict pass, so the FAR side is bounded
 * by the unchanged MATCH_THRESHOLD. Offline replay on the 36-runnable
 * corpus (2026-10-04, tools/egis0575-feedback-eval): rescued one genuine
 * run (best 403 failing on agree) with zero impostor flips.
 * EGIS0575_SECOND_CHANCE=0 disables. */
#define EGIS0575_M_SC_POS_TOL_PX 12
#define EGIS0575_M_SC_ANGLE_TOL_DEG 24.0

typedef struct
{
  uint16_t x, y;
  uint8_t  orient;                       /* winning orientation index 0..10 */
  uint8_t  desc[EGIS0575_M_DESC_BYTES];  /* 512 bits, MSB-first packing */
} Egis0575MFeature;

typedef struct
{
  int             n;
  Egis0575MFeature f[EGIS0575_M_MAX_FEATURES];
} Egis0575MFeatureSet;

/* Extract features from a processed grayscale image (w×h, row-major). */
void egis0575_m_extract (const uint8_t *img, int w, int h,
                         Egis0575MFeatureSet *out);

/* Windows-style score: Σ(128−h) over consistent matches minus the
 * unexplained-feature penalty. Returns n_good (votes) via *votes_out
 * when non-NULL. */
int egis0575_m_score (const Egis0575MFeatureSet *probe,
                      const Egis0575MFeatureSet *gallery,
                      int *votes_out);

/* Same scoring with caller-supplied geometric tolerances (the strict
 * wrapper above uses EGIS0575_M_POS_TOL_PX / _ANGLE_TOL_DEG; the
 * second-chance retry uses the wider EGIS0575_M_SC_* pair). */
int egis0575_m_score_ex (const Egis0575MFeatureSet *probe,
                         const Egis0575MFeatureSet *gallery,
                         int    *votes_out,
                         int     pos_tol_px,
                         double  angle_tol_deg);
