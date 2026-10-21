/*
 * EgisTec EH575 matcher (see egis0575-matcher.h for provenance).
 *
 * Copyright (C) 2026 cosct <cosct@outlook.com>
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

#include "egis0575-matcher.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* DLL RVA 0x3ffd0 (file offset 0x3ebd0): 11 ridge-template kernels, stored
 * packed as i32 with lengths 5,5,5,7,7,7,9,9,9,11,11; all sum 32768.
 * Orientation 4 is 7 taps in the DLL — an earlier extraction padded it to
 * 9 with the previous kernel's edge tap (706); corrected 2026-09-13. */
static const int fir_taps[EGIS0575_M_N_ORIENT][11] = {
  { 1785, 8003, 13193, 8002, 1785, 0, 0, 0, 0, 0, 0 },
  { 2319, 8011, 12109, 8010, 2319, 0, 0, 0, 0, 0, 0 },
  { 2806, 7951, 11253, 7952, 2806, 0, 0, 0, 0, 0, 0 },
  { 706, 3097, 7524, 10114, 7524, 3097, 706, 0, 0, 0, 0 },
  { 950, 3402, 7313, 9438, 7313, 3402, 950, 0, 0, 0, 0 },
  { 1200, 3646, 7104, 8870, 7102, 3646, 1200, 0, 0, 0, 0 },
  { 361, 1415, 3757, 6748, 8205, 6749, 3757, 1415, 361, 0, 0 },
  { 486, 1632, 3877, 6517, 7746, 6515, 3877, 1632, 486, 0, 0 },
  { 622, 1832, 3962, 6295, 7346, 6295, 3962, 1832, 622, 0, 0 },
  { 216, 753, 1985, 3967, 6011, 6904, 6011, 3967, 1985, 753, 216 },
  { 289, 889, 2134, 3986, 5800, 6572, 5800, 3986, 2134, 889, 289 },
};
static const int fir_len[EGIS0575_M_N_ORIENT] = { 5, 5, 5, 7, 7, 7, 9, 9, 9, 11, 11 };

/* DLL 0x40130: 11-tap descriptor pyramid weight kernels (peaks 425/392). */
static const double w_row[11] = { 57, 118, 207, 308, 392, 425, 392, 308, 207, 118, 57 };
static const double w_col[11] = { 53, 109, 191, 285, 362, 392, 362, 285, 191, 109, 53 };

#define BINW (360.0 / 32.0)

static double
clamp_sample (const double *img, int w, int h, double x, double y)
{
  int xi = (int) x, yi = (int) y;
  double fx = x - xi, fy = y - yi;

  if (xi < 0) { xi = 0; fx = 0; }
  if (yi < 0) { yi = 0; fy = 0; }
  if (xi > w - 2) { xi = w - 2; fx = 1; }
  if (yi > h - 2) { yi = h - 2; fy = 1; }

  double a = img[yi * w + xi];
  double b = img[yi * w + xi + 1];
  double c = img[(yi + 1) * w + xi];
  double d = img[(yi + 1) * w + xi + 1];

  return a * (1 - fy) * (1 - fx) + b * (1 - fy) * fx +
         c * fy * (1 - fx) + d * fy * fx;
}

static void
flat_field (const uint8_t *img, int w, int h, double *out)
{
  /* the sensor geometry (≤ a few hundred px per side) keeps the integral
   * image far below INT_MAX; the standalone harness feeds it the same
   * sensor-sized PGMs */
  int *csum = calloc ((size_t) (w + 1) * (h + 1), sizeof (int));
  double mean_all = 0.0, sq_all = 0.0;

  if (!csum)
    {
      memset (out, 0, sizeof (double) * w * h);
      return;
    }

  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      csum[(y + 1) * (w + 1) + (x + 1)] =
        img[y * w + x] + csum[y * (w + 1) + (x + 1)] +
        csum[(y + 1) * (w + 1) + x] - csum[y * (w + 1) + x];

  /* 15x15 local mean over the in-image part of the window: at the borders
   * the window is clamped and the divisor is the clamped count. This is
   * deliberately NOT the Python reference's np.pad(mode="edge") + fixed
   * 225 divisor (which replicates border pixels instead); the C behaviour
   * is the one the shipped thresholds were validated with, and the known
   * C/Python border difference is tracked in the research repo docs
   * (docs/comparison.md §6). */
  for (int y = 0; y < h; y++)
    {
      int y0 = y - 7, y1 = y + 8;
      int ya = y0 < 0 ? 0 : y0, yb = y1 > h ? h : y1;
      for (int x = 0; x < w; x++)
        {
          int x0 = x - 7, x1 = x + 8;
          int xa = x0 < 0 ? 0 : x0, xb = x1 > w ? w : x1;
          /* clamped window size: divide by the visible count, not 225 */
          int wh = (x1 > w ? w : x1) - (x0 < 0 ? 0 : x0);
          int wv = (y1 > h ? h : y1) - (y0 < 0 ? 0 : y0);
          int cnt = wh * wv;
          double sum = csum[yb * (w + 1) + xb] - csum[ya * (w + 1) + xb] -
                       csum[yb * (w + 1) + xa] + csum[ya * (w + 1) + xa];
          double v = img[y * w + x] - sum / cnt;
          out[y * w + x] = v;
          mean_all += v;
          sq_all += v * v;
        }
    }

  double n = (double) w * h;
  /* rounding can make the variance marginally negative */
  double std = sqrt (fmax (sq_all / n - (mean_all / n) * (mean_all / n), 0.0));
  for (int i = 0; i < w * h; i++)
    out[i] /= (std + 1e-9);

  free (csum);
}

static void
oriented_bank (const double *flat, int w, int h,
               double *enhanced, uint8_t *orient_idx)
{
  for (int i = 0; i < w * h; i++)
    enhanced[i] = -1e18;

  for (int o = 0; o < EGIS0575_M_N_ORIENT; o++)
    {
      double theta = 180.0 * o / EGIS0575_M_N_ORIENT;
      double rad = theta * M_PI / 180.0;
      double cs = cos (rad), sn = sin (rad);
      int len = fir_len[o];
      int c = len / 2;
      double k[11], ksum = 0.0;

      for (int t = 0; t < len; t++)
        ksum += fir_taps[o][t];
      for (int t = 0; t < len; t++)
        k[t] = fir_taps[o][t] / ksum;

      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
          {
            double acc = 0.0;
            for (int t = 0; t < len; t++)
              {
                double d = t - c;
                /* correlation along orientation theta (image y grows down) */
                double sx = x + d * cs;
                double sy = y - d * sn;
                acc += k[t] * clamp_sample (flat, w, h, sx, sy);
              }
            double a = fabs (acc);
            if (a > enhanced[y * w + x])
              {
                enhanced[y * w + x] = a;
                orient_idx[y * w + x] = (uint8_t) o;
              }
          }
    }
}

static void
pack_descriptor (const double *vals /* 512 */, uint8_t *out)
{
  double mean = 0.0;
  for (int i = 0; i < 512; i++)
    mean += vals[i];
  mean /= 512.0;

  memset (out, 0, EGIS0575_M_DESC_BYTES);
  for (int i = 0; i < 512; i++)
    if (vals[i] > mean)
      out[i >> 3] |= (uint8_t) (0x80u >> (i & 7));
}

void
egis0575_m_extract (const uint8_t *img, int w, int h,
                    Egis0575MFeatureSet *out)
{
  double *flat, *enh, *gx, *gy;
  uint8_t *oidx;

  out->n = 0;

  /* interest points scan with a 6px margin and the descriptor needs an
   * 11px window; smaller inputs cannot yield features */
  if (w < 13 || h < 13)
    return;

  flat = malloc (sizeof (double) * w * h);
  enh = malloc (sizeof (double) * w * h);
  gx = malloc (sizeof (double) * w * h);
  gy = malloc (sizeof (double) * w * h);
  oidx = malloc ((size_t) w * h);

  if (!flat || !enh || !gx || !gy || !oidx)
    {
      free (flat);
      free (enh);
      free (gx);
      free (gy);
      free (oidx);
      return;
    }

  flat_field (img, w, h, flat);
  oriented_bank (flat, w, h, enh, oidx);

  /* central-difference gradients of the enhanced image */
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        int xm = x > 0 ? x - 1 : 0, xp = x < w - 1 ? x + 1 : w - 1;
        int ym = y > 0 ? y - 1 : 0, yp = y < h - 1 ? y + 1 : h - 1;
        gx[y * w + x] = (enh[y * w + xp] - enh[y * w + xm]) / (xp - xm);
        gy[y * w + x] = (enh[yp * w + x] - enh[ym * w + x]) / (yp - ym);
      }

  /* interest points: 3x3 strict maxima, margin 6 */
  int cap = 4 * EGIS0575_M_MAX_FEATURES;
  int *pxs = malloc (sizeof (int) * cap), *pys = malloc (sizeof (int) * cap);
  double *pvals = malloc (sizeof (double) * cap);
  int npts = 0;

  if (!pxs || !pys || !pvals)
    {
      free (pxs);
      free (pys);
      free (pvals);
      goto out;
    }

  for (int y = 6; y < h - 6; y++)
    for (int x = 6; x < w - 6; x++)
      {
        double v = enh[y * w + x];
        double m = v;
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++)
            if (enh[(y + dy) * w + x + dx] > m)
              m = enh[(y + dy) * w + x + dx];
        if (m > v)
          continue;
        /* strict centre winner on ties (numpy argmax==4 semantics): the
         * centre stays only if every equal-valued neighbour comes later in
         * row-major scan order, i.e. its (dy,dx) offset indexes past the
         * centre: (dy+1)*3+(dx+1) > 4  <=>  dy*3+dx > 0. */
        if (m == v)
          {
            int strict = 1;
            for (int dy = -1; dy <= 1 && strict; dy++)
              for (int dx = -1; dx <= 1; dx++)
                {
                  if (dx == 0 && dy == 0)
                    continue;
                  if (enh[(y + dy) * w + x + dx] == v &&
                      (dy * 3 + dx) < 0)
                    { strict = 0; break; }
                }
            if (!strict)
              continue;
          }
        if (npts < cap)
          {
            pxs[npts] = x;
            pys[npts] = y;
            pvals[npts] = v;
            npts++;
          }
      }

  /* sort by response desc, density NMS radius 6, cap 120 */
  {
    /* insertion-sort index pairs is O(n²) worst; use simple approach with
     * qsort on packed (val, x, y) via index array */
    int *idx = malloc (sizeof (int) * (npts > 0 ? npts : 1));
    if (!idx)
      goto out_free_pts;
    for (int i = 0; i < npts; i++)
      idx[i] = i;
    /* selection of top-K by value with NMS: sort indices by value desc */
    for (int i = 0; i < npts; i++)
      for (int j = i + 1; j < npts; j++)
        if (pvals[idx[j]] > pvals[idx[i]])
          {
            int t = idx[i];
            idx[i] = idx[j];
            idx[j] = t;
          }

    for (int i = 0; i < npts && out->n < EGIS0575_M_MAX_FEATURES; i++)
      {
        int x = pxs[idx[i]], y = pys[idx[i]];
        int ok = 1;
        for (int k = 0; k < out->n; k++)
          {
            int ddx = x - out->f[k].x, ddy = y - out->f[k].y;
            if (ddx * ddx + ddy * ddy < 36)
              {
                ok = 0;
                break;
              }
          }
        if (!ok)
          continue;

        Egis0575MFeature *f = &out->f[out->n];
        f->x = (uint16_t) x;
        f->y = (uint16_t) y;
        f->orient = oidx[y * w + x];

        double dom = 180.0 * f->orient / EGIS0575_M_N_ORIENT;
        double vals[512];
        memset (vals, 0, sizeof (vals));
        for (int dy = 0; dy < 4; dy++)
          for (int dx = 0; dx < 4; dx++)
            {
              /* 4×4 sub-blocks with 5px windows over the 11×11 pyramid; the
               * last sub-block overruns (7:12) and numpy slicing silently
               * truncates it to 7:11 in the reference — clamp the same way
               * (w_row/w_col have exactly EGIS0575_M_N_ORIENT entries).
               * (no GLib MIN here: this file is pure C) */
              int r0 = 2 * dy + (dy > 1), r1 = r0 + 5 > EGIS0575_M_N_ORIENT ? EGIS0575_M_N_ORIENT : r0 + 5;
              int c0 = 2 * dx + (dx > 1), c1 = c0 + 5 > EGIS0575_M_N_ORIENT ? EGIS0575_M_N_ORIENT : c0 + 5;
              for (int yy = r0; yy < r1; yy++)
                for (int xx = c0; xx < c1; xx++)
                  {
                    int iy = y - 5 + yy, ix = x - 5 + xx;
                    double ang = atan2 (gy[iy * w + ix], gx[iy * w + ix]) *
                                 180.0 / M_PI;
                    ang = fmod (ang, 360.0);
                    if (ang < 0)
                      ang += 360.0;
                    double rel = (ang - dom) / BINW;
                    int b = ((int) rel) % 32;      /* numpy trunc-cast */
                    if (b < 0)
                      b += 32;
                    double mag = sqrt (gx[iy * w + ix] * gx[iy * w + ix] +
                                       gy[iy * w + ix] * gy[iy * w + ix]);
                    vals[dy * 128 + dx * 32 + b] +=
                      w_row[yy] * w_col[xx] / 425.0 * mag;
                  }
            }
        pack_descriptor (vals, f->desc);
        out->n++;
      }
    free (idx);
  }

out_free_pts:
  free (pxs);
  free (pys);
  free (pvals);
out:
  free (flat);
  free (enh);
  free (gx);
  free (gy);
  free (oidx);
}

static inline int
hamming512 (const uint8_t *a, const uint8_t *b)
{
  /* __builtin_popcountll is GCC/Clang-only; libfprint requires one of those */
  int d = 0;
  for (int i = 0; i < EGIS0575_M_DESC_BYTES; i += 8)
    {
      uint64_t x;
      memcpy (&x, a + i, 8);
      uint64_t y;
      memcpy (&y, b + i, 8);
      d += __builtin_popcountll (x ^ y);
    }
  return d;
}

static inline int
floordiv8 (int v)
{
  return v >= 0 ? v / 8 : (v - 7) / 8;
}

int
egis0575_m_score (const Egis0575MFeatureSet *probe,
                  const Egis0575MFeatureSet *gallery,
                  int *votes_out)
{
  return egis0575_m_score_ex (probe, gallery, votes_out,
                              EGIS0575_M_POS_TOL_PX,
                              EGIS0575_M_ANGLE_TOL_DEG);
}

int
egis0575_m_score_ex (const Egis0575MFeatureSet *probe,
                     const Egis0575MFeatureSet *gallery,
                     int    *votes_out,
                     int     pos_tol_px,
                     double  angle_tol_deg)
{
  int n1 = probe->n, n2 = gallery->n;

  if (votes_out)
    *votes_out = 0;
  if (n1 < EGIS0575_M_MIN_MATCHED || n2 < EGIS0575_M_MIN_MATCHED)
    return 0;

  int best_h[EGIS0575_M_MAX_FEATURES], best_j[EGIS0575_M_MAX_FEATURES];

  for (int i = 0; i < n1; i++)
    {
      best_h[i] = 512;
      best_j[i] = -1;
      for (int j = 0; j < n2; j++)
        {
          int hh = hamming512 (probe->f[i].desc, gallery->f[j].desc);
          if (hh < best_h[i])
            {
              best_h[i] = hh;
              best_j[i] = j;
            }
        }
    }

  /* translation cluster: count (qx,qy) buckets, pick most common */
  int best_cnt = 0, best_qx = 0, best_qy = 0;
  for (int i = 0; i < n1; i++)
    {
      if (best_j[i] < 0)
        continue;
      int qx = floordiv8 (probe->f[i].x - gallery->f[best_j[i]].x);
      int qy = floordiv8 (probe->f[i].y - gallery->f[best_j[i]].y);
      int cnt = 0;
      for (int k = 0; k < n1; k++)
        {
          if (best_j[k] < 0)
            continue;
          int kx = floordiv8 (probe->f[k].x - gallery->f[best_j[k]].x);
          int ky = floordiv8 (probe->f[k].y - gallery->f[best_j[k]].y);
          if (kx == qx && ky == qy)
            cnt++;
        }
      if (cnt > best_cnt)
        {
          best_cnt = cnt;
          best_qx = qx;
          best_qy = qy;
        }
    }

  double cx = best_qx * 8.0 + 4.0, cy = best_qy * 8.0 + 4.0;
  int inl[EGIS0575_M_MAX_FEATURES], n_inl = 0;

  for (int i = 0; i < n1; i++)
    {
      if (best_j[i] < 0)
        continue;
      double dx = (probe->f[i].x - gallery->f[best_j[i]].x) - cx;
      double dy = (probe->f[i].y - gallery->f[best_j[i]].y) - cy;
      if (best_h[i] <= EGIS0575_M_HAMMING_BUDGET &&
          fabs (dx) <= pos_tol_px && fabs (dy) <= pos_tol_px)
        inl[n_inl++] = i;
    }
  if (n_inl < EGIS0575_M_MIN_MATCHED)
    return 0;

  /* angle mode over SIGNED orientation-index difference k = i - j
   * (angle diff = k * 180/11 degrees, k in [-10, 10]) */
  int ang_cnt[2 * EGIS0575_M_N_ORIENT - 1] = { 0 };
  for (int k = 0; k < n_inl; k++)
    {
      int i = inl[k];
      int d = probe->f[i].orient - gallery->f[best_j[i]].orient;
      ang_cnt[d + EGIS0575_M_N_ORIENT - 1]++;
    }
  int mode_k = 0, mc = 0;
  for (int d = 0; d < 2 * EGIS0575_M_N_ORIENT - 1; d++)
    if (ang_cnt[d] > mc)
      {
        mc = ang_cnt[d];
        mode_k = d - (EGIS0575_M_N_ORIENT - 1);
      }
  double mode_deg = 180.0 * mode_k / EGIS0575_M_N_ORIENT;

  int score = 0, good = 0;
  for (int k = 0; k < n_inl; k++)
    {
      int i = inl[k];
      double a1 = 180.0 * probe->f[i].orient / EGIS0575_M_N_ORIENT;
      double a2 = 180.0 * gallery->f[best_j[i]].orient / EGIS0575_M_N_ORIENT;
      double dm = fmod (a1 - a2 - mode_deg, 360.0);
      if (dm < 0)
        dm += 360.0;
      double dang = dm > 360.0 - dm ? 360.0 - dm : dm;
      if (dang <= angle_tol_deg)
        {
          score += 128 - best_h[i];
          good++;
        }
    }
  if (good < EGIS0575_M_MIN_MATCHED)
    return 0;

  /* unexplained-feature penalty (second-chance NN); features with no
   * plausible NN at all (best_j < 0, every distance exactly 512) are
   * skipped, matching the Python reference's bj<0 drop */
  for (int i = 0; i < n1; i++)
    if (best_j[i] >= 0 && best_h[i] > 256)
      score -= best_h[i] / 2;

  if (votes_out)
    *votes_out = good;
  return score;
}
